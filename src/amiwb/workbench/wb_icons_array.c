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

// Manage icons: add new icon or remove existing icon
// When adding (add=true): returns pointer to newly allocated icon
// When removing (add=false): removes icon_to_remove from array
static FileIcon *manage_icons(bool add, FileIcon *icon_to_remove) {
    if (add) {
        // Grow array if needed
        if (icon_count >= icon_array_size) {
            icon_array_size = icon_array_size ? icon_array_size * 2 : INITIAL_ICON_CAPACITY;
            FileIcon **new_icons = realloc(icon_array, icon_array_size * sizeof(FileIcon *));
            if (!new_icons) {
                log_error("[ERROR] realloc failed for icon_array (new size=%d)", icon_array_size);
                exit(1);  // Kill on malloc failure per guidelines
            }
            icon_array = new_icons;
        }

        // Allocate new icon structure
        FileIcon *new_icon = calloc(1, sizeof(FileIcon));
        if (!new_icon) {
            log_error("[ERROR] calloc failed for FileIcon structure");
            exit(1);  // Kill on malloc failure per guidelines
        }

        // Add to array
        icon_array[icon_count++] = new_icon;
        return new_icon;

    } else if (icon_to_remove) {
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
    return NULL;
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
            icon_array_size = icon_array_size ? icon_array_size * 2 : INITIAL_ICON_CAPACITY;
            FileIcon **new_icons = realloc(icon_array, icon_array_size * sizeof(FileIcon *));
            if (!new_icons) {
                log_error("[ERROR] realloc failed for icon_array (new size=%d)", icon_array_size);
                exit(1);  // Kill on malloc failure per guidelines
            }
            icon_array = new_icons;
        }

        // Add to array
        icon_array[icon_count++] = icon;
    } else {
        manage_icons(false, icon);
    }
}

// Get most recently added icon
FileIcon *wb_icons_array_get_last_added(void) {
    return (icon_count > 0) ? icon_array[icon_count - 1] : NULL;
}

// Get currently selected icon (any canvas)
FileIcon *wb_icons_array_get_selected(void) {
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
        log_error("[ERROR] malloc failed for icon list (count=%d)", count);
        exit(1);  // Kill on malloc failure per guidelines
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
        log_error("[ERROR] malloc failed for icon_array (capacity=%d)", INITIAL_ICON_CAPACITY);
        exit(1);  // Kill on malloc failure per guidelines
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

// Get icon count (old public API name)
int get_icon_count(void) {
    return wb_icons_array_count();
}

// Get icon array (old public API name)
FileIcon **get_icon_array(void) {
    return wb_icons_array_get();
}

// Get selected icon from canvas (old public API name)
FileIcon *get_selected_icon_from_canvas(Canvas *canvas) {
    return wb_icons_array_get_selected_from_canvas(canvas);
}
