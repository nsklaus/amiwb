// File: menus.c
#define _POSIX_C_SOURCE 200809L
#include "menus.h"
#include "config.h"
#include "intuition.h"
#include "workbench.h"
#include "render.h"
#include "compositor.h"
#include "dialogs.h"
#include "events.h"
#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>
#include <fontconfig/fontconfig.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

#define RESOURCE_DIR_SYSTEM "/usr/local/share/amiwb"  
#define RESOURCE_DIR_USER ".config/amiwb"  

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
    char user_path[1024];
    snprintf(user_path, sizeof(user_path), "%s/%s/%s", home, RESOURCE_DIR_USER, rel_path);
    if (access(user_path, F_OK) == 0) return strdup(user_path);
    char sys_path[1024];
    snprintf(sys_path, sizeof(sys_path), "%s/%s", RESOURCE_DIR_SYSTEM, rel_path);
    return strdup(sys_path);
}

// Static callback functions for rename dialog (avoid nested function trampolines)
static void rename_file_ok_callback(const char *new_name) {
    // Use the global icon that was set when dialog was shown
    FileIcon *icon = g_rename_icon;
    
    printf("DEBUG: rename_file_ok_callback - icon=%p, path='%s', label='%s'\n", 
           (void*)icon, icon ? icon->path : "NULL", icon ? icon->label : "NULL");
    
    if (!icon || !new_name || strlen(new_name) == 0) {
        printf("Rename failed: invalid parameters\n");
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
        printf("Rename failed: icon no longer valid\n");
        return;
    }
    
    // Construct paths
    char old_path[PATH_MAX];
    char new_path[PATH_MAX]; 
    char *dir_path = strdup(icon->path);
    char *filename = strrchr(dir_path, '/');
    if (filename) *filename = '\0';  // Remove filename, keep directory
    
    snprintf(old_path, sizeof(old_path), "%s", icon->path);
    snprintf(new_path, sizeof(new_path), "%s/%s", dir_path, new_name);
    
    // Attempt rename with safety checks
    if (access(new_path, F_OK) == 0) {
        printf("Rename failed: file '%s' already exists\n", new_name);
    } else if (rename(old_path, new_path) == 0) {
        // Success: update icon
        free(icon->label);
        icon->label = strdup(new_name);
        free(icon->path);
        icon->path = strdup(new_path);
        
        // Refresh display - reload directory to show renamed file in correct position
        Canvas *canvas = find_canvas(icon->display_window);
        if (canvas && canvas->path) {
            refresh_canvas_from_directory(canvas, canvas->path);
            icon_cleanup(canvas);  // Use icon_cleanup to properly re-grid icons after refresh
            compute_max_scroll(canvas);
            redraw_canvas(canvas);
        }
        
        printf("Successfully renamed '%s' to '%s'\n", old_path, new_name);
    } else {
        printf("Rename failed: %s\n", strerror(errno));
    }
    
    free(dir_path);
    g_rename_icon = NULL;  // Clear the global icon reference
}

static void rename_file_cancel_callback(void) {
    printf("Rename cancelled\n");
    g_rename_icon = NULL;  // Clear the global icon reference
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
        fprintf(stderr, "Failed to load font %s\n", font_path);
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
    menubar->selected_item = -1;
    menubar->parent_menu = NULL;
    menubar->submenus = malloc(menubar->item_count * sizeof(Menu*));
    memset(menubar->submenus, 0, menubar->item_count * sizeof(Menu*));

    // Workbench submenu (index 0)
    // Basic actions for the environment and global app state.
    Menu *wb_submenu = malloc(sizeof(Menu));
    wb_submenu->item_count = 5;
    wb_submenu->items = malloc(wb_submenu->item_count * sizeof(char*));
    wb_submenu->items[0] = strdup("Execute");
    wb_submenu->items[1] = strdup("Settings");
    wb_submenu->items[2] = strdup("About");
    wb_submenu->items[3] = strdup("Suspend");
    wb_submenu->items[4] = strdup("Quit AmiWB");
    wb_submenu->selected_item = -1;
    wb_submenu->parent_menu = menubar;
    wb_submenu->parent_index = 0;
    wb_submenu->submenus = NULL;
    wb_submenu->canvas = NULL;
    menubar->submenus[0] = wb_submenu;

    // Window submenu (index 1)
    // Window management and content view controls.
    Menu *win_submenu = malloc(sizeof(Menu));
    win_submenu->item_count = 7;
    win_submenu->items = malloc(win_submenu->item_count * sizeof(char*));
    win_submenu->items[0] = strdup("New Drawer");
    win_submenu->items[1] = strdup("Open Parent");
    win_submenu->items[2] = strdup("Close");
    win_submenu->items[3] = strdup("Select Contents");
    win_submenu->items[4] = strdup("Clean Up");
    win_submenu->items[5] = strdup("Show Hidden");
    win_submenu->items[6] = strdup("View By ..");
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
    view_by_sub->selected_item = -1;
    view_by_sub->parent_menu = win_submenu;
    view_by_sub->parent_index = 6;
    view_by_sub->submenus = NULL;
    view_by_sub->canvas = NULL;
    win_submenu->submenus[6] = view_by_sub;
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
    icons_submenu->selected_item = -1;
    icons_submenu->parent_menu = menubar;
    icons_submenu->parent_index = 2;
    icons_submenu->submenus = NULL;
    icons_submenu->canvas = NULL;
    menubar->submenus[2] = icons_submenu;

    // Tools submenu (index 3)
    // Quick launchers for external apps; editable in config later.
    Menu *tools_submenu = malloc(sizeof(Menu));
    tools_submenu->item_count = 6;
    tools_submenu->items = malloc(tools_submenu->item_count * sizeof(char*));
    tools_submenu->items[0] = strdup("XCalc");
    tools_submenu->items[1] = strdup("PavuControl");
    tools_submenu->items[2] = strdup("Brave Browser");
    tools_submenu->items[3] = strdup("Sublime Text");
    tools_submenu->items[4] = strdup("Shell");
    tools_submenu->items[5] = strdup("Debug Console");
    tools_submenu->selected_item = -1;
    tools_submenu->parent_menu = menubar;
    tools_submenu->parent_index = 3;
    tools_submenu->submenus = NULL;
    tools_submenu->canvas = NULL;
    menubar->submenus[3] = tools_submenu;

    // Setup mode-specific arrays
    // Menubar can display a single logo item or full menu items.
    logo_items = malloc(logo_item_count * sizeof(char*));
    logo_items[0] = strdup("AmiWB");

    full_menu_item_count = menubar->item_count;
    full_menu_items = menubar->items;
    full_submenus = menubar->submenus;

    // Initial default mode: logo
    // Start minimal; switch to full menu on user toggle.
    menubar->items = logo_items;
    menubar->item_count = logo_item_count;
    menubar->submenus = NULL;

    redraw_canvas(menubar->canvas);
}

void cleanup_menus(void) {
    RenderContext *ctx = get_render_context();
    if (!ctx) return;

    // Release font/color and destroy any menu canvases.
    if (font) XftFontClose(ctx->dpy, font);
    if (text_color.pixel) XftColorFree(ctx->dpy, DefaultVisual(ctx->dpy, DefaultScreen(ctx->dpy)), DefaultColormap(ctx->dpy, DefaultScreen(ctx->dpy)), &text_color);
    if (active_menu) {
        if (active_menu->canvas) destroy_canvas(active_menu->canvas);
        // Removed: free(active_menu);  // Avoid double free; submenu structs are freed in the loop below
        active_menu = NULL;
    }
    if (menubar) {
        if (menubar->canvas) destroy_canvas(menubar->canvas);
        // Free full menu items and submenus
        for (int i = 0; i < full_menu_item_count; i++) {
            free(full_menu_items[i]);
            if (full_submenus[i]) {
                for (int j = 0; j < full_submenus[i]->item_count; j++) free(full_submenus[i]->items[j]);
                free(full_submenus[i]->items);
                free(full_submenus[i]);
            }
        }
        free(full_menu_items);
        free(full_submenus);
        // Free logo items
        for (int i = 0; i < logo_item_count; i++) free(logo_items[i]);
        free(logo_items);
        free(menubar);
    }
    // Last trace to confirm teardown order during shutdown.
    printf("Called cleanup_menus()\n");
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
            XUnmapWindow(ctx->dpy, nested_menu->canvas->win);
            XSync(ctx->dpy, False);  // Wait for unmap
        }
        destroy_canvas(nested_menu->canvas);
        nested_menu = NULL;
    }
}

// Handle clicks inside a dropdown or nested submenu.
// Dispatches selection and closes menus afterwards.
void menu_handle_button_press(XButtonEvent *event) {
    RenderContext *ctx = get_render_context();
    if (!ctx) return;

    Menu *target_menu = NULL;
    if (active_menu && event->window == active_menu->canvas->win) target_menu = active_menu;
    else if (nested_menu && event->window == nested_menu->canvas->win) target_menu = nested_menu;
    else return;

    int item = event->y / MENU_ITEM_HEIGHT;
    if (item >= 0 && item < target_menu->item_count) {
        handle_menu_selection(target_menu, item);
    }
    // Close dropped-down menus after selection with safe validation
    if (nested_menu && nested_menu->canvas) {
        XSync(ctx->dpy, False);  // Complete pending operations
        if (nested_menu->canvas->win != None) {
            XUnmapWindow(ctx->dpy, nested_menu->canvas->win);
            XSync(ctx->dpy, False);  // Wait for unmap
        }
        destroy_canvas(nested_menu->canvas);
        nested_menu = NULL;
    }
    if (active_menu && active_menu->canvas) {  
        XSync(ctx->dpy, False);  // Complete pending operations
        if (active_menu->canvas->win != None) {
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

// Button release inside menus is unused for now.
void menu_handle_button_release(XButtonEvent *event) {

    //XUngrabPointer(get_display(), CurrentTime);
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
            nested_menu->item_count * MENU_ITEM_HEIGHT, MENU);
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
        active_menu->selected_item = event->y / MENU_ITEM_HEIGHT;
        if (active_menu->selected_item < 0 || 
                active_menu->selected_item >= active_menu->item_count) {
            active_menu->selected_item = -1;
        }
        if (active_menu->selected_item != prev_selected) {
            redraw_canvas(active_menu->canvas);
            maybe_open_nested_for_selection();
        }
        return;
    }

    if (nested_menu && event->window == nested_menu->canvas->win) {
        int prev_selected = nested_menu->selected_item;
        nested_menu->selected_item = event->y / MENU_ITEM_HEIGHT;
        if (nested_menu->selected_item < 0 || 
                nested_menu->selected_item >= nested_menu->item_count) {
            nested_menu->selected_item = -1;
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
    printf("menu bar registered key press event\n");
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
            XUnmapWindow(ctx->dpy, nested_menu->canvas->win);
            XSync(ctx->dpy, False);  // Wait for unmap
        }
        destroy_canvas(nested_menu->canvas);
        nested_menu = NULL;
    }
    active_menu = menu->submenus[index];
    int submenu_width = get_submenu_width(active_menu);
    active_menu->canvas = create_canvas(NULL, x, y, submenu_width, 
        active_menu->item_count * MENU_ITEM_HEIGHT, MENU);
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
            Canvas *aw = get_active_window();
            if (aw) {
                if (strcmp(item, "Yes") == 0) aw->show_hidden = true;
                else if (strcmp(item, "No") == 0) aw->show_hidden = false;
                // Refresh directory view to apply hidden filter
                refresh_canvas_from_directory(aw, aw->path);
                // Ensure layout matches current view mode (fix initial list spacing)
                apply_view_layout(aw);
                compute_max_scroll(aw);
                redraw_canvas(aw);
            }
        } else if (menu->parent_index == 6) { // View By ..
            Canvas *aw = get_active_window();
            if (aw) {
                if (strcmp(item, "Icons") == 0) set_canvas_view_mode(aw, VIEW_ICONS);
                else if (strcmp(item, "Names") == 0) set_canvas_view_mode(aw, VIEW_NAMES);
            }
        }
        return;
    }
    if (menu->parent_menu != menubar) return;  // Only top-level or handled above

    switch (menu->parent_index) {
        case 0:  // Workbench
            if (strcmp(item, "Execute") == 0) {
                // TODO: Implement execute command logic
            } else if (strcmp(item, "Settings") == 0) {
                // TODO: Open settings dialog or file
            } else if (strcmp(item, "About") == 0) {
                // TODO: Display about information

            } else if (strcmp(item, "Suspend") == 0) {
                system("systemctl suspend &");

            } else if (strcmp(item, "Quit AmiWB") == 0) {
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
                return;
            }
            break;

        case 1:  // Window
            if (strcmp(item, "New Drawer") == 0) {
                // TODO: create new drawer
            } else if (strcmp(item, "Open Parent") == 0) {
                // TODO: navigate up
            } else if (strcmp(item, "Close") == 0) {
                // TODO: close active window
            } else if (strcmp(item, "Select Contents") == 0) {
                // TODO: select all icons in active window
            } else if (strcmp(item, "Clean Up") == 0) {
                Canvas *aw = get_active_window();
                if (aw) { icon_cleanup(aw); compute_max_scroll(aw); redraw_canvas(aw); }
                else { Canvas *desk = get_desktop_canvas(); if (desk) { icon_cleanup(desk); compute_max_scroll(desk); redraw_canvas(desk); } }
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
                // TODO: Open selected icon
            } else if (strcmp(item, "Copy") == 0) {
                // TODO: Copy selected icon
            } else if (strcmp(item, "Rename") == 0) {
                // CAPTURE CONTEXT BEFORE CREATING DIALOG
                Canvas *active_window = get_active_window();
                FileIcon *selected = NULL;
                
                printf("DEBUG: Active window: %p (type=%d)\n", (void*)active_window, 
                       active_window ? active_window->type : -1);
                
                // First try to get selected icon from the active window
                if (active_window && active_window->type == WINDOW) {
                    selected = get_selected_icon_from_canvas(active_window);
                    printf("DEBUG: get_selected_icon_from_canvas returned: %p\n", (void*)selected);
                }
                
                // If no active window or no selection, fall back to global search
                if (!selected) {
                    selected = get_selected_icon();
                    printf("DEBUG: get_selected_icon (fallback) returned: %p\n", (void*)selected);
                }
                
                if (selected) {
                    printf("DEBUG: selected->path = '%s'\n", selected->path ? selected->path : "NULL");
                    printf("DEBUG: selected->label = '%s'\n", selected->label ? selected->label : "NULL");
                }
                
                if (selected && selected->label && selected->path) {
                    // Store the icon globally so the callback can access it
                    g_rename_icon = selected;
                    show_rename_dialog(selected->label, rename_file_ok_callback, rename_file_cancel_callback, selected);
                } else {
                    printf("No icon selected for rename\n");
                }
            } else if (strcmp(item, "Information") == 0) {
                // TODO: Show icon properties
            } else if (strcmp(item, "delete") == 0) {
                // TODO: Delete selected icon
            }
            break;

        case 3:  // Tools
            if (strcmp(item, "XCalc") == 0) {
                system("xcalc &");  // TODO: Handle errors and paths
            
            } else if (strcmp(item, "PavuControl") == 0) {
                system("pavucontrol &");

            } else if (strcmp(item, "Sublime Text") == 0) {
                system("subl &");

            } else if (strcmp(item, "Sublime Text") == 0) {
                system("subl &");   

            } else if (strcmp(item, "Shell") == 0) {
                system("kitty &"); 

            } else if (strcmp(item, "Brave Browser") == 0) {
                //printf("launching brave\n");
                system("brave-browser &");
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
            // Handle unexpected index (log error)
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
// Measure widest label to size the dropdown width.
int get_submenu_width(Menu *menu) {
    if (!menu || !font) return 80;
    int max_width = 80;
    int padding = 20;
    for (int i = 0; i < menu->item_count; i++) {
        XGlyphInfo extents;
        XftTextExtentsUtf8(get_render_context()->dpy, font, 
            (FcChar8 *)menu->items[i], strlen(menu->items[i]), &extents);
        int width = extents.xOff + padding;
        if (width > max_width) max_width = width;
    }
    return max_width;
}

// Set app menu
// Placeholder: application-provided menu integration.
void set_app_menu(Menu *app_menu) {
    // TODO
}