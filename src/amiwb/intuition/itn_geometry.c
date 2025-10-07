// Window sizing, positioning, and stacking
// This module handles geometry changes with automatic damage tracking

#include "../config.h"
#include "itn_internal.h"
#include "../render_public.h"
#include <X11/Xlib.h>
#include <X11/Xatom.h>  // For XA_ATOM
#include <math.h>       // For fmax, fmin, roundf
#include "../render.h"  // For apply_resize_and_redraw (temporary)

// External references (temporary during migration)
// get_desktop_canvas is now itn_canvas_get_desktop
// find_canvas is now itn_canvas_find_by_window
extern void compute_max_scroll(Canvas *c);  // From intuition.c
// get_right_border_width is inline in intuition.h

void itn_geometry_move(Canvas *canvas, int x, int y) {
    if (!canvas) return;

    Display *dpy = itn_core_get_display();
    if (!dpy || !canvas->win) return;

    // Accumulate damage for old position
    DAMAGE_RECT(canvas->x, canvas->y, canvas->width, canvas->height);

    // Update position
    canvas->x = x;
    canvas->y = y;
    XMoveWindow(dpy, canvas->win, x, y);

    // Accumulate damage for new position
    DAMAGE_RECT(canvas->x, canvas->y, canvas->width, canvas->height);

    // Schedule frame for rendering
    SCHEDULE_FRAME();
}

void itn_geometry_resize(Canvas *canvas, int width, int height) {
    if (!canvas) return;

    Display *dpy = itn_core_get_display();
    if (!dpy || !canvas->win) return;

    // Apply resize with damage tracking
    itn_geometry_apply_resize(canvas, width, height);

    // Need to recreate pixmap for new size
    if (canvas->comp_pixmap) {
        itn_composite_update_canvas_pixmap(canvas);
    }
}

void itn_geometry_move_resize(Canvas *canvas, int x, int y, int width, int height) {
    if (!canvas) return;

    Display *dpy = itn_core_get_display();
    if (!dpy || !is_window_valid(dpy, canvas->win)) return;

    // Accumulate damage for old geometry
    DAMAGE_RECT(canvas->x, canvas->y, canvas->width, canvas->height);

    // Check if size is changing BEFORE updating canvas dimensions
    bool size_changed = (canvas->width != width || canvas->height != height);

    // Update position
    canvas->x = x;
    canvas->y = y;
    XMoveResizeWindow(dpy, canvas->win, x, y, width, height);

    // Apply resize with damage tracking
    itn_geometry_apply_resize(canvas, width, height);

    // Need to recreate pixmap for new size
    if (canvas->comp_pixmap && size_changed) {
        itn_composite_update_canvas_pixmap(canvas);
    }
}

void itn_geometry_raise(Canvas *canvas) {
    if (!canvas) return;

    Display *dpy = itn_core_get_display();
    if (!dpy || !is_window_valid(dpy, canvas->win)) return;

    // Use X11 stacking for now (compositor will track through events)
    XRaiseWindow(dpy, canvas->win);

    // Stacking change affects entire screen
    DAMAGE_RECT(0, 0, itn_core_get_screen_width(), itn_core_get_screen_height());
    SCHEDULE_FRAME();
}

void itn_geometry_lower(Canvas *canvas) {
    if (!canvas) return;

    Display *dpy = itn_core_get_display();
    if (!dpy) return;

    // Lower window above desktop (not under it)
    Canvas *desktop = itn_canvas_get_desktop();
    if (!desktop) {
        XLowerWindow(dpy, canvas->win);
    } else {
        // Place directly above the desktop (bottom-most among windows)
        XWindowChanges ch;
        ch.sibling = desktop->win;
        ch.stack_mode = Above;
        XConfigureWindow(dpy, canvas->win, CWSibling | CWStackMode, &ch);
        XSync(dpy, False);
    }

    // Stacking change affects entire screen
    DAMAGE_RECT(0, 0, itn_core_get_screen_width(), itn_core_get_screen_height());
    SCHEDULE_FRAME();
}

void itn_geometry_restack(void) {
    // Mark entire screen as damaged when stacking changes
    DAMAGE_RECT(0, 0, itn_core_get_screen_width(), itn_core_get_screen_height());
    SCHEDULE_FRAME();
}

// Apply resize and handle all necessary updates
void itn_geometry_apply_resize(Canvas *c, int nw, int nh) {
    if (!c) return;
    if (c->width == nw && c->height == nh) return;

    // Damage old area
    DAMAGE_RECT(c->x, c->y, c->width, c->height);

    c->width = nw;
    c->height = nh;

    // Skip expensive buffer recreation during interactive resize
    if (!c->resizing_interactive) {
        render_recreate_canvas_surfaces(c);
    }

    if (c->client_win != None) {
        // Calculate content area (excluding borders)
        int client_width, client_height;
        if (c->fullscreen) {
            client_width = max(1, c->width);
            client_height = max(1, c->height);
        } else {
            int right_border = (c->client_win == None ? BORDER_WIDTH_RIGHT : BORDER_WIDTH_RIGHT_CLIENT);
            client_width = max(1, c->width - BORDER_WIDTH_LEFT - right_border);
            client_height = max(1, c->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM);
        }

        // MUST also set position to ensure client stays within borders!
        Display *dpy = itn_core_get_display();
        XWindowChanges ch = {
            .x = BORDER_WIDTH_LEFT,
            .y = BORDER_HEIGHT_TOP,
            .width = client_width,
            .height = client_height
        };
        XConfigureWindow(dpy, c->client_win, CWX | CWY | CWWidth | CWHeight, &ch);
    } else if (c->type == WINDOW) {
        compute_max_scroll(c);
    }

    // Damage new area and schedule frame
    DAMAGE_RECT(c->x, c->y, c->width, c->height);
    SCHEDULE_FRAME();
}

// ============================================================================
// Scroll Management
// ============================================================================

void compute_max_scroll(Canvas *c) {
    if (!c) return;

    // Calculate content area inside frame
    int content_width, content_height;
    itn_decorations_get_content_area(c, NULL, NULL, &content_width, &content_height);

    // Compute maximum scroll values
    c->max_scroll_x = max(0, c->content_width - content_width);
    c->max_scroll_y = max(0, c->content_height - content_height);

    // Clamp current scroll to valid range
    c->scroll_x = min(c->scroll_x, c->max_scroll_x);
    c->scroll_y = min(c->scroll_y, c->max_scroll_y);
}

// Update scroll position during scrollbar drag
// This function is called from itn_events.c during scrollbar knob dragging
void update_scroll_from_mouse_drag(Canvas *canvas, bool is_vertical, int initial_scroll, int drag_start_pos, int current_mouse_pos) {
    if (!canvas) return;

    int mouse_movement = current_mouse_pos - drag_start_pos;

    // Get the track area dimensions (excluding arrow buttons)
    int track_width, track_height;

    // Track margins and reserved space for arrow buttons
    #define TRACK_MARGIN 18  // Space for arrow buttons
    #define TRACK_RESERVED 36  // Total reserved for both arrows

    if (is_vertical) {
        // Position would be: x = canvas->width - BORDER_WIDTH_RIGHT
        //                    y = BORDER_HEIGHT_TOP + TRACK_MARGIN
        track_width = BORDER_WIDTH_RIGHT;
        track_height = (canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM) - TRACK_RESERVED - TRACK_MARGIN;
    } else {
        // Position would be: x = BORDER_WIDTH_LEFT + TRACK_MARGIN
        //                    y = canvas->height - BORDER_HEIGHT_BOTTOM
        track_width = (canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT) - TRACK_RESERVED - TRACK_MARGIN;
        track_height = BORDER_HEIGHT_BOTTOM;
    }

    int track_length = is_vertical ? track_height : track_width;
    int content_length = is_vertical ? canvas->content_height : canvas->content_width;
    int viewport_length = is_vertical ?
        (canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM) :
        (canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT);
    int max_scroll = is_vertical ? canvas->max_scroll_y : canvas->max_scroll_x;

    // Calculate knob size based on viewport/content ratio
    int knob_length;
    if (content_length <= viewport_length) {
        knob_length = track_length;  // Full track when no scrolling needed
    } else {
        float ratio = (float)viewport_length / (float)content_length;
        knob_length = max(20, (int)(track_length * ratio));  // Min 20px for usability
    }

    int available_track_space = max(1, track_length - knob_length);

    // Calculate initial knob position as ratio of track
    float initial_knob_ratio = (max_scroll > 0) ? ((float)initial_scroll / (float)max_scroll) : 0.0f;
    float initial_knob_pos = initial_knob_ratio * available_track_space;

    // Add mouse movement and clamp to track bounds
    float new_knob_pos = fmax(0.0f, fmin((float)available_track_space, initial_knob_pos + (float)mouse_movement));

    // Convert back to scroll value
    int new_scroll = (max_scroll > 0) ? (int)roundf((new_knob_pos / (float)available_track_space) * (float)max_scroll) : 0;

    // Update the appropriate scroll value
    if (is_vertical) {
        canvas->scroll_y = new_scroll;
    } else {
        canvas->scroll_x = new_scroll;
    }

    // Trigger redraw
    DAMAGE_CANVAS(canvas);
    SCHEDULE_FRAME();
}

// ============================================================================
// Fullscreen Management
// ============================================================================

static bool fullscreen_active = false;

// Set EWMH fullscreen state on client window
static void set_net_wm_state_fullscreen(Window client, bool on) {
    Display *dpy = itn_core_get_display();
    if (!dpy || client == None) return;

    Atom wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom fullscreen = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);

    if (on) {
        XChangeProperty(dpy, client, wm_state, XA_ATOM, 32, PropModeReplace,
                       (unsigned char *)&fullscreen, 1);
    } else {
        XDeleteProperty(dpy, client, wm_state);
    }
}

// Enter fullscreen mode
void intuition_enter_fullscreen(Canvas *c) {
    if (!c || c->type != WINDOW || c->fullscreen) return;

    Display *dpy = itn_core_get_display();
    if (!dpy) return;

    // Save current geometry for restoration
    c->saved_x = c->x;
    c->saved_y = c->y;
    c->saved_w = c->width;
    c->saved_h = c->height;

    // Set fullscreen state
    c->fullscreen = true;
    fullscreen_active = true;

    // Get screen dimensions
    int sw = DisplayWidth(dpy, DefaultScreen(dpy));
    int sh = DisplayHeight(dpy, DefaultScreen(dpy));

    // Move and resize to cover entire screen
    itn_geometry_move_resize(c, 0, 0, sw, sh);

    // Handle client window - position at 0,0 in fullscreen
    if (c->client_win != None) {
        XMoveWindow(dpy, c->client_win, 0, 0);
        set_net_wm_state_fullscreen(c->client_win, true);
    }

    // Hide menubar while fullscreen is active
    extern void menubar_apply_fullscreen(bool fullscreen);
    menubar_apply_fullscreen(true);

    // Force immediate update
    DAMAGE_RECT(0, 0, sw, sh);
    SCHEDULE_FRAME();
    XSync(dpy, False);
}

// Exit fullscreen mode
void intuition_exit_fullscreen(Canvas *c) {
    if (!c || c->type != WINDOW || !c->fullscreen) return;

    Display *dpy = itn_core_get_display();
    if (!dpy) return;

    // Clear fullscreen state
    c->fullscreen = false;

    // Restore client window position and clear EWMH state
    if (c->client_win != None) {
        XMoveWindow(dpy, c->client_win, BORDER_WIDTH_LEFT, BORDER_HEIGHT_TOP);
        set_net_wm_state_fullscreen(c->client_win, false);
    }

    // Restore saved geometry
    int rx = c->saved_x;
    int ry = c->saved_y;
    int rw = c->saved_w;
    int rh = c->saved_h;

    // Ensure valid size
    if (rw <= 0 || rh <= 0) {
        rw = 800;
        rh = 600;
    }

    // Restore window position and size, ensuring it's below menubar
    itn_geometry_move_resize(c, max(0, rx), max(MENUBAR_HEIGHT, ry), rw, rh);

    // Check if any other windows are still fullscreen
    fullscreen_active = false;
    Canvas **canvases = get_canvas_array();
    int count = get_canvas_count();
    for (int i = 0; i < count; i++) {
        if (canvases[i] && canvases[i]->fullscreen) {
            fullscreen_active = true;
            break;
        }
    }

    // Show menubar if no fullscreen windows remain
    if (!fullscreen_active) {
        extern void menubar_apply_fullscreen(bool fullscreen);
        menubar_apply_fullscreen(false);
    }

    // Force redraw
    DAMAGE_CANVAS(c);
    SCHEDULE_FRAME();
}