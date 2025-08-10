// File: intuition.c
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

// Global state for intuition
static Display *display = NULL;
static RenderContext *render_context = NULL;
static Canvas **canvas_array = NULL;
static int canvas_count = 0;
static int canvas_array_size = 0;
static Bool fullscreen_active = False;
static Canvas *active_window = NULL;    // currently active window

// Suppress desktop deactivation after restore to avoid losing focus immediately
static long long g_deactivate_suppress_until_ms = 0;

static long long now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void lower_window_to_back(Canvas *win_canvas) {
    if (!win_canvas) return;
    Canvas *desktop = get_desktop_canvas();
    if (!desktop) { XLowerWindow(display, win_canvas->win); return; }

    // Deterministically place the window immediately above the desktop
    XWindowChanges ch;
    ch.sibling = desktop->win;
    ch.stack_mode = Above; // directly above the desktop means bottom-most among windows
    XConfigureWindow(display, win_canvas->win, CWSibling | CWStackMode, &ch);
    XSync(display, False);

}

// Returns the bottom-most WINDOW canvas excluding a specific one
static Canvas *find_bottommost_window_excluding(Canvas *exclude) {
    Window root_return, parent_return, *children = NULL; unsigned int n = 0;
    Window xr = DefaultRootWindow(display);
    if (!XQueryTree(display, xr, &root_return, &parent_return, &children, &n)) return NULL;
    Canvas *result = NULL;
    // iterate bottom -> top
    for (unsigned int i = 0; i < n; ++i) {
        Canvas *c = find_canvas(children[i]);
        if (c && c->type == WINDOW && c != exclude) { result = c; break; }
    }
    if (children) XFree(children);
    return result;
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
// removed unused global Visual *visual
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

// True for vertical scrollbar, false for horizontal
static bool scrolling_vertical = true;

// Initial scroll position at drag start
static int initial_scroll = 0;

// Initial mouse position (x or y root) at drag start
static int scroll_start_pos = 0;

// X error handler, log details and make them non-fatal
static bool g_shutting_down = false;

void begin_shutdown(void) {
    g_shutting_down = true;
}

// motion helpers are defined later near event handlers

static int x_error_handler(Display *dpy, XErrorEvent *error) {
    char error_text[1024];
    XGetErrorText(dpy, error->error_code, error_text, sizeof(error_text));

    // During shutdown, suppress all X errors to ensure graceful exit
    if (g_shutting_down) return 0;

    fprintf(stderr, "X Error intercepted: error_code=%d (%s), request_code=%d, minor_code=%d, resource_id=0x%lx\n",
            error->error_code, error_text, error->request_code, error->minor_code, error->resourceid);
    fflush(stderr);
    return 0;  // continue execution (non-fatal handling)
}

// Load image with Imlib2, create a tiled Pixmap for the desktop 
static Pixmap load_wallpaper_to_pixmap(Display *dpy, Window root, int screen_width, int screen_height, const char *path, bool tile) {
    
    Imlib_Image img = imlib_load_image(path);
    if (!img) {
        fprintf(stderr, "Failed to load wallpaper: %s\n", path);
        return None;
    }
    imlib_context_set_image(img);
    int img_width = imlib_image_get_width();
    int img_height = imlib_image_get_height();

    int pw = tile ? img_width : screen_width;
    int ph = tile ? img_height : screen_height;

    // Create a Pixmap matching screen size
    Pixmap pixmap = XCreatePixmap(dpy, root, screen_width, screen_height, DefaultDepth(dpy, DefaultScreen(dpy)));

    // Set drawable and render (scale to fit; for tiling, use imlib_render_image_part_on_drawable_at_size in a loop)
    imlib_context_set_drawable(pixmap);
    
    if (!tile) {
        imlib_render_image_on_drawable_at_size(0, 0, screen_width, screen_height);
    }
    else {
        // Tile the image across the pixmap
        int tile_x, tile_y;
        for (tile_y = 0; tile_y < screen_height; tile_y += img_height) {
            for (tile_x = 0; tile_x < screen_width; tile_x += img_width) {
                imlib_render_image_on_drawable(tile_x, tile_y);
            }
        }
    }

    imlib_free_image();
    return pixmap;
}

Display *get_display(void) { return display; }

RenderContext *get_render_context(void) { return render_context; }


// =======================
// canvas management array
// =======================
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

// ============
// init helpers
// ============
static XVisualInfo select_visual(CanvasType type) {
    XVisualInfo vinfo;
    if (type == DESKTOP) {
        vinfo.visual = DefaultVisual(display, screen);
        vinfo.depth = DefaultDepth(display, screen);
    } else {

        if (!XMatchVisualInfo(display, screen, GLOBAL_DEPTH, 
                TrueColor, &vinfo)) {

            vinfo.visual = DefaultVisual(display, screen);
            vinfo.depth = DefaultDepth(display, screen);
        }
    }
    XMatchVisualInfo(display, screen, vinfo.depth, TrueColor, &vinfo);
    return vinfo;
}

static bool init_render_context(void) {
    render_context = malloc(sizeof(RenderContext));
    if (!render_context) return false;
    render_context->dpy = display;

    XVisualInfo vinfo;
    XMatchVisualInfo(display, screen, depth, TrueColor, &vinfo);
    render_context->fmt = XRenderFindVisualFormat(display, vinfo.visual);
    render_context->desk_img = None;
    render_context->wind_img = None;
    return true;
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

    // enable XRANDR events
    int randr_error_base;
    if (XRRQueryExtension(display, &randr_event_base, &randr_error_base)) {
        XRRSelectInput(display, root, RRScreenChangeNotifyMask);
    } else {
        fprintf(stderr, "XRANDR extension not available; \
            resolution changes may not be handled.\n");
    }

    XSelectInput(display, root, SubstructureRedirectMask | 
        SubstructureNotifyMask | PropertyChangeMask |
        StructureNotifyMask | ButtonPressMask | 
        ButtonReleaseMask | PointerMotionMask | KeyPressMask);

    XSync(display, False);
    return true;
}

// ==============
// init intuition
// ==============
Canvas *init_intuition(void) {
    if (!init_display_and_root() || !init_render_context()) return NULL;

    Canvas *desktop = create_canvas(getenv("HOME"), 
        0, 20, width, height, DESKTOP);
    if (!desktop) return NULL;

    // Initialize Imlib2 context using desktop canvas properties
    imlib_context_set_display(display);
    imlib_context_set_visual(desktop->visual);
    imlib_context_set_colormap(desktop->colormap);
    // Optional: Disable caching for lightweight usage
    imlib_set_cache_size(0);

    if (strlen(DESKPICT) > 0) {
        render_context->desk_img = load_wallpaper_to_pixmap(display, root, width, height, DESKPICT, DESKTILE);
        // Avoid clearing or changing root background to prevent flashes; the
        // desktop canvas will composite the wallpaper via double-buffer.
    }

    if (strlen(WINDPICT) > 0) {
        render_context->wind_img = load_wallpaper_to_pixmap(display, root, width, height, WINDPICT, WINDTILE);
    }

    Window root_return, parent_return;
    Window *children;
    unsigned int nchildren;
    if (XQueryTree(display, root, &root_return, &parent_return, 
            &children, &nchildren)) {

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
            if (!XGetWindowAttributes(display, child, &attrs) || 
                attrs.override_redirect || attrs.map_state != IsViewable || 
                attrs.class == InputOnly) continue;

            bool skip_framing = should_skip_framing(child, &attrs);
            if (skip_framing) continue;

            int frame_x = attrs.x;
            // Clamp y to ensure titlebar is below menubar
            int frame_y = max(attrs.y, MENUBAR_HEIGHT);  
            int frame_width = attrs.width + BORDER_WIDTH_LEFT +
                BORDER_WIDTH_RIGHT;
            int frame_height = attrs.height + BORDER_HEIGHT_TOP + 
                BORDER_HEIGHT_BOTTOM;
            Canvas *frame = create_canvas(NULL, frame_x, frame_y, frame_width, 
                frame_height, WINDOW);
            if (!frame) continue;

            XReparentWindow(display, child, frame->win, BORDER_WIDTH_LEFT, 
                BORDER_HEIGHT_TOP);
            XSelectInput(display, child, StructureNotifyMask | 
                PropertyChangeMask);
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


// =========================
// canvas management helpers
// =========================
Canvas *get_desktop_canvas(void) { 
    return canvas_count > 0 ? canvas_array[0] : NULL; 
}

Canvas *find_canvas(Window win) {
    //printf("find canvas(win)\n");
    for (int i = 0; i < canvas_count; i++) if (canvas_array[i]->win == win) 
        return canvas_array[i];
    return NULL;
}

Canvas *find_canvas_by_client(Window client_win) {
    for (int i = 0; i < canvas_count; i++) 
        if (canvas_array[i]->client_win == client_win) 
            return canvas_array[i];
    return NULL;
}

Canvas *find_canvas_by_path(const char *path) {
    for (int i = 0; i < canvas_count; i++) {
        if (canvas_array[i]->path && strcmp(canvas_array[i]->path, path) == 0) 
            return canvas_array[i];
    }
    return NULL;
}

static void deactivate_all_windows(void){
    for (int i = 0; i < canvas_count; i++) {
        Canvas *c = canvas_array[i];
        if (c->type == WINDOW ){
            c->active = false;
            redraw_canvas(c);
        }
    }
    // Clear active window pointer after deactivation
    active_window = NULL;
}

static void deactivate_other_windows(Canvas *except) {
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

// Activate the topmost WINDOW canvas, excluding one (e.g., the just-lowered window)
static void activate_topmost_window_excluding(Canvas *exclude) {
    Window root_ret, parent_ret, *children = NULL;
    unsigned int n = 0;
    if (!XQueryTree(display, root, &root_ret, &parent_ret, &children, &n)) return;
    // Children are bottom-to-top; scan from topmost down
    for (int i = (int)n - 1; i >= 0; --i) {
        Canvas *c = find_canvas(children[i]);
        if (c && c->type == WINDOW && c != exclude) {
            set_active_window(c);
            break;
        }
    }
    if (children) XFree(children);
}

// Debug: print stacking position for a frame and whether it's topmost among WINDOWs
void set_active_window(Canvas *canvas) {
    if (!canvas || canvas->type != WINDOW) return;
    deactivate_other_windows(canvas);
    active_window = canvas;
    canvas->active = true;
    XRaiseWindow(display, canvas->win);
    compositor_sync_stacking(display);
    if (canvas->client_win != None) {
        XSetInputFocus(display, canvas->client_win,
                       RevertToParent, CurrentTime);
    } else {
        XSetInputFocus(display, canvas->win,
                       RevertToParent, CurrentTime);
    }
    redraw_canvas(canvas);
    XSync(display, False);
}

Canvas *get_active_window(void) { 
    return active_window; 
}

static bool is_fullscreen_active(Window win) {
    Atom wm_state = XInternAtom(display, "_NET_WM_STATE", False);
    Atom fullscreen = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
    Atom type;
    int format;
    unsigned long nitems, bytes_after;
    unsigned char *prop;
    
    if (XGetWindowProperty(display, win, wm_state, 0, 1024, False, 
            AnyPropertyType, &type, &format, &nitems, &bytes_after, 
            &prop) != Success || !prop) 
        return false;
    
    bool active = false;
    
    for (unsigned long i = 0; i < nitems; i++) 
        if (((Atom *)prop)[i] == fullscreen) { 
            active = true; break; 
        }
    
    XFree(prop);
    return active;
}

// Find next free grid slot on the desktop without moving existing icons
static void find_next_desktop_slot(Canvas *desktop, int *out_x, int *out_y) {
    if (!desktop || !out_x || !out_y) return;
    const int start_x = 10;
    const int start_y = 40;
    const int col_step = 100; // matches refresh_canvas_from_directory spacing
    const int row_step = ICON_SPACING; // vertical spacing

    FileIcon **icons = get_icon_array();
    int count = get_icon_count();

    // Build list of existing column x baselines by clustering icon x positions
    int col_x[64]; int col_x_count = 0; const int col_merge_thresh = 32;
    for (int i = 0; i < count; i++) {
        FileIcon *ic = icons[i];
        if (ic->display_window != desktop->win) continue;
        int x = ic->x;
        bool merged = false;
        for (int c = 0; c < col_x_count; c++) {
            if (abs(col_x[c] - x) <= col_merge_thresh) {
                // keep the minimum to represent the column baseline
                if (x < col_x[c]) col_x[c] = x;
                merged = true; break;
            }
        }
        if (!merged && col_x_count < 64) col_x[col_x_count++] = x;
    }
    // sort ascending
    for (int i = 0; i < col_x_count; i++)
        for (int j = i + 1; j < col_x_count; j++)
            if (col_x[j] < col_x[i]) { int t = col_x[i]; col_x[i] = col_x[j]; col_x[j] = t; }

    // Iterate grid positions left-to-right columns, top-to-bottom rows
    int col_idx = 0;
    for (int x = start_x; x < desktop->width - 64; x += col_step, col_idx++) {
        for (int y = start_y; y < desktop->height - 64; y += row_step) {
            bool occupied = false;
            for (int i = 0; i < count; i++) {
                FileIcon *ic = icons[i];
                if (ic->display_window != desktop->win) continue;
                // Icon rect including label area
                int iw = ic->width;
                int ih = ic->height + 20;
                if (!(x + 64 < ic->x || ic->x + iw < x ||
                      y + 64 < ic->y || ic->y + ih < y)) {
                    occupied = true; break;
                }
            }
            if (!occupied) {
                // Align x to existing column baseline if available
                if (col_idx < col_x_count) *out_x = col_x[col_idx];
                else {
                    // Default center akin to first column's minimal width
                    int default_center = start_x + (80 - 64) / 2; // min_cell_w=80
                    *out_x = default_center + col_idx * col_step;
                }
                *out_y = y; return;
            }
        }
    }
    // Fallback to start
    *out_x = start_x; *out_y = start_y;
}

// Iconify a canvas window (hide it and create a desktop icon)
void iconify_canvas(Canvas *canvas) {

    // Only for WINDOW types (both workbench and clients)
    if (!canvas || canvas->type != WINDOW) return;

    Canvas *desktop = get_desktop_canvas();
    if (!desktop) return;

    // Compute next free slot in the desktop grid without re-layout
    int next_x = 10, next_y = 40;
    find_next_desktop_slot(desktop, &next_x, &next_y);

    char *label = NULL;
    const char *icon_path = NULL;

    if (canvas->client_win == None) {
        // Workbench window: use fixed filer icon and canvas title
        label = canvas->title ? strdup(canvas->title) : strdup("Untitled");
        icon_path = "/usr/local/share/amiwb/icons/filer.info";
        printf("Iconifying workbench window: label=%s, icon=%s\n", label, icon_path);
    } else {
        // Client window: use WM_CLASS instance as 
        // short name for label and icon matching
        XClassHint class_hint;
        if (XGetClassHint(display, canvas->client_win, &class_hint) && class_hint.res_name) {

            // Instance name (e.g., "xterm")
            char *app_name = class_hint.res_name;  
            char icon_full[256];
            snprintf(icon_full, sizeof(icon_full), 
                "/usr/local/share/amiwb/icons/%s.info", app_name);

            struct stat st;
            if (stat(icon_full, &st) == 0) {
                icon_path = icon_full;
                printf("Found matching icon for app_name=%s: %s\n", 
                    app_name, icon_full);
            } else {
                icon_path = "/usr/local/share/amiwb/icons/def_tool.info";
                printf("No matching icon for app_name=%s, \
                    using default: %s\n", app_name, icon_path);
            }

            label = strdup(app_name);
            XFree(class_hint.res_name);
            XFree(class_hint.res_class);
        } else {
            // Fallback if no class hint
            label = strdup("Untitled");
            icon_path = "/usr/local/share/amiwb/icons/def_tool.info";
            printf("Could not get class hint for client, \
                using default icon: %s\n", icon_path);
        }
    }

    // Create the iconified icon
    create_icon(icon_path, desktop, next_x, next_y);
    FileIcon **icons_arr = get_icon_array();
    FileIcon *new_icon = icons_arr[get_icon_count() - 1];
    new_icon->type = TYPE_ICONIFIED;
    free(new_icon->label);
    new_icon->label = label;
    free(new_icon->path);
    new_icon->path = NULL;
    new_icon->iconified_canvas = canvas;

    // Hide the window
    XUnmapWindow(display, canvas->win);
    if (active_window == canvas) active_window = NULL;

    // Do not auto-reorganize; just redraw
    redraw_canvas(desktop);
    XSync(display, False);
}

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

// =================
// create new window
// =================
Canvas *create_canvas(const char *path, int x, int y, int width, 
        int height, CanvasType type) {
    
    RenderContext *ctx = get_render_context();
    if (!ctx) return NULL;

    Canvas *canvas = manage_canvases(true, NULL);
    if (!canvas) return NULL;

    *canvas = (Canvas){0};
    canvas->type = type;
    canvas->path = path ? strdup(path) : NULL;
    canvas->title = path ? strdup(strrchr(path, '/') ? 
        strrchr(path, '/') + 1 : path) : NULL;
    canvas->view_mode = VIEW_ICONS; // default view
    
    // default string ("/" isn't nice)
    if (canvas->title && strlen(canvas->title) == 0) {
        canvas->title = strdup("System"); 
    }

    canvas->x = x;
    // Clamp y to ensure titlebar is below menubar
    canvas->y = (type == WINDOW) ? max(y, MENUBAR_HEIGHT) : y; 

    // TODO: fix windows max size clamping 
    // int maxwidth = DisplayHeight(display, screen);
    // int maxheight = DisplayHeight(display, screen);
    // width and heigh cannot be bigger than the screen
    // canvas->width =  min(maxwidth, width); //width;
    // canvas->height = min(maxheight - 40, height ); //height;

    canvas->width =width;
    canvas->height = height;
    canvas->bg_color = GRAY;
    canvas->active = false;
    canvas->show_hidden = false;

    XVisualInfo vinfo = select_visual(type);
    canvas->visual = vinfo.visual;
    canvas->depth = vinfo.depth;

    XSetWindowAttributes attrs = {0};
    attrs.colormap = XCreateColormap(ctx->dpy, root, canvas->visual, AllocNone);
    attrs.border_pixel = 0;
    attrs.background_pixel = 0;        // fully transparent for ARGB visuals
    attrs.background_pixmap = None;    // avoid server clears that paint black
    unsigned long valuemask = CWColormap | CWBorderPixel | CWBackPixel | CWBackPixmap;
    if (type == DESKTOP) {
        // Create a real desktop window so the compositor can redirect and composite it
        // Fullscreen at (0, MENUBAR_HEIGHT) to allow menubar above it
        canvas->win = XCreateWindow(display, root, 0, MENUBAR_HEIGHT, width, height - MENUBAR_HEIGHT,
                                    0, vinfo.depth, InputOutput, canvas->visual, valuemask, &attrs);
    } else {
        // Use border width 0 to avoid server-side 1px border truncation
        canvas->win = XCreateWindow(display, root, x, y, width, height, 0,
                                    vinfo.depth, InputOutput, canvas->visual, valuemask, &attrs);
    }
    
    if (!canvas->win) {
        destroy_canvas(canvas);
        return NULL;
    }
    canvas->colormap = attrs.colormap;

    const char *dbg = getenv("WB_DEBUG");
    if (dbg && *dbg) {
        fprintf(stderr, "[WB] create_canvas: type=%d win=0x%lx pos=(%d,%d) size=%dx%d path=%s\n",
                type, (unsigned long)canvas->win, x, y, width, height, path ? path : "-");
    }

    long event_mask = ExposureMask | ButtonPressMask | PointerMotionMask |
                      ButtonReleaseMask | KeyPressMask;
    
    if (type == DESKTOP) 
        event_mask |= SubstructureRedirectMask | SubstructureNotifyMask;
    if (type == WINDOW) 
        event_mask |= StructureNotifyMask | SubstructureNotifyMask | EnterWindowMask | FocusChangeMask;
    if (type == MENU)
        event_mask |= PointerMotionMask | ButtonPressMask | ButtonReleaseMask;

    XSelectInput(ctx->dpy, canvas->win, event_mask);

    canvas->canvas_buffer = XCreatePixmap(ctx->dpy, canvas->win, 
        width, height, vinfo.depth);
    
    if (!canvas->canvas_buffer) {
        destroy_canvas(canvas);
        return NULL;
    }

    // Picture format for the offscreen buffer (matches pixmap depth/visual)
    XRenderPictFormat *fmt = XRenderFindVisualFormat(ctx->dpy, canvas->visual);
    if (!fmt) {
        destroy_canvas(canvas);
        return NULL;
    }

    canvas->canvas_render = XRenderCreatePicture(ctx->dpy, 
        canvas->canvas_buffer, fmt, 0, NULL);
    
    if (!canvas->canvas_render) {
        destroy_canvas(canvas);
        return NULL;
    }
    
    // For the on-screen window picture, use the ACTUAL window visual format.
    // Desktop uses the root window visual, not canvas->visual.
    Visual *win_visual = (type == DESKTOP) ? DefaultVisual(ctx->dpy, DefaultScreen(ctx->dpy)) : canvas->visual;
    XRenderPictFormat *wfmt = XRenderFindVisualFormat(ctx->dpy, win_visual);
    if (!wfmt) {
        destroy_canvas(canvas);
        return NULL;
    }
    canvas->window_render = XRenderCreatePicture(ctx->dpy, canvas->win, 
        wfmt, 0, NULL);
    
    if (!canvas->window_render) {
        destroy_canvas(canvas);
        return NULL;
    }

    // Do not map the WINDOW yet: prepaint offscreen to avoid initial black.
    // Menus and Desktop can be mapped immediately.
    if (type == MENU) {
        XMapRaised(ctx->dpy, canvas->win);
    } else if (type == DESKTOP) {
        XMapWindow(ctx->dpy, canvas->win);
        XLowerWindow(ctx->dpy, canvas->win);
    }

    if (dbg && *dbg) {
        const char *tn = (type == DESKTOP ? "DESKTOP" : (type == WINDOW ? "WINDOW" : "MENU"));
        fprintf(stderr, "[WB] mapped %s win=0x%lx (raised=%d)\n", tn, (unsigned long)canvas->win, (type!=DESKTOP)?1:0);
    }

    if (type == WINDOW) {
        canvas->scroll_x = 0;
        canvas->scroll_y = 0;
        canvas->content_width = width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT;
        canvas->content_height = height - BORDER_HEIGHT_TOP - 
            BORDER_HEIGHT_BOTTOM;
        compute_max_scroll(canvas);
    }

    // TODO: fix clients window from showing window pattern briefly
    // this doesn't work well
/*
    if (type == WINDOW && canvas->client_win != None) {
        XRenderFillRectangle(ctx->dpy, PictOpSrc, canvas->canvas_render, &GRAY, 0, 0, width, height);
        XRenderComposite(ctx->dpy, PictOpSrc, canvas->canvas_render, None, canvas->window_render, 0, 0, 0, 0, 0, 0, width, height);
        XFlush(ctx->dpy);
    }
*/


    if (type != DESKTOP) {
        if (type == WINDOW ) {
            attrs.background_pixmap = None;
            XChangeWindowAttributes(display, canvas->win, CWBackPixmap, &attrs);
            // Prepaint background/frame into offscreen buffer, then blit once
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

static bool should_skip_framing(Window win, XWindowAttributes *attrs) {
    // Only skip truly unmanaged windows
    if (attrs->override_redirect || attrs->class == InputOnly) return true;
    // Always frame normal client windows; ignore MWM hints requesting no decorations
    return false;
}

static void select_next_window(Canvas *closing_canvas) {
    if (active_window == closing_canvas) active_window = NULL;

    Window root_return, parent_return;
    Window *children;
    unsigned int nchildren;
    if (XQueryTree(display, root, &root_return, &parent_return, 
            &children, &nchildren)) {

        for (int i = nchildren - 1; i >= 0; i--) {  // Top to bottom
            if (children[i] == closing_canvas->win) 
                continue;  // Skip closing

            Canvas *next_canvas = find_canvas(children[i]);
            if (next_canvas && next_canvas->type == WINDOW) {
                set_active_window(next_canvas);
                //printf("Activated new top: %s\n", next_canvas->title);
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

// =================
// events handling
// =================
void intuition_handle_expose(XExposeEvent *event) {
    Canvas *canvas = find_canvas(event->window);
    if (canvas && !fullscreen_active) redraw_canvas(canvas);
}

void intuition_handle_property_notify(XPropertyEvent *event) {
    if (event->atom != XInternAtom(display, "WM_STATE", False)) return;

    Canvas *canvas = find_canvas(event->window);
    if (canvas && canvas->type == WINDOW) 
        fullscreen_active = is_fullscreen_active(event->window);

    Canvas *menubar = get_menubar();
    if (menubar) {
        
        if (fullscreen_active) { 
            XUnmapWindow(display, menubar->win); 
        }
        else { 
            XMapWindow(display, menubar->win); redraw_canvas(menubar); 
        }
        
        menubar->width = DisplayWidth(display, DefaultScreen(display));
        XResizeWindow(display, menubar->win, menubar->width, MENU_ITEM_HEIGHT);
        redraw_canvas(menubar);
    }
}

// === compact helpers to reduce bloat in button_press ===
static void handle_desktop_button(Canvas *canvas, XButtonEvent *event) {
    if (event->button == Button3) {
        toggle_menubar_state();
        Canvas *mb = get_menubar();
        if (mb) { XMapWindow(display, mb->win); redraw_canvas(mb); }
    }
    if (event->button == Button1) {
        // If we recently restored a window, suppress deactivation once
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

// Returns the WINDOW canvas immediately behind the given window in current Z order
static Canvas *find_next_window_behind(Window w) {
    Window root_return, parent_return, *children = NULL; unsigned int n = 0;
    if (!XQueryTree(display, root, &root_return, &parent_return, &children, &n)) return NULL;
    Canvas *result = NULL;
    // iterate top -> bottom
    for (int i = (int)n - 1; i >= 0; --i) {
        if (children[i] == w) {
            for (int j = i - 1; j >= 0; --j) {
                Canvas *c = find_canvas(children[j]);
                if (c && c->type == WINDOW) { result = c; break; }
            }
            break;
        }
    }
    if (children) XFree(children);
    return result;
}

static Bool handle_titlebar_buttons(Canvas *canvas, XButtonEvent *event) {
    if (event->y >= BORDER_HEIGHT_TOP) return False;
    int lower_x = canvas->width - BUTTON_LOWER_SIZE;
    int max_x = canvas->width - 2 * BUTTON_MAXIMIZE_SIZE;
    int iconify_x = canvas->width - 3 * BUTTON_ICONIFY_SIZE;

    if (event->x >= lower_x && event->button == Button1) {
        // Determine who was immediately behind BEFORE lowering
        Canvas *next = find_next_window_behind(canvas->win);

        // Send this window to the absolute back among WINDOWs (just above desktop)
        lower_window_to_back(canvas);
        // Update active state BEFORE compositor sync so the lowered window isn't re-raised
        canvas->active = false;
        if (next && next != canvas) set_active_window(next);
        else {
            // fallback: activate topmost other window or desktop
            activate_topmost_window_excluding(canvas);
        }
        // Now sync compositor stacking and repaint reflecting the new active
        compositor_sync_stacking(display);
        redraw_canvas(canvas);
        return True;
    } else if (event->x >= max_x && event->button == Button1) {
        canvas->x = 0; canvas->y = 21;
        XMoveResizeWindow(display, canvas->win, 0, 21,
                          get_desktop_canvas()->width,
                          get_desktop_canvas()->height - 20);
        return True;
    } else if (event->x >= iconify_x && event->button == Button1) {
        iconify_canvas(canvas); return True;
    } else if (event->x < BUTTON_CLOSE_SIZE && event->button == Button1) {
        destroy_canvas(canvas); return True;
    } else if (event->button == Button1) {
        dragging_canvas = canvas;
        drag_start_x = event->x_root; drag_start_y = event->y_root;
        window_start_x = canvas->x; window_start_y = canvas->y;
        return True;
    }
    return False;
}

static Bool handle_resize_button(Canvas *canvas, XButtonEvent *event) {
    if (event->x >= canvas->width - BORDER_WIDTH_RIGHT &&
        event->y >= canvas->height - BORDER_HEIGHT_BOTTOM &&
        event->button == Button1) {
        resizing_canvas = canvas;
        resize_start_x = event->x_root; resize_start_y = event->y_root;
        window_start_width = canvas->width; window_start_height = canvas->height;
        return True;
    }
    return False;
}

static Bool handle_scrollbars(Canvas *canvas, XButtonEvent *event) {
    if (canvas->client_win != None) return False; // no scrollbars on client windows

    int sb_x = canvas->width - BORDER_WIDTH_RIGHT;
    int sb_y = BORDER_HEIGHT_TOP + 10;
    int sb_w = BORDER_WIDTH_RIGHT;
    int sb_h = (canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM) - 54 - 10;
    int arrow_size = 20;

    // Vertical track
    if (event->x >= sb_x && event->x < sb_x + sb_w &&
        event->y >= sb_y && event->y < sb_y + sb_h &&
        event->button == Button1) {
        float ratio = (float)sb_h / canvas->content_height;
        int knob_h = max(MIN_KNOB_SIZE, (int)(ratio * sb_h));
        float pos_ratio = (float)canvas->scroll_y / canvas->max_scroll_y;
        int knob_y = sb_y + (int)(pos_ratio * (sb_h - knob_h));
        if (event->y >= knob_y && event->y < knob_y + knob_h) {
            scrolling_canvas = canvas; scrolling_vertical = true;
            initial_scroll = canvas->scroll_y; scroll_start_pos = event->y_root;
            return True;
        } else {
            float click_ratio = (float)(event->y - sb_y) / sb_h;
            canvas->scroll_y = max(0, min((int)(click_ratio * canvas->max_scroll_y), canvas->max_scroll_y));
            redraw_canvas(canvas); return True;
        }
    }
    // Vertical arrows
    if (event->x >= sb_x && event->x < sb_x + sb_w && event->button == Button1) {
        if (event->y >= (canvas->height - BORDER_HEIGHT_BOTTOM - (2 * arrow_size)) &&
            event->y < (canvas->height - BORDER_HEIGHT_BOTTOM - arrow_size)) {
            canvas->scroll_y = max(0, canvas->scroll_y - 20); redraw_canvas(canvas); return True;
        } else if (event->y >= (canvas->height - BORDER_HEIGHT_BOTTOM - arrow_size) &&
                   event->y < (canvas->height - BORDER_HEIGHT_BOTTOM)) {
            canvas->scroll_y = min(canvas->max_scroll_y, canvas->scroll_y + 20); redraw_canvas(canvas); return True;
        }
    }

    // Horizontal track
    int hb_x = BORDER_WIDTH_LEFT + 10;
    int hb_y = canvas->height - BORDER_HEIGHT_BOTTOM;
    int hb_w = (canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT) - 54 - 10;
    int hb_h = BORDER_HEIGHT_BOTTOM;
    if (event->x >= hb_x && event->x < hb_x + hb_w &&
        event->y >= hb_y && event->y < hb_y + hb_h &&
        event->button == Button1) {
        float ratio = (float)hb_w / canvas->content_width;
        int knob_w = max(MIN_KNOB_SIZE, (int)(ratio * hb_w));
        float pos_ratio = (float)canvas->scroll_x / canvas->max_scroll_x;
        int knob_x = hb_x + (int)(pos_ratio * (hb_w - knob_w));
        if (event->x >= knob_x && event->x < knob_x + knob_w) {
            scrolling_canvas = canvas; scrolling_vertical = false;
            initial_scroll = canvas->scroll_x; scroll_start_pos = event->x_root; return True;
        } else {
            float click_ratio = (float)(event->x - hb_x) / hb_w;
            canvas->scroll_x = max(0, min((int)(click_ratio * canvas->max_scroll_x), canvas->max_scroll_x));
            redraw_canvas(canvas); return True;
        }
    }
    // Horizontal arrows
    if (event->y >= hb_y && event->y < hb_y + hb_h && event->button == Button1) {
        if (event->x >= (canvas->width - BORDER_WIDTH_RIGHT - (2 * arrow_size)) &&
            event->x < (canvas->width - BORDER_WIDTH_RIGHT - arrow_size)) {
            canvas->scroll_x = max(0, canvas->scroll_x - 20); redraw_canvas(canvas); return True;
        } else if (event->x >= (canvas->width - BORDER_WIDTH_RIGHT - arrow_size) &&
                   event->x < (canvas->width - BORDER_WIDTH_RIGHT)) {
            canvas->scroll_x = min(canvas->max_scroll_x, canvas->scroll_x + 20); redraw_canvas(canvas); return True;
        }
    }
    return False;
}

// intuition_handle_button_press
static bool g_last_press_consumed = false;

bool intuition_last_press_consumed(void) { return g_last_press_consumed; }
bool intuition_is_scrolling_active(void) { return scrolling_canvas != NULL; }

void intuition_handle_button_press(XButtonEvent *event) {
    Canvas *canvas = find_canvas(event->window);
    if (!canvas) return;

    // Revert menubar to default state upon clicks outside menus
    if (canvas->type != MENU && (event->button == Button1 || event->button == Button3)) {
        if (get_show_menus_state()) {
            toggle_menubar_state();
            Canvas *mb = get_menubar();
            if (mb) { XMapWindow(display, mb->win); redraw_canvas(mb); }
            return;
        }
    }

    // Desktop: toggle menus on RMB, deactivate windows on empty LMB
    if (canvas->type == DESKTOP) { handle_desktop_button(canvas, event); redraw_canvas(canvas); g_last_press_consumed = true; return; }

    // window button processing past this point.
    // if canvas is not a window then stop here.
    if (canvas->type != WINDOW) 
        return;

    // Intentionally no compositor stack dump here (perf)

    set_active_window(canvas);
    if (handle_titlebar_buttons(canvas, event)) { g_last_press_consumed = true; return; }

    if (handle_resize_button(canvas, event)) { g_last_press_consumed = true; return; }

    if (handle_scrollbars(canvas, event)) { g_last_press_consumed = true; return; }

    g_last_press_consumed = false;
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
        
        // Only resize if change exceeds threshold (e.g., 5 pixels) 
        // to throttle micro-adjustments
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
        int delta, new_scroll;
        if (scrolling_vertical) {
            delta = event->y_root - scroll_start_pos;
            int sb_h = scrolling_canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM;
            float ratio = (scrolling_canvas->content_height > 0) ? ((float)sb_h / (float)scrolling_canvas->content_height) : 0.0f;
            int knob_h = max(MIN_KNOB_SIZE, (int)(ratio * sb_h));
            int track = max(1, sb_h - knob_h);
            // Map initial scroll to knob position, then apply delta in pixels
            float knob0 = (scrolling_canvas->max_scroll_y > 0) ? ((float)initial_scroll / (float)scrolling_canvas->max_scroll_y) * track : 0.0f;
            float knob  = min((float)track, max(0.0f, knob0 + (float)delta));
            new_scroll = (scrolling_canvas->max_scroll_y > 0) ? (int)roundf((knob / (float)track) * (float)scrolling_canvas->max_scroll_y) : 0;
            scrolling_canvas->scroll_y = new_scroll;

        } else {
            delta = event->x_root - scroll_start_pos;
            // Match horizontal track width used in handle_scrollbars()
            int sb_w = (scrolling_canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT) - 54 - 10;
            float ratio = (scrolling_canvas->content_width > 0) ? ((float)sb_w / (float)scrolling_canvas->content_width) : 0.0f;
            int knob_w = max(MIN_KNOB_SIZE, (int)(ratio * sb_w));
            int track = max(1, sb_w - knob_w);
            float knob0 = (scrolling_canvas->max_scroll_x > 0) ? ((float)initial_scroll / (float)scrolling_canvas->max_scroll_x) * track : 0.0f;
            float knob  = min((float)track, max(0.0f, knob0 + (float)delta));
            new_scroll = (scrolling_canvas->max_scroll_x > 0) ? (int)roundf((knob / (float)track) * (float)scrolling_canvas->max_scroll_x) : 0;
            scrolling_canvas->scroll_x = new_scroll;
        }
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
    // printf("getting destroy notify event ?\n");
    // Canvas *canvas = find_canvas_by_client(event->window);
    Canvas *canvas = find_canvas_by_client(event->window) ?: 
        find_canvas(event->window);

    if (!canvas) return;
    if (active_window == canvas) active_window = NULL;


    select_next_window(canvas);

    if (display && canvas->win != None) 
        XUnmapWindow(display, canvas->win);

    if (canvas->window_render != None) 
        XRenderFreePicture(display, canvas->window_render);

    if (canvas->canvas_render != None) 
        XRenderFreePicture(display, canvas->canvas_render);

    if (canvas->canvas_buffer != None) 
        XFreePixmap(display, canvas->canvas_buffer);

    if (canvas->colormap != None && canvas->type != DESKTOP) 
        XFreeColormap(display, canvas->colormap);

    if (canvas->win != None && canvas->type != DESKTOP) 
        XDestroyWindow(display, canvas->win);

    free(canvas->path); free(canvas->title);
    
    manage_canvases(false, canvas);

    // Remove any associated iconified icon from desktop
    remove_icon_for_canvas(canvas);  
    free(canvas);

    Canvas *desktop = get_desktop_canvas();
    if (desktop) redraw_canvas(desktop);
    XSync(display, False);
}

void intuition_handle_button_release(XButtonEvent *event) {
    dragging_canvas = NULL;
    resizing_canvas = NULL;
    scrolling_canvas = NULL;
    // Intentionally no compositor stack dump here (perf)
/*    XUngrabPointer(display, CurrentTime);
    printf("release button, do xungrab, reset scrolling, dragging...\n");*/
}

void intuition_handle_map_request(XMapRequestEvent *event) {
    XWindowAttributes attrs;
    bool attrs_valid = XGetWindowAttributes(display, event->window, &attrs);
    if (!attrs_valid) {
        attrs.x = 100; 
        attrs.y = 100; 
        attrs.width = 400; 
        attrs.height = 300;
        attrs.override_redirect = False; 
        attrs.class = InputOutput; 
        attrs.border_width = 0;
    }

    if (should_skip_framing(event->window, &attrs)) {
        XMapWindow(display, event->window);
        XSync(display, False);
        return;
    }

    int frame_x = max(attrs.x, 100);
    // Clamp y to ensure titlebar is below menubar
    int frame_y = max(attrs.y, MENUBAR_HEIGHT);  
    int frame_width = attrs.width + BORDER_WIDTH_LEFT + BORDER_WIDTH_RIGHT;
    int frame_height = attrs.height + BORDER_HEIGHT_TOP + BORDER_HEIGHT_BOTTOM;
    
    Canvas *frame = create_canvas(NULL, frame_x, frame_y, 
        frame_width, frame_height, WINDOW);
    
    if (!frame) {
        XMapWindow(display, event->window);
        return;
    }

    XReparentWindow(display, event->window, frame->win, 
        BORDER_WIDTH_LEFT, BORDER_HEIGHT_TOP);

    XSelectInput(display, event->window, 
        StructureNotifyMask | PropertyChangeMask);

    if (attrs.border_width != 0) {
        XWindowChanges bw_changes = {.border_width = 0};
        XConfigureWindow(display, event->window, CWBorderWidth, &bw_changes);
    }
    XMapWindow(display, event->window);
    frame->client_win = event->window;
    // Listen for structure and enter events on client (no grabs)
    XSelectInput(display, frame->client_win,
                 StructureNotifyMask | PropertyChangeMask | EnterWindowMask | FocusChangeMask);
    const char *dbg = getenv("WB_DEBUG");
    if (dbg && *dbg) fprintf(stderr, "[WB] map_request: framed client=0x%lx into frame=0x%lx at (%d,%d) size %dx%d\n",
                              (unsigned long)event->window, (unsigned long)frame->win, frame_x, frame_y, frame_width, frame_height);

    // ===========================
    // set client name on titlebar
    // ===========================
    if (frame->client_win != None) {
        XClassHint class_hint;
        XGetClassHint(display, frame->client_win, &class_hint); 
        if (class_hint.res_name)

            // Instance name (e.g., "xterm")
            frame->title = class_hint.res_name;  
        else
            frame->title = "NoNameApp";
    }
    //printf(" ---intuition:   frame->title=%s \n", frame->title);

    XAddToSaveSet(display, event->window);
    set_active_window(frame);
    XRaiseWindow(display, frame->win);
    compositor_sync_stacking(display);
    redraw_canvas(frame);
    XSync(display, False);
}

// Handle MapNotify for toplevel client windows that became viewable without a MapRequest
void intuition_handle_map_notify(XMapEvent *event) {
    // Ignore if this is one of our frame windows or already managed as client
    if (find_canvas(event->window) || find_canvas_by_client(event->window)) {
        return;
    }

    // Ensure it's a toplevel, viewable, input-output window and not override-redirect
    XWindowAttributes attrs;
    if (!XGetWindowAttributes(display, event->window, &attrs)) {
        return;
    }
    if (attrs.override_redirect || attrs.class == InputOnly) {
        return;
    }

    // Only handle if parent is root (toplevel)
    Window root_return, parent_return, *children = NULL;
    unsigned int nchildren = 0;
    if (!XQueryTree(display, event->window, &root_return, &parent_return, &children, &nchildren)) {
        return;
    }
    if (children) XFree(children);
    if (parent_return != root) {
        return;
    }

    if (should_skip_framing(event->window, &attrs)) {
        return;
    }

    // Compute frame geometry
    int frame_x = max(attrs.x, 100);
    int frame_y = max(attrs.y, MENUBAR_HEIGHT);
    int frame_width = attrs.width + BORDER_WIDTH_LEFT + BORDER_WIDTH_RIGHT;
    int frame_height = attrs.height + BORDER_HEIGHT_TOP + BORDER_HEIGHT_BOTTOM;

    Canvas *frame = create_canvas(NULL, frame_x, frame_y, frame_width, frame_height, WINDOW);
    if (!frame) return;

    // Reparent the already-mapped client into our frame and zero its border width to avoid truncation
    XReparentWindow(display, event->window, frame->win, BORDER_WIDTH_LEFT, BORDER_HEIGHT_TOP);
    XSelectInput(display, event->window, StructureNotifyMask | PropertyChangeMask);
    if (attrs.border_width != 0) {
        XWindowChanges bw_changes = { .border_width = 0 };
        XConfigureWindow(display, event->window, CWBorderWidth, &bw_changes);
    }

    frame->client_win = event->window;

    // Set title from class hint if available
    if (frame->client_win != None) {
        XClassHint class_hint;
        if (XGetClassHint(display, frame->client_win, &class_hint)) {
            if (class_hint.res_name) {
                frame->title = class_hint.res_name;
            } else {
                frame->title = "NoNameApp";
            }
        } else {
            frame->title = "NoNameApp";
        }
    }

    XAddToSaveSet(display, event->window);
    set_active_window(frame);
    XRaiseWindow(display, frame->win);
    compositor_sync_stacking(display);
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

        unsigned long safe_mask = event->value_mask &
            ~(CWStackMode | CWSibling);
        if (attrs.class == InputOnly) 
            safe_mask &= ~CWBorderWidth;

        if (!attrs_valid) 
            safe_mask &= ~CWBorderWidth;

        XWindowChanges changes = {0};
        if (safe_mask & CWX) changes.x = event->x;
        //if (safe_mask & CWY) changes.y = event->y;
        // Clamp y to ensure titlebar is below menubar
        if (safe_mask & CWY) changes.y = max(event->y, MENUBAR_HEIGHT);  
        if (safe_mask & CWWidth) changes.width = max(1, event->width);
        if (safe_mask & CWHeight) changes.height = max(1, event->height);

        if (attrs.class == InputOutput && (safe_mask & CWBorderWidth)) {
            bool need_set_border = false;
            
            if (event->value_mask & CWBorderWidth &&
                    event->border_width != 0) 
                need_set_border = true;
            
            if (attrs_valid && attrs.border_width != 0) 
                need_set_border = true;
            
            if (need_set_border) {
                changes.border_width = 0;
                safe_mask |= CWBorderWidth;
            }
        }

        if (safe_mask) XConfigureWindow(display, event->window, 
            safe_mask, &changes);

        XSync(display, False);
        return;
    }

    XWindowChanges frame_changes = {0};
    unsigned long frame_mask = 0;
    int new_frame_width = canvas->width, new_frame_height = canvas->height;

    if (event->value_mask & CWWidth) {

        frame_changes.width = max(1, event->width) + 
            BORDER_WIDTH_LEFT + BORDER_WIDTH_RIGHT;

        new_frame_width = frame_changes.width;
        frame_mask |= CWWidth;
    }

    if (event->value_mask & CWHeight) {
        frame_changes.height = max(1, event->height) + 
            BORDER_HEIGHT_TOP + BORDER_HEIGHT_BOTTOM;

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

    if ((event->value_mask & (CWStackMode | CWSibling)) == (CWStackMode |
            CWSibling) && event->detail >= 0 && event->detail <= 4) {

        XWindowAttributes sibling_attrs;
        if (XGetWindowAttributes(display, event->above, &sibling_attrs) &&
                sibling_attrs.map_state == IsViewable) {

            frame_changes.stack_mode = event->detail;
            frame_changes.sibling = event->above;
            frame_mask |= CWStackMode | CWSibling;
        }
    }

    if (frame_mask) 
        XConfigureWindow(display, canvas->win, frame_mask, &frame_changes);

    XWindowChanges client_changes = {0};
    unsigned long client_mask = 0;

    if (event->value_mask & CWWidth) { client_changes.width = 
            max(1, event->width); client_mask |= CWWidth; }

    if (event->value_mask & CWHeight) { client_changes.height = 
            max(1, event->height); client_mask |= CWHeight; }

    if (event->value_mask & CWBorderWidth) { 
        client_changes.border_width = 0; 
        client_mask |= CWBorderWidth; 
    }

    client_changes.x = BORDER_WIDTH_LEFT; 
    client_changes.y = BORDER_HEIGHT_TOP;
    client_mask |= CWX | CWY;

    if (client_mask) {
        XConfigureWindow(display, event->window, client_mask, &client_changes);
    }

    canvas->width = new_frame_width; canvas->height = new_frame_height;
    redraw_canvas(canvas);
    XSync(display, False);
}

void intuition_handle_configure_notify(XConfigureEvent *event) {
    Canvas *canvas = find_canvas(event->window);
    if (!canvas || (canvas->width == event->width && 
            canvas->height == event->height)) {
        return;
    }

    // Update size then recreate double-buffered surfaces in one place
    canvas->width = event->width; 
    canvas->height = event->height;
    render_recreate_canvas_surfaces(canvas);

    Display *dpy = get_display();
    if (canvas->client_win != None) {
        XWindowChanges changes = {.width = canvas->width - BORDER_WIDTH_LEFT -
            BORDER_WIDTH_RIGHT, .height = canvas->height - BORDER_HEIGHT_TOP -
            BORDER_HEIGHT_BOTTOM};
        XConfigureWindow(dpy, canvas->client_win, CWWidth | CWHeight, &changes);
    }

    if (canvas->type == WINDOW && canvas->client_win == None) {
        compute_max_scroll(canvas);
    }

    redraw_canvas(canvas);
    // allow natural batching, avoid extra XSync here
}

// resize desktop and menubar upon xrandr size changes
void intuition_handle_rr_screen_change(XRRScreenChangeNotifyEvent *event) {
    // Update global dimensions
    width = event->width;
    height = event->height;

    // Resize and redraw desktop
    Canvas *desktop = get_desktop_canvas();
    if (desktop) {
        // Set new size then recreate surfaces via helper
        desktop->width = width;
        desktop->height = height;  // full height; menubar overlays top
        render_recreate_canvas_surfaces(desktop);

        // reload/resize desktop background picture on resolution changes
        if (render_context->desk_img != None) {
            XFreePixmap(display, render_context->desk_img);
            if (strlen(DESKPICT) > 0) {
                render_context->desk_img = 
                load_wallpaper_to_pixmap(display, root, width, height, DESKPICT, DESKTILE);
            }
            // Avoid clearing the root to prevent flashes; desktop will redraw
        }
        redraw_canvas(desktop);
    }

    // Resize and redraw menubar
    Canvas *menubar_canvas = get_menubar();
    if (menubar_canvas) {
        // Height remains MENUBAR_HEIGHT
        menubar_canvas->width = width;
        XResizeWindow(display, menubar_canvas->win, width, MENUBAR_HEIGHT);
        menubar_canvas->height = MENUBAR_HEIGHT;
        render_recreate_canvas_surfaces(menubar_canvas);

        redraw_canvas(menubar_canvas);
    }

    XSync(display, False);
}

// ===================
// destroy and cleanup
// ===================
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
            ev.xclient.window = canvas->client_win; 
            ev.xclient.message_type = wm_protocols;
            ev.xclient.format = 32; ev.xclient.data.l[0] = wm_delete; 
            ev.xclient.data.l[1] = CurrentTime;
            XSendEvent(dpy, canvas->client_win, False, NoEventMask, &ev);
        } else XKillClient(dpy, canvas->client_win);
        XUnmapWindow(dpy, canvas->win);
        XUngrabServer(dpy);
        XSync(dpy, False);
        return;
    } else {

        // Unmap first to reflect in stacking
        XUnmapWindow(dpy, canvas->win);  
    
        // Restrict to WINDOW types to avoid activating on MENU destruction
        if (canvas->type == WINDOW) {  
            select_next_window(canvas);
        }

        if (canvas->window_render != None) 
            XRenderFreePicture(dpy, canvas->window_render);
        
        if (canvas->canvas_render != None) 
            XRenderFreePicture(dpy, canvas->canvas_render);
        
        if (canvas->canvas_buffer != None) 
            XFreePixmap(dpy, canvas->canvas_buffer);
        
        if (canvas->colormap != None) 
            XFreeColormap(dpy, canvas->colormap);
        
        if (canvas->win != None) 
            XDestroyWindow(dpy, canvas->win);

        free(canvas->path); 
        free(canvas->title);
        
        if (active_window == canvas) 
            active_window = NULL;

        manage_canvases(false, canvas);

        // Remove any associated iconified icon from desktop
        remove_icon_for_canvas(canvas);  
        free(canvas);
        Canvas *desktop = get_desktop_canvas();
        if (desktop) redraw_canvas(desktop);
    }
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
    printf("Called cleanup_intuition() \n");
}