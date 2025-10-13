// Window button interaction handling
// Handles close, iconify, maximize, lower, and resize buttons

#include "itn_buttons.h"
#include "itn_internal.h"
#include "../render/rnd_public.h"
#include "../config.h"
#include <X11/Xlib.h>

// Hit test results (from itn_events.c)
typedef enum {
    HIT_NONE,      // 0
    HIT_CLOSE,     // 1
    HIT_LOWER,     // 2
    HIT_ICONIFY,   // 3
    HIT_MAXIMIZE,  // 4
    HIT_TITLEBAR,  // 5
    HIT_RESIZE_N,  // 6
    HIT_RESIZE_NE, // 7
    HIT_RESIZE_E,  // 8
    HIT_RESIZE_SE, // 9
    HIT_RESIZE_S,  // 10
    HIT_RESIZE_SW, // 11
    HIT_RESIZE_W,  // 12
    HIT_RESIZE_NW  // 13
} TitlebarHit;

// ============================================================================
// Public API Implementation
// ============================================================================

// Handle button press on window controls
// Returns true if button was pressed, false otherwise
bool itn_buttons_handle_press(Canvas *canvas, XButtonEvent *event) {
    if (!canvas || !event || event->button != Button1) {
        return false;
    }

    int hit = hit_test(canvas, event->x, event->y);

    switch (hit) {
        case HIT_CLOSE:
            canvas->close_armed = true;
            redraw_canvas(canvas);
            DAMAGE_CANVAS(canvas);
            SCHEDULE_FRAME();
            return true;

        case HIT_ICONIFY:
            canvas->iconify_armed = true;
            redraw_canvas(canvas);
            DAMAGE_CANVAS(canvas);
            SCHEDULE_FRAME();
            return true;

        case HIT_MAXIMIZE:
            canvas->maximize_armed = true;
            redraw_canvas(canvas);
            DAMAGE_CANVAS(canvas);
            SCHEDULE_FRAME();
            return true;

        case HIT_LOWER:
            canvas->lower_armed = true;
            redraw_canvas(canvas);
            DAMAGE_CANVAS(canvas);
            SCHEDULE_FRAME();
            return true;

        case HIT_RESIZE_NE:
        case HIT_RESIZE_NW:
        case HIT_RESIZE_SE:
        case HIT_RESIZE_SW:
        case HIT_RESIZE_N:
        case HIT_RESIZE_S:
        case HIT_RESIZE_E:
        case HIT_RESIZE_W:
            canvas->resize_armed = true;
            redraw_canvas(canvas);
            DAMAGE_CANVAS(canvas);
            SCHEDULE_FRAME();
            // Start resize operation with the specific corner/edge
            itn_resize_start(canvas, hit);
            return true;

        default:
            return false;
    }
}

// Handle button release on window controls
// Returns true if button was released, false otherwise
bool itn_buttons_handle_release(Canvas *canvas, XButtonEvent *event) {
    if (!canvas || !event) {
        return false;
    }

    bool handled = false;
    TitlebarHit hit = hit_test(canvas, event->x, event->y);

    // Handle resize button release
    if (canvas->resize_armed) {
        canvas->resize_armed = false;
        redraw_canvas(canvas);
        DAMAGE_CANVAS(canvas);
        SCHEDULE_FRAME();
        handled = true;
        // Note: Resize already began on press, no action needed on release
    }

    // Handle close button release
    if (canvas->close_armed) {
        canvas->close_armed = false;
        redraw_canvas(canvas);
        DAMAGE_CANVAS(canvas);
        SCHEDULE_FRAME();
        if (hit == HIT_CLOSE) {
            request_client_close(canvas);
        }
        handled = true;
    }

    // Handle iconify button release
    if (canvas->iconify_armed) {
        canvas->iconify_armed = false;
        redraw_canvas(canvas);
        DAMAGE_CANVAS(canvas);
        SCHEDULE_FRAME();
        if (hit == HIT_ICONIFY) {
            iconify_canvas(canvas);
        }
        handled = true;
    }

    // Handle maximize button release
    if (canvas->maximize_armed) {
        canvas->maximize_armed = false;
        redraw_canvas(canvas);
        DAMAGE_CANVAS(canvas);
        SCHEDULE_FRAME();
        if (hit == HIT_MAXIMIZE) {
            Canvas *desk = itn_canvas_get_desktop();
            if (desk) {
                if (!canvas->maximized) {
                    // Save current position and dimensions before maximizing
                    canvas->pre_max_x = canvas->x;
                    canvas->pre_max_y = canvas->y;
                    canvas->pre_max_w = canvas->width;
                    canvas->pre_max_h = canvas->height;

                    // Maximize the window
                    int new_w = desk->width;
                    int new_h = desk->height - (MENUBAR_HEIGHT - 1);
                    itn_geometry_move_resize(canvas, 0, MENUBAR_HEIGHT, new_w, new_h);
                    canvas->maximized = true;
                } else {
                    // Restore to saved dimensions
                    itn_geometry_move_resize(canvas, canvas->pre_max_x, canvas->pre_max_y,
                                        canvas->pre_max_w, canvas->pre_max_h);
                    canvas->maximized = false;
                }
            }
        }
        handled = true;
    }

    // Handle lower button release
    if (canvas->lower_armed) {
        canvas->lower_armed = false;
        redraw_canvas(canvas);
        DAMAGE_CANVAS(canvas);
        SCHEDULE_FRAME();
        if (hit == HIT_LOWER) {
            itn_geometry_lower(canvas);
            itn_focus_activate_window_behind(canvas);
            // Let compositor handle stacking through ConfigureNotify events
        }
        handled = true;
    }

    return handled;
}

// Cancel armed buttons when mouse moves away from button area
// Returns true if any button state changed, false otherwise
bool itn_buttons_handle_motion_cancel(Canvas *canvas, XMotionEvent *event) {
    if (!canvas || !event) {
        return false;
    }

    // Check if mouse is outside window bounds entirely
    bool outside_window = (event->x < 0 || event->y < 0 ||
                          event->x >= canvas->width || event->y >= canvas->height);

    // Cancel any armed buttons if mouse moves outside or too far
    bool needs_redraw = false;

    if (canvas->close_armed && (outside_window || hit_test(canvas, event->x, event->y) != HIT_CLOSE)) {
        canvas->close_armed = false;
        needs_redraw = true;
    }
    if (canvas->iconify_armed && (outside_window || hit_test(canvas, event->x, event->y) != HIT_ICONIFY)) {
        canvas->iconify_armed = false;
        needs_redraw = true;
    }
    if (canvas->maximize_armed && (outside_window || hit_test(canvas, event->x, event->y) != HIT_MAXIMIZE)) {
        canvas->maximize_armed = false;
        needs_redraw = true;
    }
    if (canvas->lower_armed && (outside_window || hit_test(canvas, event->x, event->y) != HIT_LOWER)) {
        canvas->lower_armed = false;
        needs_redraw = true;
    }

    if (needs_redraw) {
        redraw_canvas(canvas);
        DAMAGE_CANVAS(canvas);
        SCHEDULE_FRAME();
    }

    return needs_redraw;
}
