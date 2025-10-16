// Menu System - State Management Module
// Handles logo ↔ show menus mode switching

#include "menu_internal.h"
#include "menu_public.h"
#include "../intuition/itn_public.h"
#include "../events/evt_public.h"
#include "../config.h"

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
