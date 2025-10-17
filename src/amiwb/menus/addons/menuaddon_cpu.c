// Menu System - CPU Monitoring Addon
// Displays global CPU usage in menubar (logo mode only)

#include "../menu_internal.h"
#include "../../font_manager.h"
#include "../../render/rnd_public.h"
#include "../../config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// ============================================================================
// CPU State
// ============================================================================

static char cached_text[NAME_SIZE] = {0};  // Cache formatted text "cpu 5% Use"
static unsigned long long prev_total = 0;
static unsigned long long prev_idle = 0;
static int current_usage = 0;              // CPU usage percentage
static int reserved_width = 0;             // Reserved width for maximum text (prevents shifting)

// ============================================================================
// CPU Usage Calculation
// ============================================================================

// Read /proc/stat and calculate CPU usage percentage
static void update_cpu_usage(void) {
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) {
        current_usage = 0;
        return;
    }

    char line[256];
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        current_usage = 0;
        return;
    }
    fclose(fp);

    // Parse: cpu  user nice system idle iowait irq softirq steal guest guest_nice
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
    int parsed = sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                        &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal);

    if (parsed < 4) {
        current_usage = 0;
        return;
    }

    // Calculate totals
    unsigned long long total = user + nice + system + idle + iowait + irq + softirq + steal;

    // First call - just store values
    if (prev_total == 0) {
        prev_total = total;
        prev_idle = idle;
        current_usage = 0;
        return;
    }

    // Calculate deltas
    unsigned long long total_delta = total - prev_total;
    unsigned long long idle_delta = idle - prev_idle;

    // Calculate usage percentage
    if (total_delta > 0) {
        current_usage = (int)(((total_delta - idle_delta) * 100) / total_delta);
    } else {
        current_usage = 0;
    }

    // Store for next calculation
    prev_total = total;
    prev_idle = idle;
}

// ============================================================================
// CPU Rendering
// ============================================================================

// Render CPU usage on menubar
static void cpu_render(RenderContext *ctx, Canvas *menubar, int *x, int y) {
    if (!ctx || !menubar || !x) return;

    // Render cached text at current position
    menu_render_text(ctx, menubar, cached_text, *x, y);

    // Update x for next addon: use fixed maximum width + spacing (prevents shifting)
    *x += reserved_width + 40;
}

// ============================================================================
// CPU Update
// ============================================================================

// Update callback - called periodically (every 2 seconds)
static void cpu_update(void) {
    // Update CPU usage calculation
    update_cpu_usage();

    // Format display text
    snprintf(cached_text, sizeof(cached_text), "CPU: %d%% Use", current_usage);
}

// ============================================================================
// CPU Cleanup
// ============================================================================

// Cleanup callback - called during shutdown
static void cpu_cleanup(void) {
    // Nothing to cleanup
}

// ============================================================================
// CPU Registration
// ============================================================================

// Initialize and register CPU addon
void menuaddon_cpu_init(void) {
    // Allocate addon structure
    MenuAddon *cpu_addon = calloc(1, sizeof(MenuAddon));
    if (!cpu_addon) {
        log_error("[ERROR] Failed to allocate CPU addon - continuing without CPU monitor");
        return;  // Gracefully fail - menubar works without CPU monitor
    }

    // Initial update stores baseline values (first call returns 0%)
    update_cpu_usage();
    snprintf(cached_text, sizeof(cached_text), "CPU: %d%% Use", current_usage);

    // Calculate maximum text width (worst case: "CPU: 100% Use")
    RenderContext *ctx = get_render_context();
    if (ctx) {
        reserved_width = menu_measure_text(ctx, "CPU: 100% Use");
    } else {
        reserved_width = 130;  // Fallback if context not available yet
    }

    // Configure addon
    snprintf(cpu_addon->name, sizeof(cpu_addon->name), "cpu");
    cpu_addon->position = ADDON_POS_MIDDLE;     // Center of menubar
    cpu_addon->width = reserved_width;          // Use calculated maximum width
    cpu_addon->render = cpu_render;
    cpu_addon->update = cpu_update;
    cpu_addon->cleanup = cpu_cleanup;
    cpu_addon->enabled = false;                 // Will be enabled by config loader
    cpu_addon->config_order = -1;               // Will be set by config loader

    // Register with addon system
    menu_addon_register(cpu_addon);
}
