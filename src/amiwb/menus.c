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
#include <strings.h>  // For strcasecmp
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
void trigger_extract_action(void);
void trigger_eject_action(void);
void handle_restart_request(void);

// Global variable to store the icon being renamed
static FileIcon *g_rename_icon = NULL;

// Forward declarations for menu substitution
static void send_menu_selection_to_app(Window app_window, int menu_index, int item_index);

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
        
        // Check for sidecar .info file and rename it too
        char old_info_path[PATH_SIZE + 10];  // +10 for ".info" suffix
        char new_info_path[PATH_SIZE + 10];
        snprintf(old_info_path, sizeof(old_info_path), "%s.info", old_path);
        snprintf(new_info_path, sizeof(new_info_path), "%s.info", new_path);
        
        // If sidecar .info exists, rename it too
        if (access(old_info_path, F_OK) == 0) {
            if (rename(old_info_path, new_info_path) != 0) {
                // Log warning but don't fail the whole operation
                log_error("[WARNING] Could not rename sidecar .info file: %s", strerror(errno));
            }
        }
        
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

    menubar = calloc(1, sizeof(Menu));  // calloc zeros all fields (NULL for pointers, 0 for ints)
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

    // Windows submenu (index 1)
    // Window management and content view controls.
    Menu *win_submenu = calloc(1, sizeof(Menu));  // zeros all fields
    win_submenu->item_count = 7;  // Added Refresh
    win_submenu->items = malloc(win_submenu->item_count * sizeof(char*));
    win_submenu->items[0] = strdup("New Drawer");
    win_submenu->items[1] = strdup("Open Parent");
    win_submenu->items[2] = strdup("Close");
    win_submenu->items[3] = strdup("Select Contents");
    win_submenu->items[4] = strdup("Clean Up");
    win_submenu->items[5] = strdup("Refresh");
    win_submenu->items[6] = strdup("View Modes");
    
    // Initialize shortcuts for Windows menu
    win_submenu->shortcuts = malloc(win_submenu->item_count * sizeof(char*));
    win_submenu->shortcuts[0] = strdup("N");  // New Drawer - Super+N
    win_submenu->shortcuts[1] = strdup("P");  // Open Parent - Super+P
    win_submenu->shortcuts[2] = strdup("Q");  // Close - Super+Q
    win_submenu->shortcuts[3] = strdup("A");  // Select Contents - Super+A
    win_submenu->shortcuts[4] = strdup(";");  // Clean Up - Super+;
    win_submenu->shortcuts[5] = strdup("H");  // Refresh - Super+H
    win_submenu->shortcuts[6] = NULL;  // View Modes - no shortcut yet
    init_menu_enabled(win_submenu);  // Initialize all items as enabled
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
    view_by_sub->selected_item = -1;
    view_by_sub->parent_menu = win_submenu;
    view_by_sub->parent_index = 6;  // Position within Windows submenu (View Modes is at index 6)
    view_by_sub->submenus = NULL;
    view_by_sub->canvas = NULL;
    win_submenu->submenus[6] = view_by_sub;
    menubar->submenus[1] = win_submenu;

    // Icons submenu (index 2)
    // Per-icon actions; entries are placeholders to be wired later.
    Menu *icons_submenu = calloc(1, sizeof(Menu));  // zeros all fields
    icons_submenu->item_count = 7;
    icons_submenu->items = malloc(icons_submenu->item_count * sizeof(char*));
    icons_submenu->items[0] = strdup("Open");
    icons_submenu->items[1] = strdup("Copy");
    icons_submenu->items[2] = strdup("Rename");
    icons_submenu->items[3] = strdup("Extract");      // NEW - Extract archives
    icons_submenu->items[4] = strdup("Eject");        // NEW - Eject removable drives
    icons_submenu->items[5] = strdup("Information");  // Moved down
    icons_submenu->items[6] = strdup("delete");
    
    // Initialize shortcuts array
    icons_submenu->shortcuts = malloc(icons_submenu->item_count * sizeof(char*));
    icons_submenu->shortcuts[0] = strdup("O");  // Open - Super+O
    icons_submenu->shortcuts[1] = strdup("C");  // Copy - Super+C
    icons_submenu->shortcuts[2] = strdup("R");  // Rename - Super+R
    icons_submenu->shortcuts[3] = strdup("X");  // Extract - Super+X
    icons_submenu->shortcuts[4] = strdup("Y");  // Eject - Super+Y
    icons_submenu->shortcuts[5] = strdup("I");  // Information - Super+I
    icons_submenu->shortcuts[6] = strdup("D");  // delete - Super+D
    
    init_menu_enabled(icons_submenu);  // Initialize all items as enabled
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
        
        // Redraw menubar if in logo mode AND no dropdown is open
        // Don't redraw if window list menu is shown (parent_index == -1 indicates window list)
        if (menubar && menubar->canvas && !get_show_menus_state()) {
            if (!active_menu || active_menu->parent_index != -1) {
                redraw_canvas(menubar->canvas);
            }
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

// Close window list menu if it's open
void close_window_list_if_open(void) {
    RenderContext *ctx = get_render_context();
    if (!ctx) return;
    
    // Check if active menu is the window list (parent_index == -1)
    if (active_menu && active_menu->parent_index == -1) {
        if (active_menu->canvas) {
            XSync(ctx->dpy, False);
            if (active_menu->canvas->win != None) {
                clear_press_target_if_matches(active_menu->canvas->win);
                XUnmapWindow(ctx->dpy, active_menu->canvas->win);
                XSync(ctx->dpy, False);
            }
            destroy_canvas(active_menu->canvas);
        }
        // Free the menu structure and its items
        if (active_menu->items) {
            for (int i = 0; i < active_menu->item_count; i++) {
                if (active_menu->items[i]) free(active_menu->items[i]);
            }
            free(active_menu->items);
        }
        if (active_menu->shortcuts) {
            for (int i = 0; i < active_menu->item_count; i++) {
                if (active_menu->shortcuts[i]) free(active_menu->shortcuts[i]);
            }
            free(active_menu->shortcuts);
        }
        if (active_menu->enabled) free(active_menu->enabled);
        free(active_menu);
        active_menu = NULL;
    }
}

// Handle button press on menubar
// Right-click toggles logo vs menus on the menubar.
// Show window list menu at specified position
static void show_window_list_menu(int x, int y) {
    RenderContext *ctx = get_render_context();
    if (!ctx) return;
    
    // Close any existing dropdown
    if (active_menu && active_menu->canvas) {
        XSync(ctx->dpy, False);
        if (active_menu->canvas->win != None) {
            clear_press_target_if_matches(active_menu->canvas->win);
            XUnmapWindow(ctx->dpy, active_menu->canvas->win);
            XSync(ctx->dpy, False);
        }
        destroy_canvas(active_menu->canvas);
        active_menu = NULL;
    }
    
    // Create a temporary menu for the window list
    Menu *window_menu = calloc(1, sizeof(Menu));  // zeros all fields including window_refs
    if (!window_menu) return;
    
    // Get current window list
    Canvas **window_list;
    int window_count = get_window_list(&window_list);
    
    // Build menu items
    window_menu->item_count = window_count + 1;  // +1 for "Desktop" option
    window_menu->items = malloc(window_menu->item_count * sizeof(char*));
    window_menu->shortcuts = malloc(window_menu->item_count * sizeof(char*));
    window_menu->enabled = malloc(window_menu->item_count * sizeof(bool));
    window_menu->window_refs = malloc(window_menu->item_count * sizeof(Canvas*));
    
    // Add Desktop option first
    window_menu->items[0] = strdup("Desktop");
    window_menu->shortcuts[0] = NULL;  // No shortcut for Desktop in window list
    window_menu->enabled[0] = true;
    window_menu->window_refs[0] = NULL;  // Desktop has no window reference
    
    // Add each window
    for (int i = 0; i < window_count; i++) {
        Canvas *c = window_list[i];
        const char *title = c->title_base ? c->title_base : "Untitled";
        
        // Just use the title directly - no truncation, no suffix
        window_menu->items[i + 1] = strdup(title);
        window_menu->shortcuts[i + 1] = NULL;  // No shortcuts for individual windows
        window_menu->enabled[i + 1] = true;
        window_menu->window_refs[i + 1] = c;  // Store Canvas pointer for render-time checking
    }
    
    window_menu->selected_item = -1;
    window_menu->parent_menu = NULL;
    window_menu->parent_index = -1;  // Special value for window list
    window_menu->submenus = NULL;
    window_menu->is_custom = false;
    window_menu->commands = NULL;
    
    // Calculate menu width for window list - use fixed 20 chars max
    // Since we truncate everything to 20 chars at render time, calculate based on that
    XGlyphInfo extents;
    // Measure 20 chars worth of typical text (use "M" as average width char)
    char sample_text[21] = "MMMMMMMMMMMMMMMMMMMM";  // 20 Ms for width calculation
    XftTextExtentsUtf8(ctx->dpy, font, (FcChar8 *)sample_text, 20, &extents);
    
    // Add padding: 10px left, 10px right
    int menu_width = extents.xOff + 20;
    if (menu_width < 80) menu_width = 80;  // Minimum width
    
    int menu_height = window_menu->item_count * MENU_ITEM_HEIGHT + 8;
    
    // Get screen dimensions
    int screen_width = DisplayWidth(ctx->dpy, DefaultScreen(ctx->dpy));
    int screen_height = DisplayHeight(ctx->dpy, DefaultScreen(ctx->dpy));
    
    // Position menu at right edge of screen
    x = screen_width - menu_width;
    
    // Adjust y position if menu would go off screen
    if (y + menu_height > screen_height) {
        y = screen_height - menu_height;
    }
    
    window_menu->canvas = create_canvas(NULL, x, y, menu_width, menu_height, MENU);
    if (window_menu->canvas) {
        window_menu->canvas->bg_color = (XRenderColor){0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
        active_menu = window_menu;
        window_menu->selected_item = -1;
        XMapRaised(ctx->dpy, window_menu->canvas->win);
        redraw_canvas(window_menu->canvas);
        // Ensure window stays on top even if menubar redraws
        XRaiseWindow(ctx->dpy, window_menu->canvas->win);
        XFlush(ctx->dpy);
    }
}

void menu_handle_menubar_press(XButtonEvent *event) {
    RenderContext *ctx = get_render_context();
    if (!ctx) return;
    
    // Handle right-click - always toggle menubar state and close any dropdown
    if (event->button == Button3) {
        // Close window list menu if open
        if (active_menu && active_menu->parent_index == -1) {
            // This is the window list menu
            XSync(ctx->dpy, False);
            if (active_menu->canvas->win != None) {
                clear_press_target_if_matches(active_menu->canvas->win);
                XUnmapWindow(ctx->dpy, active_menu->canvas->win);
                XSync(ctx->dpy, False);
            }
            destroy_canvas(active_menu->canvas);
            
            // Free the temporary window menu
            if (active_menu->items) {
                for (int i = 0; i < active_menu->item_count; i++) {
                    if (active_menu->items[i]) free(active_menu->items[i]);
                }
                free(active_menu->items);
            }
            if (active_menu->shortcuts) {
                for (int i = 0; i < active_menu->item_count; i++) {
                    if (active_menu->shortcuts[i]) free(active_menu->shortcuts[i]);
                }
                free(active_menu->shortcuts);
            }
            if (active_menu->enabled) free(active_menu->enabled);
            if (active_menu->window_refs) free(active_menu->window_refs);
            free(active_menu);
            active_menu = NULL;
        }
        toggle_menubar_state();
    } else if (event->button == Button1) {
        // Check if we're in logo mode and clicking on the right button area
        if (!show_menus) {
            int screen_width = DisplayWidth(ctx->dpy, DefaultScreen(ctx->dpy));
            // Button is drawn at width-28 and is 26 pixels wide (from render.c)
            int button_start = screen_width - 30;  // Give a bit more area for easier clicking
            
            if (event->x >= button_start) {
                // Check if window list is already open
                if (active_menu && active_menu->parent_index == -1) {
                    // Window list is open - close it
                    XSync(ctx->dpy, False);
                    if (active_menu->canvas->win != None) {
                        clear_press_target_if_matches(active_menu->canvas->win);
                        XUnmapWindow(ctx->dpy, active_menu->canvas->win);
                        XSync(ctx->dpy, False);
                    }
                    destroy_canvas(active_menu->canvas);
                    
                    // Free the temporary window menu
                    if (active_menu->items) {
                        for (int i = 0; i < active_menu->item_count; i++) {
                            if (active_menu->items[i]) free(active_menu->items[i]);
                        }
                        free(active_menu->items);
                    }
                    if (active_menu->shortcuts) {
                        for (int i = 0; i < active_menu->item_count; i++) {
                            if (active_menu->shortcuts[i]) free(active_menu->shortcuts[i]);
                        }
                        free(active_menu->shortcuts);
                    }
                    if (active_menu->enabled) free(active_menu->enabled);
                    free(active_menu);
                    active_menu = NULL;
                } else {
                    // Show window list below the button (menubar height)
                    // x position will be calculated to align with screen edge
                    show_window_list_menu(0, MENU_ITEM_HEIGHT);  // x is ignored, calculated inside
                }
            } else {
                // Click outside button area - close window list if open
                if (active_menu && active_menu->parent_index == -1) {
                    XSync(ctx->dpy, False);
                    if (active_menu->canvas->win != None) {
                        clear_press_target_if_matches(active_menu->canvas->win);
                        XUnmapWindow(ctx->dpy, active_menu->canvas->win);
                        XSync(ctx->dpy, False);
                    }
                    destroy_canvas(active_menu->canvas);
                    
                    // Free the temporary window menu
                    if (active_menu->items) {
                        for (int i = 0; i < active_menu->item_count; i++) {
                            if (active_menu->items[i]) free(active_menu->items[i]);
                        }
                        free(active_menu->items);
                    }
                    if (active_menu->shortcuts) {
                        for (int i = 0; i < active_menu->item_count; i++) {
                            if (active_menu->shortcuts[i]) free(active_menu->shortcuts[i]);
                        }
                        free(active_menu->shortcuts);
                    }
                    if (active_menu->enabled) free(active_menu->enabled);
                    free(active_menu);
                    active_menu = NULL;
                }
            }
        }
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
            
            // Update enabled states for View Modes submenu
            if (nested_menu->parent_menu && nested_menu->parent_menu->parent_index == 1 && 
                nested_menu->parent_index == 6) { // View Modes submenu
                Canvas *active = get_active_window();
                bool desktop_focused = (!active || active->type == DESKTOP);
                
                // Enable/disable items based on context
                if (nested_menu->enabled) {
                    nested_menu->enabled[0] = true;  // Icons - always enabled
                    nested_menu->enabled[1] = !desktop_focused;  // Names - disabled for desktop
                    nested_menu->enabled[2] = true;  // Hidden - always enabled
                    nested_menu->enabled[3] = true;  // Spatial - always enabled
                }
            }
            
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
    // Only do this for system menus, not app menus
    if (!app_menu_active && menu == menubar && index == 2) {  // Icons menu
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
    
    // Update enabled states for Windows menu based on active window
    // Only do this for system menus, not app menus
    if (!app_menu_active && menu == menubar && index == 1) {  // Windows menu
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
            active_menu->enabled[5] = is_workbench_window || desktop_focused;  // Refresh - workbench or desktop
            active_menu->enabled[6] = is_workbench_window || desktop_focused;  // View Modes - workbench or desktop
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
    
    // Handle window list menu (parent_index == -1)
    if (menu->parent_index == -1) {
        if (strcmp(item, "Desktop") == 0) {
            // Iconify all windows to show desktop
            iconify_all_windows();
        } else {
            // Find the window by title and activate it
            Canvas **window_list;
            int window_count = get_window_list(&window_list);
            
            for (int i = 0; i < window_count; i++) {
                Canvas *c = window_list[i];
                const char *title = c->title_base ? c->title_base : "Untitled";
                
                // Direct comparison - no truncation
                if (strcmp(item, title) == 0) {
                    // Find the index in canvas_array
                    for (int j = 0; j < canvas_count; j++) {
                        if (canvas_array[j] == c) {
                            activate_window_by_index(j);
                            break;
                        }
                    }
                    break;
                }
            }
        }
        
        // Close the menu
        if (active_menu && active_menu->canvas) {
            RenderContext *ctx = get_render_context();
            if (ctx) {
                XSync(ctx->dpy, False);
                if (active_menu->canvas->win != None) {
                    clear_press_target_if_matches(active_menu->canvas->win);
                    XUnmapWindow(ctx->dpy, active_menu->canvas->win);
                    XSync(ctx->dpy, False);
                }
                destroy_canvas(active_menu->canvas);
                
                // Free the temporary window menu
                if (active_menu->items) {
                    for (int i = 0; i < active_menu->item_count; i++) {
                        if (active_menu->items[i]) free(active_menu->items[i]);
                    }
                    free(active_menu->items);
                }
                if (active_menu->shortcuts) {
                    for (int i = 0; i < active_menu->item_count; i++) {
                        if (active_menu->shortcuts[i]) free(active_menu->shortcuts[i]);
                    }
                    free(active_menu->shortcuts);
                }
                if (active_menu->enabled) free(active_menu->enabled);
                free(active_menu);
                
                active_menu = NULL;
            }
        }
        return;
    }
    
    // Handle app menu selections
    if (app_menu_active && current_app_window != None) {
        // Send selection to app via client message
        send_menu_selection_to_app(current_app_window, menu->parent_index, item_index);
        
        // Close menus after selection
        if (get_show_menus_state()) {
            toggle_menubar_state();
        }
        return;
    }
    
    // If this is a nested submenu under Windows, handle here
    if (menu->parent_menu && menu->parent_menu->parent_menu == menubar && 
        menu->parent_menu->parent_index == 1) {
        // Determine which child: by parent_index in Windows submenu
        if (menu->parent_index == 6) { // View Modes (now at index 6)
            Canvas *target = get_active_window();
            if (!target) {
                // No active window - use desktop
                target = get_desktop_canvas();
            }
            if (target) {
                if (strcmp(item, "Icons") == 0) {
                    set_canvas_view_mode(target, VIEW_ICONS);
                } else if (strcmp(item, "Names") == 0) {
                    set_canvas_view_mode(target, VIEW_NAMES);
                } else if (strcmp(item, "Hidden") == 0) {
                    // Toggle global hidden files state
                    bool new_state = !get_global_show_hidden_state();
                    set_global_show_hidden_state(new_state);
                    
                    // Apply to current target window
                    target->show_hidden = new_state;
                    
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
                } else if (strcmp(item, "Spatial") == 0) {
                    // Toggle spatial mode
                    set_spatial_mode(!get_spatial_mode());
                }
            }
        } else if (menu->parent_index == 6) { // Cycle
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
            } else if (strcmp(item, "Refresh") == 0) {
                trigger_refresh_action();
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
            } else if (strcmp(item, "Extract") == 0) {
                trigger_extract_action();
            } else if (strcmp(item, "Eject") == 0) {
                trigger_eject_action();
            } else if (strcmp(item, "Information") == 0) {
                trigger_icon_info_action();
            } else if (strcmp(item, "delete") == 0) {
                trigger_delete_action();
            }
            break;

        case 3:  // Tools
            if (strcmp(item, "Text Editor") == 0) {
                launch_with_hook("editpad");
            } else if (strcmp(item, "XCalc") == 0) {
                launch_with_hook("xcalc");
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

// Public function to trigger refresh action (called from menu or global shortcut)
void trigger_refresh_action(void) {
    Canvas *active_window = get_active_window();
    Canvas *target = active_window;
    
    // If no active window, use desktop
    if (!target || target->type != WINDOW) {
        target = get_desktop_canvas();
    }
    
    if (target) {
        // Apply current global show_hidden state to the window
        target->show_hidden = get_global_show_hidden_state();
        
        // Refresh the directory contents
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
        
        // icon_cleanup is now called inside refresh_canvas_from_directory
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
        snprintf(parent_path, sizeof(parent_path), "%s", active_window->path);
        
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
        
        // Non-spatial mode: reuse current window
        if (!get_spatial_mode()) {
            // Update the current window's path to parent
            if (active_window->path) free(active_window->path);
            active_window->path = strdup(parent_path);
            
            // Update window title
            const char *dir_name = strrchr(parent_path, '/');
            if (dir_name && *(dir_name + 1)) dir_name++; 
            else dir_name = parent_path;
            if (active_window->title_base) free(active_window->title_base);
            active_window->title_base = strdup(dir_name);
            
            // Refresh with parent directory
            refresh_canvas_from_directory(active_window, parent_path);
            
            // Reset scroll (icon_cleanup now called inside refresh_canvas_from_directory)
            active_window->scroll_x = 0;
            active_window->scroll_y = 0;
            redraw_canvas(active_window);
        } else {
            // Spatial mode: check if window for parent path already exists
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
        snprintf(label_copy, sizeof(label_copy), "%s", selected->label);
        
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
            // Copy started successfully
        } else {
            log_error("[ERROR] Copy failed for: %s", saved_path);  // Use saved path
        }
        
        free(dir_path);
    }
}

// Public function to trigger extract action (called from menu or global shortcut)
void trigger_extract_action(void) {
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
        // Check if the selected file is an archive
        const char *ext = strrchr(selected->path, '.');
        if (!ext) return;
        ext++; // Skip the dot
        
        // Check for supported archive formats
        const char *archive_exts[] = {
            "lha", "lzh", "zip", "tar", "gz", "tgz", "bz2", "tbz",
            "xz", "txz", "rar", "7z", NULL
        };
        
        bool is_archive = false;
        for (int i = 0; archive_exts[i]; i++) {
            if (strcasecmp(ext, archive_exts[i]) == 0) {
                is_archive = true;
                break;
            }
        }
        
        // Also check for compound extensions
        const char *name = strrchr(selected->path, '/');
        name = name ? name + 1 : selected->path;
        if (strstr(name, ".tar.gz") || strstr(name, ".tar.bz2") || strstr(name, ".tar.xz")) {
            is_archive = true;
        }
        
        if (is_archive) {
            // Call the extraction function, passing the canvas so we know where to create the icon
            extract_file_at_path(selected->path, target_canvas);
        }
    }
}

void trigger_eject_action(void) {
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
    
    // Only eject if it's a TYPE_DEVICE icon
    if (selected && selected->type == TYPE_DEVICE) {
        // Import eject_drive function from diskdrives.h
        extern void eject_drive(FileIcon *icon);
        eject_drive(selected);
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
    launch_with_hook("reqasl");
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
            // icon_cleanup now called inside refresh_canvas_from_directory
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

// Menu substitution: switch menubar to app-specific menus
void switch_to_app_menu(const char *app_name, char **menu_items, Menu **submenus, int item_count, Window app_window) {
    if (!menubar || !app_name || !menu_items || !submenus || item_count <= 0) {
        log_error("[WARNING] switch_to_app_menu called with invalid parameters");
        return;
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
    
    // If currently showing menus, update menubar
    if (show_menus) {
        menubar->items = full_menu_items;
        menubar->submenus = full_submenus;
        menubar->item_count = full_menu_item_count;
    }
    
    // Mark app menu as active
    app_menu_active = true;
    current_app_window = app_window;
    
    // Switched to app menus
    
    // Redraw menubar with new content
    redraw_canvas(menubar->canvas);
}

// Menu substitution: restore system menus
void restore_system_menu(void) {
    if (!app_menu_active || !system_menu_items) {
        return;  // Already showing system menus or not initialized
    }
    
    // Restore logo
    free(logo_items[0]);
    logo_items[0] = strdup(system_logo_item);
    
    // Restore full menu arrays
    full_menu_items = system_menu_items;
    full_submenus = system_submenus;
    full_menu_item_count = system_menu_item_count;
    
    // If currently showing menus, update menubar
    if (show_menus) {
        menubar->items = full_menu_items;
        menubar->submenus = full_submenus;
        menubar->item_count = full_menu_item_count;
    }
    
    // Mark system menu as active
    app_menu_active = false;
    current_app_window = None;
    
    // Restored system menus
    
    // Redraw menubar
    redraw_canvas(menubar->canvas);
}

// Check if app menus are currently active
bool is_app_menu_active(void) {
    return app_menu_active;
}

// Get the window that owns current app menus
Window get_app_menu_window(void) {
    return current_app_window;
}

// TEST FUNCTION: Create hardcoded EditPad menus for testing menu substitution
static Menu **create_test_editpad_menus(void) {
    // Create top-level menu array (File, Edit, Search, View)
    Menu **editpad_menus = calloc(4, sizeof(Menu*));
    
    // File menu
    Menu *file_menu = calloc(1, sizeof(Menu));
    file_menu->item_count = 8;  // Added test item
    file_menu->items = calloc(file_menu->item_count, sizeof(char*));
    file_menu->items[0] = strdup("New");
    file_menu->items[1] = strdup("Open...");
    file_menu->items[2] = strdup("Save");
    file_menu->items[3] = strdup("Save As...");
    file_menu->items[4] = strdup("----------");
    file_menu->items[5] = strdup("Quit");
    file_menu->items[6] = strdup("----------");
    file_menu->items[7] = strdup("TEST: System Menu");
    
    file_menu->shortcuts = calloc(file_menu->item_count, sizeof(char*));
    file_menu->shortcuts[0] = strdup("N");      // Super+N
    file_menu->shortcuts[1] = strdup("O");      // Super+O
    file_menu->shortcuts[2] = strdup("S");      // Super+S
    file_menu->shortcuts[3] = strdup("^S");     // Super+Shift+S
    file_menu->shortcuts[4] = NULL;
    file_menu->shortcuts[5] = strdup("Q");      // Super+Q
    file_menu->shortcuts[6] = NULL;
    file_menu->shortcuts[7] = NULL;
    
    file_menu->enabled = calloc(file_menu->item_count, sizeof(bool));
    for (int i = 0; i < file_menu->item_count; i++) {
        file_menu->enabled[i] = true;
    }
    file_menu->selected_item = -1;
    file_menu->parent_menu = menubar;
    file_menu->parent_index = 0;
    file_menu->submenus = NULL;
    file_menu->canvas = NULL;
    editpad_menus[0] = file_menu;
    
    // Edit menu
    Menu *edit_menu = calloc(1, sizeof(Menu));
    edit_menu->item_count = 7;
    edit_menu->items = calloc(edit_menu->item_count, sizeof(char*));
    edit_menu->items[0] = strdup("Cut");
    edit_menu->items[1] = strdup("Copy");
    edit_menu->items[2] = strdup("Paste");
    edit_menu->items[3] = strdup("----------");
    edit_menu->items[4] = strdup("Select All");
    edit_menu->items[5] = strdup("----------");
    edit_menu->items[6] = strdup("Undo");
    
    edit_menu->shortcuts = calloc(edit_menu->item_count, sizeof(char*));
    edit_menu->shortcuts[0] = strdup("X");      // Super+X
    edit_menu->shortcuts[1] = strdup("C");      // Super+C
    edit_menu->shortcuts[2] = strdup("V");      // Super+V
    edit_menu->shortcuts[3] = NULL;
    edit_menu->shortcuts[4] = strdup("A");      // Super+A
    edit_menu->shortcuts[5] = NULL;
    edit_menu->shortcuts[6] = strdup("Z");      // Super+Z
    
    edit_menu->enabled = calloc(edit_menu->item_count, sizeof(bool));
    for (int i = 0; i < edit_menu->item_count; i++) {
        edit_menu->enabled[i] = true;
    }
    edit_menu->selected_item = -1;
    edit_menu->parent_menu = menubar;
    edit_menu->parent_index = 1;
    edit_menu->submenus = NULL;
    edit_menu->canvas = NULL;
    editpad_menus[1] = edit_menu;
    
    // Search menu
    Menu *search_menu = calloc(1, sizeof(Menu));
    search_menu->item_count = 4;
    search_menu->items = calloc(search_menu->item_count, sizeof(char*));
    search_menu->items[0] = strdup("Find...");
    search_menu->items[1] = strdup("Find Next");
    search_menu->items[2] = strdup("Replace...");
    search_menu->items[3] = strdup("Go to Line...");
    
    search_menu->shortcuts = calloc(search_menu->item_count, sizeof(char*));
    search_menu->shortcuts[0] = strdup("F");      // Super+F
    search_menu->shortcuts[1] = strdup("G");      // Super+G
    search_menu->shortcuts[2] = strdup("R");      // Super+R
    search_menu->shortcuts[3] = strdup("L");      // Super+L
    
    search_menu->enabled = calloc(search_menu->item_count, sizeof(bool));
    for (int i = 0; i < search_menu->item_count; i++) {
        search_menu->enabled[i] = false;  // Disabled for now (not implemented)
    }
    search_menu->selected_item = -1;
    search_menu->parent_menu = menubar;
    search_menu->parent_index = 2;
    search_menu->submenus = NULL;
    search_menu->canvas = NULL;
    editpad_menus[2] = search_menu;
    
    // View menu
    Menu *view_menu = calloc(1, sizeof(Menu));
    view_menu->item_count = 3;
    view_menu->items = calloc(view_menu->item_count, sizeof(char*));
    view_menu->items[0] = strdup("Word Wrap");
    view_menu->items[1] = strdup("----------");
    view_menu->items[2] = strdup("Syntax Highlighting");
    
    view_menu->shortcuts = calloc(view_menu->item_count, sizeof(char*));
    view_menu->shortcuts[0] = strdup("W");      // Super+W
    view_menu->shortcuts[1] = NULL;
    view_menu->shortcuts[2] = strdup("H");      // Super+H
    
    view_menu->enabled = calloc(view_menu->item_count, sizeof(bool));
    view_menu->enabled[0] = false;  // Word wrap not implemented
    view_menu->enabled[1] = true;   // Separator
    view_menu->enabled[2] = false;  // Syntax highlighting not implemented
    view_menu->selected_item = -1;
    view_menu->parent_menu = menubar;
    view_menu->parent_index = 3;
    view_menu->submenus = NULL;
    view_menu->canvas = NULL;
    editpad_menus[3] = view_menu;
    
    return editpad_menus;
}

// TEST FUNCTION: Simulate EditPad getting focus
void test_editpad_menu_substitution(Window test_window) {
    // TEST: Switching to EditPad menus
    
    // Create EditPad menu items
    char **editpad_items = calloc(4, sizeof(char*));
    editpad_items[0] = strdup("File");
    editpad_items[1] = strdup("Edit");
    editpad_items[2] = strdup("Search");
    editpad_items[3] = strdup("View");
    
    // Create EditPad submenus
    Menu **editpad_submenus = create_test_editpad_menus();
    
    // Switch to EditPad menus
    switch_to_app_menu("EditPad", editpad_items, editpad_submenus, 4, test_window);
}

// TEST FUNCTION: Simulate EditPad losing focus
void test_restore_system_menus(void) {
    // TEST: Restoring system menus
    restore_system_menu();
}

// Parse menu data string and create Menu structures
// Format: "File:New,Open,Save|Edit:Cut,Copy,Paste|..."
static void parse_and_switch_app_menus(const char *app_name, const char *menu_data, Window app_window) {
    if (!menu_data || !app_name) {
        log_error("[ERROR] parse_and_switch_app_menus: NULL parameters");
        return;
    }
    
    // Parsing menu data
    
    // Count top-level menus (separated by |)
    int menu_count = 1;
    for (const char *p = menu_data; *p; p++) {
        if (*p == '|') menu_count++;
    }
    
    // Allocate top-level arrays
    char **menu_items = calloc(menu_count, sizeof(char*));
    Menu **submenus = calloc(menu_count, sizeof(Menu*));
    
    // Parse the menu data - use strtok_r to avoid issues
    char *data_copy = strdup(menu_data);
    char *saveptr;
    char *menu_str = strtok_r(data_copy, "|", &saveptr);
    int menu_index = 0;
    
    while (menu_str && menu_index < menu_count) {
        // Split "File:New,Open,Save" into "File" and "New,Open,Save"
        char *colon = strchr(menu_str, ':');
        if (!colon) continue;
        
        *colon = '\0';
        char *menu_name = menu_str;
        char *items_str = colon + 1;
        
        // Store menu name
        menu_items[menu_index] = strdup(menu_name);
        
        // Count items in this menu
        int item_count = 1;
        for (const char *p = items_str; *p; p++) {
            if (*p == ',') item_count++;
        }
        
        // Create submenu
        Menu *submenu = calloc(1, sizeof(Menu));
        submenu->item_count = item_count;
        submenu->items = calloc(item_count, sizeof(char*));
        submenu->shortcuts = calloc(item_count, sizeof(char*));
        submenu->enabled = calloc(item_count, sizeof(bool));
        
        // Parse items - use strtok_r for thread safety and to avoid nested strtok issues
        char *saveptr2;
        char *items_copy = strdup(items_str);  // Make a copy for strtok_r
        char *item = strtok_r(items_copy, ",", &saveptr2);
        int item_index = 0;
        while (item && item_index < item_count) {
            submenu->items[item_index] = strdup(item);
            submenu->enabled[item_index] = true;
            submenu->shortcuts[item_index] = NULL;  // Initialize to NULL first
            
            // Add shortcuts for known items (EditPad specific for now)
            if (strcmp(item, "New") == 0) submenu->shortcuts[item_index] = strdup("N");
            else if (strcmp(item, "Open") == 0) submenu->shortcuts[item_index] = strdup("O");
            else if (strcmp(item, "Save") == 0) submenu->shortcuts[item_index] = strdup("S");
            else if (strcmp(item, "Save As") == 0) submenu->shortcuts[item_index] = strdup("^S");
            else if (strcmp(item, "Quit") == 0) submenu->shortcuts[item_index] = strdup("Q");
            else if (strcmp(item, "Cut") == 0) submenu->shortcuts[item_index] = strdup("X");
            else if (strcmp(item, "Copy") == 0) submenu->shortcuts[item_index] = strdup("C");
            else if (strcmp(item, "Paste") == 0) submenu->shortcuts[item_index] = strdup("V");
            else if (strcmp(item, "Select All") == 0) submenu->shortcuts[item_index] = strdup("A");
            else if (strcmp(item, "Undo") == 0) submenu->shortcuts[item_index] = strdup("Z");
            else if (strcmp(item, "Find") == 0) submenu->shortcuts[item_index] = strdup("F");
            else if (strcmp(item, "Goto Line") == 0) submenu->shortcuts[item_index] = strdup("L");
            
            item = strtok_r(NULL, ",", &saveptr2);
            item_index++;
        }
        free(items_copy);
        
        submenu->selected_item = -1;
        submenu->parent_menu = menubar;
        submenu->parent_index = menu_index;
        submenu->submenus = NULL;
        submenu->canvas = NULL;
        
        submenus[menu_index] = submenu;
        
        menu_str = strtok_r(NULL, "|", &saveptr);
        menu_index++;
    }
    
    free(data_copy);
    
    // Switch to the parsed menus
    switch_to_app_menu(app_name, menu_items, submenus, menu_count, app_window);
}

// Update menu item states from app window property
static void update_app_menu_states(Window app_window) {
    if (!app_window || !app_menu_active || !full_submenus) return;
    
    Display *dpy = get_display();
    if (!dpy) return;
    
    // Get the menu states property
    Atom states_atom = XInternAtom(dpy, "_AMIWB_MENU_STATES", False);
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *states_data = NULL;
    
    if (XGetWindowProperty(dpy, app_window, states_atom, 0, 65536, False,
                          AnyPropertyType, &actual_type, &actual_format,
                          &nitems, &bytes_after, &states_data) == Success && states_data) {
        
        // Parse the states data
        // Format: "menu_index,item_index,state;menu_index,item_index,state;..."
        // Example: "1,4,0;1,5,1" means Edit menu(1), Undo(4) disabled(0), Redo(5) enabled(1)
        char *data_copy = strdup((char*)states_data);
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
        if (active_menu && active_menu->canvas) {
            redraw_canvas(active_menu->canvas);
        }
    }
}

// Send menu selection back to app via client message
static void send_menu_selection_to_app(Window app_window, int menu_index, int item_index) {
    Display *dpy = get_display();
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
    
    Display *dpy = get_display();
    if (!dpy) return;
    
    // Define atoms for app properties
    Atom type_atom = XInternAtom(dpy, "_AMIWB_APP_TYPE", False);
    Atom menu_atom = XInternAtom(dpy, "_AMIWB_MENU_DATA", False);
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *app_type = NULL;
    unsigned char *menu_data = NULL;
    
    // Check if window has _AMIWB_APP_TYPE property
    if (XGetWindowProperty(dpy, win, type_atom, 0, 1024, False,
                          AnyPropertyType, &actual_type, &actual_format,
                          &nitems, &bytes_after, &app_type) == Success && app_type) {
        
        // Found toolkit app
        
        // It's a toolkit app! Get menu data
        if (XGetWindowProperty(dpy, win, menu_atom, 0, 65536, False,
                              AnyPropertyType, &actual_type, &actual_format,
                              &nitems, &bytes_after, &menu_data) == Success && menu_data) {
            
            // Found menu data
            
            // Parse menu data and switch menus
            parse_and_switch_app_menus((char*)app_type, (char*)menu_data, win);
            
            XFree(menu_data);
            
            // Also update menu states if available
            update_app_menu_states(win);
        }
        XFree(app_type);
    } else {
        // Not a toolkit app - restore system menus
        // Not a toolkit app, restoring system menus
        restore_system_menu();
    }
}

