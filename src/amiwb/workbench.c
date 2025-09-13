// File: workbench.c
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE  // For usleep
#include "workbench.h"
#include "render.h"
#include "intuition.h"
#include "compositor.h"
#include "config.h"  // Added to include config.h for max/min macros
#include "events.h"  // For clear_press_target_if_matches
#include "dialogs.h"  // For progress dialog support
#include "xdnd.h"     // XDND protocol support
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>  // For usleep, fork, pipe
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>  // For waitpid
#include <fcntl.h>
#include <sys/types.h>
#include <time.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <signal.h>
#include <time.h>
#include <stdarg.h>  // For va_list
#include <stdbool.h>  // For bool type
#include <libgen.h>  // For basename, dirname
#include <sys/xattr.h>  // For extended attributes (file comments)

// ============================================================================
// Type Definitions for Progress Dialog System
// ============================================================================

// Structure to pass data through recursive directory traversal
typedef struct {
    int total_files;
    int files_processed;
    ProgressDialog *dialog;
    bool abort;
    int pipe_fd;  // For child to send updates to parent
} CopyProgress;

// File operation types for generic progress function
typedef enum {
    FILE_OP_COPY,
    FILE_OP_MOVE,
    FILE_OP_DELETE
} FileOperation;

// IPC message structure for progress updates (enhanced for time-based triggering)
typedef struct {
    enum {
        MSG_START,      // Operation started (includes timestamp)
        MSG_PROGRESS,   // Progress update
        MSG_COMPLETE,   // Operation finished
        MSG_ERROR       // Error occurred
    } type;
    time_t start_time;      // When operation began
    int files_done;
    int files_total;        // Can be -1 if unknown/not counted
    char current_file[NAME_SIZE];
    size_t bytes_done;      // For large file progress
    size_t bytes_total;     // Size of current file (0 if unknown)
    
    // Icon creation metadata for copy operations (used on MSG_COMPLETE)
    char dest_path[PATH_SIZE];        // Full destination path
    char dest_dir[PATH_SIZE];          // Destination directory (for canvas lookup)
    bool create_icon;                  // Whether to create icon on completion
    bool has_sidecar;                  // Whether source had .info file
    char sidecar_src[PATH_SIZE];       // Source .info path if exists
    char sidecar_dst[PATH_SIZE];       // Dest .info path if exists
    int icon_x, icon_y;                // Position for new icon
    Window target_window;              // Window to create icon in
} ProgressMessage;

// Progress dialog time threshold (seconds)
#define PROGRESS_DIALOG_THRESHOLD 1  // Show dialog after 1 second

// Forward declarations for local helpers used later
static Canvas *canvas_under_pointer(void);
static int move_file_to_directory(const char *src_path, const char *dst_dir, char *dst_path, size_t dst_sz);
static int move_file_to_directory_ex(const char *src_path, const char *dst_dir, char *dst_path,
                                     size_t dst_sz, Canvas *target_canvas, int icon_x, int icon_y);
static bool is_directory(const char *path);
static int ensure_parent_dirs(const char *path);
static int copy_file(const char *src, const char *dst);
static int copy_file_with_progress(const char *src, const char *dst, int pipe_fd);
static int copy_directory_recursive(const char *src_dir, const char *dst_dir);
int perform_file_operation_with_progress(FileOperation op, const char *src_path, 
                                        const char *dst_path, const char *custom_title);
int perform_file_operation_with_progress_ex(FileOperation op, const char *src_path, 
                                           const char *dst_path, const char *custom_title,
                                           ProgressMessage *icon_metadata);
int remove_directory_recursive(const char *path);
static void remove_icon_by_path_on_canvas(const char *abs_path, Canvas *canvas);
static void refresh_canvas(Canvas *canvas);
static void find_free_slot(Canvas *canvas, int *out_x, int *out_y);

// Helper functions for cleaner file operations
static bool check_if_file_exists(const char *file_path);
static int determine_file_type_from_path(const char *full_path);
static void build_info_file_path(const char *base_dir, const char *filename, char *info_path_out, size_t max_size);
static void move_sidecar_info_file(const char *src_path, const char *dst_dir, const char *dst_path);
// Forward declarations for functions used before their definitions

// Drag rendering helpers (direct compositing into target canvas)
// These functions manage the creation, update, and destruction of the drag window
// and its contents during drag-and-drop operations.
static void create_drag_window(void);            // Initializes drag painter state
static void draw_drag_icon(void);                // Draws the dragged icon into the current target window
static void update_drag_window_position(int root_x, int root_y);
static void destroy_drag_window(void);           // Cleans up drag painter resources
#define INITIAL_ICON_CAPACITY 16
// Track init to make cleanup idempotent
static bool wb_initialized = false;

// Global state for workbench
// Central icon store and drag state live here to keep logic minimal and
// enable cross-canvas drag-and-drop without per-canvas duplication.
static FileIcon **icon_array = NULL;        // Dynamic array of all icons
static int icon_count = 0;                  // Current number of icons
static int icon_array_size = 0;             // Allocated size of icon array
FileIcon *dragged_icon = NULL;       // Moved from events.c - exported for XDND
static int drag_start_x, drag_start_y;      // Moved from events.c
static int drag_start_root_x, drag_start_root_y; // start position in root coordinates
static Canvas *drag_source_canvas = NULL;  // Track source canvas for cross-canvas drops
static bool dragging_floating = false;     // Render a floating drag image while dragging
static Window drag_win = None;             // Top-level drag window when composited
static Window saved_source_window = None;  // Original display_window of the dragged icon
static int drag_win_w = 120, drag_win_h = 100; // Virtual size to layout icon+label
static bool drag_active = false;           // becomes true after threshold is passed
static int drag_orig_x = 0, drag_orig_y = 0; // original icon position for restore
// Current target we draw into (canvas under pointer)
static Window target_win = None;
static Picture target_picture = 0;
static Visual *target_visual = NULL;
static Colormap target_colormap = 0;
/* removed unused target_canvas/prev_canvas to reduce code size */

// ========================
// Deficons (default icons) support
// Provide fallback .info icons when files lack sidecar .info next to them.
// Directories use def_dir; unknown filetypes use def_foo so everything gets
// a consistent icon even without custom sidecars.
// ========================
static const char *deficons_dir = "/usr/local/share/amiwb/icons/def_icons";

// Dynamic def_icons system - automatically scans directory for def_*.info files
typedef struct DefIconEntry {
    char *extension;   // File extension (without dot): "txt", "jpg", etc.
    char *icon_path;   // Full path to the .info file
} DefIconEntry;

static DefIconEntry *def_icons_array = NULL;
static int def_icons_count = 0;
static int def_icons_capacity = 0;

// Special cases that don't follow the pattern
static char *def_dir_info  = NULL;   // for directories (def_dir.info)
static char *def_foo_info  = NULL;   // generic fallback (def_foo.info)

// Spatial mode control (true = new window per directory, false = reuse window)
static bool spatial_mode = true;  // Default to spatial (AmigaOS style)
static bool global_show_hidden = false;  // Default to not showing hidden files

// Spatial mode getters/setters
bool get_spatial_mode(void) {
    return spatial_mode;
}

void set_spatial_mode(bool mode) {
    spatial_mode = mode;
}

// Global show hidden state getters/setters
bool get_global_show_hidden_state(void) {
    return global_show_hidden;
}

void set_global_show_hidden_state(bool show) {
    global_show_hidden = show;
}

// Add or update a def_icon in the dynamic array (silent - no logging)
static void add_or_update_deficon_entry(const char *extension, const char *full_path, bool is_user) {
    if (!extension || !full_path) return;
    
    // Check if this extension already exists (user icons override system icons)
    for (int i = 0; i < def_icons_count; i++) {
        if (strcasecmp(def_icons_array[i].extension, extension) == 0) {
            // Found existing entry - update it silently
            free(def_icons_array[i].icon_path);
            def_icons_array[i].icon_path = strdup(full_path);
            if (!def_icons_array[i].icon_path) {
                log_error("[ERROR] strdup failed for deficon path update");
                return;
            }
            return;
        }
    }
    
    // Not found - add new entry
    // Grow array if needed
    if (def_icons_count >= def_icons_capacity) {
        int new_capacity = def_icons_capacity == 0 ? 16 : def_icons_capacity * 2;
        DefIconEntry *new_array = realloc(def_icons_array, new_capacity * sizeof(DefIconEntry));
        if (!new_array) {
            log_error("[ERROR] Failed to allocate memory for def_icons array");
            return;
        }
        def_icons_array = new_array;
        def_icons_capacity = new_capacity;
    }
    
    // Add the new entry silently
    def_icons_array[def_icons_count].extension = strdup(extension);
    def_icons_array[def_icons_count].icon_path = strdup(full_path);
    if (!def_icons_array[def_icons_count].extension || !def_icons_array[def_icons_count].icon_path) {
        log_error("[ERROR] strdup failed for deficon entry");
        if (def_icons_array[def_icons_count].extension) free(def_icons_array[def_icons_count].extension);
        if (def_icons_array[def_icons_count].icon_path) free(def_icons_array[def_icons_count].icon_path);
        return;
    }
    def_icons_count++;
}

// Forward declaration for ends_with function
static bool ends_with(const char *s, const char *suffix);

// Scan a directory for def_*.info files and load them
static void scan_deficons_directory(const char *dir_path, bool is_user) {
    DIR *dir = opendir(dir_path);
    if (!dir) {
        // Only warn for system directory, user directory is optional
        if (!is_user) {
            log_error("[WARNING] Cannot open deficons directory: %s", dir_path);
        }
        return;
    }
    
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        // Check if filename matches pattern: def_*.info
        if (strncmp(entry->d_name, "def_", 4) != 0) continue;
        if (!ends_with(entry->d_name, ".info")) continue;
        
        // Build full path
        char full_path[PATH_SIZE];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
        
        // Verify it's a regular file
        struct stat st;
        if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        
        // Extract the extension part from def_XXX.info
        // Skip "def_" (4 chars), take until ".info" (strlen - 4 - 5)
        size_t name_len = strlen(entry->d_name);
        if (name_len <= 9) continue; // Too short to be valid def_X.info
        
        size_t ext_len = name_len - 4 - 5; // Remove "def_" and ".info"
        char extension[NAME_SIZE];
        strncpy(extension, entry->d_name + 4, ext_len);
        extension[ext_len] = '\0';
        
        // Handle special cases (silently)
        if (strcmp(extension, "dir") == 0) {
            if (def_dir_info) free(def_dir_info);
            def_dir_info = strdup(full_path);
            if (!def_dir_info) {
                log_error("[ERROR] strdup failed for def_dir.info");
            }
        } else if (strcmp(extension, "foo") == 0) {
            if (def_foo_info) free(def_foo_info);
            def_foo_info = strdup(full_path);
            if (!def_foo_info) {
                log_error("[ERROR] strdup failed for def_foo.info");
            }
        } else {
            // Regular extension - add or update in array
            add_or_update_deficon_entry(extension, full_path, is_user);
        }
    }
    
    closedir(dir);
}

// Automatically scan and load all def_*.info files from both system and user directories
static void load_deficons(void) {
    // First load system def_icons (silently)
    scan_deficons_directory(deficons_dir, false);
    
    // Then load user def_icons (these override system ones, also silently)
    const char *home = getenv("HOME");
    if (home) {
        char user_deficons_dir[PATH_SIZE];
        snprintf(user_deficons_dir, sizeof(user_deficons_dir), 
                "%s/.config/amiwb/icons/def_icons", home);
        scan_deficons_directory(user_deficons_dir, true);
    }
    
    // Now log the final active icons (only what will actually be used)
    // Log special icons
    if (def_dir_info) {
        log_error("[ICON] def_dir.info -> %s", def_dir_info);
    }
    if (def_foo_info) {
        log_error("[ICON] def_foo.info -> %s", def_foo_info);
    }
    
    // Log regular extension icons
    for (int i = 0; i < def_icons_count; i++) {
        log_error("[ICON] def_%s.info -> %s", 
                 def_icons_array[i].extension, def_icons_array[i].icon_path);
    }
}

// Forward declaration of helper functions
static char *replace_string(char **str, const char *new_str);
static inline void set_icon_meta(FileIcon *ic, const char *path, const char *label, int type);

// Get the most recently added icon from the global array
static FileIcon* get_last_added_icon(void) {
    return (icon_count > 0) ? icon_array[icon_count - 1] : NULL;
}

// Create icon and set its metadata in one call
FileIcon* create_icon_with_metadata(const char *icon_path, Canvas *canvas, int x, int y,
                                   const char *full_path, const char *name, int type) {
    
    // Create the icon with the correct type
    // Note: create_icon_with_type will set path to icon_path initially, but we'll override it
    create_icon_with_type(icon_path, canvas, x, y, type);
    FileIcon *new_icon = get_last_added_icon();
    if (new_icon) {
        // Override the path and label with the actual file/directory info, not the .info file
        set_icon_meta(new_icon, full_path, name, type);
    } else {
        log_error("[ERROR] get_last_added_icon returned NULL");
    }
    return new_icon;
}

// Choose the appropriate deficon for a file/dir name; returns NULL if none.
const char *definfo_for_file(const char *name, bool is_dir) {
    if (!name) return NULL;
    if (is_dir) return def_dir_info; // default drawer icon if present
    
    const char *dot = strrchr(name, '.');
    if (!dot || !dot[1]) {
        // No extension - return generic fallback if available
        return def_foo_info;
    }
    const char *ext = dot + 1;
    
    // Search the dynamic array for matching extension
    for (int i = 0; i < def_icons_count; i++) {
        if (strcasecmp(ext, def_icons_array[i].extension) == 0) {
            return def_icons_array[i].icon_path;
        }
        
        // Special handling for common multi-extension mappings
        // jpg/jpeg -> jpg
        if (strcasecmp(ext, "jpeg") == 0 && strcasecmp(def_icons_array[i].extension, "jpg") == 0) {
            return def_icons_array[i].icon_path;
        }
        // htm/html -> html
        if (strcasecmp(ext, "htm") == 0 && strcasecmp(def_icons_array[i].extension, "html") == 0) {
            return def_icons_array[i].icon_path;
        }
    }
    
    // Unknown or unmapped extension -> generic tool icon if available
    return def_foo_info;
}

// Case-insensitive label comparator for FileIcon** (used in list view)
static int label_cmp(const void *a, const void *b) {
    const FileIcon *ia = *(FileIcon* const*)a;
    const FileIcon *ib = *(FileIcon* const*)b;
    const char *la = (ia && ia->label) ? ia->label : "";
    const char *lb = (ib && ib->label) ? ib->label : "";
    return strcasecmp(la, lb);
}

// Directories first, then files; both groups A..Z by label
static int dir_first_cmp(const void *A, const void *B) {
    const FileIcon *a = *(FileIcon* const*)A;
    const FileIcon *b = *(FileIcon* const*)B;
    if ((a->type == TYPE_DRAWER) != (b->type == TYPE_DRAWER)) return (a->type == TYPE_DRAWER) ? -1 : 1;
    return label_cmp(A, B);
}

static int last_draw_x = -10000, last_draw_y = -10000; // last window-relative draw pos
static int last_root_x = -10000, last_root_y = -10000; // last root coords of pointer
static bool use_floating_window = false;   // Always use ARGB top-level drag window

// Helper to free and replace a string
static char *replace_string(char **str, const char *new_str) {
    if (*str) free(*str);
    *str = strdup(new_str);
    if (!*str) {
        log_error("[ERROR] strdup failed for string: %s", new_str);
        return NULL;
    }
    return *str;
}

// Small helpers to reduce repeated metadata/prime icon code
static inline void set_icon_meta(FileIcon *ic, const char *path, const char *label, int type) {
    if (!ic) return;
    replace_string(&ic->path, path);
    replace_string(&ic->label, label);
    ic->type = type;
}

static void add_prime_desktop_icons(Canvas *desktop) {
    if (!desktop) return;
    // Commented out - now handled by diskdrives.c
    // // System
    // create_icon("/usr/local/share/amiwb/icons/harddisk.info", desktop, 20, 40);
    // FileIcon *system_icon = get_last_added_icon();
    // set_icon_meta(system_icon, "/", "System", TYPE_DRAWER);
    // // Home
    // create_icon("/usr/local/share/amiwb/icons/harddisk.info", desktop, 20, 120);
    // FileIcon *home_icon = get_last_added_icon();
    // const char *home = getenv("HOME");
    // set_icon_meta(home_icon, home ? home : ".", "Home", TYPE_DRAWER);
}

// Internal helpers and implementations

// Start dragging icon
static void start_drag_icon(FileIcon *icon, int x, int y) {
    dragged_icon = icon;
    drag_start_x = x;
    drag_start_y = y;
    drag_source_canvas = find_canvas(icon->display_window);
    // Do not hide yet; wait for movement threshold
    saved_source_window = icon->display_window;
    // Remember original position to restore on failed drop
    drag_orig_x = icon->x;
    drag_orig_y = icon->y;
    dragging_floating = false;
    drag_active = false;
    // Record root pointer at press
    Display *dpy = get_display();
    int rx, ry, wx, wy; unsigned int mask; Window root_ret, child_ret;
    XQueryPointer(dpy, DefaultRootWindow(dpy), &root_ret, &child_ret, &rx, &ry, &wx, &wy, &mask);
    drag_start_root_x = rx; drag_start_root_y = ry;
}

// Continue dragging icon during motion
static void continue_drag_icon(XMotionEvent *event, Canvas *canvas) {
    if (!dragged_icon) return;
    Display *dpy = event->display;

    // Enforce movement threshold before starting drag
    if (!drag_active) {
        int dx = event->x_root - drag_start_root_x;
        int dy = event->y_root - drag_start_root_y;
        if (dx*dx + dy*dy < 10*10) {
            return; // not enough movement yet
        }
        // Activate drag: hide icon and create drag window
        drag_active = true;
        if (dragged_icon && saved_source_window != None) {
            dragged_icon->display_window = None;
            if (drag_source_canvas) redraw_canvas(drag_source_canvas);
        }
        (void)canvas; // silence unused in release builds
    }
    // Lazy create floating drag window and draw icon once we have motion past threshold
    if (!dragging_floating) { create_drag_window(); draw_drag_icon(); dragging_floating = true; }
    // Follow the global pointer; don't move icon inside the source canvas
    update_drag_window_position(event->x_root, event->y_root);

    // Check for XDND target under cursor
    Window xdnd_target = xdnd_find_target(dpy, event->x_root, event->y_root);

    // Handle XDND protocol if we found an external target
    if (xdnd_target != None && xdnd_target != xdnd_ctx.current_target) {
        // Leave previous XDND target if any
        if (xdnd_ctx.current_target != None) {
            xdnd_send_leave(dpy, canvas->win, xdnd_ctx.current_target);
        }

        // Enter new XDND target
        xdnd_send_enter(dpy, canvas->win, xdnd_target);
        xdnd_ctx.source_window = canvas->win;
    }

    // Send position updates to current XDND target
    if (xdnd_ctx.current_target != None) {
        xdnd_send_position(dpy, canvas->win, xdnd_ctx.current_target,
                          event->x_root, event->y_root, event->time,
                          xdnd_ctx.XdndActionCopy);
    }

    // If we left an XDND target and are back over our own canvas
    if (xdnd_target == None && xdnd_ctx.current_target != None) {
        xdnd_send_leave(dpy, canvas->win, xdnd_ctx.current_target);
        xdnd_ctx.current_target = None;
    }
}

static void end_drag_icon(Canvas *canvas) {
    Display *dpy = get_display();

    // Clean up floating drag window if any
    destroy_drag_window();

    // If no drag is active, nothing to do
    if (!dragged_icon) {
        drag_source_canvas = NULL;
        saved_source_window = None;
        // Clean up any XDND state
        if (xdnd_ctx.current_target != None) {
            xdnd_send_leave(dpy, canvas ? canvas->win : None, xdnd_ctx.current_target);
            xdnd_ctx.current_target = None;
        }
        return;
    }

    // Check if we're dropping on an XDND target
    if (xdnd_ctx.current_target != None) {
        // We're dropping on an external XDND-aware window
        // MUST destroy drag window first before anything else!
        destroy_drag_window();

        // Set up selection ownership for data transfer
        Window source_win = canvas ? canvas->win : DefaultRootWindow(dpy);
        XSetSelectionOwner(dpy, xdnd_ctx.XdndSelection, source_win, CurrentTime);

        // Send the drop message
        xdnd_send_drop(dpy, source_win, xdnd_ctx.current_target, CurrentTime);

        // Restore icon visibility
        if (saved_source_window != None) {
            dragged_icon->display_window = saved_source_window;
        }
        if (drag_source_canvas) {
            refresh_canvas(drag_source_canvas);
        }

        // Clear most of the drag state
        drag_active = false;
        dragging_floating = false;
        saved_source_window = None;

        // Keep dragged_icon temporarily for selection request handler
        // But clear the visual drag state immediately
        // We'll fully clear dragged_icon after a short delay or when selection is done

        // Clear the drag source canvas to stop any visual feedback
        drag_source_canvas = NULL;
        return;
    }

    // On button release, if pointer is over a different canvas, move file there
    Canvas *target = canvas_under_pointer();
    
    // Special handling for iconified windows - only allow moving on desktop
    if (dragged_icon && dragged_icon->type == TYPE_ICONIFIED) {
        if (target && target->type == DESKTOP && drag_source_canvas && 
            drag_source_canvas->type == DESKTOP) {
            // Just reposition the iconified window on desktop, no file operations
            // Only move if drag was actually activated (movement beyond threshold)
            if (drag_active) {
                // Get current mouse position to determine where to place the icon
                Display *dpy = get_display();
                int rx, ry, wx, wy; unsigned int mask; Window root_ret, child_ret;
                XQueryPointer(dpy, DefaultRootWindow(dpy), &root_ret, &child_ret, &rx, &ry, &wx, &wy, &mask);
                
                // Translate to desktop window coordinates
                int tx = 0, ty = 0; Window dummy;
                XTranslateCoordinates(dpy, target->win, DefaultRootWindow(dpy), 0, 0, &tx, &ty, &dummy);
                int local_x = rx - tx;
                int local_y = ry - ty;
                
                // Calculate new position for the icon (centered under cursor)
                int place_x = max(0, local_x - 32);
                int place_y = max(0, local_y - 32);
                
                // Move the icon to its new position
                move_icon(dragged_icon, place_x, place_y);
            }
            
            // Restore the icon's display window so it becomes visible again
            if (saved_source_window != None) {
                dragged_icon->display_window = saved_source_window;
            }
            
            // Refresh the desktop canvas
            if (drag_source_canvas) {
                refresh_canvas(drag_source_canvas);
            }
            
            // Now clear the drag state
            dragged_icon = NULL;
            drag_active = false;
            drag_source_canvas = NULL;
            saved_source_window = None;
            destroy_drag_window();
            return;
        } else {
            // Forbid moving iconified windows to other canvases
            if (dragged_icon) {
                if (saved_source_window != None) dragged_icon->display_window = saved_source_window;
                move_icon(dragged_icon, drag_orig_x, drag_orig_y);
            }
            if (drag_source_canvas) { refresh_canvas(drag_source_canvas); }
            dragged_icon = NULL;
            drag_active = false;
            drag_source_canvas = NULL;
            saved_source_window = None;
            destroy_drag_window();
            return;
        }
    }
    
    // Check if this is a prime icon (System "/" or Home)
    bool is_prime_icon = false;
    if (dragged_icon && dragged_icon->path) {
        const char *home = getenv("HOME");
        is_prime_icon = (strcmp(dragged_icon->path, "/") == 0 || 
                        (home && strcmp(dragged_icon->path, home) == 0));
    }
    
    // Special handling for prime icons - only allow repositioning on desktop
    if (is_prime_icon) {
        // Prime icons can ONLY be repositioned on desktop, never moved to windows
        if (target && target->type == DESKTOP && drag_source_canvas && 
            drag_source_canvas->type == DESKTOP) {
            // Just reposition the prime icon on desktop, no file operations
            // Only move if drag was actually activated (movement beyond threshold)
            if (drag_active) {
                // Get current mouse position to determine where to place the icon
                Display *dpy = get_display();
                int rx, ry, wx, wy; unsigned int mask; Window root_ret, child_ret;
                XQueryPointer(dpy, DefaultRootWindow(dpy), &root_ret, &child_ret, &rx, &ry, &wx, &wy, &mask);
                
                // Translate to desktop window coordinates
                int tx = 0, ty = 0; Window dummy;
                XTranslateCoordinates(dpy, target->win, DefaultRootWindow(dpy), 0, 0, &tx, &ty, &dummy);
                int local_x = rx - tx;
                int local_y = ry - ty;
                
                // Calculate new position for the icon (centered under cursor)
                int place_x = max(0, local_x - 32);
                int place_y = max(0, local_y - 32);
                
                // Move the icon to its new position
                move_icon(dragged_icon, place_x, place_y);
            }
            
            // Restore the icon's display window so it becomes visible again
            if (saved_source_window != None) {
                dragged_icon->display_window = saved_source_window;
            }
            
            // Refresh the desktop canvas
            if (drag_source_canvas) {
                refresh_canvas(drag_source_canvas);
            }
            
            // Now clear the drag state
            dragged_icon = NULL;
            drag_active = false;
            drag_source_canvas = NULL;
            saved_source_window = None;
            destroy_drag_window();
            return;
        } else {
            // Trying to move prime icon to a window or do file operations - forbidden
            if (dragged_icon) {
                if (saved_source_window != None) dragged_icon->display_window = saved_source_window;
                move_icon(dragged_icon, drag_orig_x, drag_orig_y);
            }
            if (drag_source_canvas) { refresh_canvas(drag_source_canvas); }
            dragged_icon = NULL;
            drag_active = false;
            drag_source_canvas = NULL;
            saved_source_window = None;
            destroy_drag_window();
            return;
        }
    }
    
    bool can_move_file = (dragged_icon && dragged_icon->path && *dragged_icon->path);
    
    // Valid targets: DESKTOP, or a Workbench WINDOW with a real directory path
    bool target_is_valid_dir_window = (target && target->type == WINDOW && target->path && is_directory(target->path));
    bool target_is_desktop = (target && target->type == DESKTOP);
    if (drag_source_canvas && target && target != drag_source_canvas &&
        (target_is_desktop || target_is_valid_dir_window) && can_move_file) {
        // Determine destination directory
        char dst_dir[1024];
        if (target_is_desktop) {
            const char *home = getenv("HOME");
            snprintf(dst_dir, sizeof(dst_dir), "%s/Desktop", home ? home : ".");
        } else {
            // For window canvases, assume canvas->path is directory path
            snprintf(dst_dir, sizeof(dst_dir), "%s", target->path ? target->path : ".");
        }
        
        // SAFETY CHECK: Prevent moving a directory into itself or its subdirectory
        if (dragged_icon->type == TYPE_DRAWER) {
            size_t src_len = strlen(dragged_icon->path);
            // Check if destination starts with source path (would create a loop)
            if (strncmp(dst_dir, dragged_icon->path, src_len) == 0 &&
                (dst_dir[src_len] == '/' || dst_dir[src_len] == '\0')) {
                log_error("[WARNING] Cannot move directory into itself or its subdirectory");
                // Restore icon to original position
                if (saved_source_window != None) dragged_icon->display_window = saved_source_window;
                move_icon(dragged_icon, drag_orig_x, drag_orig_y);
                if (drag_source_canvas) { refresh_canvas(drag_source_canvas); }
                dragged_icon = NULL;
                drag_active = false;
                drag_source_canvas = NULL;
                saved_source_window = None;
                destroy_drag_window();
                return;
            }
        }

        char dst_path[2048];
        // moved file (debug print removed)
        // Save absolute source path before moving for potential desktop cleanup
        char src_path_abs[2048];
        snprintf(src_path_abs, sizeof(src_path_abs), "%s", dragged_icon->path ? dragged_icon->path : "");

        // Calculate icon position for async operation
        Display *dpy = get_display();
        int rx, ry, wx, wy; unsigned int mask; Window root_ret, child_ret;
        XQueryPointer(dpy, DefaultRootWindow(dpy), &root_ret, &child_ret, &rx, &ry, &wx, &wy, &mask);
        int tx = 0, ty = 0; Window dummy;
        XTranslateCoordinates(dpy, target->win, DefaultRootWindow(dpy), 0, 0, &tx, &ty, &dummy);
        int local_x = rx - tx;
        int local_y = ry - ty;
        if (target->type == WINDOW) {
            local_x = max(0, local_x - BORDER_WIDTH_LEFT + target->scroll_x);
            local_y = max(0, local_y - BORDER_HEIGHT_TOP + target->scroll_y);
        }
        int place_x = max(0, local_x - 32);
        int place_y = max(0, local_y - 32);

        // Use extended version to handle async cross-filesystem moves
        int moved = move_file_to_directory_ex(dragged_icon->path, dst_dir, dst_path,
                                              sizeof(dst_path), target, place_x, place_y);
        if (moved == 0 || moved == 2) {
            // Success: moved=0 for sync, moved=2 for async cross-filesystem
            // 1) Remove dragged icon from source canvas
            destroy_icon(dragged_icon);
            dragged_icon = NULL;

            // Move sidecar .info file if present to keep custom icon with the file
            // But only for synchronous moves - async will handle it
            if (moved == 0) {
                move_sidecar_info_file(src_path_abs, dst_dir, dst_path);
            }

            // 2) For sync moves, create icon now. For async, it's created on completion
            if (moved == 2) {
                // Async cross-filesystem move - icon will be created by completion handler
                // Just refresh the canvases
                if (drag_source_canvas) {
                    compute_content_bounds(drag_source_canvas);
                    compute_max_scroll(drag_source_canvas);
                    redraw_canvas(drag_source_canvas);
                }
                if (target) {
                    compute_content_bounds(target);
                    compute_max_scroll(target);
                    redraw_canvas(target);
                }
                return;
            }

            // Synchronous move completed - create icon now
            // Icon position already calculated above (place_x, place_y)

            // Choose icon image path for the new icon:
            // 1) Sidecar .info if present
            // 2) def_icons fallback per extension (matches refresh logic)
            // 3) The file itself (last resort)
            char info_path[PATH_SIZE];
            int ret = snprintf(info_path, sizeof(info_path), "%s.info", dst_path);
            if (ret >= PATH_SIZE) {
                log_error("[ERROR] Icon path too long, operation cancelled: %s.info", dst_path);
                return;  // Cancel the operation
            }
            
            // Determine the actual file type BEFORE creating the icon
            struct stat dst_stat;
            int file_type = TYPE_FILE;  // Default to file
            if (stat(dst_path, &dst_stat) == 0) {
                file_type = S_ISDIR(dst_stat.st_mode) ? TYPE_DRAWER : TYPE_FILE;
            }
            
            // Get the file name for the icon
            const char *base = strrchr(dst_path, '/');
            const char *name_only = base ? base + 1 : dst_path;
            
            // Determine which icon image to use
            struct stat st_info;
            const char *img_path = NULL;
            if (stat(info_path, &st_info) == 0) {
                img_path = info_path;
            } else {
                const char *fallback_def = definfo_for_file(name_only, file_type == TYPE_DRAWER);
                img_path = fallback_def ? fallback_def : dst_path;
            }
            
            // Use create_icon_with_metadata to properly set all icon properties
            create_icon_with_metadata(img_path, target, place_x, place_y, 
                                     dst_path, name_only, file_type);

            // 3) If the source was Desktop directory, also remove the desktop canvas icon
            const char *home = getenv("HOME");
            char desktop_dir[1024] = {0};
            if (home) snprintf(desktop_dir, sizeof(desktop_dir), "%s/Desktop/", home);
            if (home && strncmp(src_path_abs, desktop_dir, strlen(desktop_dir)) == 0) {
                Canvas *desktop = get_desktop_canvas();
                if (desktop) {
                    remove_icon_by_path_on_canvas(src_path_abs, desktop);
                    refresh_canvas(desktop);
                }
            }

            // 4) Apply proper layout based on target view mode
            if (target->type == WINDOW && target->view_mode == VIEW_NAMES) {
                apply_view_layout(target);  // Align dropped icon in list view
            } else if (target->type == WINDOW && target->view_mode == VIEW_ICONS) {
                // Don't call icon_cleanup here - let user place icon where they want
                // Just compute bounds for scrolling
                compute_content_bounds(target);
            }
            compute_max_scroll(target);
            
            // 5) Recompute bounds and redraw both canvases
            if (drag_source_canvas) { 
                refresh_canvas(drag_source_canvas); 
            }
            redraw_canvas(target);
        } else {
            // Move failed, restore icon to original position on source canvas
            if (dragged_icon) {
                if (saved_source_window != None) dragged_icon->display_window = saved_source_window;
                move_icon(dragged_icon, drag_orig_x, drag_orig_y);
            }
            if (drag_source_canvas) { refresh_canvas(drag_source_canvas); }
        }
    } else {
        // No cross-canvas drop; either same-canvas placement or invalid target (e.g., client window)
        if (dragged_icon) {
            if (!drag_active) {
                // No actual drag occurred
            } else if (target == drag_source_canvas) {
                // Same-canvas drag: place under cursor
                Display *dpy = get_display();
                int rx, ry, wx, wy; unsigned int mask; Window root_ret, child_ret;
                XQueryPointer(dpy, DefaultRootWindow(dpy), &root_ret, &child_ret, &rx, &ry, &wx, &wy, &mask);
                int tx = 0, ty = 0; Window dummy;
                XTranslateCoordinates(dpy, drag_source_canvas->win, DefaultRootWindow(dpy), 0, 0, &tx, &ty, &dummy);
                int local_x = rx - tx;
                int local_y = ry - ty;
                if (drag_source_canvas->type == WINDOW) {
                    local_x = max(0, local_x - BORDER_WIDTH_LEFT + drag_source_canvas->scroll_x);
                    local_y = max(0, local_y - BORDER_HEIGHT_TOP + drag_source_canvas->scroll_y);
                }
                int place_x = max(0, local_x - 32);
                int place_y = max(0, local_y - 32);
                if (saved_source_window != None) dragged_icon->display_window = saved_source_window;
                move_icon(dragged_icon, place_x, place_y);
            } else {
                // Invalid cross-canvas target: restore original position
                if (saved_source_window != None) dragged_icon->display_window = saved_source_window;
                move_icon(dragged_icon, drag_orig_x, drag_orig_y);
            }
        }
        if (drag_source_canvas) { compute_content_bounds(drag_source_canvas); compute_max_scroll(drag_source_canvas); redraw_canvas(drag_source_canvas); }
    }
    dragged_icon = NULL;
    drag_source_canvas = NULL;
    saved_source_window = None;
    drag_active = false;
}


// Floating drag window impl
static void create_drag_window(void) {
    // Decide rendering mode
    Display *dpy = get_display();
    // Always use ARGB floating drag window; compositor will composite it
    use_floating_window = true;
    drag_win = None;
    target_win = None;
    target_picture = 0;
    target_visual = NULL;
    target_colormap = 0;
    /* removed: target_canvas/prev_canvas */
    last_draw_x = last_draw_y = -10000;
    last_root_x = last_root_y = -10000;

    // Create ARGB32 transparent window sized to icon+label
    Window root = DefaultRootWindow(dpy);
    XVisualInfo vinfo;
    if (!XMatchVisualInfo(dpy, DefaultScreen(dpy), 32, TrueColor, &vinfo)) {
        use_floating_window = false;
        return;
    }
    int tw = get_text_width(dragged_icon->label ? dragged_icon->label : "");
    drag_win_w = (dragged_icon->width > tw ? dragged_icon->width : tw) + 8;
    drag_win_h = dragged_icon->height + 24;
    XSetWindowAttributes attrs;
    attrs.override_redirect = True;
    attrs.colormap = XCreateColormap(dpy, root, vinfo.visual, AllocNone);
    attrs.border_pixel = 0;
    attrs.background_pixel = 0; // transparent
    attrs.background_pixmap = None; // avoid server clears painting black
    drag_win = XCreateWindow(dpy, root, 0, 0, drag_win_w, drag_win_h, 0,
                             32, InputOutput, vinfo.visual,
                             CWOverrideRedirect | CWColormap | CWBorderPixel | CWBackPixel | CWBackPixmap,
                             &attrs);
    if (drag_win == None) {
        log_error("[ERROR] XCreateWindow failed for drag window (%dx%d)", drag_win_w, drag_win_h);
        return;
    }
    // Make drag window input-transparent so hit-testing ignores it
    int shape_event_base, shape_error_base;
    if (XShapeQueryExtension(dpy, &shape_event_base, &shape_error_base)) {
        XShapeCombineRectangles(dpy, drag_win, ShapeInput, 0, 0, NULL, 0, ShapeSet, Unsorted);
    }
    XMapRaised(dpy, drag_win);
    XFlush(dpy);
    XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy, vinfo.visual);
    if (fmt) {
        XRenderPictureAttributes pa = {0};
        target_picture = XRenderCreatePicture(dpy, drag_win, fmt, 0, &pa);
        target_win = drag_win;
        target_visual = vinfo.visual;
        target_colormap = attrs.colormap;
    } else {
        // fallback to nothing (no canvas painting)
        XWindowAttributes wa;
        if (XGetWindowAttributes(dpy, drag_win, &wa)) {
            XDestroyWindow(dpy, drag_win);
        }
        drag_win = None;
        use_floating_window = false;
    }
}

static void draw_drag_icon(void) {
    if (!dragged_icon) return;
    if (use_floating_window) {
        if (!target_picture || target_win == None) return;
        Display *dpy = get_display();
        // Clear to fully transparent
        XRenderColor clear = {0,0,0,0};
        XRenderFillRectangle(dpy, PictOpSrc, target_picture, &clear, 0, 0, drag_win_w, drag_win_h);
        int dx = (drag_win_w - dragged_icon->width) / 2;
        int dy = (drag_win_h - dragged_icon->height - 20) / 2;
        XRenderComposite(dpy, PictOpOver,
                         dragged_icon->current_picture, None, target_picture,
                         0,0,0,0,
                         dx, dy,
                         dragged_icon->width, dragged_icon->height);
        XftDraw *xft = XftDrawCreate(dpy, target_win, target_visual, target_colormap);
        if (xft) {
            XftColor color; XRenderColor xr = {0xffff,0xffff,0xffff,0xffff};
            if (XftColorAllocValue(dpy, target_visual, target_colormap, &xr, &color)) {
                const char *text = dragged_icon->label ? dragged_icon->label : "";
                int tw = get_text_width(text);
                int tx = (drag_win_w - tw)/2;
                int ty = dy + dragged_icon->height + 16;
                XftDrawStringUtf8(xft, &color, get_font(), tx, ty, (const FcChar8*)text, (int)strlen(text));
                XftColorFree(dpy, target_visual, target_colormap, &color);
            }
            XftDrawDestroy(xft);
        }
        XFlush(dpy);
        return;
    }
    // No fallback: we do not paint into canvases during drag
    return;
}

static void update_drag_window_position(int root_x, int root_y) {
    Display *dpy = get_display();
    last_root_x = root_x; last_root_y = root_y;
    if (use_floating_window) {
        if (drag_win != None) {
            // Position so cursor is near center
            int x = root_x - drag_win_w/2;
            int y = root_y - drag_win_h/2;
            XMoveWindow(dpy, drag_win, x, y);
        }
        return;
    }
    // Floating window path only; compositor will blend
}

static void destroy_drag_window(void) {
    Display *dpy = get_display();
    if (target_picture) { XRenderFreePicture(dpy, target_picture); target_picture = 0; }
    if (drag_win != None) {
        XWindowAttributes wa;
        if (XGetWindowAttributes(dpy, drag_win, &wa)) {
            XDestroyWindow(dpy, drag_win);
        }
        drag_win = None;
    }
    target_win = None;
    target_visual = NULL;
    target_colormap = 0;
    dragging_floating = false;
}

// Missing helpers and API restored

// Cache for canvas_under_pointer to avoid repeated tree walks
static struct {
    Canvas *cached_canvas;
    int cached_x, cached_y;
    Time cache_time;
    bool valid;
} pointer_cache = {NULL, -1, -1, 0, false};

static Canvas *canvas_under_pointer(void) {
    Display *dpy = get_display();
    Window root = DefaultRootWindow(dpy);
    Window root_ret, child_ret; int rx, ry, wx, wy; unsigned int mask;
    if (!XQueryPointer(dpy, root, &root_ret, &child_ret, &rx, &ry, &wx, &wy, &mask)) return NULL;

    // Check cache - if pointer hasn't moved, return cached result
    if (pointer_cache.valid && pointer_cache.cached_x == rx && pointer_cache.cached_y == ry) {
        // Quick validation that cached canvas still exists and is visible
        if (pointer_cache.cached_canvas) {
            XWindowAttributes wa;
            if (XGetWindowAttributes(dpy, pointer_cache.cached_canvas->win, &wa) &&
                wa.map_state == IsViewable) {
                return pointer_cache.cached_canvas;
            }
        }
    }

    // Cache miss or invalid - do the expensive tree walk
    Window r, p, *children = NULL; unsigned int n = 0;
    if (!XQueryTree(dpy, root, &r, &p, &children, &n)) return NULL;

    Canvas *best = NULL;
    // Walk from topmost to bottom to find the topmost WINDOW under pointer
    for (int i = (int)n - 1; i >= 0; --i) {
        Window w = children[i];
        Canvas *c = find_canvas(w);
        if (!c) continue;
        if (c->type == MENU) continue; // menus are not drop targets

        // Check if pointer is within this canvas
        // Use cached window attributes if available
        if (c->x <= rx && rx < c->x + c->width &&
            c->y <= ry && ry < c->y + c->height) {
            // Quick visibility check
            XWindowAttributes wa;
            if (XGetWindowAttributes(dpy, w, &wa) && wa.map_state == IsViewable) {
                // Prefer WINDOW over DESKTOP
                if (c->type == WINDOW) {
                    best = c;
                    break;
                }
                if (!best) best = c; // could be DESKTOP
            }
        }
    }
    if (children) XFree(children);

    // Update cache
    pointer_cache.cached_canvas = best;
    pointer_cache.cached_x = rx;
    pointer_cache.cached_y = ry;
    pointer_cache.cache_time = CurrentTime;
    pointer_cache.valid = true;

    return best;
}

// Invalidate pointer cache when windows move or change
void invalidate_pointer_cache(void) {
    pointer_cache.valid = false;
}

static bool is_directory(const char *path) {
    if (!path || !*path) return false;
    struct stat st; if (stat(path, &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

// Check if a file exists on the filesystem
static bool check_if_file_exists(const char *file_path) {
    if (!file_path || !*file_path) return false;
    struct stat st;
    return stat(file_path, &st) == 0;
}

// Determine file type (TYPE_DRAWER or TYPE_FILE) from filesystem
static int determine_file_type_from_path(const char *full_path) {
    if (!full_path) return TYPE_FILE;
    struct stat st;
    if (stat(full_path, &st) != 0) return TYPE_FILE;
    return S_ISDIR(st.st_mode) ? TYPE_DRAWER : TYPE_FILE;
}

// Build path to .info sidecar file for a given filename in a directory
static void build_info_file_path(const char *base_dir, const char *filename, char *info_path_out, size_t max_size) {
    if (!base_dir || !filename || !info_path_out || max_size == 0) return;
    snprintf(info_path_out, max_size, "%s/%s.info", base_dir, filename);
}

// Move sidecar .info file from source to destination directory with overwrite handling
static void move_sidecar_info_file(const char *src_path, const char *dst_dir, const char *dst_path) {
    if (!src_path || !dst_dir || !dst_path) return;
    
    char src_info[2048];
    snprintf(src_info, sizeof(src_info), "%s.info", src_path);
    
    if (!check_if_file_exists(src_info)) return; // No sidecar to move
    
    const char *base = strrchr(dst_path, '/');
    const char *name_only = base ? base + 1 : dst_path;
    char dst_info[2048];
    snprintf(dst_info, sizeof(dst_info), "%s/%s.info", dst_dir, name_only);
    
    // Try to rename (move) the sidecar file
    if (rename(src_info, dst_info) != 0) {
        if (errno == EXDEV) {
            // Cross-filesystem: copy then delete source
            unlink(dst_info); // Remove destination if it exists
            if (copy_file(src_info, dst_info) == 0) {
                unlink(src_info); // Remove source after successful copy
            } else {
                perror("[amiwb] copy sidecar failed");
            }
        } else {
            // Same filesystem but target exists: remove target and try again
            unlink(dst_info);
            if (rename(src_info, dst_info) != 0) {
                perror("[amiwb] rename sidecar failed");
            }
        }
    }
}

// Helper function to clean up file descriptors
static void cleanup_file_descriptors(int in_fd, int out_fd) {
    if (out_fd >= 0) close(out_fd);
    if (in_fd >= 0) close(in_fd);
}

// Copy a regular file from src to dst. Overwrites dst if it exists only when
// the caller has already unlinked it. Returns 0 on success.
static int copy_file(const char *src, const char *dst) {
    int in_fd = -1, out_fd = -1;
    struct stat st;
    
    // Check source file
    if (stat(src, &st) != 0 || !S_ISREG(st.st_mode)) {
        return -1;
    }
    
    // Open source file
    in_fd = open(src, O_RDONLY);
    if (in_fd < 0) {
        return -1;
    }
    
    // Create destination file with 0600 and then fchmod to preserve original mode
    out_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (out_fd < 0) {
        cleanup_file_descriptors(in_fd, -1);
        return -1;
    }
    
    // Copy file contents
    char buf[1 << 16];
    ssize_t r;
    while ((r = read(in_fd, buf, sizeof(buf))) > 0) {
        char *p = buf;
        ssize_t remaining = r;
        while (remaining > 0) {
            ssize_t w = write(out_fd, p, remaining);
            if (w < 0) {
                cleanup_file_descriptors(in_fd, out_fd);
                return -1;
            }
            p += w; 
            remaining -= w;
        }
    }
    
    // Check for read error
    if (r < 0) {
        cleanup_file_descriptors(in_fd, out_fd);
        return -1;
    }
    
    // Preserve permissions (ignore ownership and times for simplicity)
    fchmod(out_fd, st.st_mode & 0777);
    
    // Clean up file descriptors
    cleanup_file_descriptors(in_fd, out_fd);
    
    // Preserve extended attributes (file comments, etc.)
    // This must be done after closing files since xattr functions use paths
    ssize_t buflen = listxattr(src, NULL, 0);
    if (buflen > 0) {
        char *buf = malloc(buflen);
        if (buf) {
            buflen = listxattr(src, buf, buflen);
            if (buflen > 0) {
                // Copy each xattr
                char *p = buf;
                while (p < buf + buflen) {
                    // Get value size
                    ssize_t vallen = getxattr(src, p, NULL, 0);
                    if (vallen > 0) {
                        char *val = malloc(vallen);
                        if (val) {
                            // Get value
                            vallen = getxattr(src, p, val, vallen);
                            if (vallen > 0) {
                                // Set on destination
                                setxattr(dst, p, val, vallen, 0);
                            }
                            free(val);
                        }
                    }
                    // Move to next attribute name
                    p += strlen(p) + 1;
                }
            }
            free(buf);
        }
    }
    
    return 0;
}

// Copy file with byte-level progress reporting
static int copy_file_with_progress(const char *src, const char *dst, int pipe_fd) {
    int in_fd = -1, out_fd = -1;
    struct stat st;
    
    // Check source file
    if (stat(src, &st) != 0 || !S_ISREG(st.st_mode)) {
        return -1;
    }
    
    // Open source file
    in_fd = open(src, O_RDONLY);
    if (in_fd < 0) {
        return -1;
    }
    
    // Create destination file with 0600 and then fchmod to preserve original mode
    out_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (out_fd < 0) {
        cleanup_file_descriptors(in_fd, -1);
        return -1;
    }
    
    // Prepare progress message
    ProgressMessage msg = {
        .type = MSG_PROGRESS,
        .start_time = time(NULL),
        .files_done = 0,
        .files_total = 1,
        .bytes_done = 0,
        .bytes_total = st.st_size
    };
    strncpy(msg.current_file, basename(strdup(src)), NAME_SIZE - 1);
    
    // Copy file contents with progress
    char buf[1 << 16];  // 64KB buffer
    ssize_t r;
    size_t total_copied = 0;
    size_t last_progress_update = 0;
    
    while ((r = read(in_fd, buf, sizeof(buf))) > 0) {
        char *p = buf;
        ssize_t remaining = r;
        while (remaining > 0) {
            ssize_t w = write(out_fd, p, remaining);
            if (w < 0) {
                cleanup_file_descriptors(in_fd, out_fd);
                return -1;
            }
            p += w;
            remaining -= w;
        }
        
        total_copied += r;
        
        // Send progress update every 1MB or at completion
        if (pipe_fd > 0 && (total_copied - last_progress_update > 1024*1024 || 
                            total_copied == st.st_size)) {
            msg.bytes_done = total_copied;
            write(pipe_fd, &msg, sizeof(msg));
            last_progress_update = total_copied;
        }
    }
    
    // Check for read error
    if (r < 0) {
        cleanup_file_descriptors(in_fd, out_fd);
        return -1;
    }
    
    // Send final progress if not already sent
    if (pipe_fd > 0 && total_copied != last_progress_update) {
        msg.bytes_done = total_copied;
        msg.files_done = 1;  // Mark file as complete
        write(pipe_fd, &msg, sizeof(msg));
    }
    
    // Preserve permissions (ignore ownership and times for simplicity)
    fchmod(out_fd, st.st_mode & 0777);
    
    // Clean up file descriptors
    cleanup_file_descriptors(in_fd, out_fd);
    
    // Preserve extended attributes (file comments, etc.)
    // This must be done after closing files since xattr functions use paths
    ssize_t buflen = listxattr(src, NULL, 0);
    if (buflen > 0) {
        char *buf = malloc(buflen);
        if (buf) {
            buflen = listxattr(src, buf, buflen);
            if (buflen > 0) {
                // Copy each xattr
                char *p = buf;
                while (p < buf + buflen) {
                    // Get value size
                    ssize_t vallen = getxattr(src, p, NULL, 0);
                    if (vallen > 0) {
                        char *val = malloc(vallen);
                        if (val) {
                            // Get value
                            vallen = getxattr(src, p, val, vallen);
                            if (vallen > 0) {
                                // Set on destination
                                setxattr(dst, p, val, vallen, 0);
                            }
                            free(val);
                        }
                    }
                    // Move to next attribute name
                    p += strlen(p) + 1;
                }
            }
            free(buf);
        }
    }
    
    return 0;
}

// Forward declaration - implementation moved after queue utilities
int remove_directory_recursive(const char *path);

// Recursively copy a directory from src to dst
// Returns 0 on success, -1 on failure
// Helper function to create parent directories recursively
static int ensure_parent_dirs(const char *path) {
    char tmp[PATH_SIZE];
    char *p = NULL;
    size_t len;
    
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    
    // Remove trailing slash if present
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = 0;
    }
    
    // Create each parent directory in the path
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            // Try to create directory, ignore if exists
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                log_error("[ERROR] Cannot create directory: %s - %s\n", tmp, strerror(errno));
                *p = '/';
                return -1;
            }
            *p = '/';
        }
    }
    
    // Create the final directory
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        log_error("[ERROR] Cannot create final directory: %s - %s\n", tmp, strerror(errno));
        return -1;
    }
    return 0;
}

static int copy_directory_recursive(const char *src_dir, const char *dst_dir) {
    if (!src_dir || !dst_dir || !*src_dir || !*dst_dir) return -1;
    
    // Get source directory stats
    struct stat src_stat;
    if (stat(src_dir, &src_stat) != 0 || !S_ISDIR(src_stat.st_mode)) {
        return -1;
    }
    
    // Ensure all parent directories exist first
    ensure_parent_dirs(dst_dir);
    
    // Create destination directory with write permissions (0755)
    // This ensures we can copy files into it, even if source is read-only
    if (mkdir(dst_dir, 0755) != 0) {
        // If it exists and is a directory, that's OK
        struct stat dst_stat;
        if (stat(dst_dir, &dst_stat) != 0 || !S_ISDIR(dst_stat.st_mode)) {
            return -1;
        }
    }
    
    // Preserve extended attributes (including comments) for the directory itself
    ssize_t buflen = listxattr(src_dir, NULL, 0);
    if (buflen > 0) {
        char *buf = malloc(buflen);
        if (buf) {
            buflen = listxattr(src_dir, buf, buflen);
            if (buflen > 0) {
                // Copy each xattr
                char *p = buf;
                while (p < buf + buflen) {
                    // Get value size
                    ssize_t vallen = getxattr(src_dir, p, NULL, 0);
                    if (vallen > 0) {
                        char *val = malloc(vallen);
                        if (val) {
                            // Get value
                            vallen = getxattr(src_dir, p, val, vallen);
                            if (vallen > 0) {
                                // Set on destination
                                setxattr(dst_dir, p, val, vallen, 0);
                            }
                            free(val);
                        }
                    }
                    // Move to next attribute name
                    p += strlen(p) + 1;
                }
            }
            free(buf);
        }
    }
    
    // Open source directory
    DIR *dir = opendir(src_dir);
    if (!dir) {
        return -1;
    }
    
    struct dirent *entry;
    char src_path[PATH_SIZE];
    char dst_path[PATH_SIZE];
    int result = 0;
    
    while ((entry = readdir(dir)) != NULL && result == 0) {
        // Skip . and ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        snprintf(src_path, sizeof(src_path), "%s/%s", src_dir, entry->d_name);
        snprintf(dst_path, sizeof(dst_path), "%s/%s", dst_dir, entry->d_name);
        
        struct stat st;
        if (stat(src_path, &st) != 0) {
            result = -1;
            break;
        }
        
        if (S_ISDIR(st.st_mode)) {
            // Recursively copy subdirectory
            if (copy_directory_recursive(src_path, dst_path) != 0) {
                log_error("[ERROR] Failed to copy directory: %s to %s", src_path, dst_path);
                result = -1;
            }
        } else if (S_ISREG(st.st_mode)) {
            // Copy regular file
            if (copy_file(src_path, dst_path) != 0) {
                log_error("[ERROR] Failed to copy file: %s to %s", src_path, dst_path);
                result = -1;
            }
        }
        // Skip other file types (symlinks, devices, etc.)
    }
    
    closedir(dir);
    return result;
}

// ============================================================================
// Directory Queue Implementation for Iterative Tree Traversal
// Replaces dangerous recursion with safe heap-based iteration
// ============================================================================

typedef struct DirQueueNode {
    char *path;  // Dynamically allocated to save memory
    char *dest_path;  // For copy operations that need source and destination
    struct DirQueueNode *next;
} DirQueueNode;

typedef struct {
    DirQueueNode *head;
    DirQueueNode *tail;
    int size;  // Track size for memory monitoring
} DirQueue;

// Initialize an empty queue
static void queue_init(DirQueue *q) {
    if (!q) return;
    q->head = NULL;
    q->tail = NULL;
    q->size = 0;
}

// Add a path to the queue (returns 0 on success, -1 on memory error)
// For simple traversal, pass NULL for dest_path
static int queue_push_pair(DirQueue *q, const char *path, const char *dest_path) {
    if (!q || !path) return -1;
    
    DirQueueNode *node = malloc(sizeof(DirQueueNode));
    if (!node) {
        log_error("[ERROR] queue_push: Failed to allocate queue node\n");
        return -1;
    }
    
    node->path = strdup(path);
    if (!node->path) {
        log_error("[ERROR] queue_push: Failed to duplicate path\n");
        free(node);
        return -1;
    }
    
    // Optional destination path for copy operations
    if (dest_path) {
        node->dest_path = strdup(dest_path);
        if (!node->dest_path) {
            log_error("[ERROR] queue_push: Failed to duplicate dest_path\n");
            free(node->path);
            free(node);
            return -1;
        }
    } else {
        node->dest_path = NULL;
    }
    
    node->next = NULL;
    
    if (q->tail) {
        q->tail->next = node;
    } else {
        q->head = node;
    }
    q->tail = node;
    q->size++;
    
    // Warn if queue is getting very large (potential memory issue)
    if (q->size > 10000) {
        log_error("[WARNING] Directory queue size exceeds 10000 entries\n");
    }
    
    return 0;
}

// Simple wrapper for single-path queuing (backward compatibility)
static int queue_push(DirQueue *q, const char *path) {
    return queue_push_pair(q, path, NULL);
}

// Remove and return the next path from queue (caller must free returned string)
// If dest_out is provided, also returns the destination path (caller must free)
static char *queue_pop_pair(DirQueue *q, char **dest_out) {
    if (!q || !q->head) return NULL;
    
    DirQueueNode *node = q->head;
    char *path = node->path;
    
    if (dest_out) {
        *dest_out = node->dest_path;  // Transfer ownership
    } else if (node->dest_path) {
        free(node->dest_path);  // Free if not wanted
    }
    
    q->head = node->next;
    if (!q->head) {
        q->tail = NULL;
    }
    
    q->size--;
    free(node);
    
    return path;  // Caller must free this
}

// Simple wrapper for single-path popping (backward compatibility)
static char *queue_pop(DirQueue *q) {
    return queue_pop_pair(q, NULL);
}

// Free all remaining queue nodes
static void queue_free(DirQueue *q) {
    if (!q) return;
    
    char *path;
    while ((path = queue_pop(q)) != NULL) {
        free(path);
    }
    
    q->head = NULL;
    q->tail = NULL;
    q->size = 0;
}

// Count files in directory tree (ITERATIVE - safe from stack overflow)
static void count_files_in_directory(const char *path, int *count) {
    if (!path || !count) return;
    
    DirQueue queue;
    queue_init(&queue);
    
    // Start with the initial directory
    if (queue_push(&queue, path) != 0) {
        queue_free(&queue);
        return;
    }
    
    char *current_path;
    while ((current_path = queue_pop(&queue)) != NULL) {
        DIR *dir = opendir(current_path);
        if (!dir) {
            free(current_path);
            continue;
        }
        
        struct dirent *entry;
        char full_path[PATH_SIZE];
        
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || 
                strcmp(entry->d_name, "..") == 0) continue;
            
            snprintf(full_path, sizeof(full_path), "%s/%s", 
                     current_path, entry->d_name);
            
            struct stat st;
            if (stat(full_path, &st) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    // Add subdirectory to queue instead of recursing
                    if (queue_push(&queue, full_path) != 0) {
                        log_error("[WARNING] count_files: Failed to queue %s\n", full_path);
                        // Continue counting even if we can't queue this dir
                    }
                } else if (S_ISREG(st.st_mode)) {
                    (*count)++;
                }
            }
        }
        closedir(dir);
        free(current_path);
    }
    
    queue_free(&queue);
}

// Remove directory tree (ITERATIVE - safe from stack overflow)
// Uses two queues: process all files first, then directories in reverse order
int remove_directory_recursive(const char *path) {
    if (!path || !*path) return -1;
    
    DirQueue dir_queue;   // Directories to process
    DirQueue rm_queue;    // Directories to remove (in reverse order)
    queue_init(&dir_queue);
    queue_init(&rm_queue);
    
    int result = 0;
    
    // Start with the root directory
    if (queue_push(&dir_queue, path) != 0) {
        queue_free(&dir_queue);
        queue_free(&rm_queue);
        return -1;
    }
    
    // Phase 1: Traverse all directories and delete files
    char *current_path;
    while ((current_path = queue_pop(&dir_queue)) != NULL) {
        DIR *dir = opendir(current_path);
        if (!dir) {
            log_error("[ERROR] Cannot open directory for removal: %s\n", current_path);
            result = -1;
            free(current_path);
            break;
        }
        
        // Add this directory to removal queue (will be removed after its contents)
        if (queue_push(&rm_queue, current_path) != 0) {
            log_error("[WARNING] Failed to queue directory for removal: %s\n", current_path);
            result = -1;
            closedir(dir);
            free(current_path);
            break;
        }
        
        struct dirent *entry;
        char full_path[PATH_SIZE];
        
        while ((entry = readdir(dir)) != NULL) {
            // Skip . and ..
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            
            snprintf(full_path, sizeof(full_path), "%s/%s", current_path, entry->d_name);
            
            struct stat st;
            if (stat(full_path, &st) != 0) {
                log_error("[WARNING] Cannot stat for removal: %s\n", full_path);
                result = -1;
                continue;
            }
            
            if (S_ISDIR(st.st_mode)) {
                // Queue subdirectory for processing
                if (queue_push(&dir_queue, full_path) != 0) {
                    log_error("[WARNING] Failed to queue subdirectory: %s\n", full_path);
                    result = -1;
                    break;
                }
            } else {
                // Remove file immediately
                if (unlink(full_path) != 0) {
                    log_error("[WARNING] Failed to remove file: %s\n", full_path);
                    result = -1;
                    // Continue trying to remove other files
                }
            }
        }
        
        closedir(dir);
        free(current_path);
    }
    
    // Phase 2: Remove directories in reverse order (deepest first)
    // We need to reverse the queue - simple way is to collect all and process backwards
    char **dir_paths = NULL;
    int dir_count = 0;
    int dir_capacity = 16;
    
    dir_paths = malloc(dir_capacity * sizeof(char*));
    if (!dir_paths) {
        log_error("[ERROR] Failed to allocate directory list for removal\n");
        queue_free(&dir_queue);
        queue_free(&rm_queue);
        return -1;
    }
    
    // Collect all directories
    while ((current_path = queue_pop(&rm_queue)) != NULL) {
        if (dir_count >= dir_capacity) {
            dir_capacity *= 2;
            char **new_paths = realloc(dir_paths, dir_capacity * sizeof(char*));
            if (!new_paths) {
                log_error("[ERROR] Failed to expand directory list\n");
                // Clean up what we have
                for (int i = 0; i < dir_count; i++) {
                    free(dir_paths[i]);
                }
                free(dir_paths);
                free(current_path);
                queue_free(&dir_queue);
                queue_free(&rm_queue);
                return -1;
            }
            dir_paths = new_paths;
        }
        dir_paths[dir_count++] = current_path;
    }
    
    // Remove directories in reverse order (deepest first)
    for (int i = dir_count - 1; i >= 0; i--) {
        if (rmdir(dir_paths[i]) != 0) {
            log_error("[WARNING] Failed to remove directory: %s (errno=%d)\n", 
                     dir_paths[i], errno);
            result = -1;
            // Continue trying to remove other directories
        }
        free(dir_paths[i]);
    }
    
    free(dir_paths);
    queue_free(&dir_queue);
    queue_free(&rm_queue);
    
    return result;
}

// Copy directory tree with progress updates (ITERATIVE - safe from stack overflow)
static int copy_directory_recursive_with_progress(const char *src_dir, const char *dst_dir, CopyProgress *progress) {
    if (!src_dir || !dst_dir || !*src_dir || !*dst_dir) return -1;
    
    DirQueue queue;
    queue_init(&queue);
    int result = 0;
    
    // Initialize with the root directories
    if (queue_push_pair(&queue, src_dir, dst_dir) != 0) {
        queue_free(&queue);
        return -1;
    }
    
    char *current_src;
    char *current_dst;
    
    while ((current_src = queue_pop_pair(&queue, &current_dst)) != NULL) {
        // Check for abort
        if (progress && progress->dialog && progress->dialog->abort_requested) {
            progress->abort = true;
            result = -1;
            free(current_src);
            free(current_dst);
            break;
        }
        
        // Get source directory stats
        struct stat src_stat;
        if (stat(current_src, &src_stat) != 0 || !S_ISDIR(src_stat.st_mode)) {
            log_error("[ERROR] Not a directory or cannot stat: %s\n", current_src);
            result = -1;
            free(current_src);
            free(current_dst);
            break;
        }
        
        // Create destination directory with write permissions (0755)
        // to avoid issues with read-only source directories
        if (mkdir(current_dst, 0755) != 0) {
            // If it exists and is a directory, that's OK
            struct stat dst_stat;
            if (stat(current_dst, &dst_stat) != 0 || !S_ISDIR(dst_stat.st_mode)) {
                log_error("[ERROR] Cannot create directory: %s\n", current_dst);
                result = -1;
                free(current_src);
                free(current_dst);
                break;
            }
        }
        
        // Preserve extended attributes (including comments) for the directory itself
        ssize_t buflen = listxattr(current_src, NULL, 0);
        if (buflen > 0) {
            char *buf = malloc(buflen);
            if (buf) {
                buflen = listxattr(current_src, buf, buflen);
                if (buflen > 0) {
                    // Copy each xattr
                    char *p = buf;
                    while (p < buf + buflen) {
                        // Get value size
                        ssize_t vallen = getxattr(current_src, p, NULL, 0);
                        if (vallen > 0) {
                            char *val = malloc(vallen);
                            if (val) {
                                // Get value
                                vallen = getxattr(current_src, p, val, vallen);
                                if (vallen > 0) {
                                    // Set on destination
                                    setxattr(current_dst, p, val, vallen, 0);
                                }
                                free(val);
                            }
                        }
                        // Move to next attribute name
                        p += strlen(p) + 1;
                    }
                }
                free(buf);
            }
        }
        
        // Open source directory
        DIR *dir = opendir(current_src);
        if (!dir) {
            log_error("[ERROR] Cannot open directory: %s\n", current_src);
            result = -1;
            free(current_src);
            free(current_dst);
            break;
        }
        
        struct dirent *entry;
        char src_path[PATH_SIZE];
        char dst_path[PATH_SIZE];
        
        while ((entry = readdir(dir)) != NULL && result == 0) {
            // Check for abort
            if (progress && progress->dialog && progress->dialog->abort_requested) {
                progress->abort = true;
                result = -1;
                break;
            }
            
            // Skip . and ..
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            
            snprintf(src_path, sizeof(src_path), "%s/%s", current_src, entry->d_name);
            snprintf(dst_path, sizeof(dst_path), "%s/%s", current_dst, entry->d_name);
            
            struct stat st;
            if (stat(src_path, &st) != 0) {
                log_error("[ERROR] Cannot stat: %s\n", src_path);
                result = -1;
                break;
            }
            
            if (S_ISDIR(st.st_mode)) {
                // Queue subdirectory for later processing instead of recursing
                if (queue_push_pair(&queue, src_path, dst_path) != 0) {
                    log_error("[WARNING] Failed to queue directory: %s\n", src_path);
                    result = -1;
                    break;
                }
            } else if (S_ISREG(st.st_mode)) {
                // Update progress - send IPC message if we have a pipe
                if (progress) {
                    progress->files_processed++;
                    
                    if (progress->pipe_fd > 0) {
                        // Child process - send progress update to parent
                        ProgressMessage msg = {
                            .type = MSG_PROGRESS,
                            .files_done = progress->files_processed,
                            .files_total = progress->total_files
                        };
                        strncpy(msg.current_file, entry->d_name, NAME_SIZE - 1);
                        msg.current_file[NAME_SIZE - 1] = '\0';
                        write(progress->pipe_fd, &msg, sizeof(msg));
                    } else if (progress->dialog) {
                        // Parent process or no pipe - update dialog directly
                        float percent = (progress->total_files > 0) ? 
                            ((float)progress->files_processed / progress->total_files * 100.0f) : 0.0f;
                        update_progress_dialog(progress->dialog, entry->d_name, percent);
                    }
                }
                
                // Copy regular file
                if (copy_file(src_path, dst_path) != 0) {
                    log_error("[ERROR] Failed to copy file: %s to %s\n", src_path, dst_path);
                    result = -1;
                    break;
                }
            }
            // Skip other file types (symlinks, devices, etc.)
        }
        
        closedir(dir);
        free(current_src);
        free(current_dst);
    }
    
    queue_free(&queue);
    return result;
}


// Move directory with progress dialog (for cross-filesystem moves)

// returns 0 on success, non-zero on failure
// Extended version that returns status codes for async operations
static int move_file_to_directory_ex(const char *src_path, const char *dst_dir, char *dst_path,
                                     size_t dst_sz, Canvas *target_canvas, int icon_x, int icon_y) {
    if (!src_path || !dst_dir || !dst_path || !*src_path || !*dst_dir) return -1;
    // Allow moving both files and directories
    // Ensure destination is a directory
    if (!is_directory(dst_dir)) { errno = ENOTDIR; return -1; }
    // Build destination path
    const char *base = strrchr(src_path, '/');
    base = base ? base + 1 : src_path;
    snprintf(dst_path, dst_sz, "%s/%s", dst_dir, base);
    // If source and destination are identical, do nothing
    if (strcmp(src_path, dst_path) == 0) return 0;
    // Check if source is a directory
    struct stat st_src;
    bool is_src_dir = (stat(src_path, &st_src) == 0 && S_ISDIR(st_src.st_mode));

    // Overwrite semantics: ensure destination path is free
    if (!is_src_dir) {
        unlink(dst_path);  // For files, remove any existing destination
    } else {
        rmdir(dst_path);   // For directories, try to remove empty destination dir
    }

    if (rename(src_path, dst_path) != 0) {
        if (errno == EXDEV) {
            // Cross-filesystem move - needs async operation with icon metadata

            // Prepare icon metadata for completion handler
            ProgressMessage icon_meta = {0};
            icon_meta.create_icon = true;
            strncpy(icon_meta.dest_path, dst_path, PATH_SIZE - 1);
            strncpy(icon_meta.dest_dir, dst_dir, PATH_SIZE - 1);
            icon_meta.icon_x = icon_x;
            icon_meta.icon_y = icon_y;
            icon_meta.target_window = target_canvas ? target_canvas->win : None;

            // Check for sidecar .info file
            char info_src[PATH_SIZE], info_dst[PATH_SIZE];
            snprintf(info_src, sizeof(info_src), "%s.info", src_path);
            snprintf(info_dst, sizeof(info_dst), "%s.info", dst_path);
            struct stat st_info;
            if (stat(info_src, &st_info) == 0) {
                icon_meta.has_sidecar = true;
                strncpy(icon_meta.sidecar_src, info_src, PATH_SIZE - 1);
                strncpy(icon_meta.sidecar_dst, info_dst, PATH_SIZE - 1);
            }

            // Start async operation with icon metadata
            if (perform_file_operation_with_progress_ex(FILE_OP_MOVE, src_path, dst_path,
                                                        NULL, &icon_meta) != 0) {
                log_error("[ERROR] Failed to move across filesystem: %s to %s", src_path, dst_path);
                return -1;
            }
            return 2; // Special code for async operation started
        } else {
            perror("[amiwb] rename (move) failed");
            return -1;
        }
    }
    return 0;
}

static int move_file_to_directory(const char *src_path, const char *dst_dir, char *dst_path, size_t dst_sz) {
    // Legacy version without icon metadata - just calls extended version
    int result = move_file_to_directory_ex(src_path, dst_dir, dst_path, dst_sz, NULL, 0, 0);
    // Convert async return code to success for backward compatibility
    return (result == 2) ? 0 : result;
}

// ============================================================================
// Generic File Operation with Time-Based Progress Dialog
// Shows progress only if operation takes longer than threshold
// ============================================================================

// Extended version that accepts icon creation metadata
int perform_file_operation_with_progress_ex(
    FileOperation op,
    const char *src_path,
    const char *dst_path,
    const char *custom_title,
    ProgressMessage *icon_metadata
) {
    if (!src_path) return -1;
    if ((op == FILE_OP_COPY || op == FILE_OP_MOVE) && !dst_path) return -1;
    
    // Determine if this is a directory
    struct stat st;
    if (stat(src_path, &st) != 0) {
        log_error("[ERROR] Cannot stat: %s\n", src_path);
        return -1;
    }
    
    bool is_directory = S_ISDIR(st.st_mode);
    
    // Create pipe for IPC
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        log_error("[ERROR] Failed to create pipe for progress\n");
        // Fall back to synchronous operation
        switch (op) {
            case FILE_OP_COPY:
                return is_directory ? copy_directory_recursive(src_path, dst_path) :
                                    copy_file(src_path, dst_path);
            case FILE_OP_MOVE:
                return move_file_to_directory(src_path, dirname(strdup(dst_path)), NULL, 0);
            case FILE_OP_DELETE:
                return is_directory ? remove_directory_recursive(src_path) : unlink(src_path);
        }
    }
    
    // Set read end to non-blocking
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
    
    // Fork to perform operation in background
    pid_t pid = fork();
    if (pid == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        log_error("[ERROR] Fork failed\n");
        // Fall back to synchronous
        switch (op) {
            case FILE_OP_COPY:
                return is_directory ? copy_directory_recursive(src_path, dst_path) :
                                    copy_file(src_path, dst_path);
            case FILE_OP_MOVE:
                return move_file_to_directory(src_path, dirname(strdup(dst_path)), NULL, 0);
            case FILE_OP_DELETE:
                return is_directory ? remove_directory_recursive(src_path) : unlink(src_path);
        }
    }
    
    if (pid == 0) {
        // ===== CHILD PROCESS - Perform the operation =====
        close(pipefd[0]);  // Close read end
        
        // Send START message immediately
        ProgressMessage msg = {
            .type = MSG_START,
            .start_time = time(NULL),
            .files_done = 0,
            .files_total = -1,
            .bytes_done = 0,
            .bytes_total = is_directory ? 0 : st.st_size
        };
        strncpy(msg.current_file, basename(strdup(src_path)), NAME_SIZE - 1);
        
        // Copy icon metadata if provided
        if (icon_metadata) {
            msg.create_icon = icon_metadata->create_icon;
            msg.has_sidecar = icon_metadata->has_sidecar;
            msg.icon_x = icon_metadata->icon_x;
            msg.icon_y = icon_metadata->icon_y;
            msg.target_window = icon_metadata->target_window;
            strncpy(msg.dest_path, icon_metadata->dest_path, PATH_SIZE - 1);
            strncpy(msg.dest_dir, icon_metadata->dest_dir, PATH_SIZE - 1);
            strncpy(msg.sidecar_src, icon_metadata->sidecar_src, PATH_SIZE - 1);
            strncpy(msg.sidecar_dst, icon_metadata->sidecar_dst, PATH_SIZE - 1);
        }
        
        write(pipefd[1], &msg, sizeof(msg));
        
        int result = 0;
        
        switch (op) {
            case FILE_OP_COPY:
                if (is_directory) {
                    int total_files = 0;
                    count_files_in_directory(src_path, &total_files);
                    
                    CopyProgress progress = {
                        .total_files = total_files,
                        .files_processed = 0,
                        .dialog = NULL,
                        .abort = false,
                        .pipe_fd = pipefd[1]
                    };
                    result = copy_directory_recursive_with_progress(src_path, dst_path, &progress);
                } else {
                    result = copy_file_with_progress(src_path, dst_path, pipefd[1]);
                }
                break;
                
            case FILE_OP_MOVE:
                if (rename(src_path, dst_path) == 0) {
                    result = 0;
                } else if (errno == EXDEV) {
                    if (is_directory) {
                        int total_files = 0;
                        count_files_in_directory(src_path, &total_files);
                        
                        CopyProgress progress = {
                            .total_files = total_files,
                            .files_processed = 0,
                            .dialog = NULL,
                            .abort = false,
                            .pipe_fd = pipefd[1]
                        };
                        result = copy_directory_recursive_with_progress(src_path, dst_path, &progress);
                        if (result == 0) {
                            result = remove_directory_recursive(src_path);
                        }
                    } else {
                        result = copy_file_with_progress(src_path, dst_path, pipefd[1]);
                        if (result == 0) {
                            result = unlink(src_path);
                        }
                    }
                } else {
                    result = -1;
                }
                break;
                
            case FILE_OP_DELETE:
                if (is_directory) {
                    result = remove_directory_recursive(src_path);
                } else {
                    result = unlink(src_path);
                }
                break;
        }
        
        // Send completion message with icon metadata
        msg.type = (result == 0) ? MSG_COMPLETE : MSG_ERROR;
        write(pipefd[1], &msg, sizeof(msg));
        close(pipefd[1]);
        _exit(result);
    }
    
    // ===== PARENT PROCESS - Return immediately =====
    close(pipefd[1]);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    
    ProgressOperation prog_op = (op == FILE_OP_COPY) ? PROGRESS_COPY :
                                (op == FILE_OP_MOVE) ? PROGRESS_MOVE : PROGRESS_DELETE;
    
    const char *title = custom_title;
    if (!title) {
        switch (op) {
            case FILE_OP_COPY: title = "Copying Files..."; break;
            case FILE_OP_MOVE: title = "Moving Files..."; break;
            case FILE_OP_DELETE: title = "Deleting Files..."; break;
        }
    }
    
    ProgressDialog *dialog = calloc(1, sizeof(ProgressDialog));
    if (!dialog) {
        close(pipefd[0]);
        int status;
        waitpid(pid, &status, 0);
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    
    dialog->operation = prog_op;
    dialog->pipe_fd = pipefd[0];
    dialog->child_pid = pid;
    dialog->start_time = time(NULL);  // Track when operation started
    dialog->canvas = NULL;
    dialog->percent = -1.0f;  // Indicate not started yet
    strncpy(dialog->current_file, basename(strdup(src_path)), PATH_SIZE - 1);
    
    extern void add_progress_dialog_to_list(ProgressDialog *dialog);
    add_progress_dialog_to_list(dialog);
    
    return 0;
}

// Wrapper for backward compatibility
int perform_file_operation_with_progress(
    FileOperation op,
    const char *src_path,
    const char *dst_path,
    const char *custom_title
) {
    return perform_file_operation_with_progress_ex(op, src_path, dst_path, custom_title, NULL);
}

// ============================================================================
// Progress Dialog Monitoring - Called from main event loop
// ============================================================================

void workbench_check_progress_dialogs(void) {
    extern ProgressDialog* get_all_progress_dialogs(void);  // From dialogs.c
    extern Canvas* create_progress_window(ProgressOperation op, const char *title);  // From dialogs.c
    ProgressDialog *dialog = get_all_progress_dialogs();
    time_t now = time(NULL);
    
    
    while (dialog) {
        ProgressDialog *next = dialog->next;  // Save next before potential deletion
        
        if (dialog->pipe_fd > 0) {
            // Check for messages from child
            ProgressMessage msg;
            ssize_t bytes_read = read(dialog->pipe_fd, &msg, sizeof(msg));
            
            if (bytes_read == sizeof(msg)) {
                // Process message from child process
                // Mark as started when we get first message
                if (dialog->percent < 0) {
                    dialog->percent = 0.0f;  // Mark as started
                }
                
                if (msg.type == MSG_START) {
                    // Handle START message - DON'T update start_time from child, keep parent's original
                    // dialog->start_time = msg.start_time;  // BUG: This makes diff always 0!
                    dialog->percent = 0.0f;
                    strncpy(dialog->current_file, msg.current_file, PATH_SIZE - 1);
                    
                    // Create window if threshold has passed
                    if (!dialog->canvas && now - dialog->start_time >= PROGRESS_DIALOG_THRESHOLD) {
                        const char *title = dialog->operation == PROGRESS_COPY ? "Copying Files..." :
                                          dialog->operation == PROGRESS_MOVE ? "Moving Files..." :
                                          dialog->operation == PROGRESS_DELETE ? "Deleting Files..." :
                                          dialog->operation == PROGRESS_EXTRACT ? "Extracting Archive..." :
                                          "Processing...";
                        dialog->canvas = create_progress_window(dialog->operation, title);
                        if (dialog->canvas) {
                            update_progress_dialog(dialog, dialog->current_file, 0.0f);
                        } else {
                            log_error("[ERROR] Failed to create progress window");
                        }
                    }
                } else if (msg.type == MSG_PROGRESS) {
                    // Calculate percent - prioritize bytes for smooth progress on large files
                    float percent = 0.0f;
                    if (msg.bytes_total > 0) {
                        percent = (float)msg.bytes_done / msg.bytes_total * 100.0f;
                    } else if (msg.files_total > 0) {
                        percent = (float)msg.files_done / msg.files_total * 100.0f;
                    }
                    
                    // Create window if 1 second has passed and window doesn't exist
                    if (!dialog->canvas && dialog->start_time > 0) {
                        if (now - dialog->start_time >= PROGRESS_DIALOG_THRESHOLD) {
                            // Create the actual dialog window now
                            const char *title = dialog->operation == PROGRESS_COPY ? "Copying Files..." :
                                              dialog->operation == PROGRESS_MOVE ? "Moving Files..." :
                                              dialog->operation == PROGRESS_DELETE ? "Deleting Files..." :
                                              dialog->operation == PROGRESS_EXTRACT ? "Extracting Archive..." :
                                              "Processing...";
                            dialog->canvas = create_progress_window(dialog->operation, title);
                            if (dialog->canvas) {
                                update_progress_dialog(dialog, msg.current_file, percent);
                            }
                        } else {
                        }
                    } else if (dialog->canvas) {
                        // Update existing dialog
                        update_progress_dialog(dialog, msg.current_file, percent);
                    }
                    
                    dialog->percent = percent;
                    strncpy(dialog->current_file, msg.current_file, PATH_SIZE - 1);
                    
                } else if (msg.type == MSG_COMPLETE || msg.type == MSG_ERROR) {
                    // Operation finished
                    // Operation finished - check if we need to create icons
                    
                    // If extraction succeeded, create icon for the extracted directory
                    if (msg.type == MSG_COMPLETE && dialog->operation == PROGRESS_EXTRACT && 
                        !msg.create_icon && strlen(msg.dest_path) > 0 && msg.target_window != None) {
                        // This is an extraction operation - create icon for the directory we extracted into
                        // This is an extraction operation - create icon for extracted directory
                        
                        // Verify the directory was actually created
                        struct stat st;
                        if (stat(msg.dest_path, &st) == 0) {
                            // Directory was successfully created
                        } else {
                            log_error("[ERROR] Directory does not exist: %s (errno=%d: %s)", 
                                     msg.dest_path, errno, strerror(errno));
                        }
                        
                        Canvas *canvas = find_canvas(msg.target_window);
                        if (canvas) {
                            // Found the target canvas for icon creation
                            // Get the directory name from the path
                            const char *dir_name = strrchr(msg.dest_path, '/');
                            dir_name = dir_name ? dir_name + 1 : msg.dest_path;
                            // Extract directory name for icon label
                            
                            // Get the def_dir.info icon path for directories
                            const char *icon_path = definfo_for_file(dir_name, true);
                            if (!icon_path) {
                                log_error("[ERROR] No def_dir.info available for directory icon");
                                break;
                            }
                            // Got appropriate icon for directory
                            
                            // Find a free spot for the new directory icon
                            int new_x, new_y;
                            find_free_slot(canvas, &new_x, &new_y);
                            // Found position for new icon
                            
                            // Create the directory icon using the proper metadata function
                            // icon_path = def_dir.info, msg.dest_path = actual directory, dir_name = label
                            // Create icon with proper metadata
                            FileIcon *new_icon = create_icon_with_metadata(icon_path, canvas, new_x, new_y, 
                                                    msg.dest_path, dir_name, TYPE_DRAWER);
                            
                            if (new_icon) {
                                // Icon created successfully - update canvas to show it
                                compute_content_bounds(canvas);
                                compute_max_scroll(canvas);
                                redraw_canvas(canvas);
                                // Canvas updated with new icon
                            } else {
                                log_error("[ERROR] Failed to create icon for extracted directory: %s", msg.dest_path);
                            }
                        } else {
                            log_error("[ERROR] Canvas not found for window 0x%lx - cannot create extracted directory icon", msg.target_window);
                        }
                    }
                    
                    // If copy succeeded and we have icon metadata, create the icon now
                    if (msg.type == MSG_COMPLETE && msg.create_icon && strlen(msg.dest_path) > 0) {
                        // Copy sidecar if needed (small file, do synchronously)
                        if (msg.has_sidecar && strlen(msg.sidecar_src) > 0 && strlen(msg.sidecar_dst) > 0) {
                            copy_file(msg.sidecar_src, msg.sidecar_dst);
                        }
                        
                        // Find the target canvas by window
                        Canvas *target = NULL;
                        if (msg.target_window != None) {
                            target = find_canvas(msg.target_window);
                        }
                        
                        if (target) {
                            // Determine file type NOW (after copy is done)
                            struct stat st;
                            bool is_dir = (stat(msg.dest_path, &st) == 0 && S_ISDIR(st.st_mode));
                            int file_type = is_dir ? TYPE_DRAWER : TYPE_FILE;
                            
                            // Get appropriate icon path
                            const char *icon_path = NULL;
                            const char *filename = strrchr(msg.dest_path, '/');
                            filename = filename ? filename + 1 : msg.dest_path;
                            
                            if (msg.has_sidecar && strlen(msg.sidecar_dst) > 0) {
                                icon_path = msg.sidecar_dst;
                            } else {
                                icon_path = definfo_for_file(filename, is_dir);
                            }
                            
                            if (icon_path) {
                                // Create the icon at the specified position
                                create_icon_with_metadata(icon_path, target, msg.icon_x, msg.icon_y,
                                                        msg.dest_path, filename, file_type);
                                
                                // Apply layout if in list view
                                if (target->view_mode == VIEW_NAMES) {
                                    apply_view_layout(target);
                                }
                                
                                // Refresh display
                                compute_content_bounds(target);
                                compute_max_scroll(target);
                                redraw_canvas(target);
                            }
                        }
                    }
                    
                    close(dialog->pipe_fd);
                    dialog->pipe_fd = -1;
                    if (dialog->canvas) {
                        close_progress_dialog(dialog);
                    } else {
                        // No window was created, just free the structure
                        extern void remove_progress_dialog_from_list(ProgressDialog *dialog);
                        remove_progress_dialog_from_list(dialog);
                        free(dialog);
                    }
                    dialog = next;
                    continue;
                }
            }
        }
        
        // Check if we need to create progress window based on elapsed time
        // This handles the case where no messages arrive but time has passed
        if (!dialog->canvas && dialog->start_time > 0 && dialog->percent >= 0) {
            // Dialog has started (percent >= 0) but no window yet
            if (now - dialog->start_time >= PROGRESS_DIALOG_THRESHOLD) {
                
                // Determine appropriate title based on operation
                const char *title = "Processing...";
                if (dialog->operation == PROGRESS_COPY) {
                    title = "Copying Files...";
                } else if (dialog->operation == PROGRESS_MOVE) {
                    title = "Moving Files...";
                } else if (dialog->operation == PROGRESS_DELETE) {
                    title = "Deleting Files...";
                } else if (dialog->operation == PROGRESS_EXTRACT) {
                    title = "Extracting Archive...";
                }
                
                dialog->canvas = create_progress_window(dialog->operation, title);
                if (dialog->canvas) {
                    // Update with current state
                    float percent = 0.0f;
                    if (dialog->percent > 0) {
                        percent = dialog->percent;
                    }
                    update_progress_dialog(dialog, dialog->current_file, percent);
                } else {
                    log_error("[ERROR] Failed to create progress window from timer check");
                }
            }
        }
        
        // Check if child process finished
        if (dialog->child_pid > 0) {
            int status;
            pid_t wait_result = waitpid(dialog->child_pid, &status, WNOHANG);
            if (wait_result == dialog->child_pid) {
                // Child finished
                if (dialog->pipe_fd > 0) {
                    close(dialog->pipe_fd);
                    dialog->pipe_fd = -1;
                }
                if (dialog->canvas) {
                    close_progress_dialog(dialog);
                } else {
                    // No window was created, just free the structure
                    extern void remove_progress_dialog_from_list(ProgressDialog *dialog);
                    remove_progress_dialog_from_list(dialog);
                    free(dialog);
                }
                dialog = next;
                continue;
            }
        }
        
        dialog = next;
    }
}

// Workbench API expected by other modules

// Manage dynamic icon array
static FileIcon *manage_icons(bool add, FileIcon *icon_to_remove) {
    if (add) {
        if (icon_count >= icon_array_size) {
            icon_array_size = icon_array_size ? icon_array_size * 2 : INITIAL_ICON_CAPACITY;
            FileIcon **new_icons = realloc(icon_array, icon_array_size * sizeof(FileIcon *));
            if (!new_icons) {
                log_error("[ERROR] realloc failed for icon_array (new size=%d)", icon_array_size);
                return NULL;
            }
            icon_array = new_icons;
        }
        FileIcon *new_icon = calloc(1, sizeof(FileIcon));
        if (!new_icon) {
            log_error("[ERROR] calloc failed for FileIcon structure");
            return NULL;
        }
        icon_array[icon_count++] = new_icon;
        return new_icon;
    } else if (icon_to_remove) {
        for (int i = 0; i < icon_count; i++) {
            if (icon_array[i] == icon_to_remove) {
                memmove(&icon_array[i], &icon_array[i + 1], (icon_count - i - 1) * sizeof(FileIcon *));
                icon_count--;
                break;
            }
        }
    }
    return NULL;
}

int get_icon_count(void) { return icon_count; }
FileIcon **get_icon_array(void) { return icon_array; }

FileIcon *get_selected_icon(void) {
    for (int i = 0; i < icon_count; i++) {
        if (icon_array[i] && icon_array[i]->selected) {
            return icon_array[i];
        }
    }
    return NULL;
}

// Get selected icon from a specific canvas window
FileIcon *get_selected_icon_from_canvas(Canvas *canvas) {
    if (!canvas) return NULL;
    
    for (int i = 0; i < icon_count; i++) {
        if (icon_array[i] && icon_array[i]->selected && 
            icon_array[i]->display_window == canvas->win) {
            return icon_array[i];
        }
    }
    return NULL;
}

// Collect icons displayed on a given canvas into a newly allocated array; returns count via out param
static FileIcon **icons_for_canvas(Canvas *canvas, int *out_count) {
    if (!canvas) {
        log_error("[ERROR] icons_for_canvas called with NULL canvas");
        return NULL;
    }
    if (!out_count) {
        log_error("[ERROR] icons_for_canvas called with NULL out_count");
        return NULL;
    }
    int count = 0; for (int i = 0; i < icon_count; ++i) if (icon_array[i] && icon_array[i]->display_window == canvas->win) ++count;
    *out_count = count; if (count == 0) return NULL;
    FileIcon **list = (FileIcon**)malloc(sizeof(FileIcon*) * count);
    if (!list) {
        log_error("[ERROR] malloc failed for icon list (count=%d)", count);
        return NULL;
    }
    int k = 0; for (int i = 0; i < icon_count; ++i) { FileIcon *ic = icon_array[i]; if (ic && ic->display_window == canvas->win) list[k++] = ic; }
    return list;
}

// Remove first icon on a given canvas whose absolute path matches
static void remove_icon_by_path_on_canvas(const char *abs_path, Canvas *canvas) {
    if (!abs_path) {
        log_error("[ERROR] remove_icon_by_path_on_canvas called with NULL abs_path");
        return;
    }
    if (!canvas) {
        log_error("[ERROR] remove_icon_by_path_on_canvas called with NULL canvas");
        return;
    }
    for (int i = 0; i < icon_count; ++i) {
        FileIcon *ic = icon_array[i];
        if (!ic) continue;
        if (ic->display_window != canvas->win) continue;
        if (ic->path && strcmp(ic->path, abs_path) == 0) { destroy_icon(ic); break; }
    }
}

// Create icon with explicit type (useful when file doesn't exist yet)
void create_icon_with_type(const char *path, Canvas *canvas, int x, int y, int type) {
    FileIcon *icon = manage_icons(true, NULL);
    if (!icon) {
        log_error("[ERROR] manage_icons failed to create new icon");
        return;
    }
    icon->path = strdup(path);
    if (!icon->path) {
        log_error("[ERROR] strdup failed for icon path '%s'", path);
        free(icon);
        return;
    }
    const char *base = strrchr(path, '/');
    icon->label = strdup(base ? base + 1 : path);
    if (!icon->label) {
        log_error("[ERROR] strdup failed for icon label");
        free(icon->path);
        free(icon);
        return;
    }
    icon->type = type;  // Use provided type instead of stat()
    icon->x = x; icon->y = y;
    icon->display_window = canvas->win;
    icon->selected = false;
    icon->last_click_time = 0;
    icon->iconified_canvas = NULL;
    create_icon_images(icon, get_render_context());
    icon->current_picture = icon->normal_picture;
}

// Original create_icon - determines type from filesystem
void create_icon(const char *path, Canvas *canvas, int x, int y) {
    struct stat st;
    int type = TYPE_FILE;  // Default to file if stat fails
    if (stat(path, &st) == 0) {
        type = S_ISDIR(st.st_mode) ? TYPE_DRAWER : TYPE_FILE;
    }
    create_icon_with_type(path, canvas, x, y, type);
}

void destroy_icon(FileIcon *icon) {
    if (!icon) return;
    // If this icon is currently being dragged, cancel the drag safely to avoid UAF
    if (icon == dragged_icon) {
        destroy_drag_window();
        dragged_icon = NULL;
        drag_active = false;
        drag_source_canvas = NULL;
        saved_source_window = None;
    }
    free_icon(icon);
    if (icon->path) free(icon->path);
    if (icon->label) free(icon->label);
    manage_icons(false, icon);
    free(icon);
}

void clear_canvas_icons(Canvas *canvas) {
    for (int i = icon_count - 1; i >= 0; i--) {
        if (icon_array[i]->display_window == canvas->win) {
            // Keep iconified window icons and device icons on the desktop during refresh
            if (icon_array[i]->type == TYPE_ICONIFIED || 
                icon_array[i]->type == TYPE_DEVICE) {
                continue;
            }
            destroy_icon(icon_array[i]);
        }
    }
}

void compute_content_bounds(Canvas *canvas) {
    if (!canvas) return;
    
    // For Names view, calculate based on text width, not icon width
    if (canvas->type == WINDOW && canvas->view_mode == VIEW_NAMES) {
        int max_text_w = 0;
        int max_y = 0;
        for (int i = 0; i < icon_count; i++) {
            if (icon_array[i]->display_window == canvas->win) {
                int lw = get_text_width(icon_array[i]->label ? icon_array[i]->label : "");
                if (lw > max_text_w) max_text_w = lw;
                max_y = max(max_y, icon_array[i]->y + 24); // row height is 24 in Names view
            }
        }
        int padding = 16; // selection + text left pad (same as apply_view_layout)
        int visible_w = canvas->width - BORDER_WIDTH_LEFT - get_right_border_width(canvas);
        canvas->content_width = max(visible_w, max_text_w + padding);
        canvas->content_height = max_y + 10;
    } else {
        // Icons view: use icon bounds INCLUDING label width
        int max_x = 0, max_y = 0;
        for (int i = 0; i < icon_count; i++) {
            if (icon_array[i]->display_window == canvas->win) {
                FileIcon *icon = icon_array[i];
                
                // Calculate the actual right edge including the label
                // Label is centered under icon, so it can extend beyond icon bounds
                int icon_right = icon->x + icon->width;
                
                // Get label width if we have a label
                int label_width = 0;
                if (icon->label) {
                    label_width = get_text_width(icon->label);
                }
                
                // Label is centered, so calculate where its right edge would be
                int label_center_x = icon->x + icon->width / 2;
                int label_right = label_center_x + label_width / 2;
                
                // Use whichever extends further right
                int actual_right = max(icon_right, label_right);
                max_x = max(max_x, actual_right);
                
                max_y = max(max_y, icon->y + icon->height + 20);
            }
        }
        // Add small padding for visual comfort, not 80 pixels
        canvas->content_width = max_x + 20;
        canvas->content_height = max_y + 10;
    }
}

// Convenience: recompute bounds, scroll ranges, and redraw
static void refresh_canvas(Canvas *canvas) {
    if (!canvas) return;
    compute_content_bounds(canvas);
    compute_max_scroll(canvas);
    redraw_canvas(canvas);
}

void move_icon(FileIcon *icon, int x, int y) {
    if (!icon) return;
    icon->x = max(0, x);
    icon->y = max(0, y);
}

// Comparison for sorting in icon_cleanup
static int icon_cmp(const void *a, const void *b) {
    FileIcon *ia = *(FileIcon **)a;
    FileIcon *ib = *(FileIcon **)b;
    // System always first
    if (strcmp(ia->label, "System") == 0) return -1;
    if (strcmp(ib->label, "System") == 0) return 1;
    // Home always second
    if (strcmp(ia->label, "Home") == 0) return -1;
    if (strcmp(ib->label, "Home") == 0) return 1;
    // Device drives come after System/Home but before everything else
    if (ia->type == TYPE_DEVICE && ib->type != TYPE_DEVICE) return -1;
    if (ia->type != TYPE_DEVICE && ib->type == TYPE_DEVICE) return 1;
    // Then drawers before files
    if (ia->type == TYPE_DRAWER && ib->type != TYPE_DRAWER) return -1;
    if (ia->type != TYPE_DRAWER && ib->type == TYPE_DRAWER) return 1;
    // Finally alphabetical
    return strcmp(ia->label, ib->label);
}

void icon_cleanup(Canvas *canvas) {
    if (!canvas) return;
    int count = 0; FileIcon **list = icons_for_canvas(canvas, &count);
    if (!list || count == 0) { refresh_canvas(canvas); return; }
    qsort(list, count, sizeof(FileIcon *), icon_cmp);
    int cell_h = ICON_SPACING; int label_space = 20; int min_cell_w = 80; char max_str[81]; memset(max_str, 'W', 80); max_str[80] = '\0';
    int max_allowed_w = get_text_width(max_str); int padding = 20;
    // visible_w is recalculated where needed (line 1043)
    // int visible_w = (canvas->type == WINDOW) ? (canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT) : canvas->width;
    int visible_h = (canvas->type == WINDOW) ? (canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM) : canvas->height;
    int start_x = (canvas->type == DESKTOP) ? 20 : 10; int start_y = (canvas->type == DESKTOP) ? 40 : 10;

    if (canvas->type == DESKTOP) {
        // Desktop uses vertical column layout
        int step_x = 110;  // Column width
        int step_y = 80;   // Row height
        int first_slot_y = 200;  // Start below Home icon (120 + 80)
        
        // Current position for placing icons
        int x = start_x;
        int y = first_slot_y;
        
        // Place all icons using column layout
        for (int i2 = 0; i2 < count; ++i2) {
            FileIcon *ic = list[i2];
            
            // System and Home get fixed positions at top of first column, but centered
            if (strcmp(ic->label, "System") == 0) {
                int column_center_offset = (step_x - ic->width) / 2;
                if (column_center_offset < 0) column_center_offset = 0;
                ic->x = 20 + column_center_offset; 
                ic->y = 40;
            } else if (strcmp(ic->label, "Home") == 0) {
                int column_center_offset = (step_x - ic->width) / 2;
                if (column_center_offset < 0) column_center_offset = 0;
                ic->x = 20 + column_center_offset; 
                ic->y = 120;
            } else {
                // All other icons (regular AND iconified) use column layout
                // Center the icon within its column slot
                int column_center_offset = (step_x - ic->width) / 2;
                if (column_center_offset < 0) column_center_offset = 0; // Don't offset if icon is wider than column
                
                ic->x = x + column_center_offset;
                ic->y = y;
                
                // Move to next slot vertically
                y += step_y;
                
                // If we've reached bottom of screen, move to next column
                if (y + 64 > canvas->height) {
                    x += step_x;
                    y = first_slot_y;
                }
            }
        }
        free(list);
    } else {
        int num_rows = max(1, (visible_h - start_y - 0) / cell_h);
        int num_columns = (count + num_rows - 1) / num_rows;
        // WINDOW canvases: keep centered columns sized by max label width for readability
        int *col_widths = malloc(num_columns * sizeof(int)); 
        if (!col_widths) { 
            log_error("[ERROR] malloc failed for col_widths (num_columns=%d)", num_columns);
            return;
        }
        for (int col = 0; col < num_columns; col++) {
            int max_w_in_col = 0;
            for (int row = 0; row < num_rows; row++) {
                int i2 = col * num_rows + row; if (i2 >= count) break;
                int label_w = get_text_width(list[i2]->label);
                if (label_w > max_w_in_col) max_w_in_col = label_w;
            }
            col_widths[col] = max(min_cell_w, min(max_w_in_col + padding, max_allowed_w + padding));
        }
        int current_x = start_x;
        for (int col = 0; col < num_columns; col++) {
            int col_w = col_widths[col];
            for (int row = 0; row < num_rows; row++) {
                int i2 = col * num_rows + row; if (i2 >= count) break;
                int cell_y = start_y + row * cell_h;
                list[i2]->x = current_x + (col_w - list[i2]->width) / 2;
                list[i2]->y = cell_y + (cell_h - list[i2]->height - label_space);
            }
            current_x += col_w;
        }
        free(col_widths); free(list);
    }
    // After scan, lay out according to current view mode (names or icons)
    apply_view_layout(canvas);
    compute_max_scroll(canvas);
    redraw_canvas(canvas);
}

// ========================
// View mode layout helpers
// ========================
void apply_view_layout(Canvas *canvas) {
    if (!canvas) return;
    // Desktop remains icon grid; windows can switch
    if (canvas->type != WINDOW) {
        compute_content_bounds(canvas);
        return;
    }
    if (canvas->view_mode == VIEW_NAMES) {
        // Single list sorted: directories first, then files; both A..Z by label
        int count = 0; FileIcon **list = icons_for_canvas(canvas, &count); if (!list || count == 0) { compute_content_bounds(canvas); return; }
        qsort(list, count, sizeof(FileIcon*), dir_first_cmp);
        int x = 12, y = 10, row_h = 24, max_text_w = 0;
        for (int i = 0; i < count; ++i) {
            FileIcon *ic = list[i]; ic->x = x; ic->y = y; y += row_h;
            int lw = get_text_width(ic->label ? ic->label : ""); if (lw > max_text_w) max_text_w = lw;
        }
        free(list);
        int padding = 16; // selection + text left pad
        int visible_w = canvas->width - BORDER_WIDTH_LEFT - get_right_border_width(canvas);
        canvas->content_width = max(visible_w, max_text_w + padding);
        canvas->content_height = y + 10;
    } else {
        // Icon grid mode: keep current positions; just recompute bounds
        compute_content_bounds(canvas);
    }
}

void set_canvas_view_mode(Canvas *canvas, ViewMode m) {
    if (!canvas) return;
    if (canvas->view_mode == m) return;
    canvas->view_mode = m;
    // Reset scroll to start for consistent knob/size on mode changes
    canvas->scroll_x = 0; canvas->scroll_y = 0;
    if (m == VIEW_ICONS) {
        // Re-grid icons when returning to icon view
        icon_cleanup(canvas);
    }
    apply_view_layout(canvas);
    compute_max_scroll(canvas);
    redraw_canvas(canvas);
}

void remove_icon_for_canvas(Canvas *canvas) {
    if (!canvas) return;
    for (int i = 0; i < icon_count; i++) {
        FileIcon *ic = icon_array[i];
        if (ic && ic->type == TYPE_ICONIFIED && ic->iconified_canvas == canvas) {
            destroy_icon(ic);
            break;
        }
    }
}

// ========================
// Directory refresh / open
// ========================

static bool ends_with(const char *s, const char *suffix) { size_t l = strlen(s), m = strlen(suffix); return l >= m && strcmp(s + l - m, suffix) == 0; }

void refresh_canvas_from_directory(Canvas *canvas, const char *dirpath) {
    if (!canvas) return;
    // Resolve directory path for canvas
    char pathbuf[1024];
    const char *dir = dirpath;
    if (canvas->type == DESKTOP || !dir) {
        const char *home = getenv("HOME");
        snprintf(pathbuf, sizeof(pathbuf), "%s/Desktop", home ? home : ".");
        dir = pathbuf;
    }
    clear_canvas_icons(canvas);
    // Draw background immediately so long directory scans don't show black
    redraw_canvas(canvas);
    // Ensure compositor pushes the frame now
    XSync(get_display(), False);
    // Suppress icon rendering during long scan
    canvas->scanning = true;
    // Recreate prime desktop icons that must always exist
    if (canvas->type == DESKTOP) add_prime_desktop_icons(canvas);
    DIR *dirp = opendir(dir);
    if (dirp) {
        // Don't position icons - just create them at 0,0 and let icon_cleanup handle layout
        struct dirent *entry;
        while ((entry = readdir(dirp))) {
            // Skip current and parent directory entries always
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            // Skip hidden unless show_hidden is enabled
            if (entry->d_name[0] == '.' && !canvas->show_hidden) continue;
            char full_path[PATH_SIZE];
            int ret = snprintf(full_path, sizeof(full_path), "%s/%s", dir, entry->d_name);
            if (ret >= PATH_SIZE) {
                log_error("[ERROR] Path too long, skipping: %s/%s", dir, entry->d_name);
                continue;
            }
            if (ends_with(entry->d_name, ".info")) {
                char base[256]; size_t namelen = strlen(entry->d_name);
                size_t blen = namelen > 5 ? namelen - 5 : 0; strncpy(base, entry->d_name, blen); base[blen] = '\0';
                char base_path[PATH_SIZE];
                ret = snprintf(base_path, sizeof(base_path), "%s/%s", dir, base);
                if (ret >= PATH_SIZE) {
                    log_error("[ERROR] Base path too long, skipping: %s/%s", dir, base);
                    continue;
                }
                struct stat st; if (stat(base_path, &st) != 0) {
                    // Orphan .info - create at 0,0, icon_cleanup will position it
                    create_icon_with_metadata(full_path, canvas, 0, 0, full_path, entry->d_name, TYPE_FILE);
                }
            } else {
                    // Determine file type using helper function
                int t = determine_file_type_from_path(full_path);
                // Check for sidecar .info using helper function
                char info_path[1024];
                build_info_file_path(dir, entry->d_name, info_path, sizeof(info_path));
                int has_sidecar = check_if_file_exists(info_path);
                // Choose icon path: sidecar .info, else deficon by type/suffix (drawer or file), else the file itself
                const char *fallback_def = (!has_sidecar) ? definfo_for_file(entry->d_name, (t == TYPE_DRAWER)) : NULL;
                const char *icon_path = has_sidecar ? info_path : (fallback_def ? fallback_def : full_path);
                
                /*
                // Debug: show decision for this entry
                printf("\n[deficons] entry: %s  \n has_sidecar=%s \n  fallback_def=%s  \n chosen=%s\n\n",
                       full_path,
                       has_sidecar ? info_path : "(none)",
                       fallback_def ? fallback_def : "(none)",
                       icon_path);
                */
                // Create at 0,0 - icon_cleanup will position properly
                create_icon_with_metadata(icon_path, canvas, 0, 0, full_path, entry->d_name, t);
            }
        }
        closedir(dirp);
    } else {
        log_error("[ERROR] Failed to open directory %s", dir);
    }
    canvas->scanning = false;
    // Always call icon_cleanup to properly sort and layout all icons
    icon_cleanup(canvas);
}

static void open_directory(FileIcon *icon, Canvas *current_canvas) {
    if (!icon || !icon->path) return;
    
    // Non-spatial mode: reuse current window if we have one
    if (!get_spatial_mode() && current_canvas && current_canvas->type == WINDOW) {
        // Store the new path in a temporary variable first
        char *new_path = strdup(icon->path);
        
        // Update window title
        const char *dir_name = strrchr(new_path, '/');
        if (dir_name) dir_name++; else dir_name = new_path;
        
        // Free old paths
        if (current_canvas->path) free(current_canvas->path);
        if (current_canvas->title_base) free(current_canvas->title_base);
        
        // Set new paths
        current_canvas->path = new_path;
        current_canvas->title_base = strdup(dir_name);
        
        // Clear existing icons and refresh with new directory
        refresh_canvas_from_directory(current_canvas, current_canvas->path);
        
        // Reset scroll position for new directory
        current_canvas->scroll_x = 0;
        current_canvas->scroll_y = 0;
        
        // Initial placement for reused window (same as new window)
        icon_cleanup(current_canvas);
        redraw_canvas(current_canvas);
        return;
    }
    
    // Spatial mode or no current window: check if window for this path exists
    Canvas *existing = find_window_by_path(icon->path);
    if (existing) {
        // Check if window is iconified (not visible)
        XWindowAttributes attrs;
        if (XGetWindowAttributes(get_display(), existing->win, &attrs)) {
            if (attrs.map_state != IsViewable) {
                // Window is iconified - find and restore it
                FileIcon **icon_array = get_icon_array();
                int icon_count = get_icon_count();
                for (int i = 0; i < icon_count; i++) {
                    FileIcon *ic = icon_array[i];
                    if (ic && ic->type == TYPE_ICONIFIED && ic->iconified_canvas == existing) {
                        restore_iconified(ic);
                        return;
                    }
                }
            }
        }
        // Window is already visible - just raise and activate it
        set_active_window(existing);
        XRaiseWindow(get_display(), existing->win);
        redraw_canvas(existing);
        return;
    }
    // Otherwise create a new window
    Canvas *new_canvas = create_canvas(icon->path, 150, 100, 400, 300, WINDOW);
    if (new_canvas) {
        refresh_canvas_from_directory(new_canvas, icon->path);
        // icon_cleanup now called inside refresh_canvas_from_directory
        redraw_canvas(new_canvas);
        // Make the newly created window active (raise on top)
        set_active_window(new_canvas);
    }
}

// Public function to open directory by path (for IPC from ReqASL)
void workbench_open_directory(const char *path) {
    if (!path || !path[0]) return;
    
    // Create temporary icon just to reuse open_directory
    FileIcon temp_icon = {0};
    temp_icon.path = (char *)path;
    temp_icon.type = TYPE_DRAWER;
    
    // Call existing function
    open_directory(&temp_icon, NULL);
}

FileIcon *find_icon(Window win, int x, int y) {
    if (!icon_array || icon_count <= 0) return NULL;
    Canvas *c = find_canvas(win);
    int base_x = 0, base_y = 0, sx = 0, sy = 0;
    if (c) {
        if (c->type == WINDOW) { base_x = BORDER_WIDTH_LEFT; base_y = BORDER_HEIGHT_TOP; }
        sx = c->scroll_x; sy = c->scroll_y;
    }
    // Iterate from topmost to bottom by scanning in reverse; later icons are typically drawn on top
    for (int i = icon_count - 1; i >= 0; i--) {
        FileIcon *ic = icon_array[i];
        if (ic->display_window != win) continue;
        int rx = base_x + ic->x - sx;
        int ry = base_y + ic->y - sy;
        if (c && c->type == WINDOW && c->view_mode == VIEW_NAMES) {
            // Only the label text area is selectable in list view
            int row_h = 18 + 6; // keep in sync with apply_view_layout/render
            int text_left_pad = 6; // matches apply_view_layout padding usage
            int text_x = base_x + ic->x + text_left_pad;
            int text_w = get_text_width(ic->label ? ic->label : "");
            if (x >= text_x && x <= text_x + text_w && y >= ry && y <= ry + row_h) return ic;
        } else {
            int w = ic->width;
            int h = ic->height;
            int label_pad = 20; // include label area under the icon
            if (x >= rx && x <= rx + w && y >= ry && y <= ry + h + label_pad) return ic;
        }
    }
    return NULL;
}

// Launch a program with ReqASL hook preloaded
// Command should be a full shell command like "leafpad file.txt" or "xdg-open file.pdf"
void launch_with_hook(const char *command) {
    if (!command || !*command) return;
    
    pid_t pid = fork();
    if (pid == -1) {
        log_error("[ERROR] fork failed for command: %s", command);
        return;
    } else if (pid == 0) {
        // Child process
        // Close file descriptors to detach from parent (but keep stdin/stdout/stderr)
        for (int i = 3; i < 256; i++) {
            close(i);
        }
        
        // Always inject ReqASL hook for GUI applications
        setenv("LD_PRELOAD", REQASL_HOOK_PATH, 1);
        
        // Execute through shell to handle arguments properly
        execl("/bin/sh", "sh", "-c", command, NULL);
        
        // If exec fails - this won't be visible due to /dev/null redirection
        _exit(EXIT_FAILURE);
    }
    // Parent continues without waiting
}

void open_file(FileIcon *icon) {
    if (!icon || !icon->path) return;
    // Directories and devices should be opened within AmiWB (safety guard)
    if (icon->type == TYPE_DRAWER || icon->type == TYPE_DEVICE) {
        Canvas *c = find_canvas(icon->display_window);
        if (c) open_directory(icon, c);
        return;
    }
    
    // Build xdg-open command and launch with hook
    char command[PATH_SIZE * 2 + 32];
    snprintf(command, sizeof(command), "xdg-open '%s'", icon->path);
    launch_with_hook(command);
}

// Find the next available desktop slot for iconified windows
static void find_next_desktop_slot(Canvas *desk, int *ox, int *oy) {
    if (!desk || !ox || !oy) return;
    const int sx = 20, step_x = 110;
    
    FileIcon **arr = get_icon_array(); 
    int n = get_icon_count();
    
    // Calculate proper start position: Home icon top + 80px gap
    int first_iconified_y = 120 + 80;  // Home icon y + 80px gap (same as System->Home)
    
    // Find next slot starting from the correct position
    for (int x = sx; x < desk->width - 64; x += step_x) {
        int y = first_iconified_y;
        
        // Keep checking for collisions until we find a free slot
        bool collision_found;
        do {
            collision_found = false;
            for (int i = 0; i < n; i++) {
                FileIcon *ic = arr[i];
                if (ic->display_window != desk->win) continue;
                // Check collision with ANY icon type (file, drawer, or iconified)
                // Check if icons would overlap in the same column slot
                // Icons in same column if their x positions are within the column range
                bool same_column = (ic->x >= x && ic->x < x + step_x) || 
                                  (x >= ic->x && x < ic->x + ic->width);
                if (same_column && ic->y == y) {
                    y += 80;  // Move down and check again
                    collision_found = true;
                    break;  // Start collision check over from beginning
                }
            }
        } while (collision_found && y + 64 < desk->height);
        
        if (y + 64 < desk->height) { 
            *ox = x; *oy = y; return; 
        }
    }
    *ox = sx; *oy = first_iconified_y;
}

// Helper function to find an icon file, checking user directory first, then system
static const char* find_icon_with_user_override(const char *icon_name, char *buffer, size_t buffer_size) {
    struct stat st;
    
    // First check user directory (~/.config/amiwb/icons/)
    const char *home = getenv("HOME");
    if (home) {
        snprintf(buffer, buffer_size, "%s/.config/amiwb/icons/%s", home, icon_name);
        if (stat(buffer, &st) == 0) {
            log_error("[ICON] Using user icon: %s", buffer);
            return buffer;
        }
    }
    
    // Then check system directory (/usr/local/share/amiwb/icons/)
    snprintf(buffer, buffer_size, "/usr/local/share/amiwb/icons/%s", icon_name);
    if (stat(buffer, &st) == 0) {
        return buffer;
    }
    
    return NULL;
}

// Create an iconified icon for a canvas window
// This handles all the icon path selection logic and icon creation
FileIcon* create_iconified_icon(Canvas *c) {
    if (!c || (c->type != WINDOW && c->type != DIALOG)) return NULL;
    
    Canvas *desk = get_desktop_canvas();
    if (!desk) return NULL;
    
    // Find next available desktop slot
    int nx = 20, ny = 40;
    find_next_desktop_slot(desk, &nx, &ny);
    
    const char *icon_path = NULL;
    char *label = NULL;
    const char *def_foo_path = "/usr/local/share/amiwb/icons/def_icons/def_foo.info";
    
    // Use the Canvas's title_base which was already set correctly when the window was created
    label = c->title_base ? strdup(c->title_base) : strdup("Untitled");
    
    // Buffer for icon path searches
    char icon_buffer[512];
    
    if (c->client_win == None) { 
        // For workbench windows and dialogs without client
        if (c->type == DIALOG) {
            // Use specific icons for different dialog types based on title
            const char *dialog_icon_name = NULL;
            if (c->title_base) {
                if (strstr(c->title_base, "Rename")) {
                    dialog_icon_name = "rename.info";
                } else if (strstr(c->title_base, "Delete")) {
                    dialog_icon_name = "delete.info";
                } else if (strstr(c->title_base, "Execute")) {
                    dialog_icon_name = "execute.info";
                } else if (strstr(c->title_base, "Progress") || strstr(c->title_base, "Copying") || strstr(c->title_base, "Moving")) {
                    dialog_icon_name = "progress.info";
                } else if (strstr(c->title_base, "Information")) {
                    dialog_icon_name = "iconinfo.info";
                } else {
                    dialog_icon_name = "dialog.info";  // Generic dialog icon
                }
            } else {
                dialog_icon_name = "dialog.info";
            }
            
            // Try to find the dialog icon with user override support
            icon_path = find_icon_with_user_override(dialog_icon_name, icon_buffer, sizeof(icon_buffer));
            
            // If not found, try generic dialog icon
            if (!icon_path) {
                icon_path = find_icon_with_user_override("dialog.info", icon_buffer, sizeof(icon_buffer));
            }
            
            // If still not found, fall back to filer icon
            if (!icon_path) {
                icon_path = find_icon_with_user_override("filer.info", icon_buffer, sizeof(icon_buffer));
            }
        } else {
            // Regular workbench window
            icon_path = find_icon_with_user_override("filer.info", icon_buffer, sizeof(icon_buffer));
        }
    } else {
        // Try to find a specific icon for this app using the title_base
        char app_icon_name[256];
        snprintf(app_icon_name, sizeof(app_icon_name), "%s.info", c->title_base);
        
        icon_path = find_icon_with_user_override(app_icon_name, icon_buffer, sizeof(icon_buffer));
        
        if (!icon_path) {
            log_error("[ICON] Couldn't find %s in user or system directories, using def_foo.info", app_icon_name);
            icon_path = def_foo_path;
        }
    }
    
    // Verify the icon path exists, use def_foo as ultimate fallback
    struct stat st;
    if (stat(icon_path, &st) != 0) {
        log_error("[WARNING] Icon file not found: %s, using def_foo.info", icon_path);
        icon_path = def_foo_path;
    }
    
    // Create the icon
    create_icon(icon_path, desk, nx, ny);
    FileIcon **ia = get_icon_array();
    FileIcon *ni = ia[get_icon_count() - 1];
    
    // Ensure we actually got an icon, this is critical
    if (!ni) {
        log_error("[ERROR] Failed to create iconified icon for window, using emergency fallback");
        // Try one more time with def_foo
        create_icon(def_foo_path, desk, nx, ny);
        ia = get_icon_array();
        ni = ia[get_icon_count() - 1];
        if (!ni) {
            log_error("[ERROR] CRITICAL: Cannot create iconified icon - window will be lost!");
            free(label);
            return NULL;
        }
    }
    
    // Set up the iconified icon
    ni->type = TYPE_ICONIFIED;
    free(ni->label);
    ni->label = label;
    free(ni->path);
    ni->path = NULL;
    ni->iconified_canvas = c;
    
    // Center the iconified icon within its column slot (same as cleanup does)
    const int step_x = 110;  // Column width
    int column_center_offset = (step_x - ni->width) / 2;
    if (column_center_offset < 0) column_center_offset = 0;
    ni->x = nx + column_center_offset;
    
    return ni;
}

void restore_iconified(FileIcon *icon) {
    if (!icon || icon->type != TYPE_ICONIFIED) {
        return;
    }
    Canvas *canvas = icon->iconified_canvas;
    if (!canvas) {
        return;
    }

    // Remap and raise the original window frame
    Display *dpy = get_display();
    XMapRaised(dpy, canvas->win);
    XSync(dpy, False);

    // Prevent the trailing click from deactivating the restored window
    suppress_desktop_deactivate_for_ms(200);

    // Wait briefly until frame is viewable to avoid focusing an unmapped window
    for (int i = 0; i < 50; ++i) { // ~50ms worst-case
        XWindowAttributes wa;
        if (XGetWindowAttributes(dpy, canvas->win, &wa) && wa.map_state == IsViewable) break;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 }; // 1ms
        nanosleep(&ts, NULL);
    }

    set_active_window(canvas);
    // Ensure frame visuals and stacking are correct immediately
    redraw_canvas(canvas);
    compositor_sync_stacking(dpy);

    // Clear press target if it matches the icon's window to prevent crash
    clear_press_target_if_matches(icon->display_window);
    
    // Remove the iconified desktop icon
    destroy_icon(icon);

    // Refresh desktop layout and visuals
    Canvas *desktop = get_desktop_canvas();
    if (desktop) {
        // Do not auto-reorganize; simply refresh
        refresh_canvas(desktop);
    }

    // Re-assert activation and stacking after desktop updates
    set_active_window(canvas);
    XRaiseWindow(dpy, canvas->win);
    compositor_sync_stacking(dpy);
    redraw_canvas(canvas);
    XSync(dpy, False);
}

// ========================
// Workbench event handling
// ========================

// forward decls for selection/double-click helpers
static bool is_double_click(Time current_time, Time last_time) { return current_time - last_time < 500; }
static void select_icon(FileIcon *icon, Canvas *canvas, unsigned int state) {
    FileIcon **icons = get_icon_array(); int count = get_icon_count();
    bool ctrl = (state & ControlMask) != 0;
    if (!ctrl) {
        // Exclusive selection: deselect all others, ensure clicked icon is selected
        for (int i = 0; i < count; i++) {
            if (icons[i] != icon && icons[i]->display_window == canvas->win && icons[i]->selected) {
                icons[i]->selected = false; icons[i]->current_picture = icons[i]->normal_picture;
            }
        }
        icon->selected = true;
    } else {
        // Ctrl held: toggle clicked icon only
        icon->selected = !icon->selected;
    }
    icon->current_picture = icon->selected ? icon->selected_picture : icon->normal_picture;
}

static void deselect_all_icons(Canvas *canvas) {
    FileIcon **icons = get_icon_array(); int count = get_icon_count();
    for (int i = 0; i < count; i++) {
        if (icons[i]->display_window == canvas->win && icons[i]->selected) { icons[i]->selected = false; icons[i]->current_picture = icons[i]->normal_picture; }
    }
}

void workbench_handle_button_press(XButtonEvent *event) {
    Canvas *canvas = find_canvas(event->window); if (!canvas) return;
    
    // Clicking on desktop (icon or empty space) should deactivate any active window
    if (canvas->type == DESKTOP) {
        deactivate_all_windows();
        // Set focus to desktop
        XSetInputFocus(get_display(), canvas->win, RevertToParent, CurrentTime);
    }
    
    FileIcon *icon = find_icon(event->window, event->x, event->y);
    if (icon && event->button == Button1) {
        // Handle double-click BEFORE preparing any drag to avoid interference
        if (is_double_click(event->time, icon->last_click_time)) {
            if (icon->type == TYPE_DRAWER || icon->type == TYPE_DEVICE) open_directory(icon, canvas);
            else if (icon->type == TYPE_FILE) open_file(icon);
            else if (icon->type == TYPE_ICONIFIED) restore_iconified(icon);
            icon->last_click_time = event->time;
            redraw_canvas(canvas);
            return;
        }
        // Single click path: select and prepare for possible drag
        select_icon(icon, canvas, event->state);
        start_drag_icon(icon, event->x, event->y);
        icon->last_click_time = event->time;
    } else {
        deselect_all_icons(canvas);
    }
    redraw_canvas(canvas);
}

void workbench_handle_motion_notify(XMotionEvent *event) {
    Canvas *canvas = find_canvas(event->window); if (!canvas) return;
    continue_drag_icon(event, canvas);
}

void workbench_handle_button_release(XButtonEvent *event) {
    Canvas *canvas = find_canvas(event->window); if (canvas) end_drag_icon(canvas);
}

void workbench_cleanup_drag_state(void) {
    // Clean up drag state after XDND transfer completes

    // Destroy the floating drag window
    destroy_drag_window();

    // Restore icon visibility if it was hidden
    if (dragged_icon && saved_source_window != None) {
        dragged_icon->display_window = saved_source_window;
        saved_source_window = None;
    }

    // Clear XDND context
    if (xdnd_ctx.current_target != None) {
        xdnd_ctx.current_target = None;
    }

    // Refresh source canvas if needed
    if (drag_source_canvas) {
        refresh_canvas(drag_source_canvas);
    }

    // Clear all drag state
    dragged_icon = NULL;
    drag_active = false;
    dragging_floating = false;
    drag_source_canvas = NULL;
}


// ========================
// Init / Cleanup
// ========================

void init_workbench(void) {
    icon_array = malloc(INITIAL_ICON_CAPACITY * sizeof(FileIcon *)); 
    if (!icon_array) {
        log_error("[ERROR] malloc failed for icon_array (capacity=%d)", INITIAL_ICON_CAPACITY);
        exit(1);
    }
    icon_array_size = INITIAL_ICON_CAPACITY; icon_count = 0;
    // Avoid zombie processes from file launches
    signal(SIGCHLD, SIG_IGN);
    // Load default icons used for filetypes without sidecar .info
    load_deficons();
    // Scan ~/Desktop directory for files and create icons for them
    // This will also add the prime desktop icons (System/Home)
    Canvas *desktop = get_desktop_canvas();
    refresh_canvas_from_directory(desktop, NULL); // NULL means use ~/Desktop
    // icon_cleanup now called inside refresh_canvas_from_directory
    redraw_canvas(desktop);
    wb_initialized = true;
}

void cleanup_workbench(void) {
    if (!wb_initialized) return;
    wb_initialized = false;
    // Ensure any drag-related resources are released
    destroy_drag_window();
    dragged_icon = NULL;
    drag_active = false;
    drag_source_canvas = NULL;
    saved_source_window = None;

    for (int i = icon_count - 1; i >= 0; i--) destroy_icon(icon_array[i]);
    if (icon_array) { free(icon_array); icon_array = NULL; }
    icon_count = 0; icon_array_size = 0;
    
    // Free dynamic deficon array
    for (int i = 0; i < def_icons_count; i++) {
        if (def_icons_array[i].extension) free(def_icons_array[i].extension);
        if (def_icons_array[i].icon_path) free(def_icons_array[i].icon_path);
    }
    if (def_icons_array) {
        free(def_icons_array);
        def_icons_array = NULL;
    }
    def_icons_count = 0;
    def_icons_capacity = 0;
    
    // Free special case deficons
    if (def_dir_info)  { free(def_dir_info);  def_dir_info  = NULL; }
    if (def_foo_info)  { free(def_foo_info);  def_foo_info  = NULL; }
}

// Directory size calculation - non-blocking via fork
// Returns child PID and sets pipe_fd for reading result
pid_t calculate_directory_size(const char *path, int *pipe_fd) {
    if (!path || !pipe_fd) {
        log_error("[ERROR] calculate_directory_size: NULL parameters");
        return -1;
    }
    
    // Create pipe for communication
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        log_error("[ERROR] Failed to create pipe for directory size calculation: %s", strerror(errno));
        return -1;
    }
    
    pid_t pid = fork();
    if (pid == -1) {
        log_error("[ERROR] Failed to fork for directory size calculation: %s", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    
    if (pid == 0) {
        // Child process - calculate directory size
        close(pipefd[0]); // Close read end
        
        // Calculate total size recursively
        off_t total_size = 0;
        
        // Use a stack-based approach to avoid deep recursion
        struct dir_entry {
            char path[PATH_SIZE];
            struct dir_entry *next;
        };
        
        struct dir_entry *stack = malloc(sizeof(struct dir_entry));
        if (!stack) {
            log_error("[ERROR] Failed to allocate memory in child process");
            _exit(1);
        }
        
        strncpy(stack->path, path, PATH_SIZE - 1);
        stack->path[PATH_SIZE - 1] = '\0';
        stack->next = NULL;
        
        while (stack) {
            // Pop from stack
            struct dir_entry *current = stack;
            stack = stack->next;
            
            DIR *dir = opendir(current->path);
            if (!dir) {
                free(current);
                continue;
            }
            
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                // Skip . and ..
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                    continue;
                }
                
                // Build full path safely
                char full_path[PATH_SIZE];
                int written = snprintf(full_path, sizeof(full_path), "%s/%s", current->path, entry->d_name);
                
                // Check if path was truncated
                if (written >= PATH_SIZE) {
                    // Path too long, skip this entry
                    continue;
                }
                
                struct stat st;
                if (lstat(full_path, &st) == 0) {
                    if (S_ISREG(st.st_mode)) {
                        // Regular file - add its size
                        total_size += st.st_size;
                    } else if (S_ISDIR(st.st_mode)) {
                        // Directory - push to stack for processing
                        struct dir_entry *new_entry = malloc(sizeof(struct dir_entry));
                        if (new_entry) {
                            strncpy(new_entry->path, full_path, PATH_SIZE - 1);
                            new_entry->path[PATH_SIZE - 1] = '\0';
                            new_entry->next = stack;
                            stack = new_entry;
                        }
                    }
                    // Skip other file types (symlinks, devices, etc.)
                }
            }
            
            closedir(dir);
            free(current);
        }
        
        // Write result to pipe
        if (write(pipefd[1], &total_size, sizeof(total_size)) != sizeof(total_size)) {
            log_error("[ERROR] Failed to write size to pipe");
        }
        
        close(pipefd[1]);
        _exit(0);
    }
    
    // Parent process
    close(pipefd[1]); // Close write end
    *pipe_fd = pipefd[0]; // Return read end
    
    // Make pipe non-blocking
    int flags = fcntl(*pipe_fd, F_GETFL, 0);
    fcntl(*pipe_fd, F_SETFL, flags | O_NONBLOCK);
    
    return pid;
}

// Read directory size result from pipe (non-blocking)
// Returns -1 if not ready yet, otherwise returns size
off_t read_directory_size_result(int pipe_fd) {
    if (pipe_fd < 0) {
        return -1;
    }
    
    off_t size;
    ssize_t bytes_read = read(pipe_fd, &size, sizeof(size));
    
    if (bytes_read == sizeof(size)) {
        close(pipe_fd);
        return size;
    } else if (bytes_read == 0) {
        // End of pipe - child finished but no data
        close(pipe_fd);
        log_error("[WARNING] Directory size calculation completed with no data");
        return 0;
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // Not ready yet
        return -1;
    } else {
        // Error
        log_error("[ERROR] Failed to read from pipe: %s", strerror(errno));
        close(pipe_fd);
        return 0;
    }
}

// ============================================================================
// Icon Positioning Helpers
// ============================================================================

// Find the next free slot for an icon in a canvas (simple version)
// Just finds the next spot after the last icon
static void find_free_slot(Canvas *canvas, int *out_x, int *out_y) {
    if (!canvas || !out_x || !out_y) return;
    
    int step_x = 110;  // Horizontal spacing between icons
    int step_y = 80;   // Vertical spacing between icons
    
    // Find the rightmost/bottommost existing icon
    FileIcon **icons = get_icon_array();
    int count = get_icon_count();
    int last_x = -1;
    int last_y = -1;
    
    for (int i = 0; i < count; i++) {
        if (icons[i] && icons[i]->display_window == canvas->win) {
            // Track the icon that's furthest along (rightmost, then bottommost)
            if (icons[i]->x > last_x || (icons[i]->x == last_x && icons[i]->y > last_y)) {
                last_x = icons[i]->x;
                last_y = icons[i]->y;
            }
        }
    }
    
    // If we found icons, place new one after the last
    if (last_x >= 0) {
        *out_x = last_x;
        *out_y = last_y + step_y;
        
        // If too far down, wrap to next column
        if (*out_y > canvas->height - 100) {
            *out_x = last_x + step_x;
            *out_y = (canvas->type == DESKTOP) ? 200 : 10;  // Desktop starts below System/Home
        }
    } else {
        // No icons found, use default position
        *out_x = (canvas->type == DESKTOP) ? 20 : 10;
        *out_y = (canvas->type == DESKTOP) ? 200 : 10;
    }
}

// ============================================================================
// Archive Extraction
// ============================================================================

// Check if file is an archive based on extension
// Kept for future menu enable/disable logic
__attribute__((unused))
static bool is_archive_file(const char *path) {
    if (!path) return false;
    
    const char *ext = strrchr(path, '.');
    if (!ext) return false;
    ext++; // Skip the dot
    
    // Supported archive formats
    const char *archive_exts[] = {
        "lha", "lzh", "zip", "tar", "gz", "tgz", "bz2", "tbz",
        "xz", "txz", "rar", "7z", NULL
    };
    
    for (int i = 0; archive_exts[i]; i++) {
        if (strcasecmp(ext, archive_exts[i]) == 0) {
            return true;
        }
    }
    
    // Check for compound extensions like .tar.gz
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    if (strstr(name, ".tar.gz") || strstr(name, ".tar.bz2") || strstr(name, ".tar.xz")) {
        return true;
    }
    
    return false;
}

// Extract archive to directory in same location
int extract_file_at_path(const char *archive_path, Canvas *canvas) {
    if (!archive_path) {
        log_error("[ERROR] extract_file_at_path: NULL archive path");
        return -1;
    }
    
    // Check if archive exists
    struct stat st;
    if (stat(archive_path, &st) != 0) {
        log_error("[ERROR] Archive file not found: %s", archive_path);
        return -1;
    }
    
    // Get directory and filename
    char dir_path[PATH_SIZE];
    char archive_name[NAME_SIZE];
    
    const char *last_slash = strrchr(archive_path, '/');
    if (last_slash) {
        size_t dir_len = last_slash - archive_path;
        if (dir_len >= PATH_SIZE) dir_len = PATH_SIZE - 1;
        strncpy(dir_path, archive_path, dir_len);
        dir_path[dir_len] = '\0';
        strncpy(archive_name, last_slash + 1, NAME_SIZE - 1);
    } else {
        strcpy(dir_path, ".");
        strncpy(archive_name, archive_path, NAME_SIZE - 1);
    }
    archive_name[NAME_SIZE - 1] = '\0';
    
    // Remove extension(s) to get base name
    char base_name[NAME_SIZE];
    strncpy(base_name, archive_name, NAME_SIZE - 1);
    base_name[NAME_SIZE - 1] = '\0';
    
    // Handle compound extensions
    char *ext = strstr(base_name, ".tar.");
    if (ext) {
        *ext = '\0';
    } else {
        ext = strrchr(base_name, '.');
        if (ext) *ext = '\0';
    }
    
    // Build target directory path with copy_ prefixes if needed
    char target_dir[PATH_SIZE];
    
    // Calculate lengths once
    size_t dir_len = strlen(dir_path);
    size_t base_len = strlen(base_name);
    
    // Check if basic path would be too long (dir + "/" + base + null)
    if (dir_len + 1 + base_len >= PATH_SIZE) {
        log_error("[ERROR] Path too long for extraction directory");
        return -1;
    }
    
    // Safe to use now - compiler can see the length check
    int written = snprintf(target_dir, PATH_SIZE, "%s/%s", dir_path, base_name);
    if (written >= PATH_SIZE) {
        log_error("[ERROR] Path truncated during extraction");
        return -1;
    }
    
    // Check for existing directory and add copy_ prefix if needed
    int copy_num = 0;
    while (access(target_dir, F_OK) == 0) {
        if (copy_num == 0) {
            // Need room for: dir + "/copy_" + base + null
            if (dir_len + 6 + base_len >= PATH_SIZE) {
                log_error("[ERROR] Path too long for copy directory");
                return -1;
            }
            written = snprintf(target_dir, PATH_SIZE, "%s/copy_%s", dir_path, base_name);
        } else {
            // Need room for: dir + "/copy99_" + base + null (worst case)
            if (dir_len + 8 + base_len >= PATH_SIZE) {
                log_error("[ERROR] Path too long for copy directory");
                return -1;
            }
            written = snprintf(target_dir, PATH_SIZE, "%s/copy%d_%s", dir_path, copy_num, base_name);
        }
        
        if (written >= PATH_SIZE) {
            log_error("[ERROR] Path truncated during copy naming");
            return -1;
        }
        
        copy_num++;
        if (copy_num > 99) {
            log_error("[ERROR] Too many copies of extraction directory");
            return -1;
        }
    }
    
    // Create target directory
    // Creating extraction directory
    if (mkdir(target_dir, 0755) != 0) {
        log_error("[ERROR] Failed to create extraction directory: %s (errno=%d: %s)", 
                  target_dir, errno, strerror(errno));
        return -1;
    }
    // Successfully created extraction directory
    
    // Determine extraction command based on extension
    char command[PATH_SIZE * 3];
    const char *ext_lower = strrchr(archive_name, '.');
    if (!ext_lower) {
        log_error("[ERROR] Unknown archive format: %s", archive_name);
        rmdir(target_dir);
        return -1;
    }
    ext_lower++; // Skip dot
    
    // Build extraction command
    if (strcasecmp(ext_lower, "lha") == 0 || strcasecmp(ext_lower, "lzh") == 0) {
        snprintf(command, sizeof(command), "lha -xw=%s %s 2>&1", target_dir, archive_path);
    } else if (strcasecmp(ext_lower, "zip") == 0) {
        snprintf(command, sizeof(command), "unzip -q %s -d %s 2>&1", archive_path, target_dir);
    } else if (strcasecmp(ext_lower, "rar") == 0) {
        snprintf(command, sizeof(command), "unrar x -y %s %s/ 2>&1", archive_path, target_dir);
    } else if (strcasecmp(ext_lower, "7z") == 0) {
        snprintf(command, sizeof(command), "7z x -y -o%s %s 2>&1", target_dir, archive_path);
    } else if (strcasecmp(ext_lower, "gz") == 0) {
        if (strstr(archive_name, ".tar.gz") || strstr(archive_name, ".tgz")) {
            snprintf(command, sizeof(command), "tar -xzvf %s -C %s 2>&1", archive_path, target_dir);
        } else {
            // Single gzip file
            char output_name[NAME_SIZE];
            strncpy(output_name, base_name, NAME_SIZE - 1);
            snprintf(command, sizeof(command), "gunzip -c %s > %s/%s 2>&1", 
                     archive_path, target_dir, output_name);
        }
    } else if (strcasecmp(ext_lower, "bz2") == 0) {
        if (strstr(archive_name, ".tar.bz2") || strstr(archive_name, ".tbz")) {
            snprintf(command, sizeof(command), "tar -xjvf %s -C %s 2>&1", archive_path, target_dir);
        } else {
            // Single bzip2 file
            char output_name[NAME_SIZE];
            strncpy(output_name, base_name, NAME_SIZE - 1);
            snprintf(command, sizeof(command), "bunzip2 -c %s > %s/%s 2>&1",
                     archive_path, target_dir, output_name);
        }
    } else if (strcasecmp(ext_lower, "xz") == 0) {
        if (strstr(archive_name, ".tar.xz") || strstr(archive_name, ".txz")) {
            snprintf(command, sizeof(command), "tar -xJvf %s -C %s 2>&1", archive_path, target_dir);
        } else {
            // Single xz file
            char output_name[NAME_SIZE];
            strncpy(output_name, base_name, NAME_SIZE - 1);
            snprintf(command, sizeof(command), "unxz -c %s > %s/%s 2>&1",
                     archive_path, target_dir, output_name);
        }
    } else if (strcasecmp(ext_lower, "tar") == 0) {
        snprintf(command, sizeof(command), "tar -xvf %s -C %s 2>&1", archive_path, target_dir);
    } else {
        log_error("[ERROR] Unsupported archive format: %s", ext_lower);
        rmdir(target_dir);
        return -1;
    }
    
    // Create pipe for IPC
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        log_error("[ERROR] Failed to create pipe: %s", strerror(errno));
        rmdir(target_dir);
        return -1;
    }
    
    pid_t pid = fork();
    if (pid == -1) {
        log_error("[ERROR] Failed to fork: %s", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        rmdir(target_dir);
        return -1;
    }
    
    if (pid == 0) {
        // ===== CHILD PROCESS - Perform extraction =====
        close(pipefd[0]); // Close read end
        
        // Get archive size instantly with stat() - no blocking!
        struct stat archive_stat;
        size_t archive_size = 0;
        if (stat(archive_path, &archive_stat) == 0) {
            archive_size = archive_stat.st_size;
        }
        
        // Send START message IMMEDIATELY with archive size for byte-based progress
        ProgressMessage msg = {0};
        msg.type = MSG_START;
        msg.start_time = time(NULL);
        msg.files_total = -1; // Files unknown, but we have bytes
        msg.bytes_total = archive_size;  // Total archive size for progress calculation
        msg.bytes_done = 0;
        strncpy(msg.current_file, archive_name, NAME_SIZE - 1);
        // Store the canvas window ID for later icon creation
        if (canvas) {
            msg.target_window = canvas->win;
        }
        write(pipefd[1], &msg, sizeof(msg));
        
        // Open archive file for reading
        int archive_fd = open(archive_path, O_RDONLY);
        if (archive_fd < 0) {
            msg.type = MSG_ERROR;
            snprintf(msg.current_file, NAME_SIZE, "Failed to open archive: %s", strerror(errno));
            log_error("[ERROR] Failed to open archive %s: %s", archive_path, strerror(errno));
            write(pipefd[1], &msg, sizeof(msg));
            close(pipefd[1]);
            _exit(1);
        }
        
        // Build extraction command that reads from stdin
        char extract_cmd[PATH_SIZE * 2];
        bool use_stdin = true;  // Most formats can read from stdin
        
        if (strcasecmp(ext_lower, "zip") == 0) {
            // funzip can extract from stdin, or use bsdtar
            snprintf(extract_cmd, sizeof(extract_cmd), "bsdtar -xf - -C %s", target_dir);
        } else if (strcasecmp(ext_lower, "rar") == 0) {
            // Use bsdtar for RAR (works for v4 and earlier)
            snprintf(extract_cmd, sizeof(extract_cmd), "bsdtar -xf - -C %s", target_dir);
        } else if (strcasecmp(ext_lower, "7z") == 0) {
            // 7z can extract from stdin with -si flag
            snprintf(extract_cmd, sizeof(extract_cmd), "7z x -si -y -o%s", target_dir);
        } else if (strcasecmp(ext_lower, "lha") == 0 || strcasecmp(ext_lower, "lzh") == 0) {
            // lha supports stdin with - flag
            snprintf(extract_cmd, sizeof(extract_cmd), "lha x -w=%s -", target_dir);
        } else if (strstr(archive_name, ".tar.gz") || strstr(archive_name, ".tgz")) {
            snprintf(extract_cmd, sizeof(extract_cmd), "tar -xz -C %s", target_dir);
        } else if (strstr(archive_name, ".tar.bz2") || strstr(archive_name, ".tbz")) {
            snprintf(extract_cmd, sizeof(extract_cmd), "tar -xj -C %s", target_dir);
        } else if (strstr(archive_name, ".tar.xz") || strstr(archive_name, ".txz")) {
            snprintf(extract_cmd, sizeof(extract_cmd), "tar -xJ -C %s", target_dir);
        } else if (strcasecmp(ext_lower, "tar") == 0) {
            snprintf(extract_cmd, sizeof(extract_cmd), "tar -x -C %s", target_dir);
        } else if (strcasecmp(ext_lower, "gz") == 0) {
            // Single gzip file
            char output_name[NAME_SIZE];
            strncpy(output_name, base_name, NAME_SIZE - 1);
            // Remove .gz extension
            char *dot = strrchr(output_name, '.');
            if (dot && strcasecmp(dot, ".gz") == 0) *dot = '\0';
            snprintf(extract_cmd, sizeof(extract_cmd), "gunzip -c > %s/%s", target_dir, output_name);
        } else {
            // Default: try bsdtar which handles many formats
            snprintf(extract_cmd, sizeof(extract_cmd), "bsdtar -xf - -C %s", target_dir);
        }
        
        // Log which extraction method we're using
        log_error("[INFO] Extracting %s using command: %.200s", archive_name, extract_cmd);
        
        // Execute extraction command
        FILE *tar_pipe;
        int status;
        
        if (use_stdin) {
            // Most formats: read archive ourselves and pipe to extractor
            tar_pipe = popen(extract_cmd, "w");
            if (!tar_pipe) {
                close(archive_fd);
                msg.type = MSG_ERROR;
                // More specific error message including which tool failed
                const char *tool = strstr(extract_cmd, "bsdtar") ? "bsdtar" :
                                  strstr(extract_cmd, "tar") ? "tar" :
                                  strstr(extract_cmd, "7z") ? "7z" :
                                  strstr(extract_cmd, "gunzip") ? "gunzip" :
                                  "extractor";
                snprintf(msg.current_file, NAME_SIZE, "Failed: %s not found or not executable", tool);
                log_error("[ERROR] Extraction failed for %s: Could not execute '%s' (command: %.100s)", 
                          archive_path, tool, extract_cmd);
                write(pipefd[1], &msg, sizeof(msg));
                close(pipefd[1]);
                _exit(1);
            }
            
            // Read archive and pipe to extractor, tracking bytes
            char buffer[65536];  // 64KB buffer for efficiency
            size_t bytes_read_total = 0;
            ssize_t bytes_read;
            time_t last_update = time(NULL);
            
            while ((bytes_read = read(archive_fd, buffer, sizeof(buffer))) > 0) {
                // Write chunk to extractor's stdin
                size_t bytes_written = fwrite(buffer, 1, bytes_read, tar_pipe);
                if (bytes_written != (size_t)bytes_read) {
                    // Write error
                    msg.type = MSG_ERROR;
                    strncpy(msg.current_file, "Extraction write error", NAME_SIZE - 1);
                    write(pipefd[1], &msg, sizeof(msg));
                    break;
                }
                
                // Track progress
                bytes_read_total += bytes_read;
                
                // Send progress update every 256KB or every second
                time_t now = time(NULL);
                if (bytes_read_total % (256 * 1024) == 0 || now > last_update) {
                    msg.type = MSG_PROGRESS;
                    msg.bytes_done = bytes_read_total;
                    msg.bytes_total = archive_size;
                    msg.files_done = 0;  // Not tracking files
                    msg.files_total = -1;
                    
                    // Just send the archive name, no "Extracting" prefix (dialog adds that)
                    snprintf(msg.current_file, NAME_SIZE, "%s", archive_name);
                    write(pipefd[1], &msg, sizeof(msg));
                    last_update = now;
                }
            }
            
            close(archive_fd);
            status = pclose(tar_pipe);
        } else {
            // Formats that don't support stdin (rar, lha): use old method with estimation
            close(archive_fd);  // Don't need this
            
            tar_pipe = popen(extract_cmd, "r");
            if (!tar_pipe) {
                msg.type = MSG_ERROR;
                // More specific error message for non-stdin tools
                const char *tool = strstr(extract_cmd, "unrar") ? "unrar" :
                                  strstr(extract_cmd, "lha") ? "lha" :
                                  "extractor";
                snprintf(msg.current_file, NAME_SIZE, "Failed: %s not found or not executable", tool);
                log_error("[ERROR] Extraction failed for %s: Could not execute '%s' (command: %.100s)", 
                          archive_path, tool, extract_cmd);
                write(pipefd[1], &msg, sizeof(msg));
                close(pipefd[1]);
                _exit(1);
            }
            
            // Read output for progress (less accurate but works)
            char line[256];
            time_t last_update = time(NULL);
            time_t start_time = time(NULL);
            
            while (fgets(line, sizeof(line), tar_pipe)) {
                time_t now = time(NULL);
                if (now > last_update) {
                    msg.type = MSG_PROGRESS;
                    msg.files_done = 0;  // Not tracking files
                    msg.files_total = -1;
                    
                    // Estimate progress based on time (rough)
                    if (archive_size > 0) {
                        time_t elapsed = now - start_time;
                        // Estimate: 100MB/sec extraction rate
                        size_t estimated_bytes = elapsed * 100000000;
                        if (estimated_bytes > archive_size * 0.9) {
                            estimated_bytes = (size_t)(archive_size * 0.9);
                        }
                        msg.bytes_done = estimated_bytes;
                        msg.bytes_total = archive_size;
                    }
                    
                    snprintf(msg.current_file, NAME_SIZE, "Extracting...");
                    write(pipefd[1], &msg, sizeof(msg));
                    last_update = now;
                }
            }
            
            status = pclose(tar_pipe);
        }
        int extraction_success = 0;
        
        if (status == -1) {
            log_error("[WARNING] pclose failed: errno=%d (%s) - checking if extraction succeeded anyway", 
                      errno, strerror(errno));
            // pclose might fail if the child was already reaped, check if extraction worked
            struct stat st;
            if (stat(target_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
                // Check if directory has any contents
                DIR *dir = opendir(target_dir);
                if (dir) {
                    struct dirent *entry;
                    int has_files = 0;
                    while ((entry = readdir(dir)) != NULL) {
                        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
                            has_files = 1;
                            break;
                        }
                    }
                    closedir(dir);
                    if (has_files) {
                        // Directory exists with content - treating as success
                        extraction_success = 1;
                    }
                }
            }
        } else if (WIFEXITED(status)) {
            int exit_code = WEXITSTATUS(status);
            // Extraction command exited with code
            extraction_success = (exit_code == 0);
        } else {
            log_error("[ERROR] Extraction command terminated abnormally");
            extraction_success = 0;
        }
        
        // Create a fresh message for completion to avoid any contamination from progress messages
        ProgressMessage final_msg = {0};
        
        if (extraction_success) {
            // Success - send completion with refresh hint
            // Extraction successful - preparing completion message
            
            final_msg.type = MSG_COMPLETE;
            final_msg.files_done = 0;  // Not tracking files
            final_msg.bytes_done = archive_size;  // 100% complete
            final_msg.bytes_total = archive_size;
            final_msg.create_icon = false;  // This is extraction, not copy with icon
            strncpy(final_msg.dest_dir, dir_path, PATH_SIZE - 1); // Parent dir for refresh
            strncpy(final_msg.dest_path, target_dir, PATH_SIZE - 1); // Created dir
            // Pass through the canvas window
            if (canvas) {
                final_msg.target_window = canvas->win;
            }
            // Completion message prepared with window and path
        } else {
            // Extraction failed
            log_error("[ERROR] Extraction failed for %s", archive_path);
            final_msg.type = MSG_ERROR;
            snprintf(final_msg.current_file, NAME_SIZE, "Extraction failed");
            // Clean up empty directory
            rmdir(target_dir);
        }
        
        write(pipefd[1], &final_msg, sizeof(final_msg));
        close(pipefd[1]);
        _exit(extraction_success ? 0 : 1);
    }
    
    // ===== PARENT PROCESS - Return immediately =====
    close(pipefd[1]); // Close write end
    
    // Make pipe non-blocking
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
    
    // Create progress dialog structure
    ProgressDialog *dialog = calloc(1, sizeof(ProgressDialog));
    if (!dialog) {
        close(pipefd[0]);
        int status;
        waitpid(pid, &status, 0);
        return -1;
    }
    
    dialog->operation = PROGRESS_EXTRACT; // Extract operation
    dialog->pipe_fd = pipefd[0];
    dialog->child_pid = pid;
    dialog->start_time = time(NULL);
    dialog->canvas = NULL; // Window created later if needed
    dialog->percent = -1.0f; // Not started yet
    strncpy(dialog->current_file, archive_name, PATH_SIZE - 1);
    
    // Add to global list
    extern void add_progress_dialog_to_list(ProgressDialog *dialog);
    add_progress_dialog_to_list(dialog);
    
    return 0;
}