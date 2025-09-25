/* main.c - Application entry point */

/*
 * What could be added later:
 * - Command line argument parsing
 * - Signal handling
 * - Multi-window support
 * - Session management
 * - Crash recovery
 */

#include <stdio.h>
#include <X11/Xlib.h>
#include "config.h"
#include "skeleton.h"
#include "font_manager.h"
#include "logging.h"
#include "events.h"

int main(int argc, char *argv[]) {
    /* Initialize logging first */
    log_init();
    log_message("Starting %s", APP_NAME);

    /* Open X display */
    Display *display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "ERROR: Cannot open X display\n");
        return 1;
    }

    /* Initialize font system */
    if (!font_init(display)) {
        fprintf(stderr, "ERROR: Cannot initialize fonts\n");
        XCloseDisplay(display);
        return 1;
    }

    /* Create application */
    SkeletonApp *app = skeleton_create(display);
    if (!app) {
        font_cleanup();
        XCloseDisplay(display);
        return 1;
    }

    /* Main event loop */
    XEvent event;
    int running = 1;
    while (running) {
        XNextEvent(display, &event);
        running = events_dispatch(app, &event);
    }

    /* Cleanup */
    skeleton_destroy(app);
    font_cleanup();
    XCloseDisplay(display);
    log_message("Application terminated normally");

    return 0;
}