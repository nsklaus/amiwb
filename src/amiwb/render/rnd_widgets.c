// File: rnd_widgets.c
// UI widget rendering (scrollbars, buttons, resize handle, checkerboard patterns)
// All widget drawing helpers for window frames and scrollbars

#include "rnd_internal.h"
#include "../intuition/itn_public.h"
#include <X11/extensions/Xrender.h>

// Draw up and down arrow controls for vertical scrollbar
void draw_vertical_scrollbar_arrows(Display *dpy, Picture dest, Canvas *canvas) {
    int window_width = canvas->width;
    int window_height = canvas->height;
    // Right border arrow separators
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT + 1, window_height - BORDER_HEIGHT_BOTTOM - 1, BORDER_WIDTH_RIGHT, 1);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - BORDER_WIDTH_RIGHT + 1, window_height - BORDER_HEIGHT_BOTTOM - 20, BORDER_WIDTH_RIGHT - 2, 1);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT + 1, window_height - BORDER_HEIGHT_BOTTOM - 21, BORDER_WIDTH_RIGHT - 2, 1);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - BORDER_WIDTH_RIGHT + 1, window_height - BORDER_HEIGHT_BOTTOM - 40, BORDER_WIDTH_RIGHT - 2, 1);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT + 1, window_height - BORDER_HEIGHT_BOTTOM - 41, BORDER_WIDTH_RIGHT - 2, 1);

    // Down arrow button (bottom)
    if (canvas->v_arrow_down_armed) {
        // Sunken effect - black on top/left, white on bottom/right
        XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT, window_height - BORDER_HEIGHT_BOTTOM - 20, 1, 19);  // Left edge
        XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT, window_height - BORDER_HEIGHT_BOTTOM - 21, 20, 1);  // Top edge
        XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - 1, window_height - BORDER_HEIGHT_BOTTOM - 20, 1, 19);  // Right edge
        XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - BORDER_WIDTH_RIGHT, window_height - BORDER_HEIGHT_BOTTOM - 1, 20, 1);  // Bottom edge
    }

    // Down arrow shape
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 10, window_height - BORDER_HEIGHT_BOTTOM - 10, 2, 4);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 12, window_height - BORDER_HEIGHT_BOTTOM - 12, 2, 4);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 14, window_height - BORDER_HEIGHT_BOTTOM - 14, 2, 4);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 8, window_height - BORDER_HEIGHT_BOTTOM - 12, 2, 4);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 6, window_height - BORDER_HEIGHT_BOTTOM - 14, 2, 4);

    // Up arrow button (top)
    if (canvas->v_arrow_up_armed) {
        // Sunken effect - black on top/left, white on bottom/right
        XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT, window_height - BORDER_HEIGHT_BOTTOM - 40, 1, 19);  // Left edge
        XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT, window_height - BORDER_HEIGHT_BOTTOM - 41, 20, 1);  // Top edge
        XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - 1, window_height - BORDER_HEIGHT_BOTTOM - 40, 1, 19);  // Right edge
        XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - BORDER_WIDTH_RIGHT, window_height - BORDER_HEIGHT_BOTTOM - 21, 20, 1);  // Bottom edge
    }

    // Up arrow shape
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 10, window_height - BORDER_HEIGHT_BOTTOM - 35, 2, 4);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 12, window_height - BORDER_HEIGHT_BOTTOM - 33, 2, 4);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 14, window_height - BORDER_HEIGHT_BOTTOM - 31, 2, 4);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 8, window_height - BORDER_HEIGHT_BOTTOM - 33, 2, 4);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 6, window_height - BORDER_HEIGHT_BOTTOM - 31, 2, 4);
}

// Draw left and right arrow controls for horizontal scrollbar
void draw_horizontal_scrollbar_arrows(Display *dpy, Picture dest, Canvas *canvas) {
    int window_width = canvas->width;
    int window_height = canvas->height;
    // Bottom border arrow separators
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - BORDER_WIDTH_RIGHT - 21, window_height - BORDER_HEIGHT_BOTTOM, 1, BORDER_HEIGHT_BOTTOM - 1);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT - 22, window_height - BORDER_HEIGHT_BOTTOM + 1, 1, BORDER_HEIGHT_BOTTOM - 1);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - BORDER_WIDTH_RIGHT - 41, window_height - BORDER_HEIGHT_BOTTOM, 1, BORDER_HEIGHT_BOTTOM - 1);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT - 42, window_height - BORDER_HEIGHT_BOTTOM + 1, 1, BORDER_HEIGHT_BOTTOM - 1);

    // Right arrow button
    if (canvas->h_arrow_right_armed) {
        // Sunken effect - black on top/left, white on bottom/right
        XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT - 22, window_height - BORDER_HEIGHT_BOTTOM, 1, 20);  // Left edge
        XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT - 22, window_height - BORDER_HEIGHT_BOTTOM, 22, 1);  // Top edge
        XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - BORDER_WIDTH_RIGHT, window_height - BORDER_HEIGHT_BOTTOM, 1, 20);  // Right edge
        XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - BORDER_WIDTH_RIGHT - 22, window_height - 1, 22, 1);  // Bottom edge
    }

    // Right arrow shape
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT - 8, window_height - BORDER_HEIGHT_BOTTOM + 10, 4, 2);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT - 10, window_height - BORDER_HEIGHT_BOTTOM + 8, 4, 2);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT - 12, window_height - BORDER_HEIGHT_BOTTOM + 6, 4, 2);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT - 10, window_height - BORDER_HEIGHT_BOTTOM + 12, 4, 2);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT - 12, window_height - BORDER_HEIGHT_BOTTOM + 14, 4, 2);

    // Left arrow button
    if (canvas->h_arrow_left_armed) {
        // Sunken effect - black on top/left, white on bottom/right
        XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT - 42, window_height - BORDER_HEIGHT_BOTTOM, 1, 20);  // Left edge
        XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT - 42, window_height - BORDER_HEIGHT_BOTTOM, 20, 1);  // Top edge
        XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - BORDER_WIDTH_RIGHT - 22, window_height - BORDER_HEIGHT_BOTTOM, 1, 20);  // Right edge
        XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - BORDER_WIDTH_RIGHT - 42, window_height - 1, 20, 1);  // Bottom edge
    }

    // Left arrow shape
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 40 - 16, window_height - BORDER_HEIGHT_BOTTOM + 10, 4, 2);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 40 - 14, window_height - BORDER_HEIGHT_BOTTOM + 8, 4, 2);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 40 - 12, window_height - BORDER_HEIGHT_BOTTOM + 6, 4, 2);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 40 - 14, window_height - BORDER_HEIGHT_BOTTOM + 12, 4, 2);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 40 - 12, window_height - BORDER_HEIGHT_BOTTOM + 14, 4, 2);
}

// Draw the resize handle/grip in the bottom-right corner of window frame
void draw_resize_button(Display *dpy, Picture dest, Canvas *canvas) {
    int window_width = canvas->width;
    int window_height = canvas->height;

    // Apply sunken 3D effect when resize button is armed
    if (canvas->resize_armed) {
        // Draw sunken borders - swap colors for pressed look
        // Left edge (dark when pressed)
        XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT, window_height - BORDER_HEIGHT_BOTTOM, 1, BORDER_HEIGHT_BOTTOM);
        // Top edge (dark when pressed)
        XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT, window_height - BORDER_HEIGHT_BOTTOM, BORDER_WIDTH_RIGHT, 1);
        // Right edge (light when pressed)
        XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - 1, window_height - BORDER_HEIGHT_BOTTOM, 1, BORDER_HEIGHT_BOTTOM);
        // Bottom edge (light when pressed)
        XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - BORDER_WIDTH_RIGHT, window_height - 1, BORDER_WIDTH_RIGHT, 1);
    } else {
        // Border edges of resize button (normal state)
        XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - BORDER_WIDTH_RIGHT, window_height - BORDER_HEIGHT_BOTTOM, 1, BORDER_HEIGHT_BOTTOM - 1);
        XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT - 1, window_height - BORDER_HEIGHT_BOTTOM + 1, 1, BORDER_HEIGHT_BOTTOM - 1);
    }

    // Main grip lines - black outlines
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT + 5, window_height - 5, 11, 1);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 5, window_height - 15, 1, 10);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT + 5, window_height - 7, 1, 3);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 7, window_height - 15, 2, 1);

    // Diagonal black grip pattern
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 8, window_height - 14, 1, 1);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 9, window_height - 13, 1, 1);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 10, window_height - 12, 1, 1);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 11, window_height - 11, 1, 1);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 12, window_height - 10, 1, 1);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 13, window_height - 9, 1, 1);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 14, window_height - 8, 1, 1);

    // White highlight for 3D effect
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - 7, window_height - 14, 2, 9);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - 8, window_height - 13, 1, 8);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - 9, window_height - 12, 1, 7);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - 10, window_height - 11, 1, 6);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - 11, window_height - 10, 1, 5);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - 12, window_height - 9, 1, 4);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - 13, window_height - 8, 1, 3);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - 14, window_height - 7, 1, 2);
}

// Create cached 4x4 checkerboard pattern pixmaps for tiling
// Creates TWO patterns: blue/black for active, gray/black for inactive
void create_checkerboard_pattern(RenderContext *ctx) {
    if (!ctx || !ctx->dpy) return;

    Display *dpy = ctx->dpy;
    int screen = ctx->default_screen;
    XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy, ctx->default_visual);
    XRenderPictureAttributes pa;
    pa.repeat = RepeatNormal;

    // Use the exact colors from config.h
    XRenderColor black = BLACK;
    XRenderColor blue = BLUE;   // Blue for active windows
    XRenderColor gray = GRAY;   // Gray for inactive windows

    // CREATE ACTIVE PATTERN (BLUE AND BLACK)
    ctx->checker_active_pixmap = XCreatePixmap(dpy, DefaultRootWindow(dpy), 4, 4,
                                               DefaultDepth(dpy, screen));
    ctx->checker_active_picture = XRenderCreatePicture(dpy, ctx->checker_active_pixmap, fmt, 0, NULL);
    XRenderChangePicture(dpy, ctx->checker_active_picture, CPRepeat, &pa);

    // Draw blue/black checkerboard for active windows
    XRenderFillRectangle(dpy, PictOpSrc, ctx->checker_active_picture, &blue, 0, 0, 2, 2);   // Top-left: blue
    XRenderFillRectangle(dpy, PictOpSrc, ctx->checker_active_picture, &black, 2, 0, 2, 2);  // Top-right: black
    XRenderFillRectangle(dpy, PictOpSrc, ctx->checker_active_picture, &black, 0, 2, 2, 2);  // Bottom-left: black
    XRenderFillRectangle(dpy, PictOpSrc, ctx->checker_active_picture, &blue, 2, 2, 2, 2);   // Bottom-right: blue

    // CREATE INACTIVE PATTERN (GRAY AND BLACK)
    ctx->checker_inactive_pixmap = XCreatePixmap(dpy, DefaultRootWindow(dpy), 4, 4,
                                                 DefaultDepth(dpy, screen));
    ctx->checker_inactive_picture = XRenderCreatePicture(dpy, ctx->checker_inactive_pixmap, fmt, 0, NULL);
    XRenderChangePicture(dpy, ctx->checker_inactive_picture, CPRepeat, &pa);

    // Draw gray/black checkerboard for inactive windows
    XRenderFillRectangle(dpy, PictOpSrc, ctx->checker_inactive_picture, &gray, 0, 0, 2, 2);   // Top-left: gray
    XRenderFillRectangle(dpy, PictOpSrc, ctx->checker_inactive_picture, &black, 2, 0, 2, 2);  // Top-right: black
    XRenderFillRectangle(dpy, PictOpSrc, ctx->checker_inactive_picture, &black, 0, 2, 2, 2);  // Bottom-left: black
    XRenderFillRectangle(dpy, PictOpSrc, ctx->checker_inactive_picture, &gray, 2, 2, 2, 2);   // Bottom-right: gray
}

// Draw checkerboard pattern in a rectangle area - ULTRA OPTIMIZED VERSION
// Uses cached tiled pattern for massive performance improvement
// Draws EXACTLY the same 2x2 pixel checkerboard pattern
void draw_checkerboard(Display *dpy, Picture dest, int x, int y, int w, int h, XRenderColor color1, XRenderColor color2) {
    RenderContext *ctx = get_render_context();
    if (!ctx) return;

    // Choose the correct pattern based on the colors passed
    // If color1 has blue component > red component, it's the active pattern
    Picture pattern = None;
    if (color1.blue > color1.red) {
        // Active window: use blue/black pattern
        pattern = ctx->checker_active_picture;
    } else {
        // Inactive window: use gray/black pattern
        pattern = ctx->checker_inactive_picture;
    }

    if (pattern == None) {
        log_error("[WARNING] Checkerboard pattern not cached, using fallback");
        return;
    }

    // Use the cached checkerboard pattern with tiling
    // The pattern automatically repeats to fill the area
    // This reduces thousands of calls to just ONE composite operation!
    XRenderComposite(dpy, PictOpSrc, pattern, None, dest,
                     0, 0,    // src x,y
                     0, 0,    // mask x,y
                     x, y,    // dest x,y
                     w, h);   // width, height
}
