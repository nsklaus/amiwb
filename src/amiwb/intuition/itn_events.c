// Event routing and handling
// This module routes X11 events to specialized handlers

#include "../config.h"
#include "itn_internal.h"
#include "../render/rnd_public.h"
#include "../workbench/wb_public.h"
#include "../menus/menu_public.h"
#include "itn_scrollbar.h"
#include "itn_drag.h"
#include "itn_buttons.h"
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrandr.h>
#include <sys/time.h>
#include <time.h>
#include <math.h>

// Define the RandR event base (was in intuition.c)
int randr_event_base = 0;

// Module-private state
static bool g_last_press_consumed = false;

// Mouse wheel scrolling constant (same as scrollbar module)
#define SCROLL_STEP 20

// Hit test constants (used by button module)
#define HIT_NONE      0
#define HIT_CLOSE     1
#define HIT_LOWER     2
#define HIT_ICONIFY   3
#define HIT_MAXIMIZE  4
#define HIT_TITLEBAR  5

// ============================================================================
// Damage Event Handling (Compositor Integration)
// ============================================================================

void itn_events_handle_damage(XDamageNotifyEvent *event) {
    if (!event || !itn_composite_is_active()) return;

    // Find canvas for this damage event
    Canvas *canvas = itn_canvas_find_by_client(event->drawable);
    if (!canvas) {
        canvas = itn_canvas_find_by_window(event->drawable);
    }
    if (!canvas) return;

    // Record damage event for metrics
    itn_render_record_damage_event();

    // Mark canvas as needing repaint
    canvas->comp_needs_repaint = true;

    // Accumulate damage bounds
    if (canvas->comp_damage_bounds.width == 0) {
        canvas->comp_damage_bounds = event->area;
    } else {
        // Expand bounds
        int right = max(canvas->comp_damage_bounds.x + canvas->comp_damage_bounds.width,
                       event->area.x + event->area.width);
        int bottom = max(canvas->comp_damage_bounds.y + canvas->comp_damage_bounds.height,
                        event->area.y + event->area.height);
        canvas->comp_damage_bounds.x = min(canvas->comp_damage_bounds.x, event->area.x);
        canvas->comp_damage_bounds.y = min(canvas->comp_damage_bounds.y, event->area.y);
        canvas->comp_damage_bounds.width = right - canvas->comp_damage_bounds.x;
        canvas->comp_damage_bounds.height = bottom - canvas->comp_damage_bounds.y;
    }

    // Update timestamp
    clock_gettime(CLOCK_MONOTONIC, &canvas->comp_last_damage_time);

    // Accumulate damage for rendering
    itn_render_accumulate_canvas_damage(canvas);

    // Clear damage so we get more events
    Display *dpy = itn_core_get_display();
    if (dpy && canvas->comp_damage) {
        XDamageSubtract(dpy, canvas->comp_damage, None, None);
    }

    // Schedule frame
    itn_render_schedule_frame();
}

// ============================================================================
// Client Message Events (Fullscreen Requests)
// ============================================================================

void intuition_handle_client_message(XClientMessageEvent *event) {
    if (!event) return;
    Display *display = itn_core_get_display();
    if (!display) return;
    Atom net_wm_state = XInternAtom(display, "_NET_WM_STATE", False);
    if ((Atom)event->message_type != net_wm_state) return;
    Atom fs = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
    long action = event->data.l[0];
    Atom a1 = (Atom)event->data.l[1];
    Atom a2 = (Atom)event->data.l[2];
    if (!(a1 == fs || a2 == fs)) return;
    Canvas *c = itn_canvas_find_by_client(event->window);
    if (!c) c = itn_canvas_find_by_window(event->window);
    if (!c) return;
    if (action == 1) {
        intuition_enter_fullscreen(c);
    } else if (action == 0) {
        intuition_exit_fullscreen(c);
    } else if (action == 2) {
        if (c->fullscreen) intuition_exit_fullscreen(c); else intuition_enter_fullscreen(c);
    }
}

// ============================================================================
// Expose Events
// ============================================================================

void intuition_handle_expose(XExposeEvent *event) {
    Canvas *canvas = itn_canvas_find_by_window(event->window);

    if (canvas && !itn_core_is_fullscreen_active()) {
        // Handle Expose events for all canvas types
        if (canvas->type == DESKTOP || canvas->type == MENU || canvas->type == DIALOG ||
            canvas->type == WINDOW) {

            // HOT-RESTART FIX: One-time full redraw + comp_pixmap refresh for obscured regions
            // After hot-restart, obscured window regions have garbage pixels in comp_pixmap
            // First Expose event triggers: draw to window + recreate comp_pixmap snapshot
            // This gives us: no trashing (fixed) + normal performance (maintained)
            if (canvas->needs_hotrestart_redraw) {
                redraw_canvas(canvas);  // Step 1: Draw decorations to window (updates backing store)
                itn_composite_update_canvas_pixmap(canvas);  // Step 2: Recreate comp_pixmap (captures fresh pixels)
                canvas->needs_hotrestart_redraw = false;  // Clear flag - back to fast path
            }

            DAMAGE_CANVAS(canvas);
            SCHEDULE_FRAME();
        }
    }
}

// ============================================================================
// Property Notify Events (Title Changes, IPC)
// ============================================================================

void intuition_handle_property_notify(XPropertyEvent *event) {
    Display *display = itn_core_get_display();
    Window root = itn_core_get_root();
    if (!display) return;

    // Check for IPC from ReqASL to open directory
    static Atom amiwb_open_dir = None;
    if (amiwb_open_dir == None) {
        amiwb_open_dir = XInternAtom(display, "AMIWB_OPEN_DIRECTORY", False);
    }

    if (event->atom == amiwb_open_dir && event->window == root) {
        // Read the path from the property
        Atom actual_type;
        int actual_format;
        unsigned long nitems, bytes_after;
        unsigned char *data = NULL;

        if (XGetWindowProperty(display, root, amiwb_open_dir,
                              0, PATH_SIZE, True, XA_STRING,
                              &actual_type, &actual_format,
                              &nitems, &bytes_after, &data) == Success) {
            if (data && nitems > 0) {
                // Open the directory
                workbench_open_directory((char *)data);
                XFree(data);
            }
        }
        return;
    }

    // Check for title change property from clients (e.g., ReqASL)
    static Atom amiwb_title_change = None;
    if (amiwb_title_change == None) {
        amiwb_title_change = XInternAtom(display, "AMIWB_TITLE_CHANGE", False);
    }

    if (event->atom == amiwb_title_change) {
        // Find the canvas by client window
        Canvas *canvas = itn_canvas_find_by_client(event->window);
        if (canvas) {
            // Property was changed, read the new value
            Atom actual_type;
            int actual_format;
            unsigned long nitems, bytes_after;
            unsigned char *prop_data = NULL;

            if (XGetWindowProperty(display, event->window, amiwb_title_change,
                                  0, 256, False, XA_STRING,
                                  &actual_type, &actual_format,
                                  &nitems, &bytes_after, &prop_data) == Success) {

                // Update the canvas title_change
                if (canvas->title_change) {
                    free(canvas->title_change);
                    canvas->title_change = NULL;
                }

                if (prop_data && nitems > 0) {
                    canvas->title_change = strndup((char *)prop_data, nitems);
                    XFree(prop_data);

                    // Trigger a redraw of the canvas to show the new title
                    DAMAGE_CANVAS(canvas);
                    SCHEDULE_FRAME();
                }
            }
        }
        return;
    }
}

// ============================================================================
// Button Press Events
// ============================================================================

void intuition_handle_button_press(XButtonEvent *event) {
    Display *display = itn_core_get_display();
    if (!display) return;

    Canvas *canvas = itn_canvas_find_by_window(event->window);

    // If not found by frame window, check if event is on a client window
    if (!canvas) {
        canvas = itn_canvas_find_by_client(event->window);
    }

    if (!canvas) {
        return;
    }

    if (canvas->type != MENU && (event->button == Button1 || event->button == Button3)) {
        if (get_show_menus_state()) {
            toggle_menubar_state();
            return;
        }
    }

    if (canvas->type == DESKTOP) {
        handle_desktop_button(event);
        DAMAGE_CANVAS(canvas);
        SCHEDULE_FRAME();
        g_last_press_consumed = true;
        return;
    }

    if (canvas->type != WINDOW && canvas->type != DIALOG)
        return;

    itn_focus_set_active(canvas);

    // If this click was on the client window itself (grabbed via XGrabButton),
    // replay it to the client so they receive the click after activation
    if (event->window == canvas->client_win) {
        XAllowEvents(display, ReplayPointer, event->time);
        return;
    }

    // Route to scrollbar module (handles arrows, track, knob)
    if (itn_scrollbar_handle_button_press(canvas, event)) {
        g_last_press_consumed = true;
        return;
    }

    // Handle window control buttons (delegated to button module)
    if (itn_buttons_handle_press(canvas, event)) {
        g_last_press_consumed = true;
        return;
    }

    // Handle titlebar drag (not a button, special case)
    if (event->button == Button1) {
        int hit = hit_test(canvas, event->x, event->y);
        if (hit == HIT_TITLEBAR) {
            itn_drag_start(canvas, event->x_root, event->y_root);
            g_last_press_consumed = true;
            return;
        }
    }


    // Handle mouse wheel scrolling for non-client windows
    if (canvas->client_win == None && !canvas->disable_scrollbars) {
        if (event->button == Button4) {  // Scroll up
            canvas->scroll_y = max(0, canvas->scroll_y - SCROLL_STEP);
            DAMAGE_CANVAS(canvas);
            SCHEDULE_FRAME();
            g_last_press_consumed = true;  // Wheel scroll consumed
            return;
        } else if (event->button == Button5) {  // Scroll down
            canvas->scroll_y = min(canvas->max_scroll_y, canvas->scroll_y + SCROLL_STEP);
            DAMAGE_CANVAS(canvas);
            SCHEDULE_FRAME();
            g_last_press_consumed = true;  // Wheel scroll consumed
            return;
        }
    }

    // If we get here, the click was in the content area and NOT consumed by intuition
    // Leave g_last_press_consumed = false so workbench can handle it
}

// ============================================================================
// Motion Notify Events
// ============================================================================

static Bool handle_resize_motion(XMotionEvent *event) {
    // Use the new clean resize module with motion compression
    if (itn_resize_is_active()) {
        itn_resize_motion(event->x_root, event->y_root);
        return True;
    }
    return False;
}

// Check if enough time has passed for arrow scroll repeat (delegated to scrollbar module)
void intuition_check_arrow_scroll_repeat(void) {
    itn_scrollbar_check_arrow_repeat();
}

void intuition_handle_motion_notify(XMotionEvent *event) {
    if (itn_drag_motion(event)) return;
    if (handle_resize_motion(event)) return;
    if (itn_scrollbar_handle_motion(event)) return;

    // Cancel armed buttons if mouse moves away (delegated to modules)
    Canvas *canvas = itn_canvas_find_by_window(event->window);
    if (canvas) {
        itn_scrollbar_handle_motion_cancel(canvas, event);  // Arrow buttons
        itn_buttons_handle_motion_cancel(canvas, event);    // Window control buttons
    }
}

// ============================================================================
// Destroy Notify Events
// ============================================================================

void intuition_handle_destroy_notify(XDestroyWindowEvent *event) {
    Display *display = itn_core_get_display();
    if (!display) return;

    // Mark stacking cache dirty (window destroyed = stacking order changed)
    itn_stack_mark_dirty();

    // First check if this is one of our frame windows being destroyed
    Canvas *canvas = itn_canvas_find_by_window(event->window);
    if (canvas) {
        // Our frame window was destroyed - clean up everything
        canvas->close_request_sent = false;
        itn_canvas_destroy(canvas);
        return;
    }

    // Check if this is a client window destroying itself
    canvas = itn_canvas_find_by_client(event->window);
    if (canvas) {
        // Save client window before clearing it
        Window client_win = canvas->client_win;

        // Clean up compositor damage tracking BEFORE destroying window
        // NOTE: Do NOT set canvas->client_win = None here!
        // itn_canvas_destroy() needs it to know there's a client window to clean up
        itn_canvas_cleanup_compositing(canvas);

        // Check if this client owns the app menus and restore system menu
        if (client_win == get_app_menu_window()) {
            // This client owns the menubar - restore system menu
            restore_system_menu();
        }

        // Handle transient windows specially
        if (canvas->is_transient) {
            // Save the parent window before cleanup
            Window parent_win = canvas->transient_for;

            // Remove from canvas list
            itn_manager_remove(canvas);

            // Free our frame window if it exists
            if (canvas->win != None && is_window_valid(display, canvas->win)) {
                XDestroyWindow(display, canvas->win);
            }

            // Free resources
            free(canvas->path);
            free(canvas->title_base);
            free(canvas->title_change);
            free(canvas);

            // Restore focus to parent window if it exists
            if (parent_win != None) {
                Canvas *parent_canvas = itn_canvas_find_by_client(parent_win);
                if (parent_canvas) {
                    itn_focus_set_active(parent_canvas);
                    // Safe focus with validation and BadMatch error handling
                    safe_set_input_focus(display, parent_win, RevertToParent, CurrentTime);
                }
            }
        } else {
            // Normal window - client destroyed itself, proceed with normal cleanup
            canvas->close_request_sent = false;
            itn_canvas_destroy(canvas);
        }
    }
}

// ============================================================================
// Button Release Events
// ============================================================================

void intuition_handle_button_release(XButtonEvent *event) {
    // Only end resize if we're actually resizing
    if (itn_resize_is_active()) {
        Canvas *resize_canvas = itn_resize_get_target();
        if (resize_canvas) {
            itn_resize_finish();
        }
    }

    // End window drag if active (delegated to drag module)
    if (itn_drag_is_active()) {
        itn_drag_end();
    }

    // Handle deferred button actions
    Canvas *canvas = itn_canvas_find_by_window(event->window);
    if (canvas) {
        // Handle scrollbar arrow releases (delegated to scrollbar module)
        itn_scrollbar_handle_button_release(canvas, event);

        // Handle button releases (delegated to button module)
        itn_buttons_handle_release(canvas, event);
    }
}

// ============================================================================
// Map Request/Notify Events
// ============================================================================

// Frame a client window, activate its frame, optionally map the client.
static void frame_and_activate(Window client, XWindowAttributes *attrs, bool map_client) {
    Display *display = itn_core_get_display();
    if (!display) return;

    Canvas *frame = frame_client_window(client, attrs);
    if (!frame) {
        if (map_client) XMapWindow(display, client);
        return;
    }

    if (map_client) XMapWindow(display, client);
    itn_focus_set_active(frame);
    DAMAGE_CANVAS(frame);
    SCHEDULE_FRAME();
    XSync(display, False);
}

void intuition_handle_map_request(XMapRequestEvent *event) {
    Display *display = itn_core_get_display();
    if (!display) return;

    // Mark stacking cache dirty (new window mapping = stacking order changed)
    itn_stack_mark_dirty();

    XWindowAttributes attrs;
    if (!get_window_attrs_with_defaults(event->window, &attrs)) {
        // Not a valid Window - ignore
        return;
    }

    if (should_skip_framing(event->window, &attrs)) {
        XMapWindow(display, event->window);
        // Raise override-redirect windows (popup menus, tooltips, etc)
        // This ensures they appear above other windows, not behind them
        if (attrs.override_redirect) {
            XRaiseWindow(display, event->window);
            itn_stack_mark_dirty();  // CRITICAL: XRaiseWindow doesn't generate ConfigureNotify!
        }
        send_x_command_and_sync();
        return;
    }
    frame_and_activate(event->window, &attrs, true);
}

// Handle MapNotify for toplevel client windows that became viewable without a MapRequest
void intuition_handle_map_notify(XMapEvent *event) {
    Display *display = itn_core_get_display();
    if (!display) return;

    // Mark stacking cache dirty (window mapped = stacking order changed)
    itn_stack_mark_dirty();

    // CRITICAL: Skip our own overlay window!
    Window overlay = itn_composite_get_overlay_window();
    if (event->window == overlay) {
        return;  // Never handle our own overlay window
    }

    // Get window attributes first to check for override-redirect
    XWindowAttributes attrs;
    if (!get_window_attrs_with_defaults(event->window, &attrs)) {
        // Not a valid Window (could be a Pixmap ID from icon creation)
        // X11 can generate MapNotify events for non-Window resources in some cases
        return;
    }

    // Handle override-redirect windows FIRST (before checking if managed)
    // These are popup menus, tooltips, etc. that bypass window manager
    if (attrs.override_redirect && attrs.class == InputOutput) {
        // Add to compositor's override list for proper rendering
        itn_composite_add_override(event->window, &attrs);

        // Raise to ensure it's on top in X11 stacking order
        XRaiseWindow(display, event->window);
        itn_stack_mark_dirty();  // CRITICAL: XRaiseWindow doesn't generate ConfigureNotify!
        XFlush(display);

        // Schedule a frame to composite it
        SCHEDULE_FRAME();
        return;
    }

    // Ignore if this is one of our frame windows or already managed as client
    if (itn_canvas_find_by_window(event->window) || itn_canvas_find_by_client(event->window)) {
        return;
    }

    // Ensure it's a toplevel, viewable, input-output window
    if (!is_viewable_client(event->window) || !is_toplevel_under_root(event->window)) return;

    // Skip framing for other windows we don't manage
    if (should_skip_framing(event->window, &attrs)) {
        return;
    }

    frame_and_activate(event->window, &attrs, true);
}

// ============================================================================
// Configure Request/Notify Events
// ============================================================================

static void handle_configure_unmanaged(XConfigureRequestEvent *event) {
    Display *display = itn_core_get_display();
    if (!display) return;

    XWindowAttributes attrs;
    bool attrs_valid = get_window_attrs_with_defaults(event->window, &attrs);
    unsigned long safe_mask = unmanaged_safe_mask(event, &attrs, attrs_valid);

    XWindowChanges changes = (XWindowChanges){0};
    if (safe_mask & CWX) changes.x = event->x;
    if (safe_mask & CWY) changes.y = max(event->y, MENUBAR_HEIGHT);
    if (safe_mask & CWWidth) changes.width = max(1, event->width);
    if (safe_mask & CWHeight) changes.height = max(1, event->height);

    if (attrs.class == InputOutput && (safe_mask & CWBorderWidth)) {
        bool need_set_border = false;
        if ((event->value_mask & CWBorderWidth) && event->border_width != 0) need_set_border = true;
        if (attrs_valid && attrs.border_width != 0) need_set_border = true;
        if (need_set_border) { changes.border_width = 0; safe_mask |= CWBorderWidth; }
    }
    if (safe_mask) {
        XConfigureWindow(display, event->window, safe_mask, &changes);
        send_x_command_and_sync();
    }
}

static void handle_configure_managed(Canvas *canvas, XConfigureRequestEvent *event) {
    Display *display = itn_core_get_display();
    if (!display) return;

    XWindowChanges frame_changes = (XWindowChanges){0};
    unsigned long frame_mask = 0;

    if (event->value_mask & (CWWidth | CWHeight)) {
        // Check if this is a fullscreen request (window size equals screen size)
        int screen_width = DisplayWidth(display, DefaultScreen(display));
        int screen_height = DisplayHeight(display, DefaultScreen(display));
        bool is_fullscreen_size = (event->width == screen_width && event->height == screen_height);
        bool has_fullscreen_state = is_fullscreen_active(event->window);

        // If requesting screen size OR has fullscreen state, don't add frame decorations
        if (is_fullscreen_size || has_fullscreen_state) {
            // Fullscreen - use client size directly as frame size
            frame_changes.width = event->width;
            frame_changes.height = event->height;
            // Position at 0,0 for fullscreen
            frame_changes.x = 0;
            frame_changes.y = 0;
            frame_mask |= CWX | CWY;

            // Mark as fullscreen and hide menubar
            if (!canvas->fullscreen) {
                canvas->fullscreen = true;
                itn_core_set_fullscreen_active(true);
                menubar_apply_fullscreen(true);
            }
        } else {
            // Normal window - add frame decorations
            calculate_frame_size_from_client_size(event->width, event->height, &frame_changes.width, &frame_changes.height);

            // If we were fullscreen, exit fullscreen mode
            if (canvas->fullscreen) {
                canvas->fullscreen = false;
                itn_core_set_fullscreen_active(false);
                menubar_apply_fullscreen(false);
            }
        }

        frame_mask |= (event->value_mask & CWWidth) ? CWWidth : 0;
        frame_mask |= (event->value_mask & CWHeight) ? CWHeight : 0;
    }

    // IGNORE position requests from transient windows - WE decide where they go!
    // Also skip position handling if we already set it for fullscreen above
    if (!canvas->is_transient && !(frame_mask & CWX)) {
        if (event->value_mask & CWX) { frame_changes.x = event->x; frame_mask |= CWX; }
        if (event->value_mask & CWY) { frame_changes.y = max(event->y, MENUBAR_HEIGHT); frame_mask |= CWY; }
    }

    if ((event->value_mask & (CWStackMode | CWSibling)) == (CWStackMode | CWSibling) &&
        event->detail >= 0 && event->detail <= 4) {
        XWindowAttributes sibling_attrs;
        if (safe_get_window_attributes(display, event->above, &sibling_attrs) && sibling_attrs.map_state == IsViewable) {
            frame_changes.stack_mode = event->detail;
            frame_changes.sibling = event->above;
            frame_mask |= CWStackMode | CWSibling;
        }
    }
    if (frame_mask) {
        // Damage old geometry before change
        DAMAGE_CANVAS(canvas);

        XConfigureWindow(display, canvas->win, frame_mask, &frame_changes);
        // Update canvas position AND SIZE if we changed the frame
        if (frame_mask & CWX) canvas->x = frame_changes.x;
        if (frame_mask & CWY) canvas->y = frame_changes.y;

        bool size_changed = false;
        if (frame_mask & CWWidth) {
            canvas->width = frame_changes.width;
            size_changed = true;
        }
        if (frame_mask & CWHeight) {
            canvas->height = frame_changes.height;
            size_changed = true;
        }

        // Recreate render surfaces once if size changed
        if (size_changed) {
            render_recreate_canvas_surfaces(canvas);
            // Update compositing pixmap
            if (canvas->comp_pixmap) {
                itn_composite_update_canvas_pixmap(canvas);
                // After creating fresh pixmap, redraw decorations onto it
                // This is OUTSIDE compositor hot path - only runs on ConfigureRequest resize events
                redraw_canvas(canvas);
            }
        }

        // Damage new geometry after change
        DAMAGE_CANVAS(canvas);
        SCHEDULE_FRAME();
    }

    // Configure client window within frame borders
    // TRUST THE CLIENT: Give it exactly what it requested (like dwm does)
    // The frame has already been resized to accommodate the client's request
    // For fullscreen, position client at 0,0 (no border offsets)
    int client_x = canvas->fullscreen ? 0 : BORDER_WIDTH_LEFT;
    int client_y = canvas->fullscreen ? 0 : BORDER_HEIGHT_TOP;
    XWindowChanges client_changes = { .x = client_x, .y = client_y };
    unsigned long client_mask = CWX | CWY;

    if (event->value_mask & CWWidth) {
        // Give client exactly what it asked for
        client_changes.width = event->width;
        client_mask |= CWWidth;
    }
    if (event->value_mask & CWHeight) {
        // Give client exactly what it asked for
        client_changes.height = event->height;
        client_mask |= CWHeight;
    }
    if (event->value_mask & CWBorderWidth) { client_changes.border_width = 0; client_mask |= CWBorderWidth; }
    XConfigureWindow(display, event->window, client_mask, &client_changes);

    // Send synthetic ConfigureNotify to client (like xfwm4 does)
    // This tells the client its actual size and position
    XConfigureEvent ce;
    ce.type = ConfigureNotify;
    ce.display = display;
    ce.event = event->window;
    ce.window = event->window;
    // Send root-relative coordinates so apps know their actual position on screen
    // This is crucial for proper menu/popup positioning
    if (canvas->fullscreen) {
        // Fullscreen windows are at 0,0
        ce.x = 0;
        ce.y = 0;
    } else {
        // Normal windows: frame position + decoration offsets
        ce.x = canvas->x + BORDER_WIDTH_LEFT;
        ce.y = canvas->y + BORDER_HEIGHT_TOP;
    }
    ce.width = event->width;
    ce.height = event->height;
    ce.border_width = 0;
    ce.above = None;
    ce.override_redirect = False;
    XSendEvent(display, event->window, False, StructureNotifyMask, (XEvent *)&ce);
    // Don't sync here - it causes major delays during app startup (especially GIMP)
}

// Handle ConfigureRequest from client - the ONLY way clients should resize
void intuition_handle_configure_request(XConfigureRequestEvent *event) {
    Canvas *canvas = itn_canvas_find_by_client(event->window);
    if (!canvas) {
        handle_configure_unmanaged(event);
        return;
    }

    // Process the client's request
    // handle_configure_managed() already damages and schedules frames as needed
    handle_configure_managed(canvas, event);
}

// Handle ConfigureNotify for OUR frame windows only
void intuition_handle_configure_notify(XConfigureEvent *event) {
    Canvas *canvas = itn_canvas_find_by_window(event->window);
    if (!canvas) return;

    // Process ConfigureNotify for ALL canvas types to stay in sync with X11
    // This is critical: if XMoveResizeWindow fails, ConfigureNotify tells us the actual size
    // Ignoring it causes canvas->width to desync from real window geometry
    itn_geometry_apply_resize(canvas, event->width, event->height);
}

// Also need to implement the new itn_events versions that were in the stub
void itn_events_handle_configure(XConfigureEvent *event) {
    if (!event) return;

    // Mark stacking cache dirty (window moved/resized = potential stacking change)
    itn_stack_mark_dirty();

    // Find canvas
    Canvas *canvas = itn_canvas_find_by_window(event->window);
    if (!canvas) {
        canvas = itn_canvas_find_by_client(event->window);
    }
    if (!canvas) return;

    // Check if geometry changed
    bool changed = false;
    if (canvas->x != event->x || canvas->y != event->y) {
        changed = true;
        // Damage old position
        DAMAGE_RECT(canvas->x, canvas->y, canvas->width, canvas->height);
        canvas->x = event->x;
        canvas->y = event->y;
    }

    if (canvas->width != event->width || canvas->height != event->height) {
        changed = true;
        // Damage old size
        DAMAGE_RECT(canvas->x, canvas->y, canvas->width, canvas->height);
        canvas->width = event->width;
        canvas->height = event->height;

        // Need to update pixmap for new size
        if (canvas->comp_pixmap) {
            itn_composite_update_canvas_pixmap(canvas);
            // Fresh pixmap is blank, need to redraw content
            redraw_canvas(canvas);
        }
    }

    if (changed) {
        // Damage new geometry
        DAMAGE_RECT(canvas->x, canvas->y, canvas->width, canvas->height);
        SCHEDULE_FRAME();
    }
}

void itn_events_handle_map(XMapEvent *event) {
    if (!event) return;

    // Mark stacking cache dirty (window mapped = stacking order changed)
    itn_stack_mark_dirty();

    Canvas *canvas = itn_canvas_find_by_window(event->window);
    if (!canvas) {
        canvas = itn_canvas_find_by_client(event->window);
    }
    if (!canvas) return;

    canvas->comp_mapped = true;
    canvas->comp_visible = true;

    // Clear app_hidden flag - app is remapping its window (e.g., Sublime tab switch back)
    if (canvas->app_hidden) {
        canvas->app_hidden = 0;
    }

    // Setup compositing if needed
    if (!canvas->comp_damage && itn_composite_is_active()) {
        itn_composite_setup_canvas(canvas);
    }

    // Damage entire window area
    DAMAGE_CANVAS(canvas);
    SCHEDULE_FRAME();
}

void itn_events_handle_unmap(XUnmapEvent *event) {
    if (!event) return;

    // Mark stacking cache dirty (window unmapped = stacking order changed)
    itn_stack_mark_dirty();

    // CRITICAL: Check for override-redirect window cleanup FIRST
    // Tooltips/popups are NOT Canvas windows - they must be removed from compositor
    // Otherwise we leak OverrideWin structs forever (every tooltip leak ~100 bytes)
    if (itn_composite_remove_override(event->window)) {
        // Successfully removed override-redirect window
        SCHEDULE_FRAME();
        return;
    }

    // Not an override-redirect window - check if it's a Canvas
    Canvas *canvas = itn_canvas_find_by_window(event->window);
    if (!canvas) {
        canvas = itn_canvas_find_by_client(event->window);
    }
    if (!canvas) return;

    // Detect application-initiated unmapping (e.g., Sublime tab switching)
    // If we didn't initiate the unmap (user_iconified not set), this is app behavior
    if (!canvas->user_iconified && canvas->comp_mapped) {
        canvas->app_hidden = 1;  // App is hiding its own window
    }

    canvas->comp_mapped = false;
    canvas->comp_visible = false;

    // Damage area where window was
    DAMAGE_CANVAS(canvas);
    SCHEDULE_FRAME();
}

// ============================================================================
// XRandR Screen Change Events
// ============================================================================

// Handle XRandR screen size changes: resize desktop/menubar and reload wallpapers
void intuition_handle_rr_screen_change(XRRScreenChangeNotifyEvent *event) {
    // CRITICAL: Update the Display structure's cached dimensions
    // Without this, DisplayWidth()/DisplayHeight() will return stale values
    XRRUpdateConfiguration((XEvent *)event);

    itn_core_set_screen_dimensions(event->width, event->height);

    // CRITICAL: Recreate compositor back buffer for new screen dimensions
    // Without this, compositor is stuck rendering into old-sized buffer (black bands)
    if (itn_composite_is_active()) {
        itn_composite_create_back_buffer();
    }

    // Resize desktop canvas to new screen dimensions
    Canvas *desktop = itn_canvas_get_desktop();
    if (desktop) {
        itn_geometry_move_resize(desktop, 0, MENUBAR_HEIGHT, event->width, event->height - MENUBAR_HEIGHT);
    } else {
        log_error("[WARN] Desktop canvas not found!");
    }

    // Resize menubar to new screen width
    Canvas *menubar = get_menubar();
    if (menubar) {
        itn_geometry_move_resize(menubar, 0, 0, event->width, MENUBAR_HEIGHT);
    } else {
        log_error("[WARN] Menubar canvas not found!");
    }

    // Reload wallpapers for new screen dimensions
    render_load_wallpapers();

    // Mark entire screen as damaged
    DAMAGE_RECT(0, 0, event->width, event->height);
    SCHEDULE_FRAME();
}

// ============================================================================
// Event Routing
// ============================================================================

void itn_events_route_to_canvas(Canvas *canvas, XEvent *event) {
    if (!canvas || !event) return;

    // Route event to appropriate handler based on type
    switch (event->type) {
        case ConfigureNotify:
            itn_events_handle_configure(&event->xconfigure);
            break;
        case MapNotify:
            itn_events_handle_map(&event->xmap);
            break;
        case UnmapNotify:
            itn_events_handle_unmap(&event->xunmap);
            break;
        default:
            // Check for damage events
            if (itn_composite_is_active() && event->type == itn_core_get_damage_event_base() + XDamageNotify) {
                itn_events_handle_damage((XDamageNotifyEvent *)event);
            }
            break;
    }
}

// ============================================================================
// Desktop Event Handling
// ============================================================================

void handle_desktop_button(XButtonEvent *event) {
    Canvas *desktop = itn_canvas_get_desktop();
    if (!desktop) return;

    // Right-click toggles menubar
    if (event->button == Button3) {
        toggle_menubar_state();
        return;
    }

    // Left-click gives focus to desktop and deactivates all windows
    if (event->button == Button1) {
        if (!itn_core_is_deactivate_suppressed()) {
            itn_focus_deactivate_all();
        }
    }
}

// ============================================================================
// Event State Query Functions
// ============================================================================

bool itn_events_last_press_consumed(void) {
    return g_last_press_consumed;
}

void itn_events_reset_press_consumed(void) {
    g_last_press_consumed = false;
}

bool itn_events_is_scrolling_active(void) {
    return itn_scrollbar_is_scrolling_active();
}