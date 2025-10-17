// Menu System - Fan Monitoring Addon
// Displays fan RPM in menubar (logo mode only)

#include "../menu_internal.h"
#include "../../font_manager.h"
#include "../../render/rnd_public.h"
#include "../../config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// ============================================================================
// Fan State
// ============================================================================

static char cached_text[NAME_SIZE] = {0};  // Cache formatted text "fans 155 RPM"
static int max_rpm = 0;                    // Maximum RPM among all fans
static int reserved_width = 0;             // Reserved width for maximum text (prevents shifting)

// ============================================================================
// Fan RPM Reading
// ============================================================================

// Read fan RPM directly from sysfs (no process fork - fast!)
static void update_fan_rpm(void) {
    int highest_rpm = 0;
    bool found_any = false;

    // Read all hwmon fan inputs: /sys/class/hwmon/hwmon*/fan*_input
    for (int hwmon = 0; hwmon < 10; hwmon++) {  // Usually hwmon0-hwmon5
        for (int fan = 1; fan <= 9; fan++) {  // fan1_input to fan9_input
            char path[256];
            snprintf(path, sizeof(path), "/sys/class/hwmon/hwmon%d/fan%d_input", hwmon, fan);

            FILE *fp = fopen(path, "r");
            if (!fp) continue;  // File doesn't exist, try next

            int rpm = 0;
            if (fscanf(fp, "%d", &rpm) == 1) {
                found_any = true;
                if (rpm > highest_rpm) {
                    highest_rpm = rpm;
                }
            }
            fclose(fp);
        }
    }

    if (!found_any) {
        snprintf(cached_text, sizeof(cached_text), "Fans: N/A");
        return;
    }

    // Store for rendering
    max_rpm = highest_rpm;

    // Format display text (show biggest RPM, or 0 if all fans are stopped)
    snprintf(cached_text, sizeof(cached_text), "Fans: %d RPM", max_rpm);
}

// ============================================================================
// Fan Rendering
// ============================================================================

// Render fan RPM on menubar
static void fans_render(RenderContext *ctx, Canvas *menubar, int *x, int y) {
    if (!ctx || !menubar || !x) return;

    // Render cached text at current position
    menu_render_text(ctx, menubar, cached_text, *x, y);

    // Update x for next addon: use fixed maximum width + spacing (prevents shifting)
    *x += reserved_width + 40;
}

// ============================================================================
// Fan Update
// ============================================================================

// Update callback - called periodically (every 2 seconds)
static void fans_update(void) {
    update_fan_rpm();
}

// ============================================================================
// Fan Cleanup
// ============================================================================

// Cleanup callback - called during shutdown
static void fans_cleanup(void) {
    // Nothing to cleanup
}

// ============================================================================
// Fan Registration
// ============================================================================

// Initialize and register fans addon
void menuaddon_fans_init(void) {
    // Allocate addon structure
    MenuAddon *fans_addon = calloc(1, sizeof(MenuAddon));
    if (!fans_addon) {
        log_error("[ERROR] Failed to allocate fans addon - continuing without fan monitor");
        return;  // Gracefully fail - menubar works without fan monitor
    }

    // Initial update to populate cached_text
    update_fan_rpm();

    // Calculate maximum text width (worst case: "Fans: 9999 RPM")
    RenderContext *ctx = get_render_context();
    if (ctx) {
        reserved_width = menu_measure_text(ctx, "Fans: 9999 RPM");
    } else {
        reserved_width = 130;  // Fallback if context not available yet
    }

    // Configure addon
    snprintf(fans_addon->name, sizeof(fans_addon->name), "fans");
    fans_addon->position = ADDON_POS_MIDDLE;    // Center of menubar
    fans_addon->width = reserved_width;         // Use calculated maximum width
    fans_addon->render = fans_render;
    fans_addon->update = fans_update;
    fans_addon->cleanup = fans_cleanup;
    fans_addon->enabled = false;                // Will be enabled by config loader
    fans_addon->config_order = -1;              // Will be set by config loader

    // Register with addon system
    menu_addon_register(fans_addon);
}
