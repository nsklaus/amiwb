// File: itn_composite_attrs.c
// Window Attributes Cache - eliminates safe_get_window_attributes from render hot path
// Batch queries instead of per-window XSync

#include "itn_internal.h"
#include "../config.h"
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>

// ============================================================================
// Module-Private State
// ============================================================================

typedef struct {
    Window win;
    XWindowAttributes attrs;
    bool valid;
    bool occupied;  // Hash table slot is occupied
} AttrHashEntry;

static AttrHashEntry *g_attr_hash_table = NULL;  // Hash table for O(1) lookup
static int g_attr_hash_capacity = 0;             // Power-of-2 capacity
static int g_attr_hash_count = 0;                // Number of entries

// ============================================================================
// Internal Implementation
// ============================================================================

// Hash function: Simple and fast for Window IDs (power-of-2 modulo using bitwise AND)
static inline int hash_window(Window win, int capacity) {
    return (int)(win & (capacity - 1));
}

// Find slot for window in hash table (insert or lookup)
// Returns index where window is found or should be inserted
static int find_slot(Window win) {
    if (g_attr_hash_capacity == 0) return -1;

    int index = hash_window(win, g_attr_hash_capacity);
    int start = index;

    // Linear probing: search until we find the window or an empty slot
    do {
        if (!g_attr_hash_table[index].occupied || g_attr_hash_table[index].win == win) {
            return index;
        }
        index = (index + 1) & (g_attr_hash_capacity - 1);  // Wrap around using bitwise AND
    } while (index != start);

    return -1;  // Table full (should never happen with proper load factor)
}

// Clear hash table (reset all occupied flags)
static void clear_hash_table(void) {
    if (!g_attr_hash_table) return;
    memset(g_attr_hash_table, 0, g_attr_hash_capacity * sizeof(AttrHashEntry));
    g_attr_hash_count = 0;
}

// Batch update all window attributes
// This is the ONLY place that queries attributes - not per-window in render loop!
static void batch_update_attributes(Display *dpy, Window *windows, int count) {
    if (!dpy || !windows || count <= 0) return;

    // Calculate required capacity (next power of 2, maintaining load factor < 0.75)
    int required_capacity = count * 2;  // Load factor 0.5 for good performance
    int new_capacity = 16;  // Minimum size
    while (new_capacity < required_capacity) {
        new_capacity *= 2;
    }

    // Grow hash table if needed
    if (new_capacity > g_attr_hash_capacity) {
        AttrHashEntry *new_table = realloc(g_attr_hash_table, new_capacity * sizeof(AttrHashEntry));
        if (!new_table) {
            log_error("[ERROR] Failed to grow hash table to %d entries", new_capacity);
            return;
        }
        g_attr_hash_table = new_table;
        g_attr_hash_capacity = new_capacity;
    }

    // Clear hash table for fresh batch (simpler than tombstone management)
    clear_hash_table();

    // Insert all windows into hash table with their attributes
    for (int i = 0; i < count; i++) {
        int slot = find_slot(windows[i]);
        if (slot < 0) continue;  // Table full (shouldn't happen)

        g_attr_hash_table[slot].win = windows[i];
        g_attr_hash_table[slot].valid = XGetWindowAttributes(dpy, windows[i], &g_attr_hash_table[slot].attrs);
        g_attr_hash_table[slot].occupied = true;
        g_attr_hash_count++;
    }

    // ONE XFlush at end instead of XSync per window
    // This is the key optimization: async batch instead of sync per-window
    XFlush(dpy);
}

// ============================================================================
// Public API Implementation
// ============================================================================

// Batch update attributes for given window list
// Call this BEFORE render loop with all windows to query
void itn_attrs_batch_update(Display *dpy, Window *windows, int count) {
    batch_update_attributes(dpy, windows, count);
}

// Get cached attributes for specific window
// Returns cached attrs and sets out_valid flag
// O(1) hash table lookup instead of O(n) linear search
XWindowAttributes *itn_attrs_get(Window win, bool *out_valid) {
    if (!out_valid) {
        log_error("[ERROR] itn_attrs_get called with NULL out_valid");
        return NULL;
    }

    // O(1) hash table lookup
    int slot = find_slot(win);
    if (slot >= 0 && g_attr_hash_table[slot].occupied && g_attr_hash_table[slot].win == win) {
        *out_valid = g_attr_hash_table[slot].valid;
        return &g_attr_hash_table[slot].attrs;
    }

    // Not found in cache
    *out_valid = false;
    return NULL;
}

// Initialize hash table
void itn_attrs_init(void) {
    g_attr_hash_table = NULL;
    g_attr_hash_capacity = 0;
    g_attr_hash_count = 0;
}

// Cleanup hash table
void itn_attrs_cleanup(void) {
    if (g_attr_hash_table) {
        free(g_attr_hash_table);
        g_attr_hash_table = NULL;
    }
    g_attr_hash_capacity = 0;
    g_attr_hash_count = 0;
}
