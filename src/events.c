#include "events.h"
#include "wm.h"
#include <X11/Xlib.h>
#include <stdint.h>   // for uintptr_t
#include <stdio.h>
#include <stdlib.h>

//static Display *dpy = NULL;
static Window dragging_window = 0;
static int drag_start_x = 0;
static int drag_start_y = 0;
static int win_start_x = 0;
static int win_start_y = 0;

void event_loop(void) {
    XEvent ev;

    //dpy = XOpenDisplay(NULL);
    //dpy = wm_get_display();
    Display *dpy = wm_get_display();
    if (dpy == NULL) {
        fprintf(stderr, "Cannot reopen display.\n");
        exit(1);
    }

    for (;;) {
        XNextEvent(dpy, &ev);

        if (ev.type == MapRequest) {
            wm_handle_map_request(&ev);
        } else if (ev.type == ConfigureRequest) {
            wm_handle_configure_request(&ev);
        } else if (ev.type == ButtonPress) {
            XButtonEvent *e = &ev.xbutton;
            Window dummy;
            unsigned int w, h, bw, depth;

            dragging_window = e->window;
            drag_start_x = e->x_root;
            drag_start_y = e->y_root;

            XGetGeometry(dpy, dragging_window, &dummy, &win_start_x, &win_start_y, &w, &h, &bw, &depth);
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


                // Get geometry of the frame before destroying
                // Window dummy;
                // int x, y;
                // unsigned int w, h, bw, d;
                // XGetGeometry(dpy, frame, &dummy, &x, &y, &w, &h, &bw, &d);

                // XDestroyWindow(dpy, frame);
                // XClearArea(dpy, DefaultRootWindow(dpy), x, y, w, h, True); // ← True = generate Expose
                // XFlush(dpy); // Ensure it happens immediately
                // XDeleteContext(dpy, ev.xdestroywindow.window, frame_context);
                // XClearWindow(dpy, DefaultRootWindow(dpy));

                // clear background
                // GC gc;
                // XGCValues gcv;
                // gcv.foreground = BlackPixel(dpy, DefaultScreen(dpy));
                // gc = XCreateGC(dpy, DefaultRootWindow(dpy), GCForeground, &gcv);
                // XFillRectangle(dpy, DefaultRootWindow(dpy), gc, x, y, w, h);
                // XFreeGC(dpy, gc);

                // Clear the area manually on root
                // XClearArea(dpy, DefaultRootWindow(dpy), x, y, w, h, False);
            }
        }
    }
}
