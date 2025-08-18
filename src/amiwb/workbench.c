// File: workbench.c
#define _POSIX_C_SOURCE 200809L
#include "workbench.h"
#include "render.h"
#include "intuition.h"
#include "compositor.h"
#include "config.h"  // Added to include config.h for max/min macros
#include "events.h"  // For clear_press_target_if_matches
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <time.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <signal.h>
#include <time.h>

// Forward declarations for local helpers used later
static Canvas *canvas_under_pointer(void);
static int move_file_to_directory(const char *src_path, const char *dst_dir, char *dst_path, size_t dst_sz);
static bool is_directory(const char *path);
static int copy_file(const char *src, const char *dst);
static void remove_icon_by_path_on_canvas(const char *abs_path, Canvas *canvas);
static void refresh_canvas(Canvas *canvas);

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
static FileIcon *dragged_icon = NULL;       // Moved from events.c
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
static char *def_txt_info  = NULL;
static char *def_jpg_info  = NULL;
static char *def_png_info  = NULL;
static char *def_gif_info  = NULL;
static char *def_pdf_info  = NULL;
static char *def_avi_info  = NULL;
static char *def_mp4_info  = NULL;
static char *def_mkv_info  = NULL;
static char *def_html_info  = NULL;
static char *def_webp_info = NULL;
static char *def_zip_info  = NULL;
static char *def_lha_info  = NULL;
static char *def_mp3_info  = NULL;
static char *def_m4a_info  = NULL;
static char *def_webm_info  = NULL;
static char *def_rar_info  = NULL;
static char *def_dir_info  = NULL;   // for directories
static char *def_foo_info  = NULL;   // generic fallback for unknown filetypes

// Resolve and cache a deficon path if present on disk; keeps runtime lookups cheap.
static void load_one_deficon(const char *basename, char **out_storage) {
    if (!basename || !out_storage) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", deficons_dir, basename);
    struct stat st;
    if (stat(path, &st) == 0) {
        if (*out_storage) free(*out_storage);
        *out_storage = strdup(path);
        printf("[deficons] present: %s -> %s\n", basename, path);
    } else {
        printf("[deficons] missing: %s -> %s/?!)\n", basename, deficons_dir);
    }
}

// Preload common deficons so refresh can pick the right fallback instantly.
static void load_deficons(void) {
    // Map common extensions to specific deficon .info files
    load_one_deficon("def_txt.info",  &def_txt_info);
    load_one_deficon("def_jpg.info",  &def_jpg_info);
    load_one_deficon("def_png.info",  &def_png_info);
    load_one_deficon("def_gif.info",  &def_gif_info);
    load_one_deficon("def_pdf.info",  &def_pdf_info);
    load_one_deficon("def_avi.info",  &def_avi_info);
    load_one_deficon("def_mp4.info",  &def_mp4_info);
    load_one_deficon("def_mkv.info",  &def_mkv_info);
    load_one_deficon("def_html.info", &def_html_info);
    load_one_deficon("def_webp.info", &def_webp_info);
    load_one_deficon("def_zip.info",  &def_zip_info);
    load_one_deficon("def_lha.info",  &def_lha_info);
    load_one_deficon("def_mp3.info",  &def_mp3_info);
    load_one_deficon("def_m4a.info",  &def_m4a_info);
    load_one_deficon("def_webm.info", &def_webm_info);
    load_one_deficon("def_rar.info",  &def_rar_info);
    load_one_deficon("def_dir.info",  &def_dir_info);
    load_one_deficon("def_foo.info",  &def_foo_info);
}

// Forward declaration of helper function
static inline void set_icon_meta(FileIcon *ic, const char *path, const char *label, int type);

// Get the most recently added icon from the global array
static FileIcon* get_last_added_icon(void) {
    return (icon_count > 0) ? icon_array[icon_count - 1] : NULL;
}

// Advance icon position to next slot with wrapping
static void advance_icon_position(int *x, int *y, Canvas *canvas, int x_offset) {
    *x += x_offset;
    if (*x + 64 > canvas->width) {
        *x = 10;
        *y += 80;
    }
}

// Create icon and set its metadata in one call
static FileIcon* create_icon_with_metadata(const char *icon_path, Canvas *canvas, int x, int y,
                                          const char *full_path, const char *name, int type) {
    create_icon(icon_path, canvas, x, y);
    FileIcon *new_icon = get_last_added_icon();
    if (new_icon) {
        set_icon_meta(new_icon, full_path, name, type);
    }
    return new_icon;
}

// Choose the appropriate deficon for a file/dir name; returns NULL if none.
static const char *definfo_for_file(const char *name, bool is_dir) {
    if (!name) return NULL;
    if (is_dir) return def_dir_info; // default drawer icon if present
    const char *dot = strrchr(name, '.');
    if (!dot || !dot[1]) return NULL;  // Use array indexing instead of pointer arithmetic
    const char *ext = dot + 1;

    if ((strcasecmp(ext, "jpg") == 0  || 
        strcasecmp(ext, "jpeg") == 0) && def_jpg_info)  return def_jpg_info;
    
    if ((strcasecmp(ext, "htm") == 0  ||
        strcasecmp(ext, "html") == 0) && def_html_info) return def_html_info;
    
    if (strcasecmp(ext, "webp") == 0  && def_webp_info) return def_webp_info;
    if (strcasecmp(ext, "zip")  == 0  && def_zip_info)  return def_zip_info;
    if (strcasecmp(ext, "lha")  == 0  && def_lha_info)  return def_lha_info;
    if (strcasecmp(ext, "mp3")  == 0  && def_mp3_info)  return def_mp3_info;
    if (strcasecmp(ext, "m4a")  == 0  && def_m4a_info)  return def_m4a_info;
    if (strcasecmp(ext, "txt")  == 0  && def_txt_info)  return def_txt_info;
    if (strcasecmp(ext, "png")  == 0  && def_png_info)  return def_png_info;
    if (strcasecmp(ext, "gif")  == 0  && def_gif_info)  return def_gif_info;
    if (strcasecmp(ext, "pdf")  == 0  && def_pdf_info)  return def_pdf_info;
    if (strcasecmp(ext, "avi")  == 0  && def_avi_info)  return def_avi_info;
    if (strcasecmp(ext, "mp4")  == 0  && def_mp4_info)  return def_mp4_info;
    if (strcasecmp(ext, "mkv")  == 0  && def_mkv_info)  return def_mkv_info;
    if (strcasecmp(ext, "webm") == 0  && def_webm_info) return def_webm_info;
    if (strcasecmp(ext, "rar")  == 0  && def_rar_info)  return def_rar_info;
    // Unknown or unmapped extension -> generic tool icon if available
    if (def_foo_info) return def_foo_info;
    return NULL;
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
    // System
    create_icon("/usr/local/share/amiwb/icons/harddisk.info", desktop, 20, 40);
    FileIcon *system_icon = get_last_added_icon();
    set_icon_meta(system_icon, "/", "System", TYPE_DRAWER);
    // Home
    create_icon("/usr/local/share/amiwb/icons/harddisk.info", desktop, 20, 120);
    FileIcon *home_icon = get_last_added_icon();
    const char *home = getenv("HOME");
    set_icon_meta(home_icon, home ? home : ".", "Home", TYPE_DRAWER);
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
}

static void end_drag_icon(Canvas *canvas) {
    // Clean up floating drag window if any
    destroy_drag_window();

    // If no drag is active, nothing to do
    if (!dragged_icon) {
        drag_source_canvas = NULL;
        saved_source_window = None;
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
                printf("[WARNING] Cannot move directory into itself or its subdirectory\n");
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
        int moved = move_file_to_directory(dragged_icon->path, dst_dir, dst_path, sizeof(dst_path));
        if (moved == 0) {
            // Success: manually update icon lists without global re-layout
            // 1) Remove dragged icon from source canvas
            destroy_icon(dragged_icon);
            dragged_icon = NULL;

            // Move sidecar .info file if present to keep custom icon with the file
            move_sidecar_info_file(src_path_abs, dst_dir, dst_path);

            // 2) Create a new icon on target at drop position
            // Determine pointer position relative to target content area
            Display *dpy = get_display();
            int rx, ry, wx, wy; unsigned int mask; Window root_ret, child_ret;
            XQueryPointer(dpy, DefaultRootWindow(dpy), &root_ret, &child_ret, &rx, &ry, &wx, &wy, &mask);
            // Translate target window origin to root
            int tx = 0, ty = 0; Window dummy;
            XTranslateCoordinates(dpy, target->win, DefaultRootWindow(dpy), 0, 0, &tx, &ty, &dummy);
            int local_x = rx - tx;
            int local_y = ry - ty;
            if (target->type == WINDOW) {
                local_x = max(0, local_x - BORDER_WIDTH_LEFT + target->scroll_x);
                local_y = max(0, local_y - BORDER_HEIGHT_TOP + target->scroll_y);
            }
            // Center icon under cursor
            int place_x = max(0, local_x - 32);
            int place_y = max(0, local_y - 32);

            // Choose icon image path for the new icon:
            // 1) Sidecar .info if present
            // 2) def_icons fallback per extension (matches refresh logic)
            // 3) The file itself (last resort)
            char info_path[PATH_SIZE];
            int ret = snprintf(info_path, sizeof(info_path), "%s.info", dst_path);
            if (ret >= PATH_SIZE) {
                printf("[ERROR] Icon path too long, operation cancelled: %s.info\n", dst_path);
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

static Canvas *canvas_under_pointer(void) {
    Display *dpy = get_display();
    Window root = DefaultRootWindow(dpy);
    Window root_ret, child_ret; int rx, ry, wx, wy; unsigned int mask;
    if (!XQueryPointer(dpy, root, &root_ret, &child_ret, &rx, &ry, &wx, &wy, &mask)) return NULL;

    // Scan root children from topmost to bottom to find the topmost WINDOW under pointer
    Window r, p, *children = NULL; unsigned int n = 0;
    if (!XQueryTree(dpy, root, &r, &p, &children, &n)) return NULL;
    Canvas *best = NULL;
    for (int i = (int)n - 1; i >= 0; --i) {
        Window w = children[i];
        Canvas *c = find_canvas(w);
        if (!c) continue;
        if (c->type == MENU) continue; // menus are not drop targets
        // Get outer geometry of the frame window
        XWindowAttributes wa; if (!XGetWindowAttributes(dpy, w, &wa)) continue;
        if (wa.map_state != IsViewable) continue;
        int x = wa.x, y = wa.y, wdt = wa.width, hgt = wa.height;
        if (rx >= x && rx < x + wdt && ry >= y && ry < y + hgt) {
            // Prefer WINDOW over DESKTOP
            if (c->type == WINDOW) { best = c; break; }
            if (!best) best = c; // could be DESKTOP
        }
    }
    if (children) XFree(children);
    return best;
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
    
    // Clean up and return success
    cleanup_file_descriptors(in_fd, out_fd);
    return 0;
}

// returns 0 on success, non-zero on failure
static int move_file_to_directory(const char *src_path, const char *dst_dir, char *dst_path, size_t dst_sz) {
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
            // Cross-filesystem move
            if (is_src_dir) {
                // TODO: Implement recursive directory copy for cross-filesystem moves
                fprintf(stderr, "[amiwb] Cross-filesystem directory moves not yet supported\n");
                return -1;
            } else {
                // For files, copy then remove source
                if (copy_file(src_path, dst_path) != 0) { 
                    perror("[amiwb] copy (move) failed"); 
                    unlink(dst_path); 
                    return -1; 
                }
                if (unlink(src_path) != 0) { 
                    perror("[amiwb] unlink source after copy failed"); 
                    /* keep file but continue */ 
                }
            }
        } else {
            perror("[amiwb] rename (move) failed");
            return -1;
        }
    }
    return 0;
}

// Workbench API expected by other modules

// Manage dynamic icon array
static FileIcon *manage_icons(bool add, FileIcon *icon_to_remove) {
    if (add) {
        if (icon_count >= icon_array_size) {
            icon_array_size = icon_array_size ? icon_array_size * 2 : INITIAL_ICON_CAPACITY;
            FileIcon **new_icons = realloc(icon_array, icon_array_size * sizeof(FileIcon *));
            if (!new_icons) return NULL;
            icon_array = new_icons;
        }
        FileIcon *new_icon = calloc(1, sizeof(FileIcon));
        if (!new_icon) return NULL;
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
    if (!canvas || !out_count) return NULL;
    int count = 0; for (int i = 0; i < icon_count; ++i) if (icon_array[i] && icon_array[i]->display_window == canvas->win) ++count;
    *out_count = count; if (count == 0) return NULL;
    FileIcon **list = (FileIcon**)malloc(sizeof(FileIcon*) * count); if (!list) { *out_count = 0; return NULL; }
    int k = 0; for (int i = 0; i < icon_count; ++i) { FileIcon *ic = icon_array[i]; if (ic && ic->display_window == canvas->win) list[k++] = ic; }
    return list;
}

// Remove first icon on a given canvas whose absolute path matches
static void remove_icon_by_path_on_canvas(const char *abs_path, Canvas *canvas) {
    if (!abs_path || !canvas) return;
    for (int i = 0; i < icon_count; ++i) {
        FileIcon *ic = icon_array[i];
        if (!ic) continue;
        if (ic->display_window != canvas->win) continue;
        if (ic->path && strcmp(ic->path, abs_path) == 0) { destroy_icon(ic); break; }
    }
}

void create_icon(const char *path, Canvas *canvas, int x, int y) {
    FileIcon *icon = manage_icons(true, NULL);
    if (!icon) return;
    icon->path = strdup(path);
    const char *base = strrchr(path, '/');
    icon->label = strdup(base ? base + 1 : path);
    struct stat st;
    if (stat(path, &st) == 0) icon->type = S_ISDIR(st.st_mode) ? TYPE_DRAWER : TYPE_FILE; else icon->type = TYPE_FILE;
    icon->x = x; icon->y = y;
    icon->display_window = canvas->win;
    icon->selected = false;
    icon->last_click_time = 0;
    icon->iconified_canvas = NULL;
    create_icon_images(icon, get_render_context());
    icon->current_picture = icon->normal_picture;
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
            // Keep iconified window icons on the desktop during refresh
            if (icon_array[i]->type == TYPE_ICONIFIED) continue;
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
        int visible_w = canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT;
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
    if (strcmp(ia->label, "System") == 0) return -1;
    if (strcmp(ib->label, "System") == 0) return 1;
    if (strcmp(ia->label, "Home") == 0) return -1;
    if (strcmp(ib->label, "Home") == 0) return 1;
    if (ia->type == TYPE_DRAWER && ib->type != TYPE_DRAWER) return -1;
    if (ia->type != TYPE_DRAWER && ib->type == TYPE_DRAWER) return 1;
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
        int *col_widths = malloc(num_columns * sizeof(int)); if (!col_widths) { free(list); return; }
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
        int visible_w = canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT;
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
        int x = 20, y = 20; int x_offset = 100;
        struct dirent *entry;
        while ((entry = readdir(dirp))) {
            // Skip current and parent directory entries always
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            // Skip hidden unless show_hidden is enabled
            if (entry->d_name[0] == '.' && !canvas->show_hidden) continue;
            char full_path[PATH_SIZE];
            int ret = snprintf(full_path, sizeof(full_path), "%s/%s", dir, entry->d_name);
            if (ret >= PATH_SIZE) {
                printf("[ERROR] Path too long, skipping: %s/%s\n", dir, entry->d_name);
                continue;
            }
            if (ends_with(entry->d_name, ".info")) {
                char base[256]; size_t namelen = strlen(entry->d_name);
                size_t blen = namelen > 5 ? namelen - 5 : 0; strncpy(base, entry->d_name, blen); base[blen] = '\0';
                char base_path[PATH_SIZE];
                ret = snprintf(base_path, sizeof(base_path), "%s/%s", dir, base);
                if (ret >= PATH_SIZE) {
                    printf("[ERROR] Base path too long, skipping: %s/%s\n", dir, base);
                    continue;
                }
                struct stat st; if (stat(base_path, &st) != 0) {
                    // Orphan .info
                    create_icon_with_metadata(full_path, canvas, x, y, full_path, entry->d_name, TYPE_FILE);
                    advance_icon_position(&x, &y, canvas, x_offset);
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
                create_icon_with_metadata(icon_path, canvas, x, y, full_path, entry->d_name, t);
                advance_icon_position(&x, &y, canvas, x_offset);
            }
        }
        closedir(dirp);
    } else {
        fprintf(stderr, "Failed to open directory %s\n", dir);
    }
    // Do not auto-reorganize icons; only menu > Window > Cleanup should do that
    canvas->scanning = false;
}

static void open_directory(FileIcon *icon, Canvas *current_canvas) {
    if (!icon || !icon->path) return;
    // If window for this path exists, raise and activate it
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
        // Initial placement for new window must use icon_cleanup
        icon_cleanup(new_canvas);
        redraw_canvas(new_canvas);
        // Make the newly created window active (raise on top)
        set_active_window(new_canvas);
    }
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

void open_file(FileIcon *icon) {
    if (!icon || !icon->path) return;
    // Directories should be opened within AmiWB (safety guard)
    if (icon->type == TYPE_DRAWER) {
        Canvas *c = find_canvas(icon->display_window);
        if (c) open_directory(icon, c);
        return;
    }
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork failed in open_file");
        return;
    } else if (pid == 0) {
        // Child: replace with xdg-open
        execlp("xdg-open", "xdg-open", icon->path, (char *)NULL);
        perror("execlp failed for xdg-open");
        _exit(EXIT_FAILURE);
    }
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
            if (icon->type == TYPE_DRAWER) open_directory(icon, canvas);
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

// ========================
// Init / Cleanup
// ========================

void init_workbench(void) {
    icon_array = malloc(INITIAL_ICON_CAPACITY * sizeof(FileIcon *)); if (!icon_array) return;
    icon_array_size = INITIAL_ICON_CAPACITY; icon_count = 0;
    // Avoid zombie processes from file launches
    signal(SIGCHLD, SIG_IGN);
    // Load default icons used for filetypes without sidecar .info
    load_deficons();
    // Scan ~/Desktop directory for files and create icons for them
    // This will also add the prime desktop icons (System/Home)
    Canvas *desktop = get_desktop_canvas();
    refresh_canvas_from_directory(desktop, NULL); // NULL means use ~/Desktop
    icon_cleanup(desktop); // Arrange all icons properly
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
    // Free deficon paths
    if (def_txt_info)  { free(def_txt_info);  def_txt_info  = NULL; }
    if (def_jpg_info)  { free(def_jpg_info);  def_jpg_info  = NULL; }
    if (def_png_info)  { free(def_png_info);  def_png_info  = NULL; }
    if (def_webp_info) { free(def_webp_info); def_webp_info = NULL; }
    if (def_gif_info)  { free(def_gif_info);  def_gif_info  = NULL; }
    if (def_pdf_info)  { free(def_pdf_info);  def_pdf_info  = NULL; }
    if (def_avi_info)  { free(def_avi_info);  def_avi_info  = NULL; }
    if (def_mp4_info)  { free(def_mp4_info);  def_mp4_info  = NULL; }
    if (def_mkv_info)  { free(def_mkv_info);  def_mkv_info  = NULL; }
    if (def_webm_info) { free(def_webm_info); def_webm_info = NULL; }
    if (def_m4a_info)  { free(def_m4a_info);  def_m4a_info =  NULL; }
    if (def_mp3_info)  { free(def_mp3_info);  def_mp3_info  = NULL; }
    if (def_html_info) { free(def_html_info); def_html_info = NULL; }
    if (def_zip_info)  { free(def_zip_info);  def_zip_info  = NULL; }
    if (def_rar_info)  { free(def_rar_info);  def_rar_info  = NULL; }
    if (def_lha_info)  { free(def_lha_info);  def_lha_info  = NULL; }
    if (def_dir_info)  { free(def_dir_info);  def_dir_info  = NULL; }
    if (def_foo_info)  { free(def_foo_info);  def_foo_info  = NULL; }
}