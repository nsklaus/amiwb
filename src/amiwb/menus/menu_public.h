// Menu System Public API
// This is the public interface exported to the rest of amiwb

#ifndef MENU_PUBLIC_H
#define MENU_PUBLIC_H

#include <X11/Xlib.h>
#include <stdbool.h>
#include "../intuition/itn_public.h"
#include "../workbench/wb_public.h"

#define MENU_ITEM_HEIGHT 20     // Menubar and menu item height

// Menu structure definition
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

// ============================================================================
// Initialization and Cleanup
// ============================================================================

void init_menus(void);          // Initialize menubar and resources
void cleanup_menus(void);       // Clean up menubar and resources
void load_custom_menus(void);   // Load custom menus from config

// ============================================================================
// State Management
// ============================================================================

bool get_show_menus_state(void);    // Get current show_menus state (logo vs menus)
void toggle_menubar_state(void);    // Toggle between logo and menus state

// ============================================================================
// Menubar Updates
// ============================================================================

void update_menubar_time(void);         // Check if time changed and redraw menubar if needed
void menu_addon_update_all(void);       // Update all menu addons (called periodically)
void update_view_modes_checkmarks(void); // Update View Modes submenu checkmarks based on current state

// ============================================================================
// Event Handlers (called from main event loop)
// ============================================================================

void menu_handle_menubar_press(XButtonEvent *event);    // Handle button press for menubar
void menu_handle_menubar_motion(XMotionEvent *event);   // Handle motion for menubar
void menu_handle_button_press(XButtonEvent *event);     // Handle button press for dropdown menus
void menu_handle_button_release(XButtonEvent *event);   // Handle button release for dropdown menus
void menu_handle_motion_notify(XMotionEvent *event);    // Handle motion for dropdown menus
void menu_handle_key_press(XKeyEvent *event);           // Handle key press for menu navigation

// ============================================================================
// Menu Closing
// ============================================================================

void close_all_menus(void);                             // Close all open menus
void close_window_list_if_open(void);                   // Close window list menu if it's open

// ============================================================================
// Menu Substitution (for native amiwb apps)
// ============================================================================

void switch_to_app_menu(const char *app_name, char **menu_items, Menu **submenus, int item_count, Window app_window);
void restore_system_menu(void);
void check_for_app_menus(Window win);                   // Check X11 properties for app menus
void handle_menu_state_change(Window win);              // Handle app menu state property changes

// ============================================================================
// Menu Queries
// ============================================================================

Canvas *get_menubar(void);                              // Get global menubar canvas
Menu *get_menubar_menu(void);                           // Get the menubar Menu struct
Menu *get_menu_by_canvas(Canvas *canvas);               // Get Menu for a canvas (menubar or active)
Menu *get_active_menu(void);                            // Get current active dropdown
bool is_app_menu_active(void);                          // Check if app menus are currently active
Window get_app_menu_window(void);                       // Get window that owns current app menus
// get_global_show_hidden() renamed to get_global_show_hidden_state() in workbench
// get_active_view_is_icons() moved to menu_core.c as static helper

// ============================================================================
// Action Triggers (called from keyboard shortcuts, menu selections, etc.)
// ============================================================================

// Workbench Menu Actions
void trigger_new_drawer_action(void);                   // Trigger new drawer creation
void trigger_parent_action(void);                       // Trigger open parent for active window
void trigger_close_action(void);                        // Trigger close for active window
void trigger_select_contents_action(void);              // Trigger select/deselect all in window
void trigger_cleanup_action(void);                      // Trigger clean up for active window or desktop
void trigger_refresh_action(void);                      // Trigger refresh for active window or desktop

// Icons Menu Actions
void trigger_open_action(void);                         // Trigger open for selected icon
void trigger_copy_action(void);                         // Trigger copy for selected icon
void trigger_rename_action(void);                       // Trigger rename for selected icon
void trigger_icon_info_action(void);                    // Trigger icon information for selected icon
void trigger_delete_action(void);                       // Trigger delete for selected icon

// Tools Menu Actions
void trigger_execute_action(void);                      // Trigger execute command dialog
void trigger_requester_action(void);                    // Trigger file requester
void trigger_extract_action(void);                      // Trigger extract archive
void trigger_eject_action(void);                        // Trigger eject disk

// Custom Menu Actions
void execute_custom_command(const char *cmd);           // Execute a custom menu command

// System Actions
void handle_quit_request(void);                         // Handle quit request (menu or shortcut)
void handle_suspend_request(void);                      // Handle suspend request (menu or shortcut)
void handle_restart_request(void);                      // Handle restart request

#endif // MENU_PUBLIC_H
