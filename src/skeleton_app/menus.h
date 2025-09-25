/* menus.h - Menu system integration with AmiWB */

/*
 * What could be added later:
 * - Dynamic menu building
 * - Menu item icons
 * - Keyboard accelerators display
 * - Context menus
 * - Menu state management
 */

#ifndef SKELETON_MENUS_H
#define SKELETON_MENUS_H

#include <X11/Xlib.h>

/* Initialize and register menus with AmiWB */
void menus_init(Display *display, Window window);

/* Handle menu selection from AmiWB */
void menus_handle_selection(int menu_id, int item_id);

#endif /* SKELETON_MENUS_H */