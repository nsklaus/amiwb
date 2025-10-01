// Menu System - Addon Coordinator Module
// Plugin system for menubar widgets (clock, CPU, RAM, etc.)
// Addons only display in logo mode

#include "menu_internal.h"
#include "menu_public.h"
#include "../config.h"
#include "../amiwbrc.h"
#include <string.h>
#include <stdlib.h>

// ============================================================================
// Addon Registry
// ============================================================================

// Head of registered addons linked list
static MenuAddon *registered_addons = NULL;

// ============================================================================
// Addon Registration
// ============================================================================

// Register a new addon to the system
void menu_addon_register(MenuAddon *addon) {
    if (!addon) {
        log_error("[ERROR] menu_addon_register: NULL addon");
        return;
    }

    if (!addon->name[0]) {
        log_error("[ERROR] menu_addon_register: addon has no name");
        return;
    }

    if (!addon->render) {
        log_error("[ERROR] menu_addon_register: addon '%s' has no render function", addon->name);
        return;
    }

    // Check if already registered (prevent duplicates)
    MenuAddon *current = registered_addons;
    while (current) {
        if (strcmp(current->name, addon->name) == 0) {
            log_error("[WARNING] Addon '%s' already registered, ignoring", addon->name);
            return;
        }
        current = current->next;
    }

    // Add to head of list
    addon->next = registered_addons;
    registered_addons = addon;

    // Addon registered successfully (silent - not critical for logging)
}

// Unregister and free an addon by name
void menu_addon_unregister(const char *name) {
    if (!name) return;

    MenuAddon **current = &registered_addons;
    while (*current) {
        MenuAddon *addon = *current;
        if (strcmp(addon->name, name) == 0) {
            // Found it - remove from list
            *current = addon->next;

            // Call cleanup if available
            if (addon->cleanup) {
                addon->cleanup();
            }

            free(addon);
            // Addon unregistered successfully (silent)
            return;
        }
        current = &(*current)->next;
    }
}

// ============================================================================
// Addon Callback Coordination
// ============================================================================

// Comparison function for sorting addons by config_order
static int compare_config_order(const void *a, const void *b) {
    MenuAddon *addon_a = *(MenuAddon **)a;
    MenuAddon *addon_b = *(MenuAddon **)b;
    return addon_a->config_order - addon_b->config_order;
}

// Render all enabled addons (called during menubar rendering in logo mode)
// Addons are positioned in 3 zones: LEFT, MIDDLE, RIGHT
// Within each zone, addons are rendered in config file order
void menu_addon_render_all(RenderContext *ctx, Canvas *menubar, int *x, int y) {
    if (!ctx || !menubar) return;

    // Only render addons in logo mode
    if (show_menus) return;

    // Collect enabled addons by position zone
    MenuAddon *left[32] = {0};
    MenuAddon *middle[32] = {0};
    MenuAddon *right[32] = {0};
    int left_count = 0, middle_count = 0, right_count = 0;

    MenuAddon *current = registered_addons;
    while (current) {
        if (current->enabled && current->render) {
            if (current->position == ADDON_POS_LEFT && left_count < 32) {
                left[left_count++] = current;
            } else if (current->position == ADDON_POS_MIDDLE && middle_count < 32) {
                middle[middle_count++] = current;
            } else if (current->position == ADDON_POS_RIGHT && right_count < 32) {
                right[right_count++] = current;
            }
        }
        current = current->next;
    }

    // Sort each zone by config_order (preserves order from amiwbrc)
    if (left_count > 1) qsort(left, left_count, sizeof(MenuAddon*), compare_config_order);
    if (middle_count > 1) qsort(middle, middle_count, sizeof(MenuAddon*), compare_config_order);
    if (right_count > 1) qsort(right, right_count, sizeof(MenuAddon*), compare_config_order);

    // Calculate total width for RIGHT zone (needed for positioning from right edge)
    int right_total_width = 0;
    for (int i = 0; i < right_count; i++) {
        right_total_width += right[i]->width;
    }

    // Calculate total ACTUAL width for MIDDLE zone (for proper visual centering)
    // Use actual text widths, not reserved widths, to center the visible content
    // Include spacing between addons: width1 + spacing + width2 + spacing + width3
    int middle_total_width = 0;
    for (int i = 0; i < middle_count; i++) {
        // Use reserved width for now (actual width would require text measurement)
        // TODO: Could cache actual widths in addon structure for more accurate centering
        middle_total_width += middle[i]->width;
        if (i < middle_count - 1) {  // Add spacing between addons (not after last one)
            middle_total_width += 40;
        }
    }

    // Adjust centering to account for typical wasted space in reserved widths
    // CPU: ~80px actual vs 120px reserved = 40px wasted
    // Memory: ~100px actual vs 140px reserved = 40px wasted
    // Fans: ~90px actual vs 130px reserved = 40px wasted
    // Total: ~120px wasted, shift left by ~60px to visually center
    int centering_adjustment = -(middle_count * 20);  // Shift left proportionally

    // Render LEFT zone (starts after logo, grows rightward)
    int left_x = 100;  // Start after "AmiWB" logo
    for (int i = 0; i < left_count; i++) {
        left[i]->render(ctx, menubar, &left_x, y);
    }

    // Render MIDDLE zone (centered in menubar)
    if (middle_count > 0) {
        int middle_x = (menubar->width - middle_total_width) / 2 + centering_adjustment;
        for (int i = 0; i < middle_count; i++) {
            middle[i]->render(ctx, menubar, &middle_x, y);
        }
    }

    // Render RIGHT zone (right-aligned, 20px gap before menu button)
    // Start position: 20px before the menu button (button is at width-30)
    if (right_count > 0) {
        int right_x = menubar->width - 30 - 20;  // Menu button at -30, gap of 20px
        for (int i = 0; i < right_count; i++) {
            right[i]->render(ctx, menubar, &right_x, y);
        }
    }
}

// Update all addons (called periodically, typically every 1 second)
void menu_addon_update_all(void) {
    MenuAddon *current = registered_addons;
    while (current) {
        if (current->enabled && current->update) {
            current->update();
        }
        current = current->next;
    }
}

// Cleanup all addons (called during shutdown)
void menu_addon_cleanup_all(void) {
    MenuAddon *current = registered_addons;
    while (current) {
        MenuAddon *next = current->next;

        // Call cleanup callback
        if (current->cleanup) {
            current->cleanup();
        }

        // Free addon structure
        free(current);
        current = next;
    }

    registered_addons = NULL;
    // All addons cleaned up successfully (silent)
}

// ============================================================================
// Configuration Loading
// ============================================================================

// Load enabled addons from amiwbrc configuration
// Format in amiwbrc: MenuAddons=clock,cpu,ram,network
// Default: No addons enabled (empty menubar in logo mode)
void menu_addon_load_config(void) {
    const AmiwbConfig *config = get_config();
    const char *addon_list = config->menu_addons;

    // If no addons configured, leave all disabled (default behavior)
    if (!addon_list || !addon_list[0]) {
        return;
    }

    // Parse comma-separated list "clock,cpu,ram"
    // Make a copy since strtok_r modifies the string
    char list_copy[NAME_SIZE];
    strncpy(list_copy, addon_list, sizeof(list_copy) - 1);
    list_copy[sizeof(list_copy) - 1] = '\0';

    char *saveptr;
    char *addon_name = strtok_r(list_copy, ",", &saveptr);
    int order = 0;  // Track order in config file

    while (addon_name) {
        // Trim leading whitespace
        while (*addon_name == ' ' || *addon_name == '\t') {
            addon_name++;
        }

        // Trim trailing whitespace
        char *end = addon_name + strlen(addon_name) - 1;
        while (end > addon_name && (*end == ' ' || *end == '\t')) {
            *end = '\0';
            end--;
        }

        // Find and enable matching addon
        if (addon_name[0]) {  // Skip empty strings
            MenuAddon *current = registered_addons;
            while (current) {
                if (strcmp(current->name, addon_name) == 0) {
                    current->enabled = true;
                    current->config_order = order++;  // Preserve config order
                    break;
                }
                current = current->next;
            }
        }

        addon_name = strtok_r(NULL, ",", &saveptr);
    }
}
