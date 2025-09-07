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
#include <strings.h>  // For strcasecmp
#include <stdio.h>
#include <sys/stat.h>
#include "resize.h"


#define INITIAL_CANVAS_CAPACITY 8

// Forward declarations
static bool should_skip_framing(Window win, XWindowAttributes *attrs);
static bool is_window_valid(Display *dpy, Window win);
static bool get_window_attributes_safely(Window win, XWindowAttributes *attrs);
static void calculate_content_area_inside_frame(const Canvas *canvas, int *content_width, int *content_height);
// Forward declaration - now public
void calculate_frame_size_from_client_size(int client_width, int client_height, int *frame_width, int *frame_height);
static inline void move_and_resize_frame(Canvas *c, int x, int y, int w, int h);
static void remove_canvas_from_array(Canvas *target_canvas);

// Global state for intuition
// Display, render context, and canvas registry live here so window
// management code can access them without passing through every call.
static Display *display = NULL;
static RenderContext *render_context = NULL;
Canvas **canvas_array = NULL;
int canvas_count = 0;
static int canvas_array_size = 0;
static Bool fullscreen_active = False;
static Canvas *active_window = NULL;

// suppress desktop deactivate flag
static long long g_deactivate_suppress_until_ms = 0;

// Shutdown flag to prevent X11 operations during cleanup
static bool g_shutting_down = false;
// Restart flag to preserve client windows during hot-restart
static bool g_restarting = false;

// Monotonic clock in milliseconds; used for input timing decisions
// like double-click and suppression windows.
static long long now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}


// Send close request to client window (WM_DELETE_WINDOW or XKillClient)
static bool send_close_request_to_client(Window client_window) {
    
    if (!is_window_valid(display, client_window)) {
        return false;
    }
    
    Atom wm_protocols = XInternAtom(display, "WM_PROTOCOLS", False);
    Atom wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
    Atom *protocols = NULL; 
    int protocol_count = 0;
    bool supports_delete = false;
    
    if (XGetWMProtocols(display, client_window, &protocols, &protocol_count)) {
        for (int i = 0; i < protocol_count; i++) {
            if (protocols[i] == wm_delete) { 
                supports_delete = true; 
                break; 
            }
        }
        XFree(protocols);
    }
    
    if (supports_delete) {
        XClientMessageEvent close_event = {0};
        close_event.type = ClientMessage;
        close_event.window = client_window;
        close_event.message_type = wm_protocols;
        close_event.format = 32;
        close_event.data.l[0] = wm_delete;
        close_event.data.l[1] = CurrentTime;
        XSendEvent(display, client_window, False, NoEventMask, (XEvent *)&close_event);
        XFlush(display);
        return true;
    } else {
        XKillClient(display, client_window);
        return true;
    }
}

// Handle close request for canvas - simple and forceful
static void request_client_close(Canvas *canvas) {
    if (!canvas) {
        log_error("[ERROR] request_client_close called with NULL canvas");
        return;
    }
    
    if (canvas->client_win && is_window_valid(display, canvas->client_win)) {
        // Always send close request to client - no special handling
        // If client doesn't respond, user can click again
        send_close_request_to_client(canvas->client_win);
    } else {
        // No client window - destroy the frame
        destroy_canvas(canvas);
    }
}



 

// Check if window is a top-level window (direct child of root window)
static bool is_toplevel_under_root(Window w) {
    if (w == None) {
        log_error("[ERROR] is_toplevel_under_root called with None window");
        return false;
    }
    XWindowAttributes attrs;
    if (!get_window_attributes_safely(w, &attrs)) {
        // Window no longer exists - that's a real answer, not an error
        return false;
    }
    Window root_return, parent_return, *children = NULL; unsigned int n = 0;
    bool ok = XQueryTree(display, w, &root_return, &parent_return, &children, &n);
    if (children) XFree(children);
    if (!ok) {
        log_error("[ERROR] XQueryTree failed for window 0x%lx", (unsigned long)w);
        return false;
    }
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

// Check if a window exists and can be safely accessed
static bool is_window_valid(Display *dpy, Window win) {
    if (win == None) return false;
    XWindowAttributes attrs;
    return XGetWindowAttributes(dpy, win, &attrs) == True;
}

// Get window attributes with validation - returns false if window is invalid
static bool get_window_attributes_safely(Window win, XWindowAttributes *attrs) {
    if (win == None) {
        return false;  // None window is a valid case - window doesn't exist
    }
    if (!attrs) {
        log_error("[ERROR] get_window_attributes_safely called with NULL attrs");
        return false;
    }
    return XGetWindowAttributes(display, win, attrs) == True;
}

// Send X command and ensure it completes immediately 
static void send_x_command_and_sync(void) {
    // Don't sync if we're shutting down or display is invalid
    if (g_shutting_down || !display) return;
    XSync(display, False);
}


// Forward declaration - now public, defined later
void apply_resize_and_redraw(Canvas *canvas, int new_w, int new_h);

// Move/resize a frame and update its cached geometry, then schedule
// a redraw so borders and scrollbars stay in sync.
static inline void move_and_resize_frame(Canvas *c, int x, int y, int w, int h) {
    if (!c || !is_window_valid(display, c->win)) return;
    XMoveResizeWindow(display, c->win, x, y, w, h);
    c->x = x; c->y = y;
    apply_resize_and_redraw(c, w, h);
}

// Calculate frame window size needed to contain client area with borders
void calculate_frame_size_from_client_size(int client_width, int client_height, int *frame_width, int *frame_height) {
    // Client windows use 8px left, 8px right, 20px top, 20px bottom
    if (frame_width) *frame_width = max(1, client_width) + BORDER_WIDTH_LEFT + BORDER_WIDTH_RIGHT_CLIENT;  // 8+8=16px horizontal
    if (frame_height) *frame_height = max(1, client_height) + BORDER_HEIGHT_TOP + BORDER_HEIGHT_BOTTOM;  // 20+20=40px vertical
}

// Calculate usable content area inside window frame (excluding borders)
static inline void calculate_content_area_inside_frame(const Canvas *canvas, int *content_width, int *content_height) {
    if (!canvas) { 
        if (content_width) *content_width = 0; 
        if (content_height) *content_height = 0; 
        return; 
    }
    
    if (canvas->fullscreen) {
        if (content_width) *content_width = max(1, canvas->width);
        if (content_height) *content_height = max(1, canvas->height);
        return;
    }
    
    if (content_width) *content_width = max(1, canvas->width - BORDER_WIDTH_LEFT - get_right_border_width(canvas));
    if (content_height) *content_height = max(1, canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM);
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
            if (c && (c->type == WINDOW || c->type == DIALOG) && c != current) { set_active_window(c); if (children) XFree(children); return; }
        }
    }
    // Fallback: topmost window excluding current
    for (int i = (int)n - 1; i >= 0; --i) {
        Canvas *c = find_canvas(children[i]);
        if (c && (c->type == WINDOW || c->type == DIALOG) && c != current) { set_active_window(c); break; }
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

// Old resize variables removed - now using clean resize.c module

// Canvas being scrolled
static Canvas *scrolling_canvas = NULL;

static bool scrolling_vertical = true;

static int initial_scroll = 0;

static int scroll_start_pos = 0;

// For holding arrow buttons
static Canvas *arrow_scroll_canvas = NULL;
static int arrow_scroll_direction = 0; // -1 for up/left, 1 for down/right, 0 for none
static bool arrow_scroll_vertical = true;

static bool g_debug_xerrors = false;

// Forward declare the error handler so we can install it early
static int x_error_handler(Display *dpy, XErrorEvent *error);

void begin_shutdown(void) {
    g_shutting_down = true;
}

void begin_restart(void) {
    g_restarting = true;
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

// Get window attributes or provide safe defaults if window is invalid
static bool get_window_attrs_with_defaults(Window win, XWindowAttributes *attrs) {
    if (get_window_attributes_safely(win, attrs)) return true;
    
    // Provide safe defaults for invalid windows
    *attrs = (XWindowAttributes){
        .x = 200, .y = 200, .width = 400, .height = 300,
        .override_redirect = False, .class = InputOutput, .border_width = 0
    };
    return false;
}

// Add new canvas to the global array, expanding if needed
static Canvas *add_new_canvas_to_array(void) {
    // Expand array if needed
    if (canvas_count >= canvas_array_size) {
        int new_size = canvas_array_size ? canvas_array_size * 2 : INITIAL_CANVAS_CAPACITY;
        Canvas **expanded_array = realloc(canvas_array, new_size * sizeof(Canvas *));
        if (!expanded_array) {
            log_error("[ERROR] realloc failed for canvas_array (new size=%d)", new_size);
            exit(1);
        }
        canvas_array = expanded_array;
        canvas_array_size = new_size;
    }
    
    // Allocate new canvas
    Canvas *new_canvas = malloc(sizeof(Canvas));
    if (!new_canvas) {
        log_error("[ERROR] malloc failed for Canvas structure (size=%zu)", sizeof(Canvas));
        return NULL;
    }
    
    canvas_array[canvas_count++] = new_canvas;
    return new_canvas;
}

// Remove canvas from global array and compact the array
static void remove_canvas_from_array(Canvas *target_canvas) {
    for (int i = 0; i < canvas_count; i++) {
        if (canvas_array[i] == target_canvas) {
            // Shift remaining canvases down to fill the gap
            memmove(&canvas_array[i], &canvas_array[i + 1], (canvas_count - i - 1) * sizeof(Canvas *));
            canvas_count--;
            break;
        }
    }
}

// Manage canvas array - either add new canvas or remove existing one
Canvas *manage_canvases(bool should_add_canvas, Canvas *canvas_to_remove) {
    if (should_add_canvas) return add_new_canvas_to_array();
    if (canvas_to_remove) remove_canvas_from_array(canvas_to_remove);
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
        log_error("[WARNING] XRANDR extension not available; resolution changes may not be handled.");
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

// Choose appropriate visual and depth for different canvas types
static void choose_visual_for_canvas_type(CanvasType canvas_type, XVisualInfo *visual_info) {
    if (canvas_type == DESKTOP) { 
        visual_info->visual = DefaultVisual(display, screen); 
        visual_info->depth = DefaultDepth(display, screen); 
    } else if (!XMatchVisualInfo(display, screen, GLOBAL_DEPTH, TrueColor, visual_info)) { 
        visual_info->visual = DefaultVisual(display, screen); 
        visual_info->depth = DefaultDepth(display, screen); 
    }
    XMatchVisualInfo(display, screen, visual_info->depth, TrueColor, visual_info);
}

// Get X11 event mask appropriate for each canvas type
static long get_event_mask_for_canvas_type(CanvasType canvas_type) {
    long base_events = ExposureMask | ButtonPressMask | PointerMotionMask | ButtonReleaseMask | KeyPressMask;
    
    if (canvas_type == DESKTOP) 
        return base_events | SubstructureRedirectMask | SubstructureNotifyMask;
    if (canvas_type == WINDOW)  
        return base_events | StructureNotifyMask | SubstructureNotifyMask | EnterWindowMask | FocusChangeMask;
    if (canvas_type == MENU)    
        return base_events | PointerMotionMask | ButtonPressMask | ButtonReleaseMask;
    
    return base_events;
}

static bool should_frame_window(Window w, XWindowAttributes *a) {
    if (!get_window_attributes_safely(w, a)) return false;
    if (a->map_state != IsViewable || a->class == InputOnly) return false;
    if (should_skip_framing(w, a)) return false;
    return true;
}
static bool init_render_context(void) {
    render_context = malloc(sizeof(RenderContext)); if (!render_context) return false;
    render_context->dpy = display;
    XVisualInfo vinfo; XMatchVisualInfo(display, screen, depth, TrueColor, &vinfo);
    render_context->fmt = XRenderFindVisualFormat(display, vinfo.visual);
    render_context->desk_img = None; render_context->wind_img = None;
    render_context->desk_picture = None; render_context->wind_picture = None;
    render_context->checker_active_pixmap = None; render_context->checker_active_picture = None;
    render_context->checker_inactive_pixmap = None; render_context->checker_inactive_picture = None;
    // Cache frequently used default values to avoid repeated lookups
    render_context->default_screen = DefaultScreen(display);
    render_context->default_visual = DefaultVisual(display, render_context->default_screen);
    render_context->default_colormap = DefaultColormap(display, render_context->default_screen);
    return true;
}

void apply_resize_and_redraw(Canvas *c, int nw, int nh) {
    if (!c) return;
    if (c->width == nw && c->height == nh) return;
    c->width = nw;
    c->height = nh; 
    
    // Skip expensive buffer recreation during interactive resize
    if (!c->resizing_interactive) {
        render_recreate_canvas_surfaces(c);
    }
    
    if (c->client_win != None) { 
        int client_width, client_height; 
        calculate_content_area_inside_frame(c, &client_width, &client_height);
        
        // MUST also set position to ensure client stays within borders!
        XWindowChanges ch = { 
            .x = BORDER_WIDTH_LEFT, 
            .y = BORDER_HEIGHT_TOP,
            .width = client_width, 
            .height = client_height 
        };
        XConfigureWindow(display, c->client_win, CWX | CWY | CWWidth | CWHeight, &ch);
    } else if (c->type == WINDOW) compute_max_scroll(c);
    
    // Always redraw to show resize visually
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

void deactivate_all_windows(void) {
    for (int i = 0; i < canvas_count; i++) { Canvas *c = canvas_array[i];
        if (c->type == WINDOW || c->type == DIALOG) { c->active = false; redraw_canvas(c); } }
    active_window = NULL;
    
    // No window active - restore system menus
    restore_system_menu();
}

static void init_canvas_metadata(Canvas *c, const char *path, CanvasType t,
                                 int x, int y, int w, int h) {
    *c = (Canvas){0}; c->type = t; c->path = path ? strdup(path) : NULL;
    c->title_base = path ? strdup(strrchr(path, '/') ? strrchr(path, '/') + 1 : path) : NULL;
    if (c->title_base && strlen(c->title_base) == 0) {
        free(c->title_base);  // Free the empty string before replacing
        c->title_base = strdup("System");
    }
    c->title_change = NULL;  // Workbench windows don't use dynamic titles
    c->x = x; c->y = (t == WINDOW) ? max(y, MENUBAR_HEIGHT) : y;
    c->width = w; c->height = h; 
    // Set default background color
    c->bg_color = GRAY;
    c->buffer_width = w; c->buffer_height = h;  // Initialize to canvas size, may be enlarged later
    c->resizing_interactive = false;
    // Use global show_hidden state for new windows
    c->show_hidden = get_global_show_hidden_state();
    // Initialize damage tracking - mark entire canvas as needing initial draw
    c->needs_redraw = true;
    c->dirty_x = 0;
    c->dirty_y = 0;
    c->dirty_w = w;
    c->dirty_h = h;
}

static Bool setup_visual_and_window(Canvas *c, CanvasType t,
                                    int x, int y, int w, int h) {
    RenderContext *ctx = get_render_context();
    if (!ctx) return False;

    // Choose a visual/depth suitable for the canvas type
    XVisualInfo vinfo; choose_visual_for_canvas_type(t, &vinfo);
    c->visual = vinfo.visual;
    c->depth  = vinfo.depth;

    // Create the X window
    // XSetWindowAttributes: Structure to configure window properties
    XSetWindowAttributes attrs = (XSetWindowAttributes){0};
    // XCreateColormap: Allocates a color palette for this window
    // A colormap maps pixel values to actual RGB colors on screen
    // AllocNone means we don't pre-allocate any specific colors
    attrs.colormap          = XCreateColormap(ctx->dpy, root, c->visual, AllocNone);
    attrs.border_pixel      = 0;      // Border color (0 = black)
    attrs.background_pixel  = 0;      // Background color (0 = black)
    attrs.background_pixmap = None;   // No background image
    // mask: Tells X11 which attributes we're actually setting
    // CW = "Change Window" - each flag indicates an attribute to use
    unsigned long mask = CWColormap | CWBorderPixel | CWBackPixel | CWBackPixmap;

    int win_x = (t == DESKTOP) ? 0 : x;
    int win_y = (t == DESKTOP) ? MENUBAR_HEIGHT : y;
    int win_w = w;
    int win_h = (t == DESKTOP) ? (h - MENUBAR_HEIGHT) : h;

    // XCreateWindow: Actually creates the window in X11
    // Parameters: display, parent window, x, y, width, height,
    //            border width, color depth, window class, visual, attribute mask, attributes
    // InputOutput means this window can both display content and receive input
    c->win = XCreateWindow(display, root, win_x, win_y, win_w, win_h,
                           0, vinfo.depth, InputOutput, c->visual, mask, &attrs);
    if (c->win == None) {
        log_error("[ERROR] XCreateWindow failed for frame at %d,%d size %dx%d", win_x, win_y, win_w, win_h);
        return False;
    }
    c->colormap = attrs.colormap;

    // No internal tagging

    // Select input events
    XSelectInput(ctx->dpy, c->win, get_event_mask_for_canvas_type(t));

    // Backing pixmap for offscreen rendering
    c->canvas_buffer = XCreatePixmap(ctx->dpy, c->win, w, h, vinfo.depth);
    if (c->canvas_buffer == None) {
        log_error("[ERROR] XCreatePixmap failed for canvas buffer %dx%d depth=%d", w, h, vinfo.depth);
        XDestroyWindow(ctx->dpy, c->win);
        return False;
    }
    return True;
}

static Bool init_render_pictures(Canvas *c, CanvasType t) {
    RenderContext *ctx = get_render_context(); if (!ctx) return False;
    // XRenderFindVisualFormat: Get the pixel format for our visual
    // This tells XRender how to interpret the pixel data (RGB layout, alpha channel, etc.)
    XRenderPictFormat *fmt = XRenderFindVisualFormat(ctx->dpy, c->visual); if (!fmt) return False;
    // XRenderCreatePicture: Create a "Picture" - XRender's drawable surface
    // Unlike raw pixmaps, Pictures support alpha blending and transformations
    // This one is for our off-screen buffer where we compose the window content
    c->canvas_render = XRenderCreatePicture(ctx->dpy, c->canvas_buffer, fmt, 0, NULL); if (!c->canvas_render) return False;
    // Get the visual for the actual window (may differ from buffer visual)
    Visual *wv = (t == DESKTOP) ? ctx->default_visual : c->visual;
    XRenderPictFormat *wfmt = XRenderFindVisualFormat(ctx->dpy, wv); if (!wfmt) return False;
    // Create Picture for the actual window - this is what gets displayed on screen
    // We composite from canvas_render (buffer) to window_render (screen)
    c->window_render = XRenderCreatePicture(ctx->dpy, c->win, wfmt, 0, NULL); 
    if (!c->window_render) return False;
    
    // Pre-allocate commonly used Xft colors to avoid repeated allocation in render loops
    if (!c->xft_colors_allocated) {
        XftColorAllocValue(ctx->dpy, c->visual, c->colormap, &BLACK, &c->xft_black);
        XftColorAllocValue(ctx->dpy, c->visual, c->colormap, &WHITE, &c->xft_white);
        XftColorAllocValue(ctx->dpy, c->visual, c->colormap, &BLUE, &c->xft_blue);
        XftColorAllocValue(ctx->dpy, c->visual, c->colormap, &GRAY, &c->xft_gray);
        c->xft_colors_allocated = true;
    }
    
    return True;
}

static Canvas *frame_client_window(Window client, XWindowAttributes *attrs) {
    if (!attrs) return NULL;
    // Calculate frame position from client position (subtract decorations)
    int fx = attrs->x - BORDER_WIDTH_LEFT;
    int fy = attrs->y - BORDER_HEIGHT_TOP;
    
    // Only adjust if window would be completely off-screen or under menubar
    if (fx + attrs->width < 50) fx = 0;  // Too far left, bring back
    if (fy < MENUBAR_HEIGHT) fy = MENUBAR_HEIGHT;  // Don't go under menubar
    
    int fw, fh;
    calculate_frame_size_from_client_size(attrs->width, attrs->height, &fw, &fh);
    
    
    // Use create_canvas_with_client to set client_win BEFORE any rendering
    Canvas *frame = create_canvas_with_client(NULL, fx, fy, fw, fh, WINDOW, client);
    if (!frame) return NULL;
    
    // Check if this is a transient window (modal dialog) and mark it
    Window transient_for = None;
    if (XGetTransientForHint(display, client, &transient_for)) {
        frame->is_transient = true;
        frame->transient_for = transient_for;
        
        // Force transient dialogs to center of screen - don't trust GTK's position
        int screen_width = DisplayWidth(display, DefaultScreen(display));
        int screen_height = DisplayHeight(display, DefaultScreen(display));
        fx = (screen_width - fw) / 2;
        fy = (screen_height - fh) / 2;
        if (fy < MENUBAR_HEIGHT) fy = MENUBAR_HEIGHT;
        
        // Move the frame to the forced position AND update canvas position
        XMoveWindow(display, frame->win, fx, fy);
        frame->x = fx;
        frame->y = fy;
        
        // Get window class and name for debugging
        XClassHint class_hint;
        if (XGetClassHint(display, client, &class_hint)) {
            if (class_hint.res_name) XFree(class_hint.res_name);
            if (class_hint.res_class) XFree(class_hint.res_class);
        }
        
        // Check window protocols
        Atom *protocols;
        int count;
        if (XGetWMProtocols(display, client, &protocols, &count)) {
            for (int i = 0; i < count; i++) {
                char *atom_name = XGetAtomName(display, protocols[i]);
                if (atom_name) {
                    XFree(atom_name);
                }
            }
            XFree(protocols);
        }
    } else {
        frame->is_transient = false;
        frame->transient_for = None;
    }
    
    // XReparentWindow: Move a window to become a child of another window
    // This is the core of window management - we take the client's window
    // and place it inside our frame window at the specified offset
    XReparentWindow(display, client, frame->win, BORDER_WIDTH_LEFT, BORDER_HEIGHT_TOP);
    // XSelectInput: Tell X11 which events we want to receive for this window
    // StructureNotifyMask: Get notified when window is resized, moved, etc.
    // PropertyChangeMask: Get notified when window properties change (like title)
    XSelectInput(display, client, StructureNotifyMask | PropertyChangeMask);
    // XGrabButton: Intercept mouse clicks on the client window
    // This lets us activate the frame when user clicks inside the client area
    // Without this, clicks would go straight to the client, bypassing our WM
    unsigned int mods[] = { AnyModifier };  // Catch clicks regardless of modifier keys
    int buttons[] = { Button1, Button2, Button3 };  // Left, middle, right mouse buttons
    for (unsigned int mi = 0; mi < sizeof(mods)/sizeof(mods[0]); ++mi) {
        for (unsigned int bi = 0; bi < sizeof(buttons)/sizeof(buttons[0]); ++bi) {
            XGrabButton(display,
                        buttons[bi], mods[mi],
                        client,
                        True,                    // owner_events: pass events to client after we process
                        ButtonPressMask,         // event mask: only care about button presses
                        GrabModeSync,            // sync: freeze mouse until we XAllowEvents
                        GrabModeAsync,           // async: don't freeze keyboard
                        None, None);             // no cursor change, no confine window
        }
    }
    if (attrs->border_width != 0) { XWindowChanges b = { .border_width = 0 }; XConfigureWindow(display, client, CWBorderWidth, &b); }
    // client_win already set earlier to ensure correct border rendering
    
    // Read window size hints from client
    if (frame->client_win != None) {
        // Get desktop dimensions for max constraints
        int screen_width = DisplayWidth(display, DefaultScreen(display));
        int screen_height = DisplayHeight(display, DefaultScreen(display));
        int max_frame_width = screen_width;
        int max_frame_height = screen_height - MENUBAR_HEIGHT;
        
        XSizeHints *hints = XAllocSizeHints();
        long supplied_hints;
        if (hints && XGetWMNormalHints(display, client, hints, &supplied_hints)) {
            
            // Apply minimum size constraints per ICCCM (like dwm does)
            // First determine base size
            int base_width = 0, base_height = 0;
            if (hints->flags & PBaseSize) {
                base_width = hints->base_width;
                base_height = hints->base_height;
            } else if (hints->flags & PMinSize) {
                base_width = hints->min_width;
                base_height = hints->min_height;
            }
            
            // Then determine minimum size
            int min_width = 0, min_height = 0;
            if (hints->flags & PMinSize) {
                min_width = hints->min_width;
                min_height = hints->min_height;
            } else if (hints->flags & PBaseSize) {
                min_width = hints->base_width;
                min_height = hints->base_height;
            }
            
            // Use the LARGER of base and min for actual minimum (ICCCM compliance)
            int actual_min_width = (base_width > min_width) ? base_width : min_width;
            int actual_min_height = (base_height > min_height) ? base_height : min_height;
            
            if (actual_min_width > 0 || actual_min_height > 0) {
                frame->min_width = actual_min_width + BORDER_WIDTH_LEFT + BORDER_WIDTH_RIGHT_CLIENT;
                frame->min_height = actual_min_height + BORDER_HEIGHT_TOP + BORDER_HEIGHT_BOTTOM;
            } else {
                frame->min_width = 150;  // Default minimum
                frame->min_height = 150;
            }
            
            // Apply maximum size constraints (add frame decorations to client maximums)
            if (hints->flags & PMaxSize) {
                frame->max_width = hints->max_width + BORDER_WIDTH_LEFT + BORDER_WIDTH_RIGHT_CLIENT;
                frame->max_height = hints->max_height + BORDER_HEIGHT_TOP + BORDER_HEIGHT_BOTTOM;
            } else {
                frame->max_width = max_frame_width;
                frame->max_height = max_frame_height;
            }
            
            // Ensure max doesn't exceed desktop
            if (frame->max_width > max_frame_width || frame->max_width == 0) {
                frame->max_width = max_frame_width;
            }
            if (frame->max_height > max_frame_height || frame->max_height == 0) {
                frame->max_height = max_frame_height;
            }
            
            // Check if window wants fixed size (min == max)
            frame->resize_x_allowed = (frame->min_width != frame->max_width);
            frame->resize_y_allowed = (frame->min_height != frame->max_height);
            XFree(hints);
        } else {
            // No hints provided - use sensible defaults
            frame->min_width = 150;
            frame->min_height = 150;
            frame->max_width = max_frame_width;
            frame->max_height = max_frame_height;
            frame->resize_x_allowed = true;
            frame->resize_y_allowed = true;
        }
        
        // If initial window is larger than max, shrink it
        if (frame->width > frame->max_width) {
            frame->width = frame->max_width;
            XResizeWindow(display, frame->win, frame->width, frame->height);
        }
        if (frame->height > frame->max_height) {
            frame->height = frame->max_height;
            XResizeWindow(display, frame->win, frame->width, frame->height);
        }
        
        XClassHint ch;
        if (XGetClassHint(display, frame->client_win, &ch)) {
            // Use res_class as the title (it's usually the proper app name)
            if (ch.res_class) {
                frame->title_base = strdup(ch.res_class);
            } else if (ch.res_name) {
                frame->title_base = strdup(ch.res_name);
            } else {
                frame->title_base = strdup("NoNameApp");
            }
            frame->title_change = NULL;  // Will be set later if needed
            
            // Also set WM_CLASS on the frame for xprop
            XSetClassHint(display, frame->win, &ch);
            
            // Now free
            if (ch.res_name) XFree(ch.res_name);
            if (ch.res_class) XFree(ch.res_class);
        } else {
            frame->title_base = strdup("NoNameApp");
            frame->title_change = NULL;
        }
        
        // Check if the client has set a custom display title
        Atom amiwb_title_change = XInternAtom(display, "_AMIWB_TITLE_CHANGE", False);
        Atom actual_type;
        int actual_format;
        unsigned long nitems, bytes_after;
        unsigned char *prop_data = NULL;
        
        if (XGetWindowProperty(display, client, amiwb_title_change,
                              0, 256, False, AnyPropertyType,
                              &actual_type, &actual_format,
                              &nitems, &bytes_after, &prop_data) == Success) {
            if (prop_data && nitems > 0) {
                // Client has specified a display title
                frame->title_change = strndup((char *)prop_data, nitems);
                XFree(prop_data);
            }
        }
    }
    
    XAddToSaveSet(display, client); 
    return frame;
}

// Frame all existing client windows that need management
static void frame_existing_client_windows(void) {
    Window root_window, parent_window, *child_windows = NULL; 
    unsigned int child_count = 0;
    
    if (!XQueryTree(display, root, &root_window, &parent_window, &child_windows, &child_count)) return;
    
    for (unsigned int i = 0; i < child_count; i++) {
        Window client_window = child_windows[i];
        
        // Skip windows we already manage
        bool already_managed = false;
        for (int j = 0; j < canvas_count; j++) {
            if (canvas_array[j]->win == client_window || canvas_array[j]->client_win == client_window) {
                already_managed = true; 
                break;
            }
        }
        if (already_managed) continue;
        
        // Frame this window if it needs management
        XWindowAttributes window_attrs;
        if (!should_frame_window(client_window, &window_attrs)) continue;
        
        Canvas *new_frame = frame_client_window(client_window, &window_attrs);
        if (new_frame) {
            raise_window(new_frame);
            redraw_canvas(new_frame);
        }
    }
    
    if (child_windows) XFree(child_windows);
}

Canvas *init_intuition(void) {
    if (!init_display_and_root() || !init_render_context()) return NULL;
    
    Canvas *desktop = create_canvas(getenv("HOME"), 0, 20, width, height, DESKTOP);
    if (!desktop) return NULL;
    
    // Set WM_CLASS for the desktop window so xprop can detect it
    XClassHint desktop_class;
    desktop_class.res_name = "workbench";
    desktop_class.res_class = "AmiWB";
    XSetClassHint(display, desktop->win, &desktop_class);
    
    // Setup Imlib2 for image loading
    imlib_context_set_display(display);
    imlib_context_set_visual(desktop->visual);
    imlib_context_set_colormap(desktop->colormap);
    imlib_set_cache_size(0);
    render_load_wallpapers();
    
    // Frame any existing client windows
    frame_existing_client_windows();
    
    send_x_command_and_sync(); 
    redraw_canvas(desktop); 
    return desktop;
}

static void init_scroll(Canvas *canvas) {
    if (!canvas || canvas->type != WINDOW) return;
    canvas->scroll_x = 0;
    canvas->scroll_y = 0;
    calculate_content_area_inside_frame(canvas, &canvas->content_width, &canvas->content_height);
    compute_max_scroll(canvas);
}

// Minimal implementations to satisfy external references
Canvas *get_desktop_canvas(void) {
    return canvas_count > 0 ? canvas_array[0] : NULL;
}

// Find canvas by either frame window or client window
static Canvas *find_canvas_by_any_window(Window search_window, bool check_client_windows) {
    for (int i = 0; i < canvas_count; i++) {
        Canvas *canvas = canvas_array[i];
        if (canvas->win == search_window) return canvas;
        if (check_client_windows && canvas->client_win == search_window) return canvas;
    }
    return NULL;
}

Canvas *find_canvas(Window frame_window) {
    return find_canvas_by_any_window(frame_window, false);
}

Canvas *find_canvas_by_client(Window client_window) {
    return find_canvas_by_any_window(client_window, true);
}

void set_active_window(Canvas *c) {
    if (!c || (c->type != WINDOW && c->type != DIALOG)) return;
    for (int i = 0; i < canvas_count; i++) {
        Canvas *o = canvas_array[i];
        if ((o->type == WINDOW || o->type == DIALOG) && o != c) {
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
    
    // Check for app menus on client windows
    if (c->client_win != None) {
        check_for_app_menus(c->client_win);
    } else {
        // Workbench window - restore system menus
        restore_system_menu();
    }
    
    XSync(display, False);
}

Canvas *get_active_window(void) {
    return active_window;
}

void cycle_next_window(void) {
    // Build list of eligible windows (WINDOW and DIALOG types)
    Canvas *windows[256];  // Max 256 windows should be enough
    int window_count = 0;
    int current_index = -1;
    
    for (int i = 0; i < canvas_count; i++) {
        Canvas *c = canvas_array[i];
        if (c && (c->type == WINDOW || c->type == DIALOG)) {
            if (c == active_window) {
                current_index = window_count;
            }
            windows[window_count++] = c;
            if (window_count >= 256) break;  // Safety limit
        }
    }
    
    // Need at least 2 windows to cycle
    if (window_count < 2) return;
    
    // Calculate next index (wrap around)
    int next_index = (current_index + 1) % window_count;
    Canvas *next_window = windows[next_index];
    
    // Check if window is iconified (not visible)
    XWindowAttributes attrs;
    if (XGetWindowAttributes(display, next_window->win, &attrs)) {
        if (attrs.map_state != IsViewable) {
            // Window is iconified - find and restore it
            FileIcon **icon_array = get_icon_array();
            int icon_count = get_icon_count();
            for (int i = 0; i < icon_count; i++) {
                FileIcon *ic = icon_array[i];
                if (ic && ic->type == TYPE_ICONIFIED && ic->iconified_canvas == next_window) {
                    restore_iconified(ic);
                    return;
                }
            }
        }
    }
    
    // Window is visible - activate it
    set_active_window(next_window);
    XRaiseWindow(display, next_window->win);
}

void cycle_prev_window(void) {
    // Build list of eligible windows (WINDOW and DIALOG types)
    Canvas *windows[256];  // Max 256 windows should be enough
    int window_count = 0;
    int current_index = -1;
    
    for (int i = 0; i < canvas_count; i++) {
        Canvas *c = canvas_array[i];
        if (c && (c->type == WINDOW || c->type == DIALOG)) {
            if (c == active_window) {
                current_index = window_count;
            }
            windows[window_count++] = c;
            if (window_count >= 256) break;  // Safety limit
        }
    }
    
    // Need at least 2 windows to cycle
    if (window_count < 2) return;
    
    // Calculate previous index (wrap around)
    int prev_index = (current_index - 1 + window_count) % window_count;
    Canvas *prev_window = windows[prev_index];
    
    // Check if window is iconified (not visible)
    XWindowAttributes attrs;
    if (XGetWindowAttributes(display, prev_window->win, &attrs)) {
        if (attrs.map_state != IsViewable) {
            // Window is iconified - find and restore it
            FileIcon **icon_array = get_icon_array();
            int icon_count = get_icon_count();
            for (int i = 0; i < icon_count; i++) {
                FileIcon *ic = icon_array[i];
                if (ic && ic->type == TYPE_ICONIFIED && ic->iconified_canvas == prev_window) {
                    restore_iconified(ic);
                    return;
                }
            }
        }
    }
    
    // Window is visible - activate it
    set_active_window(prev_window);
    XRaiseWindow(display, prev_window->win);
}

// Get list of all windows (WINDOW and DIALOG types)
// Returns count, and fills windows array with pointers
int get_window_list(Canvas ***windows) {
    static Canvas *window_list[256];  // Static to avoid allocation
    int count = 0;
    
    for (int i = 0; i < canvas_count; i++) {
        Canvas *c = canvas_array[i];
        if (c && (c->type == WINDOW || c->type == DIALOG)) {
            window_list[count++] = c;
            if (count >= 256) break;  // Safety limit
        }
    }
    
    if (windows) *windows = window_list;
    return count;
}

// Activate window by its index in canvas_array
void activate_window_by_index(int index) {
    if (index < 0 || index >= canvas_count) return;
    Canvas *c = canvas_array[index];
    if (!c || (c->type != WINDOW && c->type != DIALOG)) return;
    
    // Check if window is iconified (not visible)
    XWindowAttributes attrs;
    if (XGetWindowAttributes(display, c->win, &attrs)) {
        if (attrs.map_state != IsViewable) {
            // Window is iconified - find and restore it
            FileIcon **icon_array = get_icon_array();
            int icon_count = get_icon_count();
            for (int i = 0; i < icon_count; i++) {
                FileIcon *ic = icon_array[i];
                if (ic && ic->type == TYPE_ICONIFIED && ic->iconified_canvas == c) {
                    restore_iconified(ic);
                    return;
                }
            }
        }
    }
    
    // Window is visible - activate it
    set_active_window(c);
    XRaiseWindow(display, c->win);
}

// Iconify all windows (show desktop)
void iconify_all_windows(void) {
    // Build list of windows to iconify (can't modify canvas_array while iterating)
    Canvas *to_iconify[256];
    int count = 0;
    
    for (int i = 0; i < canvas_count; i++) {
        Canvas *c = canvas_array[i];
        if (c && (c->type == WINDOW || c->type == DIALOG)) {
            // Check if window is visible
            XWindowAttributes attrs;
            if (XGetWindowAttributes(display, c->win, &attrs)) {
                if (attrs.map_state == IsViewable) {
                    to_iconify[count++] = c;
                    if (count >= 256) break;
                }
            }
        }
    }
    
    // Now iconify all the windows
    for (int i = 0; i < count; i++) {
        iconify_canvas(to_iconify[i]);
    }
}

void compute_max_scroll(Canvas *c) {
    if (!c) return;
    int content_width, content_height;
    calculate_content_area_inside_frame(c, &content_width, &content_height);
    c->max_scroll_x = max(0, c->content_width - content_width);
    c->max_scroll_y = max(0, c->content_height - content_height);
    c->scroll_x = min(c->scroll_x, c->max_scroll_x);
    c->scroll_y = min(c->scroll_y, c->max_scroll_y);
}

void update_canvas_max_constraints(void) {
    int screen_width = DisplayWidth(display, DefaultScreen(display));
    int screen_height = DisplayHeight(display, DefaultScreen(display));
    
    for (int i = 0; i < canvas_count; i++) {
        Canvas *c = canvas_array[i];
        if (c->type == WINDOW) {
            // Update max constraints but don't resize windows
            // Frame maximums are desktop size minus menubar
            c->max_width = screen_width;
            c->max_height = screen_height - MENUBAR_HEIGHT;
        }
    }
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



void iconify_canvas(Canvas *c) {
    if (!c || (c->type != WINDOW && c->type != DIALOG)) return;
    
    // Create the iconified icon (all icon logic is now in workbench.c)
    FileIcon *icon = create_iconified_icon(c);
    if (!icon) {
        log_error("[ERROR] Failed to create iconified icon - cannot iconify window");
        return;
    }
    
    // Hide the window (this is the window management part that stays in intuition.c)
    XUnmapWindow(display, c->win);
    if (active_window == c) active_window = NULL;
    
    // Refresh the desktop to show the new icon
    Canvas *desk = get_desktop_canvas();
    if (desk) {
        redraw_canvas(desk);
    }
    send_x_command_and_sync();
}

// Create new window
// Original create_canvas - delegates to create_canvas_with_client
Canvas *create_canvas(const char *path, int x, int y, int width, 
        int height, CanvasType type) {
    return create_canvas_with_client(path, x, y, width, height, type, None);
}

// Helper function for creating canvas with client window already set
Canvas *create_canvas_with_client(const char *path, int x, int y, int width, 
        int height, CanvasType type, Window client_win) {
    
    RenderContext *ctx = get_render_context();
    if (!ctx) return NULL;

    Canvas *canvas = manage_canvases(true, NULL);
    if (!canvas) return NULL;
    init_canvas_metadata(canvas, path, type, x, y, width, height);
    
    // Set client_win immediately to prevent wrong rendering
    canvas->client_win = client_win;
    // Initialize all button armed states
    canvas->close_armed = false;
    canvas->iconify_armed = false;
    canvas->maximize_armed = false;
    canvas->lower_armed = false;
    canvas->v_arrow_up_armed = false;
    canvas->v_arrow_down_armed = false;
    canvas->h_arrow_left_armed = false;
    canvas->h_arrow_right_armed = false;
    canvas->resize_armed = false;
    // Initialize other states
    canvas->is_transient = false;
    canvas->transient_for = None;
    canvas->close_request_sent = false;
    canvas->consecutive_unmaps = 0;
    canvas->cleanup_scheduled = false;
    canvas->disable_scrollbars = false;
    // Initialize maximize toggle support
    canvas->maximized = false;
    canvas->pre_max_x = 0;
    canvas->pre_max_y = 0;
    canvas->pre_max_w = 0;
    canvas->pre_max_h = 0;
    
    // Initialize window size constraints
    int screen_width = DisplayWidth(display, DefaultScreen(display));
    int screen_height = DisplayHeight(display, DefaultScreen(display));
    canvas->min_width = 150;  // Default minimum
    canvas->min_height = 150;
    canvas->max_width = screen_width;  // Workbench windows can use full width
    canvas->max_height = screen_height - MENUBAR_HEIGHT;  // But limited by menubar
    canvas->resize_x_allowed = true;
    canvas->resize_y_allowed = true;

    if (!setup_visual_and_window(canvas, type, x, y, width, height)) {
        destroy_canvas(canvas);
        return NULL;
    }

    if (!init_render_pictures(canvas, type)) {
        destroy_canvas(canvas);
        return NULL;
    }
    
    // Create XftDraw and ensure all render surfaces are properly initialized
    render_recreate_canvas_surfaces(canvas);

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
    
    // Frame all normal client windows including transient windows (modal dialogs)
    // Transient windows will be marked and handled specially in the Canvas structure
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
    // Check for IPC from ReqASL to open directory
    static Atom amiwb_open_dir = None;
    if (amiwb_open_dir == None) {
        amiwb_open_dir = XInternAtom(display, "AMIWB_OPEN_DIRECTORY", False);
    }
    
    if (event->atom == amiwb_open_dir && event->window == root) {
        // Read the path from the property
        Atom actual_type;
        int actual_format;
        unsigned long nitems, bytes_after;
        unsigned char *data = NULL;
        
        if (XGetWindowProperty(display, root, amiwb_open_dir,
                              0, PATH_SIZE, True, XA_STRING,
                              &actual_type, &actual_format,
                              &nitems, &bytes_after, &data) == Success) {
            if (data && nitems > 0) {
                // Open the directory
                workbench_open_directory((char *)data);
                XFree(data);
            }
        }
        return;
    }
    
    // Check for title change property from clients (e.g., ReqASL)
    static Atom amiwb_title_change = None;
    if (amiwb_title_change == None) {
        amiwb_title_change = XInternAtom(display, "AMIWB_TITLE_CHANGE", False);
    }
    
    if (event->atom == amiwb_title_change) {
        // Find the canvas by client window
        Canvas *canvas = find_canvas_by_client(event->window);
        if (canvas) {
            // Property was changed, read the new value
            Atom actual_type;
            int actual_format;
            unsigned long nitems, bytes_after;
            unsigned char *prop_data = NULL;
            
            if (XGetWindowProperty(display, event->window, amiwb_title_change,
                                  0, 256, False, XA_STRING,
                                  &actual_type, &actual_format,
                                  &nitems, &bytes_after, &prop_data) == Success) {
                
                // Update the canvas title_change
                if (canvas->title_change) {
                    free(canvas->title_change);
                    canvas->title_change = NULL;
                }
                
                if (prop_data && nitems > 0) {
                    canvas->title_change = strndup((char *)prop_data, nitems);
                    XFree(prop_data);
                    
                    // Trigger a redraw of the canvas to show the new title
                    redraw_canvas(canvas);
                }
            }
        }
        return;
    }
    
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
    // Clear maximized state when manually moving
    c->maximized = false;
    return True;
}

static inline Bool begin_frame_resize(Canvas *c, XButtonEvent *e) {
    // Use the new clean resize module
    resize_begin(c, e->x_root, e->y_root);
    // Clear maximized state when manually resizing
    c->maximized = false;
    return True;
}

enum { SCROLL_STEP = 20, ARROW_SIZE = 20, TRACK_MARGIN = 10, TRACK_RESERVED = 54 };

// Keep values within reasonable bounds
static inline int clamp_value_between(int value, int minimum, int maximum) {
    if (value < minimum) return minimum; 
    if (value > maximum) return maximum; 
    return value;
}

// Calculate scrollbar track area (where the draggable knob moves)
static inline void get_scrollbar_track_area(Canvas *canvas, bool is_vertical, int *x, int *y, int *width, int *height) {
    if (is_vertical) {
        *x = canvas->width - BORDER_WIDTH_RIGHT;
        *y = BORDER_HEIGHT_TOP + TRACK_MARGIN;
        *width = BORDER_WIDTH_RIGHT;
        *height = (canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM) - TRACK_RESERVED - TRACK_MARGIN;
    } else {
        *x = BORDER_WIDTH_LEFT + TRACK_MARGIN;
        *y = canvas->height - BORDER_HEIGHT_BOTTOM;
        *width = (canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT) - TRACK_RESERVED - TRACK_MARGIN;
        *height = BORDER_HEIGHT_BOTTOM;
    }
}

// Calculate size of draggable knob based on content-to-track ratio
static inline int calculate_scrollbar_knob_size(int track_length, int content_length) {
    float size_ratio = (float)track_length / (float)content_length;
    int knob_size = (int)(size_ratio * track_length);
    if (knob_size < MIN_KNOB_SIZE) knob_size = MIN_KNOB_SIZE;
    if (knob_size > track_length) knob_size = track_length;
    return knob_size;
}

// Convert scroll position to knob position within track
static inline int calculate_knob_position_from_scroll(int track_length, int knob_length, int scroll_amount, int max_scroll) {
    if (max_scroll <= 0) return 0;
    float position_ratio = (float)scroll_amount / (float)max_scroll;
    int available_space = track_length - knob_length;
    if (available_space <= 0) return 0;
    return (int)(position_ratio * available_space);
}

// Convert mouse click position to scroll amount
static inline int calculate_scroll_from_mouse_click(int track_start, int track_length, int max_scroll, int click_position) {
    float click_ratio = (float)(click_position - track_start) / (float)track_length;
    int scroll_value = (int)(click_ratio * (float)max_scroll);
    return clamp_value_between(scroll_value, 0, max_scroll);
}

// Start scrollbar dragging operation  
static inline void start_scrollbar_dragging(Canvas *canvas, bool is_vertical, int scroll_value, int mouse_start_pos) {
    scrolling_canvas = canvas; 
    scrolling_vertical = is_vertical;
    initial_scroll = scroll_value; 
    scroll_start_pos = mouse_start_pos;
}

// Handle mouse wheel scrolling with shift key support
static Bool handle_mouse_wheel_scrolling(Canvas *canvas, XButtonEvent *event) {
    if (event->button != 4 && event->button != 5) return False;
    if (canvas->max_scroll_y <= 0 && canvas->max_scroll_x <= 0) return False;
    
    bool scroll_up = (event->button == 4);
    bool has_shift_key = (event->state & ShiftMask);
    
    if (has_shift_key && canvas->max_scroll_x > 0) {
        // Shift + wheel = horizontal scroll
        int new_scroll_x = canvas->scroll_x + (scroll_up ? -SCROLL_STEP : SCROLL_STEP);
        canvas->scroll_x = clamp_value_between(new_scroll_x, 0, canvas->max_scroll_x);
    } else if (canvas->max_scroll_y > 0) {
        // Normal wheel = vertical scroll
        int new_scroll_y = canvas->scroll_y + (scroll_up ? -SCROLL_STEP : SCROLL_STEP);
        canvas->scroll_y = clamp_value_between(new_scroll_y, 0, canvas->max_scroll_y);
    }
    return True;
}

// Handle mouse clicks on scrollbar (either knob drag or track click)
static Bool handle_scrollbar_click(Canvas *canvas, XButtonEvent *event, bool is_vertical) {
    if (event->button != Button1) return False;
    
    int track_x, track_y, track_width, track_height;
    get_scrollbar_track_area(canvas, is_vertical, &track_x, &track_y, &track_width, &track_height);
    
    // Check if click is within track area
    bool click_in_track = (event->x >= track_x && event->x < track_x + track_width &&
                          event->y >= track_y && event->y < track_y + track_height);
    if (!click_in_track) return False;
    
    int track_length = is_vertical ? track_height : track_width;
    int content_length = is_vertical ? canvas->content_height : canvas->content_width;
    int current_scroll = is_vertical ? canvas->scroll_y : canvas->scroll_x;
    int max_scroll = is_vertical ? canvas->max_scroll_y : canvas->max_scroll_x;
    
    int knob_length = calculate_scrollbar_knob_size(track_length, content_length);
    int knob_position = (is_vertical ? track_y : track_x) + 
                       calculate_knob_position_from_scroll(track_length, knob_length, current_scroll, max_scroll);
    
    int click_coordinate = is_vertical ? event->y : event->x;
    bool click_on_knob = (click_coordinate >= knob_position && click_coordinate < knob_position + knob_length);
    
    if (click_on_knob) {
        // Start dragging the knob
        int root_coordinate = is_vertical ? event->y_root : event->x_root;
        start_scrollbar_dragging(canvas, is_vertical, current_scroll, root_coordinate);
    } else {
        // Click on track - jump to that position
        int track_start = is_vertical ? track_y : track_x;
        int new_scroll = calculate_scroll_from_mouse_click(track_start, track_length, max_scroll, click_coordinate);
        if (is_vertical) canvas->scroll_y = new_scroll;
        else canvas->scroll_x = new_scroll;
        redraw_canvas(canvas);
    }
    return True;
}

// Check if click is on scroll arrow buttons
static Bool handle_scroll_arrow_buttons(Canvas *canvas, XButtonEvent *event) {
    if (event->button != Button1) return False;
    
    int x = event->x;
    int y = event->y;
    int w = canvas->width;
    int h = canvas->height;
    
    // Check vertical scroll arrows (on right border)
    if (x >= w - BORDER_WIDTH_RIGHT && x < w) {
        // Up arrow area (top part of right border)
        if (y >= h - BORDER_HEIGHT_BOTTOM - 41 && y < h - BORDER_HEIGHT_BOTTOM - 21) {
            canvas->v_arrow_up_armed = true;
            redraw_canvas(canvas);
            // Set up for repeated scrolling
            arrow_scroll_canvas = canvas;
            arrow_scroll_direction = -1;
            arrow_scroll_vertical = true;
            return True;
        }
        // Down arrow area (bottom part of right border)
        else if (y >= h - BORDER_HEIGHT_BOTTOM - 21 && y < h - BORDER_HEIGHT_BOTTOM) {
            canvas->v_arrow_down_armed = true;
            redraw_canvas(canvas);
            // Set up for repeated scrolling
            arrow_scroll_canvas = canvas;
            arrow_scroll_direction = 1;
            arrow_scroll_vertical = true;
            return True;
        }
    }
    
    // Check horizontal scroll arrows (on bottom border)
    if (y >= h - BORDER_HEIGHT_BOTTOM && y < h) {
        // Left arrow area
        if (x >= w - BORDER_WIDTH_RIGHT - 42 && x < w - BORDER_WIDTH_RIGHT - 22) {
            canvas->h_arrow_left_armed = true;
            redraw_canvas(canvas);
            // Set up for repeated scrolling
            arrow_scroll_canvas = canvas;
            arrow_scroll_direction = -1;
            arrow_scroll_vertical = false;
            return True;
        }
        // Right arrow area
        else if (x >= w - BORDER_WIDTH_RIGHT - 22 && x < w - BORDER_WIDTH_RIGHT) {
            canvas->h_arrow_right_armed = true;
            redraw_canvas(canvas);
            // Set up for repeated scrolling
            arrow_scroll_canvas = canvas;
            arrow_scroll_direction = 1;
            arrow_scroll_vertical = false;
            return True;
        }
    }
    
    return False;
}

static Bool handle_scrollbars(Canvas *canvas, XButtonEvent *event) {
    if (canvas->client_win != None) return False; // no scrollbars on client windows
    if (canvas->disable_scrollbars) return False; // scrollbars disabled (e.g., for dialogs)

    // Handle mouse wheel first
    if (handle_mouse_wheel_scrolling(canvas, event)) {
        redraw_canvas(canvas);
        return True;
    }
    
    // Handle scroll arrow buttons
    if (handle_scroll_arrow_buttons(canvas, event)) return True;
    
    // Handle vertical scrollbar clicks
    if (handle_scrollbar_click(canvas, event, true)) return True;
    
    // Handle horizontal scrollbar clicks  
    if (handle_scrollbar_click(canvas, event, false)) return True;
    
    return False;
}

// Update scrollbar position during mouse drag motion
static void update_scroll_from_mouse_drag(Canvas *canvas, bool is_vertical, int initial_scroll, int drag_start_pos, int current_mouse_pos) {
    int mouse_movement = current_mouse_pos - drag_start_pos;
    
    int track_x, track_y, track_width, track_height;
    get_scrollbar_track_area(canvas, is_vertical, &track_x, &track_y, &track_width, &track_height);
    
    int track_length = is_vertical ? track_height : track_width;
    int content_length = is_vertical ? canvas->content_height : canvas->content_width;
    int max_scroll = is_vertical ? canvas->max_scroll_y : canvas->max_scroll_x;
    
    int knob_length = calculate_scrollbar_knob_size(track_length, content_length);
    int available_track_space = max(1, track_length - knob_length);
    
    // Calculate initial knob position as ratio of track
    float initial_knob_ratio = (max_scroll > 0) ? ((float)initial_scroll / (float)max_scroll) : 0.0f;
    float initial_knob_pos = initial_knob_ratio * available_track_space;
    
    // Add mouse movement and clamp to track bounds
    float new_knob_pos = max(0.0f, min((float)available_track_space, initial_knob_pos + (float)mouse_movement));
    
    // Convert back to scroll value
    int new_scroll = (max_scroll > 0) ? (int)roundf((new_knob_pos / (float)available_track_space) * (float)max_scroll) : 0;
    
    if (is_vertical) canvas->scroll_y = new_scroll; 
    else canvas->scroll_x = new_scroll;
    redraw_canvas(canvas);
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

// Check if mouse click is on any desktop icon
static bool mouse_click_is_on_desktop_icon(Canvas *canvas, XButtonEvent *event) {
    FileIcon **icon_array = get_icon_array();
    int icon_count = get_icon_count();
    
    for (int i = 0; i < icon_count; i++) {
        FileIcon *icon = icon_array[i];
        if (icon->display_window == canvas->win &&
            event->x >= icon->x && event->x < icon->x + icon->width &&
            event->y >= icon->y && event->y < icon->y + icon->height) {
            return true;
        }
    }
    return false;
}

// Handle mouse clicks on desktop background
static void handle_desktop_button(Canvas *canvas, XButtonEvent *event) {
    if (event->button == Button3) {
        toggle_menubar_and_redraw();
        return;
    }
    
    if (event->button == Button1) {
        if (now_ms() < g_deactivate_suppress_until_ms) return;
        if (!mouse_click_is_on_desktop_icon(canvas, event)) {
            deactivate_all_windows();
        }
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
        case HIT_ICONIFY:
            canvas->iconify_armed = true; // defer action to ButtonRelease
            redraw_canvas(canvas);
            return True;
        case HIT_MAXIMIZE:
            canvas->maximize_armed = true; // defer action to ButtonRelease
            redraw_canvas(canvas);
            return True;
        case HIT_LOWER:
            canvas->lower_armed = true; // defer action to ButtonRelease
            redraw_canvas(canvas);
            return True;
        case HIT_TITLE:
            return begin_frame_drag(canvas, event);
        case HIT_RESIZE:
            canvas->resize_armed = true;
            redraw_canvas(canvas);
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

    if (canvas->type != WINDOW && canvas->type != DIALOG) 
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
    // Use the new clean resize module with motion compression
    if (resize_is_active()) {
        resize_motion(event->x_root, event->y_root);
        return True;
    }
    return False;
}

static Bool handle_scroll_motion(XMotionEvent *event) {
    if (scrolling_canvas) {
        int current_mouse_pos = scrolling_vertical ? event->y_root : event->x_root;
        update_scroll_from_mouse_drag(scrolling_canvas, scrolling_vertical, initial_scroll, scroll_start_pos, current_mouse_pos);
        return True;
    }
    return False;
}

// Handle arrow button auto-repeat while held
static Bool handle_arrow_scroll_repeat(void) {
    if (!arrow_scroll_canvas || arrow_scroll_direction == 0) return False;
    
    if (arrow_scroll_vertical) {
        int current_scroll = arrow_scroll_canvas->scroll_y;
        int new_scroll = current_scroll + (arrow_scroll_direction * SCROLL_STEP);
        new_scroll = clamp_value_between(new_scroll, 0, arrow_scroll_canvas->max_scroll_y);
        
        if (new_scroll != current_scroll) {
            arrow_scroll_canvas->scroll_y = new_scroll;
            redraw_canvas(arrow_scroll_canvas);
            return True;
        }
    } else {
        int current_scroll = arrow_scroll_canvas->scroll_x;
        int new_scroll = current_scroll + (arrow_scroll_direction * SCROLL_STEP);
        new_scroll = clamp_value_between(new_scroll, 0, arrow_scroll_canvas->max_scroll_x);
        
        if (new_scroll != current_scroll) {
            arrow_scroll_canvas->scroll_x = new_scroll;
            redraw_canvas(arrow_scroll_canvas);
            return True;
        }
    }
    
    // If we can't scroll anymore, stop the repeat
    arrow_scroll_canvas = NULL;
    arrow_scroll_direction = 0;
    return False;
}

void intuition_handle_motion_notify(XMotionEvent *event) {
    if (handle_drag_motion(event)) return;
    if (handle_resize_motion(event)) return;
    if (handle_scroll_motion(event)) return;
    
    // Handle button cancel when mouse moves away
    Canvas *canvas = find_canvas(event->window);
    if (canvas) {
        // Check if mouse is outside window bounds entirely
        bool outside_window = (event->x < 0 || event->y < 0 || 
                              event->x >= canvas->width || event->y >= canvas->height);
        
        if (canvas->close_armed) {
            TitlebarHit hit = hit_test(canvas, event->x, event->y);
            if (hit != HIT_CLOSE || outside_window) {
                canvas->close_armed = false;
                redraw_canvas(canvas);
            }
        }
        if (canvas->iconify_armed) {
            TitlebarHit hit = hit_test(canvas, event->x, event->y);
            if (hit != HIT_ICONIFY || outside_window) {
                canvas->iconify_armed = false;
                redraw_canvas(canvas);
            }
        }
        if (canvas->maximize_armed) {
            TitlebarHit hit = hit_test(canvas, event->x, event->y);
            if (hit != HIT_MAXIMIZE || outside_window) {
                canvas->maximize_armed = false;
                redraw_canvas(canvas);
            }
        }
        if (canvas->lower_armed) {
            TitlebarHit hit = hit_test(canvas, event->x, event->y);
            if (hit != HIT_LOWER || outside_window) {
                canvas->lower_armed = false;
                redraw_canvas(canvas);
            }
        }
    }
    
    // Handle arrow button auto-repeat
    if (arrow_scroll_canvas) {
        handle_arrow_scroll_repeat();
    }
}

void intuition_handle_destroy_notify(XDestroyWindowEvent *event) {
    
    // First check if this is one of our frame windows being destroyed
    Canvas *canvas = find_canvas(event->window);
    if (canvas) {
        // Our frame window was destroyed - clean up everything
        canvas->close_request_sent = false;
        destroy_canvas(canvas);
        return;
    }
    
    // Check if this is a client window destroying itself
    canvas = find_canvas_by_client(event->window);
    if (canvas) {
        
        // Only handle specially for transient windows (dialogs)
        // Normal windows should go through the regular destroy process
        if (canvas->is_transient) {
            
            // Save the parent window before cleanup
            Window parent_win = canvas->transient_for;
            
            // The dialog destroyed itself - just clean up our tracking
            canvas->client_win = None;
            
            // Remove from canvas list
            remove_canvas_from_array(canvas);
            
            // Free our frame window if it exists
            if (canvas->win != None && is_window_valid(display, canvas->win)) {
                XDestroyWindow(display, canvas->win);
            }
            
            // Free resources
            free(canvas->path);
            free(canvas->title_base);
            free(canvas->title_change);
            free(canvas);
            
            // Restore focus to parent window if it exists
            if (parent_win != None) {
                Canvas *parent_canvas = find_canvas_by_client(parent_win);
                if (parent_canvas) {
                    set_active_window(parent_canvas);
                    XSetInputFocus(display, parent_win, RevertToParent, CurrentTime);
                }
            }
        } else {
            // Normal window - client destroyed itself, proceed with normal cleanup
            canvas->close_request_sent = false;
            destroy_canvas(canvas);
        }
    }
}

void intuition_handle_button_release(XButtonEvent *event) {
    // Only end resize if we're actually resizing
    if (resize_is_active()) {
        resize_end();
    }
    
    dragging_canvas = NULL;
    scrolling_canvas = NULL;
    arrow_scroll_canvas = NULL;  // Stop arrow button auto-repeat
    arrow_scroll_direction = 0;
    // Handle deferred button actions
    Canvas *canvas = find_canvas(event->window);
    if (canvas) {
        TitlebarHit hit = hit_test(canvas, event->x, event->y);
        
        // Handle scroll arrow releases
        if (canvas->v_arrow_up_armed) {
            canvas->v_arrow_up_armed = false;
            redraw_canvas(canvas);
            // Check if still on button
            if (event->x >= canvas->width - BORDER_WIDTH_RIGHT && event->x < canvas->width &&
                event->y >= canvas->height - BORDER_HEIGHT_BOTTOM - 41 && 
                event->y < canvas->height - BORDER_HEIGHT_BOTTOM - 21) {
                if (canvas->scroll_y > 0) {
                    canvas->scroll_y = max(0, canvas->scroll_y - SCROLL_STEP);
                    redraw_canvas(canvas);
                }
            }
        }
        
        if (canvas->v_arrow_down_armed) {
            canvas->v_arrow_down_armed = false;
            redraw_canvas(canvas);
            // Check if still on button
            if (event->x >= canvas->width - BORDER_WIDTH_RIGHT && event->x < canvas->width &&
                event->y >= canvas->height - BORDER_HEIGHT_BOTTOM - 21 && 
                event->y < canvas->height - BORDER_HEIGHT_BOTTOM) {
                if (canvas->scroll_y < canvas->max_scroll_y) {
                    canvas->scroll_y = min(canvas->max_scroll_y, canvas->scroll_y + SCROLL_STEP);
                    redraw_canvas(canvas);
                }
            }
        }
        
        if (canvas->h_arrow_left_armed) {
            canvas->h_arrow_left_armed = false;
            redraw_canvas(canvas);
            // Check if still on button
            if (event->y >= canvas->height - BORDER_HEIGHT_BOTTOM && event->y < canvas->height &&
                event->x >= canvas->width - BORDER_WIDTH_RIGHT - 42 && 
                event->x < canvas->width - BORDER_WIDTH_RIGHT - 22) {
                if (canvas->scroll_x > 0) {
                    canvas->scroll_x = max(0, canvas->scroll_x - SCROLL_STEP);
                    redraw_canvas(canvas);
                }
            }
        }
        
        if (canvas->h_arrow_right_armed) {
            canvas->h_arrow_right_armed = false;
            redraw_canvas(canvas);
            // Check if still on button
            if (event->y >= canvas->height - BORDER_HEIGHT_BOTTOM && event->y < canvas->height &&
                event->x >= canvas->width - BORDER_WIDTH_RIGHT - 22 && 
                event->x < canvas->width - BORDER_WIDTH_RIGHT) {
                if (canvas->scroll_x < canvas->max_scroll_x) {
                    canvas->scroll_x = min(canvas->max_scroll_x, canvas->scroll_x + SCROLL_STEP);
                    redraw_canvas(canvas);
                }
            }
        }
        
        if (canvas->resize_armed) {
            canvas->resize_armed = false;
            redraw_canvas(canvas);
            // Resize already began on press, no action needed on release
        }
        
        if (canvas->close_armed) {
            canvas->close_armed = false;
            redraw_canvas(canvas);
            if (hit == HIT_CLOSE) {
                request_client_close(canvas);
                return;
            }
        }
        
        if (canvas->iconify_armed) {
            canvas->iconify_armed = false;
            redraw_canvas(canvas);
            if (hit == HIT_ICONIFY) {
                iconify_canvas(canvas);
                return;
            }
        }
        
        if (canvas->maximize_armed) {
            canvas->maximize_armed = false;
            redraw_canvas(canvas);
            if (hit == HIT_MAXIMIZE) {
                Canvas *desk = get_desktop_canvas();
                if (desk) {
                    if (!canvas->maximized) {
                        // Save current position and dimensions before maximizing
                        canvas->pre_max_x = canvas->x;
                        canvas->pre_max_y = canvas->y;
                        canvas->pre_max_w = canvas->width;
                        canvas->pre_max_h = canvas->height;
                        
                        // Maximize the window
                        int new_w = desk->width;
                        int new_h = desk->height - (MENUBAR_HEIGHT - 1);
                        move_and_resize_frame(canvas, 0, MENUBAR_HEIGHT, new_w, new_h);
                        canvas->maximized = true;
                    } else {
                        // Restore to saved dimensions
                        move_and_resize_frame(canvas, canvas->pre_max_x, canvas->pre_max_y, 
                                            canvas->pre_max_w, canvas->pre_max_h);
                        canvas->maximized = false;
                    }
                }
                return;
            }
        }
        
        if (canvas->lower_armed) {
            canvas->lower_armed = false;
            redraw_canvas(canvas);
            if (hit == HIT_LOWER) {
                lower_window_to_back(canvas);
                canvas->active = false;
                activate_window_behind(canvas);
                compositor_sync_stacking(display);
                return;
            }
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
    XWindowAttributes attrs;
    get_window_attrs_with_defaults(event->window, &attrs);

    if (should_skip_framing(event->window, &attrs)) {
        XMapWindow(display, event->window);
        send_x_command_and_sync();
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
    XWindowAttributes attrs;
    get_window_attrs_with_defaults(event->window, &attrs);
    if (!is_viewable_client(&attrs) || !is_toplevel_under_root(event->window)) return;
    if (should_skip_framing(event->window, &attrs)) return;

    frame_and_activate(event->window, &attrs, true);
}

static void handle_configure_unmanaged(XConfigureRequestEvent *event) {
    XWindowAttributes attrs;
    bool attrs_valid = get_window_attrs_with_defaults(event->window, &attrs);
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
    if (safe_mask) {
        XConfigureWindow(display, event->window, safe_mask, &changes);
        send_x_command_and_sync();
    }
}

static void handle_configure_managed(Canvas *canvas, XConfigureRequestEvent *event) {
    XWindowChanges frame_changes = (XWindowChanges){0};
    unsigned long frame_mask = 0;

    if (event->value_mask & (CWWidth | CWHeight)) {
        calculate_frame_size_from_client_size(event->width, event->height, &frame_changes.width, &frame_changes.height);
        
        
        frame_mask |= (event->value_mask & CWWidth) ? CWWidth : 0;
        frame_mask |= (event->value_mask & CWHeight) ? CWHeight : 0;
    }
    
    // IGNORE position requests from transient windows - WE decide where they go!
    if (!canvas->is_transient) {
        if (event->value_mask & CWX) { frame_changes.x = event->x; frame_mask |= CWX; }
        if (event->value_mask & CWY) { frame_changes.y = max(event->y, MENUBAR_HEIGHT); frame_mask |= CWY; }
    }

    if ((event->value_mask & (CWStackMode | CWSibling)) == (CWStackMode | CWSibling) &&
        event->detail >= 0 && event->detail <= 4) {
        XWindowAttributes sibling_attrs;
        if (XGetWindowAttributes(display, event->above, &sibling_attrs) && sibling_attrs.map_state == IsViewable) {
            frame_changes.stack_mode = event->detail;
            frame_changes.sibling = event->above;
            frame_mask |= CWStackMode | CWSibling;
        }
    }
    if (frame_mask) {
        XConfigureWindow(display, canvas->win, frame_mask, &frame_changes);
        // Update canvas position AND SIZE if we changed the frame
        if (frame_mask & CWX) canvas->x = frame_changes.x;
        if (frame_mask & CWY) canvas->y = frame_changes.y;
        if (frame_mask & CWWidth) canvas->width = frame_changes.width;
        if (frame_mask & CWHeight) canvas->height = frame_changes.height;
    }

    // Configure client window within frame borders
    // IMPORTANT: Must constrain client to fit within frame borders!
    int max_client_width = canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT_CLIENT;
    int max_client_height = canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM;
    
    XWindowChanges client_changes = { .x = BORDER_WIDTH_LEFT, .y = BORDER_HEIGHT_TOP };
    unsigned long client_mask = CWX | CWY;
    
    if (event->value_mask & CWWidth) { 
        // Constrain width to fit within frame
        client_changes.width = min(max(1, event->width), max_client_width);
        client_mask |= CWWidth;
    }
    if (event->value_mask & CWHeight) { 
        // Constrain height to fit within frame
        client_changes.height = min(max(1, event->height), max_client_height);
        client_mask |= CWHeight;
    }
    if (event->value_mask & CWBorderWidth) { client_changes.border_width = 0; client_mask |= CWBorderWidth; }
    XConfigureWindow(display, event->window, client_mask, &client_changes);
    // Don't sync here - it causes major delays during app startup (especially GIMP)
}

void intuition_handle_configure_request(XConfigureRequestEvent *event) {
    Canvas *canvas = find_canvas_by_client(event->window);
    if (!canvas) { handle_configure_unmanaged(event); return; }
    
    handle_configure_managed(canvas, event);
    
    // Calculate the FRAME dimensions from the requested CLIENT dimensions
    int frame_width, frame_height;
    calculate_frame_size_from_client_size(event->width, event->height, &frame_width, &frame_height);
    
    // Apply resize with FRAME dimensions, not client dimensions!
    apply_resize_and_redraw(canvas, frame_width, frame_height);
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
        // Resize the X window first
        XResizeWindow(display, desktop->win, width, height);
        desktop->width = width;
        desktop->height = height;
        
        // Reload wallpapers at the NEW screen size
        // This will free old cached Pictures and create new ones at current screen size
        render_load_wallpapers();
        
        // Now recreate surfaces and redraw with new wallpaper
        render_recreate_canvas_surfaces(desktop);
        // canvas_damage_all(desktop);  // TODO: implement
        redraw_canvas(desktop);
    }
    
    // Update max constraints for all windows based on new screen size
    update_canvas_max_constraints();

    Canvas *menubar_canvas = get_menubar();
    if (menubar_canvas) {
        // Resize and reposition menubar to span new width
        XMoveResizeWindow(display, menubar_canvas->win, 0, 0, width, MENUBAR_HEIGHT);
        menubar_canvas->width = width;
        menubar_canvas->height = MENUBAR_HEIGHT;
        render_recreate_canvas_surfaces(menubar_canvas);
        // canvas_damage_all(menubar_canvas);  // TODO: implement
        redraw_canvas(menubar_canvas);
    }

    XSync(display, False);
}

// destroy and cleanup
void destroy_canvas(Canvas *canvas) {
    if (!canvas || canvas->type == DESKTOP) return;
    clear_canvas_icons(canvas);

    // Clean up dialog-specific structures before destroying canvas
    if (canvas->type == DIALOG) {
        // Check if it's an iconinfo dialog and clean it up
        extern bool is_iconinfo_canvas(Canvas *canvas);
        extern void close_icon_info_dialog_by_canvas(Canvas *canvas);
        extern void close_dialog_by_canvas(Canvas *canvas);
        extern void close_progress_dialog_by_canvas(Canvas *canvas);
        
        if (is_iconinfo_canvas(canvas)) {
            close_icon_info_dialog_by_canvas(canvas);
        } else {
            // Try regular dialogs (rename/delete/execute)
            close_dialog_by_canvas(canvas);
            // Try progress dialogs
            close_progress_dialog_by_canvas(canvas);
        }
    }

    Display *dpy = get_display();

    // If this canvas frames a client, handle it appropriately
    if (canvas->client_win != None) {
        // XGrabServer: Temporarily lock the X11 server to prevent race conditions
        // This ensures no other client can change windows while we're cleaning up
        // Think of it like getting exclusive access to modify critical data
        XGrabServer(dpy);
        
        if (g_restarting) {
            // Restarting - preserve client by unparenting back to root
            // This keeps the client alive so it can be re-framed after restart
            // Place client at its actual screen position (frame position + decorations)
            XReparentWindow(dpy, canvas->client_win, root, 
                          canvas->x + BORDER_WIDTH_LEFT, 
                          canvas->y + BORDER_HEIGHT_TOP);
            XRemoveFromSaveSet(dpy, canvas->client_win);  // Clean up save-set
        } else {
            // Normal operation - request client to close
            send_close_request_to_client(canvas->client_win);
        }
        
        // XUngrabServer: Release the server lock so other clients can work again
        // IMPORTANT: Always ungrab after grabbing to avoid freezing the desktop!
        XUngrabServer(dpy);
        
        // XUnmapWindow: Hide the window from screen (but don't destroy it yet)
        // Like minimizing a window - it still exists but isn't visible
        if (canvas->win != None && is_window_valid(dpy, canvas->win)) {
            XUnmapWindow(dpy, canvas->win);
        }
        // XSync: Force all X11 commands to complete before continuing
        // Without this, commands might queue up and execute out of order
        send_x_command_and_sync();
        canvas->client_win = None;
    }

    // Update focus/activation before tearing down resources
    if (canvas->type == WINDOW) {
        select_next_window(canvas);
    }

    // Free X11 resources in safe order
    send_x_command_and_sync();
    
    // Skip X11 operations if shutting down or display is invalid
    if (!g_shutting_down && dpy) {
        // Free pre-allocated Xft colors
        if (canvas->xft_colors_allocated) {
            XftColorFree(dpy, canvas->visual, canvas->colormap, &canvas->xft_black);
            XftColorFree(dpy, canvas->visual, canvas->colormap, &canvas->xft_white);
            XftColorFree(dpy, canvas->visual, canvas->colormap, &canvas->xft_blue);
            XftColorFree(dpy, canvas->visual, canvas->colormap, &canvas->xft_gray);
            canvas->xft_colors_allocated = false;
        }
        
        // Free XftDraw if allocated
        if (canvas->xft_draw) {
            XftDrawDestroy(canvas->xft_draw);
            canvas->xft_draw = NULL;
        }
        
        // Free render resources
        if (canvas->window_render != None) { XRenderFreePicture(dpy, canvas->window_render); canvas->window_render = None; }
        if (canvas->canvas_render != None) { XRenderFreePicture(dpy, canvas->canvas_render); canvas->canvas_render = None; }
        if (canvas->canvas_buffer != None) { XFreePixmap(dpy, canvas->canvas_buffer); canvas->canvas_buffer = None; }
        if (canvas->colormap != None) { XFreeColormap(dpy, canvas->colormap); canvas->colormap = None; }
        
        // Destroy window
        if (canvas->win != None && is_window_valid(dpy, canvas->win)) {
            XDestroyWindow(dpy, canvas->win);
            canvas->win = None;
        }
    }

    free(canvas->path);
    free(canvas->title_base);
    free(canvas->title_change);

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

// Function to clean up GTK dialog frames without sending messages
void cleanup_gtk_dialog_frame(Canvas* canvas) {
    if (!canvas) return;
    
    // If this was a transient dialog, find the parent and activate it
    Window parent_client = None;
    Window parent_frame = None;
    if (canvas->is_transient && canvas->transient_for != None) {
        // Find the parent canvas
        Canvas *parent_canvas = find_canvas_by_client(canvas->transient_for);
        if (parent_canvas) {
            parent_client = parent_canvas->client_win;
            parent_frame = parent_canvas->win;
        }
    }
    
    // Don't touch the client window - GTK manages it
    // Clean up our frame
    if (canvas->win != None) {
        XUnmapWindow(get_display(), canvas->win);
        XDestroyWindow(get_display(), canvas->win);
    }
    
    // Remove from our management
    for (int i = 0; i < canvas_count; i++) {
        if (canvas_array[i] == canvas) {
            // Shift remaining canvases down
            for (int j = i; j < canvas_count - 1; j++) {
                canvas_array[j] = canvas_array[j + 1];
            }
            canvas_count--;
            break;
        }
    }
    
    // Free canvas resources
    if (canvas->path) free(canvas->path);
    if (canvas->title_base) free(canvas->title_base);
    if (canvas->title_change) free(canvas->title_change);
    free(canvas);
    
    // Now activate the parent window if we found one
    if (parent_client != None && parent_frame != None) {
        
        // Raise the frame window to bring it to front
        XRaiseWindow(get_display(), parent_frame);
        
        // Set input focus to the client window
        XSetInputFocus(get_display(), parent_client, RevertToParent, CurrentTime);
        
        // Also make the frame active in our window manager's view
        set_active_window(find_canvas_by_client(parent_client));
        
        XFlush(get_display());
    }
}