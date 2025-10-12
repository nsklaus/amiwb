// File: wb_events.c
// Event Handling - button press, release, motion, icon selection

#include "wb_internal.h"
#include "wb_public.h"
#include "../config.h"
#include "../render/rnd_public.h"
#include "../intuition/itn_public.h"
#include "../intuition/itn_internal.h"  // For DAMAGE_RECT, SCHEDULE_FRAME macros
#include <X11/Xlib.h>


// ============================================================================
// Selection Helpers
// ============================================================================

static bool is_double_click(Time current_time, Time last_time) {
    return current_time - last_time < 500;
}

static void select_icon(FileIcon *icon, Canvas *canvas, unsigned int state) {
    FileIcon **icons = wb_icons_array_get();
    int count = wb_icons_array_count();
    bool ctrl = (state & ControlMask) != 0;
    
    if (!ctrl) {
        // Exclusive selection
        for (int i = 0; i < count; i++) {
            if (icons[i] != icon && icons[i]->display_window == canvas->win && icons[i]->selected) {
                icons[i]->selected = false;
                icons[i]->current_picture = icons[i]->normal_picture;
            }
        }
        icon->selected = true;
    } else {
        // Toggle selection
        icon->selected = !icon->selected;
    }
    icon->current_picture = icon->selected ? icon->selected_picture : icon->normal_picture;
}

static void deselect_all_icons(Canvas *canvas) {
    FileIcon **icons = wb_icons_array_get();
    int count = wb_icons_array_count();

    for (int i = 0; i < count; i++) {
        if (icons[i]->display_window == canvas->win && icons[i]->selected) {
            icons[i]->selected = false;
            icons[i]->current_picture = icons[i]->normal_picture;
        }
    }
}

// ============================================================================
// Mouse Multiselection State (AWP - module-private)
// ============================================================================

static bool multiselect_pending = false;           // Button pressed, awaiting 10px threshold
static bool multiselect_active = false;            // Threshold crossed, drawing active
static int multiselect_start_x = 0;                // Initial click position
static int multiselect_start_y = 0;
static int multiselect_current_x = 0;              // Current mouse position
static int multiselect_current_y = 0;
static Canvas *multiselect_target_canvas = NULL;   // Canvas where selection is happening

// ============================================================================
// Mouse Multiselection Functions (AWP - module-private)
// ============================================================================

// Update icon selection state based on current rectangle (live updates)
static void multiselect_update_live_selection(Canvas *canvas, int x1, int y1, int x2, int y2) {
    if (!canvas) return;

    FileIcon **icons = wb_icons_array_get();
    int count = wb_icons_array_count();
    if (!icons || count <= 0) return;

    // Normalize rectangle bounds
    int left = min(x1, x2);
    int top = min(y1, y2);
    int right = max(x1, x2);
    int bottom = max(y1, y2);

    // Get offsets (pattern from find_icon() in wb_icons_ops.c:38-46)
    int base_x = (canvas->type == WINDOW) ? BORDER_WIDTH_LEFT : 0;
    int base_y = (canvas->type == WINDOW) ? BORDER_HEIGHT_TOP : 0;
    int sx = canvas->scroll_x;
    int sy = canvas->scroll_y;

    // Update all icons on this canvas
    for (int i = 0; i < count; i++) {
        FileIcon *icon = icons[i];
        if (icon->display_window != canvas->win) continue;

        // Calculate icon bounds in screen coordinates
        int icon_x, icon_y, icon_w, icon_h;

        if (canvas->type == WINDOW && canvas->view_mode == VIEW_NAMES) {
            // List view - only text area selectable (pattern from find_icon:57-65)
            int row_h = 18 + 6;
            int text_left_pad = 6;
            icon_x = base_x + icon->x + text_left_pad - sx;
            icon_y = base_y + icon->y - sy;
            icon_w = get_text_width(icon->label ? icon->label : "");
            icon_h = row_h;
        } else {
            // Icon view - icon + label area (pattern from find_icon:66-73)
            icon_x = base_x + icon->x - sx;
            icon_y = base_y + icon->y - sy;
            icon_w = icon->width;
            icon_h = icon->height + 20;  // Include label pad
        }

        // Rectangle intersection test
        bool intersects = !(icon_x + icon_w < left || icon_x > right ||
                           icon_y + icon_h < top || icon_y > bottom);

        // Update selection state immediately (live feedback)
        if (intersects && !icon->selected) {
            icon->selected = true;
            icon->current_picture = icon->selected_picture;
        } else if (!intersects && icon->selected) {
            icon->selected = false;
            icon->current_picture = icon->normal_picture;
        }
    }
}

// Start multiselection - simple canvas-based drawing
static void multiselect_start(Canvas *canvas) {
    if (!canvas || multiselect_active) return;
    multiselect_active = true;
}

// Update rectangle and icon selection as mouse moves
static void multiselect_update(Canvas *canvas, int x, int y) {
    if (!canvas || !multiselect_active) return;

    // Clamp Y to protect menubar
    y = max(y, MENUBAR_HEIGHT);

    // Save current position
    multiselect_current_x = x;
    multiselect_current_y = y;

    // Update live icon selection based on current rectangle
    multiselect_update_live_selection(canvas, multiselect_start_x, multiselect_start_y, x, y);

    // Refresh canvas to show updated rectangle and icon selection
    redraw_canvas(canvas);

    // Damage and schedule frame
    DAMAGE_CANVAS(canvas);
    SCHEDULE_FRAME();
}

// Complete selection and cleanup
static void multiselect_end(void) {
    if (!multiselect_active) return;

    Canvas *target = multiselect_target_canvas;

    // Reset state flags
    multiselect_active = false;
    multiselect_pending = false;
    multiselect_target_canvas = NULL;

    // Refresh canvas to remove rectangle
    if (target) {
        redraw_canvas(target);
    }
}

// PUBLIC: Draw selection rectangle on canvas (called by render system)
void wb_draw_multiselection_rect(Canvas *canvas, Drawable d, Visual *visual) {
    if (!canvas || !multiselect_active || canvas != multiselect_target_canvas) return;

    Display *dpy = itn_core_get_display();
    if (!dpy || d == None) return;

    // Draw selection rectangle using render utility
    rnd_draw_selection_rect(dpy, d, visual,
                           multiselect_start_x, multiselect_start_y,
                           multiselect_current_x, multiselect_current_y);
}

// ============================================================================
// Event Handlers
// ============================================================================

void workbench_handle_button_press(XButtonEvent *event) {
    Canvas *canvas = itn_canvas_find_by_window(event->window);
    if (!canvas) return;

    // Desktop clicks deactivate windows
    if (canvas->type == DESKTOP) {
        deactivate_all_windows();
        safe_set_input_focus(itn_core_get_display(), canvas->win, RevertToParent, CurrentTime);
    }

    FileIcon *icon = find_icon(event->window, event->x, event->y);
    if (icon && event->button == Button1) {
        // Handle double-click
        if (is_double_click(event->time, icon->last_click_time)) {
            // Save click time BEFORE opening (open_directory destroys icons)
            Time click_time = event->time;

            if (icon->type == TYPE_DRAWER || icon->type == TYPE_DEVICE) {
                open_directory(icon, canvas);
                // Icon is now freed, don't access it
            } else if (icon->type == TYPE_FILE) {
                open_file(icon);
                icon->last_click_time = click_time;  // Icon still valid for files
            } else if (icon->type == TYPE_ICONIFIED) {
                wb_icons_restore_iconified(icon);
                // Icon freed by restore, don't access
            }
            redraw_canvas(canvas);
            return;
        }

        // Single click: select and prepare drag
        // Ensure multiselect is cleared before starting drag (mutual exclusion)
        multiselect_pending = false;
        multiselect_active = false;
        multiselect_target_canvas = NULL;

        select_icon(icon, canvas, event->state);
        start_drag_icon(icon, event->x, event->y);
        icon->last_click_time = event->time;
    } else {
        // Empty space click - prepare for potential multiselection

        // ONLY allow multiselection on desktop and workbench windows (not client windows)
        if (canvas->type != DESKTOP && (canvas->type != WINDOW || canvas->client_win != None)) {
            return;  // Client windows and other types: no multiselection
        }

        // Protect menubar - ignore clicks in sacred ground
        if (event->y < MENUBAR_HEIGHT) {
            return;
        }

        // Ensure drag is cleared before starting multiselection (mutual exclusion)
        if (wb_drag_get_dragged_icon() != NULL) {
            // Drag is active, abort multiselect to enforce mutual exclusion
            return;
        }

        // Deselect all icons before starting multiselection
        deselect_all_icons(canvas);

        // Clamp start position to content area
        int start_x = event->x;
        int start_y = max(event->y, MENUBAR_HEIGHT);
        if (canvas->type == WINDOW) {
            // Workbench window: respect borders
            start_x = clamp_value_between(start_x, BORDER_WIDTH_LEFT, canvas->width - BORDER_WIDTH_RIGHT);
            start_y = clamp_value_between(start_y, BORDER_HEIGHT_TOP, canvas->height - BORDER_HEIGHT_BOTTOM);
        }

        // Enter pending state - await 10px drag threshold
        multiselect_pending = true;
        multiselect_start_x = start_x;
        multiselect_start_y = start_y;
        multiselect_target_canvas = canvas;
    }

    redraw_canvas(canvas);
}

void workbench_handle_motion_notify(XMotionEvent *event) {
    Canvas *canvas = itn_canvas_find_by_window(event->window);
    if (!canvas) return;

    // Handle multiselection ONLY if THIS canvas is the target (scoped check)
    if (canvas == multiselect_target_canvas && (multiselect_pending || multiselect_active)) {
        // Check 10px threshold before activating (pattern from wb_drag.c:274-286)
        if (multiselect_pending && !multiselect_active) {
            // Enforce mutual exclusion: prevent multiselect activation if drag is active
            if (wb_drag_get_dragged_icon() != NULL) {
                // Drag is active, abort multiselect pending state
                multiselect_pending = false;
                multiselect_target_canvas = NULL;
                return;
            }

            int dx = event->x - multiselect_start_x;
            int dy = event->y - multiselect_start_y;

            // Require 10 pixel movement (prevents accidental activation)
            if (dx*dx + dy*dy < 10*10) {
                return;  // Below threshold, stay pending
            }

            // Threshold crossed - activate multiselection
            multiselect_start(canvas);
        }

        // Update rectangle and icon selection if active
        if (multiselect_active) {
            multiselect_update(canvas, event->x, event->y);
        }
        return;  // Multiselection handled, done
    }

    continue_drag_icon(event, canvas);
}

void workbench_handle_button_release(XButtonEvent *event) {
    Canvas *canvas = itn_canvas_find_by_window(event->window);
    if (!canvas) return;

    // CRITICAL: Clean up BOTH drag and multiselect states unconditionally
    // This ensures no state leakage if both somehow became active (bug, race condition, etc.)
    // DO NOT use early returns here - both states must always be cleaned up on release

    // Clean up multiselection state if THIS canvas is the target
    if (canvas == multiselect_target_canvas && (multiselect_pending || multiselect_active)) {
        if (multiselect_active) {
            // Active state - complete selection
            multiselect_end();
        } else {
            // Pending state - below threshold, just clear flags (no selection)
            multiselect_pending = false;
            multiselect_target_canvas = NULL;
        }
    }

    // Clean up drag state unconditionally (even if multiselect was active)
    // This ensures mutual exclusion is enforced and prevents stuck drag states
    end_drag_icon(canvas);

    // Note: Both cleanups always run, ensuring complete state cleanup on every release
}
