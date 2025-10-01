// Menu System - Core Module
// Global state, initialization, cleanup, and menu lifecycle

#include "menu_internal.h"
#include "menu_public.h"
#include "../intuition/itn_public.h"
#include "../workbench/wb_public.h"
#include "../font_manager.h"
#include "../events.h"
#include "../config.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ============================================================================
// Global State
// ============================================================================

// Menu state - global so both menubar and popups share state
static XftColor text_color;
Menu *menubar = NULL;           // Global menubar
Menu *active_menu = NULL;       // Current dropdown menu (top-level)
Menu *nested_menu = NULL;       // Currently open nested submenu
bool show_menus = false;        // State: false for logo, true for menus

// Mode-specific arrays
char **logo_items = NULL;
int logo_item_count = 1;
char **full_menu_items = NULL;
int full_menu_item_count = 0;
Menu **full_submenus = NULL;

// Menu substitution system - save system menus when app menus active
char *system_logo_item = NULL;      // Original "AmiWB" logo
char **system_menu_items = NULL;    // System menu items backup
Menu **system_submenus = NULL;      // System submenus backup
int system_menu_item_count = 0;     // System menu count backup
bool app_menu_active = false;       // True when app menus are shown
Window current_app_window = None;   // Window that owns current app menus

// ============================================================================
// Helper Functions
// ============================================================================

// Helper to allocate and initialize shortcuts array with NULLs
void init_menu_shortcuts(Menu *menu) {
    if (!menu) return;
    menu->shortcuts = calloc(menu->item_count, sizeof(char*));
    // calloc initializes all to NULL, so no need to set individually
}

// Helper to allocate and initialize enabled array (all true by default)
void init_menu_enabled(Menu *menu) {
    if (!menu) return;
    menu->enabled = malloc(menu->item_count * sizeof(bool));
    if (!menu->enabled) return;
    // Default all items to enabled
    for (int i = 0; i < menu->item_count; i++) {
        menu->enabled[i] = true;
    }
}

// Initialize checkmarks array with all items unchecked
void init_menu_checkmarks(Menu *menu) {
    if (!menu) return;
    menu->checkmarks = calloc(menu->item_count, sizeof(bool));
    // calloc already zeros the array, so all items start unchecked
}

// Parse menu items for "#" shortcut notation and extract shortcuts
// Updates the menu's items and shortcuts arrays accordingly
void parse_menu_item_shortcuts(Menu *menu) {
    if (!menu || !menu->items) return;

    for (int i = 0; i < menu->item_count; i++) {
        if (!menu->items[i]) continue;

        char *item = strdup(menu->items[i]);  // Work with a copy
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
            menu->items[i] = strdup(item);

            // Store the shortcut
            if (menu->shortcuts && menu->shortcuts[i]) {
                free(menu->shortcuts[i]);  // Free any existing shortcut
            }
            if (menu->shortcuts) {
                menu->shortcuts[i] = strdup(shortcut);
            }
        }

        free(item);  // Free the working copy
    }
}

// Helper: Check if active window is in icons view mode
static bool get_active_view_is_icons(void) {
    Canvas *active_window = itn_focus_get_active();
    return active_window ? (active_window->view_mode == VIEW_ICONS) : true;  // Default to Icons
}

// Update View Modes menu checkmarks based on current state
void update_view_modes_checkmarks(void) {
    Menu *menubar_menu = get_menubar_menu();
    if (!menubar_menu || !menubar_menu->submenus || !menubar_menu->submenus[1]) return;

    Menu *win_menu = menubar_menu->submenus[1];  // Windows menu
    if (!win_menu->submenus || !win_menu->submenus[6]) return;

    Menu *view_modes = win_menu->submenus[6];  // View Modes submenu
    if (!view_modes->checkmarks) return;

    // Update checkmarks based on current state
    bool is_icons = get_active_view_is_icons();
    view_modes->checkmarks[0] = is_icons;      // Icons
    view_modes->checkmarks[1] = !is_icons;     // Names
    view_modes->checkmarks[2] = get_global_show_hidden_state();  // Hidden
    view_modes->checkmarks[3] = get_spatial_mode();  // Spatial
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
// Menu Cleanup
// ============================================================================

// Free menu and all its resources recursively
void free_menu(Menu *menu) {
    if (!menu) return;

    // Free submenus recursively
    if (menu->submenus) {
        for (int i = 0; i < menu->item_count; i++) {
            if (menu->submenus[i]) {
                free_menu(menu->submenus[i]);
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

    menubar = calloc(1, sizeof(Menu));  // calloc zeros all fields
    if (!menubar) return;
    menubar->canvas = create_canvas(NULL, 0, 0, XDisplayWidth(ctx->dpy, DefaultScreen(ctx->dpy)), MENU_ITEM_HEIGHT, MENU);
    if (!menubar->canvas) {
        free(menubar);
        menubar = NULL;
        return;
    }
    menubar->canvas->bg_color = (XRenderColor){0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};

    menubar->item_count = 4;
    menubar->items = malloc(menubar->item_count * sizeof(char*));
    menubar->items[0] = strdup("Workbench");
    menubar->items[1] = strdup("Windows");
    menubar->items[2] = strdup("Icons");
    menubar->items[3] = strdup("Tools");
    menubar->shortcuts = NULL;  // Top-level menubar doesn't need shortcuts
    menubar->enabled = NULL;  // Top-level menubar doesn't need enabled states
    menubar->window_refs = NULL;  // Regular menus don't have window references
    menubar->selected_item = -1;
    menubar->parent_menu = NULL;
    menubar->submenus = malloc(menubar->item_count * sizeof(Menu*));
    memset(menubar->submenus, 0, menubar->item_count * sizeof(Menu*));

    // Workbench submenu (index 0)
    // Basic actions for the environment and global app state.
    Menu *wb_submenu = calloc(1, sizeof(Menu));  // zeros all fields
    wb_submenu->item_count = 7;
    wb_submenu->items = malloc(wb_submenu->item_count * sizeof(char*));
    wb_submenu->items[0] = strdup("Execute #E");
    wb_submenu->items[1] = strdup("Requester #L");
    wb_submenu->items[2] = strdup("Settings");
    wb_submenu->items[3] = strdup("About");
    wb_submenu->items[4] = strdup("Suspend #^S");
    wb_submenu->items[5] = strdup("Restart AmiWB #^R");
    wb_submenu->items[6] = strdup("Quit AmiWB #^Q");

    // Initialize shortcuts array (will be populated by parsing)
    wb_submenu->shortcuts = calloc(wb_submenu->item_count, sizeof(char*));

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

    // Windows submenu (index 1)
    // Window management and content view controls.
    Menu *win_submenu = calloc(1, sizeof(Menu));  // zeros all fields
    win_submenu->item_count = 7;  // Added Refresh
    win_submenu->items = malloc(win_submenu->item_count * sizeof(char*));
    win_submenu->items[0] = strdup("New Drawer #N");
    win_submenu->items[1] = strdup("Open Parent #P");
    win_submenu->items[2] = strdup("Close #Q");
    win_submenu->items[3] = strdup("Select Contents #A");
    win_submenu->items[4] = strdup("Clean Up #;");
    win_submenu->items[5] = strdup("Refresh #H");
    win_submenu->items[6] = strdup("View Modes");

    // Initialize shortcuts array (will be populated by parsing)
    win_submenu->shortcuts = calloc(win_submenu->item_count, sizeof(char*));
    init_menu_enabled(win_submenu);  // Initialize all items as enabled
    // Parse the "#" shortcuts from menu items
    parse_menu_item_shortcuts(win_submenu);
    win_submenu->selected_item = -1;
    win_submenu->parent_menu = menubar;
    win_submenu->parent_index = 1;
    win_submenu->submenus = calloc(win_submenu->item_count, sizeof(Menu*));
    win_submenu->canvas = NULL;

    // Create nested submenu for "View Modes" (index 6)
    // Switch listing mode and toggle hidden files.
    Menu *view_by_sub = calloc(1, sizeof(Menu));  // zeros all fields
    view_by_sub->item_count = 4;  // Icons, Names, Hidden, Spatial
    view_by_sub->items = malloc(view_by_sub->item_count * sizeof(char*));
    view_by_sub->items[0] = strdup("Icons");
    view_by_sub->items[1] = strdup("Names");
    view_by_sub->items[2] = strdup("Hidden");
    view_by_sub->items[3] = strdup("Spatial");
    init_menu_shortcuts(view_by_sub);  // Initialize all shortcuts to NULL
    init_menu_enabled(view_by_sub);  // Initialize all items as enabled
    init_menu_checkmarks(view_by_sub);  // Initialize checkmarks array
    // Set initial checkmark states based on current system state
    // Default to icons mode on startup (first window will set actual state)
    view_by_sub->checkmarks[0] = true;   // Icons checked by default
    view_by_sub->checkmarks[1] = false;  // Names unchecked by default
    view_by_sub->checkmarks[2] = get_global_show_hidden_state();  // Hidden checked if showing hidden
    view_by_sub->checkmarks[3] = get_spatial_mode();  // Spatial checked if in spatial mode
    view_by_sub->selected_item = -1;
    view_by_sub->parent_menu = win_submenu;
    view_by_sub->parent_index = 6;  // Position within Windows submenu
    view_by_sub->submenus = NULL;
    view_by_sub->canvas = NULL;
    win_submenu->submenus[6] = view_by_sub;
    menubar->submenus[1] = win_submenu;

    // Icons submenu (index 2)
    // Per-icon actions; entries are placeholders to be wired later.
    Menu *icons_submenu = calloc(1, sizeof(Menu));  // zeros all fields
    icons_submenu->item_count = 7;
    icons_submenu->items = malloc(icons_submenu->item_count * sizeof(char*));
    icons_submenu->items[0] = strdup("Open #O");
    icons_submenu->items[1] = strdup("Copy #C");
    icons_submenu->items[2] = strdup("Rename #R");
    icons_submenu->items[3] = strdup("Extract #X");      // Extract archives
    icons_submenu->items[4] = strdup("Eject #Y");        // Eject removable drives
    icons_submenu->items[5] = strdup("Information #I");
    icons_submenu->items[6] = strdup("delete #D");

    // Initialize shortcuts array (will be populated by parsing)
    icons_submenu->shortcuts = calloc(icons_submenu->item_count, sizeof(char*));

    init_menu_enabled(icons_submenu);  // Initialize all items as enabled
    // Parse the "#" shortcuts from menu items
    parse_menu_item_shortcuts(icons_submenu);
    icons_submenu->selected_item = -1;
    icons_submenu->parent_menu = menubar;
    icons_submenu->parent_index = 2;
    icons_submenu->submenus = NULL;
    icons_submenu->canvas = NULL;
    menubar->submenus[2] = icons_submenu;

    // Tools submenu (index 3)
    // Quick launchers for external apps; editable in config later.
    Menu *tools_submenu = calloc(1, sizeof(Menu));  // zeros all fields
    tools_submenu->item_count = 4;
    tools_submenu->items = malloc(tools_submenu->item_count * sizeof(char*));
    tools_submenu->items[0] = strdup("Text Editor");
    tools_submenu->items[1] = strdup("XCalc");
    tools_submenu->items[2] = strdup("Shell");
    tools_submenu->items[3] = strdup("Debug Console");
    init_menu_shortcuts(tools_submenu);  // Initialize all shortcuts to NULL
    init_menu_enabled(tools_submenu);  // Initialize all items as enabled
    tools_submenu->selected_item = -1;
    tools_submenu->parent_menu = menubar;
    tools_submenu->parent_index = 3;
    tools_submenu->submenus = NULL;
    tools_submenu->canvas = NULL;
    menubar->submenus[3] = tools_submenu;

    // Load custom menus from config file (adds to menubar after system menus)
    load_custom_menus();

    // Setup mode-specific arrays
    // Menubar can display a single logo item or full menu items.
    logo_items = malloc(logo_item_count * sizeof(char*));
    logo_items[0] = strdup("AmiWB");

    full_menu_item_count = menubar->item_count;  // Now includes custom menus
    full_menu_items = menubar->items;
    full_submenus = menubar->submenus;

    // Initial default mode: logo
    // Start minimal; switch to full menu on user toggle.
    menubar->items = logo_items;
    menubar->item_count = logo_item_count;
    menubar->submenus = NULL;

    // Initialize menu addons
    menuaddon_clock_init();             // Register clock addon
    menuaddon_cpu_init();               // Register CPU addon
    menuaddon_memory_init();            // Register memory addon
    menuaddon_fans_init();              // Register fans addon
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

    menubar->items = realloc(menubar->items, new_count * sizeof(char*));
    menubar->submenus = realloc(menubar->submenus, new_count * sizeof(Menu*));
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
                menubar->items[menu_index] = strdup(line + 1);

                // Create submenu
                current_menu = calloc(1, sizeof(Menu));
                current_menu->canvas = NULL;
                current_menu->selected_item = -1;
                current_menu->parent_menu = menubar;
                current_menu->parent_index = menu_index;
                current_menu->submenus = NULL;
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
                temp_items = realloc(temp_items, temp_capacity * sizeof(char*));
                temp_commands = realloc(temp_commands, temp_capacity * sizeof(char*));
            }

            temp_items[temp_count] = strdup(label_start);
            temp_commands[temp_count] = strdup(cmd_start);
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
        for (int i = 0; i < temp_count; i++) {
            current_menu->enabled[i] = true;
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
            destroy_canvas(active_menu->canvas);
            active_menu->canvas = NULL;  // Prevent double-free
        }
        active_menu = NULL;
    }

    // Free menubar and all its submenus
    if (menubar) {
        // Destroy menubar canvas
        if (menubar->canvas) {
            clear_press_target_if_matches(menubar->canvas->win);
            destroy_canvas(menubar->canvas);
        }

        // Free all submenus recursively using helper
        if (menubar->submenus) {
            for (int i = 0; i < menubar->item_count; i++) {
                if (menubar->submenus[i]) {
                    free_menu(menubar->submenus[i]);
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
