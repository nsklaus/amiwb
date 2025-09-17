/* font_manager.h - Unified font management for AmiWB
 *
 * Single font instance, loaded once, used everywhere.
 * No fallbacks, no multiple instances, no complexity.
 */

#ifndef FONT_MANAGER_H
#define FONT_MANAGER_H

#include <X11/Xft/Xft.h>
#include <stdbool.h>

// Initialize the font system - call this ONCE at startup
// Returns true on success, false on failure
bool font_manager_init(Display *dpy);

// Get the single global font instance
// Returns NULL if not initialized
XftFont* font_manager_get(void);

// Clean up font resources - call this ONCE at shutdown
// Pass true if we're restarting (will skip cleanup to avoid crash)
void font_manager_cleanup(bool is_restarting);

// Get text width using the global font
int font_manager_text_width(const char *text);

// Get font metrics
int font_manager_get_ascent(void);
int font_manager_get_descent(void);
int font_manager_get_height(void);

#endif /* FONT_MANAGER_H */