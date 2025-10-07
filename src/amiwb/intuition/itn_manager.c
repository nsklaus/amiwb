// Canvas manager - centralized canvas collection management
// Encapsulates canvas storage implementation details following David Parnas principles

#include "itn_internal.h"
#include "../config.h"
#include <stdlib.h>
#include <stdbool.h>

// ============================================================================
// Module-Private State
// ============================================================================

static Canvas **g_canvas_array = NULL;
static int g_canvas_count = 0;
static int g_canvas_array_size = 0;

// ============================================================================
// Public API Implementation
// ============================================================================

// Get canvas at specific index (returns NULL if index invalid)
Canvas *itn_manager_get_canvas(int index) {
    if (index < 0 || index >= g_canvas_count) return NULL;
    return g_canvas_array[index];
}

// Get total number of canvases
int itn_manager_get_count(void) {
    return g_canvas_count;
}

// Get canvas array size (capacity)
int itn_manager_get_array_size(void) {
    return g_canvas_array_size;
}

// Get raw array pointer (transition helper - avoid in new code)
// This exists to minimize disruption during migration
// Prefer using iterators or get_canvas(index) in new code
Canvas **itn_manager_get_array(void) {
    return g_canvas_array;
}

// Add canvas to manager
// Returns true on success, false on allocation failure
bool itn_manager_add(Canvas *canvas) {
    if (!canvas) return false;

    // Grow array if needed
    if (g_canvas_count >= g_canvas_array_size) {
        int new_size = g_canvas_array_size == 0 ? 8 : g_canvas_array_size * 2;
        Canvas **new_array = realloc(g_canvas_array, new_size * sizeof(Canvas *));
        if (!new_array) {
            log_error("[ERROR] Failed to grow canvas array to %d entries", new_size);
            return false;
        }
        g_canvas_array = new_array;
        g_canvas_array_size = new_size;
    }

    // Add canvas to end of array
    g_canvas_array[g_canvas_count] = canvas;
    g_canvas_count++;
    return true;
}

// Remove canvas from manager
void itn_manager_remove(Canvas *canvas) {
    if (!canvas) return;

    // Find canvas in array
    for (int i = 0; i < g_canvas_count; i++) {
        if (g_canvas_array[i] == canvas) {
            // Shift remaining canvases down
            for (int j = i; j < g_canvas_count - 1; j++) {
                g_canvas_array[j] = g_canvas_array[j + 1];
            }
            g_canvas_count--;
            g_canvas_array[g_canvas_count] = NULL;
            return;
        }
    }
}

// Find first canvas matching predicate
// Predicate receives canvas and user context, returns true to select
Canvas *itn_manager_find_by_predicate(bool (*predicate)(Canvas*, void*), void *ctx) {
    if (!predicate) return NULL;

    for (int i = 0; i < g_canvas_count; i++) {
        if (g_canvas_array[i] && predicate(g_canvas_array[i], ctx)) {
            return g_canvas_array[i];
        }
    }
    return NULL;
}

// Iterate over all canvases with callback
// Callback receives canvas and user context
// Stops iteration if callback returns false
void itn_manager_foreach(void (*callback)(Canvas*, void*), void *ctx) {
    if (!callback) return;

    for (int i = 0; i < g_canvas_count; i++) {
        if (g_canvas_array[i]) {
            callback(g_canvas_array[i], ctx);
        }
    }
}

// Cleanup manager (call during shutdown)
void itn_manager_cleanup(void) {
    if (g_canvas_array) {
        free(g_canvas_array);
        g_canvas_array = NULL;
    }
    g_canvas_count = 0;
    g_canvas_array_size = 0;
}
