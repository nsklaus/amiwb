// File: intuition.h
#ifndef INTUITION_H
#define INTUITION_H

#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xrandr.h>
#include <stdbool.h>
#include <Imlib2.h>

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

typedef struct {
    Display *dpy;                 // X11 display connection
    XRenderPictFormat *fmt;       // XRender visual format
    Pixmap desk_img;              // desktop background pic
    Pixmap wind_img;              // windows background pic 
    int desk_img_w, desk_img_h;
    int wind_img_w, wind_img_h;
    
} RenderContext;

// ================
// intuition struct
// ================
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
} Canvas;

extern int randr_event_base;

// Function prototypes
RenderContext *get_render_context(void);    // getter for render_context

Display *get_display(void);                 // getter for dpy
Canvas  *init_intuition(void);              // Initialize X11 display, canvas array
Canvas  *get_desktop_canvas(void);          // Get desktop canvas
Canvas  *find_canvas(Window win);           // Find canvas by window
Canvas  *create_canvas(const char *path, int x, int y, int width, int height, CanvasType type); 
Canvas  *get_active_window(void);
Canvas  *find_canvas_by_client(Window client_win); // Find canvas by client window
Canvas  *find_window_by_path(const char *path);   // Find an existing workbench window by path

void destroy_canvas(Canvas *canvas); 
void cleanup_intuition(void);               // Clean up intuition resources
void set_active_window(Canvas *canvas);     // activate one, deactivate the others
// Temporarily suppress desktop deactivation on empty clicks (used after restore)
void suppress_desktop_deactivate_for_ms(int ms);

// Add prototype for compute_max_scroll
void compute_max_scroll(Canvas *canvas);    // Compute max scroll limits and clamp current scroll

// Iconify a workbench canvas window (hide window and create desktop icon)
void iconify_canvas(Canvas *canvas);

// Enter global shutdown mode (suppress X errors during teardown)
void begin_shutdown(void);

// Query helpers for event routing
bool intuition_last_press_consumed(void);   // True if last frame press consumed (scrollbar/title/resize)
bool intuition_is_scrolling_active(void);   // True while dragging a scrollbar knob



// ========
// events
// ========
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

// Use indices for pointers
/*extern int active_window_index;     // -1 for none
extern int dragging_canvas_index;   // -1 for none
extern int resizing_canvas_index;   // -1 for none*/
#endif