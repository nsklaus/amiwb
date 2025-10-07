// Window dragging operations
// Handles titlebar dragging, position updates, client notification

#include "itn_drag.h"
#include "itn_internal.h"
#include "../config.h"
#include <X11/Xlib.h>

// External references
extern Display *display;

// Module-private state (no longer extern - encapsulated)
static Canvas *dragging_canvas = NULL;
static int drag_start_x = 0;
static int drag_start_y = 0;
static int window_start_x = 0;
static int window_start_y = 0;

// ============================================================================
// Public API Implementation
// ============================================================================

// Start window drag from titlebar
// Returns true on success, false on failure
bool itn_drag_start(Canvas *canvas, int x_root, int y_root) {
    if (!canvas || !display) return false;

    // Initialize drag state
    dragging_canvas = canvas;
    drag_start_x = x_root;
    drag_start_y = y_root;
    window_start_x = canvas->x;
    window_start_y = canvas->y;

    // Grab pointer for smooth dragging
    XGrabPointer(display, canvas->win, False,
                ButtonReleaseMask | PointerMotionMask,
                GrabModeAsync, GrabModeAsync, None, None, CurrentTime);

    return true;
}

// Handle drag motion (updates window position)
// Returns true if drag is active and handled, false otherwise
bool itn_drag_motion(XMotionEvent *event) {
    if (!dragging_canvas || !event) return false;

    int delta_x = event->x_root - drag_start_x;
    int delta_y = event->y_root - drag_start_y;
    window_start_x += delta_x;
    // Clamp y to ensure titlebar is below menubar
    window_start_y = max(window_start_y + delta_y, MENUBAR_HEIGHT);

    // Damage old position
    DAMAGE_CANVAS(dragging_canvas);

    XMoveWindow(display, dragging_canvas->win, window_start_x, window_start_y);
    dragging_canvas->x = window_start_x;
    dragging_canvas->y = window_start_y;
    drag_start_x = event->x_root;
    drag_start_y = event->y_root;

    // Send ConfigureNotify to client window so it knows its new position
    // This is crucial for apps with menus (Steam, fs-uae-launcher, etc)
    if (dragging_canvas->client_win != None) {
        XConfigureEvent ce;
        ce.type = ConfigureNotify;
        ce.display = display;
        ce.event = dragging_canvas->client_win;
        ce.window = dragging_canvas->client_win;
        // Send root-relative coordinates (frame position + decoration offsets)
        ce.x = window_start_x + BORDER_WIDTH_LEFT;
        ce.y = window_start_y + BORDER_HEIGHT_TOP;
        // Client dimensions (subtract decorations from frame size)
        int right_border = (dragging_canvas->client_win == None ? BORDER_WIDTH_RIGHT : BORDER_WIDTH_RIGHT_CLIENT);
        ce.width = dragging_canvas->width - BORDER_WIDTH_LEFT - right_border;
        ce.height = dragging_canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM;

        ce.border_width = 0;
        ce.above = None;
        ce.override_redirect = False;
        XSendEvent(display, dragging_canvas->client_win, False,
                  StructureNotifyMask, (XEvent *)&ce);
    }

    // Damage new position
    DAMAGE_CANVAS(dragging_canvas);
    SCHEDULE_FRAME();

    return true;
}

// End window drag operation
void itn_drag_end(void) {
    if (!dragging_canvas) return;

    // Release pointer grab
    XUngrabPointer(display, CurrentTime);

    // Clear drag state
    dragging_canvas = NULL;
    drag_start_x = 0;
    drag_start_y = 0;
    window_start_x = 0;
    window_start_y = 0;
}

// Query if drag is active
bool itn_drag_is_active(void) {
    return dragging_canvas != NULL;
}

// Get the canvas being dragged (or NULL if not dragging)
Canvas *itn_drag_get_canvas(void) {
    return dragging_canvas;
}
