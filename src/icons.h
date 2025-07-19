/* Icons header: Defines icon structures and functions for loading, freeing, and recreating icons. Used for file/folder representations. */

#ifndef ICONS_H
#define ICONS_H

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrender.h>
#include <X11/Xft/Xft.h>

// Forward declares.
typedef struct RenderContext RenderContext;
typedef struct Canvas_struct Canvas;

// Base icon struct.
typedef struct {
    XImage *image; 
    int width; 
    int height; 
} Icon;

// File icon struct.
typedef struct {
    char *filename; 
    char *path; // Full path.
    char *label; // Full label.
    char *display_label; // Truncated display label.
    Icon icon; // Base icon.
    int x, y; 
    int width, height; 
    int type; 
    Pixmap pixmap; 
    Picture picture; 
    Canvas *iconified_canvas; 
    char *icon_path;
} FileIcon;

// Function prototypes.
int load_icon(Display *dpy, const char *name, Icon *icon); // Load base icon
void free_icon(Display *dpy, FileIcon *icon); // Free icon
void recreate_icon_pixmap(RenderContext *ctx, FileIcon *icon, Canvas *canvas); // Recreate icon pixmap and picture

#endif