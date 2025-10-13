// Menu System - Core Module
// Global state, initialization, cleanup, and menu lifecycle

#include "menu_internal.h"
#include "menu_public.h"
#include "../intuition/itn_public.h"
#include "../workbench/wb_public.h"
#include "../font_manager.h"
#include "../events/evt_public.h"
#include "../config.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ============================================================================
// Safe Memory Allocation Helpers
// ============================================================================

// Safe strdup wrapper - returns NULL on failure for graceful degradation
static char* safe_strdup(const char *str) {
    if (!str) return NULL;  // Allow NULL input for optional strings
    char *copy = strdup(str);
    if (!copy) {
        log_error("[ERROR] strdup failed - menu text may be missing");
        return NULL;  // Graceful degradation
    }
    return copy;
}

// ============================================================================
// Global State
// ============================================================================

// Menu state - properly encapsulated (AWP principle)
static XftColor text_color;
static Menu *menubar = NULL;           // Global menubar
static Menu *active_menu = NULL;       // Current dropdown menu (top-level)
static Menu *nested_menu = NULL;       // Currently open nested submenu
static bool show_menus = false;        // State: false for logo, true for menus

// Mode-specific arrays
static char **logo_items = NULL;
static int logo_item_count = 1;
static char **full_menu_items = NULL;
static int full_menu_item_count = 0;
static Menu **full_submenus = NULL;

// Menu substitution system - save system menus when app menus active
static char *system_logo_item = NULL;      // Original "AmiWB" logo
static char **system_menu_items = NULL;    // System menu items backup
static Menu **system_submenus = NULL;      // System submenus backup
static int system_menu_item_count = 0;     // System menu count backup
static bool app_menu_active = false;       // True when app menus are shown
static Window current_app_window = None;   // Window that owns current app menus

// ============================================================================
// Helper Functions
// ============================================================================

// Helper to allocate and initialize shortcuts array with NULLs
void init_menu_shortcuts(Menu *menu) {
    if (!menu) return;
    menu->shortcuts = calloc(menu->item_count, sizeof(char*));
    if (!menu->shortcuts) {
        log_error("[ERROR] calloc failed for menu shortcuts - menu will work without shortcuts");
        return;  // Graceful degradation: menu works without shortcuts
    }
    // calloc initializes all to NULL, so no need to set individually
}

// Helper to allocate and initialize enabled array (all true by default)
void init_menu_enabled(Menu *menu) {
    if (!menu) return;
    menu->enabled = malloc(menu->item_count * sizeof(bool));
    if (!menu->enabled) {
        log_error("[ERROR] malloc failed for menu enabled array - all items will appear enabled");
        return;  // Graceful degradation: menu works without enabled/disabled state
    }
    // Default all items to enabled
    for (int i = 0; i < menu->item_count; i++) {
        menu->enabled[i] = true;
    }
}

// Initialize checkmarks array with all items unchecked
void init_menu_checkmarks(Menu *menu) {
    if (!menu) return;
    menu->checkmarks = calloc(menu->item_count, sizeof(bool));
    if (!menu->checkmarks) {
        log_error("[ERROR] calloc failed for menu checkmarks - menu will work without checkmarks");
        return;  // Graceful degradation: menu works without toggle state
    }
    // calloc already zeros the array, so all items start unchecked
}

// Parse menu items for "#" shortcut notation and extract shortcuts
// Updates the menu's items and shortcuts arrays accordingly
void parse_menu_item_shortcuts(Menu *menu) {
    if (!menu || !menu->items) return;

    for (int i = 0; i < menu->item_count; i++) {
        if (!menu->items[i]) continue;

        char *item = safe_strdup(menu->items[i]);  // Work with a copy
        char *delimiter = strchr(item, '#');

        if (delimiter) {
            // Found "#" - split into name and shortcut
            *delimiter = '\0';  // Terminate item name at #
            char *shortcut = delimiter + 1;  // Shortcut is everything after #

            // Trim trailing spaces from item name
            char *end = item + strlen(item) - 1;
            while (end > item && *end == ' ') {
                *end = '\0';
                end--;
            }

            // Update the item to just the name (without shortcut)
            free(menu->items[i]);
            menu->items[i] = safe_strdup(item);

            // Store the shortcut
            if (menu->shortcuts && menu->shortcuts[i]) {
                free(menu->shortcuts[i]);  // Free any existing shortcut
            }
            if (menu->shortcuts) {
                menu->shortcuts[i] = safe_strdup(shortcut);
            }
        }

        free(item);  // Free the working copy
    }
}
// Update View Modes menu checkmarks based on current state
void update_view_modes_checkmarks(void) {
    Menu *menubar_menu = get_menubar_menu();
    if (!menubar_menu || !menubar_menu->submenus || !menubar_menu->submenus[1]) return;

    Menu *win_menu = menubar_menu->submenus[1];  // Windows menu
    if (!win_menu->submenus || !win_menu->submenus[6]) return;

    Menu *view_modes = win_menu->submenus[6];  // View Modes submenu
    if (!view_modes->checkmarks) return;

    // Update checkmarks based on global state
    ViewMode global_mode = get_global_view_mode();
    view_modes->checkmarks[0] = (global_mode == VIEW_ICONS);   // Icons
    view_modes->checkmarks[1] = (global_mode == VIEW_NAMES);   // Names
    view_modes->checkmarks[2] = get_global_show_hidden_state(); // Hidden
    view_modes->checkmarks[3] = get_spatial_mode();             // Spatial
}

// Get submenu width - measure widest label to size the dropdown width
int get_submenu_width(Menu *menu) {
    if (!menu || !font_manager_get()) return 80;
    int max_label_width = 0;
    int max_shortcut_width = 0;
    int padding = 20;

    // Find widest label and widest shortcut
    for (int i = 0; i < menu->item_count; i++) {
        // Measure label width
        XGlyphInfo label_extents;
        XftTextExtentsUtf8(get_render_context()->dpy, font_manager_get(),
            (FcChar8 *)menu->items[i], strlen(menu->items[i]), &label_extents);
        if (label_extents.xOff > max_label_width) {
            max_label_width = label_extents.xOff;
        }

        // Measure shortcut width if present
        if (menu->shortcuts && menu->shortcuts[i]) {
            char shortcut_text[32];
            // No space for shortcuts with modifiers (^Q), but keep space for single chars (E)
            if (menu->shortcuts[i] && menu->shortcuts[i][0] == '^') {
                snprintf(shortcut_text, sizeof(shortcut_text), "%s%s", SHORTCUT_SYMBOL, menu->shortcuts[i]);
            } else {
                snprintf(shortcut_text, sizeof(shortcut_text), "%s %s", SHORTCUT_SYMBOL, menu->shortcuts[i]);
            }
            XGlyphInfo shortcut_extents;
            XftTextExtentsUtf8(get_render_context()->dpy, font_manager_get(),
                (FcChar8 *)shortcut_text, strlen(shortcut_text), &shortcut_extents);
            if (shortcut_extents.xOff > max_shortcut_width) {
                max_shortcut_width = shortcut_extents.xOff;
            }
        }
    }

    // Calculate total width: label + 4 char gap + shortcut + 1 char padding
    int gap_width = 40;  // Minimum 4 character spaces between label and shortcut
    int end_padding = 10;  // 1 character space after shortcut
    int total_width = padding + max_label_width + gap_width + max_shortcut_width + end_padding;

    // Ensure minimum width
    return total_width > 80 ? total_width : 80;
}

// ============================================================================
// Menu Lifecycle Management
// ============================================================================

// OWNERSHIP: Returns allocated Menu - caller must call destroy_menu()
// Returns NULL on failure (graceful degradation - menu won't appear)
Menu* create_menu(const char* title, int item_count) {
    (void)title;  // Title parameter reserved for future use

    Menu* menu = calloc(1, sizeof(Menu));
    if (!menu) {
        log_error("[ERROR] Failed to allocate Menu structure - menu unavailable");
        return NULL;  // Graceful degradation
    }

    menu->item_count = item_count;

    if (item_count > 0) {
        menu->items = calloc(item_count, sizeof(char*));
        menu->shortcuts = calloc(item_count, sizeof(char*));
        menu->submenus = calloc(item_count, sizeof(Menu*));

        if (!menu->items || !menu->shortcuts || !menu->submenus) {
            log_error("[ERROR] Failed to allocate menu arrays - menu unavailable");
            // Free any successful allocations before returning
            free(menu->items);
            free(menu->shortcuts);
            free(menu->submenus);
            free(menu);
            return NULL;  // Graceful degradation
        }
    }

    return menu;
}

// Free menu and all its resources recursively
void destroy_menu(Menu *menu) {
    if (!menu) return;

    // Free submenus recursively
    if (menu->submenus) {
        for (int i = 0; i < menu->item_count; i++) {
            if (menu->submenus[i]) {
                destroy_menu(menu->submenus[i]);
            }
        }
        free(menu->submenus);
    }

    // Free items
    if (menu->items) {
        for (int i = 0; i < menu->item_count; i++) {
            free(menu->items[i]);
        }
        free(menu->items);
    }

    // Free shortcuts
    if (menu->shortcuts) {
        for (int i = 0; i < menu->item_count; i++) {
            free(menu->shortcuts[i]);
        }
        free(menu->shortcuts);
    }

    // Free commands (for custom menus)
    if (menu->commands) {
        for (int i = 0; i < menu->item_count; i++) {
            free(menu->commands[i]);
        }
        free(menu->commands);
    }

    // Free enabled array
    free(menu->enabled);

    // Free checkmarks array
    free(menu->checkmarks);

    // Free window_refs array (for window list menus)
    free(menu->window_refs);

    // Canvas is destroyed elsewhere when menus close, not here

    // Free the menu struct itself
    free(menu);
}

// ============================================================================
// Menu Initialization
// ============================================================================

// Initialize menu resources
// Builds menubar tree with submenus. The menubar is a Canvas so it can be
// redrawn like any other window.
void init_menus(void) {
    RenderContext *ctx = get_render_context();
    if (!ctx) return;

    // Font is now loaded by font_manager
    if (!font_manager_get()) {
        log_error("[ERROR] Font not initialized - call font_manager_init first");
        return;
    }

    text_color.color = (XRenderColor){0x0000, 0x0000, 0x0000, 0xFFFF}; // Black

    // create_menu returns NULL on failure
    menubar = create_menu("Menubar", 4);
    if (!menubar) {
        log_error("[ERROR] Failed to create menubar - AmiWB will run without menus");
        return;  // Graceful degradation: keyboard shortcuts still work
    }
    menubar->canvas = create_canvas(NULL, 0, 0, XDisplayWidth(ctx->dpy, DefaultScreen(ctx->dpy)), MENU_ITEM_HEIGHT, MENU);
    if (!menubar->canvas) {
        destroy_menu(menubar);
        menubar = NULL;
        return;
    }
    menubar->canvas->bg_color = (XRenderColor){0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};

    menubar->items[0] = safe_strdup("Workbench");
    menubar->items[1] = safe_strdup("Windows");
    menubar->items[2] = safe_strdup("Icons");
    menubar->items[3] = safe_strdup("Tools");
    menubar->shortcuts = NULL;  // Top-level menubar doesn't need shortcuts
    menubar->enabled = NULL;  // Top-level menubar doesn't need enabled states
    menubar->window_refs = NULL;  // Regular menus don't have window references
    menubar->selected_item = -1;
    menubar->parent_menu = NULL;
    menubar->submenus = malloc(menubar->item_count * sizeof(Menu*));
    if (!menubar->submenus) {
        log_error("[ERROR] malloc failed for menubar submenus - menus unavailable");
        itn_canvas_destroy(menubar->canvas);
        destroy_menu(menubar);
        menubar = NULL;
        return;  // Graceful degradation
    }
    memset(menubar->submenus, 0, menubar->item_count * sizeof(Menu*));

    // Workbench submenu (index 0)
    // Basic actions for the environment and global app state.
    Menu *wb_submenu = create_menu("Workbench", 7);
    if (!wb_submenu) {
        log_error("[ERROR] Failed to create Workbench submenu - menu unavailable");
        menubar->submenus[0] = NULL;
        goto create_windows_menu;  // Skip to next menu
    }
    wb_submenu->items[0] = safe_strdup("Execute #E");
    wb_submenu->items[1] = safe_strdup("Requester #L");
    wb_submenu->items[2] = safe_strdup("Settings");
    wb_submenu->items[3] = safe_strdup("About");
    wb_submenu->items[4] = safe_strdup("Suspend #^S");
    wb_submenu->items[5] = safe_strdup("Restart AmiWB #^R");
    wb_submenu->items[6] = safe_strdup("Quit AmiWB #^Q");

    init_menu_enabled(wb_submenu);  // Initialize all items as enabled
    // Parse the "#" shortcuts from menu items
    parse_menu_item_shortcuts(wb_submenu);
    // Gray out Settings and About menu items (not implemented yet)
    wb_submenu->enabled[2] = false;  // Settings
    wb_submenu->enabled[3] = false;  // About
    wb_submenu->selected_item = -1;
    wb_submenu->parent_menu = menubar;
    wb_submenu->parent_index = 0;
    wb_submenu->submenus = NULL;
    wb_submenu->canvas = NULL;
    menubar->submenus[0] = wb_submenu;

create_windows_menu:
    // Windows submenu (index 1)
    // Window management and content view controls.
    Menu *win_submenu = create_menu("Windows", 7);
    if (!win_submenu) {
        log_error("[ERROR] Failed to create Windows submenu - menu unavailable");
        menubar->submenus[1] = NULL;
        goto create_icons_menu;  // Skip to next menu
    }
    win_submenu->items[0] = safe_strdup("New Drawer #N");
    win_submenu->items[1] = safe_strdup("Open Parent #P");
    win_submenu->items[2] = safe_strdup("Close #Q");
    win_submenu->items[3] = safe_strdup("Select Contents #A");
    win_submenu->items[4] = safe_strdup("Clean Up #;");
    win_submenu->items[5] = safe_strdup("Refresh #H");
    win_submenu->items[6] = safe_strdup("View Modes");

    init_menu_enabled(win_submenu);  // Initialize all items as enabled
    // Parse the "#" shortcuts from menu items
    parse_menu_item_shortcuts(win_submenu);
    win_submenu->selected_item = -1;
    win_submenu->parent_menu = menubar;
    win_submenu->parent_index = 1;
    win_submenu->canvas = NULL;

    // Create nested submenu for "View Modes" (index 6)
    // Switch listing mode and toggle hidden files.
    Menu *view_by_sub = create_menu("View Modes", 4);
    if (!view_by_sub) {
        log_error("[ERROR] Failed to create View Modes submenu - feature unavailable");
        win_submenu->submenus[6] = NULL;
    } else {
        view_by_sub->items[0] = safe_strdup("Icons #1");
    view_by_sub->items[1] = safe_strdup("Names #2");
    view_by_sub->items[2] = safe_strdup("Hidden #3");
    view_by_sub->items[3] = safe_strdup("Spatial #4");
    init_menu_shortcuts(view_by_sub);  // Initialize all shortcuts to NULL
    init_menu_enabled(view_by_sub);  // Initialize all items as enabled
    init_menu_checkmarks(view_by_sub);  // Initialize checkmarks array
    // Parse shortcuts from menu items (e.g., "Icons #^1" -> "Icons" + "^1")
    parse_menu_item_shortcuts(view_by_sub);
    // Set initial checkmark states based on global system state
    ViewMode init_mode = get_global_view_mode();
    view_by_sub->checkmarks[0] = (init_mode == VIEW_ICONS);      // Icons
    view_by_sub->checkmarks[1] = (init_mode == VIEW_NAMES);      // Names
    view_by_sub->checkmarks[2] = get_global_show_hidden_state(); // Hidden
    view_by_sub->checkmarks[3] = get_spatial_mode();             // Spatial
    view_by_sub->selected_item = -1;
    view_by_sub->parent_menu = win_submenu;
    view_by_sub->parent_index = 6;  // Position within Windows submenu
    view_by_sub->submenus = NULL;
    view_by_sub->canvas = NULL;
    win_submenu->submenus[6] = view_by_sub;
    }
    menubar->submenus[1] = win_submenu;

create_icons_menu:
    // Icons submenu (index 2)
    // Per-icon actions; entries are placeholders to be wired later.
    Menu *icons_submenu = create_menu("Icons", 7);
    if (!icons_submenu) {
        log_error("[ERROR] Failed to create Icons submenu - menu unavailable");
        menubar->submenus[2] = NULL;
        goto create_tools_menu;  // Skip to next menu
    }
    icons_submenu->items[0] = safe_strdup("Open #O");
    icons_submenu->items[1] = safe_strdup("Copy #C");
    icons_submenu->items[2] = safe_strdup("Rename #R");
    icons_submenu->items[3] = safe_strdup("Extract #X");      // Extract archives
    icons_submenu->items[4] = safe_strdup("Eject #Y");        // Eject removable drives
    icons_submenu->items[5] = safe_strdup("Information #I");
    icons_submenu->items[6] = safe_strdup("delete #D");

    init_menu_enabled(icons_submenu);  // Initialize all items as enabled
    // Parse the "#" shortcuts from menu items
    parse_menu_item_shortcuts(icons_submenu);
    icons_submenu->selected_item = -1;
    icons_submenu->parent_menu = menubar;
    icons_submenu->parent_index = 2;
    icons_submenu->submenus = NULL;
    icons_submenu->canvas = NULL;
    menubar->submenus[2] = icons_submenu;

create_tools_menu:
    // Tools submenu (index 3)
    // Quick launchers for external apps; editable in config later.
    Menu *tools_submenu = create_menu("Tools", 4);
    if (!tools_submenu) {
        log_error("[ERROR] Failed to create Tools submenu - menu unavailable");
        menubar->submenus[3] = NULL;
        goto finish_menu_init;  // Skip to final initialization
    }
    tools_submenu->items[0] = safe_strdup("Text Editor");
    tools_submenu->items[1] = safe_strdup("XCalc");
    tools_submenu->items[2] = safe_strdup("Shell");
    tools_submenu->items[3] = safe_strdup("Debug Console");
    init_menu_shortcuts(tools_submenu);  // Initialize all shortcuts to NULL
    init_menu_enabled(tools_submenu);  // Initialize all items as enabled
    tools_submenu->selected_item = -1;
    tools_submenu->parent_menu = menubar;
    tools_submenu->parent_index = 3;
    tools_submenu->submenus = NULL;
    tools_submenu->canvas = NULL;
    menubar->submenus[3] = tools_submenu;

finish_menu_init:
    // Load custom menus from config file (adds to menubar after system menus)
    load_custom_menus();

    // Setup mode-specific arrays
    // Menubar can display a single logo item or full menu items.
    logo_items = malloc(logo_item_count * sizeof(char*));
    if (!logo_items) {
        log_error("[ERROR] malloc failed for logo_items - starting in full menu mode");
        // Graceful degradation: skip logo mode, start with full menus
        show_menus = true;
    } else {
        logo_items[0] = safe_strdup("AmiWB");
    }

    full_menu_item_count = menubar->item_count;  // Now includes custom menus
    full_menu_items = menubar->items;
    full_submenus = menubar->submenus;

    // Initial default mode: logo (unless allocation failed)
    // Start minimal; switch to full menu on user toggle.
    if (!show_menus && logo_items) {
        menubar->items = logo_items;
        menubar->item_count = logo_item_count;
        menubar->submenus = NULL;
    }

    // Initialize menu addons
    menuaddon_clock_init();             // Register clock addon
    menuaddon_cpu_init();               // Register CPU addon
    menuaddon_memory_init();            // Register memory addon
    menuaddon_fans_init();              // Register fans addon
    menuaddon_temps_init();             // Register temps addon
    menu_addon_load_config();           // Enable addons from config

    redraw_canvas(menubar->canvas);
}

// Load custom menus from toolsdaemonrc config file
void load_custom_menus(void) {
    if (!menubar) return;

    // Try user config first, then system config
    const char *home = getenv("HOME");
    char config_path[PATH_SIZE];
    FILE *fp = NULL;

    if (home) {
        snprintf(config_path, sizeof(config_path), "%s/.config/amiwb/toolsdaemonrc", home);
        fp = fopen(config_path, "r");
    }

    if (!fp) {
        // Try system fallback
        fp = fopen("/usr/local/share/amiwb/dotfiles/toolsdaemonrc", "r");
    }

    if (!fp) {
        // No config file found, that's okay
        return;
    }

    // Count how many custom menus we'll need
    int custom_menu_count = 0;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\0') continue;

        // Check for menu header [menu_name]
        if (line[0] == '[') {
            custom_menu_count++;
        }
    }

    if (custom_menu_count == 0) {
        fclose(fp);
        return;
    }

    // Rewind to parse again
    rewind(fp);

    // Expand menubar arrays to include custom menus
    int old_count = menubar->item_count;
    int new_count = old_count + custom_menu_count;

    char **new_items = realloc(menubar->items, new_count * sizeof(char*));
    if (!new_items) {
        log_error("[ERROR] realloc failed for menubar items - keeping system menus only");
        fclose(fp);
        return;  // Graceful degradation: system menus still work
    }
    menubar->items = new_items;

    Menu **new_submenus = realloc(menubar->submenus, new_count * sizeof(Menu*));
    if (!new_submenus) {
        log_error("[ERROR] realloc failed for menubar submenus - keeping system menus only");
        fclose(fp);
        return;  // Graceful degradation
    }
    menubar->submenus = new_submenus;

    menubar->item_count = new_count;

    // Parse and create custom menus
    int menu_index = old_count;
    Menu *current_menu = NULL;
    char **temp_items = NULL;
    char **temp_commands = NULL;
    int temp_count = 0;
    int temp_capacity = 0;

    while (fgets(line, sizeof(line), fp)) {
        // Remove newline
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';

        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '\0') continue;

        // Check for menu header [menu_name]
        if (line[0] == '[') {
            // Save previous menu if exists
            if (current_menu && temp_count > 0) {
                current_menu->items = temp_items;
                current_menu->commands = temp_commands;
                current_menu->item_count = temp_count;
                current_menu->shortcuts = NULL;
                current_menu->enabled = calloc(temp_count, sizeof(bool));
                for (int i = 0; i < temp_count; i++) {
                    current_menu->enabled[i] = true;
                }

                // Reset for next menu
                temp_items = NULL;
                temp_commands = NULL;
                temp_count = 0;
                temp_capacity = 0;
            }

            // Extract menu name
            char *end = strchr(line + 1, ']');
            if (end) {
                *end = '\0';
                menubar->items[menu_index] = safe_strdup(line + 1);

                // Create submenu using consistent allocation (0 items, will be set during parsing)
                current_menu = create_menu(NULL, 0);
                if (!current_menu) {
                    log_error("[ERROR] Failed to create custom menu - skipping");
                    menubar->submenus[menu_index] = NULL;
                    menu_index++;
                    continue;  // Skip this custom menu
                }
                current_menu->parent_menu = menubar;
                current_menu->parent_index = menu_index;
                current_menu->is_custom = true;

                menubar->submenus[menu_index] = current_menu;
                menu_index++;
            }
        }
        // Parse menu item "Label" = "Command"
        else if (current_menu && strchr(line, '=')) {
            char *equals = strchr(line, '=');
            *equals = '\0';

            // Extract label (strip quotes and whitespace)
            char *label_start = line;
            char *label_end = equals - 1;
            while (*label_start == ' ' || *label_start == '"') label_start++;
            while (label_end > label_start && (*label_end == ' ' || *label_end == '"')) label_end--;
            label_end[1] = '\0';

            // Extract command (strip quotes and whitespace)
            char *cmd_start = equals + 1;
            while (*cmd_start == ' ' || *cmd_start == '"') cmd_start++;
            char *cmd_end = cmd_start + strlen(cmd_start) - 1;
            while (cmd_end > cmd_start && (*cmd_end == ' ' || *cmd_end == '"')) cmd_end--;
            cmd_end[1] = '\0';

            // Grow arrays if needed
            if (temp_count >= temp_capacity) {
                temp_capacity = temp_capacity ? temp_capacity * 2 : 4;
                char **new_temp_items = realloc(temp_items, temp_capacity * sizeof(char*));
                if (!new_temp_items) {
                    log_error("[ERROR] realloc failed for temp_items - custom menu incomplete");
                    fclose(fp);
                    return;  // Graceful degradation: system menus still work
                }
                temp_items = new_temp_items;

                char **new_temp_commands = realloc(temp_commands, temp_capacity * sizeof(char*));
                if (!new_temp_commands) {
                    log_error("[ERROR] realloc failed for temp_commands - custom menu incomplete");
                    fclose(fp);
                    return;  // Graceful degradation
                }
                temp_commands = new_temp_commands;
            }

            temp_items[temp_count] = safe_strdup(label_start);
            temp_commands[temp_count] = safe_strdup(cmd_start);
            temp_count++;
        }
    }

    // Save last menu if exists
    if (current_menu && temp_count > 0) {
        current_menu->items = temp_items;
        current_menu->commands = temp_commands;
        current_menu->item_count = temp_count;
        current_menu->shortcuts = NULL;
        current_menu->enabled = calloc(temp_count, sizeof(bool));
        if (!current_menu->enabled) {
            log_error("[ERROR] calloc failed for custom menu enabled array - items will appear enabled");
            // Graceful degradation: menu works without enabled/disabled state
        } else {
            for (int i = 0; i < temp_count; i++) {
                current_menu->enabled[i] = true;
            }
        }
    }

    fclose(fp);
}

// ============================================================================
// Menu Cleanup
// ============================================================================

void cleanup_menus(void) {
    RenderContext *ctx = get_render_context();
    if (!ctx) return;

    // Cleanup menu addons
    menu_addon_cleanup_all();

    // Release color resources
    if (text_color.pixel) XftColorFree(ctx->dpy, ctx->default_visual, ctx->default_colormap, &text_color);

    // Clear active menu reference
    if (active_menu) {
        if (active_menu->canvas) {
            clear_press_target_if_matches(active_menu->canvas->win);
            itn_canvas_destroy(active_menu->canvas);
            active_menu->canvas = NULL;  // Prevent double-free
        }
        active_menu = NULL;
    }

    // Free menubar and all its submenus
    if (menubar) {
        // Destroy menubar canvas
        if (menubar->canvas) {
            clear_press_target_if_matches(menubar->canvas->win);
            itn_canvas_destroy(menubar->canvas);
        }

        // Free all submenus recursively using helper
        if (menubar->submenus) {
            for (int i = 0; i < menubar->item_count; i++) {
                if (menubar->submenus[i]) {
                    destroy_menu(menubar->submenus[i]);
                }
            }
            free(menubar->submenus);
        }

        // Free menubar items
        if (menubar->items) {
            for (int i = 0; i < menubar->item_count; i++) {
                free(menubar->items[i]);
            }
            free(menubar->items);
        }

        // Free shortcuts and enabled if they exist
        if (menubar->shortcuts) {
            for (int i = 0; i < menubar->item_count; i++) {
                free(menubar->shortcuts[i]);
            }
            free(menubar->shortcuts);
        }
        free(menubar->enabled);

        // Free the menubar struct
        free(menubar);
        menubar = NULL;
    }

    // Clear full menu pointers (they point to menubar arrays, already freed above)
    // Don't free the items - they were already freed with menubar
    full_menu_items = NULL;
    full_submenus = NULL;
    full_menu_item_count = 0;

    // Free logo items
    for (int i = 0; i < logo_item_count; i++) {
        free(logo_items[i]);
    }
    free(logo_items);
    logo_items = NULL;
    logo_item_count = 0;

    // Free system menu backup (app menu substitution system)
    if (system_logo_item) {
        free(system_logo_item);
        system_logo_item = NULL;
    }
    // system_menu_items and system_submenus point to menubar arrays (already freed above)
    system_menu_items = NULL;
    system_submenus = NULL;
    system_menu_item_count = 0;
    app_menu_active = false;
    current_app_window = None;
}

// ============================================================================
// Accessors
// ============================================================================

bool get_show_menus_state(void) {
    return show_menus;
}

Canvas *get_menubar(void) {
    return menubar ? menubar->canvas : NULL;
}

Menu *get_menubar_menu(void) {
    return menubar;
}

Menu *get_menu_by_canvas(Canvas *canvas) {
    if (canvas == get_menubar()) return get_menubar_menu();
    if (active_menu && active_menu->canvas == canvas) return active_menu;
    if (nested_menu && nested_menu->canvas == canvas) return nested_menu;
    return NULL;
}

Menu *get_active_menu(void) {
    return active_menu;
}

bool is_app_menu_active(void) {
    return app_menu_active;
}

Window get_app_menu_window(void) {
    return current_app_window;
}

// ============================================================================
// Additional State Accessors (AWP Encapsulation)
// ============================================================================

// Nested menu access
Menu *menu_core_get_nested_menu(void) {
    return nested_menu;
}

void menu_core_set_nested_menu(Menu *menu) {
    nested_menu = menu;
}

void menu_core_set_active_menu(Menu *menu) {
    active_menu = menu;
}

// Logo mode arrays (read-only - managed internally during init)
char **menu_core_get_logo_items(void) {
    return logo_items;
}

int menu_core_get_logo_item_count(void) {
    return logo_item_count;
}

// Full menu arrays (read-only - managed internally during init)
char **menu_core_get_full_menu_items(void) {
    return full_menu_items;
}

Menu **menu_core_get_full_submenus(void) {
    return full_submenus;
}

int menu_core_get_full_menu_item_count(void) {
    return full_menu_item_count;
}

// System menu backup (read-only - managed internally during substitution)
char *menu_core_get_system_logo_item(void) {
    return system_logo_item;
}

char **menu_core_get_system_menu_items(void) {
    return system_menu_items;
}

Menu **menu_core_get_system_submenus(void) {
    return system_submenus;
}

int menu_core_get_system_menu_item_count(void) {
    return system_menu_item_count;
}

// App menu state setters
void menu_core_set_app_menu_active(bool active) {
    app_menu_active = active;
}

void menu_core_set_app_menu_window(Window win) {
    current_app_window = win;
}

// Show menus toggle
void menu_core_toggle_show_menus(void) {
    show_menus = !show_menus;
}

// Menu substitution helpers (for app menu system)
void menu_core_save_system_menus(void) {
    if (!system_menu_items && !app_menu_active) {
        if (logo_items && logo_items[0]) {
            system_logo_item = strdup(logo_items[0]);
        }
        system_menu_items = full_menu_items;
        system_submenus = full_submenus;
        system_menu_item_count = full_menu_item_count;
    }
}

void menu_core_switch_to_app_menus(char **menu_items, Menu **submenus, int count) {
    full_menu_items = menu_items;
    full_submenus = submenus;
    full_menu_item_count = count;
}

void menu_core_restore_system_menus(void) {
    if (system_menu_items) {
        full_menu_items = system_menu_items;
        full_submenus = system_submenus;
        full_menu_item_count = system_menu_item_count;
    }
}
