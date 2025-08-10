// File: workbench.c
#define _POSIX_C_SOURCE 200809L
#include "workbench.h"
#include "render.h"
#include "intuition.h"
#include "compositor.h"
#include "config.h"  // Added to include config.h for max/min macros
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/types.h>
#include <time.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <signal.h>

// Forward declarations for local helpers used later
static Window window_under_pointer(void);
static Canvas *canvas_under_pointer(void);
static int move_file_to_directory(const char *src_path, const char *dst_dir, char *dst_path, size_t dst_sz);
static bool is_directory(const char *path);
static void remove_icon_by_path_on_canvas(const char *abs_path, Canvas *canvas);
// Forward declarations for functions used before their definitions
void refresh_canvas_from_directory(Canvas *canvas, const char *dirpath);
extern void open_file(FileIcon *icon);
extern void restore_iconified(FileIcon *icon);

// Drag rendering helpers (direct compositing into target canvas)
static void create_drag_window(void);            // now initializes drag painter state
static void draw_drag_icon(void);                // draws into current target window
static void update_drag_window_position(int root_x, int root_y);
static void destroy_drag_window(void);           // cleans up drag painter resources

#define INITIAL_ICON_CAPACITY 16

// Global state for workbench
static FileIcon **icon_array = NULL;        // Dynamic array of all icons
static int icon_count = 0;                  // Current number of icons
static int icon_array_size = 0;             // Allocated size of icon array
static FileIcon *dragged_icon = NULL;       // Moved from events.c
static int drag_start_x, drag_start_y;      // Moved from events.c
static int drag_start_root_x, drag_start_root_y; // start position in root coordinates
static Canvas *drag_source_canvas = NULL;  // Track source canvas for cross-canvas drops
static bool dragging_floating = false;     // Whether we render a floating drag image
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
static Canvas *target_canvas = NULL;
static Canvas *prev_canvas = NULL;         // For erasing previous frame by redraw

// Case-insensitive label comparator for FileIcon** (used in list view)
static int label_cmp(const void *a, const void *b) {
    const FileIcon *ia = *(FileIcon* const*)a;
    const FileIcon *ib = *(FileIcon* const*)b;
    const char *la = (ia && ia->label) ? ia->label : "";
    const char *lb = (ib && ib->label) ? ib->label : "";
    return strcasecmp(la, lb);
}

static int last_draw_x = -10000, last_draw_y = -10000; // last window-relative draw pos
static int last_root_x = -10000, last_root_y = -10000; // last root coords of pointer
static bool use_floating_window = false;   // Always use ARGB top-level drag window

// Check if a compositing manager is present for the current screen
static bool is_composited(Display *dpy) {
    int screen = DefaultScreen(dpy);
    char sel_name[32];
    snprintf(sel_name, sizeof(sel_name), "_NET_WM_CM_S%d", screen);
    Atom sel = XInternAtom(dpy, sel_name, False);
    Window owner = XGetSelectionOwner(dpy, sel);
    return owner != None;
}

// ... (rest of the code remains the same)

// Start dragging icon
static void start_drag_icon(FileIcon *icon, int x, int y) {
    const char *dbg = getenv("WB_DEBUG");
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
    bool can_move_file = (dragged_icon && dragged_icon->path && *dragged_icon->path &&
                          dragged_icon->type != TYPE_ICONIFIED &&
                          dragged_icon->type != TYPE_DRAWER); // SAFETY: do not move directories
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

            // Prefer .info alongside the moved file for its image
            char info_path[2048];
            snprintf(info_path, sizeof(info_path), "%s.info", dst_path);
            struct stat st_info;
            const char *img_path = (stat(info_path, &st_info) == 0) ? info_path : dst_path;
            create_icon(img_path, target, place_x, place_y);
            // Fix the icon's real file path and label
            FileIcon *new_icon = icon_array[get_icon_count() - 1];
            if (new_icon->path) { free(new_icon->path); }
            new_icon->path = strdup(dst_path);
            const char *base = strrchr(dst_path, '/');
            if (new_icon->label) { free(new_icon->label); }
            new_icon->label = strdup(base ? base + 1 : dst_path);

            // 3) If the source was Desktop directory, also remove the desktop canvas icon
            const char *home = getenv("HOME");
            char desktop_dir[1024] = {0};
            if (home) snprintf(desktop_dir, sizeof(desktop_dir), "%s/Desktop/", home);
            if (home && strncmp(src_path_abs, desktop_dir, strlen(desktop_dir)) == 0) {
                Canvas *desktop = get_desktop_canvas();
                if (desktop) {
                    remove_icon_by_path_on_canvas(src_path_abs, desktop);
                    compute_content_bounds(desktop);
                    compute_max_scroll(desktop);
                    redraw_canvas(desktop);
                }
            }

            // 4) Recompute bounds and redraw both canvases without auto cleanup
            if (drag_source_canvas) { compute_content_bounds(drag_source_canvas); compute_max_scroll(drag_source_canvas); redraw_canvas(drag_source_canvas); }
            compute_content_bounds(target); compute_max_scroll(target); redraw_canvas(target);
        } else {
            // Move failed, restore icon to original position on source canvas
            if (dragged_icon) {
                if (saved_source_window != None) dragged_icon->display_window = saved_source_window;
                move_icon(dragged_icon, drag_orig_x, drag_orig_y);
            }
            if (drag_source_canvas) {
                compute_content_bounds(drag_source_canvas);
                compute_max_scroll(drag_source_canvas);
                redraw_canvas(drag_source_canvas);
            }
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

// ... (rest of the code remains the same)

// ========================
// floating drag window impl
// ========================

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
    target_canvas = NULL;
    prev_canvas = NULL;
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
        XDestroyWindow(dpy, drag_win); drag_win = None;
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
    if (drag_win != None) { XDestroyWindow(dpy, drag_win); drag_win = None; }
    target_win = None;
    target_visual = NULL;
    target_colormap = 0;
    target_canvas = NULL;
    prev_canvas = NULL;
    dragging_floating = false;
}

// ========================
// Missing helpers and API restored
// ========================

static Window window_under_pointer(void) {
    Display *dpy = get_display();
    Window root = DefaultRootWindow(dpy);
    Window root_ret = None, child_ret = None;
    int root_x, root_y, win_x, win_y;
    unsigned int mask;
    if (XQueryPointer(dpy, root, &root_ret, &child_ret, &root_x, &root_y, &win_x, &win_y, &mask)) {
        return child_ret ? child_ret : root_ret;
    }
    return None;
}

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

// returns 0 on success, non-zero on failure
static int move_file_to_directory(const char *src_path, const char *dst_dir, char *dst_path, size_t dst_sz) {
    if (!src_path || !dst_dir || !dst_path || !*src_path || !*dst_dir) return -1;
    // SAFETY: never move directories (only files). Prevents accidental folder moves.
    struct stat st_src; if (stat(src_path, &st_src) == 0 && S_ISDIR(st_src.st_mode)) { errno = EISDIR; return -1; }
    // Ensure destination is a directory
    if (!is_directory(dst_dir)) { errno = ENOTDIR; return -1; }
    // Build destination path
    const char *base = strrchr(src_path, '/');
    base = base ? base + 1 : src_path;
    snprintf(dst_path, dst_sz, "%s/%s", dst_dir, base);
    // If source and destination are identical, do nothing
    if (strcmp(src_path, dst_path) == 0) return 0;
    if (rename(src_path, dst_path) != 0) {
        perror("[amiwb] rename (move) failed");
        return -1;
    }
    return 0;
}

// ========================
// Workbench API expected by other modules
// ========================

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
    int max_x = 0, max_y = 0;
    for (int i = 0; i < icon_count; i++) {
        if (icon_array[i]->display_window == canvas->win) {
            max_x = max(max_x, icon_array[i]->x + icon_array[i]->width);
            max_y = max(max_y, icon_array[i]->y + icon_array[i]->height + 20);
        }
    }
    canvas->content_width = max_x + 80;
    canvas->content_height = max_y + 10;
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
    int count = 0;
    for (int i = 0; i < icon_count; i++) if (icon_array[i]->display_window == canvas->win) count++;
    if (count == 0) { compute_content_bounds(canvas); compute_max_scroll(canvas); redraw_canvas(canvas); return; }
    FileIcon **list = malloc(count * sizeof(FileIcon *)); if (!list) return;
    int idx = 0; for (int i = 0; i < icon_count; i++) if (icon_array[i]->display_window == canvas->win) list[idx++] = icon_array[i];
    qsort(list, count, sizeof(FileIcon *), icon_cmp);
    int cell_h = ICON_SPACING; int label_space = 20; int min_cell_w = 80; char max_str[81]; memset(max_str, 'W', 80); max_str[80] = '\0';
    int max_allowed_w = get_text_width(max_str); int padding = 20;
    int visible_w = (canvas->type == WINDOW) ? (canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT) : canvas->width;
    int visible_h = (canvas->type == WINDOW) ? (canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM) : canvas->height;
    int start_x = 10; int start_y = (canvas->type == DESKTOP) ? 40 : 10;
    int num_rows = max(1, (visible_h - start_y - 0) / cell_h);
    int num_columns = (count + num_rows - 1) / num_rows;
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
        // Vertical list of names; directories first (A..Z), then files (A..Z)
        int y = 10;                // start below title area inside content
        int x = 12;                // slight left padding
        int row_h = 18 + 6;        // approx font height + padding
        int max_text_w = 0;

        // Collect icons for this window into two lists: dirs then files
        int dir_count = 0, file_count = 0;
        for (int i = 0; i < icon_count; i++) {
            FileIcon *ic = icon_array[i];
            if (!ic || ic->display_window != canvas->win) continue;
            if (ic->type == TYPE_DRAWER) dir_count++; else file_count++;
        }
        FileIcon **dirs = dir_count ? (FileIcon**)malloc(sizeof(FileIcon*) * dir_count) : NULL;
        FileIcon **files = file_count ? (FileIcon**)malloc(sizeof(FileIcon*) * file_count) : NULL;
        int di = 0, fi = 0;
        for (int i = 0; i < icon_count; i++) {
            FileIcon *ic = icon_array[i];
            if (!ic || ic->display_window != canvas->win) continue;
            if (ic->type == TYPE_DRAWER) dirs[di++] = ic; else files[fi++] = ic;
        }
        if (dir_count > 0) qsort(dirs, dir_count, sizeof(FileIcon*), label_cmp);
        if (file_count > 0) qsort(files, file_count, sizeof(FileIcon*), label_cmp);

        // Position rows: first dirs, then files
        for (int i = 0; i < dir_count; i++) {
            FileIcon *ic = dirs[i];
            ic->x = x; ic->y = y; y += row_h;
            int lw = get_text_width(ic->label ? ic->label : ""); if (lw > max_text_w) max_text_w = lw;
        }
        for (int i = 0; i < file_count; i++) {
            FileIcon *ic = files[i];
            ic->x = x; ic->y = y; y += row_h;
            int lw = get_text_width(ic->label ? ic->label : ""); if (lw > max_text_w) max_text_w = lw;
        }
        if (dirs) free(dirs); if (files) free(files);

        // Content width equals max label width + padding; clamp at least to visible width
        int padding = 10 + 6; // selection pad + left text offset
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
    if (canvas->type == DESKTOP) {
        create_icon("/usr/local/share/amiwb/icons/harddisk.info", canvas, 10, 40);
        FileIcon *system_icon = icon_array[icon_count - 1];
        system_icon->type = TYPE_DRAWER;
        if (system_icon->label) { free(system_icon->label); }
        system_icon->label = strdup("System");
        if (system_icon->path) { free(system_icon->path); }
        system_icon->path = strdup("/");

        create_icon("/usr/local/share/amiwb/icons/harddisk.info", canvas, 10, 120);
        FileIcon *home_icon = icon_array[icon_count - 1];
        home_icon->type = TYPE_DRAWER;
        if (home_icon->label) { free(home_icon->label); }
        home_icon->label = strdup("Home");
        if (home_icon->path) { free(home_icon->path); }
        home_icon->path = strdup(getenv("HOME"));
    }
    DIR *dirp = opendir(dir);
    if (dirp) {
        int x = 20, y = 20; int x_offset = 100;
        struct dirent *entry;
        while ((entry = readdir(dirp))) {
            // Skip current and parent directory entries always
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            // Skip hidden unless show_hidden is enabled
            if (entry->d_name[0] == '.' && !canvas->show_hidden) continue;
            char full_path[1024]; snprintf(full_path, sizeof(full_path), "%s/%s", dir, entry->d_name);
            if (ends_with(entry->d_name, ".info")) {
                char base[256]; size_t namelen = strlen(entry->d_name);
                size_t blen = namelen > 5 ? namelen - 5 : 0; strncpy(base, entry->d_name, blen); base[blen] = '\0';
                char base_path[1024]; snprintf(base_path, sizeof(base_path), "%s/%s", dir, base);
                struct stat st; if (stat(base_path, &st) != 0) {
                    // Orphan .info
                    create_icon(full_path, canvas, x, y);
                    FileIcon *new_icon = icon_array[icon_count - 1];
                    new_icon->type = TYPE_FILE;
                    x += x_offset; if (x + 64 > canvas->width) { x = 10; y += 80; }
                }
            } else {
                char info_name[256]; snprintf(info_name, sizeof(info_name), "%s.info", entry->d_name);
                char info_path[1024]; snprintf(info_path, sizeof(info_path), "%s/%s", dir, info_name);
                struct stat st_info; const char *icon_path = (stat(info_path, &st_info) == 0) ? info_path : full_path;
                create_icon(icon_path, canvas, x, y);
                FileIcon *new_icon = icon_array[icon_count - 1];
                if (new_icon->path) { free(new_icon->path); }
                new_icon->path = strdup(full_path);
                struct stat st; if (stat(full_path, &st) == 0) new_icon->type = S_ISDIR(st.st_mode) ? TYPE_DRAWER : TYPE_FILE;
                if (new_icon->label) free(new_icon->label);
                new_icon->label = strdup(entry->d_name);
                x += x_offset; if (x + 64 > canvas->width) { x = 10; y += 80; }
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
        set_active_window(existing);
        XRaiseWindow(get_display(), existing->win);
        redraw_canvas(existing);
        return;
    }
    // Otherwise create a new window
    Canvas *new_canvas = create_canvas(icon->path, 50, 50, 400, 300, WINDOW);
    if (new_canvas) {
        refresh_canvas_from_directory(new_canvas, icon->path);
        // Initial placement for new window must use icon_cleanup
        icon_cleanup(new_canvas);
        redraw_canvas(new_canvas);
        // Make the newly created window active (raise on top)
        set_active_window(new_canvas);
    }
}

// ========================
// Implementations for missing symbols (linker fixes)
// ========================

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
            // Row spans full content width; row height ~ font + padding
            int row_h = 18 + 6; // keep in sync with apply_view_layout/render
            int row_x = base_x; // full width inside content area
            int row_w = (c->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT);
            if (x >= row_x && x <= row_x + row_w && y >= ry && y <= ry + row_h) return ic;
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
    if (!icon || icon->type != TYPE_ICONIFIED) return;
    Canvas *canvas = icon->iconified_canvas;
    if (!canvas) return;

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

    // Remove the iconified desktop icon
    destroy_icon(icon);

    // Refresh desktop layout and visuals
    Canvas *desktop = get_desktop_canvas();
    if (desktop) {
        // Do not auto-reorganize; simply redraw
        compute_content_bounds(desktop);
        compute_max_scroll(desktop);
        redraw_canvas(desktop);
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
    // If Ctrl is not pressed, deselect all other icons on this canvas
    if (!(state & ControlMask)) {
        FileIcon **icons = get_icon_array(); int count = get_icon_count();
        for (int i = 0; i < count; i++) {
            if (icons[i] != icon && icons[i]->display_window == canvas->win && icons[i]->selected) {
                icons[i]->selected = false; icons[i]->current_picture = icons[i]->normal_picture;
            }
        }
    }
    icon->selected = !icon->selected; icon->current_picture = icon->selected ? icon->selected_picture : icon->normal_picture;
}

static void deselect_all_icons(Canvas *canvas) {
    FileIcon **icons = get_icon_array(); int count = get_icon_count();
    for (int i = 0; i < count; i++) {
        if (icons[i]->display_window == canvas->win && icons[i]->selected) { icons[i]->selected = false; icons[i]->current_picture = icons[i]->normal_picture; }
    }
}

void workbench_handle_button_press(XButtonEvent *event) {
    Canvas *canvas = find_canvas(event->window); if (!canvas) return;
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
    // Create desktop prime icons
    create_icon("/usr/local/share/amiwb/icons/harddisk.info", get_desktop_canvas(), 10, 40);
    FileIcon *system_icon = icon_array[icon_count - 1];
    system_icon->type = TYPE_DRAWER; if (system_icon->label) { free(system_icon->label); system_icon->label = strdup("System"); }
    if (system_icon->path) { free(system_icon->path); system_icon->path = strdup("/"); }
    create_icon("/usr/local/share/amiwb/icons/harddisk.info", get_desktop_canvas(), 10, 120);
    FileIcon *home_icon = icon_array[icon_count - 1];
    home_icon->type = TYPE_DRAWER; if (home_icon->label) { free(home_icon->label); home_icon->label = strdup("Home"); }
    if (home_icon->path) { free(home_icon->path); home_icon->path = strdup(getenv("HOME")); }
    icon_cleanup(get_desktop_canvas()); redraw_canvas(get_desktop_canvas());
}

void cleanup_workbench(void) {
    for (int i = icon_count - 1; i >= 0; i--) destroy_icon(icon_array[i]);
    free(icon_array); icon_array = NULL; icon_count = 0; icon_array_size = 0;
}