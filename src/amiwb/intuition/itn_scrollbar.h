#ifndef ITN_SCROLLBAR_H
#define ITN_SCROLLBAR_H

#include "../config.h"
#include <X11/Xlib.h>
#include <stdbool.h>

// Forward declaration (implementation hidden from external callers)
typedef struct Canvas Canvas;

// Public API for scrollbar interactions
// Returns true if event was consumed by scrollbar, false otherwise
bool itn_scrollbar_handle_button_press(Canvas *canvas, XButtonEvent *event);
bool itn_scrollbar_handle_button_release(Canvas *canvas, XButtonEvent *event);
bool itn_scrollbar_handle_motion(XMotionEvent *event);
bool itn_scrollbar_handle_motion_cancel(Canvas *canvas, XMotionEvent *event);

// Arrow button auto-repeat (called from main event loop)
void itn_scrollbar_check_arrow_repeat(void);

// Query functions
bool itn_scrollbar_is_scrolling_active(void);

#endif // ITN_SCROLLBAR_H
