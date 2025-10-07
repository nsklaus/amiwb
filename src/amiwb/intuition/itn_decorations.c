// Window frames, titles, borders
// This module handles window decorations and frame drawing

#include "../config.h"
#include "itn_internal.h"
#include <X11/Xlib.h>
#include <string.h>
#include <stdlib.h>

// Helper function to get right border width based on window type
static int get_right_border_width(Canvas *canvas) {
    if (!canvas) return 0;

    // Workbench windows (file manager) have wider right border for scrollbar
    if (canvas->client_win == None) {
        return BORDER_WIDTH_RIGHT;  // 20px for workbench windows
    }
    // Client windows and dialogs have narrow right border
    return BORDER_WIDTH_RIGHT_CLIENT;  // 8px for client windows
}

void itn_decorations_draw_frame(Canvas *canvas) {
    if (!canvas || canvas->type == DESKTOP) return;  // Desktop has no frame

    Display *dpy = itn_core_get_display();
    if (!dpy || !canvas->win) return;

    // TODO: Actual frame drawing implementation
    // This will be moved from intuition.c draw_window_frame()

    // Mark frame area as needing repaint
    DAMAGE_CANVAS(canvas);
    SCHEDULE_FRAME();
}

void itn_decorations_update_title(Canvas *canvas, const char *title) {
    if (!canvas) return;

    // Update title string
    if (canvas->title_change) {
        free(canvas->title_change);
    }
    canvas->title_change = title ? strdup(title) : NULL;

    // Redraw titlebar if window has frame
    if (canvas->type != DESKTOP) {
        // Damage just the titlebar area
        DAMAGE_RECT(canvas->x, canvas->y, canvas->width, BORDER_HEIGHT_TOP);
        SCHEDULE_FRAME();
    }
}

int itn_decorations_handle_click(Canvas *canvas, int x, int y) {
    // Just use hit_test which has the correct logic
    int hit = hit_test(canvas, x, y);

    // Set armed states based on hit result
    switch (hit) {
        case 1:  // HIT_CLOSE
            canvas->close_armed = true;
            break;
        case 2:  // HIT_LOWER
            canvas->lower_armed = true;
            break;
        case 3:  // HIT_ICONIFY
            canvas->iconify_armed = true;
            break;
        case 4:  // HIT_MAXIMIZE
            canvas->maximize_armed = true;
            break;
    }

    return hit;
}

void itn_decorations_get_content_area(Canvas *canvas, int *x, int *y, int *w, int *h) {
    if (!canvas) {
        if (x) *x = 0;
        if (y) *y = 0;
        if (w) *w = 0;
        if (h) *h = 0;
        return;
    }

    if (canvas->fullscreen) {
        if (x) *x = 0;
        if (y) *y = 0;
        if (w) *w = max(1, canvas->width);
        if (h) *h = max(1, canvas->height);
        return;
    }

    if (x) *x = BORDER_WIDTH_LEFT;
    if (y) *y = BORDER_HEIGHT_TOP;
    if (w) *w = max(1, canvas->width - BORDER_WIDTH_LEFT - get_right_border_width(canvas));
    if (h) *h = max(1, canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM);
}

void itn_decorations_calculate_frame_size(int client_w, int client_h, int *frame_w, int *frame_h) {
    // Client windows use 8px left, 8px right, 20px top, 20px bottom
    if (frame_w) *frame_w = max(1, client_w) + BORDER_WIDTH_LEFT + BORDER_WIDTH_RIGHT_CLIENT;  // 8+8=16px horizontal
    if (frame_h) *frame_h = max(1, client_h) + BORDER_HEIGHT_TOP + BORDER_HEIGHT_BOTTOM;  // 20+20=40px vertical
}

// Hit test function - determines what part of the window was clicked
// Returns: 0=client area, 1=close, 2=lower, 3=iconify, 4=titlebar, 5=resize
int hit_test(Canvas *canvas, int x, int y) {
    if (!canvas) return 0;

    // Desktop has no decorations
    if (canvas->type == DESKTOP) return 0;

    // Fullscreen windows have no decorations
    if (canvas->fullscreen) return 0;

    // Check titlebar area
    if (y < BORDER_HEIGHT_TOP) {
        // Button positions match render.c exactly:
        // Close button: x=0-30 (leftmost)
        // Iconify button: x=(width-91) to (width-61)
        // Maximize button: x=(width-61) to (width-31)
        // Lower button: x=(width-31) to width (rightmost)

        // Close button (leftmost)
        if (x >= 0 && x < 30) {
            return 1;  // Close button
        }

        // Iconify button
        if (x >= canvas->width - 91 && x < canvas->width - 61) {
            return 3;  // Iconify button
        }

        // Maximize button
        if (x >= canvas->width - 61 && x < canvas->width - 31) {
            return 4;  // Maximize button
        }

        // Lower button (rightmost)
        if (x >= canvas->width - 31 && x <= canvas->width) {
            return 2;  // Lower button
        }

        // Rest of titlebar is draggable
        return 5;  // Titlebar drag area
    }

    // Check for resize borders
    int right_border = get_right_border_width(canvas);

    // For Workbench windows, only the resize button in bottom-right is draggable
    // For client windows, edges and corners can be used for resize
    bool is_workbench = (canvas->client_win == None && !canvas->disable_scrollbars);

    // Bottom-right resize button area (always active)
    // The resize button is in the bottom-right corner, about 17x17 pixels
    if (x >= canvas->width - 17 && y >= canvas->height - 17) {
        return 9;  // HIT_RESIZE_SE = 9
    }

    // For client windows, allow resize from edges and corners
    if (!is_workbench) {
        // Corners have priority
        int corner_size = 20;

        // Bottom-left corner
        if (x < corner_size && y >= canvas->height - corner_size) {
            return 11;  // HIT_RESIZE_SW = 11
        }

        // Top-right corner
        if (x >= canvas->width - corner_size && y < corner_size) {
            return 7;  // HIT_RESIZE_NE = 7
        }

        // Top-left corner
        if (x < corner_size && y < corner_size) {
            return 13;  // HIT_RESIZE_NW = 13
        }

        // Edge resize areas (only for client windows)
        if (x < BORDER_WIDTH_LEFT) return 12;  // HIT_RESIZE_W = 12
        if (x >= canvas->width - right_border) return 8;  // HIT_RESIZE_E = 8
        if (y >= canvas->height - BORDER_HEIGHT_BOTTOM) return 10;  // HIT_RESIZE_S = 10
    }

    // Client area
    return 0;
}