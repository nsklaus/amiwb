// main.c - entry point for the amiwb window manager

#include <X11/Xlib.h>    // Includes Xlib for X11 functions
#include <X11/Xutil.h>   // Includes Xutil for utility functions
#include <stdio.h>       // Includes standard I/O functions
#include <stdlib.h>      // Includes standard library functions
#include <stdint.h>      // Includes standard integer types
#include "wm.h"          // Includes window manager declarations
#include "events.h"      // Includes event handling declarations
#include "icon_loader.h" // Includes icon loading declarations

Icon global_icon = {0};  // Defines and initializes global icon struct (zeroed)

int main() {

    // Opens connection to X11 display
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "Cannot open display\n");
        exit(1);
    }

    // Gets root window ID for display
    Window root = DefaultRootWindow(dpy);

    // Sets root window background to black
    XSetWindowBackground(dpy, root, BlackPixel(dpy, DefaultScreen(dpy)));

    // Clears root window to show background
    XClearWindow(dpy, root);

    // initialize window manager
    wm_init(dpy, root);

    // frame existing windows at startup
    wm_scan_existing_windows();

    // get default graphics context for drawing
    GC gc = DefaultGC(dpy, DefaultScreen(dpy));

    // load and displays xcalc icon, checks for failure
    if (load_do(dpy, root, gc, "../icons/def_drawer", &global_icon) != 0) {
        fprintf(stderr, "Failed to load xcalc.info icon\n");
    }

    // run the main event loop to process X11 events
    event_loop();

    // =======
    // cleanup
    // =======
    // Frees icon image if it exists
    if (global_icon.image) XDestroyImage(global_icon.image);
    // Destroys icon window if it exists
    if (global_icon.window) XDestroyWindow(dpy, global_icon.window);
    XCloseDisplay(dpy);
    return 0;
}
