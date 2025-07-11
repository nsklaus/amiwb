#ifndef EVENTS_H
#define EVENTS_H

#include <X11/Xlib.h>
#include "icons.h"

extern FileIcon *desktop_icons;
extern int num_desktop_icons;
extern unsigned long desktop_label_color;
extern XFontStruct *desktop_font;

void event_loop(Display *dpy);
void clean_icons(Display *dpy);
void quit_amiwb(Display *dpy);
void restack_windows(Display *dpy);

#endif
