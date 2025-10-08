// Event dispatch and routing between intuition (window frames),
// workbench (icons), and menus. Keeps interactions coherent
// by locking routing to the initial press target.
#include "menus/menu_public.h"
#include "events.h"
// #include "compositor.h"  // Now using itn modules
#include "intuition/itn_public.h"  // Public intuition API
#include "workbench/wb_public.h"
#include "dialogs.h"
#include "iconinfo.h"  // For iconinfo_check_size_calculations
#include "render.h"  // For redraw_canvas
#include "config.h"
#include "amiwbrc.h"  // For config access
#include "xdnd.h"    // For XDND protocol support
#include <X11/extensions/Xrandr.h>
#include <X11/XF86keysym.h> // media keys
#include <X11/Xlib.h>
#include <X11/Xutil.h>  // For XGetWMName
#include <X11/Xatom.h>  // For XA_STRING
#include <X11/keysym.h>
#include <stdio.h> // For fprintf
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <strings.h> // strcasecmp
#include <stdlib.h> // getenv
#include <time.h>
#include <sys/timerfd.h>  // For timerfd_create (Phase 1)
#include <errno.h>        // For error reporting

// Track the window that owns the current button interaction so motion and
// release are routed consistently, even if X delivers them elsewhere.
static Window g_press_target = 0;

// Grab global shortcuts at X11 level so applications can't intercept them
void grab_global_shortcuts(Display *display, Window root) {
    // Only grab shortcuts that MUST work even when other apps have focus:
    
    // Super key combinations for window management - ALWAYS grabbed
    XGrabKey(display, XKeysymToKeycode(display, XK_q),
             Mod4Mask | ShiftMask, root, True, GrabModeAsync, GrabModeAsync);  // Super+Shift+Q (Quit)
    XGrabKey(display, XKeysymToKeycode(display, XK_r),
             Mod4Mask | ShiftMask, root, True, GrabModeAsync, GrabModeAsync);  // Super+Shift+R (Restart)
    XGrabKey(display, XKeysymToKeycode(display, XK_s),
             Mod4Mask | ShiftMask, root, True, GrabModeAsync, GrabModeAsync);  // Super+Shift+S (Suspend)
    XGrabKey(display, XKeysymToKeycode(display, XK_d),
             Mod4Mask | ShiftMask, root, True, GrabModeAsync, GrabModeAsync);  // Super+Shift+D (Debug/Metrics)

    // Workbench operations - ALWAYS grabbed
    XGrabKey(display, XKeysymToKeycode(display, XK_e),
             Mod4Mask, root, True, GrabModeAsync, GrabModeAsync);  // Super+E (Execute)
    XGrabKey(display, XKeysymToKeycode(display, XK_l),
             Mod4Mask, root, True, GrabModeAsync, GrabModeAsync);  // Super+L (Requester)
    
    // Window management - ALWAYS grabbed  
    XGrabKey(display, XKeysymToKeycode(display, XK_q),
             Mod4Mask, root, True, GrabModeAsync, GrabModeAsync);  // Super+Q (Close)
    XGrabKey(display, XKeysymToKeycode(display, XK_m),
             Mod4Mask, root, True, GrabModeAsync, GrabModeAsync);  // Super+M (cycle next)
    XGrabKey(display, XKeysymToKeycode(display, XK_m),
             Mod4Mask | ShiftMask, root, True, GrabModeAsync, GrabModeAsync);  // Super+Shift+M (cycle prev)
    
    // View Modes - ALWAYS grabbed (but only active when no client has focus)
    // Grab both QWERTY numbers and AZERTY characters
    XGrabKey(display, XKeysymToKeycode(display, XK_1),
             Mod4Mask, root, True, GrabModeAsync, GrabModeAsync);  // Super+1 (QWERTY)
    XGrabKey(display, XKeysymToKeycode(display, 0x26),  // ampersand &
             Mod4Mask, root, True, GrabModeAsync, GrabModeAsync);  // Super+& (AZERTY position 1)
    XGrabKey(display, XKeysymToKeycode(display, XK_2),
             Mod4Mask, root, True, GrabModeAsync, GrabModeAsync);  // Super+2 (QWERTY)
    XGrabKey(display, XKeysymToKeycode(display, 0xe9),  // eacute é
             Mod4Mask, root, True, GrabModeAsync, GrabModeAsync);  // Super+é (AZERTY position 2)
    XGrabKey(display, XKeysymToKeycode(display, XK_3),
             Mod4Mask, root, True, GrabModeAsync, GrabModeAsync);  // Super+3 (QWERTY)
    XGrabKey(display, XKeysymToKeycode(display, 0x22),  // quotedbl "
             Mod4Mask, root, True, GrabModeAsync, GrabModeAsync);  // Super+" (AZERTY position 3)
    XGrabKey(display, XKeysymToKeycode(display, XK_4),
             Mod4Mask, root, True, GrabModeAsync, GrabModeAsync);  // Super+4 (QWERTY)
    XGrabKey(display, XKeysymToKeycode(display, 0x27),  // apostrophe '
             Mod4Mask, root, True, GrabModeAsync, GrabModeAsync);  // Super+' (AZERTY position 4)

    // Workbench operations - DO NOT GRAB
    // Super+R (Rename), Super+; (Cleanup), Super+P (Parent), Super+O (Open),
    // Super+C (Copy), Super+D (Delete), Super+N (New), Super+A (Select All)
    // These will only work when AmiWB or its windows have focus,
    // allowing client apps to use these shortcuts for their own purposes

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
    // Debug output disabled
}

// Helper function for debug canvas resolution info
static void debug_canvas_resolved(Canvas *canvas, int tx, int ty, const char *context) {
    // Debug output disabled
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
    Display *dpy = itn_core_get_display();
    if (!dpy) { return; }

    // entering event loop
    XEvent event;

    // PHASE 1: Use select() for event-driven operation
    int x_fd = ConnectionNumber(dpy);

    // Get frame timer FD from itn_render (already created during initialization)
    int frame_timer_fd = itn_render_get_timer_fd();
    if (frame_timer_fd < 0) {
        log_error("[EVENTS] Frame timer not available - rendering disabled");
    }

    // Log cap management
    #if LOGGING_ENABLED && LOG_CAP_ENABLED
    unsigned int iter = 0;  // For log truncation
    #endif
    time_t last_time_check = 0;
    time_t last_drive_check = 0;  // For diskdrives polling

    while (running) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(x_fd, &read_fds);

        int max_fd = x_fd;

        // Add frame timer to select set if available
        if (frame_timer_fd >= 0) {
            FD_SET(frame_timer_fd, &read_fds);
            if (frame_timer_fd > max_fd) max_fd = frame_timer_fd;
        }

        // Frame scheduling is now handled entirely by itn_render module
        // via itn_render_schedule_frame() when damage occurs.
        // Removing duplicate scheduling that was causing conflicts.

        // Use select with a 1-second timeout for periodic tasks
        struct timeval timeout = {1, 0};  // 1 second timeout for time/drive checks
        int ready = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);

        if (ready < 0) {
            if (errno != EINTR) {
                log_error("[EVENTS] select() failed: %s", strerror(errno));
            }
            continue;
        }

        // Handle X11 events FIRST - input has priority over rendering
        // Check running flag before XPending - display may be closed during quit
        if (FD_ISSET(x_fd, &read_fds) && running) {
            // CRITICAL: Check running on each iteration - quit handler closes display mid-loop
            while (running && XPending(dpy)) {
                XNextEvent(dpy, &event);

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

                // Route events to appropriate itn modules
                // Check for damage events
                if (event.type == itn_core_get_damage_event_base() + XDamageNotify) {
                    itn_composite_process_damage((XDamageNotifyEvent *)&event);
                } else {
                    // Compositor events now handled by itn modules
                }

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
                Canvas *canvas = itn_canvas_find_by_client(map_event->window);
                if (!canvas) {
                    // Maybe it's a frame window?
                    canvas = itn_canvas_find_by_window(map_event->window);
                    if (canvas) {
                        // Found canvas
                    }
                }
                if (canvas) {
                    
                    // ALWAYS force transient dialogs to correct position on EVERY MapNotify
                    // Simple, brutal, effective - no negotiation with GTK
                    if (canvas->is_transient) {
                        // Center dialog on screen
                        int screen_width = DisplayWidth(itn_core_get_display(), DefaultScreen(itn_core_get_display()));
                        int screen_height = DisplayHeight(itn_core_get_display(), DefaultScreen(itn_core_get_display()));
                        int frame_x = (screen_width - canvas->width) / 2;
                        int frame_y = (screen_height - canvas->height) / 2;
                        if (frame_y < MENUBAR_HEIGHT) frame_y = MENUBAR_HEIGHT;
                        
                        // Get ACTUAL position of frame window - not the lies in canvas
                        int real_x = 0, real_y = 0;
                        Window child;
                        safe_translate_coordinates(itn_core_get_display(), canvas->win, 
                                            DefaultRootWindow(itn_core_get_display()),
                                            0, 0, &real_x, &real_y, &child);
                        
                        // Position mismatch detected - silent per logging rules
                        
                        // ALWAYS force move - don't trust canvas position
                        XMoveWindow(itn_core_get_display(), canvas->win, frame_x, frame_y);
                        XSync(itn_core_get_display(), False);
                        
                        canvas->x = frame_x;
                        canvas->y = frame_y;
                        
                        // Force client to correct position within frame (only if this IS the client)
                        if (map_event->window == canvas->client_win) {
                            XMoveWindow(itn_core_get_display(), map_event->window, BORDER_WIDTH_LEFT, BORDER_HEIGHT_TOP);
                            XSync(itn_core_get_display(), False);
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
                        safe_translate_coordinates(itn_core_get_display(), canvas->win, 
                                            DefaultRootWindow(itn_core_get_display()),
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
                            if (safe_get_window_attributes(itn_core_get_display(), canvas->win, &frame_attrs) &&
                                frame_attrs.map_state == IsUnmapped) {
                                
                                // Map the window first
                                XMapWindow(itn_core_get_display(), canvas->win);
                                XSync(itn_core_get_display(), False);
                                
                                // Calculate center position
                                int screen_width = DisplayWidth(itn_core_get_display(), DefaultScreen(itn_core_get_display()));
                                int screen_height = DisplayHeight(itn_core_get_display(), DefaultScreen(itn_core_get_display()));
                                int center_x = (screen_width - canvas->width) / 2;
                                int center_y = (screen_height - canvas->height) / 2;
                                if (center_y < MENUBAR_HEIGHT) center_y = MENUBAR_HEIGHT;
                                
                                // Move to center
                                XMoveWindow(itn_core_get_display(), canvas->win, center_x, center_y);
                                XSync(itn_core_get_display(), False);
                                
                                // VERIFY it moved - if not, the client is the culprit
                                int actual_x = 0, actual_y = 0;
                                Window child;
                                safe_translate_coordinates(itn_core_get_display(), canvas->win,
                                                    DefaultRootWindow(itn_core_get_display()),
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
                        XRaiseWindow(itn_core_get_display(), canvas->win);
                        // Safe focus with validation and BadMatch error handling
                        safe_set_input_focus(itn_core_get_display(), map_event->window, RevertToParent, CurrentTime);
                        itn_focus_set_active(canvas);
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
                // Check for XDND messages first
                if (event.xclient.message_type == xdnd_ctx.XdndEnter) {
                    xdnd_handle_enter(dpy, &event.xclient);
                } else if (event.xclient.message_type == xdnd_ctx.XdndPosition) {
                    xdnd_handle_position(dpy, &event.xclient);
                } else if (event.xclient.message_type == xdnd_ctx.XdndLeave) {
                    xdnd_handle_leave(dpy, &event.xclient);
                } else if (event.xclient.message_type == xdnd_ctx.XdndDrop) {
                    xdnd_handle_drop(dpy, &event.xclient);
                } else if (event.xclient.message_type == xdnd_ctx.XdndStatus) {
                    // Handle status response when we're the source
                    Window target = event.xclient.data.l[0];
                    bool accepted = event.xclient.data.l[1] & 1;
                    xdnd_ctx.target_accepts = accepted;
                    // Silent operation - no logging in normal flow
                    (void)target;  // Suppress unused warning
                } else if (event.xclient.message_type == xdnd_ctx.XdndFinished) {
                    // Target has finished processing the drop - clean up drag state
                    workbench_cleanup_drag_state();
                } else {
                    // Other client messages (EWMH, etc.)
                    intuition_handle_client_message(&event.xclient);
                }
                break;
            case SelectionRequest:
                // Handle selection requests for XDND data transfer
                xdnd_handle_selection_request(dpy, &event.xselectionrequest);
                break;
            case SelectionNotify:
                // Handle selection notify when receiving XDND data
                xdnd_handle_selection_notify(dpy, &event.xselection);
                break;
            default:
                break;
                }

                // Note: compositor_flush_pending is no longer needed with Phase 1
                // The frame scheduler handles all batching automatically
            }  // End of while (XPending(dpy))
        }  // End of if (FD_ISSET(x_fd, &read_fds))

        // Handle frame timer AFTER X11 events - input gets priority
        if (frame_timer_fd >= 0 && FD_ISSET(frame_timer_fd, &read_fds)) {
            itn_render_consume_timer();
            itn_render_process_frame();
        }

        // CRITICAL: Check periodic tasks on EVERY iteration, not just on select() timeout
        // This ensures menubar updates even when X events are flooding in (e.g., fullscreen video)
        time_t now = time(NULL);

        // Check if we should update time and addons
        if (now - last_time_check >= 1) {  // Check every second
            last_time_check = now;
            update_menubar_time();      // Will only redraw if minute changed
            menu_addon_update_all();    // Update CPU, memory, fans, etc.
        }

        // Check for drive changes every second
        if (now - last_drive_check >= 1) {
            last_drive_check = now;
            extern void diskdrives_poll(void);
            diskdrives_poll();
        }

        // CRITICAL FIX: Check progress dialogs on EVERY iteration, not just timeout
        // These functions use non-blocking I/O and return immediately if no data
        // Without this, progress dialogs never appear because select() rarely times out
        workbench_check_progress_dialogs();
        iconinfo_check_size_calculations();
        intuition_check_arrow_scroll_repeat();
    }  // End of while (running)
}

// Ask the main loop to exit cleanly.
void quit_event_loop(void) {
    running = false;
}

// Walk up ancestors to find a Canvas window and translate coordinates
static Canvas *resolve_event_canvas(Window w, int in_x, int in_y, int *out_x, int *out_y) {
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
    Canvas *canvas = itn_canvas_find_by_window(event->window);
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
                itn_focus_cycle_prev();
                return;
            }
            // Super+Shift+D: Performance metrics debug
            if (keysym == XK_d || keysym == XK_D) {
                log_error("[METRICS] Performance snapshot requested");
                itn_render_log_metrics();
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
            // Super+R: Rename selected icon (only if no client window has focus)
            if (keysym == XK_r || keysym == XK_R) {
                Canvas *active = itn_focus_get_active();
                if (!active || active->client_win == None) {
                    trigger_rename_action();
                    return;
                }
                // Let client window handle it
            }
            // Super+I: Icon Information (only if no client window has focus)
            if (keysym == XK_i || keysym == XK_I) {
                Canvas *active = itn_focus_get_active();
                if (!active || active->client_win == None) {
                    trigger_icon_info_action();
                    return;
                }
                // Let client window handle it
            }
            // Super+;: Clean up icons (only if no client window has focus)
            if (keysym == XK_semicolon) {
                Canvas *active = itn_focus_get_active();
                if (!active || active->client_win == None) {
                    trigger_cleanup_action();
                    return;
                }
                // Let client window handle it
            }
            // Super+H: Refresh active window or desktop
            if (keysym == XK_h || keysym == XK_H) {
                trigger_refresh_action();
                return;
            }
            // Super+Q: Close active window
            if (keysym == XK_q || keysym == XK_Q) {
                trigger_close_action();
                return;
            }
            // Super+P: Open parent directory (only if no client window has focus)
            if (keysym == XK_p || keysym == XK_P) {
                Canvas *active = itn_focus_get_active();
                if (!active || active->client_win == None) {
                    trigger_parent_action();
                    return;
                }
                // Let client window handle it
            }
            // Super+O: Open selected icon (only if no client window has focus)
            if (keysym == XK_o || keysym == XK_O) {
                Canvas *active = itn_focus_get_active();
                if (!active || active->client_win == None) {
                    trigger_open_action();
                    return;
                }
                // Let client window handle it
            }
            // Super+C: Copy selected icon (only if no client window has focus)
            if (keysym == XK_c || keysym == XK_C) {
                Canvas *active = itn_focus_get_active();
                if (!active || active->client_win == None) {
                    // Only trigger icon copy if it's a Workbench window
                    trigger_copy_action();
                    return;
                }
                // Let client window handle it
            }
            // Super+D: Delete selected icon (only if no client window has focus)
            if (keysym == XK_d || keysym == XK_D) {
                Canvas *active = itn_focus_get_active();
                if (!active || active->client_win == None) {
                    trigger_delete_action();
                    return;
                }
                // Let client window handle it
            }
            // Super+N: New Drawer (only if no client window has focus)
            if (keysym == XK_n || keysym == XK_N) {
                Canvas *active = itn_focus_get_active();
                if (!active || active->client_win == None) {
                    trigger_new_drawer_action();
                    return;
                }
                // Let client window handle it (New file in editors)
            }
            // Super+A: Select Contents (only if no client window has focus)
            if (keysym == XK_a || keysym == XK_A) {
                Canvas *active = itn_focus_get_active();
                if (!active || active->client_win == None) {
                    trigger_select_contents_action();
                    return;
                }
                // Let client window handle it (Select All in text editors)
            }
            // Super+M: Cycle to next window
            if (keysym == XK_m || keysym == XK_M) {
                itn_focus_cycle_next();
                return;
            }
        }

        // View Modes shortcuts - work with or without Shift (for AZERTY support)
        // Super+1: Icons view mode (only if no client window has focus)
        // QWERTY: XK_1 (0x31), AZERTY: ampersand (0x26 = &)
        if (keysym == XK_1 || keysym == 0x26) {
            Canvas *active = itn_focus_get_active();
            if (!active || active->client_win == None) {
                Canvas *target = active ? active : itn_canvas_get_desktop();
                if (target) {
                    set_canvas_view_mode(target, VIEW_ICONS);
                    update_view_modes_checkmarks();
                }
                return;
            }
        }
        // Super+2: Names view mode (only if no client window has focus)
        // QWERTY: XK_2 (0x32), AZERTY: eacute (0xe9 = é)
        if (keysym == XK_2 || keysym == 0xe9) {
            Canvas *active = itn_focus_get_active();
            if (!active || active->client_win == None) {
                Canvas *target = active ? active : itn_canvas_get_desktop();
                if (target && target->type != DESKTOP) {  // Names mode not available for desktop
                    set_canvas_view_mode(target, VIEW_NAMES);
                    update_view_modes_checkmarks();
                }
                return;
            }
        }
        // Super+3: Toggle hidden files (only if no client window has focus)
        // QWERTY: XK_3 (0x33), AZERTY: quotedbl (0x22 = ")
        if (keysym == XK_3 || keysym == 0x22) {
            Canvas *active = itn_focus_get_active();
            if (!active || active->client_win == None) {
                Canvas *target = active ? active : itn_canvas_get_desktop();
                if (target) {
                    // Toggle global hidden files state
                    bool new_state = !get_global_show_hidden_state();
                    set_global_show_hidden_state(new_state);
                    target->show_hidden = new_state;

                    // Refresh directory view
                    if (target->path) {
                        refresh_canvas_from_directory(target, target->path);
                    } else if (target->type == DESKTOP) {
                        const char *home = getenv("HOME");
                        if (home) {
                            char desktop_path[PATH_SIZE];
                            snprintf(desktop_path, sizeof(desktop_path), "%s/Desktop", home);
                            refresh_canvas_from_directory(target, desktop_path);
                        }
                    }
                    if (target->type == WINDOW) {
                        apply_view_layout(target);
                        compute_max_scroll(target);
                    }
                    redraw_canvas(target);
                    update_view_modes_checkmarks();
                }
                return;
            }
        }
        // Super+4: Toggle spatial mode (only if no client window has focus)
        // QWERTY: XK_4 (0x34), AZERTY: apostrophe (0x27 = ')
        if (keysym == XK_4 || keysym == 0x27) {
            Canvas *active = itn_focus_get_active();
            if (!active || active->client_win == None) {
                set_spatial_mode(!get_spatial_mode());
                update_view_modes_checkmarks();
                return;
            }
        }

        // Add more global shortcuts here in the future
    }
    
    // Check if active window is a dialog and route keyboard events to it
    Canvas *active = itn_focus_get_active();
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
    Canvas *canvas = itn_canvas_find_by_window(event->window);
    if (!canvas) {
        intuition_handle_map_request(event);
    }
}

// Client wants to move/resize - this is THE ONLY way clients should resize
void handle_configure_request(XConfigureRequestEvent *event) {
    Canvas *canvas = itn_canvas_find_by_client(event->window);
    if (canvas) {
        // Managed window - handle the request properly
        intuition_handle_configure_request(event);
    } else {
        // Unmanaged window - just pass it through
        XWindowChanges changes;
        changes.x = event->x;
        changes.y = event->y;
        changes.width = event->width;
        changes.height = event->height;
        changes.border_width = event->border_width;
        changes.sibling = event->above;
        changes.stack_mode = event->detail;
        XConfigureWindow(itn_core_get_display(), event->window, event->value_mask, &changes);
    }
}

// Dispatch property notify
// WM hints, protocols, and netwm properties changes.
// Also handles dynamic title updates via _AMIWB_TITLE_CHANGE property
void handle_property_notify(XPropertyEvent *event) {
    if (!event) return;
    
    Display *dpy = itn_core_get_display();
    
    // First check for AMIWB_OPEN_DIRECTORY property on root window (from ReqASL)
    if (event->window == DefaultRootWindow(dpy)) {
        Atom amiwb_open_dir = XInternAtom(dpy, "AMIWB_OPEN_DIRECTORY", False);
        
        if (event->atom == amiwb_open_dir && event->state == PropertyNewValue) {
            // Read the directory path from the property
            Atom actual_type;
            int actual_format;
            unsigned long nitems, bytes_after;
            unsigned char *prop_data = NULL;
            
            if (XGetWindowProperty(dpy, event->window, amiwb_open_dir,
                                 0, PATH_SIZE, True, // Delete property after reading
                                 XA_STRING, &actual_type, &actual_format,
                                 &nitems, &bytes_after, &prop_data) == Success) {
                
                if (prop_data && nitems > 0) {
                    // Open the directory in a new workbench window
                    workbench_open_directory((char *)prop_data);
                    XFree(prop_data);
                }
            }
            return;
        }
    }
    
    // Now check for properties on client windows
    Canvas *canvas = itn_canvas_find_by_client(event->window);
    if (!canvas) {
        // Try to find by main window too
        canvas = itn_canvas_find_by_window(event->window);
        if (!canvas) {
            return;
        }
    }
    
    // Check if this is our custom title change property
    Atom amiwb_title_change = XInternAtom(dpy, "_AMIWB_TITLE_CHANGE", False);
    Atom amiwb_menu_states = XInternAtom(dpy, "_AMIWB_MENU_STATES", False);
    
    // Handle menu state changes
    if (event->atom == amiwb_menu_states) {
        // Call menu handler to update menu states
        extern void handle_menu_state_change(Window win);
        handle_menu_state_change(event->window);
        return;
    }
    
    if (event->atom == amiwb_title_change) {
        if (event->state == PropertyNewValue) {
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
                    canvas->title_change = NULL;
                }
                
                // Only set if we got valid data
                if (nitems > 0) {
                    canvas->title_change = strdup((char *)data);
                    if (!canvas->title_change) {
                        log_error("[ERROR] Failed to allocate memory for title_change");
                    }
                }
                XFree(data);
                
                // Trigger a redraw of the canvas to show the new title
                redraw_canvas(canvas);
            }
        } else if (event->state == PropertyDelete) {
            // Property was deleted - revert to base title
            if (canvas->title_change) {
                free(canvas->title_change);
                canvas->title_change = NULL;
                redraw_canvas(canvas);
            }
        }
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

// Handle ConfigureNotify events - ONLY for our own frame windows
void handle_configure_notify(XConfigureEvent *event) {
    // Only handle ConfigureNotify for OUR frame windows
    Canvas *canvas = itn_canvas_find_by_window(event->window);
    if (canvas && (canvas->type == WINDOW || canvas->type == DIALOG)) {
        intuition_handle_configure_notify(event);
    }
    // IGNORE all ConfigureNotify from client windows - this is the standard!
    // Clients must use ConfigureRequest to resize, not resize themselves
}

void handle_unmap_notify(XUnmapEvent *event) {
    // First check if it's an override-redirect window being unmapped
    extern bool itn_composite_remove_override(Window win);
    if (itn_composite_remove_override(event->window)) {
        // Was an override window - schedule frame to remove it from display
        extern void itn_render_schedule_frame(void);
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

void handle_destroy_notify(XDestroyWindowEvent *event) {    
    Canvas *canvas = itn_canvas_find_by_window(event->window);
    if (!canvas) {
        canvas = itn_canvas_find_by_client(event->window);
    }

    if (canvas) {
        intuition_handle_destroy_notify(event);
    }
}