// File: wb_events.c
// Event Handling - button press, release, motion, icon selection

#include "wb_internal.h"
#include "wb_public.h"
#include "../config.h"
#include "../render/rnd_public.h"
#include "../intuition/itn_public.h"
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
        select_icon(icon, canvas, event->state);
        start_drag_icon(icon, event->x, event->y);
        icon->last_click_time = event->time;
    } else {
        deselect_all_icons(canvas);
    }
    
    redraw_canvas(canvas);
}

void workbench_handle_motion_notify(XMotionEvent *event) {
    Canvas *canvas = itn_canvas_find_by_window(event->window);
    if (!canvas) return;
    
    continue_drag_icon(event, canvas);
}

void workbench_handle_button_release(XButtonEvent *event) {
    Canvas *canvas = itn_canvas_find_by_window(event->window);
    if (canvas) {
        end_drag_icon(canvas);
    }
}
