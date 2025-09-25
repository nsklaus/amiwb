/* font_manager.h - Font management for skeleton app */

/*
 * AmiWB Standard Font: Source Code Pro Bold 12pt
 * This is THE ONLY font - no fallbacks, no substitutions
 *
 * What could be added later:
 * - Multiple font sizes (but same family)
 * - Font path configuration
 * - Font validation/verification
 */

#ifndef SKELETON_FONT_MANAGER_H
#define SKELETON_FONT_MANAGER_H

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

/* Initialize font system */
int font_init(Display *display);

/* Get the font - do NOT free this */
XftFont *font_get(void);

/* Cleanup fonts */
void font_cleanup(void);

#endif /* SKELETON_FONT_MANAGER_H */