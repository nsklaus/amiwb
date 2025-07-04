#ifndef EVENTS_H
#define EVENTS_H

#include <X11/Xlib.h>

void event_loop(void);
void handle_button_press(Display *dpy, XButtonEvent *e);

#endif