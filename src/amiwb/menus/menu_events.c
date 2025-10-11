// Menu System - Event Handling Module
// All event handlers for menu interactions

#include "menu_internal.h"
#include "menu_public.h"
#include "../intuition/itn_public.h"
#include "../font_manager.h"
#include "../events/evt_public.h"
#include "../config.h"

// ============================================================================
// Menubar Motion Handling
// ============================================================================

// Handle motion on menubar (for hover highlighting)
// Update hover selection across top-level items as the mouse moves.
void menu_handle_menubar_motion(XMotionEvent *event) {
    if (!get_show_menus_state()) return;

    RenderContext *ctx = get_render_context();
    Menu *menubar = get_menubar_menu();

    if (!ctx || !menubar) return;
    int prev_selected = menubar->selected_item;
    menubar->selected_item = -1;
    int x_pos = 10;
    int padding = 20;
    for (int i = 0; i < menubar->item_count; i++) {
        XGlyphInfo extents;
        XftTextExtentsUtf8(ctx->dpy, font_manager_get(), (FcChar8 *)menubar->items[i],
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
        Menu *active = get_active_menu();
        if (active && active->canvas) {
            XSync(ctx->dpy, False);  // Ensure pending operations complete
            if (active->canvas->win != None) {
                safe_unmap_window(ctx->dpy, active->canvas->win);
                XSync(ctx->dpy, False);  // Wait for unmap to complete
            }
            itn_canvas_destroy(active->canvas);
            // Note: itn_canvas_destroy() already sets active->canvas = NULL
            menu_core_set_active_menu(NULL);
        }
        Menu *nested = menu_core_get_nested_menu();
        if (nested && nested->canvas) {
            XSync(ctx->dpy, False);  // Ensure pending operations complete
            if (nested->canvas->win != None) {
                safe_unmap_window(ctx->dpy, nested->canvas->win);
                XSync(ctx->dpy, False);  // Wait for unmap to complete
            }
            itn_canvas_destroy(nested->canvas);
            // Note: itn_canvas_destroy() already sets nested->canvas = NULL
            menu_core_set_nested_menu(NULL);
        }
        if (menubar->selected_item != -1 &&
                menubar->submenus[menubar->selected_item]) {
            int submenu_x = 10;
            for (int j = 0; j < menubar->selected_item; j++) {
                XGlyphInfo extents;
                XftTextExtentsUtf8(ctx->dpy, font_manager_get(), (FcChar8 *)menubar->items[j],
                    strlen(menubar->items[j]), &extents);
                submenu_x += extents.xOff + padding;
            }
            show_dropdown_menu(menubar, menubar->selected_item, submenu_x,
                MENU_ITEM_HEIGHT);
        }
        redraw_canvas(menubar->canvas);
    }
}

// ============================================================================
// Menu Closing Helpers
// ============================================================================

// Close the currently open nested submenu, if any.
void close_nested_if_any(void) {
    RenderContext *ctx = get_render_context();
    Menu *nested = menu_core_get_nested_menu();
    if (nested && nested->canvas) {
        XSync(ctx->dpy, False);  // Complete pending operations
        if (ctx && nested->canvas->win != None) {
            clear_press_target_if_matches(nested->canvas->win);  // Clear before destroy
            safe_unmap_window(ctx->dpy, nested->canvas->win);
            XSync(ctx->dpy, False);  // Wait for unmap
        }
        itn_canvas_destroy(nested->canvas);
        nested->canvas = NULL;  // Prevent double-free
        menu_core_set_nested_menu(NULL);
    }
}

// Close all open menus (used when resolution changes)
void close_all_menus(void) {
    RenderContext *ctx = get_render_context();
    if (!ctx) return;

    // Close nested menu if open
    Menu *nested = menu_core_get_nested_menu();
    if (nested && nested->canvas) {
        if (nested->canvas->win != None) {
            safe_unmap_window(ctx->dpy, nested->canvas->win);
            XSync(ctx->dpy, False);
        }
        itn_canvas_destroy(nested->canvas);
        nested->canvas = NULL;
        menu_core_set_nested_menu(NULL);  // Clear pointer to prevent stale reference
    }

    // Close active menu if open
    Menu *active = get_active_menu();
    if (active && active->canvas) {
        if (active->canvas->win != None) {
            safe_unmap_window(ctx->dpy, active->canvas->win);
            XSync(ctx->dpy, False);
        }
        itn_canvas_destroy(active->canvas);
        active->canvas = NULL;  // Prevent double-free

        // If it's a window list (parent_index == -1), free the temporary menu
        if (active->parent_index == -1) {
            if (active->items) {
                for (int i = 0; i < active->item_count; i++) {
                    if (active->items[i]) free(active->items[i]);
                }
                free(active->items);
            }
            if (active->shortcuts) {
                for (int i = 0; i < active->item_count; i++) {
                    if (active->shortcuts[i]) free(active->shortcuts[i]);
                }
                free(active->shortcuts);
            }
            if (active->enabled) free(active->enabled);
            if (active->window_refs) free(active->window_refs);
            free(active);
        }
        menu_core_set_active_menu(NULL);
    }

    // Revert menubar to logo state if needed
    Menu *menubar = get_menubar_menu();
    Canvas *menubar_canvas = get_menubar();
    if (menubar && menubar_canvas && get_show_menus_state()) {
        toggle_menubar_state();
        redraw_canvas(menubar_canvas);
    }
}

// Close window list menu if it's open
void close_window_list_if_open(void) {
    RenderContext *ctx = get_render_context();
    if (!ctx) return;

    // Check if active menu is the window list (parent_index == -1)
    Menu *active = get_active_menu();
    if (active && active->parent_index == -1) {
        if (active->canvas) {
            XSync(ctx->dpy, False);
            if (active->canvas->win != None) {
                clear_press_target_if_matches(active->canvas->win);
                safe_unmap_window(ctx->dpy, active->canvas->win);
                XSync(ctx->dpy, False);
            }
            itn_canvas_destroy(active->canvas);
            active->canvas = NULL;  // Prevent double-free
        }
        // Free the menu structure and its items
        if (active->items) {
            for (int i = 0; i < active->item_count; i++) {
                if (active->items[i]) free(active->items[i]);
            }
            free(active->items);
        }
        if (active->shortcuts) {
            for (int i = 0; i < active->item_count; i++) {
                if (active->shortcuts[i]) free(active->shortcuts[i]);
            }
            free(active->shortcuts);
        }
        if (active->enabled) free(active->enabled);
        free(active);
        menu_core_set_active_menu(NULL);
    }
}

// ============================================================================
// Button Press/Release Handlers
// ============================================================================

// Handle clicks inside a dropdown or nested submenu.
// Just track the press, don't trigger actions yet.
void menu_handle_button_press(XButtonEvent *event) {
    // Just track the press, actions happen on release
}

// Handle button release inside menus - this triggers the actual action.
void menu_handle_button_release(XButtonEvent *event) {
    RenderContext *ctx = get_render_context();
    if (!ctx) return;

    // Only LMB (Button1) executes menu items - ignore MMB, RMB, and scroll wheel
    if (event->button != Button1) return;

    Menu *active = get_active_menu();
    Menu *nested = menu_core_get_nested_menu();
    Menu *target_menu = NULL;
    if (active && active->canvas && event->window == active->canvas->win) target_menu = active;
    else if (nested && nested->canvas && event->window == nested->canvas->win) target_menu = nested;
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
    nested = menu_core_get_nested_menu();  // Re-fetch in case it changed
    if (nested && nested->canvas) {
        XSync(ctx->dpy, False);  // Complete pending operations
        if (nested->canvas->win != None) {
            clear_press_target_if_matches(nested->canvas->win);  // Clear before destroy
            safe_unmap_window(ctx->dpy, nested->canvas->win);
            XSync(ctx->dpy, False);  // Wait for unmap
        }
        itn_canvas_destroy(nested->canvas);
        nested->canvas = NULL;  // Prevent double-free
        menu_core_set_nested_menu(NULL);
    }
    active = get_active_menu();  // Re-fetch in case it changed
    if (active && active->canvas) {
        XSync(ctx->dpy, False);  // Complete pending operations
        if (active->canvas->win != None) {
            clear_press_target_if_matches(active->canvas->win);  // Clear before destroy
            safe_unmap_window(ctx->dpy, active->canvas->win);
            XSync(ctx->dpy, False);  // Wait for unmap
        }
        itn_canvas_destroy(active->canvas);
        active->canvas = NULL;  // Prevent double-free
        menu_core_set_active_menu(NULL);
    }

    // Conditional redraw: only if not quitting (event loop still running)
    Menu *menubar = get_menubar_menu();
    Canvas *menubar_canvas = get_menubar();
    if (evt_core_is_running() && menubar && menubar_canvas) {
        // Always revert menubar to logo state after a click
        if (get_show_menus_state()) toggle_menubar_state();
        redraw_canvas(menubar_canvas);
    }
}

// ============================================================================
// Menubar Press Handler
// ============================================================================

// Handle button press on menubar
// Right-click toggles logo vs menus on the menubar.
void menu_handle_menubar_press(XButtonEvent *event) {
    RenderContext *ctx = get_render_context();
    if (!ctx) return;

    // Handle right-click - always toggle menubar state and close any dropdown
    if (event->button == Button3) {
        // Close window list menu if open
        Menu *active = get_active_menu();
        if (active && active->parent_index == -1) {
            // This is the window list menu
            XSync(ctx->dpy, False);
            if (active->canvas->win != None) {
                clear_press_target_if_matches(active->canvas->win);
                safe_unmap_window(ctx->dpy, active->canvas->win);
                XSync(ctx->dpy, False);
            }
            itn_canvas_destroy(active->canvas);
            active->canvas = NULL;  // Prevent double-free

            // Free the temporary window menu
            if (active->items) {
                for (int i = 0; i < active->item_count; i++) {
                    if (active->items[i]) free(active->items[i]);
                }
                free(active->items);
            }
            if (active->shortcuts) {
                for (int i = 0; i < active->item_count; i++) {
                    if (active->shortcuts[i]) free(active->shortcuts[i]);
                }
                free(active->shortcuts);
            }
            if (active->enabled) free(active->enabled);
            if (active->window_refs) free(active->window_refs);
            free(active);
            menu_core_set_active_menu(NULL);
        }
        toggle_menubar_state();
    } else if (event->button == Button1) {
        // Check if we're in logo mode and clicking on the right button area
        if (!get_show_menus_state()) {
            int screen_width = DisplayWidth(ctx->dpy, DefaultScreen(ctx->dpy));
            // Button is drawn at width-28 and is 26 pixels wide (from render.c)
            int button_start = screen_width - 30;  // Give a bit more area for easier clicking

            if (event->x >= button_start) {
                // Check if window list is already open
                Menu *active = get_active_menu();
                if (active && active->parent_index == -1) {
                    // Window list is open - close it
                    XSync(ctx->dpy, False);
                    if (active->canvas->win != None) {
                        clear_press_target_if_matches(active->canvas->win);
                        safe_unmap_window(ctx->dpy, active->canvas->win);
                        XSync(ctx->dpy, False);
                    }
                    itn_canvas_destroy(active->canvas);
                    active->canvas = NULL;  // Prevent double-free

                    // Free the temporary window menu
                    if (active->items) {
                        for (int i = 0; i < active->item_count; i++) {
                            if (active->items[i]) free(active->items[i]);
                        }
                        free(active->items);
                    }
                    if (active->shortcuts) {
                        for (int i = 0; i < active->item_count; i++) {
                            if (active->shortcuts[i]) free(active->shortcuts[i]);
                        }
                        free(active->shortcuts);
                    }
                    if (active->enabled) free(active->enabled);
                    if (active->window_refs) free(active->window_refs);
                    free(active);
                    menu_core_set_active_menu(NULL);
                } else {
                    // Show window list below the button (menubar height)
                    // x position will be calculated to align with screen edge
                    show_window_list_menu(0, MENU_ITEM_HEIGHT);  // x is ignored, calculated inside
                }
            } else {
                // Click outside button area - close window list if open
                Menu *active = get_active_menu();
                if (active && active->parent_index == -1) {
                    XSync(ctx->dpy, False);
                    if (active->canvas->win != None) {
                        clear_press_target_if_matches(active->canvas->win);
                        safe_unmap_window(ctx->dpy, active->canvas->win);
                        XSync(ctx->dpy, False);
                    }
                    itn_canvas_destroy(active->canvas);
                    active->canvas = NULL;  // Prevent double-free

                    // Free the temporary window menu
                    if (active->items) {
                        for (int i = 0; i < active->item_count; i++) {
                            if (active->items[i]) free(active->items[i]);
                        }
                        free(active->items);
                    }
                    if (active->shortcuts) {
                        for (int i = 0; i < active->item_count; i++) {
                            if (active->shortcuts[i]) free(active->shortcuts[i]);
                        }
                        free(active->shortcuts);
                    }
                    if (active->enabled) free(active->enabled);
                    if (active->window_refs) free(active->window_refs);
                    free(active);
                    menu_core_set_active_menu(NULL);
                }
            }
        }
    }
}

// ============================================================================
// Dropdown Motion Handling
// ============================================================================

// If the highlighted item in the dropdown has a submenu, open it.
void maybe_open_nested_for_selection(void) {
    Menu *active = get_active_menu();
    if (!active) return;
    // Only open nested if the selected item has a submenu
    if (!active->submenus) return;
    int sel = active->selected_item;
    if (sel < 0 || sel >= active->item_count) return;
    Menu *child = active->submenus[sel];
    RenderContext *ctx = get_render_context();
    if (!ctx) return;
    if (child) {
        // If already open for the same selection, do nothing
        Menu *nested = menu_core_get_nested_menu();
        if (nested && nested == child) return;
        // Close previous nested if any
        if (nested && nested->canvas) {
            XSync(ctx->dpy, False);  // Complete pending operations
            if (nested->canvas->win != None) {
                clear_press_target_if_matches(nested->canvas->win);  // Clear before destroy
                safe_unmap_window(ctx->dpy, nested->canvas->win);
                XSync(ctx->dpy, False);  // Wait for unmap
            }
            itn_canvas_destroy(nested->canvas);
            nested->canvas = NULL;  // Prevent double-free
            menu_core_set_nested_menu(NULL);
        }
        // Open new nested at the right edge of active_menu, aligned to item
        int submenu_width = get_submenu_width(child);
        int nx = active->canvas->x + active->canvas->width;
        int ny = active->canvas->y + sel * MENU_ITEM_HEIGHT;
        menu_core_set_nested_menu(child);
        nested = child;  // Update local reference

        // Destroy any existing canvas from previous display to prevent leak
        if (nested->canvas) {
            if (nested->canvas->win != None) {
                safe_unmap_window(ctx->dpy, nested->canvas->win);
                XSync(ctx->dpy, False);
            }
            itn_canvas_destroy(nested->canvas);
            nested->canvas = NULL;
        }

        nested->canvas = create_canvas(NULL, nx, ny, submenu_width,
            nested->item_count * MENU_ITEM_HEIGHT + 8, MENU);
        if (nested->canvas) {
            nested->canvas->bg_color = (XRenderColor){0xFFFF,0xFFFF,0xFFFF,0xFFFF};
            nested->selected_item = -1;

            // Update enabled states and checkmarks for View Modes submenu
            if (nested->parent_menu && nested->parent_menu->parent_index == 1 &&
                nested->parent_index == 6) { // View Modes submenu
                Canvas *active_canvas = itn_focus_get_active();
                bool desktop_focused = (!active_canvas || active_canvas->type == DESKTOP);

                // Enable/disable items based on context
                if (nested->enabled) {
                    nested->enabled[0] = true;  // Icons - always enabled
                    nested->enabled[1] = !desktop_focused;  // Names - disabled for desktop
                    nested->enabled[2] = true;  // Hidden - always enabled
                    nested->enabled[3] = true;  // Spatial - always enabled
                }

                // Update checkmarks to reflect current state
                update_view_modes_checkmarks();
            }

            XMapRaised(ctx->dpy, nested->canvas->win);
            redraw_canvas(nested->canvas);
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

    Menu *active = get_active_menu();
    if (active && active->canvas && event->window == active->canvas->win) {
        int prev_selected = active->selected_item;
        int new_item = event->y / MENU_ITEM_HEIGHT;
        if (new_item < 0 || new_item >= active->item_count) {
            active->selected_item = -1;
        } else {
            // Don't highlight disabled items
            if (active->enabled && !active->enabled[new_item]) {
                active->selected_item = -1;  // No selection for disabled items
            } else {
                active->selected_item = new_item;
            }
        }
        if (active->selected_item != prev_selected) {
            redraw_canvas(active->canvas);
            maybe_open_nested_for_selection();
        }
        return;
    }

    Menu *nested = menu_core_get_nested_menu();
    if (nested && nested->canvas && event->window == nested->canvas->win) {
        int prev_selected = nested->selected_item;
        int new_item = event->y / MENU_ITEM_HEIGHT;
        if (new_item < 0 || new_item >= nested->item_count) {
            nested->selected_item = -1;
        } else {
            // Don't highlight disabled items
            if (nested->enabled && !nested->enabled[new_item]) {
                nested->selected_item = -1;  // No selection for disabled items
            } else {
                nested->selected_item = new_item;
            }
        }
        if (nested->selected_item != prev_selected) {
            redraw_canvas(nested->canvas);
        }
        return;
    }
}

// ============================================================================
// Keyboard Navigation
// ============================================================================

// Handle key press for menu navigation
// Keyboard navigation placeholder for menus.
void menu_handle_key_press(XKeyEvent *event) {
    // Menubar registered key press event
}
