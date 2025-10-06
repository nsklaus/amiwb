// Canvas creation/destruction/management
// This module handles Canvas lifecycle and compositing setup

#include "../config.h"
#include "itn_internal.h"
#include "../workbench/wb_public.h"
#include "../menus/menu_public.h"
#include <X11/Xlib.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xrandr.h>
#include <X11/cursorfont.h>
#include <X11/Xft/Xft.h>
#include <stdlib.h>
#include <string.h>

// External references (temporary during migration)
extern Display *display;
extern int screen;
extern Window root;
extern int width, height, depth;
extern Canvas **canvas_array;
extern int canvas_count;
extern int canvas_array_size;
extern Canvas *active_window;
extern bool fullscreen_active;
extern bool g_restarting;
extern bool g_shutting_down;
extern RenderContext *render_context;

// External functions from intuition.c (temporary during migration)
extern RenderContext *get_render_context(void);
extern bool is_window_valid(Display *dpy, Window win);
extern Display *get_display(void);
extern void send_x_command_and_sync(void);
extern void send_close_request_to_client(Window client);
// get_desktop_canvas is now itn_canvas_get_desktop (defined below)
extern bool get_window_attributes_safely(Window w, XWindowAttributes *a);
extern void clear_canvas_icons(Canvas *canvas);
extern void menubar_apply_fullscreen(bool fullscreen);
extern void remove_icon_for_canvas(Canvas *canvas);
extern void render_recreate_canvas_surfaces(Canvas *canvas);
extern void init_scroll(Canvas *canvas);
extern void compute_max_scroll(Canvas *canvas);
extern void calculate_content_area_inside_frame(Canvas *c, int *w, int *h);
extern void set_active_window(Canvas *canvas);
// find_canvas is now itn_canvas_find_by_window
extern void redraw_canvas(Canvas *canvas);
extern void restore_system_menu(void);
extern bool get_global_show_hidden_state(void);

// External compositing functions
extern bool itn_composite_init_overlay(void);
extern void itn_composite_cleanup_overlay(void);
extern void itn_composite_update_canvas_pixmap(Canvas *canvas);

#define INITIAL_CANVAS_CAPACITY 16

// ============================================================================
// Canvas Array Management (will be migrated to linked list later)
// ============================================================================

static Canvas *add_new_canvas_to_array(void) {
    // Expand array if needed
    if (canvas_count >= canvas_array_size) {
        int new_size = canvas_array_size ? canvas_array_size * 2 : INITIAL_CANVAS_CAPACITY;
        Canvas **expanded_array = realloc(canvas_array, new_size * sizeof(Canvas *));
        if (!expanded_array) {
            log_error("[ERROR] realloc failed for canvas_array (new size=%d) - canvas creation failed", new_size);
            return NULL;  // Graceful degradation: can't create new canvas
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

// Note: remove_canvas_from_array() is now exported from intuition.c

// Manage canvas array - either add new canvas or remove existing one
static Canvas *manage_canvases(bool should_add_canvas, Canvas *canvas_to_remove) {
    if (should_add_canvas) return add_new_canvas_to_array();
    if (canvas_to_remove) remove_canvas_from_array(canvas_to_remove);
    return NULL;
}

// ============================================================================
// Visual and Window Setup Helpers
// ============================================================================

// Choose appropriate visual and depth for different canvas types
static void choose_visual_for_canvas_type(CanvasType canvas_type, XVisualInfo *visual_info) {
    if (canvas_type == DESKTOP) {
        // Desktop uses default visual/depth
        visual_info->visual = DefaultVisual(display, screen);
        visual_info->depth = DefaultDepth(display, screen);
    } else {
        // Try to match GLOBAL_DEPTH with TrueColor visual
        if (!XMatchVisualInfo(display, screen, GLOBAL_DEPTH, TrueColor, visual_info)) {
            // Fallback to default visual if GLOBAL_DEPTH not available
            visual_info->visual = DefaultVisual(display, screen);
            visual_info->depth = DefaultDepth(display, screen);
        }
        // visual_info is now properly set either by XMatchVisualInfo or to defaults
    }
}

// Get X11 event mask appropriate for each canvas type
static long get_event_mask_for_canvas_type(CanvasType canvas_type) {
    long base_events = ExposureMask | ButtonPressMask | PointerMotionMask | ButtonReleaseMask | KeyPressMask;

    if (canvas_type == DESKTOP)
        return base_events | SubstructureRedirectMask | SubstructureNotifyMask;
    if (canvas_type == WINDOW)
        // Need SubstructureRedirectMask to intercept client resize attempts!
        return base_events | StructureNotifyMask | SubstructureNotifyMask |
               SubstructureRedirectMask | EnterWindowMask | FocusChangeMask;
    if (canvas_type == MENU)
        return base_events | PointerMotionMask | ButtonPressMask | ButtonReleaseMask;

    return base_events;
}

// ============================================================================
// Canvas Metadata and State Initialization
// ============================================================================

static void init_canvas_metadata(Canvas *c, const char *path, CanvasType t,
                                 int x, int y, int w, int h) {
    *c = (Canvas){0}; c->type = t;

    // Duplicate path if provided
    if (path) {
        c->path = strdup(path);
        if (!c->path) {
            log_error("[ERROR] strdup failed for canvas path: %s - canvas will have no path", path);
            c->path = NULL;  // Graceful degradation: canvas works without path
        }
    } else {
        c->path = NULL;
    }

    // Extract and duplicate the title base (filename from path)
    if (path) {
        const char *basename = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
        c->title_base = strdup(basename);
        if (!c->title_base) {
            log_error("[ERROR] strdup failed for canvas title_base: %s - will use 'Untitled'", basename);
            c->title_base = NULL;  // Graceful degradation: other code uses "Untitled" as fallback
        }
    } else {
        c->title_base = NULL;
    }

    // Handle empty title_base case
    if (c->title_base && strlen(c->title_base) == 0) {
        free(c->title_base);  // Free the empty string before replacing
        c->title_base = strdup("System");
        if (!c->title_base) {
            log_error("[ERROR] strdup failed for default title 'System' - will use 'Untitled'");
            c->title_base = NULL;  // Graceful degradation: other code uses "Untitled" as fallback
        }
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

// ============================================================================
// X11 Window and Visual Setup
// ============================================================================

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

// ============================================================================
// XRender Picture Initialization
// ============================================================================

static Bool init_render_pictures(Canvas *c, CanvasType t) {
    RenderContext *ctx = get_render_context(); if (!ctx) return False;
    // XRenderFindVisualFormat: Get the pixel format for our visual
    // This tells XRender how to interpret the pixel data (RGB layout, alpha channel, etc.)
    XRenderPictFormat *fmt = XRenderFindVisualFormat(ctx->dpy, c->visual);
    if (!fmt) {
        log_error("[ERROR] XRenderFindVisualFormat failed for visual=%p", (void*)c->visual);
        return False;
    }

    // CRITICAL: Verify format depth matches canvas depth to prevent BadMatch
    // The pixmap was created with c->depth, so the Picture format must match
    if (fmt->depth != c->depth) {
        log_error("[ERROR] Format depth mismatch: fmt->depth=%d, canvas->depth=%d",
                  fmt->depth, c->depth);
        return False;
    }

    // XRenderCreatePicture: Create a "Picture" - XRender's drawable surface
    // Unlike raw pixmaps, Pictures support alpha blending and transformations
    // This one is for our off-screen buffer where we compose the window content
    c->canvas_render = XRenderCreatePicture(ctx->dpy, c->canvas_buffer, fmt, 0, NULL);
    if (!c->canvas_render) {
        log_error("[ERROR] XRenderCreatePicture failed for canvas_buffer");
        return False;
    }
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

// ============================================================================
// Focus Management Helper
// ============================================================================

// Select next window after closing
void select_next_window(Canvas *closing_canvas) {  // Exported for intuition.c
    if (active_window == closing_canvas) active_window = NULL;

    Window root_return, parent_return;
    Window *children;
    unsigned int nchildren;
    if (XQueryTree(display, root, &root_return, &parent_return,
            &children, &nchildren)) {

        for (int i = nchildren - 1; i >= 0; i--) {  // Top to bottom
            if (children[i] == closing_canvas->win)
                continue;

            Canvas *next_canvas = itn_canvas_find_by_window(children[i]);
            if (next_canvas && next_canvas->type == WINDOW) {
                set_active_window(next_canvas);
                break;
            }
        }
        XFree(children);
    }

    // Fallback if none
    if (!active_window) {
        active_window = itn_canvas_get_desktop();
    }
}

// ============================================================================
// Public Canvas Creation API
// ============================================================================

Canvas *itn_canvas_create(Window client, XWindowAttributes *attrs) {
    // This is the compositor-style creation - used when migrating from compositor.c
    Canvas *canvas = calloc(1, sizeof(Canvas));
    if (!canvas) return NULL;

    canvas->client_win = client;
    if (attrs) {
        canvas->x = attrs->x;
        canvas->y = attrs->y;
        canvas->width = attrs->width;
        canvas->height = attrs->height;
        canvas->depth = attrs->depth;
        canvas->comp_mapped = (attrs->map_state != IsUnmapped);
    }

    canvas->comp_opacity = 1.0;
    canvas->comp_visible = true;
    canvas->type = WINDOW;

    return canvas;
}

// Main canvas creation with client window
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
    // Initialize compositor state
    canvas->comp_opacity = 1.0;
    canvas->comp_visible = true;  // Visible by default, can be toggled (e.g., menubar during fullscreen)
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
        itn_canvas_destroy(canvas);
        return NULL;
    }

    if (!init_render_pictures(canvas, type)) {
        itn_canvas_destroy(canvas);
        return NULL;
    }

    // Create XftDraw and ensure all render surfaces are properly initialized
    render_recreate_canvas_surfaces(canvas);

    init_scroll(canvas);

    // Setup compositing if active
    itn_canvas_setup_compositing(canvas);

    if (type != DESKTOP) {
        if (type == WINDOW ) {
            XSetWindowAttributes attrs = {0};
            attrs.background_pixmap = None;
            XChangeWindowAttributes(display, canvas->win, CWBackPixmap, &attrs);
            // Use damage accumulation instead of immediate redraw
            DAMAGE_CANVAS(canvas);
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
        // Use damage accumulation instead of immediate redraw
        DAMAGE_CANVAS(canvas);
        XSync(ctx->dpy, False);
    }

    // Setup compositing for this canvas (if compositor is active)
    extern bool g_compositor_active;
    if (g_compositor_active) {
        itn_composite_setup_canvas(canvas);
    }

    // Schedule frame render for initial display
    SCHEDULE_FRAME();

    return canvas;
}

// Simplified creation without client
Canvas *create_canvas(const char *path, int x, int y, int width,
        int height, CanvasType type) {
    return create_canvas_with_client(path, x, y, width, height, type, None);
}

// ============================================================================
// Canvas Destruction
// ============================================================================

// OWNERSHIP: Frees all Canvas X11 resources (Window, XftDraw, compositing), and canvas struct
void itn_canvas_destroy(Canvas *canvas) {
    if (!canvas || canvas->type == DESKTOP) return;
    clear_canvas_icons(canvas);

    // If destroying a fullscreen window, restore menubar
    if (canvas->fullscreen) {
        canvas->fullscreen = false;
        fullscreen_active = False;
        menubar_apply_fullscreen(false);
    }

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

    // CRITICAL: Clear dangling canvas pointers in menu system before destroying
    // When a menu dropdown canvas is destroyed (compositor cleanup, etc.), the Menu
    // structure's canvas pointer must be NULLed to prevent double-free on reopen
    if (canvas->type == MENU) {
        Menu *active = get_active_menu();
        if (active && active->canvas == canvas) {
            active->canvas = NULL;
        }

        // Also check nested menu (extern because it's static in menu_core.c)
        extern Menu *nested_menu;
        if (nested_menu && nested_menu->canvas == canvas) {
            nested_menu->canvas = NULL;
        }
    }

    Display *dpy = get_display();

    // Cleanup compositing resources first
    itn_canvas_cleanup_compositing(canvas);

    // If this canvas frames a client, handle it appropriately
    if (canvas->client_win != None) {
        XGrabServer(dpy);

        if (g_restarting) {
            // Restarting - preserve client by unparenting back to root
            XReparentWindow(dpy, canvas->client_win, root,
                          canvas->x + BORDER_WIDTH_LEFT,
                          canvas->y + BORDER_HEIGHT_TOP);
            XRemoveFromSaveSet(dpy, canvas->client_win);
        } else {
            // Normal operation - request client to close
            send_close_request_to_client(canvas->client_win);
        }

        XUngrabServer(dpy);
        XSync(dpy, False);

        if (g_restarting && canvas->client_win != None) {
            // Map client on root so it's visible after restart
            XMapWindow(dpy, canvas->client_win);
            XSync(dpy, False);
        }

        // XUnmapWindow: Hide the frame window from screen
        safe_unmap_window(dpy, canvas->win);
        // XSync: Force all X11 commands to complete before continuing
        // Without this, commands might queue up and execute out of order
        send_x_command_and_sync();
        canvas->client_win = None;
    }

    // Clean up compositor damage tracking BEFORE destroying windows
    itn_canvas_cleanup_compositing(canvas);

    // Update focus/activation before tearing down resources
    if (canvas->type == WINDOW) {
        select_next_window(canvas);
    }

    // Free X11 resources in safe order
    send_x_command_and_sync();

    // Critical: Always clean up XftDraw to prevent crash during XCloseDisplay
    // XftDraw objects reference fonts, must be freed before font cleanup
    if (dpy && canvas->xft_draw) {
        XftDrawDestroy(canvas->xft_draw);
        canvas->xft_draw = NULL;
    }

    // Skip other X11 operations if shutting down or display is invalid
    if (!g_shutting_down && dpy) {
        // Free pre-allocated Xft colors
        if (canvas->xft_colors_allocated) {
            XftColorFree(dpy, canvas->visual, canvas->colormap, &canvas->xft_black);
            XftColorFree(dpy, canvas->visual, canvas->colormap, &canvas->xft_white);
            XftColorFree(dpy, canvas->visual, canvas->colormap, &canvas->xft_blue);
            XftColorFree(dpy, canvas->visual, canvas->colormap, &canvas->xft_gray);
            canvas->xft_colors_allocated = false;
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

    // Damage entire desktop after window removal
    Canvas *desktop = itn_canvas_get_desktop();
    if (desktop) {
        DAMAGE_CANVAS(desktop);
        SCHEDULE_FRAME();
    }
}

// ============================================================================
// Canvas Finding Functions
// ============================================================================

Canvas *itn_canvas_get_desktop(void) {
    // Desktop is always the first canvas (canvas_array[0])
    if (canvas_array && canvas_count > 0) return canvas_array[0];
    return NULL;
}

Canvas *itn_canvas_find_by_window(Window win) {
    for (Canvas *c = g_canvas_list; c; c = c->next) {
        if (c->win == win) return c;
    }
    // Fall back to array search during migration
    if (canvas_array) {  // Guard against init failure
        for (int i = 0; i < canvas_count; i++) {
            if (canvas_array[i]->win == win) return canvas_array[i];
        }
    }
    return NULL;
}

Canvas *itn_canvas_find_by_client(Window client) {
    for (Canvas *c = g_canvas_list; c; c = c->next) {
        if (c->client_win == client) return c;
    }
    // Fall back to array search during migration
    if (canvas_array) {  // Guard against init failure
        for (int i = 0; i < canvas_count; i++) {
            if (canvas_array[i]->client_win == client) return canvas_array[i];
        }
    }
    return NULL;
}

// ============================================================================
// Canvas List Management (for compositor integration)
// ============================================================================

void itn_canvas_manage_list(Canvas *canvas, bool add) {
    if (!canvas) return;

    if (add) {
        // Add to front of list
        canvas->next = g_canvas_list;
        g_canvas_list = canvas;
    } else {
        // Remove from list
        Canvas **prev = &g_canvas_list;
        while (*prev) {
            if (*prev == canvas) {
                *prev = canvas->next;
                canvas->next = NULL;
                break;
            }
            prev = &(*prev)->next;
        }
    }
}

// ============================================================================
// Compositing Setup
// ============================================================================

void itn_canvas_setup_compositing(Canvas *canvas) {
    if (!canvas || !g_compositor_active) return;

    Display *dpy = itn_core_get_display();
    if (!dpy) return;

    // Create damage tracking for this window
    if (canvas->client_win) {
        // For client windows, use RawRectangles to ensure continuous damage reporting
        // DeltaRectangles stops sending events when damage accumulates faster than we clear it
        canvas->comp_damage = XDamageCreate(dpy, canvas->client_win, XDamageReportRawRectangles);
    } else if (canvas->win) {
        // For windows without clients, track damage on the frame itself
        canvas->comp_damage = XDamageCreate(dpy, canvas->win, XDamageReportRawRectangles);
    }

    // Mark canvas for initial rendering
    canvas->comp_needs_repaint = true;
    canvas->comp_damage_bounds = (XRectangle){0, 0, canvas->width, canvas->height};
}

// Temporary error handler to suppress BadDamage errors
static int ignore_bad_damage(Display *dpy, XErrorEvent *error) {
    if (error->error_code == 152) {  // BadDamage error code
        // Silently ignore BadDamage errors during cleanup
        return 0;
    }
    // Call the default error handler for other errors
    extern int x_error_handler(Display *dpy, XErrorEvent *error);
    return x_error_handler(dpy, error);
}

void itn_canvas_cleanup_compositing(Canvas *canvas) {
    if (!canvas || !g_compositor_active) return;

    Display *dpy = itn_core_get_display();
    if (!dpy) return;

    // When a client window destroys itself, X11 automatically destroys its damage
    // We need to trap errors when trying to destroy it ourselves
    if (canvas->comp_damage) {
        // Install temporary error handler to ignore BadDamage
        XErrorHandler old_handler = XSetErrorHandler(ignore_bad_damage);

        // Try to destroy - if already gone, error is silently ignored
        XDamageDestroy(dpy, canvas->comp_damage);
        XSync(dpy, False);  // Force execution to catch any errors now

        // Restore original error handler
        XSetErrorHandler(old_handler);
        canvas->comp_damage = 0;
    }

    if (canvas->comp_picture) {
        XRenderFreePicture(dpy, canvas->comp_picture);
        canvas->comp_picture = 0;
    }

    if (canvas->comp_pixmap) {
        XFreePixmap(dpy, canvas->comp_pixmap);
        canvas->comp_pixmap = 0;
    }
}

// Iconify a window - hide it and create desktop icon
void iconify_canvas(Canvas *canvas) {
    if (!canvas || canvas->type != WINDOW) return;

    Display *dpy = itn_core_get_display();
    if (!dpy) return;

    // Check if this window owns the app menu before iconifying
    extern Window get_app_menu_window(void);
    extern void restore_system_menu(void);
    bool was_menu_owner = (canvas->client_win == get_app_menu_window());

    // Hide the window
    safe_unmap_window(dpy, canvas->win);

    // Mark as not visible for compositor
    canvas->comp_visible = false;
    canvas->comp_mapped = false;

    // Create an iconified icon on desktop
    create_iconified_icon(canvas);

    // Damage desktop to show new icon
    Canvas *desktop = itn_canvas_get_desktop();
    if (desktop) {
        DAMAGE_CANVAS(desktop);
    }

    // Iconified window loses active state - activate next window
    // This also handles menu restoration automatically via focus change
    if (canvas->active) {
        canvas->active = false;
        itn_focus_select_next(canvas);
    } else if (was_menu_owner) {
        // Even if not active, if it owned menus, restore system menu
        restore_system_menu();
    }

    SCHEDULE_FRAME();
}

// Request a client window to close gracefully
void request_client_close(Canvas *canvas) {
    if (!canvas) return;

    Display *dpy = itn_core_get_display();
    if (!dpy) return;

    // For workbench windows (no client), destroy directly
    if (!canvas->client_win) {
        itn_canvas_destroy(canvas);
        return;
    }

    // Send WM_DELETE_WINDOW protocol to client windows
    Atom wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);

    XEvent event;
    event.type = ClientMessage;
    event.xclient.window = canvas->client_win;
    event.xclient.message_type = wm_protocols;
    event.xclient.format = 32;
    event.xclient.data.l[0] = wm_delete;
    event.xclient.data.l[1] = CurrentTime;

    XSendEvent(dpy, canvas->client_win, False, NoEventMask, &event);
    XFlush(dpy);

    canvas->close_request_sent = true;
}

// Check if a window should be framed
bool should_skip_framing(Window win, XWindowAttributes *attrs) {
    if (!attrs) return true;

    // Skip override-redirect windows
    if (attrs->override_redirect) return true;

    // Skip InputOnly windows
    if (attrs->class == InputOnly) return true;

    // Skip windows we already manage
    if (itn_canvas_find_by_window(win)) return true;
    if (itn_canvas_find_by_client(win)) return true;

    return false;
}

// Frame an existing client window
Canvas *frame_client_window(Window client, XWindowAttributes *attrs) {
    if (!client || !attrs) return NULL;

    // Calculate frame dimensions from client size
    int frame_width = attrs->width + BORDER_WIDTH_LEFT + BORDER_WIDTH_RIGHT_CLIENT;  // 8+8=16px
    int frame_height = attrs->height + BORDER_HEIGHT_TOP + BORDER_HEIGHT_BOTTOM;     // 20+20=40px

    // Position frame accounting for border offset (client will be reparented at offset BORDER inside frame)
    // This prevents accumulation during hot-restart: client at (X,Y) should end up at (X,Y) after framing
    int frame_x = attrs->x - BORDER_WIDTH_LEFT;
    int frame_y = max(MENUBAR_HEIGHT, attrs->y - BORDER_HEIGHT_TOP);

    // Use the proper canvas creation function that initializes render surfaces
    Canvas *frame = create_canvas_with_client(NULL, frame_x, frame_y,
                                              frame_width, frame_height,
                                              WINDOW, client);
    if (!frame) return NULL;

    // Reparent the client window into our frame
    Display *dpy = itn_core_get_display();
    if (dpy) {
        XReparentWindow(dpy, client, frame->win, BORDER_WIDTH_LEFT, BORDER_HEIGHT_TOP);

        // Ensure client has no border
        XSetWindowBorderWidth(dpy, client, 0);

        // Setup event selection on client
        XSelectInput(dpy, client, PropertyChangeMask | StructureNotifyMask);

        // Grab button clicks for click-to-focus behavior
        // This intercepts clicks even when client has ButtonPressMask set
        // GrabModeSync + XAllowEvents(ReplayPointer) ensures client still receives the click
        XGrabButton(dpy, Button1, AnyModifier, client,
                   False, ButtonPressMask, GrabModeSync, GrabModeAsync,
                   None, None);

        // Get window title from class hint (application name)
        // Use res_class (app class like "Firefox", "Kitty") as primary source
        // This is needed for iconify system to match application icons
        XClassHint class_hint;
        if (XGetClassHint(dpy, client, &class_hint)) {
            if (class_hint.res_class) {
                frame->title_base = strdup(class_hint.res_class);
            } else if (class_hint.res_name) {
                frame->title_base = strdup(class_hint.res_name);
            }
            // Free X11 allocated strings
            if (class_hint.res_class) XFree(class_hint.res_class);
            if (class_hint.res_name) XFree(class_hint.res_name);
        }

        // Final fallback
        if (!frame->title_base) {
            frame->title_base = strdup("NoNameApp");
        }
    }

    return frame;
}

// Frame all existing windows at startup
void frame_existing_client_windows(void) {
    Display *dpy = itn_core_get_display();
    if (!dpy) return;

    Window root_return, parent_return;
    Window *children;
    unsigned int nchildren;

    if (XQueryTree(dpy, DefaultRootWindow(dpy), &root_return, &parent_return,
                   &children, &nchildren)) {
        for (unsigned int i = 0; i < nchildren; i++) {
            XWindowAttributes attrs;
            if (!safe_get_window_attributes(dpy, children[i], &attrs)) {
                continue;
            }

            if (attrs.map_state == IsViewable && !should_skip_framing(children[i], &attrs)) {
                frame_client_window(children[i], &attrs);
            }
        }

        if (children) XFree(children);
    }
}

// Check if a window is viewable
bool is_viewable_client(Window win) {
    Display *dpy = itn_core_get_display();
    if (!dpy || !win) return false;

    XWindowAttributes attrs;
    if (!safe_get_window_attributes(dpy, win, &attrs)) return false;

    return (attrs.map_state == IsViewable &&
            attrs.class == InputOutput &&
            !attrs.override_redirect);
}

// Check if window is direct child of root
bool is_toplevel_under_root(Window win) {
    Display *dpy = itn_core_get_display();
    if (!dpy || !win) return false;

    Window root_return, parent_return;
    Window *children;
    unsigned int nchildren;

    if (!XQueryTree(dpy, win, &root_return, &parent_return, &children, &nchildren)) {
        return false;
    }

    if (children) XFree(children);

    return (parent_return == DefaultRootWindow(dpy));
}