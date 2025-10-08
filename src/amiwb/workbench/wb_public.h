#ifndef WB_PUBLIC_H
#define WB_PUBLIC_H

// Public API for workbench icon management, file operations, and layout
// This replaces the old monolithic workbench.h

#include "../intuition/itn_public.h"
#include "../icons/icon_public.h"

#define INITIAL_ICON_CAPACITY 16

// Function prototypes
void init_workbench(void);                                  // Init icon storage and desktop scan
void cleanup_workbench(void);                               // Free icons and workbench state
void clear_canvas_icons(Canvas *canvas);                    // Remove all icons for a canvas
void create_icon(const char *path, Canvas *canvas, int x, int y); // Create an icon at x,y
void create_icon_with_type(const char *path, Canvas *canvas, int x, int y, int type); // Create icon with explicit type
FileIcon* wb_icons_create_with_icon_path(const char *icon_path, Canvas *canvas, int x, int y,
                                          const char *full_path, const char *name, int type); // Create icon with explicit .info path and metadata
const char *definfo_for_file(const char *name, bool is_dir); // Get def_icon path for file extension
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
void workbench_cleanup_drag_state(void);                    // Clean up drag state after XDND

// Rearrange icons on a canvas into a tidy grid
void icon_cleanup(Canvas *canvas);

// Compute content bounds from icons on canvas
void wb_layout_compute_bounds(Canvas *canvas);

// Remove any icon associated with an iconified canvas (e.g., on destroy)
void remove_icon_for_canvas(Canvas *canvas);

// View mode/layout helpers
void wb_layout_apply_view(Canvas *canvas);              // Layout per canvas->view_mode
void set_canvas_view_mode(Canvas *canvas, ViewMode m);  // Set mode, relayout, and redraw

// Directory refresh: scan dir and rebuild icons, then redraw
void refresh_canvas_from_directory(Canvas *canvas, const char *dirpath);

// File operations
void open_file(FileIcon *icon);                 // Open file with xdg-open
FileIcon* create_iconified_icon(Canvas *c);     // Create icon for iconified window
void workbench_open_directory(const char *path); // Open directory in new workbench window
void workbench_create_new_drawer(Canvas *target_canvas); // Create new drawer with icon (no re-layout)

// Launch programs with ReqASL hook
void launch_with_hook(const char *command);     // Execute command with LD_PRELOAD set

// Directory operations
int remove_directory_recursive(const char *path); // Recursively remove directory and contents

// Directory size calculation (non-blocking via fork)
typedef void (*size_callback_t)(off_t size, void *userdata);
pid_t calculate_directory_size(const char *path, int *pipe_fd); // Start size calculation, returns child PID and pipe FD
off_t read_directory_size_result(int pipe_fd);                 // Read result from pipe when ready

// Progress monitor polling (called from event loop)
void workbench_check_progress_monitors(void);

// Icon information dialog (opaque type)
typedef struct IconInfoDialog IconInfoDialog;

// Show icon information dialog
void show_icon_info_dialog(FileIcon *icon);

// Icon info event handlers (called from events.c)
bool iconinfo_handle_key_press(XKeyEvent *event);
bool iconinfo_handle_button_press(XButtonEvent *event);
bool iconinfo_handle_button_release(XButtonEvent *event);
bool iconinfo_handle_motion(XMotionEvent *event);

// Icon info query functions
bool is_iconinfo_canvas(Canvas *canvas);
IconInfoDialog* get_iconinfo_for_canvas(Canvas *canvas);

// Icon info rendering (called from render.c)
void render_iconinfo_content(Canvas *canvas);

// Icon info cleanup
void close_icon_info_dialog(IconInfoDialog *dialog);
void close_icon_info_dialog_by_canvas(Canvas *canvas);
void cleanup_all_iconinfo_dialogs(void);

// Icon info initialization
void init_iconinfo(void);
void cleanup_iconinfo(void);

// Icon info process monitoring for directory size calculation
void iconinfo_check_size_calculations(void);

// Cache invalidation for performance optimization
void invalidate_pointer_cache(void);

// Spatial mode control
bool get_spatial_mode(void);
void set_spatial_mode(bool mode);

// Global show hidden state control
bool get_global_show_hidden_state(void);
void set_global_show_hidden_state(bool show);

// Global view mode control
ViewMode get_global_view_mode(void);
void set_global_view_mode(ViewMode mode);

// Archive extraction
int extract_file_at_path(const char *archive_path, Canvas *canvas);

// Drag operations (from wb_drag.c)
void start_drag_icon(FileIcon *icon, int x, int y);
void continue_drag_icon(XMotionEvent *event, Canvas *canvas);
void end_drag_icon(Canvas *canvas);

// Directory operations (from wb_core.c)
void open_directory(FileIcon *icon, Canvas *current_canvas);

#endif // WB_PUBLIC_H
