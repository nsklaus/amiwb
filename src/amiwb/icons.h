// File: icons.h
#ifndef ICONS_H
#define ICONS_H

#include "intuition/itn_public.h"

typedef enum IconType { TYPE_FILE, TYPE_DRAWER, TYPE_ICONIFIED, TYPE_DEVICE } IconType;

typedef struct {
    char *label;                // Icon label (filename or custom)
    char *path;                 // File/directory path
    IconType type;              // Icon type: TOOL, DRAWER, ICONIFIED
    int x, y;                   // Position on canvas
    int width, height;          // Normal icon dimensions
    int sel_width, sel_height;  // Selected icon dimensions (may differ from normal)
    int label_width;            // Cached label text width for layout
    bool selected;              // Selection state
    Picture normal_picture;     // Normal state picture
    Picture selected_picture;   // Selected state picture
    Picture current_picture;    // Current displayed picture
    Window display_window;      // Window ID of display canvas (desktop or window)
    Time last_click_time;       // Timestamp of last click for double-click detection
    Canvas *iconified_canvas;    // Pointer to the iconified canvas (for TYPE_ICONIFIED)
} FileIcon;

// Function prototypes
void create_icon_images( FileIcon *icon, RenderContext *ctx); // Load normal and selected pictures for icon
void free_icon(  FileIcon *icon); // Free icon pictures

#endif