#include "events.h"
#include "wm.h"
#include "icon_loader.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define DOUBLE_CLICK_TIME 300 // ms

static Window dragging_window = 0;
static int drag_start_x = 0;
static int drag_start_y = 0;
static int win_start_x = 0;
static int win_start_y = 0;
static long last_click_time = 0;
static int click_count = 0;
extern Icon global_icon;

long get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void handle_button_press(Display *dpy, XButtonEvent *e) {
    Window root = DefaultRootWindow(dpy);
    printf("ButtonPress on window %lu, global_icon.window %lu, root %lu\n", e->window, global_icon.window, root);
    if (e->window == global_icon.window && e->button == Button1) {
        long current_time = get_time_ms();
        if (click_count == 0 || (current_time - last_click_time) > DOUBLE_CLICK_TIME) {
            click_count = 1;
            last_click_time = current_time;
            printf("Single click on icon at (%d, %d)\n", e->x, e->y);
        } else if (click_count == 1 && (current_time - last_click_time) <= DOUBLE_CLICK_TIME) {
            click_count = 0;
            printf("Double click on icon at (%d, %d), launching xterm\n", e->x, e->y);
            system("xterm &"); // Launch xterm in background
        }
        XLowerWindow(dpy, global_icon.window); // Ensure icon stays at bottom
    } else if (e->window == root && e->button == Button1) {
        printf("Clicked desktop at (%d, %d)\n", e->x, e->y);
    } else {
        dragging_window = e->window;
        drag_start_x = e->x_root;
        drag_start_y = e->y_root;
        Window dummy;
        unsigned int w, h, bw, depth;
        XGetGeometry(dpy, dragging_window, &dummy, &win_start_x, &win_start_y, &w, &h, &bw, &depth);
    }
}

void event_loop(void) {
    XEvent ev;
    Display *dpy = wm_get_display();
    if (!dpy) {
        fprintf(stderr, "Cannot reopen display.\n");
        exit(1);
    }
    printf("global_icon.window initialized as %lu\n", global_icon.window);
    for (;;) {
        XNextEvent(dpy, &ev);
        if (ev.type == MapRequest) {
            wm_handle_map_request(&ev);
        } else if (ev.type == ConfigureRequest) {
            wm_handle_configure_request(&ev);
        } else if (ev.type == ButtonPress) {
            handle_button_press(dpy, &ev.xbutton);
        } else if (ev.type == MotionNotify && dragging_window != 0) {
            XMotionEvent *e = &ev.xmotion;
            int dx = e->x_root - drag_start_x;
            int dy = e->y_root - drag_start_y;
            XMoveWindow(dpy, dragging_window, win_start_x + dx, win_start_y + dy);
        } else if (ev.type == ButtonRelease && dragging_window != 0) {
            dragging_window = 0;
        } else if (ev.type == DestroyNotify) {
            XPointer data;
            if (XFindContext(dpy, ev.xdestroywindow.window, frame_context, &data) == 0) {
                Window frame = (Window)(uintptr_t)data;
                XDestroyWindow(dpy, frame);
                XDeleteContext(dpy, ev.xdestroywindow.window, frame_context);
            }
        } else if (ev.type == Expose && ev.xexpose.window == global_icon.window) {
            if (global_icon.image) {
                XPutImage(dpy, global_icon.window, DefaultGC(dpy, DefaultScreen(dpy)),
                          global_icon.image, 0, 0, 0, 0, global_icon.width, global_icon.height);
            }
        }
    }
}

