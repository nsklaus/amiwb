// File: wb_icons_create.c
// Icon Creation and Destruction - lifecycle management for workbench icons

#define _POSIX_C_SOURCE 200809L
#include "wb_internal.h"
#include "../config.h"
#include "../icons.h"
#include "../render/rnd_public.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// Forward declarations for helper functions
static void find_next_desktop_slot(Canvas *desk, int *ox, int *oy, int icon_width);
static const char* find_icon_with_user_override(const char *icon_name, char *buffer, size_t buffer_size);
void create_icon_with_type(const char *path, Canvas *canvas, int x, int y, int type);

// ============================================================================
// Icon Creation
// ============================================================================

// Create icon with full metadata (icon_path for .info, full_path for actual file)
FileIcon *wb_icons_create_with_icon_path(const char *icon_path, Canvas *canvas, int x, int y,
                                    const char *full_path, const char *name, int type) {
    create_icon_with_type(icon_path, canvas, x, y, type);
    FileIcon *icon = wb_icons_array_get_last_added();
    if (icon) {
        // Update with actual file path and name (not .info path)
        if (full_path) {
            if (icon->path) free(icon->path);
            icon->path = strdup(full_path);
            if (!icon->path) {
                log_error("[ERROR] strdup failed for icon path - keeping original path");
                // Graceful degradation: keep old path rather than crashing
            }
        }
        if (name) {
            if (icon->label) free(icon->label);
            icon->label = strdup(name);
            if (!icon->label) {
                log_error("[ERROR] strdup failed for icon label - keeping original label");
                // Graceful degradation: keep old label rather than crashing
            }
        }
        icon->type = type;
    }
    return icon;
}

// Create icon with explicit type
void create_icon_with_type(const char *path, Canvas *canvas, int x, int y, int type) {
    // Allocate icon structure using icons.c module
    FileIcon* icon = create_file_icon(path, x, y, type, canvas->win, get_render_context());
    if (!icon) {
        log_error("[ERROR] Failed to create icon for path '%s'", path);
        return;
    }

    // Add to global array (workbench module manages the array)
    wb_icons_array_manage(icon, true);
}

// Create icon - determines type from filesystem
void create_icon(const char *path, Canvas *canvas, int x, int y) {
    struct stat st;
    int type = TYPE_FILE;  // Default to file if stat fails
    if (stat(path, &st) == 0) {
        type = S_ISDIR(st.st_mode) ? TYPE_DRAWER : TYPE_FILE;
    }
    create_icon_with_type(path, canvas, x, y, type);
}

// ============================================================================
// Icon Destruction
// ============================================================================

// OWNERSHIP: Complete cleanup - frees Pictures, paths, label, and icon struct
void destroy_icon(FileIcon *icon) {
    if (!icon) return;

    // If this icon is currently being dragged, clean up drag state
    if (icon == wb_drag_get_dragged_icon()) {
        if (wb_drag_is_active()) {
            wb_drag_cleanup_window();
            wb_drag_set_inactive();
        }
        wb_drag_clear_dragged_icon();
    }

    // Remove from icon management array
    wb_icons_array_manage(icon, false);

    // Free icon and all its resources (handled by icons.c now)
    destroy_file_icon(icon);
}

// Remove icon for a specific iconified canvas
void remove_icon_for_canvas(Canvas *canvas) {
    if (!canvas) return;
    
    FileIcon **icon_array = wb_icons_array_get();
    int icon_count = wb_icons_array_count();
    
    for (int i = 0; i < icon_count; i++) {
        FileIcon *ic = icon_array[i];
        if (ic && ic->type == TYPE_ICONIFIED && ic->iconified_canvas == canvas) {
            destroy_icon(ic);
            break;
        }
    }
}

// ============================================================================
// Desktop Slot Management (for Iconified Windows)
// ============================================================================

static void find_next_desktop_slot(Canvas *desk, int *ox, int *oy, int icon_width) {
    if (!desk || !ox || !oy) return;
    const int sx = 20, step_x = 110;
    
    FileIcon **arr = wb_icons_array_get();
    int n = wb_icons_array_count();
    
    // Calculate start position: Home icon top + 80px gap
    int first_iconified_y = 120 + 80;
    
    // Find next free slot
    for (int x = sx; x < desk->width - 64; x += step_x) {
        int y = first_iconified_y;
        
        bool collision_found;
        do {
            collision_found = false;
            for (int i = 0; i < n; i++) {
                FileIcon *ic = arr[i];
                if (ic->display_window != desk->win) continue;
                
                bool same_column = (ic->x >= x && ic->x < x + step_x) || 
                                  (x >= ic->x && x < ic->x + ic->width);
                if (same_column && ic->y == y) {
                    y += 80;
                    collision_found = true;
                    break;
                }
            }
        } while (collision_found && y + 64 < desk->height);
        
        if (y + 64 < desk->height) {
            // Center icon within column using actual icon width (same logic as icon_cleanup)
            int column_center_offset = (step_x - icon_width) / 2;
            if (column_center_offset < 0) column_center_offset = 0;
            *ox = x + column_center_offset;
            *oy = y;
            return;
        }
    }
    // Fallback: center in first column using actual icon width
    int column_center_offset = (step_x - icon_width) / 2;
    if (column_center_offset < 0) column_center_offset = 0;
    *ox = sx + column_center_offset;
    *oy = first_iconified_y;
}

static const char* find_icon_with_user_override(const char *icon_name, char *buffer, size_t buffer_size) {
    struct stat st;
    
    // Check user directory first
    const char *home = getenv("HOME");
    if (home) {
        snprintf(buffer, buffer_size, "%s/.config/amiwb/icons/%s", home, icon_name);
        if (stat(buffer, &st) == 0) {
            log_error("[ICON] Using user icon: %s", buffer);
            return buffer;
        }
    }
    
    // Check system directory
    snprintf(buffer, buffer_size, "/usr/local/share/amiwb/icons/%s", icon_name);
    if (stat(buffer, &st) == 0) {
        return buffer;
    }
    
    return NULL;
}

// ============================================================================
// Iconified Window Icons
// ============================================================================

FileIcon* create_iconified_icon(Canvas *c) {
    if (!c || (c->type != WINDOW && c->type != DIALOG)) return NULL;
    
    Canvas *desk = itn_canvas_get_desktop();
    if (!desk) return NULL;

    const char *icon_path = NULL;
    char *label = NULL;
    const char *def_foo_path = "/usr/local/share/amiwb/icons/def_icons/def_foo.info";

    label = c->title_base ? strdup(c->title_base) : strdup("Untitled");

    char icon_buffer[PATH_SIZE];

    if (c->client_win == None) {
        if (c->type == DIALOG) {
            const char *dialog_icon_name = "dialog.info";
            if (c->title_base) {
                if (strstr(c->title_base, "Rename")) dialog_icon_name = "rename.info";
                else if (strstr(c->title_base, "Delete")) dialog_icon_name = "delete.info";
                else if (strstr(c->title_base, "Execute")) dialog_icon_name = "execute.info";
                else if (strstr(c->title_base, "Progress") || strstr(c->title_base, "Copying") ||
                         strstr(c->title_base, "Moving")) dialog_icon_name = "progress.info";
                else if (strstr(c->title_base, "Information")) dialog_icon_name = "iconinfo.info";
            }

            icon_path = find_icon_with_user_override(dialog_icon_name, icon_buffer, sizeof(icon_buffer));
            if (!icon_path) icon_path = find_icon_with_user_override("dialog.info", icon_buffer, sizeof(icon_buffer));
            if (!icon_path) icon_path = find_icon_with_user_override("filer.info", icon_buffer, sizeof(icon_buffer));
        } else {
            icon_path = find_icon_with_user_override("filer.info", icon_buffer, sizeof(icon_buffer));
        }
    } else {
        char app_icon_name[NAME_SIZE];
        snprintf(app_icon_name, sizeof(app_icon_name), "%s.info", c->title_base);
        icon_path = find_icon_with_user_override(app_icon_name, icon_buffer, sizeof(icon_buffer));
        if (!icon_path) {
            log_error("[ICON] Couldn't find %s, using def_foo.info", app_icon_name);
            icon_path = def_foo_path;
        }
    }

    // Verify path exists
    struct stat st;
    if (stat(icon_path, &st) != 0) {
        log_error("[WARNING] Icon file not found: %s, using def_foo.info", icon_path);
        icon_path = def_foo_path;
    }

    // Create icon at temporary position first (so we can get actual width)
    create_icon(icon_path, desk, 0, 0);
    FileIcon *ni = wb_icons_array_get_last_added();

    if (!ni) {
        log_error("[ERROR] Failed to create iconified icon");
        free(label);
        return NULL;
    }

    // Now find proper slot using actual icon width (same centering as icon_cleanup)
    int nx = 20, ny = 40;
    find_next_desktop_slot(desk, &nx, &ny, ni->width);

    // Move icon to properly centered position
    ni->x = nx;
    ni->y = ny;

    // Set up as iconified icon
    ni->type = TYPE_ICONIFIED;
    ni->iconified_canvas = c;
    if (ni->label) free(ni->label);
    ni->label = label;

    return ni;
}

// ============================================================================
// Prime Desktop Icons
// ============================================================================

void add_prime_desktop_icons(Canvas *desktop) {
    if (!desktop) return;
    // Commented out - now handled by diskdrives.c
}
