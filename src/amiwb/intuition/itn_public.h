// itn_public.h - Public API for the modular intuition system
// This header defines types and functions that other modules can use
// to interact with the intuition (window management) system

#ifndef ITN_PUBLIC_H
#define ITN_PUBLIC_H

#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/Xft/Xft.h>
#include <stdbool.h>
#include <time.h>
#include <Imlib2.h>

// ============================================================================
// Public Types
// ============================================================================

// View mode for workbench windows
typedef enum { VIEW_ICONS = 0, VIEW_NAMES = 1 } ViewMode;

// Canvas type
typedef enum CanvasType { DESKTOP, WINDOW, MENU, DIALOG } CanvasType;

// Global render context shared by renderer and UI subsystems
typedef struct {
    Display *dpy;
    XRenderPictFormat *fmt;
    Pixmap desk_img;
    Pixmap wind_img;
    int desk_img_w, desk_img_h;
    int wind_img_w, wind_img_h;
    Picture desk_picture;
    Picture wind_picture;
    Pixmap checker_active_pixmap;
    Picture checker_active_picture;
    Pixmap checker_inactive_pixmap;
    Picture checker_inactive_picture;
    int default_screen;
    Visual *default_visual;
    Colormap default_colormap;
} RenderContext;

// Canvas structure - represents any drawable surface (window, desktop, menu)
typedef struct Canvas {
    CanvasType type;
    Window win;
    Window client_win;
    Visual *visual;
    Pixmap canvas_buffer;
    Picture canvas_render;
    Picture window_render;
    char *path;
    char *title_base;
    char *title_change;
    int x, y;
    int width, height;
    int buffer_width, buffer_height;
    int scroll_x, scroll_y;
    int max_scroll_x, max_scroll_y;
    int content_width, content_height;
    int depth;
    XRenderColor bg_color;
    ViewMode view_mode;
    bool active;
    Colormap colormap;
    bool scanning;
    bool show_hidden;

    // Fullscreen support
    bool fullscreen;
    int saved_x, saved_y;
    int saved_w, saved_h;

    // Maximize toggle support
    bool maximized;
    int pre_max_x, pre_max_y;
    int pre_max_w, pre_max_h;

    // Button armed states
    bool close_armed;
    bool iconify_armed;
    bool maximize_armed;
    bool lower_armed;
    bool v_arrow_up_armed;
    bool v_arrow_down_armed;
    bool h_arrow_left_armed;
    bool h_arrow_right_armed;
    bool resize_armed;
    bool resizing_interactive;

    // Window properties
    bool is_transient;
    Window transient_for;
    bool close_request_sent;
    int consecutive_unmaps;
    bool ever_mapped;  // Track if transient was ever shown (prevents early destruction)
    bool cleanup_scheduled;
    bool disable_scrollbars;

    // Text rendering
    XftDraw *xft_draw;
    XftColor xft_black;
    XftColor xft_white;
    XftColor xft_blue;
    XftColor xft_gray;
    bool xft_colors_allocated;

    // Damage tracking
    bool needs_redraw;
    int dirty_x, dirty_y;
    int dirty_w, dirty_h;

    // Window size constraints
    int min_width, min_height;
    int max_width, max_height;
    bool resize_x_allowed;
    bool resize_y_allowed;

    // Compositing Integration
    Damage comp_damage;
    Pixmap comp_pixmap;
    Picture comp_picture;
    bool comp_override_redirect;

    // Enhanced damage tracking for compositor
    bool comp_needs_repaint;
    XRectangle comp_damage_bounds;
    struct timespec comp_last_damage_time;

    // Stacking order for compositor
    int comp_stack_layer;
    struct Canvas *comp_above;
    struct Canvas *comp_below;

    // Compositor render state
    bool comp_visible;
    bool comp_mapped;
    double comp_opacity;

    // Private compositor data
    void *compositor_private;

    // Linked list support
    struct Canvas *next;
} Canvas;

// Global randr event base
extern int randr_event_base;

// ============================================================================
// Public API Functions - Modular Intuition System
// ============================================================================

// --- Core initialization ---
Display *itn_core_get_display(void);
int itn_core_get_damage_event_base(void);
Canvas *init_intuition(void);
void cleanup_intuition(void);
bool init_display_and_root(void);
RenderContext *get_render_context(void);

// --- Canvas management ---
Canvas *itn_canvas_get_desktop(void);
Canvas *itn_canvas_find_by_window(Window win);
Canvas *itn_canvas_find_by_client(Window client_win);
Canvas *itn_canvas_create(Window client, XWindowAttributes *attrs);
void itn_canvas_destroy(Canvas *canvas);
Canvas *create_canvas(const char *path, int x, int y, int width, int height, CanvasType type);
Canvas *create_canvas_with_client(const char *path, int x, int y, int width, int height, CanvasType type, Window client_win);
Canvas *find_window_by_path(const char *path);
void remove_canvas_from_array(Canvas *target_canvas);

// --- Focus management ---
void itn_focus_set_active(Canvas *canvas);
Canvas *itn_focus_get_active(void);
void itn_focus_deactivate_all(void);
#define deactivate_all_windows() itn_focus_deactivate_all()
void itn_focus_cycle_next(void);
void itn_focus_cycle_prev(void);
void itn_focus_activate_by_index(int index);
void itn_focus_activate_window_behind(Canvas *current);

// --- Window operations ---
void iconify_canvas(Canvas *canvas);
void iconify_all_windows(void);
void lower_window_to_back(Canvas *canvas);
void request_client_close(Canvas *canvas);
int get_window_list(Canvas ***windows);
Canvas **get_canvas_array(void);
int get_canvas_count(void);

// --- Geometry management ---
void itn_geometry_move_resize(Canvas *canvas, int x, int y, int width, int height);
void itn_geometry_resize(Canvas *canvas, int width, int height);

// --- Scrolling ---
void compute_max_scroll(Canvas *canvas);
void update_canvas_max_constraints(void);
void update_scroll_from_mouse_drag(Canvas *canvas, bool is_vertical, int initial_scroll, int drag_start_pos, int current_mouse_pos);

// --- Fullscreen ---
void intuition_enter_fullscreen(Canvas *c);
void intuition_exit_fullscreen(Canvas *c);

// --- Event handlers ---
void intuition_handle_expose(XExposeEvent *event);
void intuition_handle_button_press(XButtonEvent *event);
void intuition_handle_button_release(XButtonEvent *event);
void intuition_handle_map_request(XMapRequestEvent *event);
void intuition_handle_map_notify(XMapEvent *event);
void intuition_handle_unmap_notify(XUnmapEvent *event);
void intuition_handle_configure_request(XConfigureRequestEvent *event);
void intuition_handle_property_notify(XPropertyEvent *event);
void intuition_handle_motion_notify(XMotionEvent *event);
void intuition_check_arrow_scroll_repeat(void);
void intuition_handle_destroy_notify(XDestroyWindowEvent *event);
void intuition_handle_configure_notify(XConfigureEvent *event);
void intuition_handle_rr_screen_change(XRRScreenChangeNotifyEvent *event);
void intuition_handle_client_message(XClientMessageEvent *event);

// --- Event state queries ---
bool intuition_last_press_consumed(void);
bool intuition_is_scrolling_active(void);
bool itn_events_last_press_consumed(void);
void itn_events_reset_press_consumed(void);
bool itn_events_is_scrolling_active(void);

// --- Render/frame scheduling ---
void itn_render_set_timer_fd(int fd);
int itn_render_get_timer_fd(void);
bool itn_render_needs_frame_scheduled(void);
struct timespec itn_render_get_next_frame_time(void);
void itn_render_consume_timer(void);
void itn_render_process_frame(void);
void itn_render_log_metrics(void);
void itn_render_record_damage_event(void);
void itn_render_accumulate_canvas_damage(Canvas *canvas);
void itn_render_schedule_frame(void);
void itn_render_update_metrics(int composite_calls, uint64_t pixels, int visible);

// --- Compositor ---
void itn_composite_process_damage(XDamageNotifyEvent *event);
bool itn_composite_remove_override(Window win);
void itn_composite_update_canvas_pixmap(Canvas *canvas);

// --- Decoration and hit testing ---
int hit_test(Canvas *canvas, int x, int y);
void handle_window_controls(Canvas *canvas, XButtonEvent *event);

// --- Client window management ---
bool should_skip_framing(Window win, XWindowAttributes *attrs);
Canvas *frame_client_window(Window client, XWindowAttributes *attrs);
void frame_existing_client_windows(void);
bool is_viewable_client(Window win);
bool is_toplevel_under_root(Window win);

// --- Desktop operations ---
void handle_desktop_button(XButtonEvent *event);
void suppress_desktop_deactivate_for_ms(int ms);

// --- Menu operations ---
void toggle_menubar_and_redraw(void);
void menubar_apply_fullscreen(bool fullscreen);

// --- Error handling and debugging ---
void install_error_handler(void);
int x_error_handler(Display *dpy, XErrorEvent *error);
bool is_window_valid(Display *dpy, Window win);
Bool safe_get_window_attributes(Display *dpy, Window win, XWindowAttributes *attrs);
void safe_unmap_window(Display *dpy, Window win);
Bool safe_translate_coordinates(Display *dpy, Window src_w, Window dest_w,
                                int src_x, int src_y, int *dest_x, int *dest_y,
                                Window *child);
void safe_set_input_focus(Display *dpy, Window win, int revert_to, Time time);

// --- Client communication ---
void send_x_command_and_sync(void);
bool send_close_request_to_client(Window client);
int debug_get_window_property(Display *dpy, Window win, Atom property,
                               long offset, long length, Bool delete,
                               Atom req_type, Atom *actual_type,
                               int *actual_format, unsigned long *nitems,
                               unsigned long *bytes_after, unsigned char **prop,
                               const char *caller_location);
void enable_property_debug(void);
void disable_property_debug(void);

// Debug macro to trace property access - use this instead of XGetWindowProperty when debugging
#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define DEBUG_GET_PROPERTY(dpy, win, prop, off, len, del, req, act_type, act_fmt, nitems, bytes, data) \
    debug_get_window_property(dpy, win, prop, off, len, del, req, act_type, act_fmt, nitems, bytes, data, \
                               __FILE__ ":" TOSTRING(__LINE__))

// --- Resize operations ---
void resize_end(Canvas *canvas);
void calculate_frame_size_from_client_size(int client_width, int client_height, int *frame_width, int *frame_height);
void apply_resize_and_redraw(Canvas *canvas, int width, int height);
Canvas *itn_resize_get_target(void);
bool itn_resize_is_active(void);

// --- Shutdown/restart ---
void begin_shutdown(void);
void begin_restart(void);
bool is_restarting(void);
void itn_core_shutdown_compositor(void);

// --- GTK dialog cleanup ---
void cleanup_gtk_dialog_frame(Canvas *canvas);

#endif // ITN_PUBLIC_H