// File: menus.h
#ifndef MENUS_H
#define MENUS_H

#include <X11/Xlib.h>
#include "intuition.h"
#include "workbench.h"

#define MENU_ITEM_HEIGHT 20     // Menubar and menu item height

typedef struct Menu {
    Canvas *canvas;             // Menubar or dropdown canvas
    char **items;               // Array of menu item labels
    int item_count;             // Number of items
    int selected_item;          // Index of selected item (-1 for none)
    int parent_index;           // Index in parent menu (-1 for top level)
    struct Menu *parent_menu;   // Parent menu (NULL for menubar)
    struct Menu **submenus;     // Array of submenus (NULL if none)
} Menu;

// Function prototypes
void init_menus(void);          // Initialize menubar and resources
void cleanup_menus(void);       // Clean up menubar and resources
Canvas *get_menubar(void);      // Get global menubar canvas

Menu *get_menubar_menu(void);   // Get the menubar Menu struct
Menu *get_menu_by_canvas(Canvas *canvas); // Get Menu for a canvas (menubar or active)

void show_dropdown_menu(Menu *menu, int index, int x, int y);   // Show dropdown menu
void handle_menu_selection(Menu *menu, int item_index);         // Process menu item or submenu selection
Menu *get_active_menu(void);                                    // Get current active dropdown
int get_submenu_width(Menu *menu);                              // Calculate submenu width based on longest label
void set_app_menu(Menu *app_menu);                              // Set menubar to app’s menu
void menu_handle_button_press(XButtonEvent *event);             // Handle button press for dropdown menus
void menu_handle_menubar_press(XButtonEvent *event);            // Handle button press for menubar
void menu_handle_motion_notify(XMotionEvent *event);            // Handle motion for dropdown menus
void menu_handle_menubar_motion(XMotionEvent *event);           // Handle motion for menubar
void menu_handle_key_press(XKeyEvent *event);                   // Handle key press for menu navigation

// New prototypes for state and rendering support
bool get_show_menus_state(void);                // Get current show_menus state
void toggle_menubar_state(void);                // Toggle between logo and menus state
int get_menu_item_count(void);                  // Get number of top-level menu items
const char *get_menu_item_label(int index);     // Get label for item at index
int get_selected_item(void);                    // Get currently selected (highlighted) item

#endif