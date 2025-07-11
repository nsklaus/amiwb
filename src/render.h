#ifndef RENDER_H
#define RENDER_H

#include <X11/Xlib.h>
#include "icons.h"

// Render menubar
void render_menubar(Display *dpy);

// Render menu
void render_menu(Display *dpy, int highlight_index);

// Render icon
void render_icon(Display *dpy, FileIcon *icon);

#endif
