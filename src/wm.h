
#ifndef WM_H            // prevent multiple inclusions of this header
#define WM_H            // define the WM_H macro

#include <X11/Xlib.h>   // Xlib for X11 types
#include <X11/Xutil.h>  // Xutil for utility types

// declare global frame context for window-frame mappings
extern XContext frame_context;

// declare function to initialize window manager
void wm_init(Display *display, Window root_window);

// declare function to frame existing windows
void wm_scan_existing_windows(void);

// declare function to handle window map requests
void wm_handle_map_request(XEvent *event);

// declare function to handle window configure requests
void wm_handle_configure_request(XEvent *event);

// declare function to handle X11 errors
int wm_error_handler(Display *d, XErrorEvent *e);

// declare function to get display pointer
Display *wm_get_display(void);

// end the header guard
#endif

