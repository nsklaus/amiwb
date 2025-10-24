// File: wb_spatial.c
// Spatial Window Geometry Management
// Implements true spatial file manager behavior using extended attributes

#include "wb_spatial.h"
#include "wb_internal.h"
#include "../intuition/itn_internal.h"
#include "../config.h"
#include <sys/xattr.h>
#include <string.h>
#include <errno.h>

// xattr name for storing window geometry
#define XATTR_WINDOW_GEOMETRY "user.window.geometry"

// Cascade defaults
#define CASCADE_START_X 100
#define CASCADE_START_Y 80
#define CASCADE_OFFSET 30
#define CASCADE_MAX 8  // Wrap after 8 windows

// Default window size
#define DEFAULT_WIDTH 400
#define DEFAULT_HEIGHT 300

// Binary format for geometry storage (16 bytes)
typedef struct {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} WindowGeometry;

// ============================================================================
// Cascade Algorithm (Fallback for New Directories)
// ============================================================================

static void cascade_position(int *x, int *y) {
    // Count existing WINDOW type canvases (exclude desktop, dialogs)
    int window_count = 0;
    int total = itn_manager_get_count();

    for (int i = 0; i < total; i++) {
        Canvas *c = itn_manager_get_canvas(i);
        if (c && c->type == WINDOW) {
            window_count++;
        }
    }

    // Calculate cascade offset with wrapping
    int cascade_index = window_count % CASCADE_MAX;
    *x = CASCADE_START_X + (cascade_index * CASCADE_OFFSET);
    *y = CASCADE_START_Y + (cascade_index * CASCADE_OFFSET);
}

// ============================================================================
// Public API Implementation
// ============================================================================

bool wb_spatial_load_geometry(const char *dir_path, int *x, int *y, int *width, int *height) {
    if (!dir_path || !x || !y || !width || !height) {
        return false;  // Caller must provide valid pointers
    }

    // Try to load from xattr
    WindowGeometry geom;
    ssize_t size = getxattr(dir_path, XATTR_WINDOW_GEOMETRY, &geom, sizeof(geom));

    if (size == sizeof(geom)) {
        // Valid geometry found - use it
        *x = geom.x;
        *y = geom.y;
        *width = geom.width;
        *height = geom.height;
        return true;
    }

    // No xattr or read failed - use cascade algorithm
    cascade_position(x, y);
    *width = DEFAULT_WIDTH;
    *height = DEFAULT_HEIGHT;
    return false;
}

void wb_spatial_save_geometry(const char *dir_path, int x, int y, int width, int height) {
    if (!dir_path) return;

    // Pack geometry into binary format
    WindowGeometry geom;
    geom.x = x;
    geom.y = y;
    geom.width = width;
    geom.height = height;

    // Attempt to save geometry - silently degrade on failure
    setxattr(dir_path, XATTR_WINDOW_GEOMETRY, &geom, sizeof(geom), 0);
}
