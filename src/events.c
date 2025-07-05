// events.c - event loop logic

#include "events.h"         // event handling declarations (event_loop, handle_button_press)
#include "wm.h"             // window manager declarations (wm_get_display, frame_context)
#include "icon_loader.h"    // icon loading declarations (Icon struct, load_do)
#include <X11/Xlib.h>       // Xlib for core X11 functions (windows, events)
#include <X11/Xutil.h>      // Xutil for utility functions (XGetGeometry)
#include <stdint.h>         // standard integer types (uint8_t)
#include <stdio.h>          // standard I/O functions ( printf)
#include <stdlib.h>         // standard library functions ( exit, malloc,..)
#include <time.h>           // time functions for click timing (clock_gettime)
#include <string.h>         // string functions (memset)


// define the maximum time in ms between clicks to detect a double-click
#define DOUBLE_CLICK_TIME 300

// track the window currently being dragged (0 if none)
static Window dragging_window = 0;

// store the x-coordinate of the mouse when dragging starts
static int drag_start_x = 0;

// store the y-coordinate of the mouse when dragging starts
static int drag_start_y = 0;

// store the initial x-coordinate of the window being dragged
static int win_start_x = 0;

// store the initial y-coordinate of the window being dragged
static int win_start_y = 0;

// track the timestamp of the last click for double-click detection
static long last_click_time = 0;

// count clicks for double-click detection (0 or 1)
static int click_count = 0;

// declare the global icon struct (defined in main.c) for icon properties
extern Icon global_icon;

// define function to get current time in milliseconds
long get_time_ms(void) {

    // declare timespec struct to hold time data
    struct timespec ts;

    // get monotonic clock time (unaffected by system time changes)
    clock_gettime(CLOCK_MONOTONIC, &ts);

    // convert seconds and nanoseconds to milliseconds and returns
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

}

// handle mouse button press events, takes display and event as parameters
void handle_button_press(Display *dpy, XButtonEvent *e) {

    // get the root window ID for the display
    Window root = DefaultRootWindow(dpy);

    // print debug info: clicked window, icon window, and root window IDs
    printf("ButtonPress on window %lu, global_icon.window %lu, root %lu\n", e->window, global_icon.window, root);

    // check if the click is on the icon window with left mouse button
    if (e->window == global_icon.window && e->button == Button1) {
        long current_time = get_time_ms();

        // check if first click or time since last click exceeds double-click threshold
        if (click_count == 0 || (current_time - last_click_time) > DOUBLE_CLICK_TIME) {

            // set click count to 1 (first click)
            click_count = 1;

            // update last click timestamp
            last_click_time = current_time;

            // print single-click coordinates
            printf("Single click on icon at (%d, %d)\n", e->x, e->y);

            // set the icon as the window being dragged, start dragging icon
            dragging_window = global_icon.window;

            // store mouse coords at drag start
            drag_start_x = e->x_root;
            drag_start_y = e->y_root;

            // store icon’s initial position
            win_start_x = global_icon.x;
            win_start_y = global_icon.y;

            // grab pointer to capture motion and release events for icon
            XGrabPointer(dpy, global_icon.window, False, PointerMotionMask | ButtonReleaseMask,
                         GrabModeAsync, GrabModeAsync, None, None, CurrentTime);

        // if second click within double-click threshold
        } else if (click_count == 1 && (current_time - last_click_time) <= DOUBLE_CLICK_TIME) {

            // reset click count
            click_count = 0;

            // print double-click coordinates and action (multi-line for readability)
            printf("Double click on icon at (%d, %d), launching xterm\n", e->x, e->y);

            // launch xterm in the background
            system("xterm &");

            // if icon was being dragged
            if (dragging_window == global_icon.window) {

                // release pointer grab
                XUngrabPointer(dpy, CurrentTime);

                // top dragging
                dragging_window = 0;
            }
        }

        // ensure icon stays at lowest stacking order
        XLowerWindow(dpy, global_icon.window);

    // check if click is on root window with left button
    } else if (e->window == root && e->button == Button1) {

        // print desktop click coordinates
        printf("Clicked desktop at (%d, %d)\n", e->x, e->y);
    } else {

        // set clicked window as dragging target
        dragging_window = e->window;

        // store mouse coords at drag start
        drag_start_x = e->x_root;
        drag_start_y = e->y_root;

        // unused variable for XGetGeometry return
        Window dummy;

        // variables for window width, height, border width, depth
        unsigned int w, h, bw, depth;

        // get geometry of dragging window (sets win_start_x, win_start_y)
        XGetGeometry(dpy, dragging_window, &dummy, &win_start_x, &win_start_y, &w, &h, &bw, &depth);

        // grab pointer for motion and release events on the window
        XGrabPointer(dpy, dragging_window, False, PointerMotionMask | ButtonReleaseMask,
                     GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
    }
}

// main event loop to process all X11 events
void event_loop(void) {

    // declare XEvent struct to hold event data
    XEvent ev;

    // get display pointer from window manager
    Display *dpy = wm_get_display();

    if (!dpy) {
        fprintf(stderr, "Cannot reopen display.\n");
        exit(1);
    }

    // print icon window ID for debugging
    printf("global_icon.window initialized as %lu\n", global_icon.window);

    // infinite loop to process events
    for (;;) {

        // wait for and retrieves the next X11 event
        XNextEvent(dpy, &ev);

        // handle window map requests
        if (ev.type == MapRequest) {

            // call WM function to frame and map new windows
            wm_handle_map_request(&ev);

        // handle window configuration requests
        } else if (ev.type == ConfigureRequest) {

            // call WM function to adjust window properties
            wm_handle_configure_request(&ev);

        // handle mouse button press events
        } else if (ev.type == ButtonPress) {

            // call function to process button press
            handle_button_press(dpy, &ev.xbutton);

        // handle mouse motion during dragging
        } else if (ev.type == MotionNotify && dragging_window != 0) {

            // cast event to motion event
            XMotionEvent *e = &ev.xmotion;

            // calculate distance moved
            int dx = e->x_root - drag_start_x;
            int dy = e->y_root - drag_start_y;

            // move window to new position
            XMoveWindow(dpy, dragging_window, win_start_x + dx, win_start_y + dy);

            // if dragging the icon
            if (dragging_window == global_icon.window) {

                // update icon’s position
                global_icon.x = win_start_x + dx;
                global_icon.y = win_start_y + dy;
            }

        // handle mouse button release during dragging
        } else if (ev.type == ButtonRelease && dragging_window != 0) {

            // print window ID of release event
            printf("ButtonRelease on window %lu\n", ev.xbutton.window);

            // output icon coords while being dragged.. too spammy
            //printf("Icon moved to (%d, %d)\n", global_icon.x, global_icon.y);

            // release pointer grab
            XUngrabPointer(dpy, CurrentTime);

            // reset dragging state
            dragging_window = 0;

        // handle window destruction events
        } else if (ev.type == DestroyNotify) {

            // pointer to store context data
            XPointer data;

            // check if destroyed window has a frame in context
            if (XFindContext(dpy, ev.xdestroywindow.window, frame_context, &data) == 0) {

                // get frame window from context
                Window frame = (Window)(uintptr_t)data;

                // destroy the frame window
                XDestroyWindow(dpy, frame);

                // remove window from context
                XDeleteContext(dpy, ev.xdestroywindow.window, frame_context);
            }

        // handle expose events for icon window
        } else if (ev.type == Expose && ev.xexpose.window == global_icon.window) {

            // check if icon image exists
            if (global_icon.image) {

                // redraw icon image on expose
                XPutImage(dpy, global_icon.window, DefaultGC(dpy, DefaultScreen(dpy)),
                          global_icon.image, 0, 0, 0, 0, global_icon.width, global_icon.height);
            }
        }
    }
}
