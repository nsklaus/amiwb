#include "wm.h"
#include <X11/Xutil.h>
#include <X11/Xresource.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//static XContext frame_context;
XContext frame_context;
static Display *dpy;
static Window root;

Display *wm_get_display(void) {
    return dpy;
}

void wm_init(Display *display, Window root_window) {
    dpy = display;
    root = root_window;

    XSetErrorHandler(wm_error_handler);
    XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask);
    XSync(dpy, False);
    frame_context = XUniqueContext();

    printf("amiwb is running.\n");
}

int wm_error_handler(Display *d, XErrorEvent *e) {
    fprintf(stderr, "Another WM is already running.\n");
    exit(1);
    return 0;
}

void wm_handle_map_request(XEvent *event) {
    XMapRequestEvent *e;
    XWindowAttributes attr;
    Window frame;
    int border;
    int frame_x, frame_y, frame_w, frame_h;

    e = &event->xmaprequest;
    XGetWindowAttributes(dpy, e->window, &attr);

    border = 2;
    frame_x = attr.x;
    frame_y = attr.y;
    frame_w = attr.width + border * 4;
    frame_h = attr.height + border * 4;

    frame = XCreateSimpleWindow(dpy, root, frame_x, frame_y, frame_w, frame_h, border,
                                BlackPixel(dpy, DefaultScreen(dpy)),
                                WhitePixel(dpy, DefaultScreen(dpy)));

    XAddToSaveSet(dpy, e->window);
    XReparentWindow(dpy, e->window, frame, border, border);
    XSelectInput(dpy, e->window, StructureNotifyMask);
    XSaveContext(dpy, e->window, frame_context, (XPointer)(uintptr_t)frame);
    XSelectInput(dpy, frame, ButtonPressMask | ButtonReleaseMask | PointerMotionMask);
    XMapWindow(dpy, frame);
    XMapWindow(dpy, e->window);
}

void wm_handle_configure_request(XEvent *event) {
    XConfigureRequestEvent *e;
    XWindowChanges changes;

    e = &event->xconfigurerequest;

    changes.x = e->x;
    changes.y = e->y;
    changes.width = e->width;
    changes.height = e->height;
    changes.border_width = e->border_width;
    changes.sibling = e->above;
    changes.stack_mode = e->detail;

    XConfigureWindow(dpy, e->window, e->value_mask, &changes);
}

void wm_scan_existing_windows(void) {
    Window root_ret, parent_ret;
    Window *children;
    unsigned int i, n;

    if (XQueryTree(dpy, root, &root_ret, &parent_ret, &children, &n)) {
        for (i = 0; i < n; ++i) {
            XWindowAttributes attr;
            XEvent fake;

            XGetWindowAttributes(dpy, children[i], &attr);
            if (!attr.override_redirect && attr.map_state == IsViewable) {
                memset(&fake, 0, sizeof(fake));
                fake.xmaprequest.type = MapRequest;
                fake.xmaprequest.display = dpy;
                fake.xmaprequest.parent = root;
                fake.xmaprequest.window = children[i];
                wm_handle_map_request(&fake);
            }
        }
        if (children) {
            XFree(children);
        }
    }
}
