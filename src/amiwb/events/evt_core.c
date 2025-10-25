// File: evt_core.c
// Core event loop for AmiWB event system
// Main select() loop that dispatches X events to all evt_* modules
// Handles frame timer, periodic tasks (clock, drives, progress), and event routing

#include "evt_internal.h"
#include "../menus/menu_public.h"
#include "../intuition/itn_public.h"
#include "../intuition/itn_internal.h"
#include "../workbench/wb_public.h"
#include "../dialogs/dialog_public.h"
#include "../render/rnd_public.h"
#include "../config.h"
#include "../amiwbrc.h"
#include "../xdnd.h"
#include "../diskdrives.h"
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

// ============================================================================
// Event Loop State (Private - Encapsulated)
// ============================================================================

// Main event loop running state - private to this module
static bool running = true;

// Getters/Setters for event loop state (Public API)
bool evt_core_is_running(void) {
    return running;
}

void evt_core_stop(void) {
    running = false;
}

// ============================================================================
// Helper Functions (Private)
// ============================================================================

// Gated debug helper (wire to env later if needed)
static inline int wb_dbg(void) { return 0; }

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

    // Get disk drives inotify FD (event-driven monitoring, module already initialized)
    int diskdrives_inotify_fd = diskdrives_get_inotify_fd();

    // Log cap management
    #if LOGGING_ENABLED && LOG_CAP_ENABLED
    unsigned int iter = 0;  // For log truncation
    #endif
    time_t last_time_check = 0;

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

        // Add diskdrives inotify to select set if available
        if (diskdrives_inotify_fd >= 0) {
            FD_SET(diskdrives_inotify_fd, &read_fds);
            if (diskdrives_inotify_fd > max_fd) max_fd = diskdrives_inotify_fd;
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
                if (canvas && canvas->is_transient) {
                    // Transient window remapping - show the frame again
                    Display *dpy = itn_core_get_display();

                    // Show the frame if it was hidden
                    XMapRaised(dpy, canvas->win);

                    // Restore compositor visibility
                    canvas->comp_visible = true;
                    canvas->comp_mapped = true;

                    // Raise and activate
                    XRaiseWindow(dpy, canvas->win);
                    safe_set_input_focus(dpy, map_event->window, RevertToParent, CurrentTime);
                    itn_focus_set_active(canvas);

                    // Schedule frame to show it
                    itn_render_schedule_frame();
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

        // Handle diskdrives inotify events (mount/unmount, device plug/unplug)
        if (diskdrives_inotify_fd >= 0 && FD_ISSET(diskdrives_inotify_fd, &read_fds)) {
            diskdrives_process_events();
        }

        // CRITICAL: Check periodic tasks on EVERY iteration, not just on select() timeout
        // This ensures menubar updates even when X events are flooding in (e.g., fullscreen video)
        time_t now = time(NULL);

        // Check if we should update time and addons
        if (now - last_time_check >= 2) {  // Check every 2 seconds
            last_time_check = now;
            menu_addon_update_all();    // Update CPU, memory, fans, clock, etc.

            // Redraw menubar to display addon changes in real-time (logo mode only)
            // Without this, CPU/RAM/fans show stale data for up to 59 seconds
            Canvas *menubar_canvas = get_menubar();
            if (menubar_canvas && !get_show_menus_state()) {
                Menu *active = get_active_menu();

                // Allow update if: no active menu, OR canvas destroyed, OR not window list
                if (!active || active->canvas == NULL || active->parent_index != -1) {
                    redraw_canvas(menubar_canvas);
                    menubar_canvas->comp_needs_repaint = true;
                    itn_render_accumulate_canvas_damage(menubar_canvas);
                    itn_render_schedule_frame();
                }
            }
        }

        // Drive monitoring now event-driven via inotify (above)

        // CRITICAL FIX: Check progress monitors on EVERY iteration, not just timeout
        // These functions use non-blocking I/O and return immediately if no data
        // Without this, progress monitors never appear because select() rarely times out
        workbench_check_progress_monitors();
        iconinfo_check_updates();  // Handles both size calculations and device stat updates
        intuition_check_arrow_scroll_repeat();
    }  // End of while (running)
}

// Ask the main loop to exit cleanly.
void quit_event_loop(void) {
    running = false;
}

