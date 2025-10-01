// Menu System - Memory Monitoring Addon
// Displays global memory usage in menubar (logo mode only)

#include "../menu_internal.h"
#include "../../font_manager.h"
#include "../../render.h"
#include "../../config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// ============================================================================
// Memory State
// ============================================================================

static char cached_text[NAME_SIZE] = {0};  // Cache formatted text "mem 12Gb Free"

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

    // Render cached text at provided x position (text left-aligned in reserved space)
    menu_render_text(ctx, menubar, cached_text, *x, y);

    // Update x for next addon: use fixed width (140px) to prevent shifting
    *x += 140 + 40;  // Fixed width + spacing
}

// ============================================================================
// Memory Update
// ============================================================================

// Update callback - called periodically (every 1 second)
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
        return;
    }

    // Initial update to populate cached_text
    update_memory_usage();

    // Configure addon
    snprintf(memory_addon->name, sizeof(memory_addon->name), "memory");
    memory_addon->position = ADDON_POS_MIDDLE;  // Center of menubar
    memory_addon->width = 140;                  // Approximate width for "mem 99.9Gb Free"
    memory_addon->render = memory_render;
    memory_addon->update = memory_update;
    memory_addon->cleanup = memory_cleanup;
    memory_addon->enabled = false;              // Will be enabled by config loader
    memory_addon->config_order = -1;            // Will be set by config loader

    // Register with addon system
    menu_addon_register(memory_addon);
}
