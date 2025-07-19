/* Render header: Declares functions for redrawing canvases, compositing rects, setting wallpapers. For rendering operations. */

#ifndef RENDER_H
#define RENDER_H

#include "intuition.h"

// Function prototypes for rendering.
void redraw_canvas(RenderContext *ctx, Canvas *canvas, XRectangle *dirty); // Redraw canvas, optional dirty rect
void composite_rect(RenderContext *ctx, Canvas *canvas, int x, int y, int w, int h); // Composite rect to window
void set_wallpaper(RenderContext *ctx, const char *path); // Set root wallpaper from JPEG

#endif