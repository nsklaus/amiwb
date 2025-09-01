// Event dispatch and routing between intuition (window frames),
// workbench (icons), and menus. Keeps interactions coherent
// by locking routing to the initial press target.
#include "menus.h"
#include "events.h"
#include "compositor.h"
#include "intuition.h"
#include "workbench.h"
#include "dialogs.h"
#include "iconinfo.h"  // For iconinfo_check_size_calculations
#include "render.h"  // For redraw_canvas
#include "config.h"
#include "amiwbrc.h"  // For config access
#include <X11/extensions/Xrandr.h>
#include <X11/XF86keysym.h> // media keys
#include <X11/Xlib.h>
#include <X11/Xutil.h>  // For XGetWMName
#include <X11/keysym.h>
#include <stdio.h> // For fprintf
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <strings.h> // strcasecmp
#include <stdlib.h> // getenv
#include <time.h>

// External functions to trigger actions from menus.c
extern void trigger_execute_action(void);
extern void trigger_requester_action(void);
extern void trigger_rename_action(void);
extern void trigger_icon_info_action(void);
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

// Grab global shortcuts at X11 level so applications can't intercept them
void grab_global_shortcuts(Display *display, Window root) {
    // Super key combinations for window management
    XGrabKey(display, XKeysymToKeycode(display, XK_q),
             Mod4Mask | ShiftMask, root, True, GrabModeAsync, GrabModeAsync);  // Super+Shift+Q
    XGrabKey(display, XKeysymToKeycode(display, XK_r),
             Mod4Mask | ShiftMask, root, True, GrabModeAsync, GrabModeAsync);  // Super+Shift+R
    XGrabKey(display, XKeysymToKeycode(display, XK_s),
             Mod4Mask | ShiftMask, root, True, GrabModeAsync, GrabModeAsync);  // Super+Shift+S
    
    // Super-only combinations
    XGrabKey(display, XKeysymToKeycode(display, XK_e),
             Mod4Mask, root, True, GrabModeAsync, GrabModeAsync);  // Super+E
    XGrabKey(display, XKeysymToKeycode(display, XK_l),
             Mod4Mask, root, True, GrabModeAsync, GrabModeAsync);  // Super+L
    XGrabKey(display, XKeysymToKeycode(display, XK_r),
             Mod4Mask, root, True, GrabModeAsync, GrabModeAsync);  // Super+R
    XGrabKey(display, XKeysymToKeycode(display, XK_semicolon),
             Mod4Mask, root, True, GrabModeAsync, GrabModeAsync);  // Super+;
    XGrabKey(display, XKeysymToKeycode(display, XK_q),
             Mod4Mask, root, True, GrabModeAsync, GrabModeAsync);  // Super+Q
    XGrabKey(display, XKeysymToKeycode(display, XK_p),
             Mod4Mask, root, True, GrabModeAsync, GrabModeAsync);  // Super+P
    XGrabKey(display, XKeysymToKeycode(display, XK_o),
             Mod4Mask, root, True, GrabModeAsync, GrabModeAsync);  // Super+O
    XGrabKey(display, XKeysymToKeycode(display, XK_c),
             Mod4Mask, root, True, GrabModeAsync, GrabModeAsync);  // Super+C
    XGrabKey(display, XKeysymToKeycode(display, XK_d),
             Mod4Mask, root, True, GrabModeAsync, GrabModeAsync);  // Super+D
    XGrabKey(display, XKeysymToKeycode(display, XK_n),
             Mod4Mask, root, True, GrabModeAsync, GrabModeAsync);  // Super+N
    XGrabKey(display, XKeysymToKeycode(display, XK_a),
             Mod4Mask, root, True, GrabModeAsync, GrabModeAsync);  // Super+A
    XGrabKey(display, XKeysymToKeycode(display, XK_m),
             Mod4Mask, root, True, GrabModeAsync, GrabModeAsync);  // Super+M (cycle next)
    XGrabKey(display, XKeysymToKeycode(display, XK_m),
             Mod4Mask | ShiftMask, root, True, GrabModeAsync, GrabModeAsync);  // Super+Shift+M (cycle prev)
    
    // Media keys - grab with AnyModifier so they work everywhere
    XGrabKey(display, XKeysymToKeycode(display, XF86XK_MonBrightnessUp),
             AnyModifier, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(display, XKeysymToKeycode(display, XF86XK_MonBrightnessDown),
             AnyModifier, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(display, XKeysymToKeycode(display, XF86XK_AudioRaiseVolume),
             AnyModifier, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(display, XKeysymToKeycode(display, XF86XK_AudioLowerVolume),
             AnyModifier, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(display, XKeysymToKeycode(display, XF86XK_AudioMute),
             AnyModifier, root, True, GrabModeAsync, GrabModeAsync);
}

// Clear the press target when a window is being destroyed
void clear_press_target_if_matches(Window win) {
    if (g_press_target == win) {
        g_press_target = 0;
    }
}

bool running = true;


// Gated debug helper (wire to env later if needed)
static inline int wb_dbg(void) { return 0; }

// Helper to check if a window is still valid
static inline bool is_window_valid(Display *dpy, Window win) {
    if (win == None) return false;
    XWindowAttributes attrs;
    return XGetWindowAttributes(dpy, win, &attrs) == True;
}

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
// Currently unused but may be needed for debugging
#if 0
static void debug_event_info(const char *event_type, Window window, int x, int y, unsigned int state) {
    if (wb_dbg()) {
        log_error("[DEBUG] %s win=0x%lx x,y=(%d,%d) state=0x%x", event_type, window, x, y, state);
    }
}

// Helper function for debug canvas resolution info
static void debug_canvas_resolved(Canvas *canvas, int tx, int ty, const char *context) {
    if (wb_dbg()) {
        log_error("[DEBUG]  %s resolved: canvas=0x%lx type=%d tx,ty=(%d,%d)", context, canvas->win, canvas->type, tx, ty);
    }
}
#endif

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

    // X connection file descriptor for select - may be used in future for select() based event loop
    // int fd = ConnectionNumber(dpy);  

    // Log cap management
    // unsigned int iter = 0;  // May be used for future debugging
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
            
            // Check progress dialogs for updates
            workbench_check_progress_dialogs();
            
            // Check iconinfo dialogs for directory size calculations
            iconinfo_check_size_calculations();
            
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
                    // Log truncated silently
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
                
                // Check if it's a client window OR a frame window
                Canvas *canvas = find_canvas_by_client(map_event->window);
                if (!canvas) {
                    // Maybe it's a frame window?
                    canvas = find_canvas(map_event->window);
                    if (canvas) {
                        // Found canvas
                    }
                }
                if (canvas) {
                    
                    // ALWAYS force transient dialogs to correct position on EVERY MapNotify
                    // Simple, brutal, effective - no negotiation with GTK
                    if (canvas->is_transient) {
                        // Center dialog on screen
                        int screen_width = DisplayWidth(get_display(), DefaultScreen(get_display()));
                        int screen_height = DisplayHeight(get_display(), DefaultScreen(get_display()));
                        int frame_x = (screen_width - canvas->width) / 2;
                        int frame_y = (screen_height - canvas->height) / 2;
                        if (frame_y < MENUBAR_HEIGHT) frame_y = MENUBAR_HEIGHT;
                        
                        // Get ACTUAL position of frame window - not the lies in canvas
                        int real_x = 0, real_y = 0;
                        Window child;
                        XTranslateCoordinates(get_display(), canvas->win, 
                                            DefaultRootWindow(get_display()),
                                            0, 0, &real_x, &real_y, &child);
                        
                        log_error("[INFO] ACTUAL position: %d,%d | Canvas THINKS: %d,%d | WANT: %d,%d", 
                               real_x, real_y, canvas->x, canvas->y, frame_x, frame_y);
                        
                        // ALWAYS force move - don't trust canvas position
                        XMoveWindow(get_display(), canvas->win, frame_x, frame_y);
                        XSync(get_display(), False);
                        
                        canvas->x = frame_x;
                        canvas->y = frame_y;
                        
                        // Force client to correct position within frame (only if this IS the client)
                        if (map_event->window == canvas->client_win) {
                            XMoveWindow(get_display(), map_event->window, BORDER_WIDTH_LEFT, BORDER_HEIGHT_TOP);
                            XSync(get_display(), False);
                        }
                        
                        // Debug: What window are we actually verifying?
                        // Verifying window mapping
                        
                        // Only verify if it's really the client window
                        if (map_event->window == canvas->client_win) {
                            // Client should be at correct position
                        } else {
                            // Window mismatch, skipping
                        }
                        
                        // Verify it actually moved
                        XTranslateCoordinates(get_display(), canvas->win, 
                                            DefaultRootWindow(get_display()),
                                            0, 0, &real_x, &real_y, &child);
                        if (real_x != frame_x || real_y != frame_y) {
                            log_error("[ERROR] Move FAILED! Still at %d,%d instead of %d,%d",
                                   real_x, real_y, frame_x, frame_y);
                        }
                    }
                    
                    // Reset unmap counter if it was previously hidden
                    if (canvas->is_transient && canvas->consecutive_unmaps > 0) {
                        canvas->consecutive_unmaps = 0;
                        
                        // Re-show frame if it was hidden
                        if (canvas->win != None) {
                            XWindowAttributes frame_attrs;
                            if (XGetWindowAttributes(get_display(), canvas->win, &frame_attrs) &&
                                frame_attrs.map_state == IsUnmapped) {
                                
                                // Map the window first
                                XMapWindow(get_display(), canvas->win);
                                XSync(get_display(), False);
                                
                                // Calculate center position
                                int screen_width = DisplayWidth(get_display(), DefaultScreen(get_display()));
                                int screen_height = DisplayHeight(get_display(), DefaultScreen(get_display()));
                                int center_x = (screen_width - canvas->width) / 2;
                                int center_y = (screen_height - canvas->height) / 2;
                                if (center_y < MENUBAR_HEIGHT) center_y = MENUBAR_HEIGHT;
                                
                                // Move to center
                                XMoveWindow(get_display(), canvas->win, center_x, center_y);
                                XSync(get_display(), False);
                                
                                // VERIFY it moved - if not, the client is the culprit
                                int actual_x = 0, actual_y = 0;
                                Window child;
                                XTranslateCoordinates(get_display(), canvas->win,
                                                    DefaultRootWindow(get_display()),
                                                    0, 0, &actual_x, &actual_y, &child);
                                
                                if (actual_x != center_x || actual_y != center_y) {
                                    // Client is at wrong position
                                    log_error("[WARNING] Client at %d,%d instead of %d,%d",
                                           actual_x, actual_y, center_x, center_y);
                                }
                                
                                // Update canvas with verified position
                                canvas->x = center_x;
                                canvas->y = center_y;
                            }
                        }
                    }
                    
                    // Always raise and activate transient dialogs when they map
                    if (canvas->is_transient && canvas->win != None) {
                        XRaiseWindow(get_display(), canvas->win);
                        XSetInputFocus(get_display(), map_event->window, RevertToParent, CurrentTime);
                        set_active_window(canvas);
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
    // rx, ry removed - were unused
    // Resolving event canvas
    while (cur && cur != root) {
        Canvas *c = find_canvas(cur);
        if (c) {
            // Translate original event coords to this canvas window coords
            int tx=0, ty=0; Window dummy;
            XTranslateCoordinates(dpy, w, c->win, in_x, in_y, &tx, &ty, &dummy);
            if (out_x) *out_x = tx;
            if (out_y) *out_y = ty;
            // Canvas resolved
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
    // Handle button press
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
            // Route to frame
        }
    }
    if (!canvas) canvas = resolve_event_canvas(event->window, event->x, event->y, &cx, &cy);
    if (!canvas) { return; }
    // Press resolved

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
                    // Reroute to frame
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
            // Check if it's an iconinfo dialog first
            extern bool is_iconinfo_canvas(Canvas *canvas);
            extern bool iconinfo_handle_button_press(XButtonEvent *event);
            
            if (is_iconinfo_canvas(canvas)) {
                dialog_consumed = iconinfo_handle_button_press(&ev);
            } else {
                dialog_consumed = dialogs_handle_button_press(&ev);
            }
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
                // Check if it's an iconinfo dialog first
                extern bool is_iconinfo_canvas(Canvas *canvas);
                extern bool iconinfo_handle_button_release(XButtonEvent *event);
                
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
    Canvas *canvas = find_canvas(event->window);
    if (!canvas) canvas = resolve_event_canvas(event->window, event->x, event->y, &cx, &cy);
    if (!canvas) { return; }
    XButtonEvent ev = create_translated_button_event(event, canvas->win, cx, cy);
    
    // For dialogs, try dialog-specific handling first
    bool dialog_consumed = false;
    if (canvas->type == DIALOG) {
        // Check if it's an iconinfo dialog first
        extern bool is_iconinfo_canvas(Canvas *canvas);
        extern bool iconinfo_handle_button_release(XButtonEvent *event);
        
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
    
    // Handle media keys first - they should work regardless of other modifiers
    // Get config for media key commands
    const AmiwbConfig *cfg = get_config();
    
    if (keysym == XF86XK_MonBrightnessUp) {
        if (cfg->brightness_up_cmd[0]) {  // If command is configured
            system(cfg->brightness_up_cmd);
        }
        // No fallback - user must configure it
        return;
    }
    if (keysym == XF86XK_MonBrightnessDown) {
        if (cfg->brightness_down_cmd[0]) {  // If command is configured
            system(cfg->brightness_down_cmd);
        }
        // No fallback - user must configure it
        return;
    }
    if (keysym == XF86XK_AudioRaiseVolume) {
        if (cfg->volume_up_cmd[0]) {  // If command is configured
            system(cfg->volume_up_cmd);
        }
        // No fallback - user must configure it
        return;
    }
    if (keysym == XF86XK_AudioLowerVolume) {
        if (cfg->volume_down_cmd[0]) {  // If command is configured
            system(cfg->volume_down_cmd);
        }
        // No fallback - user must configure it
        return;
    }
    if (keysym == XF86XK_AudioMute) {
        if (cfg->volume_mute_cmd[0]) {  // If command is configured
            system(cfg->volume_mute_cmd);
        }
        // No fallback - user must configure it
        return;
    }
    
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
            // Super+Shift+M: Cycle to previous window
            if (keysym == XK_m || keysym == XK_M) {
                cycle_prev_window();
                return;
            }
        } else {
            // Super-only combinations (no Shift)
            // Super+E: Execute command
            if (keysym == XK_e || keysym == XK_E) {
                trigger_execute_action();
                return;
            }
            // Super+L: Launch requester (reqasl)
            if (keysym == XK_l || keysym == XK_L) {
                trigger_requester_action();
                return;
            }
            // Super+R: Rename selected icon
            if (keysym == XK_r || keysym == XK_R) {
                trigger_rename_action();
                return;
            }
            // Super+I: Icon Information
            if (keysym == XK_i || keysym == XK_I) {
                trigger_icon_info_action();
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
            // Super+M: Cycle to next window
            if (keysym == XK_m || keysym == XK_M) {
                cycle_next_window();
                return;
            }
        }
        // Add more global shortcuts here in the future
    }
    
    // Check if active window is a dialog and route keyboard events to it
    Canvas *active = get_active_window();
    if (active && active->type == DIALOG) {
        bool dialog_consumed = false;
        
        // Check if it's an iconinfo dialog
        extern bool is_iconinfo_canvas(Canvas *canvas);
        extern bool iconinfo_handle_key_press(XKeyEvent *event);
        
        if (is_iconinfo_canvas(active)) {
            dialog_consumed = iconinfo_handle_key_press(event);
        } else {
            // Try other dialog types
            dialog_consumed = dialogs_handle_key_press(event);
        }
        
        if (dialog_consumed) {
            return;
        }
    }
    
    menu_handle_key_press(event);
}

// Dispatch window expose
// Forward to intuition so frames and canvases redraw.
void handle_expose(XExposeEvent *event) {
    // Canvas lookup removed - was unused
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
// Also handles dynamic title updates via AMIWB_TITLE_CHANGE property
void handle_property_notify(XPropertyEvent *event) {
    // We're only interested in client windows
    Canvas *canvas = find_canvas_by_client(event->window);
    if (!canvas) {
        return;
    }
    
    // Check if this is our custom title change property
    Display *dpy = get_display();
    Atom amiwb_title_change = XInternAtom(dpy, "AMIWB_TITLE_CHANGE", False);
    
    if (event->atom == amiwb_title_change && event->state == PropertyNewValue) {
        // Get the new title from the property
        Atom actual_type;
        int actual_format;
        unsigned long nitems, bytes_after;
        unsigned char *data = NULL;
        
        if (XGetWindowProperty(dpy, event->window, amiwb_title_change,
                              0, 256, False, AnyPropertyType,
                              &actual_type, &actual_format, &nitems, &bytes_after,
                              &data) == Success && data) {
            
            // Update the canvas title_change field
            if (canvas->title_change) {
                free(canvas->title_change);
            }
            canvas->title_change = strdup((char *)data);
            XFree(data);
            
            // Trigger a redraw of the canvas to show the new title
            redraw_canvas(canvas);
        }
    }
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
        // No canvas for this motion event
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
                if (canvas->win != None) {
                    XUnmapWindow(get_display(), canvas->win);
                }
                
                // Restore focus to parent window when dialog is hidden
                if (parent_win != None) {
                    Canvas *parent_canvas = find_canvas_by_client(parent_win);
                    if (parent_canvas) {
                        set_active_window(parent_canvas);
                        XSetInputFocus(get_display(), parent_win, RevertToParent, CurrentTime);
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

void handle_destroy_notify(XDestroyWindowEvent *event) {    
    Canvas *canvas = find_canvas(event->window);
    if (!canvas) {
        canvas = find_canvas_by_client(event->window);
    }

    if (canvas) {
        intuition_handle_destroy_notify(event);
    }
}