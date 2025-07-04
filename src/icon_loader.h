#ifndef ICON_LOADER_H
#define ICON_LOADER_H

#include <X11/Xlib.h>

typedef struct {
    Window window;
    int x, y;
    int width, height;
    XImage *image;
} Icon;

int load_do(Display *dpy, Window root, GC gc, const char *name, Icon *icon);

#endif