/*
 * window resize module
 * 
 * Core principles:
 * 1. Minimize X protocol calls
 * 2. Use motion event compression  
 * 3. Smart buffer management (create once, reuse)
 */

#include "resize.h"
#include "intuition.h"
#include "render.h"
#include "workbench.h" // For icon_cleanup and compute_max_scroll
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdbool.h>

// Simple max function
static inline int max(int a, int b) {
    return (a > b) ? a : b;
}

// Simple resize state - just what we need, nothing more
typedef struct {
    Canvas *canvas;           // Window being resized
    int start_x, start_y;     // Mouse position when resize started
    int start_width, start_height; // Window size when resize started
    bool active;              // Are we currently resizing?
    
    // Motion compression - ignore rapid mouse moves
    struct timespec last_update;
    int min_interval_ms;      // Minimum milliseconds between updates
} ResizeState;

// Global resize state - simple and clear
static ResizeState g_resize = {0};

/*
 * Check if enough time has passed since last resize update
 * This compresses motion events to avoid X protocol flooding
 */
static bool should_update_resize(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    
    long elapsed_ms = (now.tv_sec - g_resize.last_update.tv_sec) * 1000 + 
                     (now.tv_nsec - g_resize.last_update.tv_nsec) / 1000000;
    
    return elapsed_ms >= g_resize.min_interval_ms;
}

/*
 * Smart buffer management for resize
 * Create buffers large enough to handle most resize operations
 * without recreation, but not wastefully large
 */
static void create_initial_resize_buffers(Canvas *canvas, int start_width, int start_height) {
    // Use modest buffer padding - allow 1.3x growth for efficiency
    // This reduces memory usage while still providing smooth resize
    int buffer_width = start_width + (start_width * 3 / 10);   // 1.3x width
    int buffer_height = start_height + (start_height * 3 / 10); // 1.3x height
    
    // Ensure minimum reasonable buffer size
    if (buffer_width < start_width + 100) buffer_width = start_width + 100;
    if (buffer_height < start_height + 100) buffer_height = start_height + 100;
    
    // printf("[RESIZE] Creating smart buffers %dx%d (window: %dx%d)\n", 
    //        buffer_width, buffer_height, start_width, start_height);
    
    // Set buffer dimensions
    canvas->buffer_width = buffer_width;
    canvas->buffer_height = buffer_height;
    
    // Let render system recreate the actual XRender surfaces ONCE
    render_recreate_canvas_surfaces(canvas);
}

/*
 * Start a resize operation
 * Create fixed-size buffers ONCE, then never recreate during resize
 */
void resize_begin(Canvas *canvas, int mouse_x, int mouse_y) {
    if (!canvas) return;
    
    printf("DEBUG: resize_begin called for canvas type=%d\n", canvas->type);
    
    // Simple state setup
    g_resize.canvas = canvas;
    g_resize.start_x = mouse_x;
    g_resize.start_y = mouse_y;
    g_resize.start_width = canvas->width;
    g_resize.start_height = canvas->height;
    g_resize.active = true;
    g_resize.min_interval_ms = 16; // ~60 FPS maximum update rate
    
    // Mark canvas as being resized
    canvas->resizing_interactive = true;
    
    // Create fixed-size buffers ONCE - never recreate during resize
    create_initial_resize_buffers(canvas, canvas->width, canvas->height);
    
    // Record start time
    clock_gettime(CLOCK_MONOTONIC, &g_resize.last_update);
}

/*
 * Handle mouse motion during resize
 * FIXED: No buffer recreation ever during motion
 */
void resize_motion(int mouse_x, int mouse_y) {
    if (!g_resize.active || !g_resize.canvas) return;
    
    // Motion compression: skip if too soon since last update
    if (!should_update_resize()) {
        return;
    }
    
    // Calculate new size based on mouse movement
    int delta_x = mouse_x - g_resize.start_x;
    int delta_y = mouse_y - g_resize.start_y;
    
    int new_width = g_resize.start_width + delta_x;
    int new_height = g_resize.start_height + delta_y;
    
    // Enforce minimum window size
    const int min_size = 150;
    if (new_width < min_size) new_width = min_size;
    if (new_height < min_size) new_height = min_size;
    
    // Dynamic buffer growth: recreate buffer if user resizes significantly beyond current buffer
    bool need_buffer_growth = false;
    if (new_width > g_resize.canvas->buffer_width || new_height > g_resize.canvas->buffer_height) {
        // Only grow buffer if resize is significant (20+ pixels beyond current buffer)
        if (new_width > g_resize.canvas->buffer_width + 20 || 
            new_height > g_resize.canvas->buffer_height + 20) {
            need_buffer_growth = true;
            // printf("[RESIZE] Buffer growth needed: %dx%d exceeds %dx%d\n", 
            //        new_width, new_height, g_resize.canvas->buffer_width, g_resize.canvas->buffer_height);
            
            // Update buffer dimensions for new larger size
            g_resize.canvas->buffer_width = new_width + 100;  // Add padding
            g_resize.canvas->buffer_height = new_height + 100;
        } else {
            // Small overshoot - clamp to current buffer to avoid constant recreations
            if (new_width > g_resize.canvas->buffer_width) {
                new_width = g_resize.canvas->buffer_width;
            }
            if (new_height > g_resize.canvas->buffer_height) {
                new_height = g_resize.canvas->buffer_height;
            }
        }
    }
    
    // Skip tiny changes to reduce X protocol noise
    const int min_change = 5; // Increased to reduce motion noise
    if (abs(new_width - g_resize.canvas->width) < min_change && 
        abs(new_height - g_resize.canvas->height) < min_change) {
        return;
    }
    
    // printf("[RESIZE] %dx%d -> %dx%d (buffer: %dx%d)\n", 
    //        g_resize.canvas->width, g_resize.canvas->height, new_width, new_height,
    //        g_resize.canvas->buffer_width, g_resize.canvas->buffer_height);
    
    // Update window size immediately for smooth visual feedback  
    XResizeWindow(get_display(), g_resize.canvas->win, new_width, new_height);
    
    // Update canvas dimensions
    g_resize.canvas->width = new_width;
    g_resize.canvas->height = new_height;
    
    // Also resize client window if this is a client frame
    if (g_resize.canvas->client_win != None) {
        // Calculate client content area (same logic as content_rect() in intuition.c)
        int client_width = max(1, new_width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT);
        int client_height = max(1, new_height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM);
        
        XWindowChanges client_changes = { .width = client_width, .height = client_height };
        XConfigureWindow(get_display(), g_resize.canvas->client_win, CWWidth | CWHeight, &client_changes);
        
    }
    
    // Recreate buffer if growth is needed (allows unlimited resize)
    if (need_buffer_growth) {
        // printf("[RESIZE] Growing buffer to %dx%d\n", 
        //        g_resize.canvas->buffer_width, g_resize.canvas->buffer_height);
        render_recreate_canvas_surfaces(g_resize.canvas);
    }
    
    // Update scroll limits for proper scrollbar rendering during resize
    compute_max_scroll(g_resize.canvas);
    
    // Redraw only this window (render.c will skip others during interactive resize)
    redraw_canvas(g_resize.canvas);
    
    // Update timestamp for motion compression
    clock_gettime(CLOCK_MONOTONIC, &g_resize.last_update);
}

/*
 * Finish resize operation
 * Clean up and do any final operations
 */
void resize_end(void) {
    if (!g_resize.active || !g_resize.canvas) return;
    
    // printf("[RESIZE] Finished resize at %dx%d\n", 
    //        g_resize.canvas->width, g_resize.canvas->height);
    
    // Mark resize as complete
    g_resize.canvas->resizing_interactive = false;
    
    // Final cleanup: recreate buffers at exact size to free excess memory
    g_resize.canvas->buffer_width = g_resize.canvas->width;
    g_resize.canvas->buffer_height = g_resize.canvas->height;
    render_recreate_canvas_surfaces(g_resize.canvas);
    
    // Final redraw with icon cleanup
    if (g_resize.canvas->type == WINDOW || g_resize.canvas->type == DESKTOP) {
        icon_cleanup(g_resize.canvas);
        compute_max_scroll(g_resize.canvas);
    }
    redraw_canvas(g_resize.canvas);
    
    // Clear state
    g_resize.active = false;
    g_resize.canvas = NULL;
    
}

/*
 * Check if we're currently resizing
 */
bool resize_is_active(void) {
    return g_resize.active;
}

/*
 * Get the canvas being resized (for render optimizations)
 */
Canvas* resize_get_canvas(void) {
    return g_resize.active ? g_resize.canvas : NULL;
}