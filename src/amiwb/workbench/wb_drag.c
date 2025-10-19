// File: wb_drag.c
// Drag and Drop - State machine, floating drag window, XDND integration

#define _POSIX_C_SOURCE 200809L
#include "wb_internal.h"
#include "wb_public.h"
#include "../config.h"
#include "../render/rnd_public.h"
#include "../xdnd.h"
#include "../intuition/itn_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xft/Xft.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/Xrender.h>

// Progress system (from wb_progress.c)
typedef struct {
    enum {
        MSG_START,
        MSG_PROGRESS,
        MSG_COMPLETE,
        MSG_ERROR
    } type;
    time_t start_time;
    int files_done;
    int files_total;
    char current_file[NAME_SIZE];           // Filename only - OK
    size_t bytes_done;
    size_t bytes_total;
    // Icon creation metadata (used on MSG_COMPLETE)
    char dest_path[FULL_SIZE];              // Full path with potential extensions
    char dest_dir[PATH_SIZE];               // Directory only - OK
    bool create_icon;
    bool has_sidecar;
    char sidecar_src[FULL_SIZE];            // Full path + ".info" suffix requires FULL_SIZE
    char sidecar_dst[FULL_SIZE];            // Full path + ".info" suffix requires FULL_SIZE
    int icon_x, icon_y;
    Window target_window;
} ProgressMessage;

// ============================================================================
// Drag State Variables
// ============================================================================

// Global drag icon (exposed for XDND)
FileIcon *dragged_icon = NULL;

// Multi-icon drag support (AWP - module-private state)
static FileIcon **dragged_icons = NULL;    // Array of selected icons being dragged
static int dragged_icons_count = 0;        // Number of icons in array
static bool in_multi_icon_processing = false;  // Prevent cleanup during array iteration

// Spatial layout support - preserve relative positions during multi-icon drops
static int *icon_offset_x = NULL;  // Relative X offsets from clicked icon (reference)
static int *icon_offset_y = NULL;  // Relative Y offsets from clicked icon (reference)

// Drag state
static int drag_start_x, drag_start_y;                  // Click position in icon
static int drag_start_root_x, drag_start_root_y;        // Start position in root
static Canvas *drag_source_canvas = NULL;                // Source canvas
static bool dragging_floating = false;                   // Using floating window
static Window drag_win = None;                           // Drag window
static Picture target_picture = 0;                       // XRender picture
static Visual *drag_visual = NULL;                       // Drag window visual
static Colormap drag_colormap = None;                   // Drag window colormap
static bool drag_active = false;                         // Threshold passed
static int drag_orig_x = 0, drag_orig_y = 0;            // Original position for restore
static Window saved_source_window = None;                // Original display_window
static int drag_win_w = 120, drag_win_h = 100;          // Drag window size
static int saved_drag_win_x = 0, saved_drag_win_y = 0;  // Position saved before destroy
static int last_root_x = -10000, last_root_y = -10000;  // Last root coords

// Pointer cache for canvas_under_pointer optimization
static struct {
    Canvas *cached_canvas;
    int cached_x, cached_y;
    Time cache_time;
    bool valid;
} pointer_cache = {NULL, -1, -1, 0, false};

// ============================================================================
// Helper Functions
// ============================================================================

// String suffix check
static bool ends_with(const char *s, const char *suffix) {
    size_t l = strlen(s), m = strlen(suffix);
    return l >= m && strcmp(s + l - m, suffix) == 0;
}

// Check if path is directory
static bool is_directory(const char *path) {
    if (!path || !*path) return false;
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

// Check if file exists
static bool check_if_file_exists(const char *file_path) {
    if (!file_path || !*file_path) return false;
    struct stat st;
    return stat(file_path, &st) == 0;
}

// Move sidecar .info file
static void move_sidecar_info_file(const char *src_path, const char *dst_dir,
                                   const char *dst_path) {
    if (!src_path || !dst_dir || !dst_path) return;

    char src_info[FULL_SIZE];  // PATH + ".info" suffix
    snprintf(src_info, sizeof(src_info), "%s.info", src_path);

    if (!check_if_file_exists(src_info)) return;

    const char *base = strrchr(dst_path, '/');
    const char *name_only = base ? base + 1 : dst_path;
    char dst_info[FULL_SIZE];  // PATH + "/" + NAME + ".info" suffix
    snprintf(dst_info, sizeof(dst_info), "%s/%s.info", dst_dir, name_only);

    if (rename(src_info, dst_info) != 0) {
        if (errno == EXDEV) {
            unlink(dst_info);
            if (wb_fileops_copy(src_info, dst_info) == 0) {
                unlink(src_info);
            } else {
                perror("[amiwb] copy sidecar failed");
            }
        } else {
            unlink(dst_info);
            if (rename(src_info, dst_info) != 0) {
                perror("[amiwb] rename sidecar failed");
            }
        }
    }
}

// Remove icon by path from canvas
static void remove_icon_by_path_on_canvas(const char *abs_path, Canvas *canvas) {
    if (!abs_path || !canvas) return;

    FileIcon **icon_array = wb_icons_array_get();
    int icon_count = wb_icons_array_count();

    for (int i = 0; i < icon_count; ++i) {
        FileIcon *ic = icon_array[i];
        if (!ic || ic->display_window != canvas->win) continue;
        if (ic->path && strcmp(ic->path, abs_path) == 0) {
            destroy_icon(ic);
            break;
        }
    }
}

// Refresh canvas helper (exported for wb_icons_ops.c, wb_layout.c)
void refresh_canvas(Canvas *canvas) {
    if (!canvas) return;
    wb_layout_compute_bounds(canvas);
    compute_max_scroll(canvas);
    redraw_canvas(canvas);
}

// ============================================================================
// Drag Helper Functions (Static - Module Private)
// ============================================================================

// Collect all selected icons from canvas into dragged_icons array
static void collect_selected_icons(Canvas *source_canvas) {
    if (!source_canvas) return;

    FileIcon **icons = wb_icons_array_get();
    int total_count = wb_icons_array_count();
    if (!icons || total_count <= 0) return;

    // Count selected icons on source canvas
    int selected_count = 0;
    for (int i = 0; i < total_count; i++) {
        if (icons[i]->display_window == source_canvas->win && icons[i]->selected) {
            selected_count++;
        }
    }

    if (selected_count == 0) return;

    // Allocate array for selected icons
    dragged_icons = malloc(sizeof(FileIcon*) * selected_count);
    if (!dragged_icons) {
        return;
    }

    // Fill array with selected icons
    dragged_icons_count = 0;
    for (int i = 0; i < total_count; i++) {
        if (icons[i]->display_window == source_canvas->win && icons[i]->selected) {
            dragged_icons[dragged_icons_count++] = icons[i];
        }
    }

    // Capture spatial offsets from clicked icon (reference point)
    // This preserves the relative spatial arrangement of icons during drop
    if (dragged_icons_count > 0 && dragged_icon) {
        icon_offset_x = malloc(sizeof(int) * dragged_icons_count);
        icon_offset_y = malloc(sizeof(int) * dragged_icons_count);

        if (icon_offset_x && icon_offset_y) {
            // Calculate offsets relative to the clicked icon
            for (int i = 0; i < dragged_icons_count; i++) {
                icon_offset_x[i] = dragged_icons[i]->x - dragged_icon->x;
                icon_offset_y[i] = dragged_icons[i]->y - dragged_icon->y;
            }
        }
    }
}

// Get desktop directory path
static void get_desktop_directory(char *buf, size_t size) {
    const char *home = getenv("HOME");
    snprintf(buf, size, "%s/Desktop", home ? home : ".");
}

// Calculate drop position in target canvas coordinates
static void calculate_drop_position(Canvas *target, int *place_x, int *place_y) {
    Display *dpy = itn_core_get_display();

    // Get saved drag window screen position
    int drag_icon_screen_x = saved_drag_win_x;
    int drag_icon_screen_y = saved_drag_win_y;

    // Account for centering offset in drag window
    int dx = (drag_win_w - dragged_icon->width) / 2;
    int dy = (drag_win_h - dragged_icon->height - 20) / 2;
    drag_icon_screen_x += dx;
    drag_icon_screen_y += dy;

    // Translate screen coords to target window coords
    int tx = 0, ty = 0;
    Window dummy;
    safe_translate_coordinates(dpy, target->win, DefaultRootWindow(dpy),
                              0, 0, &tx, &ty, &dummy);
    int local_x = drag_icon_screen_x - tx;
    int local_y = drag_icon_screen_y - ty;

    // Adjust for window borders and scrolling
    if (target->type == WINDOW) {
        local_x = max(0, local_x - BORDER_WIDTH_LEFT + target->scroll_x);
        local_y = max(0, local_y - BORDER_HEIGHT_TOP + target->scroll_y);
    }

    *place_x = max(0, local_x);
    *place_y = max(0, local_y);
}

// Restore dragged icon to original position (on failure)
static void restore_dragged_icon_to_origin(void) {
    if (!dragged_icon) return;

    if (saved_source_window != None) {
        dragged_icon->display_window = saved_source_window;
    }
    move_icon(dragged_icon, drag_orig_x, drag_orig_y);

    if (drag_source_canvas) {
        refresh_canvas(drag_source_canvas);
    }
}

// Create icon for dropped file in target canvas
static void create_icon_for_dropped_file(const char *dst_path, Canvas *target,
                                         int place_x, int place_y) {
    const char *base = strrchr(dst_path, '/');
    const char *name_only = base ? base + 1 : dst_path;

    // Determine file type
    struct stat dst_stat;
    int file_type = TYPE_FILE;
    if (stat(dst_path, &dst_stat) == 0) {
        file_type = S_ISDIR(dst_stat.st_mode) ? TYPE_DRAWER : TYPE_FILE;
    }

    // Find icon image path (.info sidecar or default)
    const char *img_path = NULL;
    if (ends_with(name_only, ".info")) {
        img_path = dst_path;
    } else {
        char info_path[FULL_SIZE];
        snprintf(info_path, sizeof(info_path), "%s.info", dst_path);
        struct stat st_info;
        if (stat(info_path, &st_info) == 0) {
            img_path = info_path;
        } else {
            const char *fallback = wb_deficons_get_for_file(name_only,
                                                            file_type == TYPE_DRAWER);
            img_path = fallback ? fallback : dst_path;
        }
    }

    wb_icons_create_with_icon_path(img_path, target, place_x, place_y,
                              dst_path, name_only, file_type);
}

// Remove icon from desktop if source was desktop directory
static void remove_desktop_icon_if_applicable(const char *src_path_abs) {
    const char *home = getenv("HOME");
    if (!home) return;

    char desktop_dir[PATH_SIZE];
    snprintf(desktop_dir, sizeof(desktop_dir), "%s/Desktop/", home);

    if (strncmp(src_path_abs, desktop_dir, strlen(desktop_dir)) == 0) {
        Canvas *desktop = itn_canvas_get_desktop();
        if (desktop) {
            remove_icon_by_path_on_canvas(src_path_abs, desktop);
            refresh_canvas(desktop);
        }
    }
}

// ============================================================================
// Drag Window Management (XRender Transparent Window)
// ============================================================================

static void create_drag_window(void) {
    Display *dpy = itn_core_get_display();
    Window root = DefaultRootWindow(dpy);

    if (drag_win != None) return;

    XVisualInfo vinfo;
    if (!XMatchVisualInfo(dpy, DefaultScreen(dpy), 32, TrueColor, &vinfo)) {
        log_error("[ERROR] No 32-bit visual for drag window");
        return;
    }

    // Save visual and colormap for later use in draw_drag_icon()
    drag_visual = vinfo.visual;
    drag_colormap = XCreateColormap(dpy, root, vinfo.visual, AllocNone);

    XSetWindowAttributes attrs;
    attrs.colormap = drag_colormap;
    attrs.border_pixel = 0;
    attrs.background_pixel = 0;
    attrs.override_redirect = True;

    drag_win = XCreateWindow(dpy, root, 0, 0, drag_win_w, drag_win_h, 0,
                            vinfo.depth, InputOutput, vinfo.visual,
                            CWOverrideRedirect | CWColormap | CWBorderPixel | CWBackPixel,
                            &attrs);

    if (drag_win == None) {
        log_error("[ERROR] Failed to create drag window");
        return;
    }

    // Map window BEFORE creating XRender resources (prevents X server resource leaks)
    XMapWindow(dpy, drag_win);

    // Make input-transparent using X Shape extension
    XShapeCombineMask(dpy, drag_win, ShapeInput, 0, 0, None, ShapeSet);

    // Create XRender picture for compositing
    XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy, vinfo.visual);
    if (!fmt) {
        log_error("[ERROR] No XRender format for drag window");
        XDestroyWindow(dpy, drag_win);
        drag_win = None;
        return;
    }

    target_picture = XRenderCreatePicture(dpy, drag_win, fmt, 0, NULL);
}

static void draw_drag_icon(void) {
    if (!dragged_icon || drag_win == None) return;

    Display *dpy = itn_core_get_display();

    // Clear to transparent
    XRenderColor clear = {0, 0, 0, 0};
    XRenderFillRectangle(dpy, PictOpSrc, target_picture, &clear,
                        0, 0, drag_win_w, drag_win_h);

    // Determine how many icons to show (max 10 for visual clarity)
    int icons_to_show = 1;
    if (dragged_icons_count > 1) {
        icons_to_show = dragged_icons_count < 10 ? dragged_icons_count : 10;
    }

    // Render icons with stacking effect
    for (int i = 0; i < icons_to_show; i++) {
        FileIcon *icon = (i == 0) ? dragged_icon : dragged_icons[i];
        if (!icon || !icon->current_picture) continue;

        // Stack offset: each icon offset by 3px right and 3px down for depth
        int offset_x = i * 3;
        int offset_y = i * 3;

        // Center first icon, offset others
        int dx = (drag_win_w - icon->width) / 2 + offset_x;
        int dy = (drag_win_h - icon->height - 20) / 2 + offset_y;

        // Composite icon
        XRenderComposite(dpy, PictOpOver,
                        icon->current_picture, None, target_picture,
                        0, 0, 0, 0, dx, dy,
                        icon->width, icon->height);
    }

    // Draw label (count for multiple, name for single)
    XftDraw *xft = XftDrawCreate(dpy, drag_win, drag_visual, drag_colormap);
    if (xft) {
        XftColor color;
        XRenderColor rc = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
        XftColorAllocValue(dpy, drag_visual, drag_colormap, &rc, &color);

        XftFont *font = get_font();
        if (font) {
            const char *text = NULL;
            char count_text[32];
            int ty;

            if (dragged_icons_count > 1) {
                // Multiple icons: show count at bottom
                snprintf(count_text, sizeof(count_text), "%d items", dragged_icons_count);
                text = count_text;
                ty = drag_win_h - 10;
            } else if (dragged_icon->label) {
                // Single icon: show name below icon (original position)
                text = dragged_icon->label;
                int dy = (drag_win_h - dragged_icon->height - 20) / 2;
                ty = dy + dragged_icon->height + 15;
            } else {
                text = NULL;
            }

            if (text) {
                int text_w = get_text_width(text);
                int tx = (drag_win_w - text_w) / 2;

                XftDrawStringUtf8(xft, &color, font, tx, ty,
                                 (const FcChar8*)text, strlen(text));
            }
        }

        XftColorFree(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                    DefaultColormap(dpy, DefaultScreen(dpy)), &color);
        XftDrawDestroy(xft);
    }
}

static void update_drag_window_position(int root_x, int root_y) {
    if (drag_win == None) return;

    Display *dpy = itn_core_get_display();

    // Calculate new position
    int x = root_x - drag_win_w / 2;
    int y = root_y - drag_win_h / 2;

    // Damage old position (before move) so compositor erases old ghost
    if (last_root_x != -10000 && last_root_y != -10000) {
        int old_x = last_root_x - drag_win_w / 2;
        int old_y = last_root_y - drag_win_h / 2;
        DAMAGE_RECT(old_x, old_y, drag_win_w, drag_win_h);
    }

    // Move window to new position
    XMoveWindow(dpy, drag_win, x, y);

    // CRITICAL: Update compositor's cached position for override-redirect window
    // Override-redirect windows don't generate ConfigureNotify, so cached position
    // is never updated via events. Must manually update after XMoveWindow().
    itn_composite_update_override_position(drag_win, x, y);

    // Damage new position (after move) so compositor renders at new location
    DAMAGE_RECT(x, y, drag_win_w, drag_win_h);

    // Schedule frame to render drag window at new position
    SCHEDULE_FRAME();

    // Save position for next motion event
    last_root_x = root_x;
    last_root_y = root_y;
}

static void destroy_drag_window(void) {
    if (drag_win == None) return;

    Display *dpy = itn_core_get_display();

    if (target_picture) {
        XRenderFreePicture(dpy, target_picture);
        target_picture = 0;
    }

    XDestroyWindow(dpy, drag_win);
    drag_win = None;
    dragging_floating = false;
}

// ============================================================================
// Canvas Detection Under Pointer
// ============================================================================

static Canvas *canvas_under_pointer(void) {
    Display *dpy = itn_core_get_display();
    Window root = DefaultRootWindow(dpy);
    Window root_ret, child_ret;
    int rx, ry, wx, wy;
    unsigned int mask;

    if (!XQueryPointer(dpy, root, &root_ret, &child_ret, &rx, &ry, &wx, &wy, &mask)) {
        return NULL;
    }

    // Check cache
    if (pointer_cache.valid && pointer_cache.cached_x == rx && pointer_cache.cached_y == ry) {
        if (pointer_cache.cached_canvas) {
            XWindowAttributes wa;
            if (safe_get_window_attributes(dpy, pointer_cache.cached_canvas->win, &wa) &&
                wa.map_state == IsViewable) {
                return pointer_cache.cached_canvas;
            }
        }
    }

    // Cache miss - do tree walk
    Window r, p, *children = NULL;
    unsigned int n = 0;
    if (!XQueryTree(dpy, root, &r, &p, &children, &n)) return NULL;

    Canvas *best = NULL;
    for (int i = (int)n - 1; i >= 0; --i) {
        Window w = children[i];
        Canvas *c = itn_canvas_find_by_window(w);
        if (!c || c->type == MENU) continue;

        if (c->x <= rx && rx < c->x + c->width &&
            c->y <= ry && ry < c->y + c->height) {
            XWindowAttributes wa;
            if (safe_get_window_attributes(dpy, w, &wa) && wa.map_state == IsViewable) {
                if (c->type == WINDOW) {
                    best = c;
                    break;
                }
                if (!best) best = c;
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

void invalidate_pointer_cache(void) {
    pointer_cache.valid = false;
}

// ============================================================================
// Drag State Machine
// ============================================================================

void start_drag_icon(FileIcon *icon, int x, int y) {
    dragged_icon = icon;
    drag_start_x = x;
    drag_start_y = y;
    drag_source_canvas = itn_canvas_find_by_window(icon->display_window);
    saved_source_window = icon->display_window;
    drag_orig_x = icon->x;
    drag_orig_y = icon->y;
    dragging_floating = false;
    drag_active = false;

    // Note: Icon collection moved to continue_drag_icon() after 10px threshold
    // This prevents false positives on simple clicks

    Display *dpy = itn_core_get_display();
    int rx, ry, wx, wy;
    unsigned int mask;
    Window root_ret, child_ret;
    XQueryPointer(dpy, DefaultRootWindow(dpy), &root_ret, &child_ret,
                 &rx, &ry, &wx, &wy, &mask);
    drag_start_root_x = rx;
    drag_start_root_y = ry;
}

void continue_drag_icon(XMotionEvent *event, Canvas *canvas) {
    if (!dragged_icon) return;
    Display *dpy = event->display;

    // Enforce movement threshold
    if (!drag_active) {
        int dx = event->x_root - drag_start_root_x;
        int dy = event->y_root - drag_start_root_y;
        if (dx*dx + dy*dy < 10*10) return;

        // Activate drag
        drag_active = true;

        // Collect selected icons if dragging a selected icon (only when drag actually starts)
        if (dragged_icon && dragged_icon->selected && drag_source_canvas) {
            collect_selected_icons(drag_source_canvas);
        }

        // Hide all icons in selection during drag
        if (dragged_icons_count > 0) {
            for (int i = 0; i < dragged_icons_count; i++) {
                if (dragged_icons[i]) {
                    dragged_icons[i]->display_window = None;
                }
            }
        } else if (dragged_icon && saved_source_window != None) {
            // Single icon drag
            dragged_icon->display_window = None;
        }

        if (drag_source_canvas) redraw_canvas(drag_source_canvas);
    }

    // Create floating drag window
    if (!dragging_floating) {
        create_drag_window();
        draw_drag_icon();
        dragging_floating = true;
    }

    update_drag_window_position(event->x_root, event->y_root);

    // Check for XDND target
    Window xdnd_target = xdnd_find_target(dpy, event->x_root, event->y_root);

    // Handle XDND protocol
    if (xdnd_target != None && xdnd_target != xdnd_ctx.current_target) {
        if (xdnd_ctx.current_target != None) {
            xdnd_send_leave(dpy, canvas->win, xdnd_ctx.current_target);
        }
        xdnd_send_enter(dpy, canvas->win, xdnd_target);
        xdnd_ctx.source_window = canvas->win;
    }

    if (xdnd_ctx.current_target != None) {
        xdnd_send_position(dpy, canvas->win, xdnd_ctx.current_target,
                          event->x_root, event->y_root, event->time,
                          xdnd_ctx.XdndActionCopy);
    }

    if (xdnd_target == None && xdnd_ctx.current_target != None) {
        xdnd_send_leave(dpy, canvas->win, xdnd_ctx.current_target);
        xdnd_ctx.current_target = None;
    }
}

// Helper: Handle XDND protocol drops
static bool handle_xdnd_drop(Canvas *canvas) {
    Display *dpy = itn_core_get_display();

    if (xdnd_ctx.current_target == None) {
        return false;  // Not an XDND drop
    }

    destroy_drag_window();

    Window source_win = canvas ? canvas->win : DefaultRootWindow(dpy);
    XSetSelectionOwner(dpy, xdnd_ctx.XdndSelection, source_win, CurrentTime);
    xdnd_send_drop(dpy, source_win, xdnd_ctx.current_target, CurrentTime);

    if (saved_source_window != None) {
        dragged_icon->display_window = saved_source_window;
    }
    if (drag_source_canvas) {
        refresh_canvas(drag_source_canvas);
    }

    drag_active = false;
    dragging_floating = false;
    saved_source_window = None;
    drag_source_canvas = NULL;
    return true;  // Handled
}

// Helper: Handle iconified window drops
static bool handle_iconified_window_drop(Canvas *target) {
    if (!dragged_icon || dragged_icon->type != TYPE_ICONIFIED) {
        return false;  // Not an iconified window
    }

    if (target && target->type == DESKTOP && drag_source_canvas &&
        drag_source_canvas->type == DESKTOP) {
        // Allowed: desktop to desktop
        if (drag_active) {
            int place_x = 0, place_y = 0;
            calculate_drop_position(target, &place_x, &place_y);
            move_icon(dragged_icon, place_x, place_y);
        }

        if (saved_source_window != None) {
            dragged_icon->display_window = saved_source_window;
        }
        if (drag_source_canvas) {
            refresh_canvas(drag_source_canvas);
        }
    } else {
        // Forbidden: restore original position
        restore_dragged_icon_to_origin();
    }

    dragged_icon = NULL;
    drag_active = false;
    drag_source_canvas = NULL;
    saved_source_window = None;
    destroy_drag_window();
    return true;  // Handled
}

// Helper: Handle prime icon drops (System "/" or Home)
static bool handle_prime_icon_drop(Canvas *target) {
    if (!dragged_icon || !dragged_icon->path) {
        return false;  // No icon or path
    }

    const char *home = getenv("HOME");
    bool is_prime_icon = (strcmp(dragged_icon->path, "/") == 0 ||
                         (home && strcmp(dragged_icon->path, home) == 0));

    if (!is_prime_icon) {
        return false;  // Not a prime icon
    }

    if (target && target->type == DESKTOP && drag_source_canvas &&
        drag_source_canvas->type == DESKTOP) {
        // Allowed: desktop to desktop
        if (drag_active) {
            int place_x = 0, place_y = 0;
            calculate_drop_position(target, &place_x, &place_y);
            move_icon(dragged_icon, place_x, place_y);
        }

        if (saved_source_window != None) {
            dragged_icon->display_window = saved_source_window;
        }
        if (drag_source_canvas) {
            refresh_canvas(drag_source_canvas);
        }
    } else {
        // Forbidden: restore original position
        restore_dragged_icon_to_origin();
    }

    dragged_icon = NULL;
    drag_active = false;
    drag_source_canvas = NULL;
    saved_source_window = None;
    destroy_drag_window();
    return true;  // Handled
}

// Helper: Perform cross-canvas drop (file move between windows)
static void perform_cross_canvas_drop(Canvas *target) {
    bool can_move_file = (dragged_icon && dragged_icon->path && *dragged_icon->path);
    bool target_is_valid_dir_window = (target && target->type == WINDOW &&
                                       target->path && is_directory(target->path));
    bool target_is_desktop = (target && target->type == DESKTOP);

    if (!drag_source_canvas || !target || target == drag_source_canvas ||
        (!target_is_desktop && !target_is_valid_dir_window) || !can_move_file) {
        return;  // Not a valid cross-canvas drop
    }

    // Determine destination directory
    char dst_dir[PATH_SIZE];
    if (target_is_desktop) {
        get_desktop_directory(dst_dir, sizeof(dst_dir));
    } else {
        snprintf(dst_dir, sizeof(dst_dir), "%s", target->path ? target->path : ".");
    }

    // Safety check: prevent moving directory into itself
    if (dragged_icon->type == TYPE_DRAWER) {
        size_t src_len = strlen(dragged_icon->path);
        if (strncmp(dst_dir, dragged_icon->path, src_len) == 0 &&
            (dst_dir[src_len] == '/' || dst_dir[src_len] == '\0')) {
            log_error("[WARNING] Cannot move directory into itself");
            restore_dragged_icon_to_origin();
            dragged_icon = NULL;
            drag_active = false;
            drag_source_canvas = NULL;
            saved_source_window = None;
            destroy_drag_window();
            return;
        }
    }

    char dst_path[PATH_SIZE];
    char src_path_abs[PATH_SIZE];
    snprintf(src_path_abs, sizeof(src_path_abs), "%s",
            dragged_icon->path ? dragged_icon->path : "");

    // Calculate drop position
    int place_x = 0, place_y = 0;
    calculate_drop_position(target, &place_x, &place_y);

    // Move file with extended version
    int moved = wb_fileops_move_ex(dragged_icon->path, dst_dir, dst_path,
                                  sizeof(dst_path), target, place_x, place_y);

    if (moved == 0 || moved == 2) {
        destroy_icon(dragged_icon);
        dragged_icon = NULL;

        if (moved == 0) {
            move_sidecar_info_file(src_path_abs, dst_dir, dst_path);
        }

        if (moved == 2) {
            // Cross-filesystem move: use progress system for copy+delete
            ProgressMessage icon_meta = {0};
            icon_meta.create_icon = true;
            icon_meta.has_sidecar = false;
            icon_meta.icon_x = place_x;
            icon_meta.icon_y = place_y;
            icon_meta.target_window = target->win;
            snprintf(icon_meta.dest_path, sizeof(icon_meta.dest_path), "%s", dst_path);
            snprintf(icon_meta.dest_dir, sizeof(icon_meta.dest_dir), "%s", dst_dir);

            // Check for sidecar .info file
            char src_info[FULL_SIZE];
            snprintf(src_info, sizeof(src_info), "%s.info", src_path_abs);
            if (check_if_file_exists(src_info)) {
                icon_meta.has_sidecar = true;
                snprintf(icon_meta.sidecar_src, sizeof(icon_meta.sidecar_src), "%s", src_info);

                // Build destination .info path
                const char *base = strrchr(dst_path, '/');
                const char *name_ptr = base ? base + 1 : dst_path;

                // Validate filename length and construct path
                char filename[NAME_SIZE];
                size_t name_len = strlen(name_ptr);

                if (name_len >= sizeof(filename)) {
                    // Filename too long - skip sidecar handling
                    icon_meta.has_sidecar = false;
                    log_error("[WARNING] Filename too long for sidecar: %s", name_ptr);
                } else {
                    // SAFE: name_len validated < NAME_SIZE (128 bytes)
                    // Pragma silences false positive - truncation impossible due to check above
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
                    snprintf(filename, sizeof(filename), "%s", name_ptr);
#pragma GCC diagnostic pop

                    // Check if full path will fit
                    size_t needed = strlen(dst_dir) + 1 + strlen(filename) + 5 + 1; // "/" + name + ".info" + null
                    if (needed <= sizeof(icon_meta.sidecar_dst)) {
                        // SAFE: total length validated to fit in FULL_SIZE buffer
                        // Pragma silences false positive - truncation impossible due to check above
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
                        snprintf(icon_meta.sidecar_dst, sizeof(icon_meta.sidecar_dst),
                                "%s/%s.info", dst_dir, filename);
#pragma GCC diagnostic pop
                    } else {
                        // Path too long - skip sidecar handling
                        icon_meta.has_sidecar = false;
                        log_error("[WARNING] Path too long for sidecar: %s/%s.info", dst_dir, filename);
                    }
                }
            }

            // Perform cross-filesystem move with progress
            wb_progress_perform_operation_ex(FILE_OP_MOVE, src_path_abs,
                                                    dst_path, NULL, &icon_meta);

            if (drag_source_canvas) {
                refresh_canvas(drag_source_canvas);
            }
            return;
        }

        // Synchronous move - create icon
        create_icon_for_dropped_file(dst_path, target, place_x, place_y);

        // Remove desktop icon if moved from Desktop
        remove_desktop_icon_if_applicable(src_path_abs);

        // Apply layout
        if (target->type == WINDOW && target->view_mode == VIEW_NAMES) {
            wb_layout_apply_view(target);
        } else if (target->type == WINDOW && target->view_mode == VIEW_ICONS) {
            wb_layout_compute_bounds(target);
        }
        compute_max_scroll(target);

        if (drag_source_canvas) {
            refresh_canvas(drag_source_canvas);
        }
        redraw_canvas(target);
    } else {
        // Move failed - restore icon
        restore_dragged_icon_to_origin();
    }
}

// Helper: Process multi-icon drop operation (copy or move all selected icons)
static void perform_multi_icon_drop(Canvas *target, bool force_copy) {
    if (!dragged_icons || dragged_icons_count == 0) return;
    if (!target) return;

    // No drag occurred - just a click, do nothing
    if (!drag_active) {
        return;
    }

    bool is_same_canvas = (target == drag_source_canvas);

    // Same-canvas repositioning (no Shift): just move icons, no file operations
    if (is_same_canvas && !force_copy) {
        int place_x = 0, place_y = 0;
        calculate_drop_position(target, &place_x, &place_y);

        // Calculate bounds for target canvas
        int min_x, min_y, max_x, max_y;
        if (target->type == DESKTOP) {
            min_x = 20;
            min_y = 200;  // Below Home icon
            max_x = target->width - 110;
            max_y = target->height - 100;
        } else {
            min_x = 10;
            min_y = 10;
            max_x = target->width - BORDER_WIDTH_RIGHT - 110;
            max_y = target->height - BORDER_HEIGHT_BOTTOM - 100;
        }

        for (int i = 0; i < dragged_icons_count; i++) {
            FileIcon *icon = dragged_icons[i];
            if (!icon) continue;

            // Restore display_window (was set to None during drag)
            if (saved_source_window != None) {
                icon->display_window = saved_source_window;
            }

            // Apply spatial offset to preserve relative positions
            int icon_x = place_x;
            int icon_y = place_y;
            if (icon_offset_x && icon_offset_y) {
                icon_x += icon_offset_x[i];
                icon_y += icon_offset_y[i];
            }

            // Clamp to canvas bounds (prevent icons from going offscreen)
            icon_x = max(min_x, min(icon_x, max_x));
            icon_y = max(min_y, min(icon_y, max_y));

            move_icon(icon, icon_x, icon_y);
        }

        refresh_canvas(target);
        return;
    }

    // File operations: same-canvas duplication (Shift) OR cross-canvas move/copy
    // Determine destination directory
    char dst_dir[PATH_SIZE];
    if (target->type == DESKTOP) {
        get_desktop_directory(dst_dir, sizeof(dst_dir));
    } else if (target->type == WINDOW && target->path) {
        snprintf(dst_dir, sizeof(dst_dir), "%s", target->path);
    } else {
        return;  // Invalid target
    }

    // Calculate base drop position
    int place_x = 0, place_y = 0;
    calculate_drop_position(target, &place_x, &place_y);

    // Calculate bounds for target canvas (for spatial offset clamping)
    int min_x, min_y, max_x, max_y;
    if (target->type == DESKTOP) {
        min_x = 20;
        min_y = 200;  // Below Home icon
        max_x = target->width - 110;
        max_y = target->height - 100;
    } else {
        min_x = 10;
        min_y = 10;
        max_x = target->width - BORDER_WIDTH_RIGHT - 110;
        max_y = target->height - BORDER_HEIGHT_BOTTOM - 100;
    }

    // CRITICAL: Restore display_window for ALL icons BEFORE processing operations
    // Icons were hidden during drag (display_window = None)
    // If operations fail, icons must be visible again in source window
    for (int i = 0; i < dragged_icons_count; i++) {
        if (dragged_icons[i] && saved_source_window != None) {
            dragged_icons[i]->display_window = saved_source_window;
        }
    }

    // Protect dragged_icons array from being freed during iteration
    // (destroy_icon can trigger cleanup if destroying the clicked icon)
    in_multi_icon_processing = true;

    // Process each icon in selection (synchronous moves, async for cross-filesystem)
    for (int i = 0; i < dragged_icons_count; i++) {
        FileIcon *icon = dragged_icons[i];
        if (!icon || !icon->path || !*icon->path) {
            continue;
        }

        // Skip prime icons (System "/" and Home directory)
        const char *home = getenv("HOME");
        bool is_prime = (strcmp(icon->path, "/") == 0 || (home && strcmp(icon->path, home) == 0));
        if (is_prime) {
            continue;
        }

        // Skip iconified windows (not regular files/directories)
        if (icon->type == TYPE_ICONIFIED) {
            continue;
        }

        // Safety check: prevent moving directory into itself
        if (icon->type == TYPE_DRAWER && !force_copy) {
            size_t src_len = strlen(icon->path);
            if (strncmp(dst_dir, icon->path, src_len) == 0 &&
                (dst_dir[src_len] == '/' || dst_dir[src_len] == '\0')) {
                continue;
            }
        }

        // Apply spatial offset to preserve relative positions
        int icon_x = place_x;
        int icon_y = place_y;
        if (icon_offset_x && icon_offset_y) {
            icon_x += icon_offset_x[i];
            icon_y += icon_offset_y[i];
        }

        // Clamp to canvas bounds (prevent icons from going offscreen)
        icon_x = max(min_x, min(icon_x, max_x));
        icon_y = max(min_y, min(icon_y, max_y));

        // Build destination path (use FULL_SIZE for path + "/" + filename combinations)
        char dst_path[FULL_SIZE];

        if (force_copy) {
            // Copy operation - use progress system
            const char *basename_ptr = strrchr(icon->path, '/');
            const char *filename = basename_ptr ? basename_ptr + 1 : icon->path;

            if (is_same_canvas) {
                snprintf(dst_path, sizeof(dst_path), "%s/copy_%s", dst_dir, filename);
            } else {
                snprintf(dst_path, sizeof(dst_path), "%s/%s", dst_dir, filename);
            }

            // Prepare icon metadata
            ProgressMessage icon_meta = {0};
            icon_meta.create_icon = true;
            icon_meta.icon_x = icon_x;
            icon_meta.icon_y = icon_y;
            icon_meta.target_window = target->win;
            snprintf(icon_meta.dest_path, sizeof(icon_meta.dest_path), "%s", dst_path);
            snprintf(icon_meta.dest_dir, sizeof(icon_meta.dest_dir), "%s", dst_dir);

            wb_progress_perform_operation_ex(FILE_OP_COPY, icon->path, dst_path, NULL, &icon_meta);
        } else {
            // Move operation - try synchronous move first
            int moved = wb_fileops_move_ex(icon->path, dst_dir, dst_path, sizeof(dst_path),
                                          target, icon_x, icon_y);

            if (moved == 0) {
                // Synchronous move succeeded
                char src_path_abs[PATH_SIZE];
                snprintf(src_path_abs, sizeof(src_path_abs), "%s", icon->path);

                destroy_icon(icon);
                move_sidecar_info_file(src_path_abs, dst_dir, dst_path);
                create_icon_for_dropped_file(dst_path, target, icon_x, icon_y);
                remove_desktop_icon_if_applicable(src_path_abs);
            } else if (moved == 2) {
                // Cross-filesystem - use async progress system
                char src_path_abs[PATH_SIZE];
                snprintf(src_path_abs, sizeof(src_path_abs), "%s", icon->path);

                ProgressMessage icon_meta = {0};
                icon_meta.create_icon = true;
                icon_meta.icon_x = icon_x;
                icon_meta.icon_y = icon_y;
                icon_meta.target_window = target->win;
                snprintf(icon_meta.dest_path, sizeof(icon_meta.dest_path), "%s", dst_path);
                snprintf(icon_meta.dest_dir, sizeof(icon_meta.dest_dir), "%s", dst_dir);

                // Check for sidecar .info file
                char src_info[FULL_SIZE];
                snprintf(src_info, sizeof(src_info), "%s.info", src_path_abs);
                if (check_if_file_exists(src_info)) {
                    if (strlen(dst_path) + 5 + 1 <= sizeof(icon_meta.sidecar_dst)) {
                        icon_meta.has_sidecar = true;
                        snprintf(icon_meta.sidecar_src, sizeof(icon_meta.sidecar_src), "%s", src_info);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
                        snprintf(icon_meta.sidecar_dst, sizeof(icon_meta.sidecar_dst), "%s.info", dst_path);
#pragma GCC diagnostic pop
                    }
                }

                wb_progress_perform_operation_ex(FILE_OP_MOVE, src_path_abs, dst_path, NULL, &icon_meta);
                destroy_icon(icon);
            }
            // else: moved == -1 (failure), icon stays in source with display_window restored
        }
    }

    // Processing complete - allow cleanup now
    in_multi_icon_processing = false;

    // Manually clean up the multi-icon array (since wb_drag_clear_dragged_icon was blocked)
    if (dragged_icons) {
        free(dragged_icons);
        dragged_icons = NULL;
        dragged_icons_count = 0;
    }

    // Refresh canvases
    if (drag_source_canvas) {
        refresh_canvas(drag_source_canvas);
    }
    if (target && target != drag_source_canvas) {
        refresh_canvas(target);
    }
}

// Helper: Perform same-canvas drop or handle invalid drop
static void perform_same_canvas_drop(Canvas *target) {
    if (!dragged_icon) {
        return;
    }

    if (!drag_active) {
        // No drag occurred
    } else if (target == drag_source_canvas) {
        // Same-canvas drag: reposition icon
        int place_x = 0, place_y = 0;
        calculate_drop_position(drag_source_canvas, &place_x, &place_y);
        if (saved_source_window != None) dragged_icon->display_window = saved_source_window;
        move_icon(dragged_icon, place_x, place_y);
    } else {
        // Invalid target: restore original position
        restore_dragged_icon_to_origin();
    }

    if (drag_source_canvas) {
        wb_layout_compute_bounds(drag_source_canvas);
        compute_max_scroll(drag_source_canvas);
        redraw_canvas(drag_source_canvas);
    }
}

void end_drag_icon(Canvas *canvas) {
    Display *dpy = itn_core_get_display();

    // Detect Shift key at drop time for copy-instead-of-move modifier
    Window root_ret, child_ret;
    int root_x, root_y, win_x, win_y;
    unsigned int mask;
    XQueryPointer(dpy, DefaultRootWindow(dpy), &root_ret, &child_ret,
                  &root_x, &root_y, &win_x, &win_y, &mask);
    bool shift_held = (mask & ShiftMask) != 0;

    // Save drag window position before destroying
    saved_drag_win_x = 0;
    saved_drag_win_y = 0;
    if (drag_win != None) {
        Window child;
        safe_translate_coordinates(dpy, drag_win, DefaultRootWindow(dpy),
                                  0, 0, &saved_drag_win_x, &saved_drag_win_y, &child);
    }

    destroy_drag_window();

    // Early exit: no dragged icon
    if (!dragged_icon) {
        drag_source_canvas = NULL;
        saved_source_window = None;
        if (xdnd_ctx.current_target != None) {
            xdnd_send_leave(dpy, canvas ? canvas->win : None, xdnd_ctx.current_target);
            xdnd_ctx.current_target = None;
        }
        // Cleanup multi-icon array if allocated
        if (dragged_icons) {
            free(dragged_icons);
            dragged_icons = NULL;
            dragged_icons_count = 0;
        }
        // Cleanup spatial offset arrays
        if (icon_offset_x) {
            free(icon_offset_x);
            icon_offset_x = NULL;
        }
        if (icon_offset_y) {
            free(icon_offset_y);
            icon_offset_y = NULL;
        }
        return;
    }

    // Handle XDND drop
    if (handle_xdnd_drop(canvas)) {
        // Cleanup multi-icon array
        if (dragged_icons) {
            free(dragged_icons);
            dragged_icons = NULL;
            dragged_icons_count = 0;
        }
        // Cleanup spatial offset arrays
        if (icon_offset_x) {
            free(icon_offset_x);
            icon_offset_x = NULL;
        }
        if (icon_offset_y) {
            free(icon_offset_y);
            icon_offset_y = NULL;
        }
        return;
    }

    // Get target canvas
    Canvas *target = canvas_under_pointer();

    // Handle multi-icon drop if selection exists
    if (dragged_icons_count > 0) {
        perform_multi_icon_drop(target, shift_held);

        // Cleanup multi-icon array
        free(dragged_icons);
        dragged_icons = NULL;
        dragged_icons_count = 0;

        // Cleanup spatial offset arrays
        if (icon_offset_x) {
            free(icon_offset_x);
            icon_offset_x = NULL;
        }
        if (icon_offset_y) {
            free(icon_offset_y);
            icon_offset_y = NULL;
        }

        // Cleanup drag state
        dragged_icon = NULL;
        drag_source_canvas = NULL;
        saved_source_window = None;
        drag_active = false;
        return;
    }

    // Handle special icon types (single-icon only)
    if (handle_iconified_window_drop(target)) {
        return;
    }

    if (handle_prime_icon_drop(target)) {
        return;
    }

    // Handle cross-canvas or same-canvas drops (single-icon only)
    perform_cross_canvas_drop(target);
    perform_same_canvas_drop(target);

    // Cleanup
    dragged_icon = NULL;
    drag_source_canvas = NULL;
    saved_source_window = None;
    drag_active = false;
}

// ============================================================================
// Drag State Accessors (for wb_icons_create.c)
// ============================================================================

bool wb_drag_is_active(void) {
    return drag_active;
}

void wb_drag_set_inactive(void) {
    drag_active = false;
}

Canvas *wb_drag_get_source_canvas(void) {
    return drag_source_canvas;
}

Window wb_drag_get_saved_window(void) {
    return saved_source_window;
}

FileIcon *wb_drag_get_dragged_icon(void) {
    return dragged_icon;
}

void wb_drag_clear_dragged_icon(void) {
    dragged_icon = NULL;

    // Also clear multi-icon array (unless we're in the middle of processing it)
    // During multi-icon drop, destroy_icon() can be called on the clicked icon,
    // which would trigger this cleanup and free the array we're iterating through
    if (dragged_icons && !in_multi_icon_processing) {
        free(dragged_icons);
        dragged_icons = NULL;
        dragged_icons_count = 0;
    }

    // Clear spatial offset arrays (same lifetime as dragged_icons)
    if (icon_offset_x && !in_multi_icon_processing) {
        free(icon_offset_x);
        icon_offset_x = NULL;
    }
    if (icon_offset_y && !in_multi_icon_processing) {
        free(icon_offset_y);
        icon_offset_y = NULL;
    }
}

void wb_drag_cleanup_window(void) {
    destroy_drag_window();
}

// ============================================================================
// Public API
// ============================================================================

void workbench_cleanup_drag_state(void) {
    destroy_drag_window();

    // Cleanup multi-icon array
    if (dragged_icons) {
        free(dragged_icons);
        dragged_icons = NULL;
        dragged_icons_count = 0;
    }

    // Cleanup spatial offset arrays
    if (icon_offset_x) {
        free(icon_offset_x);
        icon_offset_x = NULL;
    }
    if (icon_offset_y) {
        free(icon_offset_y);
        icon_offset_y = NULL;
    }

    if (dragged_icon && saved_source_window != None) {
        dragged_icon->display_window = saved_source_window;
        saved_source_window = None;
    }

    if (xdnd_ctx.current_target != None) {
        xdnd_ctx.current_target = None;
    }

    if (drag_source_canvas) {
        refresh_canvas(drag_source_canvas);
    }

    dragged_icon = NULL;
    drag_active = false;
    dragging_floating = false;
    drag_source_canvas = NULL;
}
