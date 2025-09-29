// Core window management & initialization
// This module handles global state and compositor initialization

#include "../config.h"
#include "itn_internal.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>  // For XClassHint, XGetWMProtocols
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/Xrandr.h>
#include <Imlib2.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>

// Global state
Display *g_display = NULL;
Canvas *g_canvas_list = NULL;
Canvas *g_active_canvas = NULL;
Canvas *g_desktop_canvas = NULL;
bool g_compositor_active = false;
int g_damage_event_base = 0;
int g_damage_error_base = 0;

// Private state - will be used when migration from intuition.c is complete
// For now these are commented out to avoid warnings
// static bool g_shutting_down = false;
// static bool g_restarting = false;
// static int g_screen;
// static Window g_root;
// static int g_width;
// static int g_height;
// static int g_depth;
// static Cursor g_root_cursor;
// static RenderContext *g_render_context = NULL;

// Global variables (were in intuition.c, now defined here)
Display *display = NULL;
int screen = 0;
Window root = 0;
int width = 0, height = 0, depth = 0;
Cursor root_cursor = 0;
RenderContext *render_context = NULL;
Canvas **canvas_array = NULL;
int canvas_count = 0;
int canvas_array_size = 0;

// Additional globals for event handling
Canvas *active_window = NULL;
Canvas *dragging_canvas = NULL;
Canvas *scrolling_canvas = NULL;
Canvas *arrow_scroll_canvas = NULL;
int arrow_scroll_direction = 0;
bool arrow_scroll_vertical = false;
struct timeval arrow_scroll_start_time = {0};
struct timeval arrow_scroll_last_time = {0};
int drag_start_x = 0, drag_start_y = 0;
int window_start_x = 0, window_start_y = 0;
bool scrolling_vertical = false;
int initial_scroll = 0, scroll_start_pos = 0;
bool fullscreen_active = false;
bool g_last_press_consumed = false;

Display *itn_core_get_display(void) {
    return g_display ? g_display : display;  // Use global during migration
}

bool itn_core_init_compositor(void) {
    Display *dpy = g_display ? g_display : display;  // Use display during migration
    if (!dpy) return false;

    int scr = screen;  // Use extern during migration
    Window root_win = root;  // Use extern during migration

    // Check for required extensions
    int event, error;
    if (!XCompositeQueryExtension(dpy, &event, &error)) {
        log_error("[ERROR] XComposite extension not available");
        return false;
    }

    if (!XDamageQueryExtension(dpy, &g_damage_event_base, &g_damage_error_base)) {
        log_error("[ERROR] XDamage extension not available");
        return false;
    }

    int render_event, render_error;
    if (!XRenderQueryExtension(dpy, &render_event, &render_error)) {
        log_error("[ERROR] XRender extension not available");
        return false;
    }

    // Try to acquire compositor selection
    char selname[32];
    snprintf(selname, sizeof(selname), "_NET_WM_CM_S%d", scr);
    Atom sel = XInternAtom(dpy, selname, False);
    Window current_owner = XGetSelectionOwner(dpy, sel);

    if (current_owner != None) {
        log_error("[ERROR] Another compositor is already running");
        // Continue anyway - not critical
    } else {
        // Create owner window and claim selection
        XSetWindowAttributes swa = {0};
        swa.override_redirect = True;
        Window owner = XCreateWindow(dpy, root_win, -1, -1, 1, 1, 0,
                                    CopyFromParent, InputOutput,
                                    CopyFromParent, CWOverrideRedirect, &swa);
        XSetSelectionOwner(dpy, sel, owner, CurrentTime);
        // TODO: Store owner window for cleanup
    }

    // CRITICAL: Redirect all subwindows for compositing
    // This makes windows render to offscreen pixmaps instead of screen
    XCompositeRedirectSubwindows(dpy, root_win, CompositeRedirectManual);

    // Select input on root for structure changes
    XSelectInput(dpy, root_win,
                SubstructureNotifyMask | StructureNotifyMask | PropertyChangeMask);

    // Initialize the overlay window and back buffer
    // Hardware acceleration is MANDATORY - no fallback, no compromise!
    if (!itn_composite_init_overlay()) {
        log_error("[ERROR] Overlay initialization failed - compositing required");
        XCompositeUnredirectSubwindows(dpy, root_win, CompositeRedirectManual);
        return false;
    }

    // Initialize frame scheduler
    if (!itn_render_init_frame_scheduler()) {
        log_error("[ERROR] Frame scheduler initialization failed - compositing required");
        itn_composite_cleanup_overlay();
        XCompositeUnredirectSubwindows(dpy, root_win, CompositeRedirectManual);
        return false;
    }

    // MUST set active BEFORE setting up canvases!
    // Otherwise itn_composite_setup_canvas() will return early
    g_compositor_active = true;

    // Setup compositing for existing canvases
    // When compositor starts, existing windows need compositing setup
    for (int i = 0; i < canvas_count; i++) {
        Canvas *c = canvas_array[i];
        if (!c || !c->win) continue;

        // Check if window is mapped
        XWindowAttributes attrs;
        if (XGetWindowAttributes(dpy, c->win, &attrs)) {
            if (attrs.map_state == IsViewable && !c->comp_damage) {
                // Setup compositing for this existing canvas
                itn_composite_setup_canvas(c);
            }
        }
    }

    // Scan for any existing override-redirect windows (menus, tooltips, etc)
    // This one-time scan catches any popups that existed before we started
    Window root_return, parent_return;
    Window *children = NULL;
    unsigned int nchildren = 0;

    if (XQueryTree(dpy, root_win, &root_return, &parent_return, &children, &nchildren)) {
        int override_count = 0;

        for (unsigned int i = 0; i < nchildren; i++) {
            Window w = children[i];

            // Skip our overlay window
            Window overlay_win = itn_composite_get_overlay_window();
            if (w == overlay_win) continue;

            // Skip Canvas windows (already handled above)
            if (itn_canvas_find_by_window(w)) continue;

            // Check if it's an override-redirect window
            XWindowAttributes attrs;
            if (XGetWindowAttributes(dpy, w, &attrs)) {
                if (attrs.map_state == IsViewable && attrs.override_redirect &&
                    attrs.class == InputOutput) {
                    // Found an existing override-redirect window
                    itn_composite_add_override(w, &attrs);
                    override_count++;
                }
            }
        }

        if (override_count > 0) {
        }

        if (children) XFree(children);
    }


    // CRITICAL: Do initial render so screen isn't black!
    // The compositor is ready but nothing has triggered a render yet
    itn_composite_render_all();

    return true;
}

void itn_core_shutdown_compositor(void) {
    if (!g_compositor_active) return;

    Display *dpy = g_display ? g_display : display;
    if (!dpy) return;

    Window root_win = root;  // Use extern during migration

    // Cleanup frame scheduler
    itn_render_cleanup_frame_scheduler();

    // Cleanup overlay and compositing resources
    itn_composite_cleanup_overlay();

    // Unredirect subwindows to restore normal rendering
    XCompositeUnredirectSubwindows(dpy, root_win, CompositeRedirectManual);

    // TODO: Release compositor selection and destroy owner window

    g_compositor_active = false;
}

bool itn_core_is_compositor_active(void) {
    return g_compositor_active;
}

// ============================================================================
// Legacy Compatibility Functions (for main.c and other files)
// ============================================================================

// Forward declarations
bool init_display_and_root(void);  // Defined below
bool init_render_context(void);    // Defined below
extern int x_error_handler(Display *dpy, XErrorEvent *error);
extern void frame_existing_client_windows(void);
extern void render_load_wallpapers(void);

Canvas *init_intuition(void) {
    if (!init_display_and_root() || !init_render_context()) return NULL;

    // Create desktop canvas with proper window
    // Use create_canvas which creates the actual X11 window
    extern Canvas *create_canvas(const char *path, int x, int y, int width, int height, CanvasType type);
    Canvas *desktop = create_canvas(getenv("HOME"), 0, 20, width, height, DESKTOP);
    if (!desktop) return NULL;

    // Set WM_CLASS for the desktop window
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

    // Initialize compositor if configured
    #ifdef USE_COMPOSITOR
    if (USE_COMPOSITOR) {
        itn_core_init_compositor();
    }
    #endif

    // Redraw the desktop to show wallpaper
    extern void redraw_canvas(Canvas *canvas);
    redraw_canvas(desktop);

    return desktop;
}

void cleanup_intuition(void) {
    if (g_compositor_active) {
        itn_core_shutdown_compositor();
    }
    // Cleanup render context
    if (render_context) {
        free(render_context);
        render_context = NULL;
    }
    // Cleanup display
    if (display) {
        XCloseDisplay(display);
        display = NULL;
    }
}

void begin_shutdown(void) {
    extern bool g_shutting_down;
    g_shutting_down = true;
}

void begin_restart(void) {
    extern bool g_restarting;
    g_restarting = true;
}

// ============================================================================
// Display and Root Initialization
// ============================================================================

bool init_display_and_root(void) {
    display = XOpenDisplay(NULL);
    if (!display) return false;

    // Set error handler
    extern int x_error_handler(Display *dpy, XErrorEvent *error);
    XSetErrorHandler(x_error_handler);
    XSync(display, False);

    // Initialize display parameters
    screen = DefaultScreen(display);
    width = DisplayWidth(display, screen);
    height = DisplayHeight(display, screen);
    root = RootWindow(display, screen);
    depth = 32;

    // Set root cursor
    root_cursor = XCreateFontCursor(display, XC_left_ptr);
    XDefineCursor(display, root, root_cursor);

    // Setup RandR for resolution changes
    int randr_error_base;
    if (XRRQueryExtension(display, &randr_event_base, &randr_error_base)) {
        XRRSelectInput(display, root, RRScreenChangeNotifyMask);
    } else {
        log_error("[WARNING] XRANDR extension not available; resolution changes may not be handled.");
    }

    // Select root window events
    XSelectInput(display, root, SubstructureRedirectMask | SubstructureNotifyMask | PropertyChangeMask |
                 StructureNotifyMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | KeyPressMask);

    // Advertise EWMH support
    Atom net_supported = XInternAtom(display, "_NET_SUPPORTED", False);
    Atom supported[7];
    supported[0] = XInternAtom(display, "_NET_WM_STATE", False);
    supported[1] = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
    supported[2] = XInternAtom(display, "_NET_WM_ALLOWED_ACTIONS", False);
    supported[3] = XInternAtom(display, "_NET_WM_ACTION_FULLSCREEN", False);
    supported[4] = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
    supported[5] = XInternAtom(display, "_NET_WM_NAME", False);
    supported[6] = XInternAtom(display, "_NET_CLIENT_LIST", False);
    XChangeProperty(display, root, net_supported, XA_ATOM, 32, PropModeReplace,
                   (unsigned char *)supported, 7);

    return true;
}

bool init_render_context(void) {
    render_context = malloc(sizeof(RenderContext));
    if (!render_context) {
        log_error("[ERROR] malloc failed for RenderContext (size=%zu)", sizeof(RenderContext));
        return false;
    }
    render_context->dpy = display;
    XVisualInfo vinfo;
    XMatchVisualInfo(display, screen, depth, TrueColor, &vinfo);
    render_context->fmt = XRenderFindVisualFormat(display, vinfo.visual);
    render_context->desk_img = None;
    render_context->wind_img = None;
    render_context->desk_picture = None;
    render_context->wind_picture = None;
    render_context->checker_active_pixmap = None;
    render_context->checker_active_picture = None;
    render_context->checker_inactive_pixmap = None;
    render_context->checker_inactive_picture = None;
    // Cache frequently used default values
    render_context->default_screen = DefaultScreen(display);
    render_context->default_visual = DefaultVisual(display, render_context->default_screen);
    render_context->default_colormap = DefaultColormap(display, render_context->default_screen);
    return true;
}

// ============================================================================
// Additional Functions
// ============================================================================

RenderContext *get_render_context(void) {
    return render_context;
}

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

    *windows = window_list;
    return count;
}

void iconify_all_windows(void) {
    for (int i = 0; i < canvas_count; i++) {
        Canvas *c = canvas_array[i];
        if (c && c->type == WINDOW && c != itn_canvas_get_desktop()) {
            extern void iconify_canvas(Canvas *canvas);
            iconify_canvas(c);
        }
    }
}

Canvas *find_window_by_path(const char *path) {
    if (!path) return NULL;

    for (int i = 0; i < canvas_count; i++) {
        Canvas *c = canvas_array[i];
        // Only check WINDOW type canvases with valid paths
        if (!c || c->type != WINDOW || !c->path) continue;

        if (strcmp(c->path, path) == 0) {
            return c;
        }
    }
    return NULL;
}

bool is_window_valid(Display *dpy, Window win) {
    if (win == None) return false;
    XWindowAttributes attrs;
    return XGetWindowAttributes(dpy, win, &attrs) == True;
}

// ============================================================================
// State Management Functions
// ============================================================================

// Global state flags (exposed for itn_canvas.c)
bool g_shutting_down = false;
bool g_restarting = false;
static long long g_deactivate_suppress_until_ms = 0;

// Get current time in milliseconds
static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

bool is_restarting(void) {
    return g_restarting;
}

void suppress_desktop_deactivate_for_ms(int ms) {
    long long n = now_ms();
    if (ms < 0) ms = 0;
    long long until = n + (long long)ms;
    if (until > g_deactivate_suppress_until_ms) {
        g_deactivate_suppress_until_ms = until;
    }
}


void remove_canvas_from_array(Canvas *canvas) {
    if (!canvas) return;

    for (int i = 0; i < canvas_count; i++) {
        if (canvas_array[i] == canvas) {
            // Shift remaining canvases down
            for (int j = i; j < canvas_count - 1; j++) {
                canvas_array[j] = canvas_array[j + 1];
            }
            canvas_count--;
            canvas_array[canvas_count] = NULL;
            break;
        }
    }
}

void set_active_window(Canvas *canvas) {
    itn_focus_set_active(canvas);
}

void init_scroll(Canvas *canvas) {
    if (!canvas) return;
    canvas->scroll_x = 0;
    canvas->scroll_y = 0;
    canvas->max_scroll_x = 0;
    canvas->max_scroll_y = 0;
}

Display *get_display(void) {
    return display;
}

// ============================================================================
// Window Management Functions
// ============================================================================

bool send_close_request_to_client(Window client_window) {
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

void send_x_command_and_sync(void) {
    XSync(display, False);
}

bool is_fullscreen_active(Window win) {
    Atom wm_state = XInternAtom(display, "_NET_WM_STATE", False);
    Atom fullscreen = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
    Atom type;
    int format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;

    if (XGetWindowProperty(display, win, wm_state, 0, 1024, False, AnyPropertyType,
            &type, &format, &nitems, &bytes_after, &prop) != Success || !prop) {
        return false;
    }

    bool active = false;
    for (unsigned long i = 0; i < nitems; i++) {
        if (((Atom*)prop)[i] == fullscreen) {
            active = true;
            break;
        }
    }
    XFree(prop);
    return active;
}

void calculate_frame_size_from_client_size(int client_width, int client_height,
                                          int *frame_width, int *frame_height) {
    // Add border decorations to client size
    *frame_width = client_width + BORDER_WIDTH_LEFT + 8;  // Right border is typically 8
    *frame_height = client_height + BORDER_HEIGHT_TOP + BORDER_HEIGHT_BOTTOM;
}

void menubar_apply_fullscreen(bool fullscreen) {
    extern Canvas *get_menubar(void);
    Canvas *menubar = get_menubar();
    if (!menubar) return;

    if (fullscreen) {
        XUnmapWindow(display, menubar->win);
    } else {
        XMapWindow(display, menubar->win);
    }
}

bool get_window_attrs_with_defaults(Window win, XWindowAttributes *attrs) {
    if (!attrs) return false;

    // Set default values first
    attrs->x = 200;
    attrs->y = 200;
    attrs->width = 640;
    attrs->height = 480;
    attrs->class = InputOutput;
    attrs->override_redirect = False;

    // Try to get actual attributes
    if (XGetWindowAttributes(display, win, attrs)) {
        return true;
    }

    // If failed, keep defaults and return false
    return false;
}

unsigned long unmanaged_safe_mask(void) {
    return CWX | CWY | CWWidth | CWHeight;
}

// Public API to get canvas array
Canvas **get_canvas_array(void) {
    return canvas_array;
}

// Public API to get canvas count
int get_canvas_count(void) {
    return canvas_count;
}

// X11 error handler
int x_error_handler(Display *dpy, XErrorEvent *error) {
    char error_text[256];
    XGetErrorText(dpy, error->error_code, error_text, sizeof(error_text));

    // Log error but don't crash
    log_error("X Error: %s (code %d, request %d.%d, resource 0x%lx)\n",
              error_text, error->error_code,
              error->request_code, error->minor_code,
              error->resourceid);

    // Return 0 to continue
    return 0;
}