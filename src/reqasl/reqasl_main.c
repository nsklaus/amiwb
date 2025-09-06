// ReqASL standalone file requester
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <X11/Xlib.h>
#include "reqasl.h"

static bool files_selected = false;

static void on_file_open(const char *path) {
    printf("%s\n", path);  // Output selected path to stdout
    files_selected = true;  // Mark that we got at least one file
}

static void on_cancel(void) {
    // Don't need to do anything - main will exit with code 1
}

int main(int argc, char *argv[]) {
    Display *display = XOpenDisplay(NULL);
    if (!display) {
        log_error("[ERROR] Cannot open display");
        return 1;
    }
    
    // Create ReqASL dialog
    ReqASL *req = reqasl_create(display);
    if (!req) {
        log_error("[ERROR] Failed to create ReqASL");
        XCloseDisplay(display);
        return 1;
    }
    
    // Parse command line arguments
    const char *initial_path = NULL;
    const char *title = NULL;  // Will be set based on mode if not provided
    const char *mode = "open";
    const char *pattern = NULL;  // File extensions filter (e.g. "avi,mp4,mkv")
    bool called_by_app = false;  // Detect if called by another app
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--path") == 0 && i + 1 < argc) {
            initial_path = argv[++i];
        } else if (strcmp(argv[i], "--title") == 0 && i + 1 < argc) {
            title = argv[++i];
        } else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            mode = argv[++i];
            called_by_app = true;  // --mode flag indicates app is calling
        } else if (strcmp(argv[i], "--pattern") == 0 && i + 1 < argc) {
            pattern = argv[++i];  // e.g. "avi,mp4,mkv"
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [options]\n", argv[0]);
            printf("Options:\n");
            printf("  --path PATH       Initial directory path\n");
            printf("  --title TITLE     Window title\n");
            printf("  --mode MODE       Mode (open/save)\n");
            printf("  --pattern EXTS    File extensions filter (e.g. \"avi,mp4,mkv\")\n");
            printf("  --help            Show this help\n");
            printf("\nReqASL File Requester - Part of AmiWB\n");
            reqasl_destroy(req);
            XCloseDisplay(display);
            return 0;
        }
    }
    
    // Only set callbacks if called by another app
    // In standalone mode, reqasl.c will use xdg-open instead
    if (called_by_app) {
        reqasl_set_callbacks(req, on_file_open, on_cancel, NULL);
    }
    
    // Set pattern filter if provided
    if (pattern) {
        reqasl_set_pattern(req, pattern);
    }
    
    // Set mode (this also configures multi-selection)
    reqasl_set_mode(req, (strcmp(mode, "save") == 0));
    
    // Set window title based on arguments
    if (title) {
        // Use provided title
        reqasl_set_title(req, title);
    } else if (called_by_app || pattern) {
        // Called by app or with pattern filter - implies file selection
        if (req->is_save_mode) {
            reqasl_set_title(req, "Save File");
        } else {
            reqasl_set_title(req, "Open File");
        }
    } else {
        // Standalone mode without patterns - just browsing
        reqasl_set_title(req, "ReqASL");
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
    
    // Return 0 if files were selected, 1 if cancelled
    return files_selected ? 0 : 1;
}