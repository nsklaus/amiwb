// File: diskdrives.h
// Drive detection and management for automatic mounting
#ifndef DISKDRIVES_H
#define DISKDRIVES_H

#include <stdbool.h>
#include "icons/icon_public.h"

#define MAX_DRIVES 32

// Drive information structure
typedef struct {
    char device[64];              // /dev/nvme0n1p6, /dev/sda1
    char mount_point[512];        // /, /home/klaus, /media/Backup
    char label[128];              // "System", "Home", "Backup"
    char fs_type[32];             // btrfs, ext4, vfat, ntfs
    bool is_removable;            // From /sys/block/*/removable
    bool is_mounted;              // Currently mounted?
    FileIcon *icon;               // Desktop icon for this mount
} DiskDrive;

// Drive manager
typedef struct {
    DiskDrive drives[MAX_DRIVES];
    int drive_count;
    time_t last_poll;             // Last poll timestamp
} DriveManager;

// Public functions
void diskdrives_init(void);         // Initialize drive system on startup
void diskdrives_cleanup(void);      // Cleanup on exit

// Event-driven monitoring (replaces polling)
int diskdrives_get_inotify_fd(void);     // Get inotify FD for select() (returns -1 if disabled)
void diskdrives_process_events(void);    // Process inotify events (call when FD ready)

// Mount/unmount operations
bool mount_device(const char *device);      // Mount device using udisksctl
bool unmount_device(const char *device);    // Unmount device safely
void eject_drive(FileIcon *icon);           // Handle eject request for drive icon

// Query functions
bool is_drive_removable(const char *mount_point);
DriveManager* get_drive_manager(void);
DiskDrive* diskdrives_find_by_icon(FileIcon *icon);    // Find drive by icon pointer
DiskDrive* diskdrives_find_by_label(const char *label); // Find drive by label (e.g., "Ram Disk")
DiskDrive* diskdrives_find_by_path(const char *path);   // Find drive by mount point or path within

#endif