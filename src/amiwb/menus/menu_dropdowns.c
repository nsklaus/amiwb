// Menu System - Dropdown Management Module
// Handles creation and display of dropdown menus

#include "menu_internal.h"
#include "menu_public.h"
#include "../intuition/itn_public.h"
#include "../workbench/wb_public.h"
#include "../workbench/wb_internal.h"
#include "../font_manager.h"
#include "../events/evt_public.h"

// ============================================================================
// Dropdown Menu Creation and Display
// ============================================================================

// Show dropdown menu
// Create and show a dropdown for the given menubar item at x,y.
void show_dropdown_menu(Menu *menu, int index, int x, int y) {

    if (!menu || index < 0 || index >= menu->item_count ||
            !menu->submenus[index]) {
        return;
    }

    // Close any nested submenu from a previous active dropdown
    Menu *nested = menu_core_get_nested_menu();
    if (nested && nested->canvas) {
        RenderContext *ctx = get_render_context();
        if (ctx && nested->canvas->win != None) {
            clear_press_target_if_matches(nested->canvas->win);  // Clear before destroy
            safe_unmap_window(ctx->dpy, nested->canvas->win);
        }
        itn_canvas_destroy(nested->canvas);
        nested->canvas = NULL;  // Prevent double-free
        menu_core_set_nested_menu(NULL);
    }
    menu_core_set_active_menu(menu->submenus[index]);
    Menu *active = get_active_menu();

    // Destroy any existing canvas from previous menu display to prevent leak
    if (active->canvas) {
        RenderContext *ctx = get_render_context();
        if (ctx && active->canvas->win != None) {
            safe_unmap_window(ctx->dpy, active->canvas->win);
        }
        itn_canvas_destroy(active->canvas);
        active->canvas = NULL;
    }

    Menu *menubar = get_menubar_menu();

    // Update enabled states for Icons menu based on current selection
    // Only do this for system menus, not app menus
    if (!is_app_menu_active() && menu == menubar && index == 2) {  // Icons menu
        bool has_selected_icon = false;
        bool can_delete = false;
        FileIcon *selected = NULL;
        Canvas *aw = itn_focus_get_active();
        Canvas *check_canvas = NULL;

        // If no active window, check desktop
        if (!aw || aw->type == DESKTOP) {
            check_canvas = itn_canvas_get_desktop();
        } else if (aw->type == WINDOW) {
            check_canvas = aw;
        }

        if (check_canvas) {
            // Check if any icon is selected in the canvas
            FileIcon **icon_array = wb_icons_array_get();
            int icon_count = wb_icons_array_count();
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
        if (active->enabled) {
            active->enabled[0] = has_selected_icon;  // Open - works for all icon types
            active->enabled[1] = can_modify;         // Copy - restricted
            active->enabled[2] = can_modify;         // Rename - restricted
            active->enabled[3] = has_selected_icon;  // Extract - works for archives
            active->enabled[4] = has_selected_icon;  // Eject - works for devices
            active->enabled[5] = has_selected_icon;  // Information - works for all
            active->enabled[6] = can_delete;         // Delete - restricted
        }
    }

    // Update enabled states for Windows menu based on active window
    // Only do this for system menus, not app menus
    if (!is_app_menu_active() && menu == menubar && index == 1) {  // Windows menu
        Canvas *aw = itn_focus_get_active();
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
        if (active->enabled) {
            // Desktop-focused or workbench window operations
            active->enabled[0] = is_workbench_window || desktop_focused;  // New Drawer - workbench or desktop
            active->enabled[1] = can_go_parent;                          // Open Parent - only for workbench with parent path
            active->enabled[2] = has_active_window;                      // Close - only if there's a window
            active->enabled[3] = is_workbench_window || desktop_focused;  // Select Contents - workbench or desktop
            active->enabled[4] = is_workbench_window || desktop_focused;  // Clean Up - workbench or desktop
            active->enabled[5] = is_workbench_window || desktop_focused;  // Refresh - workbench or desktop
            active->enabled[6] = is_workbench_window || desktop_focused;  // View Modes - workbench or desktop
        }
    }

    int submenu_width = get_submenu_width(active);
    active->canvas = create_canvas(NULL, x, y, submenu_width,
        active->item_count * MENU_ITEM_HEIGHT + 8, MENU);  // Added 8 pixels: 4 for top offset, 4 for bottom padding
    if (!active->canvas) { return; }
    active->canvas->bg_color =
        (XRenderColor){0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};

    active->selected_item = -1;
    XMapRaised(get_render_context()->dpy, active->canvas->win);
    redraw_canvas(active->canvas);
}
