#ifndef RENDER_PUBLIC_H
#define RENDER_PUBLIC_H

#include "config.h"

// Canvas rendering functions
// These are the core rendering functions from render.c

// Redraw a canvas (handles both workbench and intuition windows)
void redraw_canvas(Canvas *canvas);

// Recreate canvas rendering surfaces (called after resize)
void render_recreate_canvas_surfaces(Canvas *canvas);

#endif // RENDER_PUBLIC_H
