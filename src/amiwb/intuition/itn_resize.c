// Window resizing
// This module handles interactive window resizing

#include "../config.h"
#include "itn_internal.h"
#include "../workbench/wb_spatial.h"
#include <X11/Xlib.h>

// Resize state
static Canvas *resize_target = NULL;
static int resize_corner = 0;
static int resize_start_x = 0;
static int resize_start_y = 0;
static int resize_orig_x = 0;
static int resize_orig_y = 0;
static int resize_orig_width = 0;
static int resize_orig_height = 0;

void itn_resize_start(Canvas *canvas, int corner) {
    if (!canvas || resize_target) return;

    resize_target = canvas;
    resize_corner = corner;

    // Store original geometry
    resize_orig_x = canvas->x;
    resize_orig_y = canvas->y;
    resize_orig_width = canvas->width;
    resize_orig_height = canvas->height;

    // Get current pointer position
    Display *dpy = itn_core_get_display();
    if (dpy) {
        Window root, child;
        int root_x, root_y, win_x, win_y;
        unsigned int mask;
        int screen = DefaultScreen(dpy);
        XQueryPointer(dpy, RootWindow(dpy, screen),
                     &root, &child, &root_x, &root_y,
                     &win_x, &win_y, &mask);
        resize_start_x = root_x;
        resize_start_y = root_y;

        // Grab pointer for smooth resizing
        XGrabPointer(dpy, canvas->win, False,
                    ButtonReleaseMask | PointerMotionMask,
                    GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
    }
}

void itn_resize_continue(int x, int y) {
    if (!resize_target) return;

    int dx = x - resize_start_x;
    int dy = y - resize_start_y;

    int new_x = resize_orig_x;
    int new_y = resize_orig_y;
    int new_width = resize_orig_width;
    int new_height = resize_orig_height;

    // Calculate new geometry based on resize corner
    // Corner values from HIT_RESIZE enum in itn_events.c:
    // 6=N, 7=NE, 8=E, 9=SE, 10=S, 11=SW, 12=W, 13=NW
    switch (resize_corner) {
        case 13:  // HIT_RESIZE_NW
            new_x = resize_orig_x + dx;
            new_y = resize_orig_y + dy;
            new_width = resize_orig_width - dx;
            new_height = resize_orig_height - dy;
            break;
        case 6:  // HIT_RESIZE_N
            new_y = resize_orig_y + dy;
            new_height = resize_orig_height - dy;
            break;
        case 7:  // HIT_RESIZE_NE
            new_y = resize_orig_y + dy;
            new_width = resize_orig_width + dx;
            new_height = resize_orig_height - dy;
            break;
        case 8:  // HIT_RESIZE_E
            new_width = resize_orig_width + dx;
            break;
        case 9:  // HIT_RESIZE_SE
            new_width = resize_orig_width + dx;
            new_height = resize_orig_height + dy;
            break;
        case 10:  // HIT_RESIZE_S
            new_height = resize_orig_height + dy;
            break;
        case 11:  // HIT_RESIZE_SW
            new_x = resize_orig_x + dx;
            new_width = resize_orig_width - dx;
            new_height = resize_orig_height + dy;
            break;
        case 12:  // HIT_RESIZE_W
            new_x = resize_orig_x + dx;
            new_width = resize_orig_width - dx;
            break;
    }

    // Enforce size constraints from Canvas (respects client's WM_NORMAL_HINTS)
    // Minimum width constraint
    if (new_width < resize_target->min_width) {
        if (resize_corner == 13 || resize_corner == 11 || resize_corner == 12) {  // NW, SW, W
            new_x = resize_orig_x + resize_orig_width - resize_target->min_width;
        }
        new_width = resize_target->min_width;
    }
    // Maximum width constraint
    if (new_width > resize_target->max_width) {
        if (resize_corner == 13 || resize_corner == 11 || resize_corner == 12) {  // NW, SW, W
            new_x = resize_orig_x + resize_orig_width - resize_target->max_width;
        }
        new_width = resize_target->max_width;
    }
    // Minimum height constraint
    if (new_height < resize_target->min_height) {
        if (resize_corner == 13 || resize_corner == 6 || resize_corner == 7) {  // NW, N, NE
            new_y = resize_orig_y + resize_orig_height - resize_target->min_height;
        }
        new_height = resize_target->min_height;
    }
    // Maximum height constraint
    if (new_height > resize_target->max_height) {
        if (resize_corner == 13 || resize_corner == 6 || resize_corner == 7) {  // NW, N, NE
            new_y = resize_orig_y + resize_orig_height - resize_target->max_height;
        }
        new_height = resize_target->max_height;
    }

    // Apply new geometry
    if (new_x != resize_target->x || new_y != resize_target->y ||
        new_width != resize_target->width || new_height != resize_target->height) {
        itn_geometry_move_resize(resize_target, new_x, new_y, new_width, new_height);
    }
}

void itn_resize_finish(void) {
    if (!resize_target) return;

    // Final update with pixmap recreation
    if (resize_target->comp_pixmap) {
        itn_composite_update_canvas_pixmap(resize_target);
    }

    // Save window geometry for spatial mode (workbench windows only)
    if (resize_target->type == WINDOW && resize_target->path) {
        wb_spatial_save_geometry(resize_target->path,
                                resize_target->x, resize_target->y,
                                resize_target->width, resize_target->height);
    }

    // Release pointer grab
    Display *dpy = itn_core_get_display();
    if (dpy) {
        XUngrabPointer(dpy, CurrentTime);
    }

    // Clear resize state
    resize_target = NULL;
    resize_corner = 0;
}

void itn_resize_cancel(void) {
    if (!resize_target) return;

    // Restore original geometry
    itn_geometry_move_resize(resize_target,
                            resize_orig_x, resize_orig_y,
                            resize_orig_width, resize_orig_height);

    // Release pointer grab
    Display *dpy = itn_core_get_display();
    if (dpy) {
        XUngrabPointer(dpy, CurrentTime);
    }

    // Clear resize state
    resize_target = NULL;
    resize_corner = 0;
}

bool itn_resize_is_active(void) {
    return (resize_target != NULL);
}

Canvas *itn_resize_get_target(void) {
    return resize_target;
}

void itn_resize_motion(int mouse_x, int mouse_y) {
    // Just call itn_resize_continue which has all the proper logic
    itn_resize_continue(mouse_x, mouse_y);
}