#ifndef INTUITION_H
#define INTUITION_H

#include <X11/Xlib.h>
#include <X11/Xutil.h>  // Added for XContext
#include <X11/Xresource.h>
#include "icons.h"

// Window information
typedef struct {
    Window window;      // X11 window ID
    int x, y;           // Position
    int width, height;  // Dimensions
} WindowInfo;

// Frame information
typedef struct {
    WindowInfo frame;        // Frame window
    WindowInfo client;       // Client window
    WindowInfo resize_button; // Resize button
    WindowInfo close_button;  // Close button
    WindowInfo iconify_button;// Iconify button
    WindowInfo lower_button;  // Lower button
} FrameInfo;

// Global variables from intuition.c
extern Display *g_dpy;
extern Window g_root;
extern Window active_frame;
extern XContext frame_context, client_context, resize_context, close_context, iconify_context, lower_context, iconified_frame_context;
extern FrameInfo frames[];
extern int num_frames;

// Initialize window manager
void intuition_init(Display *dpy, Window root);

// Handle map request
void intuition_handle_map_request(Display *dpy, XMapRequestEvent *e);

// Handle configure request
void intuition_handle_configure_request(Display *dpy, XConfigureRequestEvent *e);

// Scan existing windows
void scan_existing_windows(Display *dpy);

// Set active frame
void set_active_frame(Display *dpy, Window frame);

// Get top managed window
Window get_top_managed_window(Display *dpy);

#endif
