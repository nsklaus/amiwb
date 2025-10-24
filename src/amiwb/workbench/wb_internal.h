#ifndef WB_INTERNAL_H
#define WB_INTERNAL_H

#include "wb_public.h"
#include "../config.h"
#include "../icons/icon_public.h"
#include "../intuition/itn_public.h"
#include "../../toolkit/progressbar/progressbar.h"
#include <X11/Xlib.h>
#include <stdbool.h>
#include <time.h>
#include <sys/types.h>

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

// Progress operation types (for progress monitoring UI)
typedef enum {
    PROGRESS_COPY,
    PROGRESS_MOVE,
    PROGRESS_DELETE,
    PROGRESS_EXTRACT
} ProgressOperation;

// Forward declaration for progress monitor
typedef struct ProgressMonitor ProgressMonitor;

// Progress monitor structure (full definition - internal to workbench module)
struct ProgressMonitor {
    Canvas *canvas;                  // Optional UI window (NULL for background)
    ProgressBar *progress_bar;       // Toolkit progress bar widget
    ProgressOperation operation;     // Type of operation being monitored
    char current_file[PATH_SIZE];    // Current file being processed
    float percent;                   // Progress percentage (0-100, -1 = not started)
    int files_done;                  // Files completed
    int files_total;                 // Total files (-1 = unknown)
    off_t bytes_done;                // Bytes processed
    off_t bytes_total;               // Total bytes (-1 = unknown)
    int pipe_fd;                     // IPC pipe from child process
    pid_t child_pid;                 // Child process PID
    time_t start_time;               // When operation started (for threshold)
    bool abort_requested;            // User requested abort
    void (*on_abort)(void);          // Abort callback
    struct ProgressMonitor *next;    // Linked list pointer
};

// ============================================================================
// wb_progress_monitor.c - Progress Monitoring
// ============================================================================

// Create progress monitor with UI (shows window immediately)
ProgressMonitor* wb_progress_monitor_create(ProgressOperation op, const char *title);

// Create background progress monitor (no UI initially, for child process tracking)
ProgressMonitor* wb_progress_monitor_create_background(ProgressOperation op, const char *filename,
                                                       int pipe_fd, pid_t child_pid);

// Update progress monitor state
void wb_progress_monitor_update(ProgressMonitor *monitor, const char *file, float percent);

// Close progress monitor
void wb_progress_monitor_close(ProgressMonitor *monitor);

// Close progress monitor by canvas (called from intuition when window is closed)
void wb_progress_monitor_close_by_canvas(Canvas *canvas);

// Render progress monitor content
void wb_progress_monitor_render(Canvas *canvas);

// Get all monitors (for polling in event loop)
ProgressMonitor* wb_progress_monitor_get_all(void);

// Check if canvas is a progress monitor window
bool wb_progress_monitor_is_canvas(Canvas *canvas);

// Get monitor for canvas
ProgressMonitor* wb_progress_monitor_get_for_canvas(Canvas *canvas);

// Create progress window for existing monitor after threshold (internal use by wb_progress.c)
Canvas* wb_progress_monitor_create_window(ProgressMonitor *monitor, const char *title);

// ============================================================================
// wb_iconinfo.c - Icon Information Dialog
// ============================================================================

// Dialog dimensions
#define ICONINFO_WIDTH 350
#define FILE_INFO_HEIGHT 500
#define DEVICE_INFO_HEIGHT 380

// Layout constants
#define ICONINFO_MARGIN 15
#define ICONINFO_SPACING 8
#define ICONINFO_BUTTON_WIDTH 80
#define ICONINFO_BUTTON_HEIGHT 25
#define ICONINFO_LABEL_WIDTH 80
#define ICONINFO_ICON_SIZE 64

// Forward declaration
typedef struct IconInfoDialog IconInfoDialog;

// Icon Information Dialog structure (full definition - internal to workbench)
struct IconInfoDialog {
    Canvas *canvas;               // Dialog window

    // Icon snapshot (copied at dialog open for independence from icon lifecycle)
    Picture icon_picture;            // Copy of icon's normal_picture
    Picture icon_selected_picture;   // Copy of icon's selected_picture
    int icon_width;                  // Icon dimensions
    int icon_height;
    char icon_label[NAME_SIZE];      // Icon label (for display/editing)
    char icon_path[PATH_SIZE];       // Icon path (for file operations)
    IconType icon_type;              // Icon type (file/drawer/device/iconified)
    bool showing_selected;           // Toggle state: false=normal, true=selected

    // Editable fields (toolkit InputFields)
    struct InputField *name_field;      // Filename (editable)
    struct InputField *comment_field;   // File comment via xattr (editable)
    struct ListView *comment_list;      // List of comment lines
    struct InputField *app_field;       // Default application (editable)
    struct InputField *path_field;      // Directory path (read-only, for copying)

    // Read-only display strings
    char size_text[64];          // File/dir size display
    char perms_text[32];         // Permission string (rwxrwxrwx)
    char owner_text[32];         // Owner username
    char group_text[32];         // Group name
    char created_text[64];       // Creation date/time
    char modified_text[64];      // Last modified date/time

    // Permission checkbox states
    bool perm_user_read;
    bool perm_user_write;
    bool perm_user_exec;
    bool perm_group_read;
    bool perm_group_write;
    bool perm_group_exec;
    bool perm_other_read;
    bool perm_other_write;
    bool perm_other_exec;

    // Button states
    bool ok_pressed;
    bool cancel_pressed;
    bool get_size_pressed;       // For directory size calculation

    // Toolkit buttons (for proper hit testing)
    struct Button *get_size_button;     // Get Size button for directories
    struct Button *ok_button;           // OK button at bottom
    struct Button *cancel_button;       // Cancel button at bottom

    // Directory size calculation
    bool calculating_size;       // Currently calculating
    bool is_directory;          // True if icon represents a directory
    pid_t size_calc_pid;        // Child process PID
    int size_pipe_fd;           // Pipe for receiving results

    // Device-specific fields (for TYPE_DEVICE icons)
    bool is_device;                  // True if icon->type == TYPE_DEVICE
    char device_path[PATH_SIZE];     // e.g., /dev/nvme0n1p3
    char mount_point[PATH_SIZE];     // e.g., /run/media/user/Movies
    char fs_type[32];                // e.g., ext4
    char access_mode[32];            // "read-only", "read/write", etc.
    ProgressBar *usage_bar;          // Disk usage visualization widget
    off_t total_bytes;               // Total disk space
    off_t free_bytes;                // Free disk space

    // Linked list for multiple dialogs
    struct IconInfoDialog *next;
};

// Main entry point - shows dialog for selected icon
void show_icon_info_dialog(FileIcon *icon);

// Event handlers (called from events.c)
bool iconinfo_handle_key_press(XKeyEvent *event);
bool iconinfo_handle_button_press(XButtonEvent *event);
bool iconinfo_handle_button_release(XButtonEvent *event);
bool iconinfo_handle_motion(XMotionEvent *event);

// Query functions
bool is_iconinfo_canvas(Canvas *canvas);
IconInfoDialog* get_iconinfo_for_canvas(Canvas *canvas);

// Rendering (called from render.c)
void render_iconinfo_content(Canvas *canvas);

// Cleanup
void close_icon_info_dialog(IconInfoDialog *dialog);
void close_icon_info_dialog_by_canvas(Canvas *canvas);
void cleanup_all_iconinfo_dialogs(void);

// Initialize/cleanup subsystem
void init_iconinfo(void);
void cleanup_iconinfo(void);

// Process monitoring for directory size calculation
void iconinfo_check_size_calculations(void);

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

// Icon creation with explicit icon file (.info) and metadata
FileIcon *wb_icons_create_with_icon_path(const char *icon_path, Canvas *canvas, int x, int y,
                                          const char *path, const char *label, int type);

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

// Generic file operation with progress dialog
int wb_progress_perform_operation(FileOperation op, const char *src_path,
                                          const char *dst_path, const char *custom_title);
int wb_progress_perform_operation_ex(FileOperation op, const char *src_path,
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

// ============================================================================
// wb_spatial.c - Spatial Window Geometry Management
// ============================================================================

// Load window geometry from xattr, fallback to cascade if not found
bool wb_spatial_load_geometry(const char *dir_path, int *x, int *y, int *width, int *height);

// Save window geometry to xattr (called on drag/resize/close)
void wb_spatial_save_geometry(const char *dir_path, int x, int y, int width, int height);

#endif // WB_INTERNAL_H
