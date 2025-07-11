#ifndef DECORATION_H
#define DECORATION_H

#include <X11/Xlib.h>
#include "intuition.h"

// Update frame decorations
void update_frame_decorations(Display *dpy, FrameInfo *frame, int new_w, int new_h);

#endif
