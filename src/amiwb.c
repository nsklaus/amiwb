// gcc -O2 -Wall -o amiwb amiwb.c -lX11
// gcc -O2 -Wall -o amiwb amiwb.c `pkg-config --cflags --libs x11`

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Globals for dragging state ---
Window dragging_window = 0;
int drag_start_x = 0, drag_start_y = 0;
int win_start_x = 0, win_start_y = 0;

// --- Error handler: abort if another WM is running ---
int wm_error_handler(Display *d, XErrorEvent *e) {
    fprintf(stderr, "Another WM is already running.\n");
    exit(1);
    return 0;
}

// --- Global X11 state ---
Display *dpy;
Window root;

// --- Frame and map new windows ---
void handle_map_request(XEvent *ev) {
    XMapRequestEvent *e = &ev->xmaprequest;

    // Get original window attributes
    XWindowAttributes attr;
    XGetWindowAttributes(dpy, e->window, &attr);

    // Set border size and calculate frame dimensions
    int border = 2;
    int frame_x = attr.x;
    int frame_y = attr.y;
    int frame_w = attr.width + border * 4;
    int frame_h = attr.height + border * 4;

    // Create a frame window around the client window
    Window frame = XCreateSimpleWindow(dpy, root,
        frame_x, frame_y, frame_w, frame_h, border,
        BlackPixel(dpy, DefaultScreen(dpy)),
        WhitePixel(dpy, DefaultScreen(dpy)));

    // Reparent the client window into the frame
    XAddToSaveSet(dpy, e->window);
    XReparentWindow(dpy, e->window, frame, border, border);

    // Listen for mouse events on the frame
    XSelectInput(dpy, frame, ButtonPressMask | ButtonReleaseMask | PointerMotionMask);

    // Map both frame and client
    XMapWindow(dpy, frame);
    XMapWindow(dpy, e->window);
}

int main() {
    // Open display
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "Cannot open display\n");
        exit(1);
    }

    // Get the root window
    root = DefaultRootWindow(dpy);

    // Become the window manager
    XSetErrorHandler(wm_error_handler);
    XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask);

    // Ensure error handler runs before entering event loop
    XSync(dpy, False);
    printf("ami+ is running.\n");

    // --- Frame all existing top-level windows ---
    Window root_ret, parent_ret;
    Window *children;
    unsigned int n;

    if (XQueryTree(dpy, root, &root_ret, &parent_ret, &children, &n)) {
        for (unsigned int i = 0; i < n; ++i) {
            XWindowAttributes attr;
            XGetWindowAttributes(dpy, children[i], &attr);

            // Only manage normal top-level windows
            if (!attr.override_redirect && attr.map_state == IsViewable) {
                XEvent fake;
                memset(&fake, 0, sizeof(fake));
                fake.xmaprequest.type = MapRequest;
                fake.xmaprequest.display = dpy;
                fake.xmaprequest.parent = root;
                fake.xmaprequest.window = children[i];
                handle_map_request(&fake);
            }
        }
        if (children) XFree(children);
    }

    // --- Main Event Loop ---
    for (;;) {
        XEvent ev;
        XNextEvent(dpy, &ev);

        if (ev.type == MapRequest)
            handle_map_request(&ev);

        else if (ev.type == ConfigureRequest) {
            XConfigureRequestEvent *e = &ev.xconfigurerequest;
            XWindowChanges changes = {
                .x = e->x, .y = e->y, .width = e->width, .height = e->height,
                .border_width = e->border_width, .sibling = e->above, .stack_mode = e->detail
            };
            XConfigureWindow(dpy, e->window, e->value_mask, &changes);
        }

        else if (ev.type == ButtonPress) {
            XButtonEvent *e = &ev.xbutton;

            // Start dragging
            dragging_window = e->window;
            drag_start_x = e->x_root;
            drag_start_y = e->y_root;

            // Get initial window position
            Window root_ret;
            unsigned int w, h, bw, depth;
            XGetGeometry(dpy, dragging_window, &root_ret, &win_start_x, &win_start_y,
                         &w, &h, &bw, &depth);
        }

        else if (ev.type == MotionNotify && dragging_window) {
            XMotionEvent *e = &ev.xmotion;

            // Calculate new position
            int dx = e->x_root - drag_start_x;
            int dy = e->y_root - drag_start_y;

            XMoveWindow(dpy, dragging_window, win_start_x + dx, win_start_y + dy);
        }

        else if (ev.type == ButtonRelease && dragging_window) {
            dragging_window = 0;
        }
    }

    return 0;
}
