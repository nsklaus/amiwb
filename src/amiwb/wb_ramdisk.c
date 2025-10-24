// File: wb_ramdisk.c
// RAM Disk Management - AmigaOS-style RAM: disk using tmpfs
// Uses /dev/shm (pre-mounted tmpfs) for user-accessible RAM storage

#include "wb_ramdisk.h"
#include "workbench/wb_public.h"
#include "workbench/wb_internal.h"
#include "intuition/itn_internal.h"
#include "render/rnd_public.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

// RAM disk directory (inside /dev/shm, already tmpfs)
// Note: /dev/shm is pre-mounted tmpfs, writable by all users
#define RAMDISK_PATH "/dev/shm/amiwb-ramdisk"

// ============================================================================
// Static Helper Functions
// ============================================================================

// Check if ramdisk directory exists
static bool ramdisk_exists(void) {
    struct stat st;
    return (stat(RAMDISK_PATH, &st) == 0 && S_ISDIR(st.st_mode));
}

// Create ramdisk directory
// Returns true on success
static bool create_ramdisk_directory(void) {
    if (mkdir(RAMDISK_PATH, 0700) != 0) {
        log_error("[ERROR] Failed to create ramdisk directory: %s - %s",
                 RAMDISK_PATH, strerror(errno));
        return false;
    }
    return true;
}

// Remove ramdisk directory and all contents (fast cleanup on quit)
static void remove_ramdisk_directory(void) {
    // Use rm -rf for fastest deletion (kernel optimized, frees RAM immediately)
    char cmd[PATH_SIZE + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", RAMDISK_PATH);
    system(cmd);
}

// Create device icon for ramdisk on desktop
static void create_ramdisk_icon(void) {
    Canvas *desktop = itn_canvas_get_desktop();
    if (!desktop) {
        log_error("[ERROR] Cannot get desktop canvas for ramdisk icon");
        return;
    }

    // Create icon using ramdisk.info (falls back to harddisk.info if missing)
    const char *icon_file = "/usr/local/share/amiwb/icons/ramdisk.info";
    struct stat st;
    if (stat(icon_file, &st) != 0) {
        // Fallback to harddisk.info if ramdisk.info doesn't exist
        icon_file = "/usr/local/share/amiwb/icons/harddisk.info";
    }

    // Create icon at temporary position (icon_cleanup will arrange it)
    create_icon(icon_file, desktop, 0, 0);

    // Get the icon we just created
    FileIcon **icons = wb_icons_array_get();
    int count = wb_icons_array_count();
    if (count > 0) {
        FileIcon *icon = icons[count - 1];

        // Set icon metadata (same pattern as diskdrives.c)
        char *old_path = icon->path;
        char *old_label = icon->label;

        icon->path = strdup(RAMDISK_PATH);
        if (!icon->path) {
            log_error("[ERROR] strdup failed for ramdisk path - keeping old path");
            icon->path = old_path;
        } else {
            if (old_path) free(old_path);
        }

        icon->label = strdup("Ram Disk");
        if (!icon->label) {
            log_error("[ERROR] strdup failed for ramdisk label - keeping old label");
            icon->label = old_label;
        } else {
            if (old_label) free(old_label);
        }

        icon->type = TYPE_DEVICE;

        // Arrange all icons properly using workbench's column layout
        icon_cleanup(desktop);

        // Refresh desktop to show new icon
        redraw_canvas(desktop);
    } else {
        log_error("[ERROR] Failed to get ramdisk icon from array");
    }
}

// ============================================================================
// Public API Implementation
// ============================================================================

void ramdisk_init(void) {
    // Check if ramdisk already exists (hot-restart case)
    if (ramdisk_exists()) {
        // Already exists - just create icon (preserve existing data)
        create_ramdisk_icon();
        return;
    }

    // Create ramdisk directory in /dev/shm
    if (!create_ramdisk_directory()) {
        log_error("[WARNING] RAM disk disabled (directory creation failed)");
        return;
    }

    // Create device icon on desktop
    create_ramdisk_icon();
}

void ramdisk_cleanup(void) {
    // Only remove if it exists
    if (!ramdisk_exists()) {
        return;
    }

    // Remove ramdisk directory (only if empty)
    remove_ramdisk_directory();
}
