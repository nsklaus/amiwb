// File: menus.c
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdbool.h>
#include "menus.h"
#include "config.h"
#include "intuition.h"
#include "workbench.h"
#include "render.h"
#include "compositor.h"
#include "dialogs.h"
#include "iconinfo.h"
#include "events.h"
#include "icons.h"
#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>
#include <fontconfig/fontconfig.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

// Static menu resources
// Menu font and state are global so both menubar and popups share
// metrics and selection state without passing through every call.
static XftFont *font = NULL;
static XftColor text_color;
static Menu *active_menu = NULL;    // Current dropdown menu (top-level or current)
static Menu *nested_menu = NULL;    // Currently open nested submenu (child of active_menu)
static Menu *menubar = NULL;        // Global menubar
static bool show_menus = false;     // State: false for logo, true for menus

// Forward declarations for rename callbacks
static void rename_file_ok_callback(const char *new_name);
static void rename_file_cancel_callback(void);

// Forward declarations for actions
void trigger_execute_action(void);
void trigger_requester_action(void);
void handle_restart_request(void);

// Global variable to store the icon being renamed
static FileIcon *g_rename_icon = NULL;

// Mode-specific arrays
static char **logo_items = NULL;
static int logo_item_count = 1;
static char **full_menu_items = NULL;
static int full_menu_item_count = 0;
static Menu **full_submenus = NULL;

// Resolve menu resources from user config first, then system dir.
static char *get_resource_path(const char *rel_path) {
    char *home = getenv("HOME");
    char user_path[PATH_SIZE];
    snprintf(user_path, sizeof(user_path), "%s/%s/%s", home, RESOURCE_DIR_USER, rel_path);
    if (access(user_path, F_OK) == 0) return strdup(user_path);
    char sys_path[PATH_SIZE];
    snprintf(sys_path, sizeof(sys_path), "%s/%s", RESOURCE_DIR_SYSTEM, rel_path);
    return strdup(sys_path);
}

// Static callback functions for rename dialog (avoid nested function trampolines)
static void rename_file_ok_callback(const char *new_name) {
    // Use the global icon that was set when dialog was shown
    FileIcon *icon = g_rename_icon;
    
    
    if (!icon || !new_name || strlen(new_name) == 0) {
        return;
    }
    
    // Additional validation - check if icon is still valid
    bool icon_valid = false;
    for (int i = 0; i < get_icon_count(); i++) {
        if (get_icon_array()[i] == icon) {
            icon_valid = true;
            break;
        }
    }
    if (!icon_valid) {
        log_error("[ERROR] Rename failed: icon no longer valid");
        return;
    }
    
    // Construct paths
    char old_path[PATH_SIZE];
    char new_path[PATH_SIZE]; 
    char *dir_path = strdup(icon->path);
    char *filename = strrchr(dir_path, '/');
    if (filename) *filename = '\0';  // Remove filename, keep directory
    
    snprintf(old_path, sizeof(old_path), "%s", icon->path);
    snprintf(new_path, sizeof(new_path), "%s/%s", dir_path, new_name);
    
    // Attempt rename with safety checks
    if (access(new_path, F_OK) == 0) {
        log_error("[ERROR] Rename failed: file '%s' already exists", new_name);
    } else if (rename(old_path, new_path) == 0) {
        // Success: update icon
        free(icon->label);
        icon->label = strdup(new_name);
        free(icon->path);
        icon->path = strdup(new_path);
        
        // Recalculate label width after rename
        XftFont *font = get_font();
        if (icon->label && font) {
            RenderContext *ctx = get_render_context();
            if (ctx) {
                XGlyphInfo extents;
                XftTextExtentsUtf8(ctx->dpy, font, (FcChar8 *)icon->label, strlen(icon->label), &extents);
                icon->label_width = extents.xOff;
            }
        }
        
        // Refresh display WITHOUT full directory reload - just update this icon
        Canvas *canvas = find_canvas(icon->display_window);
        if (canvas && canvas->path) {
            // Just redraw the canvas with the updated icon label
            redraw_canvas(canvas);
            
            // Force compositor to update
            compositor_sync_stacking(get_display());
            XSync(get_display(), False);
        }
        
    } else {
    }
    
    free(dir_path);
    g_rename_icon = NULL;  // Clear the global icon reference
}

static void rename_file_cancel_callback(void) {
    // Rename cancelled
    g_rename_icon = NULL;  // Clear the global icon reference
}

// Helper to allocate and initialize shortcuts array with NULLs
static void init_menu_shortcuts(Menu *menu) {
    if (!menu) return;
    menu->shortcuts = calloc(menu->item_count, sizeof(char*));
    // calloc initializes all to NULL, so no need to set individually
}

// Helper to allocate and initialize enabled array (all true by default)
static void init_menu_enabled(Menu *menu) {
    if (!menu) return;
    menu->enabled = malloc(menu->item_count * sizeof(bool));
    if (!menu->enabled) return;
    // Default all items to enabled
    for (int i = 0; i < menu->item_count; i++) {
        menu->enabled[i] = true;
    }
}

// Initialize menu resources
// Loads font and builds menubar tree with submenus. The menubar is a
// Canvas so it can be redrawn like any other window.
void init_menus(void) {
    RenderContext *ctx = get_render_context();
    if (!ctx) return;

    char *font_path = get_resource_path(SYSFONT);
    FcPattern *pattern = FcPatternCreate();
    FcPatternAddString(pattern, FC_FILE, (const FcChar8 *)font_path);
    FcPatternAddDouble(pattern, FC_SIZE, 12.0);
    FcPatternAddInteger(pattern, FC_WEIGHT, 200); // bold please
    FcPatternAddDouble(pattern, FC_DPI, 75);
    FcConfigSubstitute(NULL, pattern, FcMatchPattern);
    XftDefaultSubstitute(ctx->dpy, DefaultScreen(ctx->dpy), pattern);
    font = XftFontOpenPattern(ctx->dpy, pattern);
    if (!font) {
        log_error("[ERROR] Failed to load font %s", font_path);
        FcPatternDestroy(pattern);
        free(font_path);
        return;
    }
    free(font_path);

    text_color.color = (XRenderColor){0x0000, 0x0000, 0x0000, 0xFFFF}; // Black

    menubar = malloc(sizeof(Menu));
    if (!menubar) return;
    menubar->canvas = create_canvas(NULL, 0, 0, XDisplayWidth(ctx->dpy, DefaultScreen(ctx->dpy)), MENU_ITEM_HEIGHT, MENU);
    if (!menubar->canvas) {
        free(menubar);
        menubar = NULL;
        return;
    }
    menubar->canvas->bg_color = (XRenderColor){0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
    menubar->canvas->bg_color = (XRenderColor){0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};

    menubar->item_count = 4;
    menubar->items = malloc(menubar->item_count * sizeof(char*));
    menubar->items[0] = strdup("Workbench");
    menubar->items[1] = strdup("Window");
    menubar->items[2] = strdup("Icons");
    menubar->items[3] = strdup("Tools");
    menubar->shortcuts = NULL;  // Top-level menubar doesn't need shortcuts
    menubar->enabled = NULL;  // Top-level menubar doesn't need enabled states
    menubar->selected_item = -1;
    menubar->parent_menu = NULL;
    menubar->submenus = malloc(menubar->item_count * sizeof(Menu*));
    memset(menubar->submenus, 0, menubar->item_count * sizeof(Menu*));

    // Workbench submenu (index 0)
    // Basic actions for the environment and global app state.
    Menu *wb_submenu = malloc(sizeof(Menu));
    wb_submenu->item_count = 7;
    wb_submenu->items = malloc(wb_submenu->item_count * sizeof(char*));
    wb_submenu->items[0] = strdup("Execute");
    wb_submenu->items[1] = strdup("Requester");
    wb_submenu->items[2] = strdup("Settings");
    wb_submenu->items[3] = strdup("About");
    wb_submenu->items[4] = strdup("Suspend");
    wb_submenu->items[5] = strdup("Restart AmiWB");
    wb_submenu->items[6] = strdup("Quit AmiWB");
    
    // Initialize shortcuts for Workbench menu
    wb_submenu->shortcuts = malloc(wb_submenu->item_count * sizeof(char*));
    wb_submenu->shortcuts[0] = strdup("E");  // Execute - Super+E
    wb_submenu->shortcuts[1] = strdup("L");  // Requester - Super+L
    wb_submenu->shortcuts[2] = NULL;  // Settings - no shortcut yet
    wb_submenu->shortcuts[3] = NULL;  // About - no shortcut yet
    wb_submenu->shortcuts[4] = strdup("^S");  // Suspend - Super+Shift+S
    wb_submenu->shortcuts[5] = strdup("^R");  // Restart - Super+Shift+R
    wb_submenu->shortcuts[6] = strdup("^Q");  // Quit - Super+Shift+Q
    
    init_menu_enabled(wb_submenu);  // Initialize all items as enabled
    // Gray out Settings and About menu items (not implemented yet)
    wb_submenu->enabled[2] = false;  // Settings
    wb_submenu->enabled[3] = false;  // About
    wb_submenu->selected_item = -1;
    wb_submenu->parent_menu = menubar;
    wb_submenu->parent_index = 0;
    wb_submenu->submenus = NULL;
    wb_submenu->canvas = NULL;
    menubar->submenus[0] = wb_submenu;

    // Window submenu (index 1)
    // Window management and content view controls.
    Menu *win_submenu = malloc(sizeof(Menu));
    win_submenu->item_count = 8;
    win_submenu->items = malloc(win_submenu->item_count * sizeof(char*));
    win_submenu->items[0] = strdup("New Drawer");
    win_submenu->items[1] = strdup("Open Parent");
    win_submenu->items[2] = strdup("Close");
    win_submenu->items[3] = strdup("Select Contents");
    win_submenu->items[4] = strdup("Clean Up");
    win_submenu->items[5] = strdup("Show Hidden");
    win_submenu->items[6] = strdup("View By ..");
    win_submenu->items[7] = strdup("Cycle");
    
    // Initialize shortcuts for Window menu
    win_submenu->shortcuts = malloc(win_submenu->item_count * sizeof(char*));
    win_submenu->shortcuts[0] = strdup("N");  // New Drawer - Super+N
    win_submenu->shortcuts[1] = strdup("P");  // Open Parent - Super+P
    win_submenu->shortcuts[2] = strdup("Q");  // Close - Super+Q
    win_submenu->shortcuts[3] = strdup("A");  // Select Contents - Super+A
    win_submenu->shortcuts[4] = strdup(";");  // Clean Up - Super+;
    win_submenu->shortcuts[5] = NULL;  // Show Hidden - no shortcut yet
    win_submenu->shortcuts[6] = NULL;  // View By .. - no shortcut yet
    win_submenu->shortcuts[7] = NULL;  // Cycle - submenu with shortcuts
    init_menu_enabled(win_submenu);  // Initialize all items as enabled
    win_submenu->selected_item = -1;
    win_submenu->parent_menu = menubar;
    win_submenu->parent_index = 1;
    win_submenu->submenus = calloc(win_submenu->item_count, sizeof(Menu*));
    win_submenu->canvas = NULL;
    // Create nested submenu for "Show Hidden" (index 5)
    // Simple Yes/No toggle nested under Window menu.
    Menu *show_hidden_sub = malloc(sizeof(Menu));
    show_hidden_sub->item_count = 2;
    show_hidden_sub->items = malloc(show_hidden_sub->item_count * sizeof(char*));
    show_hidden_sub->items[0] = strdup("Yes");
    show_hidden_sub->items[1] = strdup("No");
    init_menu_shortcuts(show_hidden_sub);  // Initialize all shortcuts to NULL
    init_menu_enabled(show_hidden_sub);  // Initialize all items as enabled
    show_hidden_sub->selected_item = -1;
    show_hidden_sub->parent_menu = win_submenu;   // parent is Window submenu
    show_hidden_sub->parent_index = 5;            // index within Window submenu
    show_hidden_sub->submenus = NULL;
    show_hidden_sub->canvas = NULL;
    win_submenu->submenus[5] = show_hidden_sub;
    // Create nested submenu for "View By .." (index 6)
    // Switch listing mode between icon and name views.
    Menu *view_by_sub = malloc(sizeof(Menu));
    view_by_sub->item_count = 2;
    view_by_sub->items = malloc(view_by_sub->item_count * sizeof(char*));
    view_by_sub->items[0] = strdup("Icons");
    view_by_sub->items[1] = strdup("Names");
    init_menu_shortcuts(view_by_sub);  // Initialize all shortcuts to NULL
    init_menu_enabled(view_by_sub);  // Initialize all items as enabled
    view_by_sub->selected_item = -1;
    view_by_sub->parent_menu = win_submenu;
    view_by_sub->parent_index = 6;
    view_by_sub->submenus = NULL;
    view_by_sub->canvas = NULL;
    win_submenu->submenus[6] = view_by_sub;
    // Create nested submenu for "Cycle" (index 7)
    // Cycle through open windows with Next/Previous
    Menu *cycle_sub = malloc(sizeof(Menu));
    cycle_sub->item_count = 2;
    cycle_sub->items = malloc(cycle_sub->item_count * sizeof(char*));
    cycle_sub->items[0] = strdup("Next");
    cycle_sub->items[1] = strdup("Previous");
    cycle_sub->shortcuts = malloc(cycle_sub->item_count * sizeof(char*));
    cycle_sub->shortcuts[0] = strdup("M");         // Next - Super+M
    cycle_sub->shortcuts[1] = strdup("^M");   // Previous - Super+Shift+M  
    init_menu_enabled(cycle_sub);  // Initialize all items as enabled
    cycle_sub->selected_item = -1;
    cycle_sub->parent_menu = win_submenu;
    cycle_sub->parent_index = 7;
    cycle_sub->submenus = NULL;
    cycle_sub->canvas = NULL;
    win_submenu->submenus[7] = cycle_sub;
    menubar->submenus[1] = win_submenu;

    // Icons submenu (index 2)
    // Per-icon actions; entries are placeholders to be wired later.
    Menu *icons_submenu = malloc(sizeof(Menu));
    icons_submenu->item_count = 5;
    icons_submenu->items = malloc(icons_submenu->item_count * sizeof(char*));
    icons_submenu->items[0] = strdup("Open");
    icons_submenu->items[1] = strdup("Copy");
    icons_submenu->items[2] = strdup("Rename");
    icons_submenu->items[3] = strdup("Information");
    icons_submenu->items[4] = strdup("delete");
    
    // Initialize shortcuts array
    icons_submenu->shortcuts = malloc(icons_submenu->item_count * sizeof(char*));
    icons_submenu->shortcuts[0] = strdup("O");  // Open - Super+O
    icons_submenu->shortcuts[1] = strdup("C");  // Copy - Super+C
    icons_submenu->shortcuts[2] = strdup("R");  // Rename - Super+R
    icons_submenu->shortcuts[3] = strdup("I");  // Information - Super+I
    icons_submenu->shortcuts[4] = strdup("D");  // delete - Super+D
    
    init_menu_enabled(icons_submenu);  // Initialize all items as enabled
    icons_submenu->selected_item = -1;
    icons_submenu->parent_menu = menubar;
    icons_submenu->parent_index = 2;
    icons_submenu->submenus = NULL;
    icons_submenu->canvas = NULL;
    menubar->submenus[2] = icons_submenu;

    // Tools submenu (index 3)
    // Quick launchers for external apps; editable in config later.
    Menu *tools_submenu = malloc(sizeof(Menu));
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
            label_end[1] = '\0';  // Use array indexing instead of pointer arithmetic
            
            // Extract command (strip quotes and whitespace)
            char *cmd_start = equals + 1;
            while (*cmd_start == ' ' || *cmd_start == '"') cmd_start++;
            char *cmd_end = cmd_start + strlen(cmd_start) - 1;
            while (cmd_end > cmd_start && (*cmd_end == ' ' || *cmd_end == '"')) cmd_end--;
            cmd_end[1] = '\0';  // Use array indexing instead of pointer arithmetic
            
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

// Execute a custom menu command
void execute_custom_command(const char *cmd) {
    launch_with_hook(cmd);
}

// Update menubar if time changed (called periodically)
void update_menubar_time(void) {
#if MENU_SHOW_DATE
    static time_t last_minute = 0;
    time_t now = time(NULL);
    
    // Check if minute changed
    if (now / 60 != last_minute / 60) {
        last_minute = now;
        
        // Redraw menubar if in logo mode
        if (menubar && menubar->canvas && !get_show_menus_state()) {
            redraw_canvas(menubar->canvas);
        }
    }
#endif
}

// Forward declaration
static void free_menu(Menu *menu);

void cleanup_menus(void) {
    RenderContext *ctx = get_render_context();
    if (!ctx) return;

    // Release font/color
    if (font) XftFontClose(ctx->dpy, font);
    if (text_color.pixel) XftColorFree(ctx->dpy, ctx->default_visual, ctx->default_colormap, &text_color);
    
    // Clear active menu reference
    if (active_menu) {
        if (active_menu->canvas) {
            clear_press_target_if_matches(active_menu->canvas->win);
            destroy_canvas(active_menu->canvas);
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
    
    // Free full menu backup arrays
    for (int i = 0; i < full_menu_item_count; i++) {
        free(full_menu_items[i]);
    }
    free(full_menu_items);
    free(full_submenus);
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
    
    // Cleanup menus
}

// Get show_menus state
bool get_show_menus_state(void) {
    return show_menus;
}

// Toggle menubar state
// Switch between logo mode and full menus.
// Also closes any open dropdowns safely.
void toggle_menubar_state(void) {
    show_menus = !show_menus;
    if (show_menus) {

        menubar->items = full_menu_items;
        menubar->item_count = full_menu_item_count;
        menubar->submenus = full_submenus;

    } else {
        menubar->items = logo_items;
        menubar->item_count = logo_item_count;
        menubar->submenus = NULL;
        menubar->selected_item = -1;
        if (active_menu && active_menu->canvas) {  // Close any open submenu
            RenderContext *ctx = get_render_context();
            XSync(ctx->dpy, False);  // Complete pending operations
            if (ctx && active_menu->canvas->win != None) {
                clear_press_target_if_matches(active_menu->canvas->win);  // Clear before destroy
                XUnmapWindow(ctx->dpy, active_menu->canvas->win);
                XSync(ctx->dpy, False);  // Wait for unmap
            }
            destroy_canvas(active_menu->canvas);
            active_menu = NULL;
        }
        if (nested_menu && nested_menu->canvas) {
            RenderContext *ctx = get_render_context();
            XSync(ctx->dpy, False);  // Complete pending operations
            if (ctx && nested_menu->canvas->win != None) {
                clear_press_target_if_matches(nested_menu->canvas->win);  // Clear before destroy
                XUnmapWindow(ctx->dpy, nested_menu->canvas->win);
                XSync(ctx->dpy, False);  // Wait for unmap
            }
            destroy_canvas(nested_menu->canvas);
            nested_menu = NULL;
        }

    }
    if (menubar) redraw_canvas(menubar->canvas);
}

// Get global menubar canvas
// Get the menubar canvas (or NULL if not initialized).
Canvas *get_menubar(void) {
    return menubar ? menubar->canvas : NULL;
}

// Get the menubar Menu struct
// Access the menubar Menu struct for selection state.
Menu *get_menubar_menu(void) {
    return menubar;
}

// Get Menu for a canvas
// Resolve which Menu owns a given canvas.
Menu *get_menu_by_canvas(Canvas *canvas) {
    if (canvas == get_menubar()) return get_menubar_menu();
    if (active_menu && active_menu->canvas == canvas) return active_menu;
    if (nested_menu && nested_menu->canvas == canvas) return nested_menu;
    return NULL;
}

// Get Show Hidden state from active window or desktop (for checkmark display)
bool get_global_show_hidden(void) {
    Canvas *active_window = get_active_window();
    if (active_window) {
        return active_window->show_hidden;
    }
    // No active window - check desktop
    Canvas *desktop = get_desktop_canvas();
    return desktop ? desktop->show_hidden : false;
}

// Get View Mode from active window (for checkmark display)
// Returns true if Icons view, false if Names view
bool get_active_view_is_icons(void) {
    Canvas *active_window = get_active_window();
    return active_window ? (active_window->view_mode == VIEW_ICONS) : true;  // Default to Icons
}

// Handle motion for menubar
// Update hover selection across top-level items as the mouse moves.
// Opens the corresponding dropdown when the hovered item changes.
void menu_handle_menubar_motion(XMotionEvent *event) {
    if (!show_menus) return;

    RenderContext *ctx = get_render_context();

    if (!ctx || !menubar) return;
    int prev_selected = menubar->selected_item;
    menubar->selected_item = -1;
    int x_pos = 10;
    int padding = 20;
    for (int i = 0; i < menubar->item_count; i++) {
        XGlyphInfo extents;
        XftTextExtentsUtf8(ctx->dpy, font, (FcChar8 *)menubar->items[i], 
            strlen(menubar->items[i]), &extents);
        int item_width = extents.xOff + padding;
        if (event->x >= x_pos && event->x < x_pos + item_width) {
            menubar->selected_item = i;
            break;
        }
        x_pos += item_width;
    }
    if (menubar->selected_item != prev_selected) {
        // Safe menu cleanup with validation
        if (active_menu && active_menu->canvas) {
            XSync(ctx->dpy, False);  // Ensure pending operations complete
            if (active_menu->canvas->win != None) {
                XUnmapWindow(ctx->dpy, active_menu->canvas->win);
                XSync(ctx->dpy, False);  // Wait for unmap to complete
            }
            destroy_canvas(active_menu->canvas);
            active_menu = NULL;
        }
        if (nested_menu && nested_menu->canvas) {
            XSync(ctx->dpy, False);  // Ensure pending operations complete
            if (nested_menu->canvas->win != None) {
                XUnmapWindow(ctx->dpy, nested_menu->canvas->win);
                XSync(ctx->dpy, False);  // Wait for unmap to complete
            }
            destroy_canvas(nested_menu->canvas);
            nested_menu = NULL;
        }
        if (menubar->selected_item != -1 && 
                menubar->submenus[menubar->selected_item]) {
            int submenu_x = 10;
            for (int j = 0; j < menubar->selected_item; j++) {
                XGlyphInfo extents;
                XftTextExtentsUtf8(ctx->dpy, font, (FcChar8 *)menubar->items[j], 
                    strlen(menubar->items[j]), &extents);
                submenu_x += extents.xOff + padding;
            }
            show_dropdown_menu(menubar, menubar->selected_item, submenu_x, 
                MENU_ITEM_HEIGHT);
        }
        redraw_canvas(menubar->canvas);
    }        
}

// Close the currently open nested submenu, if any.
static void close_nested_if_any(void) {
    RenderContext *ctx = get_render_context();
    if (nested_menu && nested_menu->canvas) {
        XSync(ctx->dpy, False);  // Complete pending operations
        if (ctx && nested_menu->canvas->win != None) {
            clear_press_target_if_matches(nested_menu->canvas->win);  // Clear before destroy
            XUnmapWindow(ctx->dpy, nested_menu->canvas->win);
            XSync(ctx->dpy, False);  // Wait for unmap
        }
        destroy_canvas(nested_menu->canvas);
        nested_menu = NULL;
    }
}

// Handle clicks inside a dropdown or nested submenu.
// Just track the press, don't trigger actions yet.
void menu_handle_button_press(XButtonEvent *event) {
    // Just track the press, actions happen on release
}

// Handle button release inside menus - this triggers the actual action.
void menu_handle_button_release(XButtonEvent *event) {
    RenderContext *ctx = get_render_context();
    if (!ctx) return;

    Menu *target_menu = NULL;
    if (active_menu && event->window == active_menu->canvas->win) target_menu = active_menu;
    else if (nested_menu && event->window == nested_menu->canvas->win) target_menu = nested_menu;
    else return;

    int item = event->y / MENU_ITEM_HEIGHT;
    if (item >= 0 && item < target_menu->item_count) {
        // Don't handle selection if the item is disabled
        if (target_menu->enabled && !target_menu->enabled[item]) {
            return;  // Item is disabled, ignore the click
        }
        handle_menu_selection(target_menu, item);
    }
    // Close dropped-down menus after selection with safe validation
    if (nested_menu && nested_menu->canvas) {
        XSync(ctx->dpy, False);  // Complete pending operations
        if (nested_menu->canvas->win != None) {
            clear_press_target_if_matches(nested_menu->canvas->win);  // Clear before destroy
            XUnmapWindow(ctx->dpy, nested_menu->canvas->win);
            XSync(ctx->dpy, False);  // Wait for unmap
        }
        destroy_canvas(nested_menu->canvas);
        nested_menu = NULL;
    }
    if (active_menu && active_menu->canvas) {  
        XSync(ctx->dpy, False);  // Complete pending operations
        if (active_menu->canvas->win != None) {
            clear_press_target_if_matches(active_menu->canvas->win);  // Clear before destroy
            XUnmapWindow(ctx->dpy, active_menu->canvas->win);
            XSync(ctx->dpy, False);  // Wait for unmap
        }
        destroy_canvas(active_menu->canvas);
        active_menu = NULL;
    }
    // Conditional redraw: only if not quitting (running is true)
    if (running && menubar && menubar->canvas) {
        // Always revert menubar to logo state after a click
        if (get_show_menus_state()) toggle_menubar_state();
        redraw_canvas(menubar->canvas);
    }
}

// Handle button press on menubar
// Right-click toggles logo vs menus on the menubar.
void menu_handle_menubar_press(XButtonEvent *event) {
    if (event->button == Button3) {
        toggle_menubar_state();
    }
}

// Handle motion on dropdown
// If the highlighted item in the dropdown has a submenu, open it.
static void maybe_open_nested_for_selection(void) {
    if (!active_menu) return;
    // Only open nested if the selected item has a submenu
    if (!active_menu->submenus) return;
    int sel = active_menu->selected_item;
    if (sel < 0 || sel >= active_menu->item_count) return;
    Menu *child = active_menu->submenus[sel];
    RenderContext *ctx = get_render_context();
    if (!ctx) return;
    if (child) {
        // If already open for the same selection, do nothing
        if (nested_menu && nested_menu == child) return;
        // Close previous nested if any
        if (nested_menu && nested_menu->canvas) {
            XSync(ctx->dpy, False);  // Complete pending operations
            if (nested_menu->canvas->win != None) {
                clear_press_target_if_matches(nested_menu->canvas->win);  // Clear before destroy
                XUnmapWindow(ctx->dpy, nested_menu->canvas->win);
                XSync(ctx->dpy, False);  // Wait for unmap
            }
            destroy_canvas(nested_menu->canvas);
            nested_menu = NULL;
        }
        // Open new nested at the right edge of active_menu, aligned to item
        int submenu_width = get_submenu_width(child);
        int nx = active_menu->canvas->x + active_menu->canvas->width;
        int ny = active_menu->canvas->y + sel * MENU_ITEM_HEIGHT;
        nested_menu = child;
        nested_menu->canvas = create_canvas(NULL, nx, ny, submenu_width,
            nested_menu->item_count * MENU_ITEM_HEIGHT + 8, MENU);
        if (nested_menu->canvas) {
            nested_menu->canvas->bg_color = (XRenderColor){0xFFFF,0xFFFF,0xFFFF,0xFFFF};
            nested_menu->selected_item = -1;
            XMapRaised(ctx->dpy, nested_menu->canvas->win);
            redraw_canvas(nested_menu->canvas);
        }
    } else {
        // No child for this item; close nested if open
        close_nested_if_any();
    }
}

// Track hover within dropdowns and nested menus; redraw on change.
void menu_handle_motion_notify(XMotionEvent *event) {
    
    RenderContext *ctx = get_render_context();
    if (!ctx) return;

    if (active_menu && event->window == active_menu->canvas->win) {
        int prev_selected = active_menu->selected_item;
        int new_item = event->y / MENU_ITEM_HEIGHT;
        if (new_item < 0 || new_item >= active_menu->item_count) {
            active_menu->selected_item = -1;
        } else {
            // Don't highlight disabled items
            if (active_menu->enabled && !active_menu->enabled[new_item]) {
                active_menu->selected_item = -1;  // No selection for disabled items
            } else {
                active_menu->selected_item = new_item;
            }
        }
        if (active_menu->selected_item != prev_selected) {
            redraw_canvas(active_menu->canvas);
            maybe_open_nested_for_selection();
        }
        return;
    }

    if (nested_menu && event->window == nested_menu->canvas->win) {
        int prev_selected = nested_menu->selected_item;
        int new_item = event->y / MENU_ITEM_HEIGHT;
        if (new_item < 0 || new_item >= nested_menu->item_count) {
            nested_menu->selected_item = -1;
        } else {
            // Don't highlight disabled items
            if (nested_menu->enabled && !nested_menu->enabled[new_item]) {
                nested_menu->selected_item = -1;  // No selection for disabled items
            } else {
                nested_menu->selected_item = new_item;
            }
        }
        if (nested_menu->selected_item != prev_selected) {
            redraw_canvas(nested_menu->canvas);
        }
        return;
    }
}

// Handle key press for menu navigation
// Keyboard navigation placeholder for menus.
void menu_handle_key_press(XKeyEvent *event) {
    // Menubar registered key press event
}

// Show dropdown menu 
// Create and show a dropdown for the given menubar item at x,y.
void show_dropdown_menu(Menu *menu, int index, int x, int y) {
    
    if (!menu || index < 0 || index >= menu->item_count || 
            !menu->submenus[index]) {
        return;
    }

    // Close any nested submenu from a previous active dropdown
    if (nested_menu && nested_menu->canvas) {
        RenderContext *ctx = get_render_context();
        XSync(ctx->dpy, False);  // Complete pending operations
        if (ctx && nested_menu->canvas->win != None) {
            clear_press_target_if_matches(nested_menu->canvas->win);  // Clear before destroy
            XUnmapWindow(ctx->dpy, nested_menu->canvas->win);
            XSync(ctx->dpy, False);  // Wait for unmap
        }
        destroy_canvas(nested_menu->canvas);
        nested_menu = NULL;
    }
    active_menu = menu->submenus[index];
    
    // Update enabled states for Icons menu based on current selection
    if (menu == menubar && index == 2) {  // Icons menu
        bool has_selected_icon = false;
        bool can_delete = false;
        FileIcon *selected = NULL;
        Canvas *aw = get_active_window();
        Canvas *check_canvas = NULL;
        
        // If no active window, check desktop
        if (!aw || aw->type == DESKTOP) {
            check_canvas = get_desktop_canvas();
        } else if (aw->type == WINDOW) {
            check_canvas = aw;
        }
        
        if (check_canvas) {
            // Check if any icon is selected in the canvas
            FileIcon **icon_array = get_icon_array();
            int icon_count = get_icon_count();
            for (int i = 0; i < icon_count; i++) {
                FileIcon *icon = icon_array[i];
                if (icon && icon->selected && icon->display_window == check_canvas->win) {
                    has_selected_icon = true;
                    selected = icon;
                    break;
                }
            }
        }
        
        // Check restrictions for Copy, Rename, and Delete
        bool can_modify = false;  // For Copy and Rename
        if (selected) {
            // Can't modify (copy/rename/delete) System, Home, or iconified windows
            if (strcmp(selected->label, "System") != 0 && 
                strcmp(selected->label, "Home") != 0 &&
                selected->type != TYPE_ICONIFIED) {
                can_modify = true;
                can_delete = true;  // Delete has same restrictions as modify
            }
        }
        
        // Update the enabled states
        if (active_menu->enabled) {
            active_menu->enabled[0] = has_selected_icon;  // Open - works for all icon types
            active_menu->enabled[1] = can_modify;         // Copy - restricted
            active_menu->enabled[2] = can_modify;         // Rename - restricted
            active_menu->enabled[3] = has_selected_icon;  // Information - works for all
            active_menu->enabled[4] = can_delete;         // Delete - restricted
        }
    }
    
    // Update enabled states for Window menu based on active window
    if (menu == menubar && index == 1) {  // Window menu
        Canvas *aw = get_active_window();
        bool has_active_window = (aw && aw->type == WINDOW);
        bool is_workbench_window = (aw && aw->type == WINDOW && aw->client_win == None);
        bool desktop_focused = (aw == NULL);  // No active window means desktop is focused
        bool has_path = (is_workbench_window && aw->path);
        bool can_go_parent = false;
        
        // Check if we can go to parent (not already at root)
        if (has_path) {
            // Check if we're not already at root
            if (strlen(aw->path) > 1 || (strlen(aw->path) == 1 && aw->path[0] != '/')) {
                can_go_parent = true;
            }
        }
        
        // Update the enabled states based on window type
        if (active_menu->enabled) {
            // Desktop-focused or workbench window operations
            active_menu->enabled[0] = is_workbench_window || desktop_focused;  // New Drawer - workbench or desktop
            active_menu->enabled[1] = can_go_parent;                          // Open Parent - only for workbench with parent path
            active_menu->enabled[2] = has_active_window;                      // Close - only if there's a window
            active_menu->enabled[3] = is_workbench_window || desktop_focused;  // Select Contents - workbench or desktop
            active_menu->enabled[4] = is_workbench_window || desktop_focused;  // Clean Up - workbench or desktop
            active_menu->enabled[5] = is_workbench_window || desktop_focused;  // Show Hidden - workbench or desktop  
            active_menu->enabled[6] = is_workbench_window;                     // View By - only for workbench windows (not desktop)
            active_menu->enabled[7] = true;                                    // Cycle - always enabled
        }
    }
    
    int submenu_width = get_submenu_width(active_menu);
    active_menu->canvas = create_canvas(NULL, x, y, submenu_width, 
        active_menu->item_count * MENU_ITEM_HEIGHT + 8, MENU);  // Added 8 pixels: 4 for top offset, 4 for bottom padding
    if (!active_menu->canvas) { return; }
    active_menu->canvas->bg_color = 
        (XRenderColor){0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};

    active_menu->selected_item = -1;
    XMapRaised(get_render_context()->dpy, active_menu->canvas->win);
    redraw_canvas(active_menu->canvas);
}

// Process menu item selection
// Execute action for a selected menu item.
// Handles nested Window submenu toggles inline.
void handle_menu_selection(Menu *menu, int item_index) {
    const char *item = menu->items[item_index];
    // If this is a nested submenu under Window, handle here
    if (menu->parent_menu && menu->parent_menu->parent_menu == menubar && 
        menu->parent_menu->parent_index == 1) {
        // Determine which child: by parent_index in Window submenu
        if (menu->parent_index == 5) { // Show Hidden
            Canvas *target = get_active_window();
            if (!target) {
                // No active window - use desktop
                target = get_desktop_canvas();
            }
            if (target) {
                if (strcmp(item, "Yes") == 0) {
                    target->show_hidden = true;
                } else if (strcmp(item, "No") == 0) {
                    target->show_hidden = false;
                }
                // Refresh directory view to apply hidden filter
                if (target->path) {
                    refresh_canvas_from_directory(target, target->path);
                } else if (target->type == DESKTOP) {
                    // Desktop uses ~/Desktop as its path
                    const char *home = getenv("HOME");
                    if (home) {
                        char desktop_path[PATH_SIZE];
                        snprintf(desktop_path, sizeof(desktop_path), "%s/Desktop", home);
                        refresh_canvas_from_directory(target, desktop_path);
                    }
                }
                // Layout only applies to workbench windows, not desktop
                if (target->type == WINDOW) {
                    apply_view_layout(target);
                    compute_max_scroll(target);
                }
                redraw_canvas(target);
            }
        } else if (menu->parent_index == 6) { // View By ..
            Canvas *aw = get_active_window();
            if (aw) {
                if (strcmp(item, "Icons") == 0) set_canvas_view_mode(aw, VIEW_ICONS);
                else if (strcmp(item, "Names") == 0) set_canvas_view_mode(aw, VIEW_NAMES);
            }
        } else if (menu->parent_index == 7) { // Cycle
            if (strcmp(item, "Next") == 0) {
                cycle_next_window();
            } else if (strcmp(item, "Previous") == 0) {
                cycle_prev_window();
            }
        }
        return;
    }
    if (menu->parent_menu != menubar) return;  // Only top-level or handled above

    switch (menu->parent_index) {
        case 0:  // Workbench
            if (strcmp(item, "Execute") == 0) {
                trigger_execute_action();
            } else if (strcmp(item, "Requester") == 0) {
                trigger_requester_action();
            } else if (strcmp(item, "Settings") == 0) {
                // TODO: Open settings dialog or file
            } else if (strcmp(item, "About") == 0) {
                // TODO: Display about information

            } else if (strcmp(item, "Suspend") == 0) {
                handle_suspend_request();
            
            } else if (strcmp(item, "Restart AmiWB") == 0) {
                handle_restart_request();
                return;

            } else if (strcmp(item, "Quit AmiWB") == 0) {
                handle_quit_request();
                return;
            }
            break;

        case 1:  // Window
            if (strcmp(item, "New Drawer") == 0) {
                trigger_new_drawer_action();
            } else if (strcmp(item, "Open Parent") == 0) {
                trigger_parent_action();
            } else if (strcmp(item, "Close") == 0) {
                trigger_close_action();
            } else if (strcmp(item, "Select Contents") == 0) {
                trigger_select_contents_action();
            } else if (strcmp(item, "Clean Up") == 0) {
                trigger_cleanup_action();
            } else if (strcmp(item, "Show") == 0) {
                // TODO: toggle hidden items
            } else if (strcmp(item, "View Icons") == 0) {
                Canvas *aw = get_active_window();
                if (aw) set_canvas_view_mode(aw, VIEW_ICONS);
            } else if (strcmp(item, "View Names") == 0) {
                Canvas *aw = get_active_window();
                if (aw) set_canvas_view_mode(aw, VIEW_NAMES);
            }
            break;

        case 2:  // Icons
            if (strcmp(item, "Open") == 0) {
                trigger_open_action();
            } else if (strcmp(item, "Copy") == 0) {
                trigger_copy_action();
            } else if (strcmp(item, "Rename") == 0) {
                trigger_rename_action();
            } else if (strcmp(item, "Information") == 0) {
                trigger_icon_info_action();
            } else if (strcmp(item, "delete") == 0) {
                trigger_delete_action();
            }
            break;

        case 3:  // Tools
            if (strcmp(item, "Text Editor") == 0) {
                system("editpad &");
            } else if (strcmp(item, "XCalc") == 0) {
                system("xcalc &");  // TODO: Handle errors and paths
            } 

            /*
            else if (strcmp(item, "PavuControl") == 0) {
                //system("pavucontrol &");

            } 
            
            else if (strcmp(item, "Sublime Text") == 0) {
                system("subl &");

            } 

            else if (strcmp(item, "Brave Browser") == 0) {
                //printf("launching brave\n");
                //system("brave-browser --password-store=basic &");
            } 
            

            else if (strcmp(item, "Sublime Text") == 0) {
                system("subl &");   

            } 
	    */

	    else if (strcmp(item, "Shell") == 0) {
                system("kitty &"); 

            } else if (strcmp(item, "Debug Console") == 0) {
                // Open a terminal that tails the configured log file live.
                // Uses config.h LOG_FILE_PATH and kitty.
                #if LOGGING_ENABLED
                // Embed LOG_FILE_PATH into the shell; $HOME in the macro will expand in sh -lc
                system("sh -lc 'exec kitty -e sh -lc "
                       "\"tail -f \\\"" LOG_FILE_PATH "\\\"\"' &");
                #else
                system("sh -lc 'exec kitty -e sh -lc "
                       "\"echo Logging is disabled in config.h; echo Enable LOGGING_ENABLED and rebuild.; echo; read -p '""'Press Enter to close'""' \"\"\"' &");
                #endif
            }

            break;

        default:
            // Check if this is a custom menu
            if (menu->parent_index >= 4 && menu->is_custom && menu->commands) {
                // Execute the custom command for this item
                if (item_index < menu->item_count && menu->commands[item_index]) {
                    execute_custom_command(menu->commands[item_index]);
                }
            }
            break;
    }
    if (get_show_menus_state()) {
        toggle_menubar_state();
    }
}

// Get active menu
// Current open dropdown (not menubar).
Menu *get_active_menu(void) {
    return active_menu;
}

// Get submenu width
// Measure widest label to size the dropdown width, accounting for shortcuts.
int get_submenu_width(Menu *menu) {
    if (!menu || !font) return 80;
    int max_label_width = 0;
    int max_shortcut_width = 0;
    int padding = 20;
    
    // Find widest label and widest shortcut
    for (int i = 0; i < menu->item_count; i++) {
        // Measure label width
        XGlyphInfo label_extents;
        XftTextExtentsUtf8(get_render_context()->dpy, font, 
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
            XftTextExtentsUtf8(get_render_context()->dpy, font,
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

// Set app menu
// Placeholder: application-provided menu integration.
void set_app_menu(Menu *app_menu) {
    // TODO
}

// Public function to trigger clean up action (called from menu or global shortcut)
void trigger_cleanup_action(void) {
    Canvas *active_window = get_active_window();
    
    // Clean up active window if it exists, otherwise clean up desktop
    if (active_window && active_window->type == WINDOW) {
        icon_cleanup(active_window);
        compute_max_scroll(active_window);
        redraw_canvas(active_window);
    } else {
        Canvas *desktop = get_desktop_canvas();
        if (desktop) {
            icon_cleanup(desktop);
            compute_max_scroll(desktop);
            redraw_canvas(desktop);
        }
    }
}

// Public function to trigger close window action (called from menu or global shortcut)
void trigger_close_action(void) {
    Canvas *active_window = get_active_window();
    
    // Only close if there's an active window (not desktop)
    if (active_window && active_window->type == WINDOW) {
        // Use destroy_canvas which handles both client and non-client windows properly
        destroy_canvas(active_window);
    }
}

// Public function to trigger open parent action (called from menu or global shortcut)
void trigger_parent_action(void) {
    Canvas *active_window = get_active_window();
    
    // Only works if there's an active window with a path
    if (active_window && active_window->type == WINDOW && active_window->path) {
        // Get parent directory path
        char parent_path[PATH_SIZE];
        strncpy(parent_path, active_window->path, sizeof(parent_path) - 1);
        parent_path[sizeof(parent_path) - 1] = '\0';
        
        // Remove trailing slash if present
        size_t len = strlen(parent_path);
        if (len > 1 && parent_path[len - 1] == '/') {
            parent_path[len - 1] = '\0';
        }
        
        // Find last slash to get parent directory
        char *last_slash = strrchr(parent_path, '/');
        if (last_slash && last_slash != parent_path) {
            *last_slash = '\0';  // Truncate at last slash
        } else if (last_slash == parent_path) {
            // We're at root, keep the single slash
            parent_path[1] = '\0';
        } else {
            // No slash found or already at root
            return;
        }
        
        // Check if window for parent path already exists
        Canvas *existing = find_window_by_path(parent_path);
        if (existing) {
            set_active_window(existing);
            XRaiseWindow(get_display(), existing->win);
            redraw_canvas(existing);
        } else {
            // Create new window for parent directory
            Canvas *parent_window = create_canvas(parent_path, 
                active_window->x + 30, active_window->y + 30,
                640, 480, WINDOW);
            if (parent_window) {
                refresh_canvas_from_directory(parent_window, parent_path);
                apply_view_layout(parent_window);
                compute_max_scroll(parent_window);
                redraw_canvas(parent_window);
            }
        }
    }
}

// Helper function to open a file or directory
static void open_file_or_directory(FileIcon *icon) {
    if (!icon) return;
    
    // Handle different icon types
    if (icon->type == TYPE_DRAWER) {
        // Directories (including System and Home) - open within AmiWB
        if (!icon->path) return;
        
        // Check if window for this path already exists
        Canvas *existing = find_window_by_path(icon->path);
        if (existing) {
            set_active_window(existing);
            XRaiseWindow(get_display(), existing->win);
            redraw_canvas(existing);
        } else {
            // Create new window for directory
            Canvas *new_window = create_canvas(icon->path, 100, 100, 640, 480, WINDOW);
            if (new_window) {
                refresh_canvas_from_directory(new_window, icon->path);
                apply_view_layout(new_window);
                compute_max_scroll(new_window);
                redraw_canvas(new_window);
            }
        }
    } else if (icon->type == TYPE_ICONIFIED) {
        // Use the existing restore_iconified function from workbench.c
        // It properly handles window restoration and icon cleanup
        restore_iconified(icon);
    } else if (icon->type == TYPE_FILE) {
        // Use the existing open_file function from workbench.c
        open_file(icon);
    }
}

// Public function to trigger open action (called from menu or global shortcut)
void trigger_open_action(void) {
    // Get the selected icon from active window or desktop
    FileIcon *selected = NULL;
    Canvas *aw = get_active_window();
    Canvas *check_canvas = NULL;
    
    // If no active window, check desktop
    if (!aw || aw->type == DESKTOP) {
        check_canvas = get_desktop_canvas();
    } else if (aw->type == WINDOW) {
        check_canvas = aw;
    }
    
    if (check_canvas) {
        FileIcon **icon_array = get_icon_array();
        int icon_count = get_icon_count();
        for (int i = 0; i < icon_count; i++) {
            FileIcon *icon = icon_array[i];
            if (icon && icon->selected && icon->display_window == check_canvas->win) {
                selected = icon;
                break;
            }
        }
    }
    
    if (selected) {
        // Save label before calling open_file_or_directory as it may destroy the icon
        char label_copy[256];
        strncpy(label_copy, selected->label, sizeof(label_copy) - 1);
        label_copy[sizeof(label_copy) - 1] = '\0';
        
        open_file_or_directory(selected);
        // Use saved label as icon may have been destroyed
    } else {
    }
}

// Public function to trigger copy action (called from menu or global shortcut)
void trigger_copy_action(void) {
    // Get the selected icon from active window or desktop
    FileIcon *selected = NULL;
    Canvas *aw = get_active_window();
    Canvas *target_canvas = NULL;
    
    // If no active window, check desktop
    if (!aw || aw->type == DESKTOP) {
        target_canvas = get_desktop_canvas();
    } else if (aw->type == WINDOW) {
        target_canvas = aw;
    }
    
    if (target_canvas) {
        FileIcon **icon_array = get_icon_array();
        int icon_count = get_icon_count();
        for (int i = 0; i < icon_count; i++) {
            FileIcon *icon = icon_array[i];
            if (icon && icon->selected && icon->display_window == target_canvas->win) {
                selected = icon;
                break;
            }
        }
    }
    
    if (selected && selected->path) {
        // Check restrictions - cannot copy System, Home, or iconified windows
        if (strcmp(selected->label, "System") == 0 || strcmp(selected->label, "Home") == 0) {
            return;
        }
        if (selected->type == TYPE_ICONIFIED) {
            return;
        }
        
        // Generate copy name
        char copy_path[PATH_SIZE];
        char base_name[NAME_SIZE];
        char *last_slash = strrchr(selected->path, '/');
        char *dir_path = NULL;
        
        
        if (last_slash) {
            size_t dir_len = last_slash - selected->path;
            dir_path = malloc(dir_len + 2);
            if (!dir_path) {
                log_error("[ERROR] Failed to allocate memory for directory path");
                return;
            }
            strncpy(dir_path, selected->path, dir_len);
            dir_path[dir_len] = '\0';
            strncpy(base_name, last_slash + 1, NAME_SIZE - 1);
            base_name[NAME_SIZE - 1] = '\0';
            // Extract directory and base name
        } else {
            dir_path = strdup(".");
            strncpy(base_name, selected->path, NAME_SIZE - 1);
            base_name[NAME_SIZE - 1] = '\0';
            // Use current directory
        }
        
        // Find available copy name with prefix pattern: copy_myfile, copy1_myfile, copy2_myfile...
        int copy_num = 0;
        do {
            if (copy_num == 0) {
                // Ensure we don't overflow: dir_path + "/" + "copy_" + base_name
                if (strlen(dir_path) + strlen(base_name) + 7 < sizeof(copy_path)) {
                    snprintf(copy_path, sizeof(copy_path), "%s/copy_%s", dir_path, base_name);
                } else {
                    log_error("[ERROR] Path too long for copy operation");
                    free(dir_path);
                    return;
                }
            } else {
                // Ensure we don't overflow: dir_path + "/" + "copy" + num + "_" + base_name
                if (strlen(dir_path) + strlen(base_name) + 12 < sizeof(copy_path)) {
                    snprintf(copy_path, sizeof(copy_path), "%s/copy%d_%s", dir_path, copy_num, base_name);
                } else {
                    log_error("[ERROR] Path too long for copy operation");
                    free(dir_path);
                    return;
                }
            }
            copy_num++;
        } while (access(copy_path, F_OK) == 0 && copy_num < 100);
        
        // Save path before refresh as it will destroy the icon
        char saved_path[PATH_SIZE];
        strncpy(saved_path, selected->path, sizeof(saved_path) - 1);
        saved_path[sizeof(saved_path) - 1] = '\0';
        
        // Check if source has a sidecar .info file
        char src_info_path[PATH_SIZE + 10];  // PATH_SIZE + room for ".info"
        char dst_info_path[PATH_SIZE + 10];  // PATH_SIZE + room for ".info"
        bool has_sidecar = false;
        
        if (strlen(selected->path) < PATH_SIZE && strlen(copy_path) < PATH_SIZE) {
            snprintf(src_info_path, sizeof(src_info_path), "%s.info", selected->path);
            struct stat info_stat;
            if (stat(src_info_path, &info_stat) == 0) {
                has_sidecar = true;
                snprintf(dst_info_path, sizeof(dst_info_path), "%s.info", copy_path);
            }
        }
        
        // Find a good position for the new icon
        int new_x = selected->x + 110;
        int new_y = selected->y;
        
        if (target_canvas) {
            // Check for overlaps and adjust position
            FileIcon **icon_array = get_icon_array();
            int icon_count = get_icon_count();
            bool position_occupied = true;
            int attempts = 0;
            
            while (position_occupied && attempts < 10) {
                position_occupied = false;
                for (int i = 0; i < icon_count; i++) {
                    FileIcon *other = icon_array[i];
                    if (other && other != selected && 
                        other->display_window == target_canvas->win) {
                        if (abs(other->x - new_x) < 100 && abs(other->y - new_y) < 80) {
                            position_occupied = true;
                            if (attempts < 5) {
                                new_x += 110;
                            } else {
                                new_x = selected->x + 110;
                                new_y += 80;
                            }
                            break;
                        }
                    }
                }
                attempts++;
            }
        }
        
        // Prepare icon metadata for deferred creation
        typedef struct {
            enum { MSG_START, MSG_PROGRESS, MSG_COMPLETE, MSG_ERROR } type;
            time_t start_time;
            int files_done;
            int files_total;
            char current_file[128];
            size_t bytes_done;
            size_t bytes_total;
            char dest_path[512];
            char dest_dir[512];
            bool create_icon;
            bool has_sidecar;
            char sidecar_src[512];
            char sidecar_dst[512];
            int icon_x, icon_y;
            Window target_window;
        } ProgressMessage;
        
        ProgressMessage icon_metadata = {0};
        icon_metadata.create_icon = (target_canvas != NULL);
        icon_metadata.has_sidecar = has_sidecar;
        icon_metadata.icon_x = new_x;
        icon_metadata.icon_y = new_y;
        icon_metadata.target_window = target_canvas ? target_canvas->win : None;
        strncpy(icon_metadata.dest_path, copy_path, sizeof(icon_metadata.dest_path) - 1);
        strncpy(icon_metadata.dest_dir, dir_path, sizeof(icon_metadata.dest_dir) - 1);
        if (has_sidecar) {
            strncpy(icon_metadata.sidecar_src, src_info_path, sizeof(icon_metadata.sidecar_src) - 1);
            strncpy(icon_metadata.sidecar_dst, dst_info_path, sizeof(icon_metadata.sidecar_dst) - 1);
        }
        
        // Use extended file operation with icon metadata
        extern int perform_file_operation_with_progress_ex(int op, const char *src_path, 
                                                          const char *dst_path, const char *custom_title,
                                                          void *icon_metadata);
        int result = perform_file_operation_with_progress_ex(0, // FILE_OP_COPY = 0
                                                            selected->path, 
                                                            copy_path, 
                                                            "Copying Files...",
                                                            &icon_metadata);
        
        if (result == 0) {
            // Copy started successfully - icon will be created when it completes
            log_error("[INFO] Copy started: %s -> %s", saved_path, copy_path);
        } else {
            log_error("[ERROR] Copy failed for: %s", saved_path);  // Use saved path
        }
        
        free(dir_path);
    }
}

// Static storage for delete operation
static FileIcon *g_pending_delete_icons[256];
static int g_pending_delete_count = 0;
static Canvas *g_pending_delete_canvas = NULL;

// Actual delete execution after confirmation
static void execute_pending_deletes(void) {
    if (!g_pending_delete_canvas || g_pending_delete_count == 0) {
        log_error("[ERROR] No pending deletes or canvas lost!");
        return;
    }
    
    int delete_count = 0;
    bool need_layout_update = false;
    
    for (int i = 0; i < g_pending_delete_count; i++) {
        FileIcon *selected = g_pending_delete_icons[i];
        
        // CRITICAL: Verify icon still exists and belongs to same window
        bool icon_still_valid = false;
        FileIcon **icon_array = get_icon_array();
        int icon_count = get_icon_count();
        for (int j = 0; j < icon_count; j++) {
            if (icon_array[j] == selected && 
                icon_array[j]->display_window == g_pending_delete_canvas->win) {
                icon_still_valid = true;
                break;
            }
        }
        
        if (!icon_still_valid) {
            log_error("[WARNING] Icon no longer valid, skipping");
            continue;
        }
        
        // Skip System, Home icons, and iconified windows
        if (strcmp(selected->label, "System") == 0 || 
            strcmp(selected->label, "Home") == 0) {
            continue;
        }
        
        if (selected->type == TYPE_ICONIFIED) {
            continue;
        }
        
        // Save path before operations as destroy_icon will free it
        char saved_path[PATH_SIZE];
        strncpy(saved_path, selected->path, sizeof(saved_path) - 1);
        saved_path[sizeof(saved_path) - 1] = '\0';
        
        // Execute delete using progress-enabled function
        extern int perform_file_operation_with_progress(int op, const char *src_path, 
                                                       const char *dst_path, const char *custom_title);
        int result = perform_file_operation_with_progress(2, // FILE_OP_DELETE = 2
                                                         saved_path, 
                                                         NULL,  // No destination for delete
                                                         "Deleting Files...");
        
        // Check if file was actually deleted
        if (result != 0 && access(saved_path, F_OK) != 0) {
            result = 0;  // Force success since file is gone
        }
        
        if (result == 0) {
            // Also delete the sidecar .info file if it exists
            char sidecar_path[PATH_SIZE + 10];  // PATH_SIZE + room for ".info"
            snprintf(sidecar_path, sizeof(sidecar_path), "%s.info", saved_path);
            if (access(sidecar_path, F_OK) == 0) {
                // Sidecar exists, delete it
                if (unlink(sidecar_path) != 0) {
                    log_error("[WARNING] Failed to delete sidecar: %s\n", sidecar_path);
                }
            }
            
            destroy_icon(selected);
            delete_count++;
            if (g_pending_delete_canvas->view_mode == VIEW_NAMES) {
                need_layout_update = true;
            }
        } else {
        }
    }
    
    // Update display once
    if (delete_count > 0 && g_pending_delete_canvas) {
        if (need_layout_update) {
            apply_view_layout(g_pending_delete_canvas);
        }
        compute_content_bounds(g_pending_delete_canvas);
        compute_max_scroll(g_pending_delete_canvas);
        redraw_canvas(g_pending_delete_canvas);
        compositor_sync_stacking(get_display());
        XSync(get_display(), False);
    }
    
    // Clear pending state
    g_pending_delete_count = 0;
    g_pending_delete_canvas = NULL;
}

static void cancel_pending_deletes(void) {
    // Delete operation cancelled
    g_pending_delete_count = 0;
    g_pending_delete_canvas = NULL;
}

// Public function to trigger delete action (called from menu or global shortcut)
void trigger_delete_action(void) {
    Canvas *aw = get_active_window();
    Canvas *target_canvas = NULL;
    
    // If no active window, check desktop
    if (!aw || aw->type == DESKTOP) {
        target_canvas = get_desktop_canvas();
    } else if (aw->type == WINDOW) {
        target_canvas = aw;
    }
    
    if (!target_canvas) return;
    
    // CRITICAL: Clear any previous pending deletes
    g_pending_delete_count = 0;
    g_pending_delete_canvas = target_canvas;
    
    // Collect ALL selected icons FROM THIS WINDOW ONLY
    FileIcon **icon_array = get_icon_array();
    int icon_count = get_icon_count();
    for (int i = 0; i < icon_count && g_pending_delete_count < 256; i++) {
        FileIcon *icon = icon_array[i];
        // CRITICAL: Only from target canvas window!
        if (icon && icon->selected && icon->display_window == target_canvas->win) {
            g_pending_delete_icons[g_pending_delete_count++] = icon;
        }
    }
    
    // Check if anything selected
    if (g_pending_delete_count == 0) return;
    
    // Count files and directories separately for proper message formatting
    int file_count = 0;
    int dir_count = 0;
    for (int i = 0; i < g_pending_delete_count; i++) {
        FileIcon *icon = g_pending_delete_icons[i];
        if (icon->type == TYPE_DRAWER) {
            dir_count++;
        } else {
            file_count++;
        }
    }
    
    // Build confirmation message with proper grammar
    char message[256];
    if (file_count > 0 && dir_count > 0) {
        // Both files and directories
        if (file_count == 1 && dir_count == 1) {
            snprintf(message, sizeof(message), "1 file and 1 directory?");
        } else if (file_count == 1) {
            snprintf(message, sizeof(message), "1 file and %d directories?", dir_count);
        } else if (dir_count == 1) {
            snprintf(message, sizeof(message), "%d files and 1 directory?", file_count);
        } else {
            snprintf(message, sizeof(message), "%d files and %d directories?", 
                    file_count, dir_count);
        }
    } else if (file_count > 0) {
        // Only files
        if (file_count == 1) {
            snprintf(message, sizeof(message), "1 file?");
        } else {
            snprintf(message, sizeof(message), "%d files?", file_count);
        }
    } else {
        // Only directories
        if (dir_count == 1) {
            snprintf(message, sizeof(message), "1 directory?");
        } else {
            snprintf(message, sizeof(message), "%d directories?", dir_count);
        }
    }
    
    // Show confirmation dialog - CRITICAL FOR DATA SAFETY
    show_delete_confirmation(message, execute_pending_deletes, cancel_pending_deletes);
}

// Callback for execute command dialog
static void execute_command_ok_callback(const char *command);
static void execute_command_cancel_callback(void);

// Execute command dialog callback - run the command
static void execute_command_ok_callback(const char *command) {
    launch_with_hook(command);
}

static void execute_command_cancel_callback(void) {
    // Nothing to do - dialog will be closed automatically
}

// Public function to trigger execute command dialog
void trigger_execute_action(void) {
    show_execute_dialog(execute_command_ok_callback, execute_command_cancel_callback);
}

// Public function to trigger requester (launch reqasl)
void trigger_requester_action(void) {
    // Launch reqasl in the background
    system("reqasl &");
}

// Public function to trigger rename action (called from menu or global shortcut)
void trigger_rename_action(void) {
    Canvas *active_window = get_active_window();
    FileIcon *selected = NULL;
    
    // Check conditions for rename:
    // 1. Active window with selected icon, OR
    // 2. No active window but desktop has selected icon
    
    if (active_window && active_window->type == WINDOW) {
        // Active window exists - get selected icon from it
        selected = get_selected_icon_from_canvas(active_window);
    } else if (!active_window) {
        // No active window - check desktop for selected icon
        Canvas *desktop = get_desktop_canvas();
        if (desktop) {
            selected = get_selected_icon_from_canvas(desktop);
        }
    }
    // If active_window exists but is not WINDOW type, do nothing
    
    // Show rename dialog only if proper conditions are met
    if (selected && selected->label && selected->path) {
        // Check restrictions - cannot rename System, Home, or iconified windows
        if (strcmp(selected->label, "System") == 0 || strcmp(selected->label, "Home") == 0) {
            return;
        }
        if (selected->type == TYPE_ICONIFIED) {
            return;
        }
        
        // Store the icon globally so the callback can access it
        g_rename_icon = selected;
        show_rename_dialog(selected->label, rename_file_ok_callback, rename_file_cancel_callback, selected);
    } else {
    }
}

// Trigger icon information dialog (Super+I or Icons > Information menu)
void trigger_icon_info_action(void) {
    Canvas *active_window = get_active_window();
    FileIcon *selected = NULL;
    
    // Check conditions for icon info:
    // 1. Active window with selected icon, OR
    // 2. No active window but desktop has selected icon
    
    if (active_window && active_window->type == WINDOW) {
        // Active window exists - get selected icon from it
        selected = get_selected_icon_from_canvas(active_window);
    } else if (!active_window) {
        // No active window - check desktop for selected icon
        Canvas *desktop = get_desktop_canvas();
        if (desktop) {
            selected = get_selected_icon_from_canvas(desktop);
        }
    }
    
    // Show icon info dialog if an icon is selected
    if (selected) {
        show_icon_info_dialog(selected);
    }
}

// Handle quit request (from menu or Super+Shift+Q)
void handle_quit_request(void) {
    // Enter shutdown mode: silence X errors from teardown
    begin_shutdown();
    // Menus/workbench use canvases; keep render/Display alive until after compositor shut down
    // First, stop compositing (uses the Display)
    shutdown_compositor(get_display());
    // Then tear down UI modules
    cleanup_menus();
    cleanup_workbench();
    // Finally close Display and render resources
    cleanup_intuition();
    cleanup_render();
    quit_event_loop();
}

// Handle suspend request (from menu or Super+Shift+S)
void handle_suspend_request(void) {
    system("systemctl suspend &");
}

// Handle restart request (from menu or Super+Shift+R)
void handle_restart_request(void) {
    extern void restart_amiwb(void);
    restart_amiwb();
}

// Trigger select contents action (from menu)
void trigger_select_contents_action(void) {
    Canvas *target_canvas = NULL;
    
    // Determine which canvas to select icons in
    Canvas *active_window = get_active_window();
    
    if (active_window && active_window->type == WINDOW) {
        target_canvas = active_window;
    } else {
        // No active window - use desktop
        target_canvas = get_desktop_canvas();
    }
    
    if (!target_canvas) return;
    
    // Get all icons and check if any are already selected in this canvas
    FileIcon **icon_array = get_icon_array();
    int icon_count = get_icon_count();
    bool has_selected = false;
    
    // First pass: check if any icons are selected
    for (int i = 0; i < icon_count; i++) {
        FileIcon *icon = icon_array[i];
        if (icon && icon->display_window == target_canvas->win && icon->selected) {
            has_selected = true;
            break;
        }
    }
    
    // Second pass: toggle selection
    // If some are selected, deselect all. If none selected, select all.
    bool new_state = !has_selected;
    
    for (int i = 0; i < icon_count; i++) {
        FileIcon *icon = icon_array[i];
        if (icon && icon->display_window == target_canvas->win) {
            // Don't select System or Home icons on desktop
            if (target_canvas->type == DESKTOP && 
                (strcmp(icon->label, "System") == 0 || strcmp(icon->label, "Home") == 0)) {
                continue;
            }
            icon->selected = new_state;
            // Update the icon's picture to show selection state
            icon->current_picture = new_state ? icon->selected_picture : icon->normal_picture;
        }
    }
    
    // Redraw the canvas to show selection changes
    redraw_canvas(target_canvas);
}

// Trigger new drawer action (from menu or Super+N)
void trigger_new_drawer_action(void) {
    Canvas *target_canvas = NULL;
    char target_path[PATH_SIZE];
    
    // Determine where to create the new drawer
    Canvas *active_window = get_active_window();
    
    if (active_window && active_window->type == WINDOW) {
        // Create in active window's directory
        target_canvas = active_window;
        strncpy(target_path, active_window->path, PATH_SIZE - 1);
        target_path[PATH_SIZE - 1] = '\0';
    } else {
        // Create on desktop
        Canvas *desktop = get_desktop_canvas();
        if (desktop) {
            target_canvas = desktop;
            strncpy(target_path, desktop->path, PATH_SIZE - 1);
            target_path[PATH_SIZE - 1] = '\0';
        } else {
            return;
        }
    }
    
    // Find a unique name for the new drawer
    char new_dir_name[NAME_SIZE];
    char full_path[PATH_SIZE];
    int counter = 0;
    
    while (1) {
        if (counter == 0) {
            snprintf(new_dir_name, NAME_SIZE, "Unnamed_dir");
        } else {
            snprintf(new_dir_name, NAME_SIZE, "Unnamed_dir_%d", counter);
        }
        
        int ret = snprintf(full_path, PATH_SIZE, "%s/%s", target_path, new_dir_name);
        if (ret >= PATH_SIZE) {
            // Path too long, stop trying
            log_error("[ERROR] Path too long for new directory: %s/%s", target_path, new_dir_name);
            return;
        }
        
        // Check if directory already exists
        struct stat st;
        if (stat(full_path, &st) != 0) {
            // Directory doesn't exist, we can use this name
            break;
        }
        counter++;
        
        if (counter > 999) {
            return;
        }
    }
    
    // Create the directory
    if (mkdir(full_path, 0755) == 0) {
        
        // Refresh the target canvas to show the new drawer
        if (target_canvas) {
            refresh_canvas_from_directory(target_canvas, target_path);
            icon_cleanup(target_canvas);  // Reorganize icons
            redraw_canvas(target_canvas);
        }
    } else {
    }
}

// Free a menu and all its resources recursively
static void free_menu(Menu *menu) {
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
    
    // Canvas is destroyed elsewhere when menus close, not here
    
    // Free the menu struct itself
    free(menu);
}

