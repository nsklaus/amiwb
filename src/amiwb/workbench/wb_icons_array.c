// File: wb_icons_array.c
// Icon Array Management - central storage and access for all workbench icons

#include "wb_internal.h"
#include "../config.h"
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Global Icon Array
// ============================================================================

#define INITIAL_ICON_CAPACITY 16

// Central icon store - all workbench icons live here
static FileIcon **icon_array = NULL;        // Dynamic array of all icons
static int icon_count = 0;                  // Current number of icons
static int icon_array_size = 0;             // Allocated size of icon array

// ============================================================================
// Array Management (Internal)
// ============================================================================

// Remove icon from array
// NOTE: This function only handles removal. Icon allocation is done via create_file_icon()
// and addition to array is done via wb_icons_array_manage()
static void manage_icons_remove(FileIcon *icon_to_remove) {
    if (!icon_to_remove) return;
    if (!icon_array) return;  // Guard against init failure

    // Remove icon from array (shifts remaining icons down)
    for (int i = 0; i < icon_count; i++) {
        if (icon_array[i] == icon_to_remove) {
            memmove(&icon_array[i], &icon_array[i + 1],
                   (icon_count - i - 1) * sizeof(FileIcon *));
            icon_count--;
            break;
        }
    }
}

// ============================================================================
// Public API - Array Access
// ============================================================================

// Get count of icons in array
int wb_icons_array_count(void) {
    return icon_count;
}

// Get pointer to icon array
FileIcon **wb_icons_array_get(void) {
    return icon_array;
}

// Add or remove icon from global array
void wb_icons_array_manage(FileIcon *icon, bool add) {
    if (add) {
        // Add existing icon to array (icon already allocated by caller)
        if (!icon) {
            log_error("[ERROR] wb_icons_array_manage: cannot add NULL icon");
            return;
        }

        // Grow array if needed
        if (icon_count >= icon_array_size) {
            int new_size = icon_array_size ? icon_array_size * 2 : INITIAL_ICON_CAPACITY;
            FileIcon **new_icons = realloc(icon_array, new_size * sizeof(FileIcon *));
            if (!new_icons) {
                log_error("[ERROR] realloc failed for icon_array (new size=%d) - icon will not appear", new_size);
                // Graceful degradation: can't grow array, so can't add this icon
                // The icon was already created, so destroy it to prevent leak
                destroy_file_icon(icon);
                return;
            }
            icon_array = new_icons;
            icon_array_size = new_size;
        }

        // Add to array
        icon_array[icon_count++] = icon;
    } else {
        manage_icons_remove(icon);
    }
}

// Get most recently added icon
FileIcon *wb_icons_array_get_last_added(void) {
    if (!icon_array) return NULL;  // Guard against init failure
    return (icon_count > 0) ? icon_array[icon_count - 1] : NULL;
}

// Get currently selected icon (any canvas)
FileIcon *wb_icons_array_get_selected(void) {
    if (!icon_array) return NULL;  // Guard against init failure
    for (int i = 0; i < icon_count; i++) {
        if (icon_array[i] && icon_array[i]->selected) {
            return icon_array[i];
        }
    }
    return NULL;
}

// Get selected icon from specific canvas
FileIcon *wb_icons_array_get_selected_from_canvas(Canvas *canvas) {
    if (!canvas) return NULL;
    if (!icon_array) return NULL;  // Guard against init failure

    for (int i = 0; i < icon_count; i++) {
        if (icon_array[i] && icon_array[i]->selected &&
            icon_array[i]->display_window == canvas->win) {
            return icon_array[i];
        }
    }
    return NULL;
}

// ============================================================================
// Helper Functions (Used by Other Modules)
// ============================================================================

// Collect icons displayed on a given canvas into a newly allocated array
// Returns count via out param
// Caller must free returned array
FileIcon **wb_icons_for_canvas(Canvas *canvas, int *out_count) {
    if (!canvas) {
        log_error("[ERROR] wb_icons_for_canvas called with NULL canvas");
        return NULL;
    }
    if (!out_count) {
        log_error("[ERROR] wb_icons_for_canvas called with NULL out_count");
        return NULL;
    }

    // Guard against init failure
    if (!icon_array) {
        *out_count = 0;
        return NULL;
    }

    // Count icons on this canvas
    int count = 0;
    for (int i = 0; i < icon_count; ++i) {
        if (icon_array[i] && icon_array[i]->display_window == canvas->win) {
            ++count;
        }
    }

    *out_count = count;
    if (count == 0) return NULL;

    // Allocate array
    FileIcon **list = (FileIcon**)malloc(sizeof(FileIcon*) * count);
    if (!list) {
        log_error("[ERROR] malloc failed for icon list (count=%d) - icon list unavailable", count);
        *out_count = 0;  // No icons available
        return NULL;  // Graceful degradation
    }

    // Collect icons
    int k = 0;
    for (int i = 0; i < icon_count; ++i) {
        FileIcon *ic = icon_array[i];
        if (ic && ic->display_window == canvas->win) {
            list[k++] = ic;
        }
    }

    return list;
}

// ============================================================================
// Initialization
// ============================================================================

// Initialize icon array (called from wb_core.c)
void wb_icons_array_init(void) {
    icon_array = malloc(INITIAL_ICON_CAPACITY * sizeof(FileIcon *));
    if (!icon_array) {
        log_error("[ERROR] malloc failed for icon_array (capacity=%d) - AmiWB will run without icons", INITIAL_ICON_CAPACITY);
        // Graceful degradation: AmiWB runs without icons rather than crashing desktop
        icon_array = NULL;
        icon_array_size = 0;
        icon_count = 0;
        return;
    }
    icon_array_size = INITIAL_ICON_CAPACITY;
    icon_count = 0;
}

// Cleanup icon array (called from wb_core.c)
void wb_icons_array_cleanup(void) {
    // Note: Icons themselves are freed by destroy_icon()
    // This just frees the array structure
    if (icon_array) {
        free(icon_array);
        icon_array = NULL;
    }
    icon_array_size = 0;
    icon_count = 0;
}

// ============================================================================
// Public API Compatibility Wrappers (for external modules)
// ============================================================================

// Get selected icon from canvas (old public API name)
FileIcon *get_selected_icon_from_canvas(Canvas *canvas) {
    return wb_icons_array_get_selected_from_canvas(canvas);
}
