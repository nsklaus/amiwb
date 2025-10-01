// Menu System - Window List Module
// Handles dynamic window list menu creation and display

#include "menu_internal.h"
#include "menu_public.h"
#include "../intuition/itn_public.h"
#include "../font_manager.h"
#include "../events.h"
#include <string.h>
#include <stdlib.h>

// ============================================================================
// Window List Menu
// ============================================================================

// Show window list menu at specified position
void show_window_list_menu(int x, int y) {
    RenderContext *ctx = get_render_context();
    if (!ctx) return;

    // Close any existing dropdown
    if (active_menu && active_menu->canvas) {
        XSync(ctx->dpy, False);
        if (active_menu->canvas->win != None) {
            clear_press_target_if_matches(active_menu->canvas->win);
            safe_unmap_window(ctx->dpy, active_menu->canvas->win);
            XSync(ctx->dpy, False);
        }
        destroy_canvas(active_menu->canvas);
        active_menu->canvas = NULL;  // Prevent double-free
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

    // Add each window with instance numbering for duplicates
    // Count occurrences of each title to add (1), (2), (3) suffixes
    int *title_counts = calloc(window_count, sizeof(int));  // Track which instance number we're on

    for (int i = 0; i < window_count; i++) {
        Canvas *c = window_list[i];
        const char *title = c->title_base ? c->title_base : "Untitled";

        // Count how many windows with this title we've seen so far
        int instance_num = 0;
        for (int j = 0; j < i; j++) {
            const char *prev_title = window_list[j]->title_base ? window_list[j]->title_base : "Untitled";
            if (strcmp(title, prev_title) == 0) {
                instance_num++;
            }
        }

        // Check if there are any more windows with this title after this one
        bool has_duplicates = false;
        for (int j = i + 1; j < window_count; j++) {
            const char *next_title = window_list[j]->title_base ? window_list[j]->title_base : "Untitled";
            if (strcmp(title, next_title) == 0) {
                has_duplicates = true;
                break;
            }
        }

        // Build display name with instance number if needed
        char display_name[256];
        if (instance_num > 0 || has_duplicates) {
            snprintf(display_name, sizeof(display_name), "%s (%d)", title, instance_num + 1);
        } else {
            snprintf(display_name, sizeof(display_name), "%s", title);
        }

        window_menu->items[i + 1] = strdup(display_name);
        window_menu->shortcuts[i + 1] = NULL;  // No shortcuts for individual windows
        window_menu->enabled[i + 1] = true;
        window_menu->window_refs[i + 1] = c;  // Store Canvas pointer
    }

    free(title_counts);

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
    XftTextExtentsUtf8(ctx->dpy, font_manager_get(), (FcChar8 *)sample_text, 20, &extents);

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
