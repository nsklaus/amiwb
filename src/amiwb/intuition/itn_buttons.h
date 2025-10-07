#ifndef ITN_BUTTONS_H
#define ITN_BUTTONS_H

#include "../config.h"
#include <X11/Xlib.h>
#include <stdbool.h>

// Forward declaration (implementation hidden from external callers)
typedef struct Canvas Canvas;

// Public API for window button interactions
// Handles: close, iconify, maximize, lower, resize buttons

// Handle button press on window controls
// Returns true if button was pressed (event consumed), false otherwise
bool itn_buttons_handle_press(Canvas *canvas, XButtonEvent *event);

// Handle button release on window controls
// Returns true if button was released (event consumed), false otherwise
bool itn_buttons_handle_release(Canvas *canvas, XButtonEvent *event);

// Cancel armed buttons when mouse moves away from button area
// Returns true if any button state changed, false otherwise
bool itn_buttons_handle_motion_cancel(Canvas *canvas, XMotionEvent *event);

#endif // ITN_BUTTONS_H
