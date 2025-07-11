#ifndef ICONS_H
#define ICONS_H

#include <X11/Xlib.h>

// Icon image data
typedef struct {
    XImage *image;  // X11 image for icon pixels
    int width;      // Icon width in pixels
    int height;     // Icon height in pixels
} Icon;

// File icon data for desktop and file manager
typedef struct {
    char *filename;     // File or directory name
    char *path;         // Directory path
    char *label;        // Display label
    Icon icon;          // Icon image data
    Window window;      // X11 window for rendering
    int x, y;           // Position relative to parent
    int width, height;  // Window dimensions
    int type;           // 0=file, 1=dir, 2=.info
    GC gc;              // Graphics context
    Pixmap shape_mask;  // Shape mask for transparency
} FileIcon;

// Global default icon paths
extern char *def_tool_path;
extern char *def_drawer_path;

// Load icon from .info file
int load_icon(Display *dpy, const char *name, Icon *icon);

// Add icon to array (desktop or workbench)
void add_icon(Display *dpy, Window parent, const char *path, const char *filename, int is_dir,
              FileIcon **icons, int *num_icons, unsigned long label_color, int is_desktop,
              XFontStruct *font);

// Scan directory for icons
void scan_icons(Display *dpy, Window parent, const char *path, FileIcon **icons,
                int *num_icons, unsigned long label_color, int is_desktop, XFontStruct *font);

// Free icon resources
void free_icon(Display *dpy, FileIcon *icon);

#endif
