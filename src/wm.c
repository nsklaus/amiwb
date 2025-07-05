// wm.c - WM logic, init, frame window, scan existing ..

#include "wm.h"             //  window manager declarations (functions, frame_context)
#include <X11/Xlib.h>       //  Xlib for core X11 functions
#include <X11/Xutil.h>      //  Xutil for utility functions (XGetWindowAttributes,..)
#include <X11/Xresource.h>  //  Xresource for resource management (XrmUniqueQuark,..)
#include <stdio.h>          //  standard I/O functions (fprintf,..)
#include <stdlib.h>         //  standard library functions (exit,..)
#include <string.h>         //  string functions (memset,..)
#include "icon_loader.h"    //  icon loading declarations (Icon struct,..)

// declare global icon struct from main.c
extern Icon global_icon;

// define context to store frame-client window mappings
XContext frame_context;

// store the X11 display connection
static Display *dpy;

// store the root window ID
static Window root;

// function to return the display pointer
Display *wm_get_display(void) { return dpy; }

// initialize the window manager with display and root window
void wm_init(Display *display, Window root_window) {

    // store display pointer
    dpy = display;

    // store root window ID
    root = root_window;

    // set custom error handler for X11 errors
    XSetErrorHandler(wm_error_handler);

    // select events on root window: redirects child events, notifies changes, captures clicks
    XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask | ButtonPressMask);

    // flushe X11 requests to ensure events are set
    XSync(dpy, False);

    // generate unique ID for frame context
    frame_context = XrmUniqueQuark();

    printf("amiwb is running.\n");
}

// handle X11 errors (e.g., another WM running)
int wm_error_handler(Display *d, XErrorEvent *e) {

    fprintf(stderr, "Another WM is already running.\n");
    exit(1);
    return 0;
}

// handle requests to map (show) windows
void wm_handle_map_request(XEvent *event) {

    // declare map request event pointer
    XMapRequestEvent *e;

    // declare struct for window attributes
    XWindowAttributes attr;

    // declare frame window ID
    Window frame;

    // declare border width for frame
    int border;

    // declare frame position and size
    int frame_x, frame_y, frame_w, frame_h;

    // cast event to map request type
    e = &event->xmaprequest;

    // get attributes of the client window
    XGetWindowAttributes(dpy, e->window, &attr);

    // check if the window is the icon, if yes skip framing
    if (e->window == global_icon.window) {

        // set icon window to receive click and expose events
        XSelectInput(dpy, e->window, ButtonPressMask | ExposureMask);

        // map (shows) the icon window
        XMapWindow(dpy, e->window);

        // ensure icon stays at lowest stacking order
        XLowerWindow(dpy, e->window);

        // exit function to skip framing
        return;
    }

    // set frame border width
    border = 2;

    // set frame position from client window
    frame_x = attr.x;
    frame_y = attr.y;

    // set frame size (client size + borders)
    frame_w = attr.width + border * 4;
    frame_h = attr.height + border * 4;

    // create frame window with black border, white background
    frame = XCreateSimpleWindow(dpy, root, frame_x, frame_y, frame_w, frame_h, border,
                                BlackPixel(dpy, DefaultScreen(dpy)),
                                WhitePixel(dpy, DefaultScreen(dpy)));

    // add client window to saveset to prevent it from closing
    XAddToSaveSet(dpy, e->window);

    // reparent client window into frame, offset by border
    XReparentWindow(dpy, e->window, frame, border, border);

    // select structure change events for client window
    XSelectInput(dpy, e->window, StructureNotifyMask);

    // store frame-client mapping in context
    XSaveContext(dpy, e->window, frame_context, (XPointer)frame);

    // select click and motion events for frame window
    XSelectInput(dpy, frame, ButtonPressMask | ButtonReleaseMask | PointerMotionMask);

    // map (show) the frame window
    XMapWindow(dpy, frame);

    // map (show) the client window
    XMapWindow(dpy, e->window);

}

// handle requests to configure (resize/move) windows
void wm_handle_configure_request(XEvent *event) {

    // declare configure request event pointer
    XConfigureRequestEvent *e;

    // declare struct for window changes
    XWindowChanges changes;

    // cast event to configure request type
    e = &event->xconfigurerequest;

    // set new position from request
    changes.x = e->x;
    changes.y = e->y;

    // set new size from request
    changes.width = e->width;
    changes.height = e->height;

    // set new border width from request
    changes.border_width = e->border_width;

    // set stacking sibling from request
    changes.sibling = e->above;

    // set stacking mode from request
    changes.stack_mode = e->detail;

    // apply requested changes to window
    XConfigureWindow(dpy, e->window, e->value_mask, &changes);

}

// scan and frames existing windows at startup
void wm_scan_existing_windows(void) {

    // declare variables for root and parent window IDs
    Window root_ret, parent_ret;

    // declare array for child windows
    Window *children;

    // declare loop counter and child count
    unsigned int i, n;

    // query child windows of root
    if (XQueryTree(dpy, root, &root_ret, &parent_ret, &children, &n)) {

        // loop through each child window
        for (i = 0; i < n; ++i) {

            // declare struct for window attributes
            XWindowAttributes attr;

            // declare fake event for mapping
            XEvent fake;

            // get attributes of child window
            XGetWindowAttributes(dpy, children[i], &attr);

            // check if window is viewable, not override-redirect, and not the icon
            if (!attr.override_redirect && attr.map_state == IsViewable && children[i] != global_icon.window) {

                // clear fake event struct
                memset(&fake, 0, sizeof(fake));

                // set fake event type to MapRequest
                fake.xmaprequest.type = MapRequest;

                // set display for fake event
                fake.xmaprequest.display = dpy;

                // set parent to root for fake event
                fake.xmaprequest.parent = root;

                // set window ID for fake event
                fake.xmaprequest.window = children[i];

                // frame the existing window
                wm_handle_map_request(&fake);

            }
        }

        // check if children array exists
        if (children) {

            // free the children array
            XFree(children);

        }
    }
}
