// Core window management & initialization
// This module handles global state and compositor initialization

#include "../config.h"
#include "itn_internal.h"
#include "../render_public.h"
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
#include <execinfo.h>  // For backtrace support

// Global state
static Display *g_display = NULL;  // Now encapsulated via itn_core_get_display()
static bool g_compositor_active = false;  // Now encapsulated via getter/setter
static int g_damage_event_base = 0;
static int g_damage_error_base = 0;

// Global state flags (exposed for itn_canvas.c)
bool g_shutting_down = false;
bool g_restarting = false;

// Global variables (were in intuition.c, now defined here)
Display *display = NULL;
int screen = 0;
Window root = 0;
static int width = 0, height = 0, depth = 0;  // Encapsulated - use getters
Cursor root_cursor = 0;
RenderContext *render_context = NULL;
// Canvas array moved to itn_manager.c - use itn_manager_* functions

// Additional globals for event handling (legacy - to be migrated)
// active_window removed - use itn_focus_get_active() / itn_focus_set_active()
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

Display *itn_core_get_display(void) {
    return g_display ? g_display : display;  // Use global during migration
}

int itn_core_get_screen(void) {
    return screen;
}

Window itn_core_get_root(void) {
    return root;
}

int itn_core_get_screen_width(void) {
    return width;
}

int itn_core_get_screen_height(void) {
    return height;
}

int itn_core_get_screen_depth(void) {
    return depth;
}

void itn_core_set_screen_dimensions(int w, int h) {
    width = w;
    height = h;
}

bool itn_core_is_fullscreen_active(void) {
    return fullscreen_active;
}

void itn_core_set_fullscreen_active(bool active) {
    fullscreen_active = active;
}

bool itn_composite_is_active(void) {
    return g_compositor_active;
}

void itn_composite_set_active(bool active) {
    g_compositor_active = active;
}

int itn_core_get_damage_event_base(void) {
    return g_damage_event_base;
}

int itn_core_get_damage_error_base(void) {
    return g_damage_error_base;
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
    itn_composite_set_active(true);

    // Setup compositing for existing canvases
    // When compositor starts, existing windows need compositing setup
    int count = itn_manager_get_count();
    for (int i = 0; i < count; i++) {
        Canvas *c = itn_manager_get_canvas(i);
        if (!c || !c->win) continue;

        // Check if window is mapped
        XWindowAttributes attrs;
        if (safe_get_window_attributes(dpy, c->win, &attrs)) {
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
            if (safe_get_window_attributes(dpy, w, &attrs)) {
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

    itn_composite_set_active(false);
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

    // Initialize compositor BEFORE framing existing windows
    // This ensures XCompositeRedirectSubwindows is active when frames are created
    #ifdef USE_COMPOSITOR
    if (USE_COMPOSITOR) {
        itn_core_init_compositor();
    }
    #endif

    // Frame any existing client windows AFTER compositor is initialized
    // This ensures frames are redirected when created
    frame_existing_client_windows();

    // Redraw the desktop to show wallpaper
    redraw_canvas(desktop);

    return desktop;
}

void cleanup_intuition(void) {
    if (g_compositor_active) {
        itn_core_shutdown_compositor();
    }

    // CRITICAL: Destroy all canvases BEFORE closing display
    // This triggers client preservation logic if g_restarting is true
    int count = itn_manager_get_count();
    if (count > 0) {
        // Iterate backwards to avoid array shifting issues
        for (int i = count - 1; i >= 0; i--) {
            Canvas *c = itn_manager_get_canvas(i);
            if (c) {
                itn_canvas_destroy(c);
            }
        }
    }

    // Cleanup manager
    itn_manager_cleanup();

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
    g_shutting_down = true;
}

void begin_restart(void) {
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

    int total = itn_manager_get_count();
    for (int i = 0; i < total; i++) {
        Canvas *c = itn_manager_get_canvas(i);
        if (c && (c->type == WINDOW || c->type == DIALOG)) {
            window_list[count++] = c;
            if (count >= 256) break;  // Safety limit
        }
    }

    *windows = window_list;
    return count;
}

void iconify_all_windows(void) {
    int count = itn_manager_get_count();
    for (int i = 0; i < count; i++) {
        Canvas *c = itn_manager_get_canvas(i);
        if (c && c->type == WINDOW && c != itn_canvas_get_desktop()) {
            extern void iconify_canvas(Canvas *canvas);
            iconify_canvas(c);
        }
    }
}

Canvas *find_window_by_path(const char *path) {
    if (!path) return NULL;

    int count = itn_manager_get_count();
    for (int i = 0; i < count; i++) {
        Canvas *c = itn_manager_get_canvas(i);
        // Only check WINDOW type canvases with valid paths
        if (!c || c->type != WINDOW || !c->path) continue;

        if (strcmp(c->path, path) == 0) {
            return c;
        }
    }
    return NULL;
}

// Window being validated (to match errors to the correct validation)
static volatile Window g_validating_window = None;
// Flag to track whether validation encountered an error
// Must be volatile because error handler runs in signal-like context
static volatile int g_validation_error = 0;

// Temporary error handler to suppress BadWindow errors during X11 operations
// Expected X11 race: window destroyed between validation and operation - unavoidable, must silence
// Only suppresses errors for g_validating_window to avoid hiding unrelated bugs
static int ignore_bad_window_on_get_attrs(Display *dpy, XErrorEvent *error) {
    extern void log_error(const char *format, ...);

    // CRITICAL: Only suppress if error is for the EXACT window we're validating
    // X11 errors are asynchronous - we may receive errors from previous operations
    if (error->error_code == 3 && error->resourceid == g_validating_window) {  // BadWindow error code
        // Expected X11 async race - window destroyed between validation and operation (unavoidable)
        g_validation_error = 1;  // Mark validation as failed
        return 0;  // Suppress (expected, harmless)
    }

    if (error->error_code == 4) {  // BadPixmap error code
        // This indicates a Pixmap ID was passed where a Window ID was expected
        log_error("[ERROR] BadPixmap: resourceid=0x%lx, request=%d.%d",
                  error->resourceid, error->request_code, error->minor_code);

        // Only mark validation failed if this Pixmap IS the window we're validating
        // (which means someone passed a Pixmap ID where a Window ID was expected)
        if (error->resourceid == g_validating_window) {
            g_validation_error = 1;
        }
        return 0;
    }

    if (error->error_code == 8) {  // BadMatch error code
        // Log details but DON'T make any X calls (that would cause recursion/deadlock)
        const char *atom_name = "UNKNOWN";
        if (error->resourceid == 0x25) atom_name = "XA_WM_COMMAND";
        else if (error->resourceid == 0x29) atom_name = "XA_WM_ICON_SIZE";

        log_error("[ERROR] BadMatch: resourceid=0x%lx (%s), request=%d.%d, serial=%lu",
                  error->resourceid, atom_name,
                  error->request_code, error->minor_code, error->serial);

        // Generate backtrace to identify the source of the error
        void *buffer[64];
        int nptrs = backtrace(buffer, 64);
        char **strings = backtrace_symbols(buffer, nptrs);
        if (strings) {
            log_error("[ERROR] Backtrace:");
            for (int i = 0; i < nptrs; i++) {
                log_error("[ERROR]   %s", strings[i]);
            }
            free(strings);
        }

        // Ignore BadMatch - might be invalid window with wrong attributes
        return 0;
    }

    // Call the default error handler for other errors
    extern int x_error_handler(Display *dpy, XErrorEvent *error);
    return x_error_handler(dpy, error);
}

bool is_window_valid(Display *dpy, Window win) {
    if (win == None) return false;

    // Set window being validated and clear error flag
    g_validating_window = win;
    g_validation_error = 0;

    // Install temporary error handler to ignore BadWindow errors
    // This prevents validation checks from generating error spam when
    // checking destroyed windows (which is the whole point of validation)
    XErrorHandler old_handler = XSetErrorHandler(ignore_bad_window_on_get_attrs);

    XWindowAttributes attrs;
    XGetWindowAttributes(dpy, win, &attrs);
    XSync(dpy, False);  // Force execution to catch any errors now

    // Restore original error handler
    XSetErrorHandler(old_handler);

    // Clear window being validated
    g_validating_window = None;

    // Check if an error occurred (the error handler sets g_validation_error)
    // This is necessary because XGetWindowAttributes may return success even when
    // an error occurs asynchronously
    bool valid = (g_validation_error == 0);

    return valid;
}

// Safe XGetWindowAttributes - handles asynchronous window destruction race
// Even with is_window_valid() checks, window can be destroyed between
// validation and attribute query due to X11's asynchronous nature
Bool safe_get_window_attributes(Display *dpy, Window win, XWindowAttributes *attrs) {
    if (win == None) return False;

    // Set window being validated so error handler can match errors correctly
    g_validating_window = win;
    g_validation_error = 0;

    // Install temporary error handler to ignore BadWindow errors
    XErrorHandler old_handler = XSetErrorHandler(ignore_bad_window_on_get_attrs);

    Bool result = XGetWindowAttributes(dpy, win, attrs);
    XSync(dpy, False);  // Force execution to catch any errors now

    // Restore original error handler
    XSetErrorHandler(old_handler);

    // Clear window being validated
    g_validating_window = None;

    return result;
}

// Safe unmap - validates window exists before unmapping
void safe_unmap_window(Display *dpy, Window win) {
    if (is_window_valid(dpy, win)) {
        XUnmapWindow(dpy, win);
    }
}

// Safe translate coordinates - validates windows exist before translating
Bool safe_translate_coordinates(Display *dpy, Window src_w, Window dest_w,
                                int src_x, int src_y, int *dest_x, int *dest_y,
                                Window *child) {
    // Validate both source and destination windows exist
    if (!is_window_valid(dpy, src_w) || !is_window_valid(dpy, dest_w)) {
        // Set outputs to safe defaults
        if (dest_x) *dest_x = 0;
        if (dest_y) *dest_y = 0;
        if (child) *child = None;
        return False;
    }

    // Install temporary error handler to catch window destruction race
    // Even with validation, window can be destroyed before XTranslateCoordinates executes
    XErrorHandler old_handler = XSetErrorHandler(ignore_bad_window_on_get_attrs);

    Bool result = XTranslateCoordinates(dpy, src_w, dest_w, src_x, src_y, dest_x, dest_y, child);
    XSync(dpy, False);  // Force execution to catch any errors now

    // Restore original error handler
    XSetErrorHandler(old_handler);

    return result;
}

// Temporary error handler to suppress BadMatch errors during SetInputFocus
// BadMatch occurs when window is destroyed between validation and focus operation
static int ignore_bad_match_on_focus(Display *dpy, XErrorEvent *error) {
    if (error->error_code == 8) {  // BadMatch error code
        // Silently ignore - window destroyed between validation and focus
        return 0;
    }
    // Call the default error handler for other errors
    extern int x_error_handler(Display *dpy, XErrorEvent *error);
    return x_error_handler(dpy, error);
}

// Safe SetInputFocus - handles asynchronous window destruction race
// Even with is_window_valid() checks, window can be destroyed between
// validation and focus operation due to X11's asynchronous nature
void safe_set_input_focus(Display *dpy, Window win, int revert_to, Time time) {
    // First check if window exists at all
    if (!is_window_valid(dpy, win)) {
        return;
    }

    // Install temporary error handler to ignore BadMatch errors
    // Window can still be destroyed after validation but before focus
    XErrorHandler old_handler = XSetErrorHandler(ignore_bad_match_on_focus);

    XSetInputFocus(dpy, win, revert_to, time);
    XSync(dpy, False);  // Force execution to catch any errors now

    // Restore original error handler
    XSetErrorHandler(old_handler);
}

// Debug wrapper for XGetWindowProperty to trace property access
// Set to 1 to enable verbose property access logging (disabled now that errors are fixed)
// Also provides error protection against X11 async race (window destroyed during property read)
static int g_debug_property_access = 0;

int debug_get_window_property(Display *dpy, Window win, Atom property,
                               long offset, long length, Bool delete,
                               Atom req_type, Atom *actual_type,
                               int *actual_format, unsigned long *nitems,
                               unsigned long *bytes_after, unsigned char **prop,
                               const char *caller_location) {
    if (g_debug_property_access) {
        char *prop_name = XGetAtomName(dpy, property);
        log_error("[PROP-DEBUG] %s: XGetWindowProperty(win=0x%lx, prop=%s)",
                  caller_location, win, prop_name ? prop_name : "unknown");
        if (prop_name) XFree(prop_name);
    }

    // Protect against BadWindow when window destroyed during property read (expected X11 race)
    g_validating_window = win;
    g_validation_error = 0;

    XErrorHandler old_handler = XSetErrorHandler(ignore_bad_window_on_get_attrs);

    int result = XGetWindowProperty(dpy, win, property, offset, length, delete,
                                     req_type, actual_type, actual_format, nitems,
                                     bytes_after, prop);

    XSync(dpy, False);  // Force immediate error delivery
    XSetErrorHandler(old_handler);

    g_validating_window = None;

    return result;
}

// Enable property access debugging
void enable_property_debug(void) {
    g_debug_property_access = 1;
    log_error("[PROP-DEBUG] Property access debugging ENABLED");
}

// Disable property access debugging
void disable_property_debug(void) {
    g_debug_property_access = 0;
    log_error("[PROP-DEBUG] Property access debugging DISABLED");
}

// ============================================================================
// State Management Functions
// ============================================================================

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
    itn_manager_remove(canvas);
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

    // Control compositor visibility without unmapping window
    // This keeps all rendering resources valid (XftDraw, Pictures, pixmaps)
    if (fullscreen) {
        menubar->comp_visible = false;
    } else {
        menubar->comp_visible = true;
    }

    // Trigger compositor to update display
    SCHEDULE_FRAME();
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

    // Try to get actual attributes (safe wrapper handles window destruction race)
    if (safe_get_window_attributes(display, win, attrs)) {
        return true;
    }

    // If failed, keep defaults and return false
    return false;
}

unsigned long unmanaged_safe_mask(void) {
    return CWX | CWY | CWWidth | CWHeight;
}

// Public API to get canvas array (transition helper)
Canvas **get_canvas_array(void) {
    return itn_manager_get_array();
}

// Public API to get canvas count
int get_canvas_count(void) {
    return itn_manager_get_count();
}

// X11 error handler with request decoding
int x_error_handler(Display *dpy, XErrorEvent *error) {
    // Suppress compositor errors from short-lived tooltip/popup race conditions
    // Browser tooltips are destroyed microseconds after mapping, causing errors when:
    // - Damage events arrive after window destroyed (BadDamage)
    // - Compositor tries to configure destroyed window (BadWindow on ConfigureWindow)
    // - Compositor tries to render with freed Picture (RenderBadPicture)
    // These are harmless and expected for override-redirect windows
    if (error->error_code == 152) {  // BadDamage
        return 0;  // Silently ignore
    }

    // Suppress BadWindow errors specifically on ConfigureWindow (request 12)
    // This happens when compositor tries to position an already-destroyed tooltip
    if (error->error_code == BadWindow && error->request_code == 12) {
        return 0;  // Silently ignore
    }

    // Suppress RenderBadPicture errors (code 143)
    // This happens when compositor tries to render an already-destroyed tooltip
    if (error->error_code == 143) {  // RenderBadPicture
        return 0;  // Silently ignore
    }

    char error_text[256];
    XGetErrorText(dpy, error->error_code, error_text, sizeof(error_text));

    // Decode common request codes for better diagnostics
    const char *request_name = "Unknown";
    switch (error->request_code) {
        case 2: request_name = "ChangeWindowAttributes"; break;
        case 3: request_name = "GetWindowAttributes"; break;
        case 4: request_name = "DestroyWindow"; break;
        case 8: request_name = "MapWindow"; break;
        case 10: request_name = "UnmapWindow"; break;
        case 12: request_name = "ConfigureWindow"; break;
        case 15: request_name = "QueryTree"; break;
        case 18: request_name = "ChangeProperty"; break;
        case 19: request_name = "DeleteProperty"; break;
        case 20: request_name = "GetProperty"; break;
        case 38: request_name = "QueryPointer"; break;
        case 40: request_name = "TranslateCoordinates"; break;
        case 42: request_name = "SetInputFocus"; break;
    }

    // Log error with decoded request name
    log_error("X Error: %s (code %d, request %d.%d [%s], resource 0x%lx)\n",
              error_text, error->error_code,
              error->request_code, error->minor_code,
              request_name, error->resourceid);

    // Capture and print backtrace (call stack)
    void *buffer[64];
    int nptrs = backtrace(buffer, 64);
    char **strings = backtrace_symbols(buffer, nptrs);

    if (strings) {
        log_error("Call stack:\n");
        for (int i = 0; i < nptrs; i++) {
            log_error("  [%d] %s\n", i, strings[i]);

            // Try to resolve address to file:line using addr2line
            char cmd[512];
            // Use absolute path to binary instead of /proc/self/exe
            snprintf(cmd, sizeof(cmd), "addr2line -e /usr/local/bin/amiwb %p", buffer[i]);
            FILE *fp = popen(cmd, "r");
            if (fp) {
                char location[256];
                if (fgets(location, sizeof(location), fp)) {
                    // Remove newline
                    location[strcspn(location, "\n")] = 0;
                    // Only print if not "??:?" (unknown)
                    if (strcmp(location, "??:?") != 0 && strcmp(location, "??:0") != 0) {
                        log_error("      → %s\n", location);
                    }
                }
                pclose(fp);
            }
        }
        free(strings);
    }

    // Return 0 to continue
    return 0;
}