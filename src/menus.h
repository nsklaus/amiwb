#ifndef MENUS_H
#define MENUS_H

#include <X11/Xlib.h>

// Initialize menubar and menu
void menus_init(Display *dpy, Window root, int screen, XFontStruct *font);

// Get menubar window
Window get_menubar_window(void);

// Get menu window
Window get_menu_window(void);

// Handle menu selection
int handle_menu_selection(Display *dpy, int x, int y);

// Clean up menu resources
void cleanup_menus(Display *dpy);

#endif
