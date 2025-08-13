// File: intuition.h
// Core window/canvas management for AmiWB. Provides the `Canvas` type,
// render context, and the event-facing API used by the dispatcher.
#ifndef INTUITION_H
#define INTUITION_H

#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xrandr.h>
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

typedef enum CanvasType { DESKTOP, WINDOW, MENU } CanvasType;

// Global render context shared by renderer and UI subsystems.
// Holds Display, XRender format, and wallpaper pixmaps.
typedef struct {
    Display *dpy;                 // X11 display connection
    XRenderPictFormat *fmt;       // XRender visual format
    Pixmap desk_img;              // desktop background pic
    Pixmap wind_img;              // windows background pic 
    int desk_img_w, desk_img_h;
    int wind_img_w, wind_img_h;
    
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
    char *title;                        // Window/menu title (NULL for desktop)
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
    bool close_armed;                    // Close gadget pressed (until release)
    bool resizing_interactive;           // True during interactive resize (no buffer recreation)
    bool is_transient;                   // True if this is a transient window (modal dialog)
    Window transient_for;                // Parent window for transient windows
    bool close_request_sent;             // True if WM_DELETE_WINDOW was sent (for single-click close)
    int consecutive_unmaps;              // Count of unmaps without remap (for zombie detection)
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
Canvas  *get_active_window(void);           // Currently active WINDOW canvas
Canvas  *find_canvas_by_client(Window client_win); // Lookup by client window
Canvas  *find_window_by_path(const char *path);   // Find WINDOW by directory path

void destroy_canvas(Canvas *canvas);        // Destroy canvas and free X resources
void cleanup_intuition(void);               // Free visuals, desktop, Display
void set_active_window(Canvas *canvas);     // Activate one, deactivate others
// Temporarily suppress desktop deactivation on empty clicks (used after restore)
void suppress_desktop_deactivate_for_ms(int ms);

// Compute scroll limits from content and clamp current scroll.
void compute_max_scroll(Canvas *canvas);

// Iconify a workbench canvas window (hide window and create desktop icon)
void iconify_canvas(Canvas *canvas);

// Enter global shutdown mode (suppress X errors during teardown)
void begin_shutdown(void);

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
#endif