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
#include "resize.h"


#define INITIAL_CANVAS_CAPACITY 8

// Forward declarations
static bool should_skip_framing(Window win, XWindowAttributes *attrs);
static bool is_window_valid(Display *dpy, Window win);
static bool get_window_attributes_safely(Window win, XWindowAttributes *attrs);
static void calculate_content_area_inside_frame(const Canvas *canvas, int *content_width, int *content_height);
static void calculate_frame_size_from_client_size(int client_width, int client_height, int *frame_width, int *frame_height);
static inline void move_and_resize_frame(Canvas *c, int x, int y, int w, int h);

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

// Monotonic clock in milliseconds; used for input timing decisions
// like double-click and suppression windows.
static long long now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}


// Send close request to client window (WM_DELETE_WINDOW or XKillClient)
static bool send_close_request_to_client(Window client_window) {
    if (!is_window_valid(display, client_window)) return false;
    
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

// Handle close request for canvas (special logic for transient windows)
static void request_client_close(Canvas *canvas) {
    if (!canvas) return;
           
    if (canvas->client_win && is_window_valid(display, canvas->client_win)) {
        // For transient windows that already had a close request, destroy completely
        if (canvas->is_transient && canvas->close_request_sent) {
            destroy_canvas(canvas);
            return;
        }
        
        // Quick responsiveness test for transient windows
        if (canvas->is_transient) {
            Atom wm_name = XInternAtom(display, "WM_NAME", False);
            Atom actual_type; int actual_format; unsigned long nitems, bytes_after;
            unsigned char *prop_return = NULL;
            
            int result = XGetWindowProperty(display, canvas->client_win, wm_name, 0, 0, False, AnyPropertyType,
                                           &actual_type, &actual_format, &nitems, &bytes_after, &prop_return);
            if (prop_return) XFree(prop_return);
            
            if (result != Success) {
                destroy_canvas(canvas); // Client is dead
                return;
            }
        }
        
        if (send_close_request_to_client(canvas->client_win) && canvas->is_transient) {
            canvas->close_request_sent = true;
        }
    } else {
        destroy_canvas(canvas);
    }
}



 

// Check if window is a top-level window (direct child of root window)
static bool is_toplevel_under_root(Window w) {
    XWindowAttributes attrs;
    if (!get_window_attributes_safely(w, &attrs)) return false;
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

// Check if a window exists and can be safely accessed
static bool is_window_valid(Display *dpy, Window win) {
    if (win == None) return false;
    XWindowAttributes attrs;
    return XGetWindowAttributes(dpy, win, &attrs) == True;
}

// Get window attributes with validation - returns false if window is invalid
static bool get_window_attributes_safely(Window win, XWindowAttributes *attrs) {
    if (win == None || !attrs) return false;
    return XGetWindowAttributes(display, win, attrs) == True;
}

// Send X command and ensure it completes immediately 
static void send_x_command_and_sync(void) {
    // Don't sync if we're shutting down or display is invalid
    if (g_shutting_down || !display) return;
    XSync(display, False);
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

// Calculate frame window size needed to contain client area with borders
static inline void calculate_frame_size_from_client_size(int client_width, int client_height, int *frame_width, int *frame_height) {
    if (frame_width) *frame_width = max(1, client_width) + BORDER_WIDTH_LEFT + BORDER_WIDTH_RIGHT;
    if (frame_height) *frame_height = max(1, client_height) + BORDER_HEIGHT_TOP + BORDER_HEIGHT_BOTTOM;
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
    
    if (content_width) *content_width = max(1, canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT);
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
        if (!expanded_array) return NULL;
        canvas_array = expanded_array;
        canvas_array_size = new_size;
    }
    
    // Allocate new canvas
    Canvas *new_canvas = malloc(sizeof(Canvas));
    if (!new_canvas) return NULL;
    
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
    render_context->desk_img = None; render_context->wind_img = None; return true;
}

static void apply_resize_and_redraw(Canvas *c, int nw, int nh) {
    if (!c) return; if (c->width == nw && c->height == nh) return;
    c->width = nw; c->height = nh; 
    
    // Skip expensive buffer recreation during interactive resize
    if (!c->resizing_interactive) {
        render_recreate_canvas_surfaces(c);
    }
    
    if (c->client_win != None) { 
        int client_width, client_height; 
        calculate_content_area_inside_frame(c, &client_width, &client_height);
        XWindowChanges ch = { .width = client_width, .height = client_height };
        XConfigureWindow(display, c->client_win, CWWidth | CWHeight, &ch);
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
}

static void init_canvas_metadata(Canvas *c, const char *path, CanvasType t,
                                 int x, int y, int w, int h) {
    *c = (Canvas){0}; c->type = t; c->path = path ? strdup(path) : NULL;
    c->title = path ? strdup(strrchr(path, '/') ? strrchr(path, '/') + 1 : path) : NULL;
    if (c->title && strlen(c->title) == 0) c->title = strdup("System");
    c->x = x; c->y = (t == WINDOW) ? max(y, MENUBAR_HEIGHT) : y;
    c->width = w; c->height = h; 
    // Set default background color
    c->bg_color = GRAY;
    c->buffer_width = w; c->buffer_height = h;  // Initialize to canvas size, may be enlarged later
    c->resizing_interactive = false;
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
    if (!c->win) return False;
    c->colormap = attrs.colormap;

    // No internal tagging

    // Select input events
    XSelectInput(ctx->dpy, c->win, get_event_mask_for_canvas_type(t));

    // Backing pixmap for offscreen rendering
    c->canvas_buffer = XCreatePixmap(ctx->dpy, c->win, w, h, vinfo.depth);
    return c->canvas_buffer ? True : False;
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
    Visual *wv = (t == DESKTOP) ? DefaultVisual(ctx->dpy, DefaultScreen(ctx->dpy)) : c->visual;
    XRenderPictFormat *wfmt = XRenderFindVisualFormat(ctx->dpy, wv); if (!wfmt) return False;
    // Create Picture for the actual window - this is what gets displayed on screen
    // We composite from canvas_render (buffer) to window_render (screen)
    c->window_render = XRenderCreatePicture(ctx->dpy, c->win, wfmt, 0, NULL); return c->window_render ? True : False;
}

static Canvas *frame_client_window(Window client, XWindowAttributes *attrs) {
    if (!attrs) return NULL;
    int fx = max(attrs->x, 200), fy = max(attrs->y, MENUBAR_HEIGHT + 100);
    int fw, fh;
    calculate_frame_size_from_client_size(attrs->width, attrs->height, &fw, &fh);
    Canvas *frame = create_canvas(NULL, fx, fy, fw, fh, WINDOW); if (!frame) return NULL;
    
    // Check if this is a transient window (modal dialog) and mark it
    Window transient_for = None;
    if (XGetTransientForHint(display, client, &transient_for)) {
        frame->is_transient = true;
        frame->transient_for = transient_for;
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
    frame->client_win = client;
    if (frame->client_win != None) {
        XClassHint ch;
        if (XGetClassHint(display, frame->client_win, &ch)) {
            // For MPV and similar apps, res_name might be generic (like "x11", "wayland")
            // Use res_class if res_name looks like a video/audio output driver
            if (ch.res_name && ch.res_class) {
                // Check if res_name is a generic output driver name
                if (strcmp(ch.res_name, "x11") == 0 || 
                    strcmp(ch.res_name, "wayland") == 0 ||
                    strcmp(ch.res_name, "opengl") == 0 ||
                    strcmp(ch.res_name, "vulkan") == 0 ||
                    strcmp(ch.res_name, "sdl") == 0) {
                    // Use res_class instead (usually the app name)
                    frame->title = ch.res_class;
                    XFree(ch.res_name);
                } else {
                    // Use res_name as normal
                    frame->title = ch.res_name;
                    XFree(ch.res_class);
                }
            } else if (ch.res_name) {
                frame->title = ch.res_name;
                if (ch.res_class) XFree(ch.res_class);
            } else if (ch.res_class) {
                frame->title = ch.res_class;
            } else {
                frame->title = "NoNameApp";
            }
        } else {
            frame->title = "NoNameApp";
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
    XSync(display, False);
}

Canvas *get_active_window(void) {
    return active_window;
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
    const char *def_foo_path = "/usr/local/share/amiwb/icons/def_icons/def_foo.info";
    
    if (c->client_win == None) { 
        label = c->title ? strdup(c->title) : strdup("Untitled"); 
        icon_path = "/usr/local/share/amiwb/icons/filer.info";
    } else {
        XClassHint ch;
        if (XGetClassHint(display, c->client_win, &ch)) {
            // Determine label - use same logic as window title
            const char *app_name = ch.res_name;
            if (ch.res_name && ch.res_class) {
                // Check if res_name is a generic output driver name
                if (strcmp(ch.res_name, "x11") == 0 || 
                    strcmp(ch.res_name, "wayland") == 0 ||
                    strcmp(ch.res_name, "opengl") == 0 ||
                    strcmp(ch.res_name, "vulkan") == 0 ||
                    strcmp(ch.res_name, "sdl") == 0) {
                    app_name = ch.res_class;
                }
            }
            
            // Try to find a specific icon for this app
            char icon_full[256]; 
            snprintf(icon_full, sizeof(icon_full), "/usr/local/share/amiwb/icons/%s.info", app_name);
            struct stat st; 
            if (stat(icon_full, &st) == 0) {
                icon_path = icon_full;
            } else {
                printf("[ICON] Couldn't find %s.info at %s, using def_foo.info\n", app_name, icon_full);
                icon_path = def_foo_path;
            }
            label = strdup(app_name);
            
            if (ch.res_name) XFree(ch.res_name);
            if (ch.res_class) XFree(ch.res_class);
        } else { 
            label = strdup("Untitled"); 
            icon_path = def_foo_path;
        }
    }
    
    // Verify the icon path exists, use def_foo as ultimate fallback
    struct stat st;
    if (stat(icon_path, &st) != 0) {
        printf("[WARNING] Icon file not found: %s, using def_foo.info\n", icon_path);
        icon_path = def_foo_path;
    }
    
    create_icon(icon_path, desk, nx, ny);
    FileIcon **ia = get_icon_array(); 
    FileIcon *ni = ia[get_icon_count() - 1];
    
    // Ensure we actually got an icon, this is critical
    if (!ni) {
        printf("[ERROR] Failed to create iconified icon for window, using emergency fallback\n");
        // Try one more time with def_foo
        create_icon(def_foo_path, desk, nx, ny);
        ia = get_icon_array();
        ni = ia[get_icon_count() - 1];
        if (!ni) {
            printf("[ERROR] CRITICAL: Cannot create iconified icon - window will be lost!\n");
            free(label);
            return;
        }
    }
    
    ni->type = TYPE_ICONIFIED; 
    free(ni->label); 
    ni->label = label; 
    free(ni->path); 
    ni->path = NULL; 
    ni->iconified_canvas = c;
    XUnmapWindow(display, c->win); 
    if (active_window == c) active_window = NULL; 
    redraw_canvas(desk); 
    send_x_command_and_sync();
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
    canvas->is_transient = false;
    canvas->transient_for = None;
    canvas->close_request_sent = false;
    canvas->consecutive_unmaps = 0;
    canvas->disable_scrollbars = false;

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
    // Use the new clean resize module
    resize_begin(c, e->x_root, e->y_root);
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
    Canvas *canvas = find_canvas_by_client(event->window) ?: find_canvas(event->window);
    if (!canvas) {
        return;
    }
    // Clear any pending close request flag
    canvas->close_request_sent = false;
    destroy_canvas(canvas);
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
                    int new_w = desk->width;
                    int new_h = desk->height - (MENUBAR_HEIGHT - 1);
                    move_and_resize_frame(canvas, 0, MENUBAR_HEIGHT, new_w, new_h);
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

    // Configure client window within frame borders
    XWindowChanges client_changes = { .x = BORDER_WIDTH_LEFT, .y = BORDER_HEIGHT_TOP };
    unsigned long client_mask = CWX | CWY;
    if (event->value_mask & CWWidth) { client_changes.width = max(1, event->width); client_mask |= CWWidth; }
    if (event->value_mask & CWHeight) { client_changes.height = max(1, event->height); client_mask |= CWHeight; }
    if (event->value_mask & CWBorderWidth) { client_changes.border_width = 0; client_mask |= CWBorderWidth; }
    XConfigureWindow(display, event->window, client_mask, &client_changes);
    send_x_command_and_sync();
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
        // XGrabServer: Temporarily lock the X11 server to prevent race conditions
        // This ensures no other client can change windows while we're cleaning up
        // Think of it like getting exclusive access to modify critical data
        XGrabServer(dpy);
        send_close_request_to_client(canvas->client_win);
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