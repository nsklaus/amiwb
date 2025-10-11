// Menu System - State Management Module
// Handles logo ↔ show menus mode switching and time updates

#include "menu_internal.h"
#include "menu_public.h"
#include "../intuition/itn_public.h"
#include "../events/evt_public.h"
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

    menu_core_toggle_show_menus();
    if (get_show_menus_state()) {
        // Switching to menu mode
        menubar->items = menu_core_get_full_menu_items();
        menubar->item_count = menu_core_get_full_menu_item_count();
        menubar->submenus = menu_core_get_full_submenus();
    } else {
        // Switching to logo mode - close ALL dropdowns first
        close_all_menus();

        menubar->items = menu_core_get_logo_items();
        menubar->item_count = menu_core_get_logo_item_count();
        menubar->submenus = NULL;
        menubar->selected_item = -1;
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

            // Allow update if: no active menu, OR canvas was destroyed (stale pointer), OR not window list
            if (!active || active->canvas == NULL || active->parent_index != -1) {
                redraw_canvas(menubar_canvas);

                // Mark canvas as needing compositor update and schedule frame
                menubar_canvas->comp_needs_repaint = true;
                itn_render_accumulate_canvas_damage(menubar_canvas);
                itn_render_schedule_frame();
            }
        }
    }
#endif
}
