// File: wb_core.c
// Workbench Core - initialization, cleanup, and main public API

#define _POSIX_C_SOURCE 200809L
#include "wb_internal.h"
#include "wb_spatial.h"
#include "../config.h"
#include "../render/rnd_public.h"
#include "../intuition/itn_internal.h"
#include "../events/evt_public.h"
#include "../diskdrives.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <X11/Xlib.h>

// Module initialization flag
static bool wb_initialized = false;

// Forward declarations for internal helpers (open_directory exported for wb_events.c)
void open_directory(FileIcon *icon, Canvas *current_canvas);

// ============================================================================
// Initialization and Cleanup
// ============================================================================

void init_workbench(void) {
    // Icon array is managed by wb_icons_array.c, but we need to ensure it's initialized
    // Signal handling to avoid zombies from file launches
    signal(SIGCHLD, SIG_IGN);

    // Load default icons
    wb_deficons_load();

    // Scan Desktop directory and create icons
    Canvas *desktop = itn_canvas_get_desktop();
    refresh_canvas_from_directory(desktop, NULL);  // NULL means use ~/Desktop
    redraw_canvas(desktop);

    wb_initialized = true;
}

void cleanup_workbench(void) {
    if (!wb_initialized) return;
    wb_initialized = false;

    // Ensure drag resources are released
    workbench_cleanup_drag_state();

    // Destroy all icons
    FileIcon **icons = wb_icons_array_get();
    int count = wb_icons_array_count();
    for (int i = count - 1; i >= 0; i--) {
        destroy_icon(icons[i]);
    }

    // Note: Icon array cleanup is handled by wb_icons_array.c
    // Note: Deficon array cleanup is handled by wb_deficons.c
}

// ============================================================================
// Directory Operations
// ============================================================================

// Open directory (exported for wb_events.c)
void open_directory(FileIcon *icon, Canvas *current_canvas) {
    if (!icon || !icon->path) return;

    // Non-spatial mode: reuse current window
    if (!get_spatial_mode() && current_canvas && current_canvas->type == WINDOW) {
        char *new_path = strdup(icon->path);

        // Update window title (use drive label if it's a device mount point)
        const char *dir_name;
        DiskDrive *drive = diskdrives_find_by_path(new_path);
        if (drive) {
            dir_name = drive->label;  // Use drive label (e.g., "Ram Disk", "System", "Home")
        } else {
            dir_name = strrchr(new_path, '/');
            if (dir_name) dir_name++;
            else dir_name = new_path;
        }

        // Free old paths
        if (current_canvas->path) free(current_canvas->path);
        if (current_canvas->title_base) free(current_canvas->title_base);

        // Set new paths
        current_canvas->path = new_path;
        current_canvas->title_base = strdup(dir_name);

        // Recalculate cached title width (cache invalidation after title change)
        itn_decorations_recalc_title_width(current_canvas);

        // Refresh with new directory
        refresh_canvas_from_directory(current_canvas, current_canvas->path);

        // Reset scroll
        current_canvas->scroll_x = 0;
        current_canvas->scroll_y = 0;

        icon_cleanup(current_canvas);
        redraw_canvas(current_canvas);
        return;
    }

    // Check if window for this path exists
    Canvas *existing = find_window_by_path(icon->path);
    if (existing) {
        // Check if iconified
        XWindowAttributes attrs;
        if (safe_get_window_attributes(itn_core_get_display(), existing->win, &attrs)) {
            if (attrs.map_state != IsViewable) {
                // Find and restore iconified window
                FileIcon **icons = wb_icons_array_get();
                int count = wb_icons_array_count();
                for (int i = 0; i < count; i++) {
                    FileIcon *ic = icons[i];
                    if (ic && ic->type == TYPE_ICONIFIED && ic->iconified_canvas == existing) {
                        wb_icons_restore_iconified(ic);
                        return;
                    }
                }
            }
        }
        // Window visible - raise it
        itn_focus_set_active(existing);
        XRaiseWindow(itn_core_get_display(), existing->win);
        redraw_canvas(existing);
        return;
    }

    // Load spatial geometry or use cascade fallback
    int x, y, width, height;
    wb_spatial_load_geometry(icon->path, &x, &y, &width, &height);

    // Create new window with loaded/calculated geometry
    Canvas *new_canvas = create_canvas(icon->path, x, y, width, height, WINDOW);
    if (new_canvas) {
        refresh_canvas_from_directory(new_canvas, icon->path);
        redraw_canvas(new_canvas);
        itn_focus_set_active(new_canvas);
    }
}

void workbench_open_directory(const char *path) {
    if (!path || !path[0]) return;

    // Create temporary icon to reuse open_directory
    FileIcon temp_icon = {0};
    temp_icon.path = (char *)path;
    temp_icon.type = TYPE_DRAWER;

    open_directory(&temp_icon, NULL);
}

// ============================================================================
// File Operations
// ============================================================================

void open_file(FileIcon *icon) {
    if (!icon || !icon->path) return;

    // Directories should open in AmiWB
    if (icon->type == TYPE_DRAWER || icon->type == TYPE_DEVICE) {
        Canvas *c = itn_canvas_find_by_window(icon->display_window);
        if (c) open_directory(icon, c);
        return;
    }

    // Launch file with xdg-open
    // TODO: implement launch_with_hook properly
    char command[PATH_SIZE * 2 + 32];
    snprintf(command, sizeof(command), "xdg-open '%s'", icon->path);

    // Simple launch (hook system integration needed)
    if (fork() == 0) {
        setsid();
        system(command);
        _exit(0);
    }
}

void workbench_create_new_drawer(Canvas *target_canvas) {
    if (!target_canvas) return;

    char target_path[PATH_SIZE];
    snprintf(target_path, PATH_SIZE, "%s", target_canvas->path);

    // Find unique name
    char new_dir_name[NAME_SIZE];
    char full_path[PATH_SIZE];
    int counter = 0;

    while (1) {
        if (counter == 0) {
            snprintf(new_dir_name, NAME_SIZE, "Unnamed_dir");
        } else {
            snprintf(new_dir_name, NAME_SIZE, "Unnamed_dir_%d", counter);
        }

        int ret = snprintf(full_path, PATH_SIZE, "%s/%s", target_path, new_dir_name);
        if (ret >= PATH_SIZE) {
            log_error("[ERROR] Path too long for new directory: %s/%s", target_path, new_dir_name);
            return;
        }

        struct stat st;
        if (stat(full_path, &st) != 0) {
            break;
        }
        counter++;

        if (counter > 999) {
            log_error("[ERROR] Cannot find unique name for new directory");
            return;
        }
    }

    // Create directory
    if (mkdir(full_path, 0755) != 0) {
        log_error("[ERROR] Failed to create directory: %s - %s", full_path, strerror(errno));
        return;
    }

    // Get icon path
    const char *icon_path = wb_deficons_get_for_file(new_dir_name, true);
    if (!icon_path) {
        log_error("[WARNING] No def_dir.info available for directory icon");
        return;
    }

    // Find free position
    int new_x, new_y;
    wb_layout_find_free_slot(target_canvas, &new_x, &new_y);

    // Build full path
    char new_dir_full[FULL_SIZE];
    snprintf(new_dir_full, sizeof(new_dir_full), "%s/%s", target_path, new_dir_name);

    // Create icon
    FileIcon *new_icon = wb_icons_create_with_icon_path(icon_path, target_canvas,
                                                    new_x, new_y, new_dir_full,
                                                    new_dir_name, TYPE_DRAWER);
    if (!new_icon) {
        log_error("[ERROR] Failed to create icon for new directory: %s", full_path);
        return;
    }

    // Update display
    wb_layout_compute_bounds(target_canvas);
    compute_max_scroll(target_canvas);
    redraw_canvas(target_canvas);
}
