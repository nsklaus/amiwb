// Menu System - App Menu Parser Module
// Parses X11 properties for app menu data and state updates

#include "menu_internal.h"
#include "menu_public.h"
#include "../intuition/itn_public.h"
#include "../config.h"
#include <string.h>
#include <stdlib.h>

// Forward declaration for cache function (defined in menu_substitution.c)
extern void cache_app_menus(const char *app_type, char **menu_items, Menu **submenus, int menu_count);

// ============================================================================
// Safe Memory Allocation Helpers
// ============================================================================

// Safe strdup wrapper - returns NULL on failure for graceful degradation
static char* safe_strdup(const char *str) {
    if (!str) return NULL;  // Allow NULL input for optional strings
    char *copy = strdup(str);
    if (!copy) {
        log_error("[ERROR] strdup failed - app menu text may be missing");
        return NULL;  // Graceful degradation
    }
    return copy;
}

// ============================================================================
// App Menu Parsing
// ============================================================================

// Parse and switch to app menus from X11 property data
void parse_and_switch_app_menus(const char *app_name, const char *menu_data, Window app_window) {
    if (!menu_data || !app_name) {
        log_error("[ERROR] parse_and_switch_app_menus: NULL parameters");
        return;
    }

    Menu *menubar = get_menubar_menu();

    // Parsing menu data with submenu support

    // First pass: count top-level menus and identify submenus
    // Top-level menus are sections that don't contain "/"
    int menu_count = 0;
    int submenu_count = 0;

    // Split by | and count
    char *temp_copy = safe_strdup(menu_data);
    char *temp_saveptr;
    char *temp_str = strtok_r(temp_copy, "|", &temp_saveptr);

    while (temp_str) {
        // Check if this is a submenu definition (contains "/")
        if (strchr(temp_str, '/')) {
            submenu_count++;
        } else {
            menu_count++;
        }
        temp_str = strtok_r(NULL, "|", &temp_saveptr);
    }
    free(temp_copy);

    // Allocate top-level arrays
    char **menu_items = calloc(menu_count, sizeof(char*));
    if (!menu_items) {
        log_error("[ERROR] calloc failed for menu_items - keeping system menus");
        return;  // Graceful degradation: app menus won't load, system menus remain
    }
    Menu **submenus = calloc(menu_count, sizeof(Menu*));
    if (!submenus) {
        log_error("[ERROR] calloc failed for submenus - keeping system menus");
        free(menu_items);
        return;  // Graceful degradation
    }

    // Store submenu definitions temporarily
    typedef struct {
        char *parent_menu;
        char *parent_item;
        Menu *submenu;
    } SubmenuDef;
    SubmenuDef *submenu_defs = calloc(submenu_count, sizeof(SubmenuDef));
    if (!submenu_defs) {
        log_error("[ERROR] calloc failed for submenu_defs - keeping system menus");
        free(menu_items);
        free(submenus);
        return;  // Graceful degradation
    }
    int submenu_def_count = 0;

    // Parse the menu data - use strtok_r to avoid issues
    char *data_copy = safe_strdup(menu_data);
    char *saveptr;
    char *menu_str = strtok_r(data_copy, "|", &saveptr);
    int menu_index = 0;

    while (menu_str) {
        // Check if this is a submenu definition
        char *slash = strchr(menu_str, '/');
        if (slash) {
            // Parse "View/Syntax:None,C/C++,Python" format
            *slash = '\0';
            char *parent_menu = menu_str;
            char *rest = slash + 1;

            char *colon = strchr(rest, ':');
            if (colon) {
                *colon = '\0';
                char *parent_item = rest;
                char *items_str = colon + 1;

                // Create the submenu
                int item_count = 1;
                for (const char *p = items_str; *p; p++) {
                    if (*p == ',') item_count++;
                }

                // Create submenu (returns NULL on failure)
                Menu *submenu = create_menu(NULL, item_count);
                if (!submenu) {
                    log_error("[ERROR] Failed to create nested submenu - skipping");
                    menu_str = strtok_r(NULL, "|", &saveptr);
                    continue;  // Skip this submenu, continue parsing
                }
                init_menu_enabled(submenu);
                init_menu_checkmarks(submenu);

                // Parse submenu items
                char *items_copy = safe_strdup(items_str);
                char *saveptr2;
                char *item = strtok_r(items_copy, ",", &saveptr2);
                int item_index = 0;

                while (item && item_index < item_count) {
                    // Skip leading spaces
                    while (*item == ' ') item++;
                    submenu->items[item_index] = safe_strdup(item);
                    submenu->shortcuts[item_index] = NULL;
                    submenu->enabled[item_index] = true;
                    item = strtok_r(NULL, ",", &saveptr2);
                    item_index++;
                }
                free(items_copy);

                submenu->selected_item = -1;
                submenu->submenus = NULL;
                submenu->canvas = NULL;

                // Store submenu definition
                submenu_defs[submenu_def_count].parent_menu = safe_strdup(parent_menu);
                submenu_defs[submenu_def_count].parent_item = safe_strdup(parent_item);
                submenu_defs[submenu_def_count].submenu = submenu;
                submenu_def_count++;
            }
        } else {
            // Regular top-level menu
            char *colon = strchr(menu_str, ':');
            if (!colon) {
                menu_str = strtok_r(NULL, "|", &saveptr);
                continue;
            }

            *colon = '\0';
            char *menu_name = menu_str;
            char *items_str = colon + 1;

            // Store menu name
            menu_items[menu_index] = safe_strdup(menu_name);

            // Count items in this menu
            int item_count = 1;
            for (const char *p = items_str; *p; p++) {
                if (*p == ',') item_count++;
            }

            // Create submenu (returns NULL on failure)
            Menu *submenu = create_menu(NULL, item_count);
            if (!submenu) {
                log_error("[ERROR] Failed to create app menu - skipping");
                menu_str = strtok_r(NULL, "|", &saveptr);
                continue;  // Skip this menu, continue parsing
            }
            init_menu_enabled(submenu);
            init_menu_checkmarks(submenu);

            // Parse items - use strtok_r for thread safety and to avoid nested strtok issues
            char *saveptr2;
            char *items_copy = safe_strdup(items_str);  // Make a copy for strtok_r
            char *item = strtok_r(items_copy, ",", &saveptr2);
            int item_index = 0;
            while (item && item_index < item_count) {
                // Skip leading spaces
                while (*item == ' ') item++;

                // Check for checkbox notation [o] = checked, [x] = unchecked
                if (strncmp(item, "[o]", 3) == 0) {
                    submenu->checkmarks[item_index] = true;  // Checked (on)
                    item += 3;  // Skip "[o]"
                    if (*item == ' ') item++;  // Skip space after checkbox
                } else if (strncmp(item, "[x]", 3) == 0) {
                    submenu->checkmarks[item_index] = false;  // Unchecked (off)
                    item += 3;  // Skip "[x]"
                    if (*item == ' ') item++;  // Skip space after checkbox
                }
                // Items without [o] or [x] are not toggleable

                // Check for submenu marker ">"
                char *submenu_marker = strstr(item, " >");
                if (submenu_marker) {
                    // This item has a submenu
                    *submenu_marker = '\0';  // Remove the " >" from the item name
                    // The actual submenu will be linked later
                }

                // Parse menu item with potential "#" shortcut notation
                char *delimiter = strchr(item, '#');
                if (delimiter) {
                    // Found "#" - split into name and shortcut
                    *delimiter = '\0';  // Terminate item name at #
                    char *shortcut = delimiter + 1;  // Shortcut is everything after #

                    // Strip any trailing spaces from item name
                    char *end = item + strlen(item) - 1;
                    while (end > item && *end == ' ') {
                        *end = '\0';
                        end--;
                    }

                    // Strip any leading spaces from shortcut
                    while (*shortcut == ' ') {
                        shortcut++;
                    }

                    submenu->items[item_index] = safe_strdup(item);
                    submenu->shortcuts[item_index] = safe_strdup(shortcut);
                } else {
                    // No "#" delimiter - no shortcut
                    // Strip trailing spaces
                    char *end = item + strlen(item) - 1;
                    while (end > item && *end == ' ') {
                        *end = '\0';
                        end--;
                    }
                    submenu->items[item_index] = safe_strdup(item);
                    submenu->shortcuts[item_index] = NULL;
                }

                submenu->enabled[item_index] = true;

                item = strtok_r(NULL, ",", &saveptr2);
                item_index++;
            }
            free(items_copy);

            submenu->selected_item = -1;
            submenu->parent_menu = menubar;
            submenu->parent_index = menu_index;
            submenu->canvas = NULL;

            submenus[menu_index] = submenu;
            menu_index++;
        }

        menu_str = strtok_r(NULL, "|", &saveptr);
    }

    free(data_copy);

    // Now link the submenus to their parent items
    for (int i = 0; i < submenu_def_count; i++) {
        SubmenuDef *def = &submenu_defs[i];

        // Find the parent menu
        int parent_menu_idx = -1;
        for (int j = 0; j < menu_count; j++) {
            if (menu_items[j] && strcmp(menu_items[j], def->parent_menu) == 0) {
                parent_menu_idx = j;
                break;
            }
        }

        if (parent_menu_idx >= 0 && submenus[parent_menu_idx]) {
            Menu *parent_menu = submenus[parent_menu_idx];

            // Find the parent item
            for (int j = 0; j < parent_menu->item_count; j++) {
                if (parent_menu->items[j] && strcmp(parent_menu->items[j], def->parent_item) == 0) {
                    // Link the submenu
                    parent_menu->submenus[j] = def->submenu;
                    def->submenu->parent_menu = parent_menu;
                    def->submenu->parent_index = j;
                    break;
                }
            }
        }

        // Clean up submenu def strings
        free(def->parent_menu);
        free(def->parent_item);
    }
    free(submenu_defs);

    // Cache the menus for reuse by other instances
    cache_app_menus(app_name, menu_items, submenus, menu_count);

    // Switch to the parsed menus
    switch_to_app_menu(app_name, menu_items, submenus, menu_count, app_window);
}

// ============================================================================
// Menu State Updates
// ============================================================================

// Update menu item states from app window property
void update_app_menu_states(Window app_window) {
    if (!app_window || !app_menu_active || !full_submenus) return;

    Display *dpy = itn_core_get_display();
    if (!dpy) return;

    // Validate window still exists before querying properties (race: window may have closed)
    if (!is_window_valid(dpy, app_window)) {
        return;
    }

    // Get the menu states property
    Atom states_atom = XInternAtom(dpy, "_AMIWB_MENU_STATES", False);
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *states_data = NULL;

    if (DEBUG_GET_PROPERTY(dpy, app_window, states_atom, 0, 65536, False,
                          AnyPropertyType, &actual_type, &actual_format,
                          &nitems, &bytes_after, &states_data) == Success && states_data) {

        // Parse the states data
        // Format: "menu_index,item_index,state;menu_index,item_index,state;..."
        // Example: "1,4,0;1,5,1" means Edit menu(1), Undo(4) disabled(0), Redo(5) enabled(1)
        char *data_copy = safe_strdup((char*)states_data);
        char *saveptr;
        char *state_str = strtok_r(data_copy, ";", &saveptr);

        while (state_str) {
            int menu_idx, item_idx, enabled;
            if (sscanf(state_str, "%d,%d,%d", &menu_idx, &item_idx, &enabled) == 3) {
                // Update the menu item state
                if (menu_idx >= 0 && menu_idx < full_menu_item_count) {
                    Menu *submenu = full_submenus[menu_idx];
                    if (submenu && item_idx >= 0 && item_idx < submenu->item_count) {
                        submenu->enabled[item_idx] = (enabled != 0);
                    }
                }
            }
            state_str = strtok_r(NULL, ";", &saveptr);
        }

        free(data_copy);
        XFree(states_data);

        // Redraw the menu if it's currently visible
        Menu *active = get_active_menu();
        if (active && active->canvas) {
            redraw_canvas(active->canvas);
        }
    }
}

// ============================================================================
// Menu Selection Communication
// ============================================================================

// Send menu selection back to app via client message
void send_menu_selection_to_app(Window app_window, int menu_index, int item_index) {
    Display *dpy = itn_core_get_display();
    if (!dpy) return;

    XEvent event;
    memset(&event, 0, sizeof(event));
    event.type = ClientMessage;
    event.xclient.window = app_window;
    event.xclient.message_type = XInternAtom(dpy, "_AMIWB_MENU_SELECT", False);
    event.xclient.format = 32;
    event.xclient.data.l[0] = menu_index;  // Which menu (0=File, 1=Edit, etc)
    event.xclient.data.l[1] = item_index;  // Which item in that menu

    XSendEvent(dpy, app_window, False, NoEventMask, &event);
    XFlush(dpy);

    // Sent menu selection to app
}
