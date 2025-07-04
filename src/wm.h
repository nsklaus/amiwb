#ifndef WM_H
#define WM_H

#include <X11/Xlib.h>
#include <X11/Xutil.h>
extern XContext frame_context;

void wm_init(Display *display, Window root_window);
void wm_scan_existing_windows(void);
void wm_handle_map_request(XEvent *event);
void wm_handle_configure_request(XEvent *event);
int wm_error_handler(Display *d, XErrorEvent *e);
Display *wm_get_display(void);

#endif
