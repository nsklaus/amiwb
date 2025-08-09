// File: render.h
#ifndef RENDER_H
#define RENDER_H

#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>
#include <X11/Xft/Xft.h>
#include "intuition.h"
#include "workbench.h"

void init_render(void); 			// initialize rendering resources
void cleanup_render(void);			// clean up rendering resources
void redraw_canvas(Canvas *canvas); // redraw entire canvas and its icons
void redraw_menu(Canvas *canvas);	// redraw a menu
void render_icon(FileIcon *icon, Canvas *canvas); 	// render a single icon
int get_text_width(const char *text);  // Compute pixel width of text from font
XftFont *get_font(void);               // Accessor for the global UI font

// Deduplicated helpers to manage XRender/Xlib resources for a canvas
// Recreate pixmap and Pictures matching current canvas size/visual
void render_recreate_canvas_surfaces(Canvas *canvas);
// Destroy pixmap and Pictures (safe to call if already NULL)
void render_destroy_canvas_surfaces(Canvas *canvas);
#endif