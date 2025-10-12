// File: itn_composite_stack.c
// Window Stacking Cache - eliminates XQueryTree from render hot path
// Event-driven cache updated only when stacking order changes

#include "itn_internal.h"
#include "../config.h"
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>

// ============================================================================
// Module-Private State
// ============================================================================

static Window *g_cached_stack = NULL;      // Cached window stacking order
static int g_cached_stack_count = 0;       // Number of windows in cache
static int g_cached_stack_capacity = 0;    // Allocated capacity
static bool g_stack_dirty = true;          // Cache needs update

// ============================================================================
// Internal Implementation
// ============================================================================

// Update cached stacking order from X server
// This is the ONLY place that calls XQueryTree - not in render loop!
static void update_stack_cache(Display *dpy, Window root) {
    Window root_ret, parent_ret, *children = NULL;
    unsigned int nchildren = 0;

    if (!XQueryTree(dpy, root, &root_ret, &parent_ret, &children, &nchildren)) {
        log_error("[ERROR] XQueryTree failed in stack cache update");
        return;
    }

    // Resize cache if needed
    if ((int)nchildren > g_cached_stack_capacity) {
        int new_capacity = nchildren * 2;  // Grow with headroom
        Window *new_stack = realloc(g_cached_stack, new_capacity * sizeof(Window));
        if (!new_stack) {
            log_error("[ERROR] Failed to grow stack cache to %d entries", new_capacity);
            XFree(children);
            return;
        }
        g_cached_stack = new_stack;
        g_cached_stack_capacity = new_capacity;
    }

    // Copy stacking order to cache
    memcpy(g_cached_stack, children, nchildren * sizeof(Window));
    g_cached_stack_count = nchildren;
    g_stack_dirty = false;

    XFree(children);
}

// ============================================================================
// Public API Implementation
// ============================================================================

// Mark stack cache as dirty (needs update)
// Called from event handlers when stacking order changes
void itn_stack_mark_dirty(void) {
    g_stack_dirty = true;
}

// Get cached stacking order (updates if dirty)
// This is what compositor uses instead of XQueryTree
Window *itn_stack_get_cached(Display *dpy, Window root, int *out_count) {
    if (!out_count) {
        log_error("[ERROR] itn_stack_get_cached called with NULL out_count");
        return NULL;
    }

    // Update cache if dirty or uninitialized
    if (g_stack_dirty || !g_cached_stack) {
        update_stack_cache(dpy, root);
    }

    *out_count = g_cached_stack_count;
    return g_cached_stack;
}

// Initialize stacking cache
void itn_stack_init(void) {
    g_cached_stack = NULL;
    g_cached_stack_count = 0;
    g_cached_stack_capacity = 0;
    g_stack_dirty = true;  // Force initial update
}

// Cleanup stacking cache
void itn_stack_cleanup(void) {
    if (g_cached_stack) {
        free(g_cached_stack);
        g_cached_stack = NULL;
    }
    g_cached_stack_count = 0;
    g_cached_stack_capacity = 0;
    g_stack_dirty = true;
}
