// File: wb_drag.c
// Drag and Drop - State machine, floating drag window, XDND integration

#define _POSIX_C_SOURCE 200809L
#include "wb_internal.h"
#include "wb_public.h"
#include "../config.h"
#include "../render.h"
#include "../xdnd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xft/Xft.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/Xrender.h>

// External dependencies
extern XdndContext xdnd_ctx;
extern void redraw_canvas(Canvas *canvas);
extern XftFont *get_font(void);
extern void compute_max_scroll(Canvas *canvas);
extern int get_text_width(const char *text);

// ============================================================================
// Drag State Variables
// ============================================================================

// Global drag icon (exposed for XDND)
FileIcon *dragged_icon = NULL;

// Drag state
static int drag_start_x, drag_start_y;                  // Click position in icon
static int drag_start_root_x, drag_start_root_y;        // Start position in root
static Canvas *drag_source_canvas = NULL;                // Source canvas
static bool dragging_floating = false;                   // Using floating window
static Window drag_win = None;                           // Drag window
static Picture target_picture = 0;                       // XRender picture
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
    compute_content_bounds(canvas);
    compute_max_scroll(canvas);
    redraw_canvas(canvas);
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

    XSetWindowAttributes attrs;
    attrs.colormap = XCreateColormap(dpy, root, vinfo.visual, AllocNone);
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

    // Make input-transparent using X Shape extension
    XShapeCombineRectangles(dpy, drag_win, ShapeInput, 0, 0, NULL, 0, ShapeSet, Unsorted);

    // Map window BEFORE creating XRender resources (prevents X server resource leaks)
    XMapWindow(dpy, drag_win);

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

    // Center icon in drag window
    int dx = (drag_win_w - dragged_icon->width) / 2;
    int dy = (drag_win_h - dragged_icon->height - 20) / 2;

    // Composite icon
    if (dragged_icon->current_picture) {
        XRenderComposite(dpy, PictOpOver,
                        dragged_icon->current_picture, None, target_picture,
                        0, 0, 0, 0, dx, dy,
                        dragged_icon->width, dragged_icon->height);
    }

    // Draw label
    if (dragged_icon->label) {
        XftDraw *xft = XftDrawCreate(dpy, drag_win,
                                     DefaultVisual(dpy, DefaultScreen(dpy)),
                                     DefaultColormap(dpy, DefaultScreen(dpy)));
        if (xft) {
            XftColor color;
            XRenderColor rc = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
            XftColorAllocValue(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                              DefaultColormap(dpy, DefaultScreen(dpy)), &rc, &color);

            XftFont *font = get_font();
            if (font) {
                const char *text = dragged_icon->label;
                int text_w = get_text_width(text);
                int tx = (drag_win_w - text_w) / 2;
                int ty = dy + dragged_icon->height + 15;

                XftDrawStringUtf8(xft, &color, font, tx, ty,
                                 (const FcChar8*)text, strlen(text));
            }

            XftColorFree(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                        DefaultColormap(dpy, DefaultScreen(dpy)), &color);
            XftDrawDestroy(xft);
        }
    }
}

static void update_drag_window_position(int root_x, int root_y) {
    if (drag_win == None) return;

    Display *dpy = itn_core_get_display();
    int x = root_x - drag_win_w / 2;
    int y = root_y - drag_win_h / 2;

    XMoveWindow(dpy, drag_win, x, y);
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
        if (dragged_icon && saved_source_window != None) {
            dragged_icon->display_window = None;
            if (drag_source_canvas) redraw_canvas(drag_source_canvas);
        }
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
    Display *dpy = itn_core_get_display();

    if (!dragged_icon || dragged_icon->type != TYPE_ICONIFIED) {
        return false;  // Not an iconified window
    }

    if (target && target->type == DESKTOP && drag_source_canvas &&
        drag_source_canvas->type == DESKTOP) {
        // Allowed: desktop to desktop
        if (drag_active) {
            int drag_icon_screen_x = saved_drag_win_x;
            int drag_icon_screen_y = saved_drag_win_y;

            int dx = (drag_win_w - dragged_icon->width) / 2;
            int dy = (drag_win_h - dragged_icon->height - 20) / 2;
            drag_icon_screen_x += dx;
            drag_icon_screen_y += dy;

            int tx = 0, ty = 0;
            Window dummy;
            safe_translate_coordinates(dpy, target->win, DefaultRootWindow(dpy),
                                     0, 0, &tx, &ty, &dummy);
            int local_x = drag_icon_screen_x - tx;
            int local_y = drag_icon_screen_y - ty;

            int place_x = max(0, local_x);
            int place_y = max(0, local_y);
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
        if (dragged_icon) {
            if (saved_source_window != None) dragged_icon->display_window = saved_source_window;
            move_icon(dragged_icon, drag_orig_x, drag_orig_y);
        }
        if (drag_source_canvas) refresh_canvas(drag_source_canvas);
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
    Display *dpy = itn_core_get_display();

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
            int drag_icon_screen_x = saved_drag_win_x;
            int drag_icon_screen_y = saved_drag_win_y;

            int dx = (drag_win_w - dragged_icon->width) / 2;
            int dy = (drag_win_h - dragged_icon->height - 20) / 2;
            drag_icon_screen_x += dx;
            drag_icon_screen_y += dy;

            int tx = 0, ty = 0;
            Window dummy;
            safe_translate_coordinates(dpy, target->win, DefaultRootWindow(dpy),
                                     0, 0, &tx, &ty, &dummy);
            int local_x = drag_icon_screen_x - tx;
            int local_y = drag_icon_screen_y - ty;

            int place_x = max(0, local_x);
            int place_y = max(0, local_y);
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
        if (dragged_icon) {
            if (saved_source_window != None) dragged_icon->display_window = saved_source_window;
            move_icon(dragged_icon, drag_orig_x, drag_orig_y);
        }
        if (drag_source_canvas) refresh_canvas(drag_source_canvas);
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
    Display *dpy = itn_core_get_display();

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
        const char *home = getenv("HOME");
        snprintf(dst_dir, sizeof(dst_dir), "%s/Desktop", home ? home : ".");
    } else {
        snprintf(dst_dir, sizeof(dst_dir), "%s", target->path ? target->path : ".");
    }

    // Safety check: prevent moving directory into itself
    if (dragged_icon->type == TYPE_DRAWER) {
        size_t src_len = strlen(dragged_icon->path);
        if (strncmp(dst_dir, dragged_icon->path, src_len) == 0 &&
            (dst_dir[src_len] == '/' || dst_dir[src_len] == '\0')) {
            log_error("[WARNING] Cannot move directory into itself");
            if (saved_source_window != None) dragged_icon->display_window = saved_source_window;
            move_icon(dragged_icon, drag_orig_x, drag_orig_y);
            if (drag_source_canvas) refresh_canvas(drag_source_canvas);
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

    // Calculate icon position
    int drag_icon_screen_x = saved_drag_win_x;
    int drag_icon_screen_y = saved_drag_win_y;

    int dx = (drag_win_w - dragged_icon->width) / 2;
    int dy = (drag_win_h - dragged_icon->height - 20) / 2;
    drag_icon_screen_x += dx;
    drag_icon_screen_y += dy;

    int tx = 0, ty = 0;
    Window dummy;
    safe_translate_coordinates(dpy, target->win, DefaultRootWindow(dpy),
                              0, 0, &tx, &ty, &dummy);
    int local_x = drag_icon_screen_x - tx;
    int local_y = drag_icon_screen_y - ty;

    if (target->type == WINDOW) {
        local_x = max(0, local_x - BORDER_WIDTH_LEFT + target->scroll_x);
        local_y = max(0, local_y - BORDER_HEIGHT_TOP + target->scroll_y);
    }
    int place_x = max(0, local_x);
    int place_y = max(0, local_y);

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

        // Synchronous move - create icon
        const char *base = strrchr(dst_path, '/');
        const char *name_only = base ? base + 1 : dst_path;

        struct stat dst_stat;
        int file_type = TYPE_FILE;
        if (stat(dst_path, &dst_stat) == 0) {
            file_type = S_ISDIR(dst_stat.st_mode) ? TYPE_DRAWER : TYPE_FILE;
        }

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

        create_icon_with_metadata(img_path, target, place_x, place_y,
                                  dst_path, name_only, file_type);

        // Remove desktop icon if moved from Desktop
        const char *home = getenv("HOME");
        char desktop_dir[PATH_SIZE] = {0};
        if (home) snprintf(desktop_dir, sizeof(desktop_dir), "%s/Desktop/", home);
        if (home && strncmp(src_path_abs, desktop_dir, strlen(desktop_dir)) == 0) {
            Canvas *desktop = itn_canvas_get_desktop();
            if (desktop) {
                remove_icon_by_path_on_canvas(src_path_abs, desktop);
                refresh_canvas(desktop);
            }
        }

        // Apply layout
        if (target->type == WINDOW && target->view_mode == VIEW_NAMES) {
            apply_view_layout(target);
        } else if (target->type == WINDOW && target->view_mode == VIEW_ICONS) {
            compute_content_bounds(target);
        }
        compute_max_scroll(target);

        if (drag_source_canvas) {
            refresh_canvas(drag_source_canvas);
        }
        redraw_canvas(target);
    } else {
        // Move failed
        if (dragged_icon) {
            if (saved_source_window != None) dragged_icon->display_window = saved_source_window;
            move_icon(dragged_icon, drag_orig_x, drag_orig_y);
        }
        if (drag_source_canvas) refresh_canvas(drag_source_canvas);
    }
}

// Helper: Perform same-canvas drop or handle invalid drop
static void perform_same_canvas_drop(Canvas *target) {
    Display *dpy = itn_core_get_display();

    if (!dragged_icon) {
        return;
    }

    if (!drag_active) {
        // No drag occurred
    } else if (target == drag_source_canvas) {
        // Same-canvas drag: reposition icon
        int drag_icon_screen_x = saved_drag_win_x;
        int drag_icon_screen_y = saved_drag_win_y;

        int dx = (drag_win_w - dragged_icon->width) / 2;
        int dy = (drag_win_h - dragged_icon->height - 20) / 2;
        drag_icon_screen_x += dx;
        drag_icon_screen_y += dy;

        int tx = 0, ty = 0;
        Window dummy;
        safe_translate_coordinates(dpy, drag_source_canvas->win, DefaultRootWindow(dpy),
                                 0, 0, &tx, &ty, &dummy);
        int local_x = drag_icon_screen_x - tx;
        int local_y = drag_icon_screen_y - ty;

        if (drag_source_canvas->type == WINDOW) {
            local_x = max(0, local_x - BORDER_WIDTH_LEFT + drag_source_canvas->scroll_x);
            local_y = max(0, local_y - BORDER_HEIGHT_TOP + drag_source_canvas->scroll_y);
        }
        int place_x = max(0, local_x);
        int place_y = max(0, local_y);
        if (saved_source_window != None) dragged_icon->display_window = saved_source_window;
        move_icon(dragged_icon, place_x, place_y);
    } else {
        // Invalid target: restore original position
        if (saved_source_window != None) dragged_icon->display_window = saved_source_window;
        move_icon(dragged_icon, drag_orig_x, drag_orig_y);
    }

    if (drag_source_canvas) {
        compute_content_bounds(drag_source_canvas);
        compute_max_scroll(drag_source_canvas);
        redraw_canvas(drag_source_canvas);
    }
}

void end_drag_icon(Canvas *canvas) {
    Display *dpy = itn_core_get_display();

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
        return;
    }

    // Handle XDND drop
    if (handle_xdnd_drop(canvas)) {
        return;
    }

    // Get target canvas
    Canvas *target = canvas_under_pointer();

    // Handle special icon types
    if (handle_iconified_window_drop(target)) {
        return;
    }

    if (handle_prime_icon_drop(target)) {
        return;
    }

    // Handle cross-canvas or same-canvas drops
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
}

void wb_drag_cleanup_window(void) {
    destroy_drag_window();
}

// ============================================================================
// Public API
// ============================================================================

void workbench_cleanup_drag_state(void) {
    destroy_drag_window();

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
