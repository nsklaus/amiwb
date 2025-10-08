// File: evt_window.c
// Window event handling for AmiWB event system
// Routes window lifecycle events (map, unmap, destroy, expose) to intuition

#include "evt_internal.h"
#include <X11/Xlib.h>

// ============================================================================
// Coordinate Translation Helper (Internal API)
// ============================================================================

// Walk up ancestors to find a Canvas window and translate coordinates
Canvas *resolve_event_canvas(Window w, int in_x, int in_y, int *out_x, int *out_y) {
    Display *dpy = itn_core_get_display();
    Window root = DefaultRootWindow(dpy);
    Window cur = w;
    // rx, ry removed - were unused
    // Resolving event canvas
    while (cur && cur != root) {
        Canvas *c = itn_canvas_find_by_window(cur);
        if (c) {
            // Translate original event coords to this canvas window coords
            int tx=0, ty=0; Window dummy;
            safe_translate_coordinates(dpy, w, c->win, in_x, in_y, &tx, &ty, &dummy);
            if (out_x) *out_x = tx;
            if (out_y) *out_y = ty;
            // Canvas resolved
            return c;
        }
        // Ensure 'cur' is still a valid window before walking up the tree
        XWindowAttributes wa;
        if (!safe_get_window_attributes(dpy, cur, &wa)) break;
        Window root_ret, parent_ret, *children = NULL; unsigned int n = 0;
        if (!XQueryTree(dpy, cur, &root_ret, &parent_ret, &children, &n)) break;
        if (children) XFree(children);
        if (parent_ret == 0 || parent_ret == cur) break;
        cur = parent_ret;
    }
    return NULL;
}

// ============================================================================
// Window Event Dispatchers (Public API)
// ============================================================================

// Dispatch window expose - forward to intuition so frames and canvases redraw
void handle_expose(XExposeEvent *event) {
    // Canvas lookup removed - was unused
    intuition_handle_expose(event);
}

// A client asks to be mapped - give it an AmiWB frame
void handle_map_request(XMapRequestEvent *event) {
    Canvas *canvas = itn_canvas_find_by_window(event->window);
    if (!canvas) {
        intuition_handle_map_request(event);
    }
}

// Handle window unmapping - special handling for transient windows (GTK dialogs)
void handle_unmap_notify(XUnmapEvent *event) {
    // First check if it's an override-redirect window being unmapped
    if (itn_composite_remove_override(event->window)) {
        // Was an override window - schedule frame to remove it from display
        itn_render_schedule_frame();
        return;
    }

    Canvas *canvas = itn_canvas_find_by_client(event->window);
    if (canvas) {

        // For transient windows, track unmaps
        // GTK file picker dialogs unmap themselves multiple times when finishing
        if (canvas->is_transient) {
            canvas->consecutive_unmaps++;

            // Check if this is a GTK dialog and handle after 3 unmaps
            if (canvas->consecutive_unmaps >= 3 && !canvas->cleanup_scheduled) {
                canvas->cleanup_scheduled = true;

                // Save parent before hiding
                Window parent_win = canvas->transient_for;

                // Just hide our frame, don't destroy anything
                // GTK dialogs will send DestroyNotify when they're really done
                safe_unmap_window(itn_core_get_display(), canvas->win);

                // Restore focus to parent window when dialog is hidden
                if (parent_win != None) {
                    Canvas *parent_canvas = itn_canvas_find_by_client(parent_win);
                    if (parent_canvas) {
                        itn_focus_set_active(parent_canvas);
                        // Safe focus with validation and BadMatch error handling
                        safe_set_input_focus(itn_core_get_display(), parent_win, RevertToParent, CurrentTime);
                    }
                }

                // Reset the counter so it can be shown again
                canvas->consecutive_unmaps = 0;
                canvas->cleanup_scheduled = false;
                return;
            }
        }
    }
}

// Handle window destruction - cleanup Canvas and frame
void handle_destroy_notify(XDestroyWindowEvent *event) {
    Canvas *canvas = itn_canvas_find_by_window(event->window);
    if (!canvas) {
        canvas = itn_canvas_find_by_client(event->window);
    }

    if (canvas) {
        intuition_handle_destroy_notify(event);
    }
}
