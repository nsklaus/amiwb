// File: render.h
// Rendering helpers for canvases and icons.
// Uses XRender for compositing and Xft for text.
// Keeps a single global font and wallpaper data in RenderContext.
#ifndef RENDER_H
#define RENDER_H

#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>
#include <X11/Xft/Xft.h>
#include "intuition.h"
#include "icons.h"

void init_render(void);               // Initialize render ctx, fonts, wallpapers
void cleanup_render(void);            // Free render resources safely
void redraw_canvas(Canvas *canvas);   // Redraw full canvas contents
void redraw_menu(Canvas *canvas);     // Redraw a menu canvas
void render_icon(FileIcon *icon, Canvas *canvas); // Draw one icon
int get_text_width(const char *text); // Width in pixels of a UTF-8 string
XftFont *get_font(void);              // Access the global UI font

// Canvas surface lifecycle
// Recreate pixmap and Pictures for current size/visual.
void render_recreate_canvas_surfaces(Canvas *canvas);
// Destroy pixmap and Pictures (safe if already NULL).
void render_destroy_canvas_surfaces(Canvas *canvas);

// Load or reload wallpapers (desktop/window) into RenderContext.
// Uses screen size and config (DESKPICT/WINDPICT, tiling flags).
void render_load_wallpapers(void);
#endif