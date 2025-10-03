// Menu System - Event Handling Module
// All event handlers for menu interactions

#include "menu_internal.h"
#include "menu_public.h"
#include "../intuition/itn_public.h"
#include "../font_manager.h"
#include "../events.h"
#include "../config.h"

// ============================================================================
// Menubar Motion Handling
// ============================================================================

// Handle motion on menubar (for hover highlighting)
// Update hover selection across top-level items as the mouse moves.
void menu_handle_menubar_motion(XMotionEvent *event) {
    if (!show_menus) return;

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
        if (active_menu && active_menu->canvas) {
            XSync(ctx->dpy, False);  // Ensure pending operations complete
            if (active_menu->canvas->win != None) {
                safe_unmap_window(ctx->dpy, active_menu->canvas->win);
                XSync(ctx->dpy, False);  // Wait for unmap to complete
            }
            destroy_canvas(active_menu->canvas);
            // Note: destroy_canvas() already sets active_menu->canvas = NULL
            active_menu = NULL;
        }
        if (nested_menu && nested_menu->canvas) {
            XSync(ctx->dpy, False);  // Ensure pending operations complete
            if (nested_menu->canvas->win != None) {
                safe_unmap_window(ctx->dpy, nested_menu->canvas->win);
                XSync(ctx->dpy, False);  // Wait for unmap to complete
            }
            destroy_canvas(nested_menu->canvas);
            // Note: destroy_canvas() already sets nested_menu->canvas = NULL
            nested_menu = NULL;
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
    if (nested_menu && nested_menu->canvas) {
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

// Close all open menus (used when resolution changes)
void close_all_menus(void) {
    RenderContext *ctx = get_render_context();
    if (!ctx) return;

    // Close nested menu if open
    if (nested_menu && nested_menu->canvas) {
        if (nested_menu->canvas->win != None) {
            safe_unmap_window(ctx->dpy, nested_menu->canvas->win);
            XSync(ctx->dpy, False);
        }
        destroy_canvas(nested_menu->canvas);
        nested_menu->canvas = NULL;
    }

    // Close active menu if open
    if (active_menu && active_menu->canvas) {
        if (active_menu->canvas->win != None) {
            safe_unmap_window(ctx->dpy, active_menu->canvas->win);
            XSync(ctx->dpy, False);
        }
        destroy_canvas(active_menu->canvas);
        active_menu->canvas = NULL;  // Prevent double-free

        // If it's a window list (parent_index == -1), free the temporary menu
        if (active_menu->parent_index == -1) {
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
        }
        active_menu = NULL;
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
    if (active_menu && active_menu->parent_index == -1) {
        if (active_menu->canvas) {
            XSync(ctx->dpy, False);
            if (active_menu->canvas->win != None) {
                clear_press_target_if_matches(active_menu->canvas->win);
                safe_unmap_window(ctx->dpy, active_menu->canvas->win);
                XSync(ctx->dpy, False);
            }
            destroy_canvas(active_menu->canvas);
            active_menu->canvas = NULL;  // Prevent double-free
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

    Menu *target_menu = NULL;
    if (active_menu && active_menu->canvas && event->window == active_menu->canvas->win) target_menu = active_menu;
    else if (nested_menu && nested_menu->canvas && event->window == nested_menu->canvas->win) target_menu = nested_menu;
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
            safe_unmap_window(ctx->dpy, nested_menu->canvas->win);
            XSync(ctx->dpy, False);  // Wait for unmap
        }
        destroy_canvas(nested_menu->canvas);
        nested_menu->canvas = NULL;  // Prevent double-free
        nested_menu = NULL;
    }
    if (active_menu && active_menu->canvas) {
        XSync(ctx->dpy, False);  // Complete pending operations
        if (active_menu->canvas->win != None) {
            clear_press_target_if_matches(active_menu->canvas->win);  // Clear before destroy
            safe_unmap_window(ctx->dpy, active_menu->canvas->win);
            XSync(ctx->dpy, False);  // Wait for unmap
        }
        destroy_canvas(active_menu->canvas);
        active_menu->canvas = NULL;  // Prevent double-free
        active_menu = NULL;
    }

    // Conditional redraw: only if not quitting (running is true)
    Menu *menubar = get_menubar_menu();
    Canvas *menubar_canvas = get_menubar();
    extern bool running;  // Defined in main
    if (running && menubar && menubar_canvas) {
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
        if (active_menu && active_menu->parent_index == -1) {
            // This is the window list menu
            XSync(ctx->dpy, False);
            if (active_menu->canvas->win != None) {
                clear_press_target_if_matches(active_menu->canvas->win);
                safe_unmap_window(ctx->dpy, active_menu->canvas->win);
                XSync(ctx->dpy, False);
            }
            destroy_canvas(active_menu->canvas);
            active_menu->canvas = NULL;  // Prevent double-free

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
                        safe_unmap_window(ctx->dpy, active_menu->canvas->win);
                        XSync(ctx->dpy, False);
                    }
                    destroy_canvas(active_menu->canvas);
                    active_menu->canvas = NULL;  // Prevent double-free

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
                        safe_unmap_window(ctx->dpy, active_menu->canvas->win);
                        XSync(ctx->dpy, False);
                    }
                    destroy_canvas(active_menu->canvas);
                    active_menu->canvas = NULL;  // Prevent double-free

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
            }
        }
    }
}

// ============================================================================
// Dropdown Motion Handling
// ============================================================================

// If the highlighted item in the dropdown has a submenu, open it.
void maybe_open_nested_for_selection(void) {
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
                safe_unmap_window(ctx->dpy, nested_menu->canvas->win);
                XSync(ctx->dpy, False);  // Wait for unmap
            }
            destroy_canvas(nested_menu->canvas);
            nested_menu->canvas = NULL;  // Prevent double-free
            nested_menu = NULL;
        }
        // Open new nested at the right edge of active_menu, aligned to item
        int submenu_width = get_submenu_width(child);
        int nx = active_menu->canvas->x + active_menu->canvas->width;
        int ny = active_menu->canvas->y + sel * MENU_ITEM_HEIGHT;
        nested_menu = child;

        // Destroy any existing canvas from previous display to prevent leak
        if (nested_menu->canvas) {
            if (nested_menu->canvas->win != None) {
                safe_unmap_window(ctx->dpy, nested_menu->canvas->win);
                XSync(ctx->dpy, False);
            }
            destroy_canvas(nested_menu->canvas);
            nested_menu->canvas = NULL;
        }

        nested_menu->canvas = create_canvas(NULL, nx, ny, submenu_width,
            nested_menu->item_count * MENU_ITEM_HEIGHT + 8, MENU);
        if (nested_menu->canvas) {
            nested_menu->canvas->bg_color = (XRenderColor){0xFFFF,0xFFFF,0xFFFF,0xFFFF};
            nested_menu->selected_item = -1;

            // Update enabled states for View Modes submenu
            if (nested_menu->parent_menu && nested_menu->parent_menu->parent_index == 1 &&
                nested_menu->parent_index == 6) { // View Modes submenu
                Canvas *active = itn_focus_get_active();
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

    if (active_menu && active_menu->canvas && event->window == active_menu->canvas->win) {
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

    if (nested_menu && nested_menu->canvas && event->window == nested_menu->canvas->win) {
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

// ============================================================================
// Keyboard Navigation
// ============================================================================

// Handle key press for menu navigation
// Keyboard navigation placeholder for menus.
void menu_handle_key_press(XKeyEvent *event) {
    // Menubar registered key press event
}
