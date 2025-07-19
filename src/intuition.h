/* Intuition header: Declares structures and functions for window management, canvas creation, activation, closing, and frame drawing. Core for UI handling. */

#ifndef INTUITION_H
#define INTUITION_H

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrender.h>
#include <X11/Xft/Xft.h>
#include "icons.h"
#include "config.h"

// Render context struct holds shared rendering data.
typedef struct RenderContext {
    Display *dpy; // Display connection.
    Visual *visual; // Visual for 32-bit color.
    XRenderPictFormat *fmt; // Picture format.
    XftFont *font; 
    XftColor label_color; 
    Colormap cmap; 
    Pixmap bg_pixmap; // Background pixmap reference.
    Canvas *active_canvas; 
} RenderContext;

// Forward declare Canvas.
typedef struct Canvas_struct Canvas;

// Canvas struct for windows/folders/desktop.
struct Canvas_struct {
    Window win; // Frame window.
    Pixmap backing; // Backing pixmap for double buffering.
    Picture back_pic; // Backing picture.
    Picture win_pic; // Window picture.
    FileIcon *icons; // Icons array.
    int num_icons; // Number of icons.
    int x, y; 
    int width, height; 
    XRenderColor bg_color; // Background color.
    bool active; // Active flag.
    int titlebar_height; 
    char *path; // Folder path.
    Window client_win; // Client window if managed.
    Picture client_pic; // Client picture if managed.
    Visual *client_visual; // Client visual if managed.
    char *title; // Window title.
};

// Function prototypes.
Window create_canvas_window(RenderContext *ctx, Window parent, int x, int y, int w, int h, XSetWindowAttributes *attrs); // Create window
void activate_canvas(RenderContext *ctx, Canvas *canvas, Canvas *folders, int num_folders); // Activate window
void close_canvas(RenderContext *ctx, Canvas *canvas, Canvas *folders, int *num_folders); // Close window
void iconify_canvas(RenderContext *ctx, Canvas *canvas, Canvas *desktop); // Iconify window
void draw_frame(RenderContext *ctx, Canvas *canvas, Picture pic); // Draw frame decorations

#endif