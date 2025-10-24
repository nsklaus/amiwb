// File: wb_ramdisk.h
// RAM Disk Management - AmigaOS-style RAM: disk using tmpfs
// Provides temporary fast storage in RAM, cleared on unmount/reboot

#ifndef WB_RAMDISK_H
#define WB_RAMDISK_H

#include <stdbool.h>

// Initialize RAM disk system (mount tmpfs if not already mounted, create icon)
// Called during AmiWB startup after diskdrives_init()
void ramdisk_init(void);

// Cleanup RAM disk system (unmount tmpfs, free all RAM)
// Called during AmiWB shutdown - destroys all files in ramdisk
void ramdisk_cleanup(void);

#endif // WB_RAMDISK_H
