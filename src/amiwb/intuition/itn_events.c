// Event routing and handling
// This module handles X11 events and damage notifications

#include "../config.h"
#include "itn_internal.h"
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrandr.h>
#include <sys/time.h>
#include <time.h>
#include <math.h>

// External references (temporary during migration)
extern Display *display;
extern int screen;
extern Window root;
extern int width, height;
extern Canvas *active_window;
extern bool fullscreen_active;
// Define the RandR event base (was in intuition.c)
int randr_event_base = 0;
extern bool g_last_press_consumed;
extern Canvas *dragging_canvas;
extern Canvas *scrolling_canvas;
extern Canvas *arrow_scroll_canvas;
extern int arrow_scroll_direction;
extern bool arrow_scroll_vertical;
extern struct timeval arrow_scroll_start_time;
extern struct timeval arrow_scroll_last_time;
extern int drag_start_x, drag_start_y;
extern int window_start_x, window_start_y;
extern bool scrolling_vertical;
extern int initial_scroll, scroll_start_pos;

// External functions from intuition.c (temporary during migration)
// find_canvas is now itn_canvas_find_by_window
extern Canvas *itn_canvas_find_by_client(Window client);
extern void set_active_window(Canvas *canvas);
extern void redraw_canvas(Canvas *canvas);  // Will be replaced with damage accumulation
// get_desktop_canvas is now itn_canvas_get_desktop
extern bool is_window_valid(Display *dpy, Window win);
extern void send_x_command_and_sync(void);
extern void send_close_request_to_client(Window client);
extern void request_client_close(Canvas *canvas);
extern void iconify_canvas(Canvas *canvas);
extern void menubar_apply_fullscreen(bool fullscreen);
extern bool get_show_menus_state(void);
extern void toggle_menubar_state(void);
extern void intuition_enter_fullscreen(Canvas *canvas);
extern void intuition_exit_fullscreen(Canvas *canvas);
extern Canvas *frame_client_window(Window client, XWindowAttributes *attrs);
extern bool get_window_attrs_with_defaults(Window win, XWindowAttributes *attrs);
extern bool should_skip_framing(Window win, XWindowAttributes *attrs);
extern bool is_viewable_client(Window win);
extern bool is_toplevel_under_root(Window win);
extern unsigned long unmanaged_safe_mask(XConfigureRequestEvent *event, XWindowAttributes *attrs, bool attrs_valid);
extern void calculate_frame_size_from_client_size(int client_w, int client_h, int *frame_w, int *frame_h);
extern bool is_fullscreen_active(Window win);
extern void render_recreate_canvas_surfaces(Canvas *canvas);
extern void remove_canvas_from_array(Canvas *canvas);
extern void workbench_open_directory(const char *path);
extern void itn_canvas_destroy(Canvas *canvas);
extern void create_iconified_icon(Canvas *canvas);
// Scrollbar constants
#define SCROLL_STEP 20
#define TRACK_MARGIN 10
#define TRACK_RESERVED 54

extern int clamp_value_between(int value, int min, int max);

// Scrollbar calculation functions
static void get_scrollbar_track_area(Canvas *canvas, bool is_vertical, int *x, int *y, int *width, int *height) {
    if (is_vertical) {
        *x = canvas->width - BORDER_WIDTH_RIGHT;
        *y = BORDER_HEIGHT_TOP + TRACK_MARGIN;
        *width = BORDER_WIDTH_RIGHT;
        *height = (canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM) - TRACK_RESERVED - TRACK_MARGIN;
    } else {
        *x = BORDER_WIDTH_LEFT + TRACK_MARGIN;
        *y = canvas->height - BORDER_HEIGHT_BOTTOM;
        *width = (canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT) - TRACK_RESERVED - TRACK_MARGIN;
        *height = BORDER_HEIGHT_BOTTOM;
    }
}

static int calculate_scrollbar_knob_size(int track_length, int content_length) {
    float size_ratio = (float)track_length / (float)content_length;
    int knob_size = (int)(size_ratio * track_length);
    if (knob_size < MIN_KNOB_SIZE) knob_size = MIN_KNOB_SIZE;
    if (knob_size > track_length) knob_size = track_length;
    return knob_size;
}

static int calculate_knob_position_from_scroll(int track_length, int knob_length, int scroll_amount, int max_scroll) {
    if (max_scroll <= 0) return 0;
    float position_ratio = (float)scroll_amount / (float)max_scroll;
    int available_space = track_length - knob_length;
    if (available_space <= 0) return 0;
    return (int)(position_ratio * available_space);
}

static int calculate_scroll_from_mouse_click(int track_start, int track_length, int max_scroll, int click_position) {
    float click_ratio = (float)(click_position - track_start) / (float)track_length;
    int scroll_value = (int)(click_ratio * (float)max_scroll);
    return clamp_value_between(scroll_value, 0, max_scroll);
}

static bool handle_scrollbar_click(Canvas *canvas, XButtonEvent *event, bool is_vertical) {
    if (event->button != Button1) return false;

    int track_x, track_y, track_width, track_height;
    get_scrollbar_track_area(canvas, is_vertical, &track_x, &track_y, &track_width, &track_height);

    // Check if click is within track area
    bool click_in_track = (event->x >= track_x && event->x < track_x + track_width &&
                          event->y >= track_y && event->y < track_y + track_height);
    if (!click_in_track) return false;

    int track_length = is_vertical ? track_height : track_width;
    int content_length = is_vertical ? canvas->content_height : canvas->content_width;
    int current_scroll = is_vertical ? canvas->scroll_y : canvas->scroll_x;
    int max_scroll = is_vertical ? canvas->max_scroll_y : canvas->max_scroll_x;

    int knob_length = calculate_scrollbar_knob_size(track_length, content_length);
    int knob_position = (is_vertical ? track_y : track_x) +
                       calculate_knob_position_from_scroll(track_length, knob_length, current_scroll, max_scroll);

    int click_coordinate = is_vertical ? event->y : event->x;
    bool click_on_knob = (click_coordinate >= knob_position && click_coordinate < knob_position + knob_length);

    if (click_on_knob) {
        // Start dragging the knob
        int root_coordinate = is_vertical ? event->y_root : event->x_root;
        scrolling_canvas = canvas;
        scrolling_vertical = is_vertical;
        initial_scroll = current_scroll;
        scroll_start_pos = root_coordinate;
    } else {
        // Click on track - jump to that position
        int track_start = is_vertical ? track_y : track_x;
        int new_scroll = calculate_scroll_from_mouse_click(track_start, track_length, max_scroll, click_coordinate);
        if (is_vertical) canvas->scroll_y = new_scroll;
        else canvas->scroll_x = new_scroll;
        DAMAGE_CANVAS(canvas);
        SCHEDULE_FRAME();
    }
    return true;
}

extern bool resize_is_active(void);

// Hit test for window controls
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

extern int hit_test(Canvas *canvas, int x, int y);

// ============================================================================
// Damage Event Handling (Compositor Integration)
// ============================================================================

void itn_events_handle_damage(XDamageNotifyEvent *event) {
    if (!event || !g_compositor_active) return;

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
    if (canvas && !fullscreen_active) {
        // Only damage non-composited canvases
        // Client windows (type==WINDOW with client_win) are tracked via XDamage
        // We only need to handle Expose for our own rendering: desktop, menus, dialogs
        if (canvas->type == DESKTOP || canvas->type == MENU || canvas->type == DIALOG ||
            (canvas->type == WINDOW && canvas->client_win == None)) {
            DAMAGE_CANVAS(canvas);
            SCHEDULE_FRAME();
        }
        // Composited client windows: XDamage will notify us, don't double-damage
    }
}

// ============================================================================
// Property Notify Events (Title Changes, IPC)
// ============================================================================

void intuition_handle_property_notify(XPropertyEvent *event) {
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

    set_active_window(canvas);

    // If this click was on the client window itself (grabbed via XGrabButton),
    // replay it to the client so they receive the click after activation
    if (event->window == canvas->client_win) {
        XAllowEvents(display, ReplayPointer, event->time);
        return;
    }

    // Check for scrollbar interactions FIRST (for workbench windows)
    if (event->button == Button1 && canvas->client_win == None && !canvas->disable_scrollbars) {
        int x = event->x;
        int y = event->y;
        int w = canvas->width;
        int h = canvas->height;

        // Check vertical scroll arrows (on right border) FIRST
        if (x >= w - BORDER_WIDTH_RIGHT && x < w) {
            // Up arrow area
            if (y >= h - BORDER_HEIGHT_BOTTOM - 41 && y < h - BORDER_HEIGHT_BOTTOM - 21) {
                canvas->v_arrow_up_armed = true;
                arrow_scroll_canvas = canvas;
                arrow_scroll_direction = -1;
                arrow_scroll_vertical = true;
                gettimeofday(&arrow_scroll_start_time, NULL);
                arrow_scroll_last_time = arrow_scroll_start_time;
                // Perform initial scroll
                if (canvas->scroll_y > 0) {
                    canvas->scroll_y = max(0, canvas->scroll_y - SCROLL_STEP);
                }
                DAMAGE_CANVAS(canvas);
                SCHEDULE_FRAME();
                g_last_press_consumed = true;
                return;
            }
            // Down arrow area
            else if (y >= h - BORDER_HEIGHT_BOTTOM - 21 && y < h - BORDER_HEIGHT_BOTTOM) {
                canvas->v_arrow_down_armed = true;
                arrow_scroll_canvas = canvas;
                arrow_scroll_direction = 1;
                arrow_scroll_vertical = true;
                gettimeofday(&arrow_scroll_start_time, NULL);
                arrow_scroll_last_time = arrow_scroll_start_time;
                // Perform initial scroll
                if (canvas->scroll_y < canvas->max_scroll_y) {
                    canvas->scroll_y = min(canvas->max_scroll_y, canvas->scroll_y + SCROLL_STEP);
                }
                DAMAGE_CANVAS(canvas);
                SCHEDULE_FRAME();
                g_last_press_consumed = true;
                return;
            }
        }

        // Check horizontal scroll arrows (on bottom border)
        if (y >= h - BORDER_HEIGHT_BOTTOM && y < h) {
            // Left arrow area
            if (x >= w - BORDER_WIDTH_RIGHT - 41 && x < w - BORDER_WIDTH_RIGHT - 21) {
                canvas->h_arrow_left_armed = true;
                arrow_scroll_canvas = canvas;
                arrow_scroll_direction = -1;
                arrow_scroll_vertical = false;
                gettimeofday(&arrow_scroll_start_time, NULL);
                arrow_scroll_last_time = arrow_scroll_start_time;
                // Perform initial scroll
                if (canvas->scroll_x > 0) {
                    canvas->scroll_x = max(0, canvas->scroll_x - SCROLL_STEP);
                }
                DAMAGE_CANVAS(canvas);
                SCHEDULE_FRAME();
                g_last_press_consumed = true;
                return;
            }
            // Right arrow area
            else if (x >= w - BORDER_WIDTH_RIGHT - 21 && x < w - BORDER_WIDTH_RIGHT) {
                canvas->h_arrow_right_armed = true;
                arrow_scroll_canvas = canvas;
                arrow_scroll_direction = 1;
                arrow_scroll_vertical = false;
                gettimeofday(&arrow_scroll_start_time, NULL);
                arrow_scroll_last_time = arrow_scroll_start_time;
                // Perform initial scroll
                if (canvas->scroll_x < canvas->max_scroll_x) {
                    canvas->scroll_x = min(canvas->max_scroll_x, canvas->scroll_x + SCROLL_STEP);
                }
                DAMAGE_CANVAS(canvas);
                SCHEDULE_FRAME();
                g_last_press_consumed = true;
                return;
            }
        }

        // Now check scrollbar track/knob
        // Try vertical scrollbar first
        if (handle_scrollbar_click(canvas, event, true)) {
            g_last_press_consumed = true;
            return;
        }
        // Then horizontal scrollbar
        if (handle_scrollbar_click(canvas, event, false)) {
            g_last_press_consumed = true;
            return;
        }
    }

    // Handle window control clicks
    if (event->button == Button1) {
        int hit = hit_test(canvas, event->x, event->y);

        switch (hit) {
            case HIT_CLOSE:
                canvas->close_armed = true;
                DAMAGE_CANVAS(canvas);
                SCHEDULE_FRAME();
                break;

            case HIT_ICONIFY:
                canvas->iconify_armed = true;
                DAMAGE_CANVAS(canvas);
                SCHEDULE_FRAME();
                break;

            case HIT_MAXIMIZE:
                canvas->maximize_armed = true;
                DAMAGE_CANVAS(canvas);
                SCHEDULE_FRAME();
                break;

            case HIT_LOWER:
                canvas->lower_armed = true;
                DAMAGE_CANVAS(canvas);
                SCHEDULE_FRAME();
                break;

            case HIT_TITLEBAR:
                // Start window drag
                dragging_canvas = canvas;
                drag_start_x = event->x_root;
                drag_start_y = event->y_root;
                window_start_x = canvas->x;
                window_start_y = canvas->y;

                // Grab pointer for smooth dragging
                XGrabPointer(display, canvas->win, False,
                           ButtonReleaseMask | PointerMotionMask,
                           GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
                break;

            case HIT_RESIZE_NE:
            case HIT_RESIZE_NW:
            case HIT_RESIZE_SE:
            case HIT_RESIZE_SW:
            case HIT_RESIZE_N:
            case HIT_RESIZE_S:
            case HIT_RESIZE_E:
            case HIT_RESIZE_W:
                canvas->resize_armed = true;
                DAMAGE_CANVAS(canvas);
                SCHEDULE_FRAME();
                // Start resize operation with the specific corner/edge
                itn_resize_start(canvas, hit);
                break;

            case HIT_NONE:
                // Click in client area - don't consume, let it pass to workbench handler
                g_last_press_consumed = false;
                return;
        }
    }


    // Handle mouse wheel scrolling for non-client windows
    if (canvas->client_win == None && !canvas->disable_scrollbars) {
        if (event->button == Button4) {  // Scroll up
            canvas->scroll_y = max(0, canvas->scroll_y - SCROLL_STEP);
            DAMAGE_CANVAS(canvas);
            SCHEDULE_FRAME();
        } else if (event->button == Button5) {  // Scroll down
            canvas->scroll_y = min(canvas->max_scroll_y, canvas->scroll_y + SCROLL_STEP);
            DAMAGE_CANVAS(canvas);
            SCHEDULE_FRAME();
        }
    }

    g_last_press_consumed = true;  // Window controls always consume the press
}

// ============================================================================
// Motion Notify Events
// ============================================================================

static Bool handle_drag_motion(XMotionEvent *event) {
    if (dragging_canvas) {
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

        return True;
    }
    return False;
}

static Bool handle_resize_motion(XMotionEvent *event) {
    // Use the new clean resize module with motion compression
    if (itn_resize_is_active()) {
        itn_resize_motion(event->x_root, event->y_root);
        return True;
    }
    return False;
}

static Bool handle_scroll_motion(XMotionEvent *event) {
    if (scrolling_canvas) {
        // Pass the motion coordinate in the appropriate axis
        int current_mouse_pos = scrolling_vertical ? event->y_root : event->x_root;
        update_scroll_from_mouse_drag(scrolling_canvas, scrolling_vertical,
                                     initial_scroll, scroll_start_pos, current_mouse_pos);
        return True;
    }
    return False;
}

// Handle arrow button auto-repeat while held
static Bool handle_arrow_scroll_repeat(void) {
    if (!arrow_scroll_canvas || arrow_scroll_direction == 0) return False;

    if (arrow_scroll_vertical) {
        int current_scroll = arrow_scroll_canvas->scroll_y;
        int new_scroll = current_scroll + (arrow_scroll_direction * SCROLL_STEP);
        new_scroll = clamp_value_between(new_scroll, 0, arrow_scroll_canvas->max_scroll_y);

        if (new_scroll != current_scroll) {
            arrow_scroll_canvas->scroll_y = new_scroll;
            DAMAGE_CANVAS(arrow_scroll_canvas);
            SCHEDULE_FRAME();
            return True;
        }
    } else {
        int current_scroll = arrow_scroll_canvas->scroll_x;
        int new_scroll = current_scroll + (arrow_scroll_direction * SCROLL_STEP);
        new_scroll = clamp_value_between(new_scroll, 0, arrow_scroll_canvas->max_scroll_x);

        if (new_scroll != current_scroll) {
            arrow_scroll_canvas->scroll_x = new_scroll;
            DAMAGE_CANVAS(arrow_scroll_canvas);
            SCHEDULE_FRAME();
            return True;
        }
    }

    // If we can't scroll anymore, stop the repeat
    arrow_scroll_canvas = NULL;
    arrow_scroll_direction = 0;
    return False;
}

// Check if enough time has passed for arrow scroll repeat
void intuition_check_arrow_scroll_repeat(void) {
    if (!arrow_scroll_canvas || arrow_scroll_direction == 0) return;

    struct timeval now;
    gettimeofday(&now, NULL);

    // Calculate time since last scroll
    long elapsed_ms = (now.tv_sec - arrow_scroll_last_time.tv_sec) * 1000 +
                      (now.tv_usec - arrow_scroll_last_time.tv_usec) / 1000;

    // Initial delay: 400ms, then repeat every 50ms
    long delay_ms = 50;  // Fast repeat rate
    if (now.tv_sec == arrow_scroll_start_time.tv_sec &&
        (now.tv_usec - arrow_scroll_start_time.tv_usec) < 400000) {
        delay_ms = 400;  // Initial delay before auto-repeat starts
    }

    if (elapsed_ms >= delay_ms) {
        handle_arrow_scroll_repeat();
        arrow_scroll_last_time = now;
    }
}

void intuition_handle_motion_notify(XMotionEvent *event) {
    if (handle_drag_motion(event)) return;
    if (handle_resize_motion(event)) return;
    if (handle_scroll_motion(event)) return;

    // Handle button cancel when mouse moves away
    Canvas *canvas = itn_canvas_find_by_window(event->window);
    if (canvas) {
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
            DAMAGE_CANVAS(canvas);
            SCHEDULE_FRAME();
        }
    }
}

// ============================================================================
// Destroy Notify Events
// ============================================================================

void intuition_handle_destroy_notify(XDestroyWindowEvent *event) {
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

        // Client destroyed itself - clean up properly
        canvas->client_win = None;  // Mark client as gone

        // Clean up compositor damage tracking BEFORE destroying window
        itn_canvas_cleanup_compositing(canvas);

        // Check if this client owns the app menus and restore system menu
        extern Window get_app_menu_window(void);
        extern void restore_system_menu(void);
        if (client_win == get_app_menu_window()) {
            // This client owns the menubar - restore system menu
            restore_system_menu();
        }

        // Handle transient windows specially
        if (canvas->is_transient) {
            // Save the parent window before cleanup
            Window parent_win = canvas->transient_for;

            // Remove from canvas list
            remove_canvas_from_array(canvas);

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
                    set_active_window(parent_canvas);
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

    // Release pointer grab if we were dragging
    if (dragging_canvas) {
        XUngrabPointer(display, CurrentTime);
        dragging_canvas = NULL;
    }

    scrolling_canvas = NULL;
    arrow_scroll_canvas = NULL;  // Stop arrow button auto-repeat
    arrow_scroll_direction = 0;

    // Handle deferred button actions
    Canvas *canvas = itn_canvas_find_by_window(event->window);
    if (canvas) {
        TitlebarHit hit = hit_test(canvas, event->x, event->y);

        // Handle scroll arrow releases with damage accumulation
        if (canvas->v_arrow_up_armed) {
            canvas->v_arrow_up_armed = false;
            DAMAGE_CANVAS(canvas);
            // Check if still on button
            if (event->x >= canvas->width - BORDER_WIDTH_RIGHT && event->x < canvas->width &&
                event->y >= canvas->height - BORDER_HEIGHT_BOTTOM - 41 &&
                event->y < canvas->height - BORDER_HEIGHT_BOTTOM - 21) {
                if (canvas->scroll_y > 0) {
                    canvas->scroll_y = max(0, canvas->scroll_y - SCROLL_STEP);
                    DAMAGE_CANVAS(canvas);
                }
            }
            SCHEDULE_FRAME();
        }

        if (canvas->v_arrow_down_armed) {
            canvas->v_arrow_down_armed = false;
            DAMAGE_CANVAS(canvas);
            // Check if still on button
            if (event->x >= canvas->width - BORDER_WIDTH_RIGHT && event->x < canvas->width &&
                event->y >= canvas->height - BORDER_HEIGHT_BOTTOM - 21 &&
                event->y < canvas->height - BORDER_HEIGHT_BOTTOM) {
                if (canvas->scroll_y < canvas->max_scroll_y) {
                    canvas->scroll_y = min(canvas->max_scroll_y, canvas->scroll_y + SCROLL_STEP);
                    DAMAGE_CANVAS(canvas);
                }
            }
            SCHEDULE_FRAME();
        }

        if (canvas->h_arrow_left_armed) {
            canvas->h_arrow_left_armed = false;
            DAMAGE_CANVAS(canvas);
            // Check if still on button
            if (event->y >= canvas->height - BORDER_HEIGHT_BOTTOM && event->y < canvas->height &&
                event->x >= canvas->width - BORDER_WIDTH_RIGHT - 42 &&
                event->x < canvas->width - BORDER_WIDTH_RIGHT - 22) {
                if (canvas->scroll_x > 0) {
                    canvas->scroll_x = max(0, canvas->scroll_x - SCROLL_STEP);
                    DAMAGE_CANVAS(canvas);
                }
            }
            SCHEDULE_FRAME();
        }

        if (canvas->h_arrow_right_armed) {
            canvas->h_arrow_right_armed = false;
            DAMAGE_CANVAS(canvas);
            // Check if still on button
            if (event->y >= canvas->height - BORDER_HEIGHT_BOTTOM && event->y < canvas->height &&
                event->x >= canvas->width - BORDER_WIDTH_RIGHT - 22 &&
                event->x < canvas->width - BORDER_WIDTH_RIGHT) {
                if (canvas->scroll_x < canvas->max_scroll_x) {
                    canvas->scroll_x = min(canvas->max_scroll_x, canvas->scroll_x + SCROLL_STEP);
                    DAMAGE_CANVAS(canvas);
                }
            }
            SCHEDULE_FRAME();
        }

        if (canvas->resize_armed) {
            canvas->resize_armed = false;
            DAMAGE_CANVAS(canvas);
            SCHEDULE_FRAME();
            // Resize already began on press, no action needed on release
        }

        if (canvas->close_armed) {
            canvas->close_armed = false;
            DAMAGE_CANVAS(canvas);
            SCHEDULE_FRAME();
            if (hit == HIT_CLOSE) {
                request_client_close(canvas);
                return;
            }
        }

        if (canvas->iconify_armed) {
            canvas->iconify_armed = false;
            DAMAGE_CANVAS(canvas);
            SCHEDULE_FRAME();
            if (hit == HIT_ICONIFY) {
                iconify_canvas(canvas);
                return;
            }
        }

        if (canvas->maximize_armed) {
            canvas->maximize_armed = false;
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
                return;
            }
        }

        if (canvas->lower_armed) {
            canvas->lower_armed = false;
            DAMAGE_CANVAS(canvas);
            SCHEDULE_FRAME();
            if (hit == HIT_LOWER) {
                itn_geometry_lower(canvas);
                canvas->active = false;
                itn_focus_activate_window_behind(canvas);
                // Let compositor handle stacking through ConfigureNotify events
                return;
            }
        }
    }
}

// ============================================================================
// Map Request/Notify Events
// ============================================================================

// Frame a client window, activate its frame, optionally map the client.
static void frame_and_activate(Window client, XWindowAttributes *attrs, bool map_client) {
    Canvas *frame = frame_client_window(client, attrs);
    if (!frame) {
        if (map_client) XMapWindow(display, client);
        return;
    }

    if (map_client) XMapWindow(display, client);
    set_active_window(frame);
    DAMAGE_CANVAS(frame);
    SCHEDULE_FRAME();
    XSync(display, False);
}

void intuition_handle_map_request(XMapRequestEvent *event) {
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
        }
        send_x_command_and_sync();
        return;
    }
    frame_and_activate(event->window, &attrs, true);
}

// Handle MapNotify for toplevel client windows that became viewable without a MapRequest
void intuition_handle_map_notify(XMapEvent *event) {
    // CRITICAL: Skip our own overlay window!
    extern Window itn_composite_get_overlay_window(void);
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
                fullscreen_active = True;
                menubar_apply_fullscreen(true);
            }
        } else {
            // Normal window - add frame decorations
            calculate_frame_size_from_client_size(event->width, event->height, &frame_changes.width, &frame_changes.height);

            // If we were fullscreen, exit fullscreen mode
            if (canvas->fullscreen) {
                canvas->fullscreen = false;
                fullscreen_active = False;
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

    // Only process if this is our frame window
    if (canvas->type == WINDOW || canvas->type == DIALOG) {
        itn_geometry_apply_resize(canvas, event->width, event->height);
    }
}

// Also need to implement the new itn_events versions that were in the stub
void itn_events_handle_configure(XConfigureEvent *event) {
    if (!event) return;

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

    Canvas *canvas = itn_canvas_find_by_window(event->window);
    if (!canvas) {
        canvas = itn_canvas_find_by_client(event->window);
    }
    if (!canvas) return;

    canvas->comp_mapped = true;
    canvas->comp_visible = true;

    // Setup compositing if needed
    if (!canvas->comp_damage && g_compositor_active) {
        itn_composite_setup_canvas(canvas);
    }

    // Damage entire window area
    DAMAGE_CANVAS(canvas);
    SCHEDULE_FRAME();
}

void itn_events_handle_unmap(XUnmapEvent *event) {
    if (!event) return;

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

    width = event->width;
    height = event->height;

    // Mark entire screen as damaged
    DAMAGE_RECT(0, 0, width, height);
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
            if (g_compositor_active && event->type == g_damage_event_base + XDamageNotify) {
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
        itn_focus_deactivate_all();
    }
}

// ============================================================================
// Event State Query Functions
// ============================================================================

bool itn_events_last_press_consumed(void) {
    return g_last_press_consumed;
}

bool itn_events_is_scrolling_active(void) {
    return scrolling_canvas != NULL;
}