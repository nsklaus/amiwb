// File: menus.h
#ifndef MENUS_H
#define MENUS_H

#include <X11/Xlib.h>
#include <stdbool.h>
#include "intuition.h"
#include "workbench.h"

#define MENU_ITEM_HEIGHT 20     // Menubar and menu item height

typedef struct Menu {
    Canvas *canvas;             // Menubar or dropdown canvas
    char **items;               // Array of menu item labels
    char **shortcuts;           // Array of shortcut keys (e.g., "R" for Rename, NULL if none)
    bool *enabled;              // Array of enabled states (true = enabled, false = grayed out)
    bool *checkmarks;           // Array of checkmark states (true = show checkmark, false = no checkmark)
    char **commands;            // Array of commands for custom menu items (NULL for system menus)
    int item_count;             // Number of items
    int selected_item;          // Index of selected item (-1 for none)
    int parent_index;           // Index in parent menu (-1 for top level)
    struct Menu *parent_menu;   // Parent menu (NULL for menubar)
    struct Menu **submenus;     // Array of submenus (NULL if none)
    Canvas **window_refs;       // Window references for window_list menu (NULL for regular menus)
    bool is_custom;             // True if this is a custom menu from config file
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
void close_window_list_if_open(void);                           // Close window list menu if it's open
void close_all_menus(void);                                     // Close all open menus (resolution change)
void menu_handle_motion_notify(XMotionEvent *event);            // Handle motion for dropdown menus
void menu_handle_menubar_motion(XMotionEvent *event);           // Handle motion for menubar
void menu_handle_button_release(XButtonEvent *event);
void menu_handle_key_press(XKeyEvent *event);                   // Handle key press for menu navigation

// New prototypes for state and rendering support
bool get_show_menus_state(void);                // Get current show_menus state
void toggle_menubar_state(void);                // Toggle between logo and menus state
int get_menu_item_count(void);                  // Get number of top-level menu items

// Show Hidden state management
bool get_global_show_hidden(void);              // Get Show Hidden state from active window
bool get_active_view_is_icons(void);            // Get View Mode from active window (true=Icons, false=Names)
const char *get_menu_item_label(int index);     // Get label for item at index
int get_selected_item(void);                    // Get currently selected (highlighted) item

// Global action triggers (can be called from shortcuts or menus)
void trigger_rename_action(void);               // Trigger rename for selected icon
void trigger_icon_info_action(void);            // Trigger icon information for selected icon
void trigger_cleanup_action(void);              // Trigger clean up for active window or desktop
void trigger_refresh_action(void);              // Trigger refresh for active window or desktop
void trigger_close_action(void);                // Trigger close for active window
void trigger_parent_action(void);               // Trigger open parent for active window
void trigger_open_action(void);                 // Trigger open for selected icon
void trigger_copy_action(void);                 // Trigger copy for selected icon
void trigger_delete_action(void);               // Trigger delete for selected icon
void trigger_execute_action(void);              // Trigger execute command dialog
void trigger_new_drawer_action(void);           // Trigger new drawer creation
void trigger_select_contents_action(void);      // Trigger select/deselect all in window
void handle_quit_request(void);                 // Handle quit request (menu or shortcut)
void handle_suspend_request(void);              // Handle suspend request (menu or shortcut)

// Custom menu support
void load_custom_menus(void);                   // Load custom menus from toolsdaemonrc
void execute_custom_command(const char *cmd);   // Execute a custom menu command

// Date/time display
void update_menubar_time(void);                 // Check if time changed and redraw menubar if needed

// Menu substitution system
void switch_to_app_menu(const char *app_name, char **menu_items, Menu **submenus, int item_count, Window app_window);
void restore_system_menu(void);
bool is_app_menu_active(void);
Window get_app_menu_window(void);
void check_for_app_menus(Window win);  // Check X11 properties for app menus


#endif