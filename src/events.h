#ifndef EVENTS_H        // prevent multiple inclusions of this header file to avoid redefinition errors
#define EVENTS_H        // define the EVENTS_H macro to mark this header as included

#include <X11/Xlib.h>   // Xlib header for X11 display and window management functions

// declare the main event loop function that processes X11 events
void event_loop(void);

// declare function to handle mouse button press events, takes display and event as parameters
void handle_button_press(Display *dpy, XButtonEvent *e);

// end the header guard
#endif
