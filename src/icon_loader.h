#ifndef ICON_LOADER_H
#define ICON_LOADER_H

#include <X11/Xlib.h>

int load_do(Display *dpy, Window root, GC gc, const char *name, XImage **out_img);

#endif
