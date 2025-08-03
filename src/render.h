// File: render.h
#ifndef RENDER_H
#define RENDER_H

#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>
#include "intuition.h"
#include "workbench.h"

void init_render(void); 			// initialize rendering resources
void cleanup_render(void);			// clean up rendering resources
void redraw_canvas(Canvas *canvas); // redraw entire canvas and its icons
void redraw_menu(Canvas *canvas);	// redraw a menu
void render_icon(FileIcon *icon, Canvas *canvas); 	// render a single icon

#endif