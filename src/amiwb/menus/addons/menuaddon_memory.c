// Menu System - Memory Monitoring Addon
// Displays global memory usage in menubar (logo mode only)

#include "../menu_internal.h"
#include "../../font_manager.h"
#include "../../render/rnd_public.h"
#include "../../config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// ============================================================================
// Memory State
// ============================================================================

static char cached_text[NAME_SIZE] = {0};  // Cache formatted text "mem 12Gb Free"
static int reserved_width = 0;             // Reserved width for maximum text (prevents shifting)

// ============================================================================
// Memory Usage Calculation
// ============================================================================

// Read /proc/meminfo and calculate available memory
static void update_memory_usage(void) {
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) {
        snprintf(cached_text, sizeof(cached_text), "Mem: N/A");
        return;
    }

    char line[256];
    unsigned long mem_available = 0;
    unsigned long mem_free = 0;
    unsigned long buffers = 0;
    unsigned long cached = 0;
    bool has_available = false;

    // Parse /proc/meminfo
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "MemAvailable: %lu kB", &mem_available) == 1) {
            has_available = true;
        } else if (sscanf(line, "MemFree: %lu kB", &mem_free) == 1) {
            // Got MemFree
        } else if (sscanf(line, "Buffers: %lu kB", &buffers) == 1) {
            // Got Buffers
        } else if (sscanf(line, "Cached: %lu kB", &cached) == 1) {
            // Got Cached
        }
    }
    fclose(fp);

    // Calculate available memory
    unsigned long available_kb;
    if (has_available) {
        // Use MemAvailable if available (more accurate)
        available_kb = mem_available;
    } else {
        // Fallback: MemFree + Buffers + Cached
        available_kb = mem_free + buffers + cached;
    }

    // Convert to GB (with 1 decimal precision)
    double available_gb = available_kb / (1024.0 * 1024.0);

    // Format display text
    snprintf(cached_text, sizeof(cached_text), "Mem: %.1fGb Free", available_gb);
}

// ============================================================================
// Memory Rendering
// ============================================================================

// Render memory usage on menubar
static void memory_render(RenderContext *ctx, Canvas *menubar, int *x, int y) {
    if (!ctx || !menubar || !x) return;

    // Render cached text at current position
    menu_render_text(ctx, menubar, cached_text, *x, y);

    // Update x for next addon: use fixed maximum width + spacing (prevents shifting)
    *x += reserved_width + 40;
}

// ============================================================================
// Memory Update
// ============================================================================

// Update callback - called periodically (every 2 seconds)
static void memory_update(void) {
    update_memory_usage();
}

// ============================================================================
// Memory Cleanup
// ============================================================================

// Cleanup callback - called during shutdown
static void memory_cleanup(void) {
    // Nothing to cleanup
}

// ============================================================================
// Memory Registration
// ============================================================================

// Initialize and register memory addon
void menuaddon_memory_init(void) {
    // Allocate addon structure
    MenuAddon *memory_addon = calloc(1, sizeof(MenuAddon));
    if (!memory_addon) {
        log_error("[ERROR] Failed to allocate memory addon - continuing without memory monitor");
        return;  // Gracefully fail - menubar works without memory monitor
    }

    // Initial update to populate cached_text
    update_memory_usage();

    // Calculate maximum text width based on actual system RAM
    // Read MemTotal from /proc/meminfo to determine realistic maximum
    FILE *fp = fopen("/proc/meminfo", "r");
    unsigned long mem_total_kb = 0;
    if (fp) {
        char line[256];
        if (fgets(line, sizeof(line), fp)) {
            sscanf(line, "MemTotal: %lu kB", &mem_total_kb);
        }
        fclose(fp);
    }

    // Convert to GB and build maximum display string (with space before unit)
    double total_gb = mem_total_kb / (1024.0 * 1024.0);
    char max_text[NAME_SIZE];
    snprintf(max_text, sizeof(max_text), "Mem: %.1f Gb Free", total_gb);

    // Measure maximum width
    RenderContext *ctx = get_render_context();
    if (ctx) {
        reserved_width = menu_measure_text(ctx, max_text);
    } else {
        reserved_width = 150;  // Fallback if context not available yet
    }

    // Configure addon
    snprintf(memory_addon->name, sizeof(memory_addon->name), "memory");
    memory_addon->position = ADDON_POS_MIDDLE;  // Center of menubar
    memory_addon->width = reserved_width;       // Use calculated maximum width
    memory_addon->render = memory_render;
    memory_addon->update = memory_update;
    memory_addon->cleanup = memory_cleanup;
    memory_addon->enabled = false;              // Will be enabled by config loader
    memory_addon->config_order = -1;            // Will be set by config loader

    // Register with addon system
    menu_addon_register(memory_addon);
}
