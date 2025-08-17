// Event dispatch and routing between intuition (window frames),
// workbench (icons), and menus. Keeps interactions coherent
// by locking routing to the initial press target.
#include "menus.h"
#include "events.h"
#include "compositor.h"
#include "intuition.h"
#include "workbench.h"
#include "dialogs.h"
#include "config.h"
#include <X11/extensions/Xrandr.h>

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <stdio.h> // For fprintf
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h> // getenv
#include <time.h>

// External functions to trigger actions from menus.c
extern void trigger_execute_action(void);
extern void trigger_rename_action(void);
extern void trigger_cleanup_action(void);
extern void trigger_close_action(void);
extern void trigger_parent_action(void);
extern void trigger_open_action(void);
extern void trigger_copy_action(void);
extern void trigger_delete_action(void);
extern void trigger_select_contents_action(void);

// Track the window that owns the current button interaction so motion and
// release are routed consistently, even if X delivers them elsewhere.
static Window g_press_target = 0;

// Clear the press target when a window is being destroyed
void clear_press_target_if_matches(Window win) {
    if (g_press_target == win) {
        g_press_target = 0;
    }
}

bool running = true;

// Gated debug helper (wire to env later if needed)
static inline int wb_dbg(void) { return 0; }

// Helper function to create event copy with translated coordinates
static XButtonEvent create_translated_button_event(XButtonEvent *original, Window target_window, int new_x, int new_y) {
    XButtonEvent ev = *original;
    ev.window = target_window;
    ev.x = new_x;
    ev.y = new_y;
    return ev;
}

// Helper function to create motion event copy with translated coordinates
static XMotionEvent create_translated_motion_event(XMotionEvent *original, Window target_window, int new_x, int new_y) {
    XMotionEvent ev = *original;
    ev.window = target_window;
    ev.x = new_x;
    ev.y = new_y;
    return ev;
}

// Helper function to handle menubar vs regular menu routing
static void handle_menu_canvas_press(Canvas *canvas, XButtonEvent *event, int cx, int cy) {
    if (canvas == get_menubar()) {
        XButtonEvent ev = create_translated_button_event(event, canvas->win, cx, cy);
        menu_handle_menubar_press(&ev);
    } else {
        XButtonEvent ev = create_translated_button_event(event, canvas->win, cx, cy);
        menu_handle_button_press(&ev);
    }
}

// Helper function to handle menubar vs regular menu motion
static void handle_menu_canvas_motion(Canvas *canvas, XMotionEvent *event, int cx, int cy) {
    if (canvas == get_menubar()) {
        menu_handle_menubar_motion(event);
    } else {
        menu_handle_motion_notify(event);
    }
}

// Helper function for debug output (controlled by wb_dbg)
static void debug_event_info(const char *event_type, Window window, int x, int y, unsigned int state) {
    if (wb_dbg()) {
        fprintf(stderr, "[WB] %s win=0x%lx x,y=(%d,%d) state=0x%x\n", event_type, window, x, y, state);
    }
}

// Helper function for debug canvas resolution info
static void debug_canvas_resolved(Canvas *canvas, int tx, int ty, const char *context) {
    if (wb_dbg()) {
        fprintf(stderr, "[WB]  %s resolved: canvas=0x%lx type=%d tx,ty=(%d,%d)\n", context, canvas->win, canvas->type, tx, ty);
    }
}

// Initialize event handling
// Initialize event subsystem (reserved for future setup).
static char g_log_path[1024] = {0};
void init_events(void) {
    #if LOGGING_ENABLED
    // Expand LOG_FILE_PATH (support leading $HOME)
    const char *cfg = LOG_FILE_PATH;
    if (cfg && strncmp(cfg, "$HOME/", 6) == 0) {
        const char *home = getenv("HOME");
        if (home) snprintf(g_log_path, sizeof(g_log_path), "%s/%s", home, cfg + 6);
        else snprintf(g_log_path, sizeof(g_log_path), "%s", cfg);
    } else if (cfg) {
        snprintf(g_log_path, sizeof(g_log_path), "%s", cfg);
    }
    #endif
}

// Main event loop
// Central dispatcher that forwards X events to subsystems. We translate
// coordinates and reroute presses so each canvas receives coherent input.
void handle_events(void) {
    Display *dpy = get_display();
    if (!dpy) { return; }

    // entering event loop
    XEvent event;

    // X connection file descriptor for select
    int fd = ConnectionNumber(dpy);  

    // Log cap management
    unsigned int iter = 0;
    time_t last_time_check = 0;
    
    while (running) {
        // Check for events with timeout for periodic updates
        if (XPending(dpy)) {
            // Deliver events as-is. If coalescing is needed, keep the last motion
            // per target window rather than discarding all motions globally.
            XNextEvent(dpy, &event);
        } else {
            // No events pending, check if we should update time
            time_t now = time(NULL);
            if (now - last_time_check >= 1) {  // Check every second
                last_time_check = now;
                update_menubar_time();  // Will only redraw if minute changed
            }
            
            // Very brief sleep to avoid busy-waiting but remain responsive
            usleep(1000);  // 1ms - much more responsive
            continue;
        }

        // Periodically enforce log cap (optional)
        #if LOGGING_ENABLED && LOG_CAP_ENABLED
        if ((++iter % 1000u) == 0u && g_log_path[0]) {
            struct stat st;
            if (stat(g_log_path, &st) == 0 && st.st_size > (off_t)LOG_CAP_BYTES) {
                FILE *lf = fopen(g_log_path, "w"); // truncate
                if (lf) {
                    setvbuf(lf, NULL, _IOLBF, 0);
                    dup2(fileno(lf), fileno(stdout));
                    dup2(fileno(lf), fileno(stderr));
                    fprintf(stderr, "[amiwb] log truncated (cap=%ld bytes)\n", (long)LOG_CAP_BYTES);
                }
            }
        }
        #endif
        
        
        // Let the compositor see every event so it can maintain damage/topology
        compositor_handle_event(dpy, &event);

        if (event.type == randr_event_base + RRScreenChangeNotify) {
            intuition_handle_rr_screen_change(
                (XRRScreenChangeNotifyEvent *)&event);
            continue;
        }

        switch (event.type) {
            case ButtonPress:
                handle_button_press(&event.xbutton);
                break;
            case ButtonRelease:
                handle_button_release(&event.xbutton);
                break;
            case EnterNotify: {
                // Do not auto-activate on pointer enter to avoid focus ping-pong
                break;
            }
            case FocusIn: {
                // Do not auto-activate on FocusIn; activation is explicit via click or map
                break;
            }
            case KeyPress:
                // Check if dialog system handles the key first
                if (!dialogs_handle_key_press(&event.xkey)) {
                    handle_key_press(&event.xkey);
                }
                break;
            case Expose:
                handle_expose(&event.xexpose);
                break;
            case MapRequest:
                handle_map_request(&event.xmaprequest);
                break;
            case MapNotify: {
                // Catch unmanaged toplevels when SubstructureRedirect was not granted
                XMapEvent *map_event = &event.xmap;
                Canvas *canvas = find_canvas_by_client(map_event->window);
                if (canvas) {
                    // Clear consecutive unmap counter - window is healthy again
                    if (canvas->is_transient && canvas->consecutive_unmaps > 0) {
                        canvas->consecutive_unmaps = 0;
                    }
                }
                intuition_handle_map_notify(map_event);
                break;
            }
            case UnmapNotify:
                handle_unmap_notify(&event.xunmap);
                break;
            case ConfigureRequest:
                handle_configure_request(&event.xconfigurerequest);
                break;
            case MotionNotify:
                handle_motion_notify(&event.xmotion);
                break;
            case PropertyNotify:
                handle_property_notify(&event.xproperty);
                break;
            case ConfigureNotify:
                handle_configure_notify(&event.xconfigure);
                break;
            case DestroyNotify:
                handle_destroy_notify(&event.xdestroywindow);
                break;
            case ClientMessage:
                intuition_handle_client_message(&event.xclient);
                break;
            default:
                break;
        }
        
        // Flush any pending compositor repaints after processing events
        // This batches multiple position updates from rapid motion events
        compositor_flush_pending(dpy);
    }
}

// Ask the main loop to exit cleanly.
void quit_event_loop(void) {
    running = false;
}

// Walk up ancestors to find a Canvas window and translate coordinates
static Canvas *resolve_event_canvas(Window w, int in_x, int in_y, int *out_x, int *out_y) {
    Display *dpy = get_display();
    Window root = DefaultRootWindow(dpy);
    Window cur = w;
    int rx = in_x, ry = in_y;
    if (wb_dbg()) fprintf(stderr, "[WB] resolve_event_canvas: w=0x%lx in=(%d,%d)\n", w, in_x, in_y);
    while (cur && cur != root) {
        Canvas *c = find_canvas(cur);
        if (c) {
            // Translate original event coords to this canvas window coords
            int tx=0, ty=0; Window dummy;
            XTranslateCoordinates(dpy, w, c->win, in_x, in_y, &tx, &ty, &dummy);
            if (out_x) *out_x = tx; if (out_y) *out_y = ty;
            if (wb_dbg()) fprintf(stderr, "[WB]  -> resolved to canvas win=0x%lx type=%d tx,ty=(%d,%d)\n", c->win, c->type, tx, ty);
            return c;
        }
        // Ensure 'cur' is still a valid window before walking up the tree
        XWindowAttributes wa;
        if (!XGetWindowAttributes(dpy, cur, &wa)) break;
        Window root_ret, parent_ret, *children = NULL; unsigned int n = 0;
        if (!XQueryTree(dpy, cur, &root_ret, &parent_ret, &children, &n)) break;
        if (children) XFree(children);
        if (parent_ret == 0 || parent_ret == cur) break;
        cur = parent_ret;
    }
    return NULL;
}

// Dispatch mouse button press
void handle_button_press(XButtonEvent *event) {
    int cx = event->x, cy = event->y; // may be rewritten
    Canvas *canvas = find_canvas(event->window);
    if (wb_dbg()) fprintf(stderr, "[WB] ButtonPress win=0x%lx x,y=(%d,%d) state=0x%x\n", event->window, event->x, event->y, event->state);
    // If the press is on a managed client, activate its frame, replay pointer, and translate
    if (!canvas) {
        Canvas *owner = find_canvas_by_client(event->window);
        if (owner) {
            set_active_window(owner);
            // We grabbed buttons on the client in frame_client_window();
            // allow the click to proceed to the client after focusing
            // XAllowEvents: Controls what happens to grabbed events
            // ReplayPointer means "pretend the grab never happened" - 
            // the click goes through to the client window normally
            // This lets us intercept clicks for focus, then pass them along
            XAllowEvents(get_display(), ReplayPointer, event->time);
            // translate coords from client to frame canvas
            Window dummy; XTranslateCoordinates(get_display(), event->window, owner->win, event->x, event->y, &cx, &cy, &dummy);
            canvas = owner;
            if (wb_dbg()) fprintf(stderr, "[WB]  client press routed to frame=0x%lx tx,ty=(%d,%d)\n", canvas->win, cx, cy);
        }
    }
    if (!canvas) canvas = resolve_event_canvas(event->window, event->x, event->y, &cx, &cy);
    if (!canvas) { return; }
    if (wb_dbg()) fprintf(stderr, "[WB]  press resolved: canvas=0x%lx type=%d tx,ty=(%d,%d) was_active=%d\n",
                           canvas->win, canvas->type, cx, cy, canvas->active?1:0);

    // If the desktop got the press but a window is actually under the pointer,
    // reroute the event to the topmost WINDOW canvas at the pointer's root coords.
    if (canvas->type == DESKTOP) {
        Display *dpy = get_display();
        Window root = DefaultRootWindow(dpy);
        // Query stacking order
        Window root_ret, parent_ret, *children = NULL; unsigned int n = 0;
        if (XQueryTree(dpy, root, &root_ret, &parent_ret, &children, &n)) {
            // Children array is bottom-to-top; scan from topmost down.
            for (int i = (int)n - 1; i >= 0; --i) {
                Canvas *c = find_canvas(children[i]);
                if (!c || (c->type != WINDOW && c->type != DIALOG)) continue;
                // Validate the X window is still valid and viewable
                XWindowAttributes a; 
                if (!XGetWindowAttributes(dpy, c->win, &a) || a.map_state != IsViewable) continue;
                // Hit test in root coordinates against the frame rect
                int rx = event->x_root, ry = event->y_root;
                if (rx >= c->x && rx < c->x + c->width &&
                    ry >= c->y && ry < c->y + c->height) {
                    // Translate root coords to frame coords
                    Window dummy; int tx=0, ty=0;
                    XTranslateCoordinates(dpy, root, c->win, rx, ry, &tx, &ty, &dummy);
                    if (wb_dbg()) fprintf(stderr, "[WB]  desktop press rerouted to frame=0x%lx tx,ty=(%d,%d)\n", c->win, tx, ty);
                    set_active_window(c);
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
        set_active_window(canvas);
        XButtonEvent ev = create_translated_button_event(event, canvas->win, cx, cy);
        
        // For dialogs, try dialog-specific handling first
        bool dialog_consumed = false;
        if (canvas->type == DIALOG) {
            dialog_consumed = dialogs_handle_button_press(&ev);
        }
        
        // If dialog didn't consume the event, handle as normal window
        if (!dialog_consumed) {
            intuition_handle_button_press(&ev);
            if (!intuition_last_press_consumed()) {
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
        Display *dpy = get_display();
        
        // First check if g_press_target window still exists
        Canvas *target_canvas = find_canvas(g_press_target);
        if (!target_canvas) {
            g_press_target = 0;
            return;
        }
        
        // Ensure both source and target windows still exist before translating
        XWindowAttributes src_attrs, dst_attrs;
        bool src_ok = XGetWindowAttributes(dpy, event->window, &src_attrs);
        bool dst_ok = XGetWindowAttributes(dpy, g_press_target, &dst_attrs);
        if (!src_ok || !dst_ok) { g_press_target = 0; return; }
        Window dummy; int tx=0, ty=0;
        
        // Set an error handler to catch X errors
        XSync(dpy, False);  // Clear any pending errors
        XTranslateCoordinates(dpy, event->window, g_press_target, event->x, event->y, &tx, &ty, &dummy);
        XSync(dpy, False);  // Force error to occur now if any
        
        XButtonEvent ev = *event; ev.window = g_press_target; ev.x = tx; ev.y = ty;
        Canvas *tc = find_canvas(g_press_target);
        if (tc && tc->type == MENU) {
            menu_handle_button_release(&ev);
        } else {
            // For dialogs, try dialog-specific handling first
            bool dialog_consumed = false;
            if (tc && tc->type == DIALOG) {
                dialog_consumed = dialogs_handle_button_release(&ev);
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
    Canvas *canvas = find_canvas(event->window);
    if (!canvas) canvas = resolve_event_canvas(event->window, event->x, event->y, &cx, &cy);
    if (!canvas) { return; }
    XButtonEvent ev = create_translated_button_event(event, canvas->win, cx, cy);
    
    // For dialogs, try dialog-specific handling first
    bool dialog_consumed = false;
    if (canvas->type == DIALOG) {
        dialog_consumed = dialogs_handle_button_release(&ev);
    }
    
    // If dialog didn't consume the event, handle as normal window
    if (!dialog_consumed) {
        workbench_handle_button_release(&ev);
        intuition_handle_button_release(&ev);
    }
}

// Dispatch key press
void handle_key_press(XKeyEvent *event) {
    /*
    // keypress debug removed
    KeySym keysym = XLookupKeysym(event, 0);
    if (keysym == XK_Escape) {
        running = false;
        return;
    }
    */
    
    // Check for global shortcuts (Super/Windows key + letter)
    KeySym keysym = XLookupKeysym(event, 0);
    if (event->state & Mod4Mask) {  // Super/Windows key is pressed
        if (event->state & ShiftMask) {
            // Super+Shift combinations
            // Super+Shift+Q: Quit AmiWB
            if (keysym == XK_q || keysym == XK_Q) {
                handle_quit_request();
                return;
            }
            // Super+Shift+R: Restart AmiWB
            if (keysym == XK_r || keysym == XK_R) {
                extern void handle_restart_request(void);
                handle_restart_request();
                return;
            }
            // Super+Shift+S: Suspend
            if (keysym == XK_s || keysym == XK_S) {
                handle_suspend_request();
                return;
            }
        } else {
            // Super-only combinations (no Shift)
            // Super+E: Execute command
            if (keysym == XK_e || keysym == XK_E) {
                trigger_execute_action();
                return;
            }
            // Super+R: Rename selected icon
            if (keysym == XK_r || keysym == XK_R) {
                trigger_rename_action();
                return;
            }
            // Super+;: Clean up icons
            if (keysym == XK_semicolon) {
                trigger_cleanup_action();
                return;
            }
            // Super+Q: Close active window
            if (keysym == XK_q || keysym == XK_Q) {
                trigger_close_action();
                return;
            }
            // Super+P: Open parent directory
            if (keysym == XK_p || keysym == XK_P) {
                trigger_parent_action();
                return;
            }
            // Super+O: Open selected icon
            if (keysym == XK_o || keysym == XK_O) {
                trigger_open_action();
                return;
            }
            // Super+C: Copy selected icon
            if (keysym == XK_c || keysym == XK_C) {
                trigger_copy_action();
                return;
            }
            // Super+D: Delete selected icon
            if (keysym == XK_d || keysym == XK_D) {
                trigger_delete_action();
                return;
            }
            // Super+N: New Drawer
            if (keysym == XK_n || keysym == XK_N) {
                trigger_new_drawer_action();
                return;
            }
            // Super+A: Select Contents
            if (keysym == XK_a || keysym == XK_A) {
                trigger_select_contents_action();
                return;
            }
        }
        // Add more global shortcuts here in the future
    }
    
    menu_handle_key_press(event);
}

// Dispatch window expose
// Forward to intuition so frames and canvases redraw.
void handle_expose(XExposeEvent *event) {
    Canvas *canvas = find_canvas(event->window);
    intuition_handle_expose(event);
}

// A client asks to be mapped; give it an AmiWB frame.
void handle_map_request(XMapRequestEvent *event) {
    Canvas *canvas = find_canvas(event->window);
    if (!canvas) {
        intuition_handle_map_request(event);
    }
}

// Client wants to move/resize; let intuition translate to frame ops.
void handle_configure_request(XConfigureRequestEvent *event) {

    Canvas *canvas = find_canvas_by_client(event->window);
    if (canvas) {
        // ConfigureRequest trace removed
        intuition_handle_configure_request(event);
    } else {
        // ConfigureRequest trace removed
        XWindowChanges changes;
        changes.x = event->x;
        changes.y = event->y;
        changes.width = event->width;
        changes.height = event->height;
        XConfigureWindow(get_display(), event->window, 
            event->value_mask, &changes);
    }
}

// Dispatch property notify
// WM hints, protocols, and netwm properties changes.
void handle_property_notify(XPropertyEvent *event) {
    intuition_handle_property_notify(event);
}

// Dispatch mouse motion
void handle_motion_notify(XMotionEvent *event) {
    // If we're in an active interaction, forward motion to the press target
    if (g_press_target) {
        Display *dpy = get_display();
        Window dummy; int tx=0, ty=0;
        // Translate from source to target using root coords for robustness
        int rx = event->x_root, ry = event->y_root;
        XTranslateCoordinates(dpy, DefaultRootWindow(dpy), g_press_target, rx, ry, &tx, &ty, &dummy);
        XMotionEvent ev = *event; ev.window = g_press_target; ev.x = tx; ev.y = ty;
        Canvas *tc = find_canvas(g_press_target);
        if (tc && tc->type == MENU) {
            handle_menu_canvas_motion(tc, &ev, tx, ty);
        } else {
            // While scrolling a scrollbar, do not send motion to icons
            if (!(tc && tc->type == WINDOW && intuition_is_scrolling_active())) {
                workbench_handle_motion_notify(&ev);
            }
            if (tc && (tc->type == WINDOW || tc->type == DIALOG)) {
                intuition_handle_motion_notify(&ev);
            }
        }
        return;
    }

    int cx = event->x, cy = event->y;
    Canvas *canvas = find_canvas(event->window);
    // Suppress motion logging to avoid log spam
    if (!canvas) canvas = resolve_event_canvas(event->window, event->x, event->y, &cx, &cy);
    if (!canvas) {
        fprintf(stderr, "No canvas for MotionNotify \
            event on window %lu\n", event->window);
        return;
    }

    if (canvas->type == MENU || canvas == get_menubar()) {
        handle_menu_canvas_motion(canvas, event, cx, cy);
    } else {
        XMotionEvent ev = create_translated_motion_event(event, canvas->win, cx, cy);
        if (!(canvas->type == WINDOW && intuition_is_scrolling_active())) {
            workbench_handle_motion_notify(&ev);
        }
        if (canvas->type == WINDOW || canvas->type == DIALOG) {
            intuition_handle_motion_notify(&ev);
        }
    }
}

// Geometry of a managed frame changed; update caches.
void handle_configure_notify(XConfigureEvent *event) {
    Canvas *canvas = find_canvas(event->window);
    if (canvas && canvas->type == WINDOW) {
        intuition_handle_configure_notify(event);
    }
}

void handle_unmap_notify(XUnmapEvent *event) {
    Canvas *canvas = find_canvas_by_client(event->window);
    if (canvas) {
        // Zombie detection for transient windows
        if (canvas->is_transient) {
            canvas->consecutive_unmaps++;
            
            // If this is the second consecutive unmap without a remap, it's a zombie
            if (canvas->consecutive_unmaps >= 2) {
                destroy_canvas(canvas);
            }
        }
    }
}

void handle_destroy_notify(XDestroyWindowEvent *event) {    
    Canvas *canvas = find_canvas(event->window);
    if (!canvas) {
        canvas = find_canvas_by_client(event->window);
    }

    if (canvas) {
        intuition_handle_destroy_notify(event);
    }
}