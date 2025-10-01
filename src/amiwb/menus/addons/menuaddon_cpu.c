// Menu System - CPU Monitoring Addon
// Displays global CPU usage in menubar (logo mode only)

#include "../menu_internal.h"
#include "../../font_manager.h"
#include "../../render.h"
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

    // Render cached text at provided x position (text left-aligned in reserved space)
    menu_render_text(ctx, menubar, cached_text, *x, y);

    // Update x for next addon: use fixed width (120px) to prevent shifting
    *x += 120 + 40;  // Fixed width + spacing
}

// ============================================================================
// CPU Update
// ============================================================================

// Update callback - called periodically (every 1 second)
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
        return;
    }

    // Initial update stores baseline values (first call returns 0%)
    update_cpu_usage();
    snprintf(cached_text, sizeof(cached_text), "CPU: %d%% Use", current_usage);

    // Configure addon
    snprintf(cpu_addon->name, sizeof(cpu_addon->name), "cpu");
    cpu_addon->position = ADDON_POS_MIDDLE;     // Center of menubar
    cpu_addon->width = 120;                     // Approximate width for "cpu 99% Use"
    cpu_addon->render = cpu_render;
    cpu_addon->update = cpu_update;
    cpu_addon->cleanup = cpu_cleanup;
    cpu_addon->enabled = false;                 // Will be enabled by config loader
    cpu_addon->config_order = -1;               // Will be set by config loader

    // Register with addon system
    menu_addon_register(cpu_addon);
}
