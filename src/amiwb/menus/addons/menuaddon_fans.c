// Menu System - Fan Monitoring Addon
// Displays fan RPM in menubar (logo mode only)

#include "../menu_internal.h"
#include "../../font_manager.h"
#include "../../render.h"
#include "../../config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// ============================================================================
// Fan State
// ============================================================================

static char cached_text[NAME_SIZE] = {0};  // Cache formatted text "fans 155 RPM"
static int max_rpm = 0;                    // Maximum RPM among all fans

// ============================================================================
// Fan RPM Reading
// ============================================================================

// Run sensors command and parse fan RPM values
static void update_fan_rpm(void) {
    // Run: sensors | grep -E "cpu_fan|gpu_fan|mid_fan"
    FILE *fp = popen("sensors 2>/dev/null | grep -E 'cpu_fan|gpu_fan|mid_fan'", "r");
    if (!fp) {
        snprintf(cached_text, sizeof(cached_text), "Fans: N/A");
        return;
    }

    char line[256];
    int highest_rpm = 0;
    bool found_any = false;

    // Parse each line: "cpu_fan:        0 RPM"
    while (fgets(line, sizeof(line), fp)) {
        int rpm = 0;
        // Try to extract RPM value
        if (sscanf(line, "%*[^:]: %d RPM", &rpm) == 1) {
            found_any = true;
            if (rpm > highest_rpm) {
                highest_rpm = rpm;
            }
        }
    }
    pclose(fp);

    if (!found_any) {
        snprintf(cached_text, sizeof(cached_text), "Fans: N/A");
        return;
    }

    // Store for rendering
    max_rpm = highest_rpm;

    // Format display text
    snprintf(cached_text, sizeof(cached_text), "Fans: %d RPM", max_rpm);
}

// ============================================================================
// Fan Rendering
// ============================================================================

// Render fan RPM on menubar
static void fans_render(RenderContext *ctx, Canvas *menubar, int *x, int y) {
    if (!ctx || !menubar || !x) return;

    // Render cached text at provided x position (text left-aligned in reserved space)
    menu_render_text(ctx, menubar, cached_text, *x, y);

    // Update x for next addon: use fixed width (130px) to prevent shifting
    *x += 130 + 40;  // Fixed width + spacing
}

// ============================================================================
// Fan Update
// ============================================================================

// Update callback - called periodically (every 1 second)
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
        return;
    }

    // Initial update to populate cached_text
    update_fan_rpm();

    // Configure addon
    snprintf(fans_addon->name, sizeof(fans_addon->name), "fans");
    fans_addon->position = ADDON_POS_MIDDLE;    // Center of menubar
    fans_addon->width = 130;                    // Approximate width for "fans 9999 RPM"
    fans_addon->render = fans_render;
    fans_addon->update = fans_update;
    fans_addon->cleanup = fans_cleanup;
    fans_addon->enabled = false;                // Will be enabled by config loader
    fans_addon->config_order = -1;              // Will be set by config loader

    // Register with addon system
    menu_addon_register(fans_addon);
}
