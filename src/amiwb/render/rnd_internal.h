// File: rnd_internal.h
// Internal API for render system modules (rnd_* only)
// NOT for external subsystems - use rnd_public.h instead

#ifndef RND_INTERNAL_H
#define RND_INTERNAL_H

#include "rnd_public.h"
#include "../config.h"
#include <X11/extensions/Xrender.h>

// ============================================================================
// Internal Helper Functions (used by multiple rnd_* modules)
// ============================================================================

// Widget drawing (used by rnd_canvas.c for frame rendering)
void draw_vertical_scrollbar_arrows(Display *dpy, Picture dest, Canvas *canvas);
void draw_horizontal_scrollbar_arrows(Display *dpy, Picture dest, Canvas *canvas);
void draw_resize_button(Display *dpy, Picture dest, Canvas *canvas);
void create_checkerboard_pattern(RenderContext *ctx);
void draw_checkerboard(Display *dpy, Picture dest, int x, int y, int w, int h, XRenderColor color1, XRenderColor color2);

#endif // RND_INTERNAL_H
