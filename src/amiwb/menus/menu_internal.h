// Menu System Internal API
// Shared between menu modules only - NOT for external use

#ifndef MENU_INTERNAL_H
#define MENU_INTERNAL_H

#include "menu_public.h"
#include "../render/rnd_public.h"

// ============================================================================
// Menu State Access (AWP Encapsulation - defined in menu_core.c)
// ============================================================================

// Core menu state accessors
Menu *get_menubar_menu(void);                   // Get menubar menu structure
Menu *get_active_menu(void);                    // Get current dropdown menu
void menu_core_set_active_menu(Menu *menu);     // Set active dropdown menu
Menu *menu_core_get_nested_menu(void);          // Get nested submenu
void menu_core_set_nested_menu(Menu *menu);     // Set nested submenu
bool get_show_menus_state(void);                // Get menu visibility state
void menu_core_toggle_show_menus(void);         // Toggle between logo and menu mode

// Logo mode arrays (read-only)
char **menu_core_get_logo_items(void);          // Get logo mode items array
int menu_core_get_logo_item_count(void);        // Get logo item count

// Full menu arrays (read-only)
char **menu_core_get_full_menu_items(void);     // Get full menu items array
Menu **menu_core_get_full_submenus(void);       // Get full submenus array
int menu_core_get_full_menu_item_count(void);   // Get full menu item count

// System menu backup (read-only - managed internally)
char *menu_core_get_system_logo_item(void);     // Get system logo item
char **menu_core_get_system_menu_items(void);   // Get system menu items backup
Menu **menu_core_get_system_submenus(void);     // Get system submenus backup
int menu_core_get_system_menu_item_count(void); // Get system menu count backup

// App menu state
bool is_app_menu_active(void);                  // Check if app menus are active
void menu_core_set_app_menu_active(bool active);// Set app menu active state
Window get_app_menu_window(void);               // Get app menu window
void menu_core_set_app_menu_window(Window win); // Set app menu window

// Menu substitution helpers (app menu switching)
void menu_core_save_system_menus(void);                                          // Save system menus for restore
void menu_core_switch_to_app_menus(char **items, Menu **subs, int count);       // Switch to app menu arrays
void menu_core_restore_system_menus(void);                                       // Restore system menu arrays

// ============================================================================
// Internal Helper Functions
// ============================================================================

// Menu lifecycle management
Menu* create_menu(const char* title, int item_count); // Create and allocate menu
void destroy_menu(Menu *menu);                      // Free menu and all its resources recursively
void init_menu_shortcuts(Menu *menu);               // Initialize shortcuts array
void init_menu_enabled(Menu *menu);                 // Initialize enabled states
void init_menu_checkmarks(Menu *menu);              // Initialize checkmarks array
void parse_menu_item_shortcuts(Menu *menu);         // Parse shortcuts from item labels

// Menu metrics
int get_submenu_width(Menu *menu);                  // Calculate submenu width based on items

// Menu state updates
void update_view_modes_checkmarks(void);            // Update View Modes submenu checkmarks

// ============================================================================
// Dropdown Management (called from events)
// ============================================================================

void show_dropdown_menu(Menu *menu, int index, int x, int y);  // Create and display dropdown
void close_nested_if_any(void);                                 // Close nested submenu if open
void maybe_open_nested_for_selection(void);                     // Open nested menu on hover

// ============================================================================
// Window List
// ============================================================================

void show_window_list_menu(int x, int y);                       // Build and display window list

// ============================================================================
// Menu Selection Handler
// ============================================================================

void handle_menu_selection(Menu *menu, int item_index);         // Route selection to action

// ============================================================================
// Menu Parsing (for app menus)
// ============================================================================

void parse_and_switch_app_menus(const char *app_name, const char *menu_data, Window app_window);
void update_app_menu_states(Window app_window);                 // Update checkmarks/enabled from property
void send_menu_selection_to_app(Window app_window, int menu_index, int item_index);
void cache_app_menus(const char *app_type, char **menu_items, Menu **submenus, int menu_count);

// ============================================================================
// Rendering Helpers
// ============================================================================

int menu_render_text(RenderContext *ctx, Canvas *menubar, const char *text, int x, int y);
int menu_measure_text(RenderContext *ctx, const char *text);

// ============================================================================
// Addon System
// ============================================================================

// Addon positioning zones
typedef enum {
    ADDON_POS_LEFT = 0,     // Left side of menubar (after logo)
    ADDON_POS_MIDDLE = 1,   // Center of menubar
    ADDON_POS_RIGHT = 2     // Right side of menubar (before menu button)
} AddonPosition;

// Addon callback types
typedef void (*MenuAddonRenderFunc)(RenderContext *ctx, Canvas *menubar, int *x, int y);
typedef void (*MenuAddonUpdateFunc)(void);
typedef void (*MenuAddonCleanupFunc)(void);

// Addon registration struct
typedef struct MenuAddon {
    char name[32];                          // "clock", "cpu", "ram", etc.
    AddonPosition position;                 // Where to display (left/middle/right)
    int width;                              // Display width in pixels
    MenuAddonRenderFunc render;             // Called during menubar render in logo mode
    MenuAddonUpdateFunc update;             // Called periodically (1s timer)
    MenuAddonCleanupFunc cleanup;           // Called during shutdown
    bool enabled;                           // Controlled by config
    int config_order;                       // Order in config file (-1 = not configured)
    struct MenuAddon *next;                 // Linked list
} MenuAddon;

// Addon management
void menu_addon_register(MenuAddon *addon);                                         // Register an addon
void menu_addon_unregister(const char *name);                                       // Remove addon
void menu_addon_render_all(RenderContext *ctx, Canvas *menubar, int *x, int y);    // Render all enabled addons
void menu_addon_update_all(void);                                                   // Update all addons
void menu_addon_cleanup_all(void);                                                  // Cleanup all addons
void menu_addon_load_config(void);                                                  // Load enabled addons from config

// Addon implementations (called from menu_core.c init_menus)
void menuaddon_clock_init(void);                                                    // Initialize clock addon
void menuaddon_cpu_init(void);                                                      // Initialize CPU addon
void menuaddon_memory_init(void);                                                   // Initialize memory addon
void menuaddon_fans_init(void);                                                     // Initialize fans addon
void menuaddon_temps_init(void);                                                    // Initialize temps addon

#endif // MENU_INTERNAL_H
