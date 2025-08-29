// File: intuition.h
// Core window/canvas management for AmiWB. Provides the `Canvas` type,
// render context, and the event-facing API used by the dispatcher.
#ifndef INTUITION_H
#define INTUITION_H

#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xrandr.h>
#include <X11/Xft/Xft.h>
#include <stdbool.h>
#include <Imlib2.h>

// UI metrics (frame and menubar sizes). Keep short to match Amiga style.
#define MENUBAR_HEIGHT 20
#define BORDER_WIDTH_LEFT 8
#define BORDER_WIDTH_RIGHT 20
#define BORDER_HEIGHT_TOP 20
#define BORDER_HEIGHT_BOTTOM 20
#define BG_COLOR_DESKTOP 0xFF888888
#define BG_COLOR_FOLDER 0xFFFFFFFF

// View mode for workbench windows
typedef enum { VIEW_ICONS = 0, VIEW_NAMES = 1 } ViewMode;

typedef enum CanvasType { DESKTOP, WINDOW, MENU, DIALOG } CanvasType;

// Global render context shared by renderer and UI subsystems.
// Holds Display, XRender format, and wallpaper pixmaps.
typedef struct {
    Display *dpy;                 // X11 display connection
    XRenderPictFormat *fmt;       // XRender visual format
    Pixmap desk_img;              // desktop background pic
    Pixmap wind_img;              // windows background pic 
    int desk_img_w, desk_img_h;
    int wind_img_w, wind_img_h;
    Picture desk_picture;         // Cached XRender Picture for desktop wallpaper
    Picture wind_picture;         // Cached XRender Picture for window wallpaper
    Pixmap checker_active_pixmap;  // Cached 4x4 checkerboard pattern for active windows (blue/black)
    Picture checker_active_picture; // Cached XRender Picture for active window checkerboard
    Pixmap checker_inactive_pixmap;  // Cached 4x4 checkerboard pattern for inactive windows (gray/black)
    Picture checker_inactive_picture; // Cached XRender Picture for inactive window checkerboard
    int default_screen;           // Cached DefaultScreen(dpy)
    Visual *default_visual;       // Cached DefaultVisual(dpy, default_screen)
    Colormap default_colormap;    // Cached DefaultColormap(dpy, default_screen)
} RenderContext;

// Canvas describes any drawable AmiWB surface:
// DESKTOP root, WINDOW frames, and MENU popups/menubar.
typedef struct {
    CanvasType type;                    // Canvas type: DESKTOP, WINDOW, MENU
    Window win;                         // X11 window
    Window client_win;                  // Client window (if any)
    Visual *visual;                     // Client visual (if any)
    Pixmap canvas_buffer;               // Pixel buffer for storing canvas content
    Picture canvas_render;              // XRender pic for compositing content onto buffer
    Picture window_render;              // XRender picture for displaying content to window
    char *path;                         // Directory path (NULL for menus)
    char *title_base;                   // Base app name (never changes, used for iconify)
    char *title_change;                 // Dynamic display title (if NULL, render uses title_base)
    int x, y;                           // Position
    int width, height;                  // Dimensions
    int buffer_width, buffer_height;    // Actual buffer dimensions (may be larger)
    int scroll_x, scroll_y;             // Scroll offsets
    int max_scroll_x, max_scroll_y;     // Max scroll limits
    int content_width, content_height;  // Content dimensions
    int depth;                          // Visual depth (for pixmap recreation during resize)
    XRenderColor bg_color;              // Background color
    ViewMode view_mode;                 // View mode (icons or names) for WINDOW canvases
    bool active;                        // Active state
    Colormap colormap;                  // Colormap for the canvas
    bool scanning;                      // True while directory scan is in progress
    bool show_hidden;                   // Show dotfiles in directory views
    // Fullscreen support
    bool fullscreen;                    // EWMH fullscreen active
    int saved_x, saved_y;               // Saved frame pos before fullscreen
    int saved_w, saved_h;               // Saved frame size before fullscreen
    // Maximize toggle support
    bool maximized;                     // True if window is currently maximized
    int pre_max_x, pre_max_y;           // Saved position before maximize
    int pre_max_w, pre_max_h;           // Saved dimensions before maximize
    bool close_armed;                    // Close gadget pressed (until release)
    bool iconify_armed;                  // Iconify gadget pressed (until release)
    bool maximize_armed;                  // Maximize gadget pressed (until release)
    bool lower_armed;                     // Lower gadget pressed (until release)
    bool v_arrow_up_armed;               // Vertical up arrow pressed
    bool v_arrow_down_armed;             // Vertical down arrow pressed
    bool h_arrow_left_armed;             // Horizontal left arrow pressed
    bool h_arrow_right_armed;            // Horizontal right arrow pressed
    bool resize_armed;                   // Resize button pressed
    bool resizing_interactive;           // True during interactive resize (no buffer recreation)
    bool is_transient;                   // True if this is a transient window (modal dialog)
    Window transient_for;                // Parent window for transient windows
    bool close_request_sent;             // True if WM_DELETE_WINDOW was sent (for single-click close)
    int consecutive_unmaps;              // Count of unmaps without remap (for zombie detection)
    bool cleanup_scheduled;              // True when dialog frame should be cleaned up
    bool disable_scrollbars;             // True to disable scrollbar rendering (for dialogs)
    XftDraw *xft_draw;                    // Cached XftDraw for text rendering (avoids recreation)
    // Pre-allocated Xft colors to avoid repeated allocation in render loops
    XftColor xft_black;                   // Pre-allocated black color
    XftColor xft_white;                   // Pre-allocated white color
    XftColor xft_blue;                    // Pre-allocated blue (selection) color
    XftColor xft_gray;                    // Pre-allocated gray color (for disabled items)
    bool xft_colors_allocated;            // True if Xft colors have been allocated
    // Damage tracking - only redraw changed regions instead of entire canvas
    bool needs_redraw;                    // True if any part needs redrawing
    int dirty_x, dirty_y;                 // Top-left corner of damaged region
    int dirty_w, dirty_h;                 // Size of damaged region
    // Window size constraints (from XSizeHints or defaults)
    int min_width, min_height;            // Minimum window size
    int max_width, max_height;            // Maximum window size (0 = no limit)
    bool resize_x_allowed;                // Allow horizontal resize
    bool resize_y_allowed;                // Allow vertical resize
} Canvas;

extern int randr_event_base;

// Function prototypes
RenderContext *get_render_context(void);    // Getter for global RenderContext

Display *get_display(void);                 // Getter for X Display
Canvas  *init_intuition(void);              // Init Display, visuals, desktop canvas
Canvas  *get_desktop_canvas(void);          // Get desktop canvas
Canvas  *find_canvas(Window win);           // Find canvas by its frame window

// Canvas array management (for timer loop access)
extern Canvas **canvas_array;
extern int canvas_count;
Canvas  *create_canvas(const char *path, int x, int y, int width, int height, CanvasType type);
Canvas  *create_canvas_with_client(const char *path, int x, int y, int width, int height, CanvasType type, Window client_win); 
Canvas  *get_active_window(void);           // Currently active WINDOW canvas
Canvas  *find_canvas_by_client(Window client_win); // Lookup by client window
Canvas  *find_window_by_path(const char *path);   // Find WINDOW by directory path

void destroy_canvas(Canvas *canvas);        // Destroy canvas and free X resources
void cleanup_intuition(void);               // Free visuals, desktop, Display
void set_active_window(Canvas *canvas);     // Activate one, deactivate others
void deactivate_all_windows(void);          // Deactivate all windows
// Temporarily suppress desktop deactivation on empty clicks (used after restore)
void suppress_desktop_deactivate_for_ms(int ms);

// Window cycling functions
void cycle_next_window(void);               // Cycle to next window (Super+M)
void cycle_prev_window(void);               // Cycle to previous window (Super+Shift+M)

// Compute scroll limits from content and clamp current scroll.
void compute_max_scroll(Canvas *canvas);

// Update max constraints when screen resolution changes
void update_canvas_max_constraints(void);

// Iconify a workbench canvas window (hide window and create desktop icon)
void iconify_canvas(Canvas *canvas);

// Enter global shutdown mode (suppress X errors during teardown)
void begin_shutdown(void);

// Enter global restart mode (preserve clients during restart)
void begin_restart(void);

// Install the X error handler early; respects AMIWB_DEBUG_XERRORS for verbose suppression logs
void install_error_handler(void);

// Query helpers for event routing
bool intuition_last_press_consumed(void);   // Last press handled by frame widgets
bool intuition_is_scrolling_active(void);   // True while dragging a scrollbar knob



// Event handlers called by central dispatcher
// (workbench/events.c forwards here for frame-related actions)
void intuition_handle_expose(XExposeEvent *event);                
void intuition_handle_button_press(XButtonEvent *event);          
void intuition_handle_button_release(XButtonEvent *event);

void intuition_handle_map_request(XMapRequestEvent *event);       
void intuition_handle_map_notify(XMapEvent *event);
void intuition_handle_configure_request(XConfigureRequestEvent *event); 

void intuition_handle_property_notify(XPropertyEvent *event);
void intuition_handle_motion_notify(XMotionEvent *event);
void intuition_handle_destroy_notify(XDestroyWindowEvent *event); 
void intuition_handle_configure_notify(XConfigureEvent *event);   // for resizing
void intuition_handle_rr_screen_change(XRRScreenChangeNotifyEvent *event);
void intuition_handle_client_message(XClientMessageEvent *event);


// Fullscreen helpers
void intuition_enter_fullscreen(Canvas *c);
void intuition_exit_fullscreen(Canvas *c);

// Use indices for pointers
/*extern int active_window_index;     // -1 for none
extern int dragging_canvas_index;   // -1 for none
extern int resizing_canvas_index;   // -1 for none*/
// Old resize global removed - now using clean resize.c module

// Helper function to determine the actual right border width for a window
static inline int get_right_border_width(const Canvas *canvas) {
    // Border sizes differ based on window type:
    // - Workbench windows (file manager): 20px all borders (for scrollbar)
    // - Client windows & dialogs: 8px left/right, 20px top/bottom
    if (canvas->type == WINDOW && 
        canvas->client_win == None && 
        !canvas->disable_scrollbars) {
        return BORDER_WIDTH_RIGHT;  // 20px for workbench windows with scrollbar
    }
    return 8;  // 8px for client windows and dialogs
}

// Function to clean up GTK dialog frames without sending messages
void cleanup_gtk_dialog_frame(Canvas* canvas);

#endif