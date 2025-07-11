#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include "events.h"
#include "icons.h"
#include "intuition.h"
#include "menus.h"
#include "render.h"
#include "decoration.h"

#define DOUBLE_CLICK_TIME 300
#define MOTION_THROTTLE_MS 8
#define MENUBAR_HEIGHT 20

// Local state
static Window dragging_window = 0;
static int dragged_icon_index = -1;
static int drag_start_x, drag_start_y, win_start_x, win_start_y;
static long last_click_time = 0, last_motion_time = 0;
static int click_count = 0;
static Bool menu_visible = False;
static int resizing = 0;
static Window resize_frame = 0, resize_button = 0, resize_client = 0;
static int resize_start_w, resize_start_h, last_button_x = -1, last_button_y = -1;

// Get time in milliseconds
static long get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Restack windows: menubar/menu at top, frames, icons at bottom
void restack_windows(Display *dpy) {
    Window *stack = malloc((num_desktop_icons + num_frames + 2) * sizeof(Window));
    int stack_count = 0;
    // Menubar and menu at top
    if (get_menubar_window()) stack[stack_count++] = get_menubar_window();
    if (get_menu_window()) stack[stack_count++] = get_menu_window();
    // Managed windows at order 2+
    for (int i = 0; i < num_frames; i++) {
        if (frames[i].frame.window) stack[stack_count++] = frames[i].frame.window;
    }
    // Desktop icons at bottom (stacking order 1)
    for (int i = 0; i < num_desktop_icons; i++) {
        if (desktop_icons[i].window) stack[stack_count++] = desktop_icons[i].window;
    }
    // Debug stacking order
    printf("[events] Restack: count=%d, order (top to bottom): ", stack_count);
    for (int i = 0; i < stack_count; i++) printf("0x%lx ", stack[i]);
    printf("\n");
    XRestackWindows(dpy, stack, stack_count);
    free(stack);
    XSync(dpy, False);
}

// Handle button press
static void handle_button_press(Display *dpy, XButtonEvent *e) {
    XPointer frame_data;

    // Menubar right-click
    if (e->window == get_menubar_window() && e->button == Button3) {
        if (e->y >= 0 && e->y <= MENUBAR_HEIGHT) {
            menu_visible = True;
            XMapWindow(dpy, get_menu_window());
            restack_windows(dpy);  // Ensure menu at top
            render_menu(dpy, -1);
            XGrabPointer(dpy, get_menu_window(), False, PointerMotionMask | ButtonReleaseMask,
                         GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
        }
        return;
    }

    // Menu window (handled in event_loop)
    if (e->window == get_menu_window()) return;

    // Lower button
    if (XFindContext(dpy, e->window, lower_context, &frame_data) == 0 && e->button == Button1) {
        Window frame = (Window)(uintptr_t)frame_data;
        if (frame == active_frame) {
            XSetWindowBackground(dpy, frame, 0xFF9F9F9F);
            XClearWindow(dpy, frame);
            active_frame = 0;
            set_active_frame(dpy, get_top_managed_window(dpy));
        }
        // Restack: menubar/menu, other frames, lowered frame at 2, icons at 1
        Window *stack = malloc((num_desktop_icons + num_frames + 2) * sizeof(Window));
        int stack_count = 0;
        if (get_menubar_window()) stack[stack_count++] = get_menubar_window();
        if (get_menu_window()) stack[stack_count++] = get_menu_window();
        for (int i = 0; i < num_frames; i++) {
            if (frames[i].frame.window && frames[i].frame.window != frame) {
                stack[stack_count++] = frames[i].frame.window;
            }
        }
        stack[stack_count++] = frame;  // Lowered frame at order 2
        for (int i = 0; i < num_desktop_icons; i++) {
            if (desktop_icons[i].window) stack[stack_count++] = desktop_icons[i].window;
        }
        printf("[events] Lower frame 0x%lx, stack: count=%d\n", frame, stack_count);
        XRestackWindows(dpy, stack, stack_count);
        free(stack);
        XSync(dpy, False);
        return;
    }

    // Iconify button
    if (XFindContext(dpy, e->window, iconify_context, &frame_data) == 0 && e->button == Button1) {
        Window frame = (Window)(uintptr_t)frame_data;
        if (XFindContext(dpy, frame, client_context, &frame_data) == 0) {
            XUnmapWindow(dpy, frame);
            add_icon(dpy, g_root, "/home/klaus/Sources/amiwb/icons", "xterm", 0,
                     &desktop_icons, &num_desktop_icons, desktop_label_color, 1, desktop_font);
            FileIcon *icon = &desktop_icons[num_desktop_icons - 1];
            XSaveContext(dpy, icon->window, iconified_frame_context, (XPointer)frame);
            restack_windows(dpy);  // Ensure new icon at bottom
            if (frame == active_frame) {
                active_frame = 0;
                set_active_frame(dpy, get_top_managed_window(dpy));
            }
        }
        return;
    }

    // Close button
    if (XFindContext(dpy, e->window, close_context, &frame_data) == 0 && e->button == Button1) {
        Window frame = (Window)(uintptr_t)frame_data;
        if (XFindContext(dpy, frame, client_context, &frame_data) == 0) {
            Window client = (Window)(uintptr_t)frame_data;
            XRemoveFromSaveSet(dpy, client);
            XKillClient(dpy, client);
            XSync(dpy, False);
        }
        return;
    }

    // Resize button
    if (XFindContext(dpy, e->window, resize_context, &frame_data) == 0 && e->button == Button1) {
        resize_frame = (Window)(uintptr_t)frame_data;
        resize_button = e->window;
        if (XFindContext(dpy, resize_frame, client_context, &frame_data) == 0) {
            resize_client = (Window)(uintptr_t)frame_data;
        }
        resizing = 1;
        drag_start_x = e->x_root;
        drag_start_y = e->y_root;
        for (int i = 0; i < num_frames; i++) {
            if (frames[i].frame.window == resize_frame) {
                XWindowAttributes attr;
                if (XGetWindowAttributes(dpy, frames[i].frame.window, &attr)) {
                    win_start_x = attr.x;
                    win_start_y = attr.y < MENUBAR_HEIGHT ? MENUBAR_HEIGHT : attr.y;
                    resize_start_w = attr.width;
                    resize_start_h = attr.height;
                    last_button_x = frames[i].resize_button.x;
                    last_button_y = frames[i].resize_button.y;
                }
                break;
            }
        }
        XGrabPointer(dpy, e->window, False, PointerMotionMask | ButtonReleaseMask,
                     GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
        last_motion_time = get_time_ms();
        return;
    }

    // Frame drag
    if (XFindContext(dpy, e->window, client_context, &frame_data) == 0 && e->button == Button1) {
        Window frame = e->window;
        if (frame != active_frame) set_active_frame(dpy, frame);
        drag_start_x = e->x_root;
        drag_start_y = e->y_root;
        for (int i = 0; i < num_frames; i++) {
            if (frames[i].frame.window == frame) {
                XWindowAttributes attr;
                if (XGetWindowAttributes(dpy, frame, &attr)) {
                    win_start_x = attr.x;
                    win_start_y = attr.y < MENUBAR_HEIGHT ? MENUBAR_HEIGHT : attr.y;
                    frames[i].frame.x = attr.x;
                    frames[i].frame.y = attr.y;
                }
                break;
            }
        }
        dragging_window = frame;
        XGrabPointer(dpy, frame, False, PointerMotionMask | ButtonReleaseMask,
                     GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
        last_motion_time = get_time_ms();
        return;
    }

    // Icon click
    for (int i = 0; i < num_desktop_icons; i++) {
        if (e->window == desktop_icons[i].window && e->button == Button1) {
            long current_time = get_time_ms();
            if (click_count == 0 || (current_time - last_click_time) > DOUBLE_CLICK_TIME) {
                click_count = 1;
                last_click_time = current_time;
                dragging_window = desktop_icons[i].window;
                dragged_icon_index = i;
                int dest_x, dest_y;
                Window child;
                XTranslateCoordinates(dpy, g_root, g_root, e->x_root, e->y_root, &dest_x, &dest_y, &child);
                drag_start_x = dest_x;
                drag_start_y = dest_y;
                XWindowAttributes attr;
                if (XGetWindowAttributes(dpy, desktop_icons[i].window, &attr)) {
                    win_start_x = attr.x;
                    win_start_y = attr.y < MENUBAR_HEIGHT ? MENUBAR_HEIGHT : attr.y;
                    desktop_icons[i].x = win_start_x;
                    desktop_icons[i].y = win_start_y;
                }
                restack_windows(dpy);  // Ensure dragged icon in order
                XGrabPointer(dpy, e->window, False, PointerMotionMask | ButtonReleaseMask,
                             GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
                last_motion_time = current_time;
            } else if (click_count == 1 && (current_time - last_click_time) <= DOUBLE_CLICK_TIME) {
                click_count = 0;
                last_click_time = 0;
                XPointer frame_data;
                if (XFindContext(dpy, e->window, iconified_frame_context, &frame_data) == 0) {
                    Window frame = (Window)(uintptr_t)frame_data;
                    XDeleteContext(dpy, e->window, iconified_frame_context);
                    XDestroyWindow(dpy, e->window);
                    free_icon(dpy, &desktop_icons[i]);
                    for (int j = i; j < num_desktop_icons - 1; j++) desktop_icons[j] = desktop_icons[j + 1];
                    num_desktop_icons--;
                    FileIcon *new_icons = realloc(desktop_icons, num_desktop_icons * sizeof(FileIcon));
                    if (new_icons || num_desktop_icons == 0) desktop_icons = new_icons;
                    XMapWindow(dpy, frame);
                    restack_windows(dpy);  // Ensure restored frame above icons
                    set_active_frame(dpy, frame);
                } else {
                    system("xterm &");
                }
            }
            break;
        }
    }
}

// Main event loop
void event_loop(Display *dpy) {
    XEvent ev, next_ev;
    while (1) {
        XNextEvent(dpy, &ev);
        switch (ev.type) {
            case MapRequest:
                intuition_handle_map_request(dpy, &ev.xmaprequest);
                break;
            case ConfigureRequest:
                intuition_handle_configure_request(dpy, &ev.xconfigurerequest);
                break;
            case ButtonPress:
                handle_button_press(dpy, &ev.xbutton);
                break;
            case ButtonRelease:
                if (menu_visible && ev.xbutton.window == get_menu_window()) {
                    if (ev.xbutton.button == Button1) {
                        menu_visible = False;
                        XUngrabPointer(dpy, CurrentTime);
                        if (!handle_menu_selection(dpy, ev.xbutton.x, ev.xbutton.y)) {
                            XUnmapWindow(dpy, get_menu_window());
                        }
                    } else if (ev.xbutton.button == Button3) {
                        menu_visible = False;
                        XUngrabPointer(dpy, CurrentTime);
                        XUnmapWindow(dpy, get_menu_window());
                    }
                } else if (dragging_window || resizing) {
                    if (dragged_icon_index >= 0) {
                        restack_windows(dpy);  // Ensure dragged icon in order
                    }
                    XUngrabPointer(dpy, CurrentTime);
                    dragging_window = 0;
                    dragged_icon_index = -1;
                    resizing = 0;
                    resize_frame = 0;
                    resize_client = 0;
                    resize_button = 0;
                    last_motion_time = 0;
                }
                break;
            case MotionNotify:
                if (menu_visible && ev.xmotion.window == get_menu_window()) {
                    while (XCheckTypedEvent(dpy, MotionNotify, &next_ev)) ev = next_ev;
                    int highlight_index = ev.xmotion.y / 20;
                    if (highlight_index >= 0 && highlight_index < 4 && ev.xmotion.x >= 0 && ev.xmotion.x < 120) {
                        render_menu(dpy, highlight_index);
                    } else {
                        render_menu(dpy, -1);
                    }
                } else if (dragging_window || resizing) {
                    while (XCheckTypedEvent(dpy, MotionNotify, &next_ev)) ev = next_ev;
                    long current_time = get_time_ms();
                    if (current_time - last_motion_time < MOTION_THROTTLE_MS) continue;
                    last_motion_time = current_time;
                    int dest_x, dest_y;
                    Window child;
                    XTranslateCoordinates(dpy, g_root, g_root, ev.xmotion.x_root, ev.xmotion.y_root, &dest_x, &dest_y, &child);
                    int dx = dest_x - drag_start_x, dy = dest_y - drag_start_y;
                    int new_x = win_start_x + dx, new_y = win_start_y + dy;
                    if (new_y < MENUBAR_HEIGHT) new_y = MENUBAR_HEIGHT;
                    if (dragging_window && !resizing) {
                        XMoveWindow(dpy, dragging_window, new_x, new_y);
                        if (dragged_icon_index >= 0) {
                            desktop_icons[dragged_icon_index].x = new_x;
                            desktop_icons[dragged_icon_index].y = new_y;
                        }
                    } else if (resizing && resize_frame && resize_client) {
                        int new_w = resize_start_w + dx, new_h = resize_start_h + dy;
                        for (int i = 0; i < num_frames; i++) {
                            if (frames[i].frame.window == resize_frame) {
                                XWindowAttributes attr;
                                if (XGetWindowAttributes(dpy, resize_frame, &attr)) {
                                    update_frame_decorations(dpy, &frames[i], new_w, new_h);
                                    frames[i].frame.x = attr.x;
                                    frames[i].frame.y = attr.y < MENUBAR_HEIGHT ? MENUBAR_HEIGHT : attr.y;
                                }
                                break;
                            }
                        }
                    }
                }
                break;
            case Expose:
                if (ev.xexpose.window == get_menubar_window()) {
                    render_menubar(dpy);
                } else if (ev.xexpose.window == get_menu_window() && menu_visible) {
                    render_menu(dpy, -1);
                } else {
                    for (int i = 0; i < num_desktop_icons; i++) {
                        if (ev.xexpose.window == desktop_icons[i].window && dragged_icon_index != i) {
                            render_icon(dpy, &desktop_icons[i]);
                        }
                    }
                }
                break;
            case DestroyNotify:
                XPointer frame_data;
                if (XFindContext(dpy, ev.xdestroywindow.window, frame_context, &frame_data) == 0) {
                    Window frame = (Window)(uintptr_t)frame_data;
                    XRemoveFromSaveSet(dpy, ev.xdestroywindow.window);
                    XDestroyWindow(dpy, frame);
                    XDeleteContext(dpy, ev.xdestroywindow.window, frame_context);
                    XDeleteContext(dpy, frame, client_context);
                    XDeleteContext(dpy, frame, close_context);
                    XDeleteContext(dpy, frame, iconify_context);
                    XDeleteContext(dpy, frame, lower_context);
                    for (int i = 0; i < num_frames; i++) {
                        if (frames[i].frame.window == frame) {
                            for (int j = i; j < num_frames - 1; j++) frames[j] = frames[j + 1];
                            num_frames--;
                            break;
                        }
                    }
                    if (frame == active_frame) {
                        active_frame = 0;
                        set_active_frame(dpy, get_top_managed_window(dpy));
                    }
                    restack_windows(dpy);  // Ensure order after destruction
                    XSync(dpy, False);
                }
                break;
        }
        XFlush(dpy);
    }
}

// Clean icons
void clean_icons(Display *dpy) {
    const int icon_spacing = 74, max_per_column = 6;
    for (int i = 0; i < num_desktop_icons; i++) {
        int column = i / max_per_column, row = i % max_per_column;
        int new_x = column * icon_spacing + 10, new_y = row * icon_spacing + 40;
        desktop_icons[i].x = new_x;
        desktop_icons[i].y = new_y;
        XMoveWindow(dpy, desktop_icons[i].window, new_x, new_y);
    }
    restack_windows(dpy);  // Ensure icons at bottom
    XFlush(dpy);
}

// Quit window manager
void quit_amiwb(Display *dpy) {
    for (int i = 0; i < num_desktop_icons; i++) {
        free_icon(dpy, &desktop_icons[i]);
    }
    free(desktop_icons);
    cleanup_menus(dpy);
    extern char *def_tool_path, *def_drawer_path, *desktop_font_name;
    if (def_tool_path) free(def_tool_path);
    if (def_drawer_path) free(def_drawer_path);
    if (desktop_font_name) free(desktop_font_name);
    if (desktop_font) XFreeFont(dpy, desktop_font);
    XCloseDisplay(dpy);
    exit(0);
}
