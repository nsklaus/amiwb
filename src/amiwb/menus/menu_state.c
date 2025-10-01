// Menu System - State Management Module
// Handles logo ↔ show menus mode switching and time updates

#include "menu_internal.h"
#include "menu_public.h"
#include "../intuition/itn_public.h"
#include "../events.h"
#include "../config.h"
#include <time.h>

// ============================================================================
// State Switching
// ============================================================================

// Toggle menubar state
// Switch between logo mode and full menus.
// Also closes any open dropdowns safely.
void toggle_menubar_state(void) {
    Menu *menubar = get_menubar_menu();
    if (!menubar) return;

    show_menus = !show_menus;
    if (show_menus) {
        // Switching to menu mode
        menubar->items = full_menu_items;
        menubar->item_count = full_menu_item_count;
        menubar->submenus = full_submenus;
    } else {
        // Switching to logo mode
        menubar->items = logo_items;
        menubar->item_count = logo_item_count;
        menubar->submenus = NULL;
        menubar->selected_item = -1;

        // Close any open submenu
        if (active_menu && active_menu->canvas) {
            RenderContext *ctx = get_render_context();
            XSync(ctx->dpy, False);  // Complete pending operations
            if (ctx && active_menu->canvas->win != None) {
                clear_press_target_if_matches(active_menu->canvas->win);  // Clear before destroy
                safe_unmap_window(ctx->dpy, active_menu->canvas->win);
                XSync(ctx->dpy, False);  // Wait for unmap
            }
            destroy_canvas(active_menu->canvas);
            active_menu->canvas = NULL;  // Prevent double-free
            active_menu = NULL;
        }

        // Close nested menu if open
        if (nested_menu && nested_menu->canvas) {
            RenderContext *ctx = get_render_context();
            XSync(ctx->dpy, False);  // Complete pending operations
            if (ctx && nested_menu->canvas->win != None) {
                clear_press_target_if_matches(nested_menu->canvas->win);  // Clear before destroy
                safe_unmap_window(ctx->dpy, nested_menu->canvas->win);
                XSync(ctx->dpy, False);  // Wait for unmap
            }
            destroy_canvas(nested_menu->canvas);
            nested_menu->canvas = NULL;  // Prevent double-free
            nested_menu = NULL;
        }
    }

    if (menubar) redraw_canvas(menubar->canvas);
}

// ============================================================================
// Time-based Updates
// ============================================================================

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
        Canvas *menubar_canvas = get_menubar();
        if (menubar_canvas && !get_show_menus_state()) {
            Menu *active = get_active_menu();
            if (!active || active->parent_index != -1) {
                redraw_canvas(menubar_canvas);
            }
        }
    }
#endif
}
