// Menu System - App Menu Substitution Module
// Handles switching between system menus and app-specific menus

#include "menu_internal.h"
#include "menu_public.h"
#include "../intuition/itn_public.h"
#include "../config.h"
#include <string.h>
#include <stdlib.h>

// ============================================================================
// App Menu Cache
// ============================================================================

// App menu caching system - cache menus by app type for multi-window support
typedef struct AppMenuCache {
    char app_type[32];              // App type identifier (e.g., "EDITPAD")
    char **menu_items;              // Cached menu bar items
    Menu **submenus;                // Cached submenu structures
    int menu_item_count;            // Number of menu items
    struct AppMenuCache *next;      // Linked list next pointer
} AppMenuCache;

static AppMenuCache *cached_apps = NULL;   // Head of cached app menus list

// Find cached app menus by app type
static AppMenuCache* find_cached_app(const char *app_type) {
    AppMenuCache *current = cached_apps;
    while (current) {
        if (strcmp(current->app_type, app_type) == 0) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

// Cache app menus for reuse by multiple instances
void cache_app_menus(const char *app_type, char **menu_items, Menu **submenus, int menu_count) {
    // Check if already cached
    AppMenuCache *existing = find_cached_app(app_type);
    if (existing) {
        // Update existing cache with new menu data
        // Don't free the old data - it might still be in use by menubar!
        // Just update the pointers to the new data
        existing->menu_items = menu_items;
        existing->submenus = submenus;
        existing->menu_item_count = menu_count;
        return;
    }

    // Create new cache entry
    AppMenuCache *cache = calloc(1, sizeof(AppMenuCache));
    if (!cache) {
        log_error("[ERROR] Failed to allocate memory for app menu cache");
        exit(1);  // Fatal - can't continue without memory
    }
    snprintf(cache->app_type, sizeof(cache->app_type), "%s", app_type);
    cache->menu_items = menu_items;
    cache->submenus = submenus;
    cache->menu_item_count = menu_count;

    // Add to head of list
    cache->next = cached_apps;
    cached_apps = cache;
}

// ============================================================================
// Menu Substitution
// ============================================================================

// Menu substitution: switch menubar to app-specific menus
void switch_to_app_menu(const char *app_name, char **menu_items, Menu **submenus, int item_count, Window app_window) {
    Menu *menubar = get_menubar_menu();
    Canvas *menubar_canvas = get_menubar();

    if (!menubar || !app_name || !menu_items || !submenus || item_count <= 0) {
        log_error("[WARNING] switch_to_app_menu called with invalid parameters");
        return;
    }

    // Don't switch menus during shutdown/restart - menubar might be destroyed
    if (!menubar_canvas) {
        return;  // Menubar canvas already destroyed, we're shutting down
    }

    // Save system menus on first app menu activation
    if (!system_menu_items && !app_menu_active) {
        system_logo_item = strdup(logo_items[0]);
        system_menu_items = full_menu_items;
        system_submenus = full_submenus;
        system_menu_item_count = full_menu_item_count;
        // Saved system menus
    }

    // Switch logo to app name
    free(logo_items[0]);
    logo_items[0] = strdup(app_name);

    // Switch full menu arrays to app menus
    full_menu_items = menu_items;
    full_submenus = submenus;
    full_menu_item_count = item_count;

    // CRITICAL: Always update menubar data to match current mode
    // This ensures toggle_menubar_state() has valid data
    if (show_menus) {
        // Currently showing menus - update to app menus immediately
        menubar->items = full_menu_items;
        menubar->submenus = full_submenus;
        menubar->item_count = full_menu_item_count;
    } else {
        // Currently showing logo - keep logo visible but ensure full_menu_items is ready
        // Logo items stay visible; full_menu_items are ready for next toggle
    }

    // Mark app menu as active
    app_menu_active = true;
    current_app_window = app_window;

    // Redraw menubar with new content
    redraw_canvas(menubar_canvas);
}

// Menu substitution: restore system menus
void restore_system_menu(void) {
    if (!app_menu_active || !system_menu_items) {
        return;  // Already showing system menus or not initialized
    }

    Menu *menubar = get_menubar_menu();
    Canvas *menubar_canvas = get_menubar();

    // Don't restore menus during shutdown/restart - menubar might be destroyed
    if (!menubar || !menubar_canvas) {
        return;  // Menubar or its canvas already destroyed, we're shutting down
    }

    // Restore logo
    free(logo_items[0]);
    logo_items[0] = strdup(system_logo_item);

    // Restore full menu arrays
    full_menu_items = system_menu_items;
    full_submenus = system_submenus;
    full_menu_item_count = system_menu_item_count;

    // CRITICAL: Always update menubar data to match current mode
    // This ensures toggle_menubar_state() has valid data
    if (show_menus) {
        // Currently showing menus - update to system menus immediately
        menubar->items = full_menu_items;
        menubar->submenus = full_submenus;
        menubar->item_count = full_menu_item_count;
    } else {
        // Currently showing logo - keep logo visible but ensure full_menu_items is ready
        // Logo items stay visible; full_menu_items are ready for next toggle
    }

    // Mark system menu as active
    app_menu_active = false;
    current_app_window = None;

    // Redraw menubar
    redraw_canvas(menubar_canvas);
}

// ============================================================================
// App Menu Detection
// ============================================================================

// Update menu states when app changes them
void handle_menu_state_change(Window win) {
    if (!win || !current_app_window || win != current_app_window) return;

    // Update the menu states from the property
    update_app_menu_states(win);
}

// Check if a window has toolkit app menus via X11 properties
void check_for_app_menus(Window win) {
    if (win == None) {
        restore_system_menu();
        return;
    }

    // Don't check for menus if menubar not initialized yet (during startup)
    Menu *menubar = get_menubar_menu();
    if (!menubar) return;

    Display *dpy = itn_core_get_display();
    if (!dpy) return;

    // Validate window still exists before querying properties (race: window may have closed)
    if (!is_window_valid(dpy, win)) {
        restore_system_menu();
        return;
    }

    // Define atoms for app properties
    Atom type_atom = XInternAtom(dpy, "_AMIWB_APP_TYPE", False);
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *app_type = NULL;

    // Check if window has _AMIWB_APP_TYPE property
    if (DEBUG_GET_PROPERTY(dpy, win, type_atom, 0, 1024, False,
                          AnyPropertyType, &actual_type, &actual_format,
                          &nitems, &bytes_after, &app_type) == Success && app_type) {

        // Found toolkit app

        // Always read menu data to get current checkmark states
        Atom menu_atom = XInternAtom(dpy, "_AMIWB_MENU_DATA", False);
        unsigned char *menu_data = NULL;

        if (DEBUG_GET_PROPERTY(dpy, win, menu_atom, 0, 65536, False,
                              AnyPropertyType, &actual_type, &actual_format,
                              &nitems, &bytes_after, &menu_data) == Success && menu_data) {

            // Parse menu data - this will update checkmark states
            parse_and_switch_app_menus((char*)app_type, (char*)menu_data, win);

            XFree(menu_data);

            // Also update menu states if available
            update_app_menu_states(win);
        } else {
            // No menu data available - check if we have cached menus
            AppMenuCache *cached = find_cached_app((char*)app_type);
            if (cached) {
                // Use cached menus as fallback
                switch_to_app_menu((char*)app_type, cached->menu_items,
                                 cached->submenus, cached->menu_item_count, win);

                // Update menu states if available
                update_app_menu_states(win);
            }
        }
        XFree(app_type);
    } else {
        // Not a toolkit app - restore system menus
        restore_system_menu();
    }
}
