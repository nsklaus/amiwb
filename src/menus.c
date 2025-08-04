// File: menus.c
#include "menus.h"
#include "config.h"
#include "intuition.h"
#include "workbench.h"
#include "render.h"
#include "events.h"
#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>
#include <fontconfig/fontconfig.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RESOURCE_DIR_SYSTEM "/usr/local/share/amiwb"  
#define RESOURCE_DIR_USER ".config/amiwb"  

// Static menu resources
static XftFont *font = NULL;
static XftColor text_color;
static Menu *active_menu = NULL;    // Current dropdown menu
static Menu *menubar = NULL;        // Global menubar
static bool show_menus = false;     // State: false for logo, true for menus

// Mode-specific arrays
static char **logo_items = NULL;
static int logo_item_count = 1;
static char **full_menu_items = NULL;
static int full_menu_item_count = 0;
static Menu **full_submenus = NULL;

static char *get_resource_path(const char *rel_path) {
    char *home = getenv("HOME");
    char user_path[1024];
    snprintf(user_path, sizeof(user_path), "%s/%s/%s", home, RESOURCE_DIR_USER, rel_path);
    if (access(user_path, F_OK) == 0) return strdup(user_path);
    char sys_path[1024];
    snprintf(sys_path, sizeof(sys_path), "%s/%s", RESOURCE_DIR_SYSTEM, rel_path);
    return strdup(sys_path);
}

// Initialize menu resources
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
/*    fprintf(stderr, "Set menubar bg_color: R=0x%04X, G=0x%04X, B=0x%04X, A=0x%04X\n",
        menubar->canvas->bg_color.red, menubar->canvas->bg_color.green,
        menubar->canvas->bg_color.blue, menubar->canvas->bg_color.alpha);*/
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
    Menu *wb_submenu = malloc(sizeof(Menu));
    wb_submenu->item_count = 4;
    wb_submenu->items = malloc(wb_submenu->item_count * sizeof(char*));
    wb_submenu->items[0] = strdup("Execute");
    wb_submenu->items[1] = strdup("Settings");
    wb_submenu->items[2] = strdup("About");
    wb_submenu->items[3] = strdup("Quit AmiWB");
    wb_submenu->selected_item = -1;
    wb_submenu->parent_menu = menubar;
    wb_submenu->parent_index = 0;
    wb_submenu->submenus = NULL;
    wb_submenu->canvas = NULL;
    menubar->submenus[0] = wb_submenu;

    // Window submenu (index 1)
    Menu *win_submenu = malloc(sizeof(Menu));
    win_submenu->item_count = 7;
    win_submenu->items = malloc(win_submenu->item_count * sizeof(char*));
    win_submenu->items[0] = strdup("New Drawer");
    win_submenu->items[1] = strdup("Open Parent");
    win_submenu->items[2] = strdup("Close");
    win_submenu->items[3] = strdup("Select Contents");
    win_submenu->items[4] = strdup("Clean Up");
    win_submenu->items[5] = strdup("Show");
    win_submenu->items[6] = strdup("View By ..");
    win_submenu->selected_item = -1;
    win_submenu->parent_menu = menubar;
    win_submenu->parent_index = 1;
    win_submenu->submenus = NULL;
    win_submenu->canvas = NULL;
    menubar->submenus[1] = win_submenu;

    // Icons submenu (index 2)
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
    Menu *tools_submenu = malloc(sizeof(Menu));
    tools_submenu->item_count = 4;
    tools_submenu->items = malloc(tools_submenu->item_count * sizeof(char*));
    tools_submenu->items[0] = strdup("XCalc");
    tools_submenu->items[1] = strdup("Brave Browser");
    tools_submenu->items[2] = strdup("Sublime Text");
    tools_submenu->items[3] = strdup("Shell");
    tools_submenu->selected_item = -1;
    tools_submenu->parent_menu = menubar;
    tools_submenu->parent_index = 3;
    tools_submenu->submenus = NULL;
    tools_submenu->canvas = NULL;
    menubar->submenus[3] = tools_submenu;

    // Setup mode-specific arrays
    logo_items = malloc(logo_item_count * sizeof(char*));
    logo_items[0] = strdup("AmiWB");

    full_menu_item_count = menubar->item_count;
    full_menu_items = menubar->items;
    full_submenus = menubar->submenus;

    // Initial default mode: logo
    menubar->items = logo_items;
    menubar->item_count = logo_item_count;
    menubar->submenus = NULL;

    redraw_canvas(menubar->canvas);
}

void cleanup_menus(void) {
    RenderContext *ctx = get_render_context();
    if (!ctx) return;

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
    printf("Called cleanup_menus()\n");
}

// Get show_menus state
bool get_show_menus_state(void) {
    return show_menus;
}

// Toggle menubar state
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
        if (active_menu) {  // Close any open submenu
            RenderContext *ctx = get_render_context();
            if (ctx) XUnmapWindow(ctx->dpy, active_menu->canvas->win);
            destroy_canvas(active_menu->canvas);
            active_menu = NULL;
        }

    }
    if (menubar) redraw_canvas(menubar->canvas);
}

// Get global menubar canvas
Canvas *get_menubar(void) {
    return menubar ? menubar->canvas : NULL;
}

// Get the menubar Menu struct
Menu *get_menubar_menu(void) {
    return menubar;
}

// Get Menu for a canvas
Menu *get_menu_by_canvas(Canvas *canvas) {
    if (canvas == get_menubar()) return get_menubar_menu();
    if (active_menu && active_menu->canvas == canvas) return active_menu;
    return NULL;
}

// Handle motion for menubar
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
        if (active_menu) {
            XUnmapWindow(ctx->dpy, active_menu->canvas->win);
            destroy_canvas(active_menu->canvas);
            active_menu = NULL;
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

void menu_handle_button_press(XButtonEvent *event) {
    RenderContext *ctx = get_render_context();
    if (!ctx || !active_menu || event->window != active_menu->canvas->win) {
        return;
    }

    int item = event->y / MENU_ITEM_HEIGHT;
    if (item >= 0 && item < active_menu->item_count) {
        handle_menu_selection(active_menu, item);
    }
    // Close submenu
    // Check if active_menu is still set 
    // (may have been closed in handle_menu_selection via toggle)
    if (active_menu) {  
        XUnmapWindow(ctx->dpy, active_menu->canvas->win);
        destroy_canvas(active_menu->canvas);
        active_menu = NULL;
    }
    // Conditional redraw: only if not quitting (running is true)
    if (running && menubar && menubar->canvas) {
        redraw_canvas(menubar->canvas);
    }
}

void menu_handle_button_release(XButtonEvent *event) {

    //XUngrabPointer(get_display(), CurrentTime);
}

// Handle button press on menubar
void menu_handle_menubar_press(XButtonEvent *event) {
    if (event->button == Button3) {
        toggle_menubar_state();
    }
}

// Handle motion on dropdown
void menu_handle_motion_notify(XMotionEvent *event) {
    
    RenderContext *ctx = get_render_context();
    if (!ctx || !active_menu || event->window != active_menu->canvas->win) {
        return;
    }

    int prev_selected = active_menu->selected_item;
    active_menu->selected_item = event->y / MENU_ITEM_HEIGHT;
    if (active_menu->selected_item < 0 || 
            active_menu->selected_item >= active_menu->item_count) {

        active_menu->selected_item = -1;
    }

    if (active_menu->selected_item != prev_selected) {
        redraw_canvas(active_menu->canvas);
    }
}

// Handle key press for menu navigation
void menu_handle_key_press(XKeyEvent *event) {
    printf("menu bar registered key press event\n");
}

// Show dropdown menu 
void show_dropdown_menu(Menu *menu, int index, int x, int y) {
    
    if (!menu || index < 0 || index >= menu->item_count || 
            !menu->submenus[index]) {
        return;
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
/*    XGrabPointer(get_display(), menubar->canvas->win, False, 
        PointerMotionMask | ButtonReleaseMask, GrabModeAsync, 
        GrabModeAsync, None, None, CurrentTime);*/
    redraw_canvas(active_menu->canvas);
}

// Process menu item selection
void handle_menu_selection(Menu *menu, int item_index) {
    const char *item = menu->items[item_index];
    if (menu->parent_menu != menubar) return;  // Safeguard for non-top-level 

    switch (menu->parent_index) {
        case 0:  // Workbench
            //printf("reached 'Workbench' menu\n");
            if (strcmp(item, "Execute") == 0) {
                // TODO: Implement execute command logic
            } else if (strcmp(item, "Settings") == 0) {
                // TODO: Open settings dialog or file
            } else if (strcmp(item, "About") == 0) {
                // TODO: Display about information
            } else if (strcmp(item, "Quit AmiWB") == 0) {
                cleanup_menus();
                cleanup_workbench();
                cleanup_intuition();
                cleanup_render();
                quit_event_loop();
                return;
            }
            break;

        case 1:  // Window
            //printf("reached 'Window' menu\n");
            if (strcmp(item, "New Drawer") == 0) {
                // TODO: Create new directory/drawer
            } else if (strcmp(item, "Open Parent") == 0) {
                // TODO: Navigate to parent directory
            } else if (strcmp(item, "Close") == 0) {
                // TODO: Close current window
            } else if (strcmp(item, "Select Contents") == 0) {
                // TODO: Select all icons in window
            } else if (strcmp(item, "Clean Up") == 0) {
                Canvas *target = get_active_window();
                if (target && target->type == WINDOW && target->client_win == None) {
                    icon_cleanup(target);
                } else {
                    icon_cleanup(get_desktop_canvas());
                }
            } else if (strcmp(item, "Show") == 0) {
                // TODO: Show hidden items or similar
            } else if (strcmp(item, "View By ..") == 0) {
                // TODO: Change view mode (e.g., list/icon)
            }
            break;

        case 2:  // Icons
            //printf("reached 'Icons' menu\n");
            if (strcmp(item, "Open") == 0) {
                // TODO: Open selected icon
            } else if (strcmp(item, "Copy") == 0) {
                // TODO: Copy selected icon
            } else if (strcmp(item, "Rename") == 0) {
                // TODO: Rename selected icon
            } else if (strcmp(item, "Information") == 0) {
                // TODO: Show icon properties
            } else if (strcmp(item, "delete") == 0) {
                // TODO: Delete selected icon
            }
            break;

        case 3:  // Tools
            //printf("reached 'Tools' menu\n");
            if (strcmp(item, "XCalc") == 0) {
                system("xcalc &");  // TODO: Handle errors and paths

            } else if (strcmp(item, "Sublime Text") == 0) {
                system("subl &");   

            } else if (strcmp(item, "Shell") == 0) {
                //printf("launching kitty\n");
                system("kitty &"); 

            } else if (strcmp(item, "Brave Browser") == 0) {
                //printf("launching brave\n");
                system("brave-browser &");
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
Menu *get_active_menu(void) {
    return active_menu;
}

// Get submenu width
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
void set_app_menu(Menu *app_menu) {
    // TODO
}