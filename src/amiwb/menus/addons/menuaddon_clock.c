// Menu System - Clock Addon
// Displays current date and time in menubar (logo mode only)

#include "../menu_internal.h"
#include "../../font_manager.h"
#include "../../render.h"
#include "../../config.h"
#include <time.h>
#include <string.h>
#include <stdlib.h>

// ============================================================================
// Clock State
// ============================================================================

static char cached_time[NAME_SIZE] = {0};  // Cache formatted time string (NAME_SIZE=128)
static int cached_width = 0;                // Cache text width in pixels (updated with cached_time)
static time_t last_update = 0;              // Last time we updated the cache

// ============================================================================
// Clock Rendering
// ============================================================================

// Render clock on menubar
// Called during menubar rendering in logo mode
// Coordinator positions us, we render at *x and update it
static void clock_render(RenderContext *ctx, Canvas *menubar, int *x, int y) {
    if (!ctx || !menubar || !x) return;

#if !MENU_SHOW_DATE
    // Clock disabled in config - do nothing
    return;
#endif

    // Render text right-aligned using cached width (position text so it ends at *x)
    int text_x = *x - cached_width;
    menu_render_text(ctx, menubar, cached_time, text_x, y);

    // Update x for next addon: move left by cached width + spacing
    *x -= cached_width + 40;
}

// ============================================================================
// Clock Update
// ============================================================================

// Update callback - called periodically (every 1 second)
// Updates cached text and width when minute changes
static void clock_update(void) {
    time_t now;
    time(&now);

    // Check if minute changed or first call
    if (now - last_update >= 60 || cached_time[0] == '\0') {
        struct tm *tm_info = localtime(&now);
        strftime(cached_time, sizeof(cached_time), MENUBAR_DATE_FORMAT, tm_info);
        last_update = now;

        // Recalculate width only when text changes
        RenderContext *ctx = get_render_context();
        if (ctx) {
            cached_width = menu_measure_text(ctx, cached_time);
        }
    }
}

// ============================================================================
// Clock Cleanup
// ============================================================================

// Cleanup callback - called during shutdown
static void clock_cleanup(void) {
    // Nothing to cleanup for clock addon
    // Time formatting doesn't allocate resources
}

// ============================================================================
// Clock Registration
// ============================================================================

// Initialize and register clock addon
// Called from init_menus() in menu_core.c
void menuaddon_clock_init(void) {
    // Allocate addon structure
    MenuAddon *clock_addon = calloc(1, sizeof(MenuAddon));
    if (!clock_addon) {
        log_error("[ERROR] Failed to allocate clock addon - continuing without clock");
        return;  // Gracefully fail - menubar works without clock
    }

    // Initialize cached time and width
    time_t now;
    time(&now);
    struct tm *tm_info = localtime(&now);
    strftime(cached_time, sizeof(cached_time), MENUBAR_DATE_FORMAT, tm_info);
    last_update = now;

    // Calculate initial width
    RenderContext *ctx = get_render_context();
    if (ctx) {
        cached_width = menu_measure_text(ctx, cached_time);
    } else {
        cached_width = 120;  // Fallback width if context not available yet
    }

    // Configure addon
    snprintf(clock_addon->name, sizeof(clock_addon->name), "clock");
    clock_addon->position = ADDON_POS_RIGHT;    // Always on right side
    clock_addon->width = 180;                   // Reserved width (not used for actual positioning)
    clock_addon->render = clock_render;
    clock_addon->update = clock_update;
    clock_addon->cleanup = clock_cleanup;
    clock_addon->enabled = false;               // Will be enabled by config loader
    clock_addon->config_order = -1;             // Will be set by config loader

    // Register with addon system
    menu_addon_register(clock_addon);
}
