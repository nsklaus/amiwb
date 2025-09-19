/* font_manager.h - Unified font management for EditPad */

#ifndef EDITPAD_FONT_MANAGER_H
#define EDITPAD_FONT_MANAGER_H

#include <stdbool.h>
#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

// Initialize the font system - call once at startup
// Returns false if font cannot be loaded (fatal error)
bool editpad_font_init(Display *dpy);

// Get the single font instance - NEVER free this!
// Returns NULL if not initialized (should not happen in normal operation)
XftFont* editpad_font_get(void);

// Clean up font resources - call at shutdown
void editpad_font_cleanup(void);

// Helper functions for text metrics
int editpad_font_text_width(const char *text);
int editpad_font_get_ascent(void);
int editpad_font_get_descent(void);
int editpad_font_get_height(void);

#endif /* EDITPAD_FONT_MANAGER_H */