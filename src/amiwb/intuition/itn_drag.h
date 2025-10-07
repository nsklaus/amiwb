#ifndef ITN_DRAG_H
#define ITN_DRAG_H

#include "../config.h"
#include <X11/Xlib.h>
#include <stdbool.h>

// Forward declaration (implementation hidden from external callers)
typedef struct Canvas Canvas;

// Public API for window dragging operations
// Starts window drag from titlebar
bool itn_drag_start(Canvas *canvas, int x_root, int y_root);

// Handles drag motion (updates window position)
// Returns true if drag is active, false otherwise
bool itn_drag_motion(XMotionEvent *event);

// Ends window drag operation
void itn_drag_end(void);

// Query functions
bool itn_drag_is_active(void);
Canvas *itn_drag_get_canvas(void);

#endif // ITN_DRAG_H
