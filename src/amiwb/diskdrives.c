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
#include "diskdrives.h"
#include "workbench.h"
#include "intuition.h"
#include "render.h"
#include "config.h"

static DriveManager drive_manager = {0};

// Track ejected devices to prevent remounting until replug
#define MAX_EJECTED 8
static char ejected_devices[MAX_EJECTED][64];
static int ejected_count = 0;

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
    char base_device[64];
    strncpy(base_device, device, sizeof(base_device)-1);
    
    // Remove partition number
    char *p = base_device + strlen(base_device) - 1;
    while (p > base_device && *p >= '0' && *p <= '9') p--;
    if (p > base_device) *(p+1) = '\0';
    
    // Remove /dev/ prefix
    char *dev_name = strrchr(base_device, '/');
    if (!dev_name) dev_name = base_device;
    else dev_name++;
    
    // Check removable flag in sysfs
    char path[256];
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
    strncpy(drive->device, device, sizeof(drive->device)-1);
    strncpy(drive->mount_point, mount_point, sizeof(drive->mount_point)-1);
    strncpy(drive->fs_type, fs_type, sizeof(drive->fs_type)-1);
    
    // Determine label
    if (strcmp(mount_point, "/") == 0) {
        strcpy(drive->label, "System");
    } else if (strcmp(mount_point, "/home") == 0) {
        // For /home mount, we want to show user's home
        strcpy(drive->label, "Home");
        // Keep mount_point as /home for tracking, but icon will point to user's home
    } else if (strcmp(mount_point, getenv("HOME")) == 0) {
        strcpy(drive->label, "Home");
    } else if (strstr(mount_point, "/media/") || strstr(mount_point, "/run/media/")) {
        // Extract last component as label
        const char *label = strrchr(mount_point, '/');
        if (label && *(label+1)) {
            strncpy(drive->label, label+1, sizeof(drive->label)-1);
        } else {
            strcpy(drive->label, "Drive");
        }
    } else {
        snprintf(drive->label, sizeof(drive->label), "Drive%d", drive_manager.drive_count);
    }
    
    // Check if removable
    drive->is_removable = check_removable(device);
    drive->is_mounted = true;
    
    // Get desktop canvas
    Canvas *desktop = get_desktop_canvas();
    if (!desktop) return;
    
    // Create icon at temporary position - icon_cleanup will arrange it properly
    create_icon("/usr/local/share/amiwb/icons/harddisk.info", desktop, 0, 0);
    
    // Get the icon we just created
    FileIcon **icons = get_icon_array();
    int count = get_icon_count();
    if (count > 0) {
        FileIcon *icon = icons[count - 1];
        
        // For /home mount, icon should point to user's home directory
        const char *icon_path = mount_point;
        if (strcmp(mount_point, "/home") == 0) {
            const char *home = getenv("HOME");
            if (home) icon_path = home;
        }
        
        // Set icon metadata
        if (icon->path) free(icon->path);
        icon->path = strdup(icon_path);
        if (icon->label) free(icon->label);
        icon->label = strdup(drive->label);
        icon->type = TYPE_DEVICE;
        drive->icon = icon;
        
        log_error("[INFO] Added drive: %s at %s (device: %s, removable: %s, icon ptr: %p)",
                  drive->label, mount_point, device, 
                  drive->is_removable ? "yes" : "no", (void*)icon);
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
            
            log_error("[INFO] Drive removed: %s at %s", 
                      drive->label, drive->mount_point);
            
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
        Canvas *desktop = get_desktop_canvas();
        if (desktop) {
            redraw_canvas(desktop);
        }
    }
}

// Public functions

void diskdrives_init(void) {
    log_error("[INFO] Initializing disk drives system");
    drive_manager.drive_count = 0;
    drive_manager.last_poll = 0;
    
    // Do initial scan
    diskdrives_poll();
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

// Track devices we've seen to avoid spam
static char seen_devices[32][64];
static int seen_count = 0;

// Clear ejected devices that no longer exist (unplugged)
static void clean_ejected_list(void) {
    int new_count = 0;
    for (int i = 0; i < ejected_count; i++) {
        // Check if device still exists in /dev/
        if (access(ejected_devices[i], F_OK) == 0) {
            // Device still exists, keep it in list
            if (new_count != i) {
                strcpy(ejected_devices[new_count], ejected_devices[i]);
            }
            new_count++;
        } else {
            log_error("[INFO] Device %s unplugged, removing from ejected list", ejected_devices[i]);
            // Also remove from seen devices when unplugged
            for (int j = 0; j < seen_count; j++) {
                if (strcmp(seen_devices[j], ejected_devices[i]) == 0) {
                    // Remove by shifting remaining devices
                    for (int k = j; k < seen_count - 1; k++) {
                        strcpy(seen_devices[k], seen_devices[k + 1]);
                    }
                    seen_count--;
                    break;
                }
            }
        }
    }
    ejected_count = new_count;
}

static bool have_seen_device(const char *device) {
    for (int i = 0; i < seen_count; i++) {
        if (strcmp(seen_devices[i], device) == 0) return true;
    }
    return false;
}

static void mark_device_seen(const char *device) {
    if (seen_count < 32 && !have_seen_device(device)) {
        strcpy(seen_devices[seen_count++], device);
    }
}

// Track /sys/block devices for immediate detection  
// Device names in /dev/ are limited by system, but we'll use safe size
#define DEVICE_PATH_SIZE 64  // More than enough for /dev/xxx device paths
static char sys_block_devices[32][DEVICE_PATH_SIZE];
static int sys_block_count = 0;

static void check_sys_block_devices(void) {
    // Check /sys/block for ANY new devices immediately
    DIR *dir = opendir("/sys/block");
    if (!dir) return;
    
    char current_sys_devices[32][DEVICE_PATH_SIZE];
    int current_count = 0;
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        
        // Track current devices
        if (current_count < 32) {
            char temp[DEVICE_PATH_SIZE];
            snprintf(temp, DEVICE_PATH_SIZE, "/dev/%.58s", entry->d_name);  // Max 58 chars + /dev/ = 63
            strcpy(current_sys_devices[current_count++], temp);
        }
        
        // Check if this is new
        bool is_new = true;
        char full_path[DEVICE_PATH_SIZE];
        snprintf(full_path, DEVICE_PATH_SIZE, "/dev/%.58s", entry->d_name);
        for (int i = 0; i < sys_block_count; i++) {
            if (strcmp(sys_block_devices[i], full_path) == 0) {
                is_new = false;
                break;
            }
        }
        
        if (is_new) {
            log_error("[INFO] DEVICE PLUGGED IN: /dev/%s (detected in /sys/block)", entry->d_name);
            // Don't add it here - we'll update our list at the end
        }
    }
    closedir(dir);
    
    // Check for removed devices
    for (int i = 0; i < sys_block_count; i++) {
        bool still_exists = false;
        for (int j = 0; j < current_count; j++) {
            if (strcmp(sys_block_devices[i], current_sys_devices[j]) == 0) {
                still_exists = true;
                break;
            }
        }
        if (!still_exists) {
            log_error("[INFO] DEVICE UNPLUGGED: %s (removed from /sys/block)", sys_block_devices[i]);
        }
    }
    
    // Update our tracking list to current state
    sys_block_count = current_count;
    for (int i = 0; i < current_count; i++) {
        strcpy(sys_block_devices[i], current_sys_devices[i]);
    }
}

// Try to auto-mount unmounted removable devices
static void try_automount_removable(void) {
    // First check /sys/block for immediate device detection
    check_sys_block_devices();
    
    // Clean up ejected list
    clean_ejected_list();
    
    // Test the command directly first
    const char *cmd = "lsblk -rno NAME,MOUNTPOINT,FSTYPE 2>&1";
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        log_error("[WARNING] Failed to run lsblk for automount check");
        return;
    }
    
    char line[256];
    // Track what devices exist THIS poll
    char current_devices[32][64];
    int current_count = 0;
    
    while (fgets(line, sizeof(line), fp)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        
        // Track ALL block devices we see
        char device_name[64];
        if (sscanf(line, "%63s", device_name) >= 1) {
            // Skip partitions of devices we already know
            if (strchr(device_name, 'p') || isdigit(device_name[strlen(device_name)-1])) {
                // This is likely a partition, track it anyway
            }
            
            char full_device[256];
            snprintf(full_device, sizeof(full_device), "/dev/%s", device_name);
            
            // Track current devices
            if (current_count < 32) {
                strcpy(current_devices[current_count++], full_device);
            }
            
            // Log ANY new device immediately
            if (!have_seen_device(full_device)) {
                log_error("[INFO] NEW DEVICE APPEARED: %s", full_device);
                mark_device_seen(full_device);
            }
        }
        
        // Only process sd devices for mounting
        if (strncmp(line, "sd", 2) == 0) {
            
            // Parse the line
            char name[64], mountpoint[256], fstype[64];
            mountpoint[0] = '\0';
            fstype[0] = '\0';
            
            // Simple parsing - name is first field, mountpoint could be empty
            if (sscanf(line, "%63s %255s %63s", name, mountpoint, fstype) >= 2) {
                // If only 2 fields, second is fstype not mountpoint
                if (fstype[0] == '\0' && mountpoint[0] != '\0' && mountpoint[0] != '/') {
                    strcpy(fstype, mountpoint);
                    mountpoint[0] = '\0';
                }
                
                // Check if unmounted with filesystem
                if (mountpoint[0] == '\0' && fstype[0] != '\0') {
                    char device[256];
                    snprintf(device, sizeof(device), "/dev/%s", name);
                    
                    // Skip if device was manually ejected
                    if (is_ejected(device)) {
                        continue;
                    }
                    
                    // Log detection and attempt mount
                    log_error("[INFO] Detected unmounted device: %s (fs: %s)", device, fstype);
                    log_error("[INFO] Attempting to mount %s", device);
                    if (mount_device(device)) {
                        log_error("[INFO] Successfully mounted %s", device);
                    } else {
                        log_error("[WARNING] Failed to mount %s", device);
                    }
                }
            }
        }
    }
    pclose(fp);
    
    // Check if any previously seen devices have disappeared
    for (int i = 0; i < seen_count; i++) {
        bool still_exists = false;
        for (int j = 0; j < current_count; j++) {
            if (strcmp(seen_devices[i], current_devices[j]) == 0) {
                still_exists = true;
                break;
            }
        }
        if (!still_exists) {
            log_error("[INFO] DEVICE DISAPPEARED: %s", seen_devices[i]);
            // Remove from seen list
            for (int j = i; j < seen_count - 1; j++) {
                strcpy(seen_devices[j], seen_devices[j + 1]);
            }
            seen_count--;
            i--; // Recheck this index since we shifted
        }
    }
}

void diskdrives_poll(void) {
    static int poll_count = 0;
    poll_count++;
    
    // Try to auto-mount removable devices every poll (2 seconds)
    try_automount_removable();
    
    FILE *mounts = fopen("/proc/mounts", "r");
    if (!mounts) {
        log_error("[WARNING] Cannot open /proc/mounts");
        return;
    }
    
    
    char line[1024];
    bool found[MAX_DRIVES] = {0};
    
    while (fgets(line, sizeof(line), mounts)) {
        char device[256], mount_point[256], fs_type[64];
        if (sscanf(line, "%255s %255s %63s", device, mount_point, fs_type) < 3)
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

void diskdrives_cleanup(void) {
    // Clean up all drive icons
    for (int i = 0; i < drive_manager.drive_count; i++) {
        if (drive_manager.drives[i].icon) {
            destroy_icon(drive_manager.drives[i].icon);
        }
    }
    drive_manager.drive_count = 0;
}

bool mount_device(const char *device) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "udisksctl mount -b %s 2>&1", device);
    
    FILE *fp = popen(cmd, "r");
    if (!fp) return false;
    
    char result[256];
    bool success = false;
    while (fgets(result, sizeof(result), fp)) {
        // udisksctl outputs: "Mounted /dev/sda1 at /media/user/LABEL"
        if (strstr(result, "Mounted")) {
            success = true;
            log_error("[INFO] Mounted device: %s", device);
        }
    }
    pclose(fp);
    return success;
}

bool unmount_device(const char *device) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "udisksctl unmount -b %s 2>&1", device);
    
    FILE *fp = popen(cmd, "r");
    if (!fp) return false;
    
    char result[256];
    bool success = false;
    while (fgets(result, sizeof(result), fp)) {
        if (strstr(result, "Unmounted")) {
            success = true;
            log_error("[INFO] Unmounted device: %s", device);
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
                log_error("[INFO] Cannot eject %s - system drive", drive->label);
                return;
            }
            
            // TODO: Close any windows browsing this mount
            // close_windows_for_path(drive->mount_point);
            
            // Unmount the device
            if (unmount_device(drive->device)) {
                log_error("[INFO] Drive ejected: %s", drive->label);
                
                // Add to ejected list to prevent auto-remount
                if (ejected_count < MAX_EJECTED) {
                    strncpy(ejected_devices[ejected_count], drive->device, 63);
                    ejected_devices[ejected_count][63] = '\0';
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
                Canvas *desktop = get_desktop_canvas();
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