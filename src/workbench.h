// File: workbench.h
#ifndef WORKBENCH_H
#define WORKBENCH_H

#include "intuition.h"
#include "icons.h"

#define INITIAL_ICON_CAPACITY 16

// Function prototypes
void init_workbench(void);                                  // Initialize icon array
void cleanup_workbench(void);                               // Clean up icon array
void clear_canvas_icons(Canvas *canvas);                    // Remove icons for a given canvas
void create_icon(const char *path, Canvas *canvas, int x, int y); // Create new icon
void destroy_icon(FileIcon *icon);                          // Destroy an icon
FileIcon *find_icon(Window win, int x, int y);              // Find icon by position
void move_icon(FileIcon *icon, int x, int y);               // Move icon to new position
int get_icon_count(void);                                   // Get current icon count
FileIcon **get_icon_array(void);                             // Get icon array

void workbench_handle_button_press(XButtonEvent *event);    // Handle button press for icons
void workbench_handle_button_release(XButtonEvent *event);  // Handle button release for icons
void workbench_handle_motion_notify(XMotionEvent *event);   // Handle motion for icon dragging

// Rearrange icons on the given canvas in a sorted grid layout
void icon_cleanup(Canvas *canvas);

// Add after existing prototypes
void compute_content_bounds(Canvas *canvas);  // Compute content bounds from icons on canvas

// Remove any icon associated with an iconified canvas (e.g., on destroy)
void remove_icon_for_canvas(Canvas *canvas);

#endif