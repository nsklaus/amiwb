// ReqASL standalone file requester
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include "reqasl.h"

static void on_file_open(const char *path) {
    printf("%s\n", path);  // Output selected path to stdout
    exit(0);
}

static void on_cancel(void) {
    exit(1);  // Exit with error code on cancel
}

int main(int argc, char *argv[]) {
    Display *display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "Cannot open display\n");
        return 1;
    }
    
    // Create ReqASL dialog
    ReqASL *req = reqasl_create(display);
    if (!req) {
        fprintf(stderr, "Failed to create ReqASL\n");
        XCloseDisplay(display);
        return 1;
    }
    
    // Set callbacks
    reqasl_set_callbacks(req, on_file_open, on_cancel, NULL);
    
    // Parse command line arguments
    const char *initial_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--path") == 0 && i + 1 < argc) {
            initial_path = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [--path /initial/path]\n", argv[0]);
            printf("ReqASL File Requester - Part of WBDE\n");
            reqasl_destroy(req);
            XCloseDisplay(display);
            return 0;
        }
    }
    
    // Show dialog
    reqasl_show(req, initial_path);
    
    // Event loop
    XEvent event;
    while (req->is_open) {
        XNextEvent(display, &event);
        reqasl_handle_event(req, &event);
    }
    
    // Cleanup
    reqasl_destroy(req);
    XCloseDisplay(display);
    
    return 0;
}