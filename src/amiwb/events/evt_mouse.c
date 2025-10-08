// File: evt_mouse.c
// Mouse event handling for AmiWB event system
// Handles button press/release and motion events, routing them to subsystems
// (intuition, workbench, menus) with proper coordinate translation.

#include "evt_internal.h"
#include "../menus/menu_public.h"
#include "../workbench/wb_public.h"
#include "../dialogs/dialog_public.h"
#include <X11/Xlib.h>

// ============================================================================
// Module State (Private)
// ============================================================================

// Track the window that owns the current button interaction so motion and
// release are routed consistently, even if X delivers them elsewhere.
static Window g_press_target = 0;

// ============================================================================
// Getters/Setters (Internal API - for evt_internal.h)
// ============================================================================

// Get current press target
Window evt_mouse_get_press_target(void) {
    return g_press_target;
}

// Set press target
void evt_mouse_set_press_target(Window w) {
    g_press_target = w;
}

// Clear press target if it matches the given window
void evt_mouse_clear_press_target_if_matches(Window win) {
    if (g_press_target == win) {
        g_press_target = 0;
    }
}

// ============================================================================
// Helper Functions (Private)
// ============================================================================

// Helper function to create event copy with translated coordinates
XButtonEvent create_translated_button_event(XButtonEvent *original, Window target_window, int new_x, int new_y) {
    XButtonEvent ev = *original;
    ev.window = target_window;
    ev.x = new_x;
    ev.y = new_y;
    return ev;
}

// Helper function to create motion event copy with translated coordinates
XMotionEvent create_translated_motion_event(XMotionEvent *original, Window target_window, int new_x, int new_y) {
    XMotionEvent ev = *original;
    ev.window = target_window;
    ev.x = new_x;
    ev.y = new_y;
    return ev;
}

// Helper function to handle menubar vs regular menu routing
void handle_menu_canvas_press(Canvas *canvas, XButtonEvent *event, int cx, int cy) {
    if (canvas == get_menubar()) {
        XButtonEvent ev = create_translated_button_event(event, canvas->win, cx, cy);
        menu_handle_menubar_press(&ev);
    } else {
        XButtonEvent ev = create_translated_button_event(event, canvas->win, cx, cy);
        menu_handle_button_press(&ev);
    }
}

// Helper function to handle menubar vs regular menu motion
void handle_menu_canvas_motion(Canvas *canvas, XMotionEvent *event, int cx, int cy) {
    if (canvas == get_menubar()) {
        menu_handle_menubar_motion(event);
    } else {
        menu_handle_motion_notify(event);
    }
}

// ============================================================================
// Public Dispatchers (Public API - for evt_public.h)
// ============================================================================

// Dispatch mouse button press
void handle_button_press(XButtonEvent *event) {
    // Check if window list menu is open and close it on any click outside
    Menu *active = get_active_menu();
    if (active && active->parent_index == -1) {  // parent_index == -1 means window list
        // Check if click is outside the window list menu
        Canvas *menu_canvas = active->canvas;
        Canvas *menubar = get_menubar();
        // Don't close if clicking on menubar (button area will be handled by menu_handle_menubar_press)
        if (menu_canvas && event->window != menu_canvas->win && event->window != menubar->win) {
            // Click is outside the window list and not on menubar - close it
            close_window_list_if_open();
        }
    }

    // Check if this is a click on an InputField dropdown (not a Canvas)
    if (dialogs_handle_button_press(event)) {
        return;  // Event was handled by dialog's InputField
    }

    int cx = event->x, cy = event->y; // may be rewritten
    Canvas *canvas = itn_canvas_find_by_window(event->window);
    // Handle button press
    // If the press is on a managed client, activate its frame, replay pointer, and translate
    if (!canvas) {
        Canvas *owner = itn_canvas_find_by_client(event->window);
        if (owner) {
            itn_focus_set_active(owner);
            // We grabbed buttons on the client in frame_client_window();
            // allow the click to proceed to the client after focusing
            // XAllowEvents: Controls what happens to grabbed events
            // ReplayPointer means "pretend the grab never happened" -
            // the click goes through to the client window normally
            // This lets us intercept clicks for focus, then pass them along
            XAllowEvents(itn_core_get_display(), ReplayPointer, event->time);
            // translate coords from client to frame canvas
            Window dummy; safe_translate_coordinates(itn_core_get_display(), event->window, owner->win, event->x, event->y, &cx, &cy, &dummy);
            canvas = owner;
            // Route to frame
        }
    }
    if (!canvas) canvas = resolve_event_canvas(event->window, event->x, event->y, &cx, &cy);
    if (!canvas) { return; }
    // Press resolved

    // If the desktop got the press but a window is actually under the pointer,
    // reroute the event to the topmost WINDOW canvas at the pointer's root coords.
    if (canvas->type == DESKTOP) {
        Display *dpy = itn_core_get_display();
        Window root = DefaultRootWindow(dpy);
        // Query stacking order
        Window root_ret, parent_ret, *children = NULL; unsigned int n = 0;
        if (XQueryTree(dpy, root, &root_ret, &parent_ret, &children, &n)) {
            // Children array is bottom-to-top; scan from topmost down.
            for (int i = (int)n - 1; i >= 0; --i) {
                Canvas *c = itn_canvas_find_by_window(children[i]);
                if (!c || (c->type != WINDOW && c->type != DIALOG)) continue;
                // Validate the X window is still valid and viewable
                XWindowAttributes a;
                if (!safe_get_window_attributes(dpy, c->win, &a) || a.map_state != IsViewable) continue;
                // Hit test in root coordinates against the frame rect
                int rx = event->x_root, ry = event->y_root;
                if (rx >= c->x && rx < c->x + c->width &&
                    ry >= c->y && ry < c->y + c->height) {
                    // Translate root coords to frame coords
                    Window dummy; int tx=0, ty=0;
                    safe_translate_coordinates(dpy, root, c->win, rx, ry, &tx, &ty, &dummy);
                    // Reroute to frame
                    itn_focus_set_active(c);
                    XButtonEvent ev = *event; ev.window = c->win; ev.x = tx; ev.y = ty;
                    intuition_handle_button_press(&ev);
                    workbench_handle_button_press(&ev);
                    g_press_target = c->win; // lock routing until release
                    if (children) XFree(children);
                    return;
                }
            }
            if (children) XFree(children);
        }
    }

    if (canvas->type == MENU) {
        // Menus are handled exclusively by menu subsystem
        handle_menu_canvas_press(canvas, event, cx, cy);
        g_press_target = canvas->win; // route motion/release to this menu until release
    } else if (canvas->type == WINDOW || canvas->type == DIALOG) {
        // Activate and forward to window/dialog frame
        itn_focus_set_active(canvas);
        XButtonEvent ev = create_translated_button_event(event, canvas->win, cx, cy);

        // For dialogs, try dialog-specific handling first
        bool dialog_consumed = false;
        if (canvas->type == DIALOG) {
            // Check if it's an iconinfo dialog first
            if (is_iconinfo_canvas(canvas)) {
                dialog_consumed = iconinfo_handle_button_press(&ev);
            } else {
                dialog_consumed = dialogs_handle_button_press(&ev);
            }
        }

        // If dialog didn't consume the event, handle as normal window
        if (!dialog_consumed) {
            itn_events_reset_press_consumed();  // Reset flag before processing
            intuition_handle_button_press(&ev);
            if (!itn_events_last_press_consumed()) {
                workbench_handle_button_press(&ev);
            }
        }
        g_press_target = canvas->win;
    } else {
        // default: forward to specific canvas
        XButtonEvent ev = create_translated_button_event(event, canvas->win, cx, cy);
        workbench_handle_button_press(&ev);
        intuition_handle_button_press(&ev);
    }
    // No grabs in use; nothing to release
}

// Dispatch mouse button release
void handle_button_release(XButtonEvent *event) {
    // If we have a press target, translate release to it
    if (g_press_target) {
        Display *dpy = itn_core_get_display();

        // First check if g_press_target window still exists
        Canvas *target_canvas = itn_canvas_find_by_window(g_press_target);
        if (!target_canvas) {
            g_press_target = 0;
            return;
        }

        // Ensure both source and target windows still exist before translating
        XWindowAttributes src_attrs, dst_attrs;
        bool src_ok = safe_get_window_attributes(dpy, event->window, &src_attrs);
        bool dst_ok = safe_get_window_attributes(dpy, g_press_target, &dst_attrs);
        if (!src_ok || !dst_ok) { g_press_target = 0; return; }
        Window dummy; int tx=0, ty=0;

        // Set an error handler to catch X errors
        XSync(dpy, False);  // Clear any pending errors
        safe_translate_coordinates(dpy, event->window, g_press_target, event->x, event->y, &tx, &ty, &dummy);
        XSync(dpy, False);  // Force error to occur now if any

        XButtonEvent ev = *event; ev.window = g_press_target; ev.x = tx; ev.y = ty;
        Canvas *tc = itn_canvas_find_by_window(g_press_target);
        if (tc && tc->type == MENU) {
            menu_handle_button_release(&ev);
        } else {
            // For dialogs, try dialog-specific handling first
            bool dialog_consumed = false;
            if (tc && tc->type == DIALOG) {
                // Check if it's an iconinfo dialog first
                if (is_iconinfo_canvas(tc)) {
                    dialog_consumed = iconinfo_handle_button_release(&ev);
                } else {
                    dialog_consumed = dialogs_handle_button_release(&ev);
                }
            }

            // If dialog didn't consume the event, handle as normal window
            if (!dialog_consumed) {
                workbench_handle_button_release(&ev);
                intuition_handle_button_release(&ev);
            }
        }
        g_press_target = 0; // clear routing lock
        return;
    }
    int cx = event->x, cy = event->y; // fallback path
    Canvas *canvas = itn_canvas_find_by_window(event->window);
    if (!canvas) canvas = resolve_event_canvas(event->window, event->x, event->y, &cx, &cy);
    if (!canvas) { return; }
    XButtonEvent ev = create_translated_button_event(event, canvas->win, cx, cy);

    // For dialogs, try dialog-specific handling first
    bool dialog_consumed = false;
    if (canvas->type == DIALOG) {
        // Check if it's an iconinfo dialog first
        if (is_iconinfo_canvas(canvas)) {
            dialog_consumed = iconinfo_handle_button_release(&ev);
        } else {
            dialog_consumed = dialogs_handle_button_release(&ev);
        }
    }

    // If dialog didn't consume the event, handle as normal window
    if (!dialog_consumed) {
        workbench_handle_button_release(&ev);
        intuition_handle_button_release(&ev);
    }
}

// Dispatch mouse motion
void handle_motion_notify(XMotionEvent *event) {
    // If we're in an active interaction, forward motion to the press target
    if (g_press_target) {
        Display *dpy = itn_core_get_display();
        Window dummy; int tx=0, ty=0;
        // Translate from source to target using root coords for robustness
        int rx = event->x_root, ry = event->y_root;
        safe_translate_coordinates(dpy, DefaultRootWindow(dpy), g_press_target, rx, ry, &tx, &ty, &dummy);
        XMotionEvent ev = *event; ev.window = g_press_target; ev.x = tx; ev.y = ty;
        Canvas *tc = itn_canvas_find_by_window(g_press_target);
        if (tc && tc->type == MENU) {
            handle_menu_canvas_motion(tc, &ev, tx, ty);
        } else {
            // While scrolling a scrollbar, do not send motion to icons
            if (!(tc && tc->type == WINDOW && itn_events_is_scrolling_active())) {
                workbench_handle_motion_notify(&ev);
            }
            if (tc && (tc->type == WINDOW || tc->type == DIALOG)) {
                // Try dialog handler first for InputField selection
                if (tc->type == DIALOG) {
                    if (dialogs_handle_motion(&ev)) {
                        return;
                    }
                }
                intuition_handle_motion_notify(&ev);
            }
        }
        return;
    }

    int cx = event->x, cy = event->y;
    Canvas *canvas = itn_canvas_find_by_window(event->window);
    // Suppress motion logging to avoid log spam
    if (!canvas) canvas = resolve_event_canvas(event->window, event->x, event->y, &cx, &cy);
    if (!canvas) {
        // No canvas for this motion event
        return;
    }

    if (canvas->type == MENU || canvas == get_menubar()) {
        handle_menu_canvas_motion(canvas, event, cx, cy);
    } else {
        XMotionEvent ev = create_translated_motion_event(event, canvas->win, cx, cy);
        if (!(canvas->type == WINDOW && itn_events_is_scrolling_active())) {
            workbench_handle_motion_notify(&ev);
        }
        if (canvas->type == WINDOW || canvas->type == DIALOG) {
            intuition_handle_motion_notify(&ev);
        }
    }
}

// ============================================================================
// Public Cleanup (Public API - for evt_public.h)
// ============================================================================

// Clear the press target when a window is being destroyed
void clear_press_target_if_matches(Window win) {
    evt_mouse_clear_press_target_if_matches(win);
}
