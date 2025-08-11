#include <stddef.h>
#define _POSIX_C_SOURCE 200809L
#include "intuition.h"
#include "render.h"
#include "compositor.h"
#include "menus.h"
#include "config.h"
#include "workbench.h"
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/cursorfont.h>
#include <X11/Xatom.h>
#include <time.h>
#include <math.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xdamage.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>


#define INITIAL_CANVAS_CAPACITY 8

// Forward declarations
static bool should_skip_framing(Window win, XWindowAttributes *attrs);
static bool is_window_valid(Display *dpy, Window win);
static inline void move_and_resize_frame(Canvas *c, int x, int y, int w, int h);

// Global state for intuition
// Display, render context, and canvas registry live here so window
// management code can access them without passing through every call.
static Display *display = NULL;
static RenderContext *render_context = NULL;
static Canvas **canvas_array = NULL;
static int canvas_count = 0;
static int canvas_array_size = 0;
static Bool fullscreen_active = False;
static Canvas *active_window = NULL;

// suppress desktop deactivate flag
static long long g_deactivate_suppress_until_ms = 0;

// Monotonic clock in milliseconds; used for input timing decisions
// like double-click and suppression windows.
static long long now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

// Helper to request client to close via WM_DELETE, with fallback to XKillClient
static void request_client_close(Canvas *canvas) {
    if (!canvas) return;
    Display *dpy = display;
    if (canvas->client_win && is_window_valid(dpy, canvas->client_win)) {
        Atom wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
        Atom wm_delete    = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
        Atom *protocols = NULL; int n = 0;
        bool supports_delete = false;
        if (XGetWMProtocols(dpy, canvas->client_win, &protocols, &n)) {
            for (int i = 0; i < n; i++) if (protocols[i] == wm_delete) { supports_delete = true; break; }
            if (protocols) XFree(protocols);
        }
        if (supports_delete) {
            XClientMessageEvent ev = {0};
            ev.type = ClientMessage;
            ev.window = canvas->client_win;
            ev.message_type = wm_protocols;
            ev.format = 32;
            ev.data.l[0] = wm_delete;
            ev.data.l[1] = CurrentTime;
            XSendEvent(dpy, canvas->client_win, False, NoEventMask, (XEvent *)&ev);
            XFlush(dpy);
        } else {
            XKillClient(dpy, canvas->client_win);
        }
    } else {
        // No client: just destroy our frame
        destroy_canvas(canvas);
    }
}


 

// True if the window's parent is the root (it is a toplevel client).
// We only frame toplevels; children are left to their parents.
static bool is_toplevel_under_root(Window w) {
    // Validate 'w' before querying its ancestry to avoid BadWindow
    XWindowAttributes attrs;
    if (!XGetWindowAttributes(display, w, &attrs)) return false;
    Window root_return, parent_return, *children = NULL; unsigned int n = 0;
    bool ok = XQueryTree(display, w, &root_return, &parent_return, &children, &n);
    if (children) XFree(children);
    if (!ok) return false;
    return parent_return == RootWindow(display, DefaultScreen(display));
}

// Compute a safe value_mask for unmanaged configure requests so we
// avoid illegal fields (e.g., border on InputOnly).
static unsigned long unmanaged_safe_mask(const XConfigureRequestEvent *ev, const XWindowAttributes *attrs, bool attrs_valid) {
    unsigned long mask = ev->value_mask & ~(CWStackMode | CWSibling);
    if (attrs->class == InputOnly) mask &= ~CWBorderWidth;
    if (!attrs_valid) mask &= ~CWBorderWidth;
    return mask;
}

// Validate a window is still valid before issuing X calls to it.
static bool is_window_valid(Display *dpy, Window win) {
    if (win == None) return false;
    XWindowAttributes attrs;
    return XGetWindowAttributes(dpy, win, &attrs) == True;
}

static void apply_resize_and_redraw(Canvas *canvas, int new_w, int new_h);

// Move/resize a frame and update its cached geometry, then schedule
// a redraw so borders and scrollbars stay in sync.
static inline void move_and_resize_frame(Canvas *c, int x, int y, int w, int h) {
    if (!c || !is_window_valid(display, c->win)) return;
    XMoveResizeWindow(display, c->win, x, y, w, h);
    c->x = x; c->y = y;
    apply_resize_and_redraw(c, w, h);
}

// Compute frame size from client size by adding border thickness.
static inline void frame_sizes_from_client(int cw, int ch, int *fw, int *fh) {
    if (fw) *fw = max(1, cw) + BORDER_WIDTH_LEFT + BORDER_WIDTH_RIGHT;
    if (fh) *fh = max(1, ch) + BORDER_HEIGHT_TOP + BORDER_HEIGHT_BOTTOM;
}

// Visible content area inside the frame; excludes borders so layout
// math elsewhere can rely on drawable width/height.
static inline void content_rect(const Canvas *c, int *vw, int *vh) {
    if (!c) { if (vw) *vw = 0; if (vh) *vh = 0; return; }
    if (c->fullscreen) {
        if (vw) *vw = max(1, c->width);
        if (vh) *vh = max(1, c->height);
        return;
    }
    if (vw) *vw = max(1, c->width  - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT);
    if (vh) *vh = max(1, c->height - BORDER_HEIGHT_TOP  - BORDER_HEIGHT_BOTTOM);
}

// True if attrs indicate a normal manageable client; skip override
// redirect and InputOnly which we never frame.
static inline bool is_viewable_client(const XWindowAttributes *attrs) {
    return !(attrs->override_redirect || attrs->class == InputOnly);
}

static void raise_window(Canvas *c) {
    if (c && is_window_valid(display, c->win)) {
        XRaiseWindow(display, c->win);
    }
}

// Lower a window directly above the desktop so it becomes the
// bottom-most among framed windows without hiding under the desktop.
static void lower_window_to_back(Canvas *win_canvas) {
    if (!win_canvas) return;
    Canvas *desktop = get_desktop_canvas();
    if (!desktop) { XLowerWindow(display, win_canvas->win); return; }

    XWindowChanges ch;
    ch.sibling = desktop->win;
    ch.stack_mode = Above; // directly above the desktop means bottom-most among windows
    XConfigureWindow(display, win_canvas->win, CWSibling | CWStackMode, &ch);
    XSync(display, False);
}

static void activate_window_behind(Canvas *current) {
    if (!current) return;
    Window root_ret, parent_ret, *children = NULL; unsigned int n = 0;
    if (!XQueryTree(display, DefaultRootWindow(display), &root_ret, &parent_ret, &children, &n)) return;
    int idx = -1;
    for (unsigned int i = 0; i < n; ++i) if (children[i] == current->win) { idx = (int)i; break; }
    if (idx >= 0) {
        for (int j = idx - 1; j >= 0; --j) {
            Canvas *c = find_canvas(children[j]);
            if (c && c->type == WINDOW && c != current) { set_active_window(c); if (children) XFree(children); return; }
        }
    }
    // Fallback: topmost window excluding current
    for (int i = (int)n - 1; i >= 0; --i) {
        Canvas *c = find_canvas(children[i]);
        if (c && c->type == WINDOW && c != current) { set_active_window(c); break; }
    }
    if (children) XFree(children);
}

void suppress_desktop_deactivate_for_ms(int ms) {
    long long n = now_ms();
    if (ms < 0) ms = 0;
    long long until = n + (long long)ms;
    if (until > g_deactivate_suppress_until_ms) g_deactivate_suppress_until_ms = until;
}

static Cursor root_cursor;
static int width = 0;
static int height = 0;
static int screen = 0;
static Window root = 0;
static int depth = 0;
static Canvas *dragging_canvas = NULL;
static int drag_start_x = 0, drag_start_y = 0;
static int window_start_x = 0, window_start_y = 0;
int randr_event_base = 0;

// resizing
static Canvas *resizing_canvas = NULL;
static int resize_start_x = 0, resize_start_y = 0;
static int window_start_width = 0, window_start_height = 0;

// Canvas being scrolled
static Canvas *scrolling_canvas = NULL;

static bool scrolling_vertical = true;

static int initial_scroll = 0;

static int scroll_start_pos = 0;

static bool g_shutting_down = false;
static bool g_debug_xerrors = false;

// Forward declare the error handler so we can install it early
static int x_error_handler(Display *dpy, XErrorEvent *error);

void begin_shutdown(void) {
    g_shutting_down = true;
}

void install_error_handler(void) {
    const char *env = getenv("AMIWB_DEBUG_XERRORS");
    if (env && *env) g_debug_xerrors = true;
    XSetErrorHandler(x_error_handler);
}

static int x_error_handler(Display *dpy, XErrorEvent *error) {
    // During shutdown, suppress all X errors to ensure graceful exit
    if (g_shutting_down) return 0;

    // Suppress common benign errors that occur during normal operation
    return 0;  // continue execution (non-fatal handling)
}

Display *get_display(void) { return display; }
RenderContext *get_render_context(void) { return render_context; }

// Update menubar visibility and size based on fullscreen state
static void menubar_apply_fullscreen(bool fullscreen) {
    Canvas *menubar = get_menubar();
    if (!menubar) return;
    if (fullscreen) {
        XUnmapWindow(display, menubar->win);
    } else {
        XMapWindow(display, menubar->win);
    }
    apply_resize_and_redraw(menubar, width, MENUBAR_HEIGHT);
}

// Load attributes; if unavailable, provide reasonable defaults for framing
static void load_window_attrs_or_defaults(Window win, XWindowAttributes *attrs, bool *valid_out) {
    bool ok = XGetWindowAttributes(display, win, attrs);
    if (!ok) {
        attrs->x = 200;
        attrs->y = 200;
        attrs->width = 400;
        attrs->height = 300;
        attrs->override_redirect = False;
        attrs->class = InputOutput;
        attrs->border_width = 0;
    }
    if (valid_out) *valid_out = ok;
}

static Canvas *add_canvas(void) {
    if (canvas_count >= canvas_array_size) {
        canvas_array_size = canvas_array_size ? 
            canvas_array_size * 2 : INITIAL_CANVAS_CAPACITY;

        Canvas **new_canvases = realloc(canvas_array, 
            canvas_array_size * sizeof(Canvas *));
        if (!new_canvases) 
            return NULL;
        canvas_array = new_canvases;
    }
    Canvas *new_canvas = malloc(sizeof(Canvas));
    if (!new_canvas) 
        return NULL;

    canvas_array[canvas_count++] = new_canvas;
    return new_canvas;
}

static void remove_canvas(Canvas *canvas_to_remove) {
    for (int i = 0; i < canvas_count; i++) {
        if (canvas_array[i] == canvas_to_remove) {
            memmove(&canvas_array[i], &canvas_array[i + 1],
                    (canvas_count - i - 1) * sizeof(Canvas *));
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

Canvas *find_window_by_path(const char *path) {
    if (!path) return NULL;
    for (int i = 0; i < canvas_count; ++i) {
        Canvas *c = canvas_array[i];
        if (!c || c->type != WINDOW || !c->path) continue;
        if (strcmp(c->path, path) == 0) return c;
    }
    return NULL;
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
    int randr_error_base;
    if (XRRQueryExtension(display, &randr_event_base, &randr_error_base)) {
        XRRSelectInput(display, root, RRScreenChangeNotifyMask);
    } else {
        fprintf(stderr, "XRANDR extension not available; resolution changes may not be handled.\n");
    }
    XSelectInput(display, root, SubstructureRedirectMask | SubstructureNotifyMask | PropertyChangeMask |
                 StructureNotifyMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | KeyPressMask);
    // Advertise minimal EWMH support for fullscreen
    Atom net_supported = XInternAtom(display, "_NET_SUPPORTED", False);
    Atom supported[2];
    supported[0] = XInternAtom(display, "_NET_WM_STATE", False);
    supported[1] = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
    XChangeProperty(display, root, net_supported, XA_ATOM, 32, PropModeReplace,
                   (unsigned char*)supported, 2);
    XSync(display, False);
    return true;
}

// Small utility helpers for clarity and fewer repeated lines
static void choose_visual(CanvasType t, XVisualInfo *out) {
    if (t == DESKTOP) { out->visual = DefaultVisual(display, screen); out->depth = DefaultDepth(display, screen); }
    else if (!XMatchVisualInfo(display, screen, GLOBAL_DEPTH, TrueColor, out)) { out->visual = DefaultVisual(display, screen); out->depth = DefaultDepth(display, screen); }
    XMatchVisualInfo(display, screen, out->depth, TrueColor, out);
}

static long event_mask_for(CanvasType t) {
    long em = ExposureMask | ButtonPressMask | PointerMotionMask | ButtonReleaseMask | KeyPressMask;
    if (t == DESKTOP) em |= SubstructureRedirectMask | SubstructureNotifyMask;
    if (t == WINDOW)  em |= StructureNotifyMask | SubstructureNotifyMask | EnterWindowMask | FocusChangeMask;
    if (t == MENU)    em |= PointerMotionMask | ButtonPressMask | ButtonReleaseMask;
    return em;
}

static bool should_frame_window(Window w, XWindowAttributes *a) {
    if (!XGetWindowAttributes(display, w, a)) return false;
    if (a->map_state != IsViewable || a->class == InputOnly) return false;
    if (should_skip_framing(w, a)) return false;
    return true;
}
static bool init_render_context(void) {
    render_context = malloc(sizeof(RenderContext)); if (!render_context) return false;
    render_context->dpy = display;
    XVisualInfo vinfo; XMatchVisualInfo(display, screen, depth, TrueColor, &vinfo);
    render_context->fmt = XRenderFindVisualFormat(display, vinfo.visual);
    render_context->desk_img = None; render_context->wind_img = None; return true;
}

static void apply_resize_and_redraw(Canvas *c, int nw, int nh) {
    if (!c) return; if (c->width == nw && c->height == nh) return;
    c->width = nw; c->height = nh; render_recreate_canvas_surfaces(c);
    if (c->client_win != None) { int vw, vh; content_rect(c, &vw, &vh);
        XWindowChanges ch = { .width = vw, .height = vh };
        XConfigureWindow(display, c->client_win, CWWidth | CWHeight, &ch);
    } else if (c->type == WINDOW) compute_max_scroll(c);
    redraw_canvas(c);
}

static bool is_fullscreen_active(Window win) {
    Atom wm_state = XInternAtom(display, "_NET_WM_STATE", False);
    Atom fullscreen = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
    Atom type; int format; unsigned long nitems, bytes_after; unsigned char *prop = NULL;
    if (XGetWindowProperty(display, win, wm_state, 0, 1024, False, AnyPropertyType,
            &type, &format, &nitems, &bytes_after, &prop) != Success || !prop) return false;
    bool active = false; for (unsigned long i = 0; i < nitems; i++) if (((Atom*)prop)[i] == fullscreen) { active = true; break; }
    XFree(prop); return active;
}

static void deactivate_all_windows(void) {
    for (int i = 0; i < canvas_count; i++) { Canvas *c = canvas_array[i];
        if (c->type == WINDOW) { c->active = false; redraw_canvas(c); } }
    active_window = NULL;
}

static void init_canvas_metadata(Canvas *c, const char *path, CanvasType t,
                                 int x, int y, int w, int h) {
    *c = (Canvas){0}; c->type = t; c->path = path ? strdup(path) : NULL;
    c->title = path ? strdup(strrchr(path, '/') ? strrchr(path, '/') + 1 : path) : NULL;
    if (c->title && strlen(c->title) == 0) c->title = strdup("System");
    c->x = x; c->y = (t == WINDOW) ? max(y, MENUBAR_HEIGHT) : y;
    c->width = w; c->height = h; c->bg_color = GRAY;
}

static Bool setup_visual_and_window(Canvas *c, CanvasType t,
                                    int x, int y, int w, int h) {
    RenderContext *ctx = get_render_context();
    if (!ctx) return False;

    // Choose a visual/depth suitable for the canvas type
    XVisualInfo vinfo; choose_visual(t, &vinfo);
    c->visual = vinfo.visual;
    c->depth  = vinfo.depth;

    // Create the X window
    XSetWindowAttributes attrs = (XSetWindowAttributes){0};
    attrs.colormap          = XCreateColormap(ctx->dpy, root, c->visual, AllocNone);
    attrs.border_pixel      = 0;
    attrs.background_pixel  = 0;
    attrs.background_pixmap = None;
    unsigned long mask = CWColormap | CWBorderPixel | CWBackPixel | CWBackPixmap;

    int win_x = (t == DESKTOP) ? 0 : x;
    int win_y = (t == DESKTOP) ? MENUBAR_HEIGHT : y;
    int win_w = w;
    int win_h = (t == DESKTOP) ? (h - MENUBAR_HEIGHT) : h;

    c->win = XCreateWindow(display, root, win_x, win_y, win_w, win_h,
                           0, vinfo.depth, InputOutput, c->visual, mask, &attrs);
    if (!c->win) return False;
    c->colormap = attrs.colormap;

    // No internal tagging

    // Select input events
    XSelectInput(ctx->dpy, c->win, event_mask_for(t));

    // Backing pixmap for offscreen rendering
    c->canvas_buffer = XCreatePixmap(ctx->dpy, c->win, w, h, vinfo.depth);
    return c->canvas_buffer ? True : False;
}

static Bool init_render_pictures(Canvas *c, CanvasType t) {
    RenderContext *ctx = get_render_context(); if (!ctx) return False;
    XRenderPictFormat *fmt = XRenderFindVisualFormat(ctx->dpy, c->visual); if (!fmt) return False;
    c->canvas_render = XRenderCreatePicture(ctx->dpy, c->canvas_buffer, fmt, 0, NULL); if (!c->canvas_render) return False;
    Visual *wv = (t == DESKTOP) ? DefaultVisual(ctx->dpy, DefaultScreen(ctx->dpy)) : c->visual;
    XRenderPictFormat *wfmt = XRenderFindVisualFormat(ctx->dpy, wv); if (!wfmt) return False;
    c->window_render = XRenderCreatePicture(ctx->dpy, c->win, wfmt, 0, NULL); return c->window_render ? True : False;
}

static Canvas *frame_client_window(Window client, XWindowAttributes *attrs) {
    if (!attrs) return NULL;
    int fx = max(attrs->x, 200), fy = max(attrs->y, MENUBAR_HEIGHT + 100);
    int fw = attrs->width + BORDER_WIDTH_LEFT + BORDER_WIDTH_RIGHT;
    int fh = attrs->height + BORDER_HEIGHT_TOP + BORDER_HEIGHT_BOTTOM;
    Canvas *frame = create_canvas(NULL, fx, fy, fw, fh, WINDOW); if (!frame) return NULL;
    XReparentWindow(display, client, frame->win, BORDER_WIDTH_LEFT, BORDER_HEIGHT_TOP);
    XSelectInput(display, client, StructureNotifyMask | PropertyChangeMask);
    // Grab mouse buttons on client so clicks can activate the frame and set focus
    unsigned int mods[] = { AnyModifier };
    int buttons[] = { Button1, Button2, Button3 };
    for (unsigned int mi = 0; mi < sizeof(mods)/sizeof(mods[0]); ++mi) {
        for (unsigned int bi = 0; bi < sizeof(buttons)/sizeof(buttons[0]); ++bi) {
            XGrabButton(display,
                        buttons[bi], mods[mi],
                        client,
                        True,                    // owner_events (deliver to grab window)
                        ButtonPressMask,         // event mask we care about
                        GrabModeSync,            // freeze pointer until we replay
                        GrabModeAsync,           // keyboard unaffected
                        None, None);
        }
    }
    if (attrs->border_width != 0) { XWindowChanges b = { .border_width = 0 }; XConfigureWindow(display, client, CWBorderWidth, &b); }
    frame->client_win = client;
    if (frame->client_win != None) {
        XClassHint ch; if (XGetClassHint(display, frame->client_win, &ch) && ch.res_name) frame->title = ch.res_name; else frame->title = "NoNameApp";
    }
    XAddToSaveSet(display, client); return frame;
}

Canvas *init_intuition(void) {
    if (!init_display_and_root() || !init_render_context()) return NULL;
    Canvas *desktop = create_canvas(getenv("HOME"), 0, 20, width, height, DESKTOP);
    if (!desktop) return NULL;
    imlib_context_set_display(display);
    imlib_context_set_visual(desktop->visual);
    imlib_context_set_colormap(desktop->colormap);
    imlib_set_cache_size(0);
    render_load_wallpapers();
    Window rt, pt, *kids = NULL; unsigned int nk = 0;
    if (XQueryTree(display, root, &rt, &pt, &kids, &nk)) {
        for (unsigned int i = 0; i < nk; i++) {
            Window w = kids[i];
            bool own = false; for (int j = 0; j < canvas_count; j++) if (canvas_array[j]->win == w || canvas_array[j]->client_win == w) { own = true; break; }
            if (own) continue; XWindowAttributes a;
            if (!should_frame_window(w, &a)) continue;
            Canvas *f = frame_client_window(w, &a); if (!f) continue; raise_window(f); redraw_canvas(f);
        }
        if (kids) XFree(kids);
    }
    XSync(display, False); 
    redraw_canvas(desktop); 
    return desktop;
}

static void init_scroll(Canvas *canvas) {
    if (!canvas || canvas->type != WINDOW) return;
    canvas->scroll_x = 0;
    canvas->scroll_y = 0;
    content_rect(canvas, &canvas->content_width, &canvas->content_height);
    compute_max_scroll(canvas);
}

// Minimal implementations to satisfy external references
Canvas *get_desktop_canvas(void) {
    return canvas_count > 0 ? canvas_array[0] : NULL;
}

Canvas *find_canvas(Window win) {
    for (int i = 0; i < canvas_count; i++) {
        if (canvas_array[i]->win == win) return canvas_array[i];
    }
    return NULL;
}

Canvas *find_canvas_by_client(Window cw) {
    for (int i = 0; i < canvas_count; i++) {
        if (canvas_array[i]->client_win == cw) return canvas_array[i];
    }
    return NULL;
}

void set_active_window(Canvas *c) {
    if (!c || c->type != WINDOW) return;
    for (int i = 0; i < canvas_count; i++) {
        Canvas *o = canvas_array[i];
        if (o->type == WINDOW && o != c) {
            o->active = false;
            redraw_canvas(o);
        }
    }
    active_window = c;
    c->active = true;
    XRaiseWindow(display, c->win);
    compositor_sync_stacking(display);
    Window focus = (c->client_win != None) ? c->client_win : c->win;
    XSetInputFocus(display, focus, RevertToParent, CurrentTime);
    redraw_canvas(c);
    XSync(display, False);
}

Canvas *get_active_window(void) {
    return active_window;
}

void compute_max_scroll(Canvas *c) {
    if (!c) return;
    int vw, vh;
    content_rect(c, &vw, &vh);
    c->max_scroll_x = max(0, c->content_width - vw);
    c->max_scroll_y = max(0, c->content_height - vh);
    c->scroll_x = min(c->scroll_x, c->max_scroll_x);
    c->scroll_y = min(c->scroll_y, c->max_scroll_y);
}


// ===============
// EWMH Fullscreen
// ===============
static void set_net_wm_state_fullscreen(Window client, bool on) {
    Atom wm_state = XInternAtom(display, "_NET_WM_STATE", False);
    Atom fullscreen = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
    if (on) {
        XChangeProperty(display, client, wm_state, XA_ATOM, 32, PropModeReplace,
                        (unsigned char *)&fullscreen, 1);
    } else {
        XDeleteProperty(display, client, wm_state);
    }
}

void intuition_enter_fullscreen(Canvas *c) {
    if (!c || c->type != WINDOW || c->fullscreen) return;
    c->saved_x = c->x; c->saved_y = c->y; c->saved_w = c->width; c->saved_h = c->height;
    c->fullscreen = true;
    fullscreen_active = True;
    int sw = DisplayWidth(display, DefaultScreen(display));
    int sh = DisplayHeight(display, DefaultScreen(display));
    move_and_resize_frame(c, 0, 0, sw, sh);
    if (c->client_win != None) {
        XMoveWindow(display, c->client_win, 0, 0);
        set_net_wm_state_fullscreen(c->client_win, true);
    }
    // Hide menubar while any fullscreen is active
    menubar_apply_fullscreen(true);
    redraw_canvas(c);
    XSync(display, False);
}

void intuition_exit_fullscreen(Canvas *c) {
    if (!c || c->type != WINDOW || !c->fullscreen) return;
    c->fullscreen = false;
    if (c->client_win != None) {
        XMoveWindow(display, c->client_win, BORDER_WIDTH_LEFT, BORDER_HEIGHT_TOP);
        set_net_wm_state_fullscreen(c->client_win, false);
    }
    int rx = c->saved_x, ry = c->saved_y, rw = c->saved_w, rh = c->saved_h;
    if (rw <= 0 || rh <= 0) { rw = 800; rh = 600; }
    move_and_resize_frame(c, max(0, rx), max(MENUBAR_HEIGHT, ry), rw, rh);
    fullscreen_active = False;
    // Show menubar again when exiting fullscreen
    menubar_apply_fullscreen(false);
    redraw_canvas(c);
    XSync(display, False);
}

void intuition_handle_client_message(XClientMessageEvent *event) {
    if (!event) return;
    Atom net_wm_state = XInternAtom(display, "_NET_WM_STATE", False);
    if ((Atom)event->message_type != net_wm_state) return;
    Atom fs = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
    long action = event->data.l[0];
    Atom a1 = (Atom)event->data.l[1];
    Atom a2 = (Atom)event->data.l[2];
    if (!(a1 == fs || a2 == fs)) return;
    Canvas *c = find_canvas_by_client(event->window);
    if (!c) c = find_canvas(event->window);
    if (!c) return;
    if (action == 1) {
        intuition_enter_fullscreen(c);
    } else if (action == 0) {
        intuition_exit_fullscreen(c);
    } else if (action == 2) {
        if (c->fullscreen) intuition_exit_fullscreen(c); else intuition_enter_fullscreen(c);
    }
}


static void find_next_desktop_slot(Canvas *desk, int *ox, int *oy) {
    if (!desk || !ox || !oy) return;
    const int sx = 20, sy = 40, step_x = 110;
    // Use same spacing as main desktop icons (System/Home)
    const int label_space = 20;
    
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
                if (ic->display_window != desk->win || ic->type != TYPE_ICONIFIED) continue;
                if (ic->x == x && ic->y == y) {
                    y += 80;  // Move down and check again
                    collision_found = true;
                    break;  // Start collision check over from beginning
                }
            }
        } while (collision_found && y + 64 < desk->height);
        
        if (y + 64 < desk->height) { *ox = x; *oy = y; return; }
    }
    *ox = sx; *oy = first_iconified_y;
}

void iconify_canvas(Canvas *c) {
    if (!c || c->type != WINDOW) return;
    Canvas *desk = get_desktop_canvas(); if (!desk) return;
    int nx = 20, ny = 40; find_next_desktop_slot(desk, &nx, &ny);
    const char *icon_path = NULL; char *label = NULL;
    if (c->client_win == None) { label = c->title ? strdup(c->title) : strdup("Untitled"); icon_path = "/usr/local/share/amiwb/icons/filer.info"; }
    else {
        XClassHint ch;
        if (XGetClassHint(display, c->client_win, &ch) && ch.res_name) {
            char icon_full[256]; snprintf(icon_full, sizeof(icon_full), "/usr/local/share/amiwb/icons/%s.info", ch.res_name);
            struct stat st; icon_path = (stat(icon_full, &st) == 0) ? icon_full : "/usr/local/share/amiwb/icons/def_tool.info";
            label = strdup(ch.res_name); XFree(ch.res_name); XFree(ch.res_class);
        } else { label = strdup("Untitled"); icon_path = "/usr/local/share/amiwb/icons/def_tool.info"; }
    }
    create_icon(icon_path, desk, nx, ny);
    FileIcon **ia = get_icon_array(); FileIcon *ni = ia[get_icon_count() - 1];
    ni->type = TYPE_ICONIFIED; free(ni->label); ni->label = label; free(ni->path); ni->path = NULL; ni->iconified_canvas = c;
    XUnmapWindow(display, c->win); if (active_window == c) active_window = NULL; redraw_canvas(desk); XSync(display, False);
}

// Create new window
Canvas *create_canvas(const char *path, int x, int y, int width, 
        int height, CanvasType type) {
    
    RenderContext *ctx = get_render_context();
    if (!ctx) return NULL;

    Canvas *canvas = manage_canvases(true, NULL);
    if (!canvas) return NULL;
    init_canvas_metadata(canvas, path, type, x, y, width, height);
    canvas->close_armed = false;

    if (!setup_visual_and_window(canvas, type, x, y, width, height)) {
        destroy_canvas(canvas);
        return NULL;
    }

    if (!init_render_pictures(canvas, type)) {
        destroy_canvas(canvas);
        return NULL;
    }

    init_scroll(canvas);

    if (type != DESKTOP) {
        if (type == WINDOW ) {
            XSetWindowAttributes attrs = {0};
            attrs.background_pixmap = None;
            XChangeWindowAttributes(display, canvas->win, CWBackPixmap, &attrs);
            redraw_canvas(canvas);
        }
        XMapRaised(ctx->dpy, canvas->win);
        if (type == WINDOW) {
            // Newly created Workbench windows should become active immediately
            set_active_window(canvas);
        }
        XSync(ctx->dpy, False);
    } else {
        // Map desktop window at bottom of stack
        XMapWindow(ctx->dpy, canvas->win);
        redraw_canvas(canvas);
        XSync(ctx->dpy, False);
    }
    return canvas;
}

// Should skip framing
static bool should_skip_framing(Window win, XWindowAttributes *attrs) {
    // Only skip truly unmanaged windows
    if (attrs->override_redirect || attrs->class == InputOnly) return true;
    // Always frame normal client windows
    return false;
}

// Select next window
static void select_next_window(Canvas *closing_canvas) {
    if (active_window == closing_canvas) active_window = NULL;

    Window root_return, parent_return;
    Window *children;
    unsigned int nchildren;
    if (XQueryTree(display, root, &root_return, &parent_return, 
            &children, &nchildren)) {

        for (int i = nchildren - 1; i >= 0; i--) {  // Top to bottom
            if (children[i] == closing_canvas->win) 
                continue;  

            Canvas *next_canvas = find_canvas(children[i]);
            if (next_canvas && next_canvas->type == WINDOW) {
                set_active_window(next_canvas);
                break;
            }
        }
        XFree(children);
    }

    // Fallback if none
    if (!active_window) {
        active_window = get_desktop_canvas();
    }
}

void intuition_handle_expose(XExposeEvent *event) {
    Canvas *canvas = find_canvas(event->window);
    if (canvas && !fullscreen_active) redraw_canvas(canvas);
}

void intuition_handle_property_notify(XPropertyEvent *event) {
    // Track EWMH fullscreen changes via property updates on the client
    Atom net_wm_state = XInternAtom(display, "_NET_WM_STATE", False);
    if (event->atom != net_wm_state) return;

    Canvas *canvas = find_canvas(event->window);
    if (canvas && canvas->type == WINDOW) {
        fullscreen_active = is_fullscreen_active(event->window);
        menubar_apply_fullscreen(fullscreen_active);
    }
}

//

// Common helpers for concise handlers
static inline Bool begin_frame_drag(Canvas *c, XButtonEvent *e) {
    dragging_canvas = c;
    drag_start_x = e->x_root; drag_start_y = e->y_root;
    window_start_x = c->x; window_start_y = c->y;
    return True;
}

static inline Bool begin_frame_resize(Canvas *c, XButtonEvent *e) {
    resizing_canvas = c;
    resize_start_x = e->x_root; resize_start_y = e->y_root;
    window_start_width = c->width; window_start_height = c->height;
    return True;
}

enum { SCROLL_STEP = 20, ARROW_SIZE = 20, TRACK_MARGIN = 10, TRACK_RESERVED = 54 };

// -------- Scrollbar utilities 
static inline void vscroll_geom(Canvas *c, int *x, int *y, int *w, int *h) {
    *x = c->width - BORDER_WIDTH_RIGHT;
    *y = BORDER_HEIGHT_TOP + TRACK_MARGIN;
    *w = BORDER_WIDTH_RIGHT;
    *h = (c->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM) - TRACK_RESERVED - TRACK_MARGIN;
}

static inline void hscroll_geom(Canvas *c, int *x, int *y, int *w, int *h) {
    *x = BORDER_WIDTH_LEFT + TRACK_MARGIN;
    *y = c->height - BORDER_HEIGHT_BOTTOM;
    *w = (c->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT) - TRACK_RESERVED - TRACK_MARGIN;
    *h = BORDER_HEIGHT_BOTTOM;
}

static inline int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo; if (v > hi) return hi; return v;
}

static inline int knob_size(int track_len, int content_len) {
    float ratio = (float)track_len / (float)content_len;
    int size = (int)(ratio * track_len);
    if (size < MIN_KNOB_SIZE) size = MIN_KNOB_SIZE;
    if (size > track_len) size = track_len;
    return size;
}

static inline int knob_pos_from_scroll(int track_len, int knob_len, int scroll, int max_scroll) {
    if (max_scroll <= 0) return 0;
    float pos_ratio = (float)scroll / (float)max_scroll;
    int free_len = track_len - knob_len;
    if (free_len <= 0) return 0;
    return (int)(pos_ratio * free_len);
}

static inline int scroll_from_click(int track_off, int track_len, int max_scroll, int click_pos) {
    float click_ratio = (float)(click_pos - track_off) / (float)track_len;
    int val = (int)(click_ratio * (float)max_scroll);
    return clamp_int(val, 0, max_scroll);
}

static inline void begin_scroll(Canvas *c, bool vertical, int initial, int start_root) {
    scrolling_canvas = c; scrolling_vertical = vertical;
    initial_scroll = initial; scroll_start_pos = start_root;
}

static Bool handle_scrollbars(Canvas *canvas, XButtonEvent *event) {
    if (canvas->client_win != None) return False; // no scrollbars on client windows

    // mouse wheel 
    if (event->button == 4 || event->button == 5) {
        if (canvas->max_scroll_y <= 0 && canvas->max_scroll_x <= 0) return False;
        
        bool scroll_up = (event->button == 4);
        bool has_shift = (event->state & ShiftMask);
        
        if (has_shift && canvas->max_scroll_x > 0) {
            // Shift + wheel = horizontal scroll
            int new_x = canvas->scroll_x + (scroll_up ? -SCROLL_STEP : SCROLL_STEP);
            canvas->scroll_x = clamp_int(new_x, 0, canvas->max_scroll_x);
        } else if (canvas->max_scroll_y > 0) {
            // Normal wheel = vertical scroll
            int new_y = canvas->scroll_y + (scroll_up ? -SCROLL_STEP : SCROLL_STEP);
            canvas->scroll_y = clamp_int(new_y, 0, canvas->max_scroll_y);
        }
    }
    
    int sb_x, sb_y, sb_w, sb_h; vscroll_geom(canvas, &sb_x, &sb_y, &sb_w, &sb_h);

    if (event->x >= sb_x && event->x < sb_x + sb_w &&
        event->y >= sb_y && event->y < sb_y + sb_h &&
        event->button == Button1) {
        int track_len = sb_h;
        int content_len = canvas->content_height;
        int knob_len = knob_size(track_len, content_len);
        int knob_y = sb_y + knob_pos_from_scroll(track_len, knob_len, canvas->scroll_y, canvas->max_scroll_y);
        if (event->y >= knob_y && event->y < knob_y + knob_len) {
            begin_scroll(canvas, true, canvas->scroll_y, event->y_root);
            return True;
        } else {
            int new_sy = scroll_from_click(sb_y, track_len, canvas->max_scroll_y, event->y);
            canvas->scroll_y = new_sy; redraw_canvas(canvas); return True;
        }
    }
    if (event->x >= sb_x && event->x < sb_x + sb_w && event->button == Button1) {
        if (event->y >= (canvas->height - BORDER_HEIGHT_BOTTOM - (2 * ARROW_SIZE)) &&
            event->y < (canvas->height - BORDER_HEIGHT_BOTTOM - ARROW_SIZE)) {
            canvas->scroll_y = max(0, canvas->scroll_y - SCROLL_STEP); redraw_canvas(canvas); return True;
        } else if (event->y >= (canvas->height - BORDER_HEIGHT_BOTTOM - ARROW_SIZE) &&
                   event->y < (canvas->height - BORDER_HEIGHT_BOTTOM)) {
            canvas->scroll_y = min(canvas->max_scroll_y, canvas->scroll_y + SCROLL_STEP); redraw_canvas(canvas); return True;
        }
    }

    int hb_x, hb_y, hb_w, hb_h; hscroll_geom(canvas, &hb_x, &hb_y, &hb_w, &hb_h);
    if (event->x >= hb_x && event->x < hb_x + hb_w &&
        event->y >= hb_y && event->y < hb_y + hb_h &&
        event->button == Button1) {
        int track_len = hb_w;
        int content_len = canvas->content_width;
        int knob_len = knob_size(track_len, content_len);
        int knob_x = hb_x + knob_pos_from_scroll(track_len, knob_len, canvas->scroll_x, canvas->max_scroll_x);
        if (event->x >= knob_x && event->x < knob_x + knob_len) {
            begin_scroll(canvas, false, canvas->scroll_x, event->x_root); return True;
        } else {
            int new_sx = scroll_from_click(hb_x, track_len, canvas->max_scroll_x, event->x);
            canvas->scroll_x = new_sx; redraw_canvas(canvas); return True;
        }
    }
    if (event->y >= hb_y && event->y < hb_y + hb_h && event->button == Button1) {
        if (event->x >= (canvas->width - BORDER_WIDTH_RIGHT - (2 * ARROW_SIZE)) &&
            event->x < (canvas->width - BORDER_WIDTH_RIGHT - ARROW_SIZE)) {
            canvas->scroll_x = max(0, canvas->scroll_x - SCROLL_STEP); redraw_canvas(canvas); return True;
        } else if (event->x >= (canvas->width - BORDER_WIDTH_RIGHT - ARROW_SIZE) &&
                   event->x < (canvas->width - BORDER_WIDTH_RIGHT)) {
            canvas->scroll_x = min(canvas->max_scroll_x, canvas->scroll_x + SCROLL_STEP); redraw_canvas(canvas); return True;
        }
    }
    return False;
}

static void scroll_update_from_motion(Canvas *c, bool vertical, int initial, int start_root, int current_root) {
    int delta = vertical ? (current_root - start_root) : (current_root - start_root);
    int track_len = vertical ? (c->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM) - TRACK_RESERVED - TRACK_MARGIN : (c->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT) - TRACK_RESERVED - TRACK_MARGIN;
    int knob_len = knob_size(track_len, vertical ? c->content_height : c->content_width);
    int track = max(1, track_len - knob_len);
    float knob0 = (vertical ? c->max_scroll_y : c->max_scroll_x > 0) ? ((float)initial / (float)(vertical ? c->max_scroll_y : c->max_scroll_x)) * track : 0.0f;
    float knob = min((float)track, max(0.0f, knob0 + (float)delta));
    int new_scroll = (vertical ? c->max_scroll_y : c->max_scroll_x > 0) ? (int)roundf((knob / (float)track) * (float)(vertical ? c->max_scroll_y : c->max_scroll_x)) : 0;
    if (vertical) c->scroll_y = new_scroll; else c->scroll_x = new_scroll;
    redraw_canvas(c);
}


// Unified hit-test for frame controls
typedef enum {
    HIT_NONE = 0,
    HIT_CLOSE,
    HIT_ICONIFY,
    HIT_MAXIMIZE,
    HIT_LOWER,
    HIT_TITLE,
    HIT_RESIZE
} TitlebarHit;

static inline TitlebarHit hit_test(Canvas *c, int x, int y) {
    if (y < BORDER_HEIGHT_TOP) {
        if (x < BUTTON_CLOSE_SIZE) return HIT_CLOSE;
        int right = c->width;
        if (x >= right - BUTTON_LOWER_SIZE) return HIT_LOWER;
        if (x >= right - (BUTTON_LOWER_SIZE + BUTTON_MAXIMIZE_SIZE)) return HIT_MAXIMIZE;
        if (x >= right - (BUTTON_LOWER_SIZE + BUTTON_MAXIMIZE_SIZE + BUTTON_ICONIFY_SIZE)) return HIT_ICONIFY;
        return HIT_TITLE;
    }
    if (x >= c->width - BORDER_WIDTH_RIGHT && y >= c->height - BORDER_HEIGHT_BOTTOM) return HIT_RESIZE;
    return HIT_NONE;
}

static inline void toggle_menubar_and_redraw(void) {
    toggle_menubar_state();
    Canvas *mb = get_menubar();
    if (mb) { XMapWindow(display, mb->win); redraw_canvas(mb); }
}

// Button press helpers
static void handle_desktop_button(Canvas *canvas, XButtonEvent *event) {
    if (event->button == Button3) {
        toggle_menubar_and_redraw();
    }
    if (event->button == Button1) {
        if (now_ms() < g_deactivate_suppress_until_ms) {
            return;
        }
        bool on_icon = false;
        FileIcon **icon_array = get_icon_array();
        int icon_count = get_icon_count();
        for (int i = 0; i < icon_count; i++) {
            if (icon_array[i]->display_window == canvas->win &&
                event->x >= icon_array[i]->x &&
                event->x < icon_array[i]->x + icon_array[i]->width &&
                event->y >= icon_array[i]->y &&
                event->y < icon_array[i]->y + icon_array[i]->height) { on_icon = true; break; }
        }
        if (!on_icon) deactivate_all_windows();
    }
}



static Bool handle_frame_controls(Canvas *canvas, XButtonEvent *event) {
    TitlebarHit hit = hit_test(canvas, event->x, event->y);
    if (event->button != Button1) return False;
    switch (hit) {
        case HIT_CLOSE:
            canvas->close_armed = true; // defer action to ButtonRelease
            redraw_canvas(canvas);
            return True;
        case HIT_ICONIFY:  iconify_canvas(canvas); return True;
        case HIT_MAXIMIZE: {
            Canvas *desk = get_desktop_canvas();
            if (desk) {
                int new_w = desk->width;
                int new_h = desk->height - (MENUBAR_HEIGHT - 1);
                move_and_resize_frame(canvas, 0, MENUBAR_HEIGHT, new_w, new_h);
            }
            return True;
        }
        case HIT_LOWER:
            lower_window_to_back(canvas);
            canvas->active = false;
            activate_window_behind(canvas);
            compositor_sync_stacking(display);
            redraw_canvas(canvas);
            return True;
        case HIT_TITLE:
            return begin_frame_drag(canvas, event);
        case HIT_RESIZE:
            return begin_frame_resize(canvas, event);
        default:
            return False;
    }
}

// frame control handlers
static bool g_last_press_consumed = false;
bool intuition_last_press_consumed(void) { return g_last_press_consumed; }
bool intuition_is_scrolling_active(void) { return scrolling_canvas != NULL; }

static Bool handle_window_controls(Canvas *canvas, XButtonEvent *event) {
    if (handle_frame_controls(canvas, event)) return True;
    if (handle_scrollbars(canvas, event))     return True;
    return False;
}

void intuition_handle_button_press(XButtonEvent *event) {
    Canvas *canvas = find_canvas(event->window);
    if (!canvas) return;

    if (canvas->type != MENU && (event->button == Button1 || event->button == Button3)) {
        if (get_show_menus_state()) { toggle_menubar_and_redraw(); return; }
    }

    if (canvas->type == DESKTOP) {
        handle_desktop_button(canvas, event);
        redraw_canvas(canvas);
        g_last_press_consumed = true;
        return;
    }

    if (canvas->type != WINDOW) 
        return;

    set_active_window(canvas);
    g_last_press_consumed = handle_window_controls(canvas, event);
}

static Bool handle_drag_motion(XMotionEvent *event) {
    if (dragging_canvas) {
        int delta_x = event->x_root - drag_start_x;
        int delta_y = event->y_root - drag_start_y;
        window_start_x += delta_x; 
        // Clamp y to ensure titlebar is below menubar
        window_start_y = max(window_start_y + delta_y, MENUBAR_HEIGHT);  
        XMoveWindow(display, dragging_canvas->win, window_start_x, window_start_y);
        dragging_canvas->x = window_start_x; 
        dragging_canvas->y = window_start_y;
        drag_start_x = event->x_root; drag_start_y = event->y_root;
        return True;
    }
    return False;
}

static Bool handle_resize_motion(XMotionEvent *event) {
    if (resizing_canvas) {
        int delta_x = event->x_root - resize_start_x;
        int delta_y = event->y_root - resize_start_y;
        int new_width = max(150, window_start_width + delta_x);
        int new_height = max(150, window_start_height + delta_y);
        
        // Only resize if change exceeds a small threshold to avoid micro-adjustments
        const int resize_threshold = 2;
        if (abs(new_width - resizing_canvas->width) > resize_threshold ||
            abs(new_height - resizing_canvas->height) > resize_threshold) {
            XResizeWindow(display, resizing_canvas->win, new_width, new_height);
            // Update start positions to current for next delta calculation
            resize_start_x = event->x_root; 
            resize_start_y = event->y_root;
            window_start_width = new_width; 
            window_start_height = new_height;
        }
        return True;
    }
    return False;
}

static Bool handle_scroll_motion(XMotionEvent *event) {
    if (scrolling_canvas) {
        if (scrolling_vertical)
            scroll_update_from_motion(scrolling_canvas, true, initial_scroll, scroll_start_pos, event->y_root);
        else
            scroll_update_from_motion(scrolling_canvas, false, initial_scroll, scroll_start_pos, event->x_root);
        redraw_canvas(scrolling_canvas);
        return True;
    }
    return False;
}

void intuition_handle_motion_notify(XMotionEvent *event) {
    if (handle_drag_motion(event)) return;
    if (handle_resize_motion(event)) return;
    if (handle_scroll_motion(event)) return;
}

void intuition_handle_destroy_notify(XDestroyWindowEvent *event) {
    Canvas *canvas = find_canvas_by_client(event->window) ?: find_canvas(event->window);
    if (!canvas) return;
    destroy_canvas(canvas);
}

void intuition_handle_button_release(XButtonEvent *event) {
    dragging_canvas = NULL;
    resizing_canvas = NULL;
    scrolling_canvas = NULL;
    // Handle deferred close action if any
    Canvas *canvas = find_canvas(event->window);
    if (canvas && canvas->close_armed) {
        TitlebarHit hit = hit_test(canvas, event->x, event->y);
        canvas->close_armed = false;
        if (hit == HIT_CLOSE) {
            request_client_close(canvas);
            return;
        }
    }
    // Intentionally no compositor stack dump here (perf)
}

// Frame a client window, activate its frame, optionally map the client.
static void frame_and_activate(Window client, XWindowAttributes *attrs, bool map_client) {
    Canvas *frame = frame_client_window(client, attrs);
    if (!frame) {
        if (map_client) XMapWindow(display, client);
        return;
    }
    if (map_client) XMapWindow(display, client);
    set_active_window(frame);
    redraw_canvas(frame);
    XSync(display, False);
}

void intuition_handle_map_request(XMapRequestEvent *event) {
    XWindowAttributes attrs; bool attrs_valid;
    load_window_attrs_or_defaults(event->window, &attrs, &attrs_valid);

    if (should_skip_framing(event->window, &attrs)) {
        XMapWindow(display, event->window);
        XSync(display, False);
        return;
    }
    frame_and_activate(event->window, &attrs, true);
}

// Handle MapNotify for toplevel client windows that became viewable without a MapRequest
void intuition_handle_map_notify(XMapEvent *event) {
    // Ignore if this is one of our frame windows or already managed as client
    if (find_canvas(event->window) || find_canvas_by_client(event->window)) {
        return;
    }

    // Ensure it's a toplevel, viewable, input-output window and not override-redirect
    XWindowAttributes attrs; bool attrs_valid;
    load_window_attrs_or_defaults(event->window, &attrs, &attrs_valid);
    if (!is_viewable_client(&attrs)) return;
    if (!is_toplevel_under_root(event->window)) return;

    if (should_skip_framing(event->window, &attrs)) {
        return;
    }

    frame_and_activate(event->window, &attrs, true);
}

static void handle_configure_unmanaged(XConfigureRequestEvent *event) {
    XWindowAttributes attrs; bool attrs_valid;
    load_window_attrs_or_defaults(event->window, &attrs, &attrs_valid);
    unsigned long safe_mask = unmanaged_safe_mask(event, &attrs, attrs_valid);

    XWindowChanges changes = (XWindowChanges){0};
    if (safe_mask & CWX) changes.x = event->x;
    if (safe_mask & CWY) changes.y = max(event->y, MENUBAR_HEIGHT);
    if (safe_mask & CWWidth) changes.width = max(1, event->width);
    if (safe_mask & CWHeight) changes.height = max(1, event->height);

    if (attrs.class == InputOutput && (safe_mask & CWBorderWidth)) {
        bool need_set_border = false;
        if ((event->value_mask & CWBorderWidth) && event->border_width != 0) need_set_border = true;
        if (attrs_valid && attrs.border_width != 0) need_set_border = true;
        if (need_set_border) { changes.border_width = 0; safe_mask |= CWBorderWidth; }
    }
    if (safe_mask) XConfigureWindow(display, event->window, safe_mask, &changes);
    XSync(display, False);
}

static void handle_configure_managed(Canvas *canvas, XConfigureRequestEvent *event) {
    XWindowChanges frame_changes = (XWindowChanges){0};
    unsigned long frame_mask = 0;

    if (event->value_mask & (CWWidth | CWHeight)) {
        frame_sizes_from_client(event->width, event->height, &frame_changes.width, &frame_changes.height);
        frame_mask |= (event->value_mask & CWWidth) ? CWWidth : 0;
        frame_mask |= (event->value_mask & CWHeight) ? CWHeight : 0;
    }
    if (event->value_mask & CWX) { frame_changes.x = event->x; frame_mask |= CWX; }
    if (event->value_mask & CWY) { frame_changes.y = max(event->y, MENUBAR_HEIGHT); frame_mask |= CWY; }

    if ((event->value_mask & (CWStackMode | CWSibling)) == (CWStackMode | CWSibling) &&
        event->detail >= 0 && event->detail <= 4) {
        XWindowAttributes sibling_attrs;
        if (XGetWindowAttributes(display, event->above, &sibling_attrs) && sibling_attrs.map_state == IsViewable) {
            frame_changes.stack_mode = event->detail;
            frame_changes.sibling = event->above;
            frame_mask |= CWStackMode | CWSibling;
        }
    }
    if (frame_mask) XConfigureWindow(display, canvas->win, frame_mask, &frame_changes);

    XWindowChanges client_changes = (XWindowChanges){0};
    unsigned long client_mask = 0;
    if (event->value_mask & CWWidth) { client_changes.width = max(1, event->width); client_mask |= CWWidth; }
    if (event->value_mask & CWHeight) { client_changes.height = max(1, event->height); client_mask |= CWHeight; }
    if (event->value_mask & CWBorderWidth) { client_changes.border_width = 0; client_mask |= CWBorderWidth; }
    client_changes.x = BORDER_WIDTH_LEFT; client_changes.y = BORDER_HEIGHT_TOP; client_mask |= CWX | CWY;
    if (client_mask) XConfigureWindow(display, event->window, client_mask, &client_changes);
    XSync(display, False);
}

void intuition_handle_configure_request(XConfigureRequestEvent *event) {
    Canvas *canvas = find_canvas_by_client(event->window);
    if (!canvas) { handle_configure_unmanaged(event); return; }
    handle_configure_managed(canvas, event);
    apply_resize_and_redraw(canvas, event->width, event->height);
    // allow natural batching; no XSync here
}

// Handle ConfigureNotify (post-resize) to keep frame and client surfaces in sync
void intuition_handle_configure_notify(XConfigureEvent *event) {
    Canvas *canvas = find_canvas(event->window);
    if (!canvas) return;
    apply_resize_and_redraw(canvas, event->width, event->height);
}

// Handle XRandR screen size changes: resize desktop/menubar and reload wallpapers
void intuition_handle_rr_screen_change(XRRScreenChangeNotifyEvent *event) {
    width = event->width;
    height = event->height;

    Canvas *desktop = get_desktop_canvas();
    if (desktop) {
        apply_resize_and_redraw(desktop, width, height);
        render_load_wallpapers();
    }

    Canvas *menubar_canvas = get_menubar();
    if (menubar_canvas) {
        apply_resize_and_redraw(menubar_canvas, width, MENUBAR_HEIGHT);
    }

    XSync(display, False);
}

// destroy and cleanup
void destroy_canvas(Canvas *canvas) {
    if (!canvas || canvas->type == DESKTOP) return;
    clear_canvas_icons(canvas);

    Display *dpy = get_display();

    // If this canvas frames a client, request it to close first
    if (canvas->client_win != None) {
        XWindowAttributes attrs;
        if (XGetWindowAttributes(dpy, canvas->client_win, &attrs)) {
            XGrabServer(dpy);
            Atom wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
            Atom wm_delete    = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
            Atom *protocols = NULL; int num = 0; bool supports_delete = false;
            if (XGetWMProtocols(dpy, canvas->client_win, &protocols, &num)) {
                for (int i = 0; i < num; i++) { if (protocols[i] == wm_delete) { supports_delete = true; break; } }
                XFree(protocols);
            }
            if (supports_delete) {
                XEvent ev = { .type = ClientMessage };
                ev.xclient.window = canvas->client_win;
                ev.xclient.message_type = wm_protocols;
                ev.xclient.format = 32;
                ev.xclient.data.l[0] = wm_delete;
                ev.xclient.data.l[1] = CurrentTime;
                XSendEvent(dpy, canvas->client_win, False, NoEventMask, &ev);
                XSync(dpy, False);
            } else {
                XKillClient(dpy, canvas->client_win);
            }
            XUngrabServer(dpy);
        }
        // Unmap the frame; proceed with full teardown (no early return)
        if (canvas->win != None) {
            XWindowAttributes fa;
            if (XGetWindowAttributes(dpy, canvas->win, &fa)) {
                XUnmapWindow(dpy, canvas->win);
            }
        }
        XSync(dpy, False);
        canvas->client_win = None;
    }

    // Update focus/activation before tearing down resources
    if (canvas->type == WINDOW) {
        select_next_window(canvas);
    }

    // Free resources in safe order
    XSync(dpy, False);
    if (canvas->window_render != None) {
        XRenderFreePicture(dpy, canvas->window_render);
        canvas->window_render = None;
    }
    if (canvas->canvas_render != None) {
        XRenderFreePicture(dpy, canvas->canvas_render);
        canvas->canvas_render = None;
    }
    if (canvas->canvas_buffer != None) {
        XFreePixmap(dpy, canvas->canvas_buffer);
        canvas->canvas_buffer = None;
    }
    if (canvas->colormap != None) {
        XFreeColormap(dpy, canvas->colormap);
        canvas->colormap = None;
    }
    if (canvas->win != None) {
        XWindowAttributes fa2;
        if (XGetWindowAttributes(dpy, canvas->win, &fa2)) {
            XDestroyWindow(dpy, canvas->win);
        }
        canvas->win = None;
    }

    free(canvas->path);
    free(canvas->title);

    if (active_window == canvas) active_window = NULL;

    manage_canvases(false, canvas);
    remove_icon_for_canvas(canvas);
    free(canvas);

    Canvas *desktop = get_desktop_canvas();
    if (desktop) redraw_canvas(desktop);
}

void cleanup_intuition(void) {
    if (!render_context) return;
    for (int i = 0; i < canvas_count; i++) destroy_canvas(canvas_array[i]);
    free(canvas_array); 
    canvas_array = NULL; 
    canvas_count = 0; 
    canvas_array_size = 0;

    if (root_cursor) 
        XFreeCursor(render_context->dpy, root_cursor);
    
    if (render_context->desk_img != None) 
        XFreePixmap(render_context->dpy, render_context->desk_img);

    if (render_context->wind_img != None) 
        XFreePixmap(render_context->dpy, render_context->wind_img);

    XCloseDisplay(render_context->dpy);
    free(render_context); render_context = NULL; display = NULL;
}