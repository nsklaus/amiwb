// File: diskdrives.c
// Drive detection and automatic mounting implementation
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/inotify.h>  // For event-driven mount monitoring
#include <fcntl.h>         // For O_NONBLOCK
#include <errno.h>         // For EAGAIN
#include "diskdrives.h"
#include "workbench/wb_public.h"
#include "workbench/wb_internal.h"
#include "intuition/itn_internal.h"
#include "render/rnd_public.h"
#include "config.h"

static DriveManager drive_manager = {0};

// Event-driven monitoring (inotify) - Module-private state (AWP)
static int inotify_fd = -1;       // Inotify file descriptor (added to select())
static int mountinfo_watch = -1;  // Watch descriptor for /proc/self/mountinfo
static int dev_watch = -1;        // Watch descriptor for /dev (device plug/unplug)

// Track ejected devices to prevent remounting until replug
#define MAX_EJECTED 8
static char ejected_devices[MAX_EJECTED][PATH_SIZE];  // Device paths like /dev/sda1
static int ejected_count = 0;

// Forward declarations for static helpers
static bool is_ejected(const char *device);
static void clean_ejected_list(void);

// Check if filesystem type should be ignored
static bool is_virtual_fs(const char *fs_type) {
    const char *virtual_fs[] = {
        "proc", "sysfs", "devtmpfs", "tmpfs", "devpts",
        "cgroup", "cgroup2", "debugfs", "tracefs", "fusectl",
        "configfs", "securityfs", "pstore", "bpf", "autofs",
        "mqueue", "hugetlbfs", "rpc_pipefs", "overlay", "squashfs",
        "fuse.portal", NULL
    };
    
    for (int i = 0; virtual_fs[i]; i++) {
        if (strcmp(fs_type, virtual_fs[i]) == 0) return true;
    }
    return false;
}

// Check if mount point should be skipped
static bool should_skip_mount(const char *mount_point) {
    // Always show these
    if (strcmp(mount_point, "/") == 0) return false;

    // Show /home mount (will be converted to user's home)
    if (strcmp(mount_point, "/home") == 0) return false;

    const char *home = getenv("HOME");
    if (home && strcmp(mount_point, home) == 0) return false;

    // Show anything in /media, /run/media, or /mnt
    if (strstr(mount_point, "/media/") || strstr(mount_point, "/run/media/") ||
        strstr(mount_point, "/mnt/"))
        return false;
    
    // Skip system directories
    if (strstr(mount_point, "/sys") || strstr(mount_point, "/proc") ||
        strstr(mount_point, "/dev") || strstr(mount_point, "/run") ||
        strstr(mount_point, "/var") || strstr(mount_point, "/tmp") ||
        strstr(mount_point, "/boot") || strstr(mount_point, "/snap"))
        return true;
    
    return true; // Skip unknown paths by default
}

// Find drive by mount point
static int find_drive_by_mount(const char *mount_point) {
    for (int i = 0; i < drive_manager.drive_count; i++) {
        if (strcmp(drive_manager.drives[i].mount_point, mount_point) == 0) {
            return i;
        }
    }
    return -1;
}

// Check if device is removable
static bool check_removable(const char *device) {
    // Extract base device name (e.g., sda from /dev/sda1)
    char base_device[NAME_SIZE];  // Device name only
    snprintf(base_device, sizeof(base_device), "%s", device);
    
    // Remove partition number
    char *p = base_device + strlen(base_device) - 1;
    while (p > base_device && *p >= '0' && *p <= '9') p--;
    if (p > base_device) *(p+1) = '\0';
    
    // Remove /dev/ prefix
    char *dev_name = strrchr(base_device, '/');
    if (!dev_name) dev_name = base_device;
    else dev_name++;
    
    // Check removable flag in sysfs
    char path[PATH_SIZE];  // Filesystem path
    snprintf(path, sizeof(path), "/sys/block/%s/removable", dev_name);
    
    FILE *f = fopen(path, "r");
    if (!f) return false;
    
    char val[2];
    bool removable = false;
    if (fgets(val, sizeof(val), f)) {
        removable = (val[0] == '1');
    }
    fclose(f);
    
    return removable;
}


// Add new drive and create icon
static void add_new_drive(const char *device, const char *mount_point, const char *fs_type) {
    if (drive_manager.drive_count >= MAX_DRIVES) return;

    DiskDrive *drive = &drive_manager.drives[drive_manager.drive_count];
    snprintf(drive->device, sizeof(drive->device), "%s", device);
    snprintf(drive->mount_point, sizeof(drive->mount_point), "%s", mount_point);
    snprintf(drive->fs_type, sizeof(drive->fs_type), "%s", fs_type);
    
    // Determine label
    if (strcmp(mount_point, "/") == 0) {
        snprintf(drive->label, sizeof(drive->label), "System");
    } else if (strcmp(mount_point, "/home") == 0) {
        // For /home mount, we want to show user's home
        snprintf(drive->label, sizeof(drive->label), "Home");
        // Keep mount_point as /home for tracking, but icon will point to user's home
    } else if (strcmp(mount_point, getenv("HOME")) == 0) {
        snprintf(drive->label, sizeof(drive->label), "Home");
    } else if (strstr(mount_point, "/media/") || strstr(mount_point, "/run/media/")) {
        // Extract last component as label
        const char *label = strrchr(mount_point, '/');
        if (label && *(label+1)) {
            snprintf(drive->label, sizeof(drive->label), "%s", label+1);
        } else {
            snprintf(drive->label, sizeof(drive->label), "Drive");
        }
    } else {
        snprintf(drive->label, sizeof(drive->label), "Drive%d", drive_manager.drive_count);
    }
    
    // Check if removable
    drive->is_removable = check_removable(device);
    drive->is_mounted = true;
    
    // Get desktop canvas
    Canvas *desktop = itn_canvas_get_desktop();
    if (!desktop) return;
    
    // Create icon at temporary position - icon_cleanup will arrange it properly
    create_icon("/usr/local/share/amiwb/icons/harddisk.info", desktop, 0, 0);
    
    // Get the icon we just created
    FileIcon **icons = wb_icons_array_get();
    int count = wb_icons_array_count();
    if (count > 0) {
        FileIcon *icon = icons[count - 1];
        
        // For /home mount, icon should point to user's home directory
        const char *icon_path = mount_point;
        if (strcmp(mount_point, "/home") == 0) {
            const char *home = getenv("HOME");
            if (home) icon_path = home;
        }
        
        // Set icon metadata - save old values in case strdup fails
        char *old_path = icon->path;
        char *old_label = icon->label;

        icon->path = strdup(icon_path);
        if (!icon->path) {
            log_error("[ERROR] strdup failed for drive icon path: %s - keeping old path", icon_path);
            icon->path = old_path;  // Restore old path on failure
        } else {
            if (old_path) free(old_path);  // Only free after successful strdup
        }

        icon->label = strdup(drive->label);
        if (!icon->label) {
            log_error("[ERROR] strdup failed for drive label: %s - keeping old label", drive->label);
            icon->label = old_label;  // Restore old label on failure
        } else {
            if (old_label) free(old_label);  // Only free after successful strdup
        }
        icon->type = TYPE_DEVICE;
        drive->icon = icon;
        
        // Drive added successfully - silent per logging rules
    } else {
        log_error("[ERROR] Failed to get icon for drive %s", drive->label);
    }
    
    drive_manager.drive_count++;
    
    // Arrange all icons properly using workbench's column layout
    icon_cleanup(desktop);
    
    // Refresh desktop to show new icon
    if (desktop) {
        redraw_canvas(desktop);
    }
}

// Remove drives that no longer exist
static void remove_missing_drives(bool *found) {
    for (int i = 0; i < drive_manager.drive_count; i++) {
        if (!found[i] && drive_manager.drives[i].is_mounted) {
            DiskDrive *drive = &drive_manager.drives[i];
            
            // Drive removed - silent per logging rules
            
            // Destroy the icon
            if (drive->icon) {
                destroy_icon(drive->icon);
                drive->icon = NULL;
            }
            
            // Mark as unmounted (will be removed on next poll)
            drive->is_mounted = false;
        }
    }
    
    // Compact the array to remove unmounted entries
    int new_count = 0;
    bool any_removed = false;
    for (int i = 0; i < drive_manager.drive_count; i++) {
        if (drive_manager.drives[i].is_mounted) {
            if (new_count != i) {
                drive_manager.drives[new_count] = drive_manager.drives[i];
            }
            new_count++;
        } else {
            any_removed = true;
        }
    }
    drive_manager.drive_count = new_count;
    
    // Refresh desktop if any icons were removed
    if (any_removed) {
        Canvas *desktop = itn_canvas_get_desktop();
        if (desktop) {
            redraw_canvas(desktop);
        }
    }
}

// ============================================================================
// Event-Driven Monitoring (inotify) - Static Helpers (AWP)
// ============================================================================

// Initialize inotify monitoring on /proc/self/mountinfo and /sys/block
// Replaces polling with kernel notifications (true zero-CPU when idle)
static void init_inotify_monitoring(void) {
    // Create inotify instance with non-blocking flag
    inotify_fd = inotify_init1(IN_NONBLOCK);
    if (inotify_fd < 0) {
        log_error("[ERROR] Failed to initialize inotify: %s", strerror(errno));
        log_error("[WARNING] Drive monitoring disabled");
        return;
    }

    // Watch /proc/self/mountinfo for modifications (mount/unmount events)
    mountinfo_watch = inotify_add_watch(inotify_fd, "/proc/self/mountinfo", IN_MODIFY);
    if (mountinfo_watch < 0) {
        log_error("[ERROR] Failed to watch /proc/self/mountinfo: %s", strerror(errno));
    }

    // Watch /dev for device additions/removals (plug/unplug events)
    dev_watch = inotify_add_watch(inotify_fd, "/dev", IN_CREATE | IN_DELETE);
    if (dev_watch < 0) {
        log_error("[ERROR] Failed to watch /dev: %s", strerror(errno));
    }

    // Verify at least one watch succeeded
    if (mountinfo_watch < 0 && dev_watch < 0) {
        log_error("[WARNING] Both inotify watches failed - drive monitoring disabled");
        close(inotify_fd);
        inotify_fd = -1;
    }
}

// Scan /proc/mounts and update drive list
// Called when /proc/self/mountinfo changes (mount/unmount event)
static void scan_mounted_drives(void) {
    FILE *mounts = fopen("/proc/mounts", "r");
    if (!mounts) {
        log_error("[WARNING] Cannot open /proc/mounts");
        return;
    }

    char line[1024];
    bool found[MAX_DRIVES] = {0};

    while (fgets(line, sizeof(line), mounts)) {
        char device[PATH_SIZE], mount_point[PATH_SIZE], fs_type[NAME_SIZE];
        if (sscanf(line, "%511s %511s %127s", device, mount_point, fs_type) < 3)
            continue;

        // Skip virtual filesystems
        if (is_virtual_fs(fs_type)) continue;

        // Skip system paths except / and /home/$USER
        if (should_skip_mount(mount_point)) continue;

        // Check if we already have this mount
        int idx = find_drive_by_mount(mount_point);
        if (idx >= 0) {
            found[idx] = true;
            continue;
        }

        // New mount detected - create drive entry
        add_new_drive(device, mount_point, fs_type);
        // Mark the newly added drive as found too!
        if (drive_manager.drive_count > 0) {
            found[drive_manager.drive_count - 1] = true;
        }
    }

    fclose(mounts);

    // Remove drives that disappeared
    remove_missing_drives(found);

    drive_manager.last_poll = time(NULL);
}

// Detect and mount newly plugged devices
// Called when /dev changes (device plug event)
// This is the ONLY place that runs lsblk (and only when device actually plugged)
static void detect_and_mount_new_devices(void) {
    // Clean up ejected list (remove devices that were unplugged)
    clean_ejected_list();

    // Retry loop: kernel needs time to scan filesystem after device appears
    // Exponential backoff: 0, 50, 100, 200, 400, 800, 1600ms (~3.1s max)
    const int max_retries = 7;  // Fewer retries needed with exponential backoff
    bool found_unscanned_device = false;

    for (int retry = 0; retry < max_retries; retry++) {
        if (retry > 0) {
            int retry_delay_ms = 50 << (retry - 1);  // 50*2^(retry-1): 50, 100, 200, 400, 800, 1600
            usleep(retry_delay_ms * 1000);
        }

        found_unscanned_device = false;

        // Run lsblk to find unmounted devices with filesystems
        const char *cmd = "lsblk -rno NAME,MOUNTPOINT,FSTYPE 2>&1";
        FILE *fp = popen(cmd, "r");
        if (!fp) {
            log_error("[WARNING] Failed to run lsblk for device detection");
            return;
        }

        // Read all lsblk output into memory (can't rewind pipes!)
        char lsblk_output[8192];  // Buffer for entire lsblk output (uninitialized)
        size_t total_read = 0;
        char line[PATH_SIZE];
        while (fgets(line, sizeof(line), fp) && total_read < sizeof(lsblk_output) - 1) {
            size_t len = strlen(line);
            if (total_read + len < sizeof(lsblk_output)) {
                memcpy(lsblk_output + total_read, line, len);
                total_read += len;
            }
        }
        pclose(fp);
        lsblk_output[total_read] = '\0';

        // First pass: identify parent disks AND partitions
        char parent_disks[32][NAME_SIZE];  // Track parent disk names
        int parent_count = 0;
        char partition_names[64][NAME_SIZE];  // Track partition names (sda1, nvme0n1p1, etc)
        int partition_count = 0;
        bool saw_any_partitions = false;   // Track if we saw ANY partitions

        char *line_ptr = lsblk_output;
        while (*line_ptr) {
            // Extract one line
            char *line_end = strchr(line_ptr, '\n');
            size_t line_len = line_end ? (size_t)(line_end - line_ptr) : strlen(line_ptr);
            if (line_len >= sizeof(line)) line_len = sizeof(line) - 1;
            memcpy(line, line_ptr, line_len);
            line[line_len] = '\0';

            char name[NAME_SIZE], mountpoint[NAME_SIZE], fstype[NAME_SIZE];
            mountpoint[0] = '\0';
            fstype[0] = '\0';

            int fields = sscanf(line, "%127s %127s %127s", name, mountpoint, fstype);
            if (fields >= 1) {
                size_t len = strlen(name);
                if (len > 0) {
                    // Extract parent disk name from partition names
                    char parent[NAME_SIZE] = {0};
                    bool is_partition = false;

                    // Check if name ends with digit (potential partition)
                    if (name[len-1] >= '0' && name[len-1] <= '9') {
                        // Look for 'p' separator (nvme0n1p1, mmcblk0p2)
                        char *p_pos = strrchr(name, 'p');
                        if (p_pos && p_pos > name && *(p_pos+1) >= '0' && *(p_pos+1) <= '9') {
                            // Has 'pN' pattern - parent is everything before 'p'
                            size_t parent_len = p_pos - name;
                            snprintf(parent, sizeof(parent), "%.*s", (int)parent_len, name);
                            is_partition = true;
                        } else {
                            // No 'p' separator - strip trailing digits (sda1, hda2)
                            snprintf(parent, sizeof(parent), "%s", name);
                            char *p = parent + strlen(parent) - 1;
                            while (p >= parent && *p >= '0' && *p <= '9') {
                                *p = '\0';
                                p--;
                            }
                            if (strlen(parent) > 0 && strcmp(parent, name) != 0) {
                                is_partition = true;
                            }
                        }
                    }

                    // Add partition to list and track parent
                    if (is_partition && parent[0] != '\0') {
                        saw_any_partitions = true;  // We found at least one partition

                        // Track partition name
                        if (partition_count < 64) {
                            strcpy(partition_names[partition_count], name);
                            partition_count++;
                        }

                        // Track parent disk
                        bool already_tracked = false;
                        for (int i = 0; i < parent_count; i++) {
                            if (strcmp(parent_disks[i], parent) == 0) {
                                already_tracked = true;
                                break;
                            }
                        }
                        if (!already_tracked && parent_count < 32) {
                            strcpy(parent_disks[parent_count], parent);
                            parent_count++;
                        }
                    }
                }
            }

            // Move to next line
            if (line_end) line_ptr = line_end + 1;
            else break;
        }

        // If we saw no partitions at all, only bare parent disks → nothing to mount yet
        // Wait for partition inotify event instead of wasting retry cycles
        if (!saw_any_partitions) {
            break;  // Exit retry loop - nothing to do
        }

        // Second pass: process devices, ignoring parent disks
        line_ptr = lsblk_output;
        while (*line_ptr) {
            // Extract one line
            char *line_end = strchr(line_ptr, '\n');
            size_t line_len = line_end ? (size_t)(line_end - line_ptr) : strlen(line_ptr);
            if (line_len >= sizeof(line)) line_len = sizeof(line) - 1;
            memcpy(line, line_ptr, line_len);
            line[line_len] = '\0';

            // Parse the line
            char name[NAME_SIZE], mountpoint[NAME_SIZE], fstype[NAME_SIZE];
            mountpoint[0] = '\0';
            fstype[0] = '\0';

            // Simple parsing - name is first field, second could be mountpoint or fstype
            int fields = sscanf(line, "%127s %127s %127s", name, mountpoint, fstype);

            // Check if this is an unscanned partition (no filesystem detected yet)
            // Ignore parent disks - only retry for actual partitions
            if (fields >= 1 && mountpoint[0] == '\0' && fstype[0] == '\0') {
                // Check if this device is a known parent disk (has partitions we've seen)
                bool is_parent_disk = false;
                for (int i = 0; i < parent_count; i++) {
                    if (strcmp(name, parent_disks[i]) == 0) {
                        is_parent_disk = true;
                        break;
                    }
                }

                if (!is_parent_disk) {
                    // Not in parent_disks list → check if it's a known partition
                    bool is_known_partition = false;
                    for (int i = 0; i < partition_count; i++) {
                        if (strcmp(name, partition_names[i]) == 0) {
                            is_known_partition = true;
                            break;
                        }
                    }

                    if (is_known_partition) {
                        // This device was seen as a partition in first pass → unscanned partition
                        found_unscanned_device = true;
                    }
                    // Otherwise: bare parent disk from different family - ignore
                }
            }

            if (fields >= 2) {
                // If only 2 fields, second is fstype not mountpoint
                if (fstype[0] == '\0' && mountpoint[0] != '\0' && mountpoint[0] != '/') {
                    snprintf(fstype, sizeof(fstype), "%s", mountpoint);
                    mountpoint[0] = '\0';
                }

                // Check if unmounted with filesystem
                if (mountpoint[0] == '\0' && fstype[0] != '\0') {
                    char device[PATH_SIZE];
                    snprintf(device, sizeof(device), "/dev/%s", name);

                    // Skip if device was manually ejected
                    if (is_ejected(device)) {
                        // Move to next line before continue
                        if (line_end) line_ptr = line_end + 1;
                        else break;
                        continue;
                    }

                    // Attempt to mount the device
                    if (mount_device(device)) {
                        // Device mounted successfully - create icon immediately
                        // Can't rely on /proc inotify (pseudo-fs doesn't support it reliably)
                        scan_mounted_drives();
                    } else {
                        log_error("[WARNING] Failed to mount %s", device);
                    }
                }
            }

            // Move to next line
            if (line_end) line_ptr = line_end + 1;
            else break;
        }

        // Check if we should retry or break
        if (!found_unscanned_device) {
            break;  // Success - all devices have filesystem info or are mounted
        }
    }  // End of retry loop
}

// Process inotify events and dispatch to appropriate handlers
// Called from event loop when inotify_fd has data ready
static void process_inotify_events(void) {
    // Buffer for inotify events
    char buffer[4096] __attribute__ ((aligned(__alignof__(struct inotify_event))));
    const struct inotify_event *event;

    bool mountinfo_changed = false;
    bool dev_created = false;  // Only mount on CREATE, not DELETE

    while (true) {
        ssize_t len = read(inotify_fd, buffer, sizeof(buffer));

        if (len == -1) {
            if (errno == EAGAIN) {
                // No more events - normal with non-blocking read
                break;
            }
            log_error("[ERROR] Error reading inotify events: %s", strerror(errno));
            break;
        }

        if (len == 0) {
            break;
        }

        // Parse events to determine what changed
        for (char *ptr = buffer; ptr < buffer + len; ) {
            event = (const struct inotify_event *)ptr;

            // Check which watch triggered
            if (event->wd == mountinfo_watch) {
                mountinfo_changed = true;
            } else if (event->wd == dev_watch) {
                // Only trigger mount on CREATE (0x100), not DELETE (0x200)
                if (event->mask & IN_CREATE) {
                    dev_created = true;
                }
            }

            ptr += sizeof(struct inotify_event) + event->len;
        }
    }

    // Dispatch to appropriate handlers (avoid redundant work)
    if (dev_created) {
        // Device plugged - check for new unmounted devices
        detect_and_mount_new_devices();
    }

    if (mountinfo_changed) {
        // Mount/unmount happened - rescan mount list
        scan_mounted_drives();
    }
}

// ============================================================================
// Public Functions
// ============================================================================

void diskdrives_init(void) {
    // Initializing disk drives system
    drive_manager.drive_count = 0;
    drive_manager.last_poll = 0;

    // Initialize event-driven monitoring (zero-CPU when idle)
    init_inotify_monitoring();

    // Do initial scan (before events start arriving)
    scan_mounted_drives();
}

// Check if device was ejected and shouldn't be remounted
static bool is_ejected(const char *device) {
    for (int i = 0; i < ejected_count; i++) {
        if (strcmp(ejected_devices[i], device) == 0) {
            return true;
        }
    }
    return false;
}

// Clear ejected devices that no longer exist (unplugged)
static void clean_ejected_list(void) {
    int new_count = 0;
    for (int i = 0; i < ejected_count; i++) {
        // Check if device still exists in /dev/
        if (access(ejected_devices[i], F_OK) == 0) {
            // Device still exists, keep it in list
            if (new_count != i) {
                memmove(ejected_devices[new_count], ejected_devices[i], PATH_SIZE);
            }
            new_count++;
        }
        // Device unplugged - removed from ejected list
    }
    ejected_count = new_count;
}

void diskdrives_cleanup(void) {
    // Close inotify file descriptor
    if (inotify_fd >= 0) {
        close(inotify_fd);
        inotify_fd = -1;
    }

    // Don't destroy icons here - they're workbench icons that will be cleaned up
    // by cleanup_workbench(). Just clear our references to prevent dangling pointers.
    for (int i = 0; i < drive_manager.drive_count; i++) {
        drive_manager.drives[i].icon = NULL;
    }
    drive_manager.drive_count = 0;
}

bool mount_device(const char *device) {
    char cmd[FULL_SIZE];  // Command with device path
    // udisksctl automatically mounts with user permissions when run by user
    snprintf(cmd, sizeof(cmd), "udisksctl mount -b %s 2>&1", device);

    FILE *fp = popen(cmd, "r");
    if (!fp) return false;

    char result[PATH_SIZE];  // Command output line buffer
    bool success = false;
    while (fgets(result, sizeof(result), fp)) {
        // udisksctl outputs: "Mounted /dev/sda1 at /media/user/LABEL"
        if (strstr(result, "Mounted")) {
            success = true;
            // Device mounted successfully
        }
    }
    pclose(fp);
    return success;
}

bool unmount_device(const char *device) {
    char cmd[FULL_SIZE];  // Command with device path
    snprintf(cmd, sizeof(cmd), "udisksctl unmount -b %s 2>&1", device);

    FILE *fp = popen(cmd, "r");
    if (!fp) return false;

    char result[PATH_SIZE];  // Command output line buffer
    bool success = false;
    while (fgets(result, sizeof(result), fp)) {
        if (strstr(result, "Unmounted")) {
            success = true;
            // Device unmounted successfully
        }
    }
    pclose(fp);
    return success;
}

void eject_drive(FileIcon *icon) {
    if (!icon || icon->type != TYPE_DEVICE) return;
    
    // Find drive by mount point
    for (int i = 0; i < drive_manager.drive_count; i++) {
        if (strcmp(drive_manager.drives[i].mount_point, icon->path) == 0) {
            DiskDrive *drive = &drive_manager.drives[i];
            
            // Don't allow ejecting System or Home
            if (strstr(drive->label, "System") || strstr(drive->label, "Home")) {
                // Cannot eject system drive
                return;
            }
            
            // TODO: Close any windows browsing this mount
            // close_windows_for_path(drive->mount_point);
            
            // Unmount the device
            if (unmount_device(drive->device)) {
                // Drive ejected successfully

                // Add to ejected list to prevent auto-remount
                if (ejected_count < MAX_EJECTED) {
                    snprintf(ejected_devices[ejected_count], sizeof(ejected_devices[0]), "%s", drive->device);
                    ejected_count++;
                }
                
                // Destroy the icon immediately
                if (drive->icon) {
                    destroy_icon(drive->icon);
                    drive->icon = NULL;
                }
                
                // Remove this drive from the array by shifting others down
                for (int j = i; j < drive_manager.drive_count - 1; j++) {
                    drive_manager.drives[j] = drive_manager.drives[j + 1];
                }
                drive_manager.drive_count--;
                
                // Refresh desktop to show the icon is gone
                Canvas *desktop = itn_canvas_get_desktop();
                if (desktop) {
                    redraw_canvas(desktop);
                }
            }
            break;
        }
    }
}

bool is_drive_removable(const char *mount_point) {
    for (int i = 0; i < drive_manager.drive_count; i++) {
        if (strcmp(drive_manager.drives[i].mount_point, mount_point) == 0) {
            return drive_manager.drives[i].is_removable;
        }
    }
    return false;
}

DriveManager* get_drive_manager(void) {
    return &drive_manager;
}

// Find DiskDrive by FileIcon pointer
DiskDrive* diskdrives_find_by_icon(FileIcon *icon) {
    if (!icon) return NULL;

    for (int i = 0; i < drive_manager.drive_count; i++) {
        if (drive_manager.drives[i].icon == icon) {
            return &drive_manager.drives[i];
        }
    }
    return NULL;
}

// Get inotify file descriptor for event loop integration
// Returns -1 if inotify initialization failed
// Following same pattern as itn_render_get_timer_fd() (AWP - standardization)
int diskdrives_get_inotify_fd(void) {
    return inotify_fd;
}

// Process pending inotify events
// Called from event loop when inotify_fd has data ready
// Following same pattern as itn_render_process_frame() (AWP - standardization)
void diskdrives_process_events(void) {
    if (inotify_fd < 0) return;  // Inotify not initialized
    process_inotify_events();
}