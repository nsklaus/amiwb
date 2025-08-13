/*
 *  window resize system for amiwb
 * 
 * This module handles window resizing with these optimizations:
 * - Motion event compression (max 60 FPS)
 * - Smart buffer management (minimal XRender recreations)
 */

#ifndef RESIZE_H
#define RESIZE_H

#include "intuition.h" // For Canvas type
#include <stdbool.h>

/*
 * Start a resize operation
 * Call this when user presses mouse button on resize handle
 */
void resize_begin(Canvas *canvas, int mouse_x, int mouse_y);

/*
 * Handle mouse motion during resize
 * Call this for each mouse motion event while resizing
 * Motion compression is handled automatically
 */
void resize_motion(int mouse_x, int mouse_y);

/*
 * Finish resize operation  
 * Call this when user releases mouse button
 */
void resize_end(void);

/*
 * Check if we're currently resizing
 * Useful for render optimizations
 */
bool resize_is_active(void);

/*
 * Get the canvas being resized
 * Returns NULL if not resizing
 * Useful for render optimizations to skip non-resizing windows
 */
Canvas* resize_get_canvas(void);

#endif // RESIZE_H