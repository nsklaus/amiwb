// File: evt_internal.h
// Internal API for event system modules
// This header is included ONLY by evt_*.c files within events/

#ifndef EVT_INTERNAL_H
#define EVT_INTERNAL_H

#include "evt_public.h"
#include "../config.h"
#include "../intuition/itn_public.h"
#include "../workbench/wb_public.h"
#include "../menus/menu_public.h"
#include <X11/Xlib.h>
#include <stdbool.h>

// ============================================================================
// evt_mouse.c - Mouse Event State (Internal)
// ============================================================================

// Track the window that owns the current button interaction so motion and
// release are routed consistently, even if X delivers them elsewhere.

Window evt_mouse_get_press_target(void);                    // Get current press target
void evt_mouse_set_press_target(Window w);                  // Set press target
void evt_mouse_clear_press_target_if_matches(Window win);   // Clear if matches

// ============================================================================
// evt_window.c - Window Event Helpers (Internal)
// ============================================================================

// Resolve which canvas an event belongs to, translating coordinates
Canvas *resolve_event_canvas(Window w, int in_x, int in_y, int *out_x, int *out_y);

// ============================================================================
// Helper Functions - Coordinate Translation (Internal)
// ============================================================================

// Create translated event copies for canvas-local coordinates
XButtonEvent create_translated_button_event(XButtonEvent *original, Window target_window, int new_x, int new_y);
XMotionEvent create_translated_motion_event(XMotionEvent *original, Window target_window, int new_x, int new_y);

// ============================================================================
// Menu Canvas Helpers (Internal)
// ============================================================================

// Handle menubar vs regular menu routing
void handle_menu_canvas_press(Canvas *canvas, XButtonEvent *event, int cx, int cy);
void handle_menu_canvas_motion(Canvas *canvas, XMotionEvent *event, int cx, int cy);

// ============================================================================
// Debug Helpers (Internal - Currently Disabled)
// ============================================================================

#if 0
void debug_event_info(const char *event_type, Window window, int x, int y, unsigned int state);
void debug_canvas_resolved(Canvas *canvas, int tx, int ty, const char *context);
#endif

#endif // EVT_INTERNAL_H
