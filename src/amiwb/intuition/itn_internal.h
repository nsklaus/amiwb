#ifndef ITN_INTERNAL_H
#define ITN_INTERNAL_H

// Internal header for intuition module - shared between itn_*.c files
// This header is NOT for public use - only itn_public.h is public

#include "../config.h"
#include "itn_public.h"
#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xcomposite.h>
#include <stdbool.h>
#include <time.h>

// ============================================================================
// Internal Module APIs
// All state properly encapsulated - no extern globals!
// ============================================================================

// --- itn_core.c ---
Display *itn_core_get_display(void);
int itn_core_get_screen(void);
Window itn_core_get_root(void);
int itn_core_get_screen_width(void);
int itn_core_get_screen_height(void);
int itn_core_get_screen_depth(void);
void itn_core_set_screen_dimensions(int w, int h);
bool itn_core_is_fullscreen_active(void);
void itn_core_set_fullscreen_active(bool active);
int itn_core_get_damage_event_base(void);
int itn_core_get_damage_error_base(void);
bool itn_core_init_compositor(void);
void itn_core_shutdown_compositor(void);
bool itn_core_is_compositor_active(void);
bool get_window_attrs_with_defaults(Window win, XWindowAttributes *attrs);
unsigned long unmanaged_safe_mask(XConfigureRequestEvent *event, XWindowAttributes *attrs, bool attrs_valid);
bool is_fullscreen_active(Window win);
bool itn_core_is_shutting_down(void);
bool itn_core_is_restarting(void);
void init_scroll(Canvas *canvas);
Canvas *find_window_by_path(const char *path);

// --- itn_canvas.c ---
Canvas *itn_canvas_create(Window client, XWindowAttributes *attrs);
void itn_canvas_destroy(Canvas *canvas);
Canvas *itn_canvas_find_by_window(Window win);
Canvas *itn_canvas_find_by_client(Window client);
Canvas *itn_canvas_get_desktop(void);
void itn_canvas_manage_list(Canvas *canvas, bool add);
void itn_canvas_setup_compositing(Canvas *canvas);
void itn_canvas_cleanup_compositing(Canvas *canvas);
void request_client_close(Canvas *canvas);
void iconify_canvas(Canvas *canvas);
Canvas *create_canvas(const char *path, int x, int y, int w, int h, CanvasType type);

// --- itn_manager.c ---
Canvas *itn_manager_get_canvas(int index);
int itn_manager_get_count(void);
int itn_manager_get_array_size(void);
Canvas **itn_manager_get_array(void);  // Transition helper - avoid in new code
bool itn_manager_add(Canvas *canvas);
void itn_manager_remove(Canvas *canvas);
Canvas *itn_manager_find_by_predicate(bool (*predicate)(Canvas*, void*), void *ctx);
void itn_manager_foreach(void (*callback)(Canvas*, void*), void *ctx);
void itn_manager_cleanup(void);

// --- itn_geometry.c ---
void itn_geometry_move(Canvas *canvas, int x, int y);
void itn_geometry_resize(Canvas *canvas, int width, int height);
void itn_geometry_move_resize(Canvas *canvas, int x, int y, int width, int height);
void itn_geometry_raise(Canvas *canvas);
void itn_geometry_lower(Canvas *canvas);
void itn_geometry_restack(void);
void itn_geometry_apply_resize(Canvas *c, int nw, int nh);
void compute_max_scroll(Canvas *c);

// --- itn_render.c ---
void itn_render_accumulate_damage(int x, int y, int width, int height);
void itn_render_accumulate_canvas_damage(Canvas *canvas);
void itn_render_schedule_frame(void);
void itn_render_process_frame(void);
bool itn_render_init_frame_scheduler(void);
void itn_render_cleanup_frame_scheduler(void);
void itn_render_damaged_canvases(void);
bool itn_render_needs_frame(void);
int itn_render_get_timer_fd(void);
void itn_render_consume_timer(void);
void itn_render_set_target_fps(int fps);
void itn_render_get_metrics(uint64_t *frames, uint64_t *damage, uint64_t *skipped);
void itn_render_set_timer_fd(int fd);
bool itn_render_needs_frame_scheduled(void);
struct timespec itn_render_get_next_frame_time(void);
void itn_render_log_metrics(void);

// --- itn_composite.c ---
bool itn_composite_init_overlay(void);
void itn_composite_cleanup_overlay(void);
Window itn_composite_get_overlay_window(void);
bool itn_composite_is_active(void);
void itn_composite_set_active(bool active);
bool itn_composite_create_back_buffer(void);
void itn_composite_setup_canvas(Canvas *canvas);
void itn_composite_render_all(void);
void itn_composite_render_canvas(Canvas *canvas);
Picture itn_composite_get_canvas_picture(Canvas *canvas);
void itn_composite_process_damage(XDamageNotifyEvent *ev);
void itn_composite_handle_expose(XExposeEvent *ev);
bool itn_composite_needs_frame(void);
void itn_composite_reorder_windows(void);
void itn_composite_swap_buffers(void);

// --- itn_events.c ---
void itn_events_handle_damage(XDamageNotifyEvent *event);
void itn_events_handle_configure(XConfigureEvent *event);
void itn_events_handle_map(XMapEvent *event);
bool itn_events_last_press_consumed(void);
void itn_events_reset_press_consumed(void);
bool itn_events_is_scrolling_active(void);
void itn_events_handle_unmap(XUnmapEvent *event);
void itn_events_route_to_canvas(Canvas *canvas, XEvent *event);

// --- itn_focus.c ---
void itn_focus_set_active(Canvas *canvas);
Canvas *itn_focus_get_active(void);
void itn_focus_deactivate_all(void);
void itn_focus_cycle_next(void);
void itn_focus_cycle_prev(void);
void itn_focus_activate_window_behind(Canvas *current);
void itn_focus_select_next(Canvas *closing_canvas);
void itn_focus_activate_by_index(int index);

// --- itn_decorations.c ---
void itn_decorations_draw_frame(Canvas *canvas);
void itn_decorations_update_title(Canvas *canvas, const char *title);
int itn_decorations_handle_click(Canvas *canvas, int x, int y);
void itn_decorations_get_content_area(Canvas *canvas, int *x, int *y, int *w, int *h);
void itn_decorations_calculate_frame_size(int client_w, int client_h, int *frame_w, int *frame_h);

// --- itn_resize.c ---
void itn_resize_start(Canvas *canvas, int corner);
void itn_resize_continue(int x, int y);
void itn_resize_finish(void);
void itn_resize_cancel(void);
bool itn_resize_is_active(void);
Canvas *itn_resize_get_target(void);
void itn_resize_motion(int mouse_x, int mouse_y);

// Composite module - override-redirect window management
void itn_composite_add_override(Window win, XWindowAttributes *attrs);

// ============================================================================
// Utility Macros
// ============================================================================

// Damage tracking helpers
#define DAMAGE_CANVAS(c) itn_render_accumulate_canvas_damage(c)
#define DAMAGE_RECT(x, y, w, h) itn_render_accumulate_damage(x, y, w, h)
#define DAMAGE_REGION(x, y, w, h) itn_render_accumulate_damage(x, y, w, h)
#define SCHEDULE_FRAME() itn_render_schedule_frame()

// Logging helpers (use existing log_error from config.h)
#define ITN_LOG(fmt, ...) log_error("[ITN] " fmt, ##__VA_ARGS__)
#define ITN_DEBUG(fmt, ...) /* log_error("[ITN_DEBUG] " fmt, ##__VA_ARGS__) */

#endif // ITN_INTERNAL_H