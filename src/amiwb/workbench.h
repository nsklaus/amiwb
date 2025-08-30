// File: workbench.h
// Workbench icon management, selection, and drag-and-drop.
// Exposes helpers to create/destroy icons, layout, and refresh views.
#ifndef WORKBENCH_H
#define WORKBENCH_H

#include "intuition.h"
#include "icons.h"

#define INITIAL_ICON_CAPACITY 16

// Function prototypes
void init_workbench(void);                                  // Init icon storage and desktop scan
void cleanup_workbench(void);                               // Free icons and workbench state
void clear_canvas_icons(Canvas *canvas);                    // Remove all icons for a canvas
void create_icon(const char *path, Canvas *canvas, int x, int y); // Create an icon at x,y
void destroy_icon(FileIcon *icon);                          // Free icon resources and pictures
FileIcon *find_icon(Window win, int x, int y);              // Hit-test an icon by position
void move_icon(FileIcon *icon, int x, int y);               // Move icon to new position
int get_icon_count(void);                                   // Total icons across canvases
FileIcon **get_icon_array(void);                            // Raw icon array (for drawing)
FileIcon *get_selected_icon(void);                             // Get currently selected icon (or NULL)
FileIcon *get_selected_icon_from_canvas(Canvas *canvas);       // Get selected icon from specific canvas

// Event hooks used by central dispatcher
void workbench_handle_button_press(XButtonEvent *event);    // Icon selection/drag start
void workbench_handle_button_release(XButtonEvent *event);  // Drag/drop end and clicks
void workbench_handle_motion_notify(XMotionEvent *event);   // Drag update and hover

// Rearrange icons on a canvas into a tidy grid
void icon_cleanup(Canvas *canvas);

// Compute content bounds from icons on canvas
void compute_content_bounds(Canvas *canvas);

// Remove any icon associated with an iconified canvas (e.g., on destroy)
void remove_icon_for_canvas(Canvas *canvas);

// View mode/layout helpers
void apply_view_layout(Canvas *canvas);                 // Layout per canvas->view_mode
void set_canvas_view_mode(Canvas *canvas, ViewMode m);  // Set mode, relayout, and redraw

// Directory refresh: scan dir and rebuild icons, then redraw
void refresh_canvas_from_directory(Canvas *canvas, const char *dirpath);

// File operations
void open_file(FileIcon *icon);                 // Open file with xdg-open
void restore_iconified(FileIcon *icon);         // Restore iconified window
void workbench_open_directory(const char *path); // Open directory in new workbench window

// Launch programs with ReqASL hook
void launch_with_hook(const char *command);     // Execute command with LD_PRELOAD set

// Directory operations
int remove_directory_recursive(const char *path); // Recursively remove directory and contents

// Progress dialog monitoring (called from event loop)
void workbench_check_progress_dialogs(void);

#endif