// File: about_sysinfo.h
// System information gathering for About dialog
// Encapsulates hardware detection logic following The AmiWB Way

#ifndef ABOUT_SYSINFO_H
#define ABOUT_SYSINFO_H

#include <stdbool.h>

// ============================================================================
// System Information Structure
// ============================================================================

typedef struct {
    char amiwb_version[32];
    char toolkit_version[32];
    char os_name[64];
    char os_version[32];
    char kernel_name[65];      // utsname.sysname is char[65]
    char kernel_version[65];   // utsname.release is char[65]
    char total_ram[32];
    char cpu_name[128];
    char cpu_arch[65];         // utsname.machine is char[65]
    char igpu_name[128];
    char igpu_ram[32];
    char dgpu_name[128];
    char dgpu_ram[32];
    char xorg_version[64];
    char input_backend[64];
    int optimal_width;         // Calculated minimum width based on longest string
} SystemInfo;

// ============================================================================
// Public API
// ============================================================================

// Gather all system information (caches result for subsequent calls)
// Should be called once before first About dialog open
// Returns: pointer to cached SystemInfo, or NULL on failure
SystemInfo *about_sysinfo_gather(void);

// Get cached system information (fast, no re-detection)
// Returns: pointer to cached SystemInfo, or NULL if not yet gathered
SystemInfo *about_sysinfo_get_cached(void);

#endif // ABOUT_SYSINFO_H
