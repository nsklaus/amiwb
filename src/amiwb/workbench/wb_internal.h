#ifndef WB_INTERNAL_H
#define WB_INTERNAL_H

#include "../icons.h"
#include "../intuition/itn_public.h"
#include <X11/Xlib.h>
#include <stdbool.h>

// Internal workbench module communication API
// This header is used by workbench modules to call each other
// External code should use ../workbench.h instead

// ============================================================================
// Type Definitions (shared across modules)
// ============================================================================

// File operation types for progress system
typedef enum {
    FILE_OP_COPY,
    FILE_OP_MOVE,
    FILE_OP_DELETE
} FileOperation;

// ============================================================================
// wb_deficons.c - Default Icons System
// ============================================================================

// Load all def_*.info files from system and user directories
void wb_deficons_load(void);

// Get deficon path for a file (returns NULL if no match)
const char *wb_deficons_get_for_file(const char *filename, bool is_dir);

// ============================================================================
// wb_icons_array.c - Icon Array Management
// ============================================================================

// Legacy function names (TODO: migrate to wb_icons_array_* naming)
FileIcon **get_icon_array(void);  // Use wb_icons_array_get()
int get_icon_count(void);         // Use wb_icons_array_count()

// Get pointer to global icon array
FileIcon **wb_icons_array_get(void);

// Get count of icons in array
int wb_icons_array_count(void);

// Add or remove icon from global array
void wb_icons_array_manage(FileIcon *icon, bool add);

// Get currently selected icon (any canvas)
FileIcon *wb_icons_array_get_selected(void);

// Get selected icon from specific canvas
FileIcon *wb_icons_array_get_selected_from_canvas(Canvas *canvas);

// Get most recently added icon
FileIcon *wb_icons_array_get_last_added(void);

// Get icons for specific canvas
FileIcon **wb_icons_for_canvas(Canvas *canvas, int *out_count);

// ============================================================================
// wb_icons_create.c - Icon Creation and Destruction
// ============================================================================

// Basic icon creation
FileIcon *wb_icons_create(const char *path, const char *label, int type);

// Icon creation with explicit type
FileIcon *wb_icons_create_with_type(const char *path, const char *label,
                                     int type, const char *deficon_path);

// Icon creation with full metadata
FileIcon *wb_icons_create_with_metadata(const char *path, const char *label,
                                         int type, int x, int y, Canvas *canvas);

// Destroy icon and free resources
void wb_icons_destroy(FileIcon *icon);

// Create icon images (load .info file, create pixmaps)
void wb_icons_create_images(FileIcon *icon);

// Create icon for iconified window
FileIcon *wb_icons_create_iconified(Window client_window, const char *title, Canvas *canvas);

// Remove all icons from specific canvas
void wb_icons_remove_for_canvas(Canvas *canvas);

// Add prime desktop icons (RAM:, DH0:, etc)
void wb_icons_add_prime_desktop(Canvas *desktop);

// Legacy function names (TODO: migrate to wb_icons_* naming)
FileIcon *create_icon_with_metadata(const char *icon_path, Canvas *canvas, int x, int y,
                                     const char *path, const char *label, int file_type);

// ============================================================================
// wb_icons_ops.c - Icon Operations
// ============================================================================

// Find icon by path and/or canvas
FileIcon *wb_icons_find(const char *path, Canvas *canvas);

// Move icon to new position
void wb_icons_move(FileIcon *icon, int x, int y);

// Set icon metadata (position, canvas)
void wb_icons_set_meta(FileIcon *icon, int x, int y);

// Restore iconified window from icon
void wb_icons_restore_iconified(FileIcon *icon);
void restore_iconified(FileIcon *icon);  // Legacy alias

// ============================================================================
// wb_fileops.c - File Operations
// ============================================================================

// Count files for progress
void count_files_in_directory(const char *path, int *count);
void count_files_and_bytes(const char *path, int *file_count, off_t *total_bytes);

// Copy file (basic)
int wb_fileops_copy(const char *src, const char *dst);

// Move file to directory (returns 0 on success)
int wb_fileops_move(const char *src_path, const char *dst_dir,
                    char *dst_path, size_t dst_sz);

// Move file with icon creation metadata
int wb_fileops_move_ex(const char *src_path, const char *dst_dir,
                       char *dst_path, size_t dst_sz,
                       Canvas *target_canvas, int icon_x, int icon_y);

// Remove directory recursively
int wb_fileops_remove_recursive(const char *path);

// Check if path is directory
bool wb_fileops_is_directory(const char *path);

// Check if file exists
bool wb_fileops_check_exists(const char *path);

// ============================================================================
// wb_progress.c - Progress Dialog System
// ============================================================================

// Perform file operation with automatic progress dialog
int wb_progress_file_operation(FileOperation op, const char *src_path,
                                const char *dst_path, const char *custom_title);

// Extended version with icon creation metadata
int wb_progress_file_operation_ex(FileOperation op, const char *src_path,
                                   const char *dst_path, const char *custom_title,
                                   void *icon_metadata);

// Legacy function names (TODO: rename implementation)
int perform_file_operation_with_progress(FileOperation op, const char *src_path,
                                          const char *dst_path, const char *custom_title);
int perform_file_operation_with_progress_ex(FileOperation op, const char *src_path,
                                             const char *dst_path, const char *custom_title,
                                             void *icon_metadata);

// ============================================================================
// wb_drag.c - Drag and Drop
// ============================================================================

// Start dragging icon
void wb_drag_start(FileIcon *icon, int x, int y, Canvas *source);

// Continue drag (update position)
void wb_drag_continue(int root_x, int root_y);

// End drag (perform drop)
void wb_drag_end(int root_x, int root_y);

// Get source canvas of current drag
Canvas *wb_drag_get_source_canvas(void);

// Check if drag is currently active
bool wb_drag_is_active(void);

// Set drag inactive
void wb_drag_set_inactive(void);

// Get saved source window
Window wb_drag_get_saved_window(void);

// Get currently dragged icon
FileIcon *wb_drag_get_dragged_icon(void);

// Clear dragged icon reference
void wb_drag_clear_dragged_icon(void);

// Cleanup drag window
void wb_drag_cleanup_window(void);

// Cleanup drag state (called on errors or external drops)
void wb_drag_cleanup_state(void);

// Refresh canvas display
void refresh_canvas(Canvas *canvas);

// ============================================================================
// wb_layout.c - View Modes and Layout
// ============================================================================

// Legacy function names (TODO: migrate to wb_layout_* naming)
void compute_content_bounds(Canvas *canvas);
void apply_view_layout(Canvas *canvas);
void find_free_slot(Canvas *canvas, int *out_x, int *out_y);

// Apply current view mode to canvas
void wb_layout_apply_view(Canvas *canvas);

// Set canvas view mode
void wb_layout_set_view_mode(Canvas *canvas, int mode);

// Find next free slot for icon
void wb_layout_find_free_slot(Canvas *canvas, int *x, int *y);

// Compute content bounds for canvas
void wb_layout_compute_bounds(Canvas *canvas);

// Icon cleanup/arrangement
void wb_layout_icon_cleanup(Canvas *canvas);

// Spatial mode getters/setters
bool wb_layout_get_spatial_mode(void);
void wb_layout_set_spatial_mode(bool mode);

// Show hidden files getters/setters
bool wb_layout_get_show_hidden(void);
void wb_layout_set_show_hidden(bool show);

// ============================================================================
// wb_canvas.c - Canvas Operations
// ============================================================================

// Refresh canvas from directory
void wb_canvas_refresh_from_dir(Canvas *canvas, const char *dirpath);

// Clear all icons from canvas
void wb_canvas_clear_icons(Canvas *canvas);

// ============================================================================
// wb_archive.c - Archive Extraction
// ============================================================================

// Check if file is supported archive
bool wb_archive_is_supported(const char *path);

// Extract archive to canvas directory
int wb_archive_extract(const char *path, Canvas *canvas);

#endif // WB_INTERNAL_H
