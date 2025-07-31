// File: intuition.c
#include "intuition.h"
#include "render.h"
#include "menus.h"
#include "config.h"
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <X11/cursorfont.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define INITIAL_CANVAS_CAPACITY 8

// Global state for intuition
static Display *display = NULL;
static RenderContext *render_context = NULL;
static Canvas **canvas_array = NULL;
static int canvas_count = 0;
static int canvas_array_size = 0;
static Bool fullscreen_active = False;
static Canvas *active_window = NULL;
static Cursor root_cursor;
static int width = 0;
static int height = 0;
static Window screen = 0;
static Window root = 0;
static int depth = 0;
static Visual *visual = NULL;
static Canvas *dragging_canvas = NULL;
static int drag_start_x = 0, drag_start_y = 0;
static int window_start_x = 0, window_start_y = 0;

// resizing
static Canvas *resizing_canvas = NULL;
static int resize_start_x = 0, resize_start_y = 0;
static int window_start_width = 0, window_start_height = 0;

// scroling
static Canvas *scrolling_canvas = NULL;  // Canvas being scrolled
static bool scrolling_vertical = true;   // True for vertical scrollbar, false for horizontal
static int initial_scroll = 0;           // Initial scroll position at drag start
static int scroll_start_pos = 0;         // Initial mouse position (x or y root) at drag start

// Custom X error handler to log details about errors (e.g., BadValue) and make them non-fatal
static int x_error_handler(Display *dpy, XErrorEvent *error) {
    char error_text[1024];
    XGetErrorText(dpy, error->error_code, error_text, sizeof(error_text));
    fprintf(stderr, "X Error intercepted: error_code=%d (%s), request_code=%d, minor_code=%d, resource_id=0x%lx\n",
            error->error_code, error_text, error->request_code, error->minor_code, error->resourceid);
    fflush(stderr);
    return 0;  // continue execution (non-fatal handling)
}

Display *get_display(void) { return display; }

RenderContext *get_render_context(void) { return render_context; }

static Canvas *add_canvas(void) {
    if (canvas_count >= canvas_array_size) {
        canvas_array_size = canvas_array_size ? canvas_array_size * 2 : INITIAL_CANVAS_CAPACITY;
        Canvas **new_canvases = realloc(canvas_array, canvas_array_size * sizeof(Canvas *));
        if (!new_canvases) return NULL;
        canvas_array = new_canvases;
    }
    Canvas *new_canvas = malloc(sizeof(Canvas));
    if (!new_canvas) return NULL;
    canvas_array[canvas_count++] = new_canvas;
    return new_canvas;
}

static void remove_canvas(Canvas *canvas_to_remove) {
    for (int i = 0; i < canvas_count; i++) {
        if (canvas_array[i] == canvas_to_remove) {
            memmove(&canvas_array[i], &canvas_array[i + 1], (canvas_count - i - 1) * sizeof(Canvas *));
            canvas_count--;
            break;
        }
    }
}

Canvas *manage_canvases(bool add, Canvas *canvas_to_remove) {
    if (add) return add_canvas();
    if (canvas_to_remove) remove_canvas(canvas_to_remove);
    return NULL;
}

static XVisualInfo select_visual(CanvasType type) {
    XVisualInfo vinfo;
    if (type == DESKTOP || type == MENU) {
        vinfo.visual = DefaultVisual(display, screen);
        vinfo.depth = DefaultDepth(display, screen);
    } else {
        if (!XMatchVisualInfo(display, screen, GLOBAL_DEPTH, TrueColor, &vinfo)) {
            vinfo.visual = DefaultVisual(display, screen);
            vinfo.depth = DefaultDepth(display, screen);
        }
    }
    XMatchVisualInfo(display, screen, vinfo.depth, TrueColor, &vinfo);
    return vinfo;
}

static bool init_display_and_root(void) {
    display = XOpenDisplay(NULL);
    if (!display) return false;
    XSetErrorHandler(x_error_handler);
    XSync(display, False);

    screen = DefaultScreen(display);
    width = DisplayWidth(display, screen);
    height = DisplayHeight(display, screen);
    root = RootWindow(display, screen);
    depth = 32;

    root_cursor = XCreateFontCursor(display, XC_left_ptr);
    XDefineCursor(display, root, root_cursor);

    XSelectInput(display, root, SubstructureRedirectMask | SubstructureNotifyMask | PropertyChangeMask |
                                StructureNotifyMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | KeyPressMask);
    XSync(display, False);
    return true;
}

static bool init_render_context(void) {
    render_context = malloc(sizeof(RenderContext));
    if (!render_context) return false;
    render_context->dpy = display;

    XVisualInfo vinfo;
    XMatchVisualInfo(display, screen, depth, TrueColor, &vinfo);
    render_context->fmt = XRenderFindVisualFormat(display, vinfo.visual);
    render_context->bg_pixmap = None;
    return true;
}

Canvas *init_intuition(void) {
    if (!init_display_and_root() || !init_render_context()) return NULL;

    Canvas *desktop = create_canvas(getenv("HOME"), 0, 20, width, height, DESKTOP);
    if (!desktop) return NULL;

    Window root_return, parent_return;
    Window *children;
    unsigned int nchildren;
    if (XQueryTree(display, root, &root_return, &parent_return, &children, &nchildren)) {
        for (unsigned int i = 0; i < nchildren; i++) {
            Window child = children[i];
            bool is_own = false;
            for (int j = 0; j < canvas_count; j++) {
                if (canvas_array[j]->win == child || canvas_array[j]->client_win == child) {
                    is_own = true;
                    break;
                } else canvas_array[j]->client_win = None;
            }
            if (is_own) continue;

            XWindowAttributes attrs;
            if (!XGetWindowAttributes(display, child, &attrs) || attrs.override_redirect || attrs.map_state != IsViewable || attrs.class == InputOnly) continue;

            Atom motif_wm_hints = XInternAtom(display, "_MOTIF_WM_HINTS", True);
            Atom actual_type;
            int actual_format;
            unsigned long nitems, bytes_after;
            unsigned char *prop = NULL;
            bool skip_framing = false;
            if (motif_wm_hints != None && XGetWindowProperty(display, child, motif_wm_hints, 0, 5, False, AnyPropertyType,
                                                             &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success && nitems >= 3) {
                long *hints = (long *)prop;
                if (hints[0] & (1L << 2) && hints[2] == 0) skip_framing = true;
                XFree(prop);
            }
            if (skip_framing) continue;

            int frame_x = attrs.x;
            //, frame_y = attrs.y;
            // Clamp y to ensure titlebar is below menubar
            int frame_y = max(attrs.y, MENUBAR_HEIGHT);  
            int frame_width = attrs.width + BORDER_WIDTH_LEFT + BORDER_WIDTH_RIGHT;
            int frame_height = attrs.height + BORDER_HEIGHT_TOP + BORDER_HEIGHT_BOTTOM;
            Canvas *frame = create_canvas(NULL, frame_x, frame_y, frame_width, frame_height, WINDOW);
            if (!frame) continue;

            XReparentWindow(display, child, frame->win, BORDER_WIDTH_LEFT, BORDER_HEIGHT_TOP);
            XSelectInput(display, child, StructureNotifyMask | PropertyChangeMask);
            XResizeWindow(display, child, attrs.width, attrs.height);
            frame->client_win = child;
            XAddToSaveSet(display, child);
            XRaiseWindow(display, frame->win);
            redraw_canvas(frame);
        }
        if (children) XFree(children);
    }
    XSync(display, False);
    redraw_canvas(desktop);
    return desktop;
}

Canvas *get_desktop_canvas(void) { return canvas_count > 0 ? canvas_array[0] : NULL; }

Canvas *find_canvas(Window win) {
    for (int i = 0; i < canvas_count; i++) if (canvas_array[i]->win == win) return canvas_array[i];
    return NULL;
}

static void deactivate_all_windows(Canvas *except) {
    for (int i = 0; i < canvas_count; i++) {
        Canvas *c = canvas_array[i];
        if (c->type == WINDOW && c != except) {
            c->active = false;
            redraw_canvas(c);
        }
    }
}

static void deselect_all_icons(void) {
    FileIcon **icon_array = get_icon_array();
    int icon_count = get_icon_count();
    for (int i = 0; i < icon_count; i++) {
        if (icon_array[i]->selected) {
            icon_array[i]->selected = false;
            icon_array[i]->current_picture = icon_array[i]->normal_picture;
        }
    }
}

void set_active_window(Canvas *canvas) {
    if (!canvas || canvas->type != WINDOW || active_window == canvas) return;
    deactivate_all_windows(canvas);
    Canvas *prev_active = active_window;
    active_window = canvas;
    deselect_all_icons();
    if (prev_active) redraw_canvas(prev_active);
    canvas->active = true;
    XRaiseWindow(display, canvas->win);
    redraw_canvas(canvas);
    XSync(display, False);
}

Canvas *get_active_window(void) { return active_window; }

// compute max scroll and clamp current scroll
void compute_max_scroll(Canvas *canvas) {
    if (!canvas) return;
    int visible_w = canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT;
    int visible_h = canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM;
    canvas->max_scroll_x = max(0, canvas->content_width - visible_w);
    canvas->max_scroll_y = max(0, canvas->content_height - visible_h);
    canvas->scroll_x = min(canvas->scroll_x, canvas->max_scroll_x);
    canvas->scroll_y = min(canvas->scroll_y, canvas->max_scroll_y);
}

Canvas *create_canvas(const char *path, int x, int y, int width, int height, CanvasType type) {
    RenderContext *ctx = get_render_context();
    if (!ctx) return NULL;

    Canvas *canvas = manage_canvases(true, NULL);
    if (!canvas) return NULL;

    *canvas = (Canvas){0};
    canvas->type = type;
    canvas->path = path ? strdup(path) : NULL;
    canvas->title = path ? strdup(strrchr(path, '/') ? strrchr(path, '/') + 1 : path) : NULL;
    canvas->x = x;
    // Clamp y to ensure titlebar is below menubar
    canvas->y = (type == WINDOW) ? max(y, MENUBAR_HEIGHT) : y; 
    canvas->width = width;
    canvas->height = height;
    canvas->bg_color = GRAY;
    canvas->active = false;

    XVisualInfo vinfo = select_visual(type);
    canvas->visual = vinfo.visual;
    canvas->depth = vinfo.depth;

    XSetWindowAttributes attrs = {0};
    attrs.colormap = XCreateColormap(ctx->dpy, root, canvas->visual, AllocNone);
    attrs.border_pixel = 0;
    unsigned long valuemask = CWColormap | CWBorderPixel;
    canvas->win = (type == DESKTOP) ? DefaultRootWindow(ctx->dpy) :
                  XCreateWindow(display, root, x, y, width, height, 1, vinfo.depth, InputOutput, canvas->visual, valuemask, &attrs);
    if (!canvas->win) {
        destroy_canvas(canvas);
        return NULL;
    }
    canvas->colormap = attrs.colormap;

    long event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | KeyPressMask;
    if (type == DESKTOP) event_mask |= SubstructureRedirectMask | SubstructureNotifyMask;
    if (type == WINDOW) event_mask |= StructureNotifyMask;
    XSelectInput(ctx->dpy, canvas->win, event_mask);

    canvas->canvas_buffer = XCreatePixmap(ctx->dpy, canvas->win, width, height, vinfo.depth);
    if (!canvas->canvas_buffer) {
        destroy_canvas(canvas);
        return NULL;
    }

    XRenderPictFormat *fmt = XRenderFindVisualFormat(ctx->dpy, canvas->visual);
    if (!fmt) {
        destroy_canvas(canvas);
        return NULL;
    }

    canvas->canvas_render = XRenderCreatePicture(ctx->dpy, canvas->canvas_buffer, fmt, 0, NULL);
    if (!canvas->canvas_render) {
        destroy_canvas(canvas);
        return NULL;
    }
    canvas->window_render = XRenderCreatePicture(ctx->dpy, canvas->win, fmt, 0, NULL);
    if (!canvas->window_render) {
        destroy_canvas(canvas);
        return NULL;
    }

    if (type == WINDOW) {
        canvas->scroll_x = 0;
        canvas->scroll_y = 0;
        canvas->content_width = width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT;
        canvas->content_height = height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM;
        compute_max_scroll(canvas);
    }

    if (type != DESKTOP) {
        if (type == WINDOW) {
            attrs.background_pixmap = None;
            XChangeWindowAttributes(ctx->dpy, canvas->win, CWBackPixmap, &attrs);
            canvas->active = true;
            set_active_window(canvas);
        }
        XMapRaised(ctx->dpy, canvas->win);
        if (type == WINDOW) redraw_canvas(canvas);
        XSync(ctx->dpy, False);
    }
    return canvas;
}

void intuition_handle_expose(XExposeEvent *event) {
    Canvas *canvas = find_canvas(event->window);
    if (canvas && !fullscreen_active) redraw_canvas(canvas);
}

static bool is_fullscreen_active(Window win) {
    Atom wm_state = XInternAtom(display, "_NET_WM_STATE", False);
    Atom fullscreen = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
    Atom type;
    int format;
    unsigned long nitems, bytes_after;
    unsigned char *prop;
    if (XGetWindowProperty(display, win, wm_state, 0, 1024, False, AnyPropertyType, &type, &format, &nitems, &bytes_after, &prop) != Success || !prop) return false;
    bool active = false;
    for (unsigned long i = 0; i < nitems; i++) if (((Atom *)prop)[i] == fullscreen) { active = true; break; }
    XFree(prop);
    return active;
}

void intuition_handle_property_notify(XPropertyEvent *event) {
    if (event->atom != XInternAtom(display, "WM_STATE", False)) return;

    Canvas *canvas = find_canvas(event->window);
    if (canvas && canvas->type == WINDOW) fullscreen_active = is_fullscreen_active(event->window);

    Canvas *menubar = get_menubar();
    if (menubar) {
        if (fullscreen_active) XUnmapWindow(display, menubar->win);
        else { XMapWindow(display, menubar->win); redraw_canvas(menubar); }
        menubar->width = DisplayWidth(display, DefaultScreen(display));
        XResizeWindow(display, menubar->win, menubar->width, MENU_ITEM_HEIGHT);
        redraw_canvas(menubar);
    }
}

// intuition_handle_button_press
void intuition_handle_button_press(XButtonEvent *event) {
    Canvas *canvas = find_canvas(event->window);
    if (!canvas) return;

    if (canvas->type != MENU && (event->button == Button1 || event->button == Button3)) {
        if (get_show_menus_state()) {
            toggle_menubar_state();  // Revert to default state on outside click
            redraw_canvas(get_desktop_canvas());  // Redraw desktop if needed
            return;
        }
    }

    if (canvas->type == DESKTOP) {
        if (event->button == Button3) toggle_menubar_state();
        redraw_canvas(canvas);
        return;
    }

    if (canvas->type != WINDOW) return;
    set_active_window(canvas);

    if (event->y < BORDER_HEIGHT_TOP && event->x < BUTTON_CLOSE_SIZE) { destroy_canvas(canvas); return; }
    if (event->y < BORDER_HEIGHT_TOP && event->x >= BUTTON_CLOSE_SIZE) {
        dragging_canvas = canvas;
        drag_start_x = event->x_root; drag_start_y = event->y_root;
        window_start_x = canvas->x; window_start_y = canvas->y;
        return;
    }
    if (event->x >= canvas->width - BORDER_WIDTH_RIGHT && event->y >= canvas->height - BORDER_HEIGHT_BOTTOM) {
        resizing_canvas = canvas;
        resize_start_x = event->x_root; resize_start_y = event->y_root;
        window_start_width = canvas->width; window_start_height = canvas->height;
        return;
    }

    if (canvas->client_win != None) return;

    int sb_x = canvas->width - BORDER_WIDTH_RIGHT;
    int sb_y = BORDER_HEIGHT_TOP + 10;
    int sb_w = BORDER_WIDTH_RIGHT;
    int sb_h = (canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM) - 54 - 10;
    if (event->x >= sb_x && event->x < sb_x + sb_w && event->y >= sb_y && event->y < sb_y + sb_h) {
        float ratio = (float)sb_h / canvas->content_height;
        int knob_h = max(MIN_KNOB_SIZE, (int)(ratio * sb_h));
        float pos_ratio = (float)canvas->scroll_y / canvas->max_scroll_y;
        int knob_y = sb_y + (int)(pos_ratio * (sb_h - knob_h));
        if (event->y >= knob_y && event->y < knob_y + knob_h) {
            scrolling_canvas = canvas; scrolling_vertical = true;
            initial_scroll = canvas->scroll_y; scroll_start_pos = event->y_root;
            return;
        } else {
            float click_ratio = (float)(event->y - sb_y) / sb_h;
            canvas->scroll_y = max(0, min((int)(click_ratio * canvas->max_scroll_y), canvas->max_scroll_y));
            redraw_canvas(canvas);
            return;
        }
    }

    int arrow_size = 20;
    if (event->x >= sb_x && event->x < sb_x + sb_w) {
        if (event->y >= (canvas->height - BORDER_HEIGHT_BOTTOM - (2 * arrow_size)) && event->y < (canvas->height - BORDER_HEIGHT_BOTTOM - arrow_size)) {
            canvas->scroll_y = max(0, canvas->scroll_y - 20); redraw_canvas(canvas); return;
        } else if (event->y >= (canvas->height - BORDER_HEIGHT_BOTTOM - arrow_size) && event->y < (canvas->height - BORDER_HEIGHT_BOTTOM)) {
            canvas->scroll_y = min(canvas->max_scroll_y, canvas->scroll_y + 20); redraw_canvas(canvas); return;
        }
    }

    int hb_x = BORDER_WIDTH_LEFT + 10;
    int hb_y = canvas->height - BORDER_HEIGHT_BOTTOM;
    int hb_w = (canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT) - 54 - 10;
    int hb_h = BORDER_HEIGHT_BOTTOM;
    if (event->x >= hb_x && event->x < hb_x + hb_w && event->y >= hb_y && event->y < hb_y + hb_h) {
        float ratio = (float)hb_w / canvas->content_width;
        int knob_w = max(MIN_KNOB_SIZE, (int)(ratio * hb_w));
        float pos_ratio = (float)canvas->scroll_x / canvas->max_scroll_x;
        int knob_x = hb_x + (int)(pos_ratio * (hb_w - knob_w));
        if (event->x >= knob_x && event->x < knob_x + knob_w) {
            scrolling_canvas = canvas; scrolling_vertical = false;
            initial_scroll = canvas->scroll_x; scroll_start_pos = event->x_root;
            return;
        } else {
            float click_ratio = (float)(event->x - hb_x) / hb_w;
            canvas->scroll_x = max(0, min((int)(click_ratio * canvas->max_scroll_x), canvas->max_scroll_x));
            redraw_canvas(canvas);
            return;
        }
    }

    if (event->y >= hb_y && event->y < hb_y + hb_h) {
        if (event->x >= (canvas->width - BORDER_WIDTH_RIGHT - (2 * arrow_size)) && event->x < (canvas->width - BORDER_WIDTH_RIGHT - arrow_size)) {
            canvas->scroll_x = max(0, canvas->scroll_x - 20); redraw_canvas(canvas); return;
        } else if (event->x >= (canvas->width - BORDER_WIDTH_RIGHT - arrow_size) && event->x < (canvas->width - BORDER_WIDTH_RIGHT)) {
            canvas->scroll_x = min(canvas->max_scroll_x, canvas->scroll_x + 20); redraw_canvas(canvas); return;
        }
    }
}

void intuition_handle_motion_notify(XMotionEvent *event) {
    if (dragging_canvas) {
        int delta_x = event->x_root - drag_start_x;
        int delta_y = event->y_root - drag_start_y;
        window_start_x += delta_x; 
        //window_start_y += delta_y;
        // Clamp y to ensure titlebar is below menubar
        window_start_y = max(window_start_y + delta_y, MENUBAR_HEIGHT);  
        XMoveWindow(display, dragging_canvas->win, window_start_x, window_start_y);
        dragging_canvas->x = window_start_x; dragging_canvas->y = window_start_y;
        redraw_canvas(dragging_canvas);
        XSync(display, False);
        drag_start_x = event->x_root; drag_start_y = event->y_root;
        return;
    }
    if (resizing_canvas) {
        int delta_x = event->x_root - resize_start_x;
        int delta_y = event->y_root - resize_start_y;
        int new_width = max(150, window_start_width + delta_x);
        int new_height = max(150, window_start_height + delta_y);
        XResizeWindow(display, resizing_canvas->win, new_width, new_height);
        XSync(display, False);
        return;
    }
    if (scrolling_canvas) {
        int delta, scale, new_scroll;
        if (scrolling_vertical) {
            delta = event->y_root - scroll_start_pos;
            int sb_h = scrolling_canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM;
            float ratio = (float)sb_h / scrolling_canvas->content_height;
            int knob_h = max(MIN_KNOB_SIZE, (int)(ratio * sb_h));
            scale = (sb_h - knob_h > 0) ? (scrolling_canvas->max_scroll_y / (sb_h - knob_h)) : 0;
            new_scroll = max(0, min(initial_scroll + delta * scale, scrolling_canvas->max_scroll_y));
            scrolling_canvas->scroll_y = new_scroll;
        } else {
            delta = event->x_root - scroll_start_pos;
            int sb_w = scrolling_canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT;
            float ratio = (float)sb_w / scrolling_canvas->content_width;
            int knob_w = max(MIN_KNOB_SIZE, (int)(ratio * sb_w));
            scale = (sb_w - knob_w > 0) ? (scrolling_canvas->max_scroll_x / (sb_w - knob_w)) : 0;
            new_scroll = max(0, min(initial_scroll + delta * scale, scrolling_canvas->max_scroll_x));
            scrolling_canvas->scroll_x = new_scroll;
        }
        redraw_canvas(scrolling_canvas);
        XSync(display, False);
        return;
    }
}

Canvas *find_canvas_by_client(Window client_win) {
    for (int i = 0; i < canvas_count; i++) if (canvas_array[i]->client_win == client_win) return canvas_array[i];
    return NULL;
}

void intuition_handle_destroy_notify(XDestroyWindowEvent *event) {
    Canvas *canvas = find_canvas_by_client(event->window);
    if (!canvas) return;

    Display *dpy = get_display();
    if (dpy && canvas->win != None) XUnmapWindow(dpy, canvas->win);

    if (canvas->window_render != None) XRenderFreePicture(dpy, canvas->window_render);
    if (canvas->canvas_render != None) XRenderFreePicture(dpy, canvas->canvas_render);
    if (canvas->canvas_buffer != None) XFreePixmap(dpy, canvas->canvas_buffer);
    if (canvas->colormap != None && canvas->type != DESKTOP) XFreeColormap(dpy, canvas->colormap);
    if (canvas->win != None && canvas->type != DESKTOP) XDestroyWindow(dpy, canvas->win);

    free(canvas->path); free(canvas->title);
    if (active_window == canvas) active_window = NULL;
    manage_canvases(false, canvas);
    free(canvas);

    Canvas *desktop = get_desktop_canvas();
    if (desktop) redraw_canvas(desktop);
    XSync(dpy, False);
}

void intuition_handle_button_release(XButtonEvent *event) {
    dragging_canvas = NULL;
    resizing_canvas = NULL;
    scrolling_canvas = NULL;
    XUngrabPointer(display, CurrentTime);
}

static bool should_skip_framing(Window win, XWindowAttributes *attrs) {
    if (attrs->override_redirect || attrs->class == InputOnly) return true;

    Atom motif_wm_hints = XInternAtom(display, "_MOTIF_WM_HINTS", True);
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;
    bool skip = false;
    if (motif_wm_hints != None && XGetWindowProperty(display, win, motif_wm_hints, 0, 5, False, AnyPropertyType,
                                                     &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success && nitems >= 3) {
        long *hints = (long *)prop;
        if (hints[0] & (1L << 2) && hints[2] == 0) skip = true;
        XFree(prop);
    }
    return skip;
}

void intuition_handle_map_request(XMapRequestEvent *event) {
    XWindowAttributes attrs;
    bool attrs_valid = XGetWindowAttributes(display, event->window, &attrs);
    if (!attrs_valid) {
        attrs.x = 100; attrs.y = 100; attrs.width = 400; attrs.height = 300;
        attrs.override_redirect = False; attrs.class = InputOutput; attrs.border_width = 0;
    }

    if (!attrs_valid || should_skip_framing(event->window, &attrs)) {
        XMapWindow(display, event->window);
        XSync(display, False);
        return;
    }

    int frame_x = attrs.x;
    // Clamp y to ensure titlebar is below menubar
    int frame_y = max(attrs.y, MENUBAR_HEIGHT);  
    int frame_width = attrs.width + BORDER_WIDTH_LEFT + BORDER_WIDTH_RIGHT;
    int frame_height = attrs.height + BORDER_HEIGHT_TOP + BORDER_HEIGHT_BOTTOM;
    Canvas *frame = create_canvas(NULL, frame_x, frame_y, frame_width, frame_height, WINDOW);
    if (!frame) {
        XMapWindow(display, event->window);
        return;
    }

    XReparentWindow(display, event->window, frame->win, BORDER_WIDTH_LEFT, BORDER_HEIGHT_TOP);
    XSelectInput(display, event->window, StructureNotifyMask | PropertyChangeMask);
    if (attrs.border_width != 0) {
        XWindowChanges bw_changes = {.border_width = 0};
        XConfigureWindow(display, event->window, CWBorderWidth, &bw_changes);
    }
    XMapWindow(display, event->window);
    frame->client_win = event->window;
    XAddToSaveSet(display, event->window);
    XRaiseWindow(display, frame->win);
    redraw_canvas(frame);
    XSync(display, False);
}

void intuition_handle_configure_request(XConfigureRequestEvent *event) {
    Canvas *canvas = find_canvas_by_client(event->window);
    if (!canvas) {
        XWindowAttributes attrs;
        bool attrs_valid = XGetWindowAttributes(display, event->window, &attrs);
        if (!attrs_valid) {
            attrs.class = InputOutput;
        }

        unsigned long safe_mask = event->value_mask & ~(CWStackMode | CWSibling);
        if (attrs.class == InputOnly) safe_mask &= ~CWBorderWidth;

        if (!attrs_valid) safe_mask &= ~CWBorderWidth;

        XWindowChanges changes = {0};
        if (safe_mask & CWX) changes.x = event->x;
        //if (safe_mask & CWY) changes.y = event->y;
        // Clamp y to ensure titlebar is below menubar
        if (safe_mask & CWY) changes.y = max(event->y, MENUBAR_HEIGHT);  
        if (safe_mask & CWWidth) changes.width = max(1, event->width);
        if (safe_mask & CWHeight) changes.height = max(1, event->height);

        if (attrs.class == InputOutput && (safe_mask & CWBorderWidth)) {
            bool need_set_border = false;
            if (event->value_mask & CWBorderWidth && event->border_width != 0) need_set_border = true;
            if (attrs_valid && attrs.border_width != 0) need_set_border = true;
            if (need_set_border) {
                changes.border_width = 0;
                safe_mask |= CWBorderWidth;
            }
        }

        if (safe_mask) XConfigureWindow(display, event->window, safe_mask, &changes);
        XSync(display, False);
        return;
    }

    XWindowChanges frame_changes = {0};
    unsigned long frame_mask = 0;
    int new_frame_width = canvas->width, new_frame_height = canvas->height;

    if (event->value_mask & CWWidth) {
        frame_changes.width = max(1, event->width) + BORDER_WIDTH_LEFT + BORDER_WIDTH_RIGHT;
        new_frame_width = frame_changes.width;
        frame_mask |= CWWidth;
    }

    if (event->value_mask & CWHeight) {
        frame_changes.height = max(1, event->height) + BORDER_HEIGHT_TOP + BORDER_HEIGHT_BOTTOM;
        new_frame_height = frame_changes.height;
        frame_mask |= CWHeight;
    }

    if (event->value_mask & CWX) {
        frame_changes.x = event->x;
        frame_mask |= CWX;
    }

    if (event->value_mask & CWY) {
        // Clamp y to ensure titlebar is below menubar
        frame_changes.y = max(event->y, MENUBAR_HEIGHT);  
        frame_mask |= CWY;
    }

    if ((event->value_mask & (CWStackMode | CWSibling)) == (CWStackMode | CWSibling) && event->detail >= 0 && event->detail <= 4) {
        XWindowAttributes sibling_attrs;
        if (XGetWindowAttributes(display, event->above, &sibling_attrs) && sibling_attrs.map_state == IsViewable) {
            frame_changes.stack_mode = event->detail;
            frame_changes.sibling = event->above;
            frame_mask |= CWStackMode | CWSibling;
        }
    }

    if (frame_mask) XConfigureWindow(display, canvas->win, frame_mask, &frame_changes);

    XWindowChanges client_changes = {0};
    unsigned long client_mask = 0;
    if (event->value_mask & CWWidth) { client_changes.width = max(1, event->width); client_mask |= CWWidth; }
    if (event->value_mask & CWHeight) { client_changes.height = max(1, event->height); client_mask |= CWHeight; }
    if (event->value_mask & CWBorderWidth) { client_changes.border_width = 0; client_mask |= CWBorderWidth; }
    client_changes.x = BORDER_WIDTH_LEFT; client_changes.y = BORDER_HEIGHT_TOP;
    client_mask |= CWX | CWY;
    if (client_mask) XConfigureWindow(display, event->window, client_mask, &client_changes);

    canvas->width = new_frame_width; canvas->height = new_frame_height;
    redraw_canvas(canvas);
    XSync(display, False);
}

void intuition_handle_configure_notify(XConfigureEvent *event) {
    Canvas *canvas = find_canvas(event->window);
    if (!canvas || (canvas->width == event->width && canvas->height == event->height)) return;

    Display *dpy = get_display();
    if (canvas->canvas_buffer != None) XFreePixmap(dpy, canvas->canvas_buffer);
    if (canvas->canvas_render != None) XRenderFreePicture(dpy, canvas->canvas_render);
    if (canvas->window_render != None) XRenderFreePicture(dpy, canvas->window_render);

    canvas->width = event->width; canvas->height = event->height;

    canvas->canvas_buffer = XCreatePixmap(dpy, canvas->win, canvas->width, canvas->height, canvas->depth);
    XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy, canvas->visual);
    canvas->canvas_render = XRenderCreatePicture(dpy, canvas->canvas_buffer, fmt, 0, NULL);
    canvas->window_render = XRenderCreatePicture(dpy, canvas->win, fmt, 0, NULL);

    if (canvas->client_win != None) {
        XWindowChanges changes = {.width = canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT,
                                  .height = canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM};
        XConfigureWindow(dpy, canvas->client_win, CWWidth | CWHeight, &changes);
    }

    if (canvas->type == WINDOW && canvas->client_win == None) compute_max_scroll(canvas);
    redraw_canvas(canvas);
    XSync(dpy, False);
}

void destroy_canvas(Canvas *canvas) {
    if (!canvas || canvas->type == DESKTOP) return;
    clear_canvas_icons(canvas);

    Display *dpy = get_display();
    if (canvas->client_win != None) {
        XGrabServer(dpy);
        Atom wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
        Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
        Atom *protocols; int num; bool supports_delete = false;
        if (XGetWMProtocols(dpy, canvas->client_win, &protocols, &num)) {
            for (int i = 0; i < num; i++) if (protocols[i] == wm_delete) { supports_delete = true; break; }
            XFree(protocols);
        }
        if (supports_delete) {
            XEvent ev = {.type = ClientMessage};
            ev.xclient.window = canvas->client_win; ev.xclient.message_type = wm_protocols;
            ev.xclient.format = 32; ev.xclient.data.l[0] = wm_delete; ev.xclient.data.l[1] = CurrentTime;
            XSendEvent(dpy, canvas->client_win, False, NoEventMask, &ev);
        } else XKillClient(dpy, canvas->client_win);
        XUnmapWindow(dpy, canvas->win);
        XUngrabServer(dpy);
        XSync(dpy, False);
        return;
    } else {
        if (canvas->window_render != None) XRenderFreePicture(dpy, canvas->window_render);
        if (canvas->canvas_render != None) XRenderFreePicture(dpy, canvas->canvas_render);
        if (canvas->canvas_buffer != None) XFreePixmap(dpy, canvas->canvas_buffer);
        if (canvas->colormap != None) XFreeColormap(dpy, canvas->colormap);
        if (canvas->win != None) XDestroyWindow(dpy, canvas->win);
        free(canvas->path); free(canvas->title);
        if (active_window == canvas) active_window = NULL;
        manage_canvases(false, canvas);
        free(canvas);
        Canvas *desktop = get_desktop_canvas();
        if (desktop) redraw_canvas(desktop);
    }
}

void cleanup_intuition(void) {
    if (!render_context) return;
    for (int i = 0; i < canvas_count; i++) destroy_canvas(canvas_array[i]);
    free(canvas_array); canvas_array = NULL; canvas_count = 0; canvas_array_size = 0;
    if (root_cursor) XFreeCursor(render_context->dpy, root_cursor);
    if (render_context->bg_pixmap != None) XFreePixmap(render_context->dpy, render_context->bg_pixmap);
    XCloseDisplay(render_context->dpy);
    free(render_context); render_context = NULL; display = NULL;
    printf("Called cleanup_intuition() \n");
}