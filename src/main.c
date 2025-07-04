// main.c - entry point for the amiwb window manager
// Compile with: gcc -O2 -Wall -o amiwb main.c wm.c events.c icon_loader.c -lX11 -lX11-xcb -lXext -lXmu

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include "wm.h"
#include "events.h"
#include "icon_loader.h"

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
    XImage *img = NULL;
    if (load_do(dpy, root, gc, "../icons/xcalc", &img) != 0) {
        fprintf(stderr, "Failed to load xcalc.info icon\n");
    } else {
        // Keep the image displayed; it will be freed on display close
    }

    // Enter the main event loop
    event_loop();

    if (img) XDestroyImage(img);
    XCloseDisplay(dpy);
    return 0;
}
