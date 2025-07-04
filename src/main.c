// main.c - entry point for the amiwb window manager
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "wm.h"
#include "events.h"
#include "icon_loader.h"

Icon global_icon = {0};

int main() {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "Cannot open display\n");
        exit(1);
    }

    Window root = DefaultRootWindow(dpy);
    XSetWindowBackground(dpy, root, BlackPixel(dpy, DefaultScreen(dpy)));
    XClearWindow(dpy, root);

    // Initialize window manager
    wm_init(dpy, root);
    wm_scan_existing_windows();

    // Load and display xcalc.info icon
    GC gc = DefaultGC(dpy, DefaultScreen(dpy));
    if (load_do(dpy, root, gc, "../icons/xcalc", &global_icon) != 0) {
        fprintf(stderr, "Failed to load xcalc.info icon\n");
    }

    // Enter the main event loop
    event_loop();

    if (global_icon.image) XDestroyImage(global_icon.image);
    if (global_icon.window) XDestroyWindow(dpy, global_icon.window);
    XCloseDisplay(dpy);
    return 0;
}
