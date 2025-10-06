// progressbar.c - Progress bar widget implementation
#include "progressbar.h"
#include "../toolkit.h"
#include "../toolkit_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Create a new progress bar widget
ProgressBar* progressbar_create(int x, int y, int width, int height, XftFont *font) {
    ProgressBar *pb = calloc(1, sizeof(ProgressBar));
    if (!pb) {
        toolkit_log_error("[ERROR] ProgressBar: Failed to allocate memory (size=%zu)", sizeof(ProgressBar));
        return NULL;
    }

    pb->x = x;
    pb->y = y;
    pb->width = width;
    pb->height = height;
    pb->font = font;  // Borrowed reference
    pb->visual = NULL;  // Will be cached from xft_draw on first render
    pb->colormap = None;  // Will be cached from xft_draw on first render
    pb->percent = 0.0f;
    pb->visible = true;
    pb->show_percentage = (font != NULL);  // Default: show if font provided

    // Default colors from config.h - Amiga style
    pb->bar_color = BLUE;    // Blue fill from config.h
    pb->bg_color = GRAY;     // Gray background from config.h
    pb->border_dark = BLACK;   // Black border (top/left for 3D effect)
    pb->border_light = WHITE;  // White border (bottom/right for 3D effect)

    return pb;
}

// Update the progress percentage
void progressbar_set_percent(ProgressBar *pb, float percent) {
    if (!pb) return;

    // Clamp to valid range
    if (percent < 0.0f) percent = 0.0f;
    if (percent > 100.0f) percent = 100.0f;

    pb->percent = percent;
}

// Show/hide the percentage text
void progressbar_set_show_percentage(ProgressBar *pb, bool show) {
    if (!pb) return;
    pb->show_percentage = show;
}

// Show/hide the progress bar
void progressbar_set_visible(ProgressBar *pb, bool visible) {
    if (!pb) return;
    pb->visible = visible;
}

// Render the progress bar with 3D effect and optional percentage text
void progressbar_render(ProgressBar *pb, Picture dest, Display *dpy, XftDraw *xft_draw) {
    if (!pb || !pb->visible || !dpy || !dest) return;

    // Draw background/empty area (gray)
    XRenderFillRectangle(dpy, PictOpSrc, dest, &pb->bg_color,
                         pb->x + 1, pb->y + 1,
                         pb->width - 2, pb->height - 2);

    // Calculate filled width based on percentage
    int filled_width = (int)((pb->width - 2) * (pb->percent / 100.0f));

    // Draw filled portion (blue)
    if (filled_width > 0) {
        XRenderFillRectangle(dpy, PictOpSrc, dest, &pb->bar_color,
                            pb->x + 1, pb->y + 1,
                            filled_width, pb->height - 2);
    }

    // Draw 3D borders (inset appearance: black top/left, white bottom/right)
    XRenderFillRectangle(dpy, PictOpSrc, dest, &pb->border_dark,
                        pb->x, pb->y, pb->width, 1);  // Top
    XRenderFillRectangle(dpy, PictOpSrc, dest, &pb->border_dark,
                        pb->x, pb->y, 1, pb->height);  // Left
    XRenderFillRectangle(dpy, PictOpSrc, dest, &pb->border_light,
                        pb->x, pb->y + pb->height - 1, pb->width, 1);  // Bottom
    XRenderFillRectangle(dpy, PictOpSrc, dest, &pb->border_light,
                        pb->x + pb->width - 1, pb->y, 1, pb->height);  // Right

    // Draw percentage text if enabled
    if (pb->show_percentage && pb->font && xft_draw) {
        // Format percentage text
        char percent_text[16];
        snprintf(percent_text, sizeof(percent_text), "%.0f%%", pb->percent);

        // Calculate text dimensions
        XGlyphInfo percent_ext;
        XftTextExtentsUtf8(dpy, pb->font, (FcChar8*)percent_text,
                          strlen(percent_text), &percent_ext);

        // Center the text horizontally and vertically in the progress bar
        int percent_x = pb->x + (pb->width - percent_ext.xOff) / 2;
        int percent_y = pb->y + (pb->height + pb->font->ascent) / 2 - 2;

        // Cache visual and colormap from xft_draw on first use (like Button widget)
        if (!pb->visual) {
            pb->visual = XftDrawVisual(xft_draw);
            pb->colormap = XftDrawColormap(xft_draw);
        }

        // Determine text color based on progress bar fill position
        // Use white when blue fill is within 5px of text start (adaptive color)
        bool use_white = (percent_x - pb->x - 5) < filled_width;

        // Render percentage text with adaptive color
        if (use_white) {
            // White text on blue background
            XRenderColor white_color = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
            XftColor xft_white;
            XftColorAllocValue(dpy, pb->visual, pb->colormap, &white_color, &xft_white);
            XftDrawStringUtf8(xft_draw, &xft_white, pb->font, percent_x, percent_y,
                             (FcChar8*)percent_text, strlen(percent_text));
            XftColorFree(dpy, pb->visual, pb->colormap, &xft_white);
        } else {
            // Black text on gray background
            XRenderColor black_color = {0x0000, 0x0000, 0x0000, 0xFFFF};
            XftColor xft_black;
            XftColorAllocValue(dpy, pb->visual, pb->colormap, &black_color, &xft_black);
            XftDrawStringUtf8(xft_draw, &xft_black, pb->font, percent_x, percent_y,
                             (FcChar8*)percent_text, strlen(percent_text));
            XftColorFree(dpy, pb->visual, pb->colormap, &xft_black);
        }
    }
}

// Destroy and free the progress bar
void progressbar_destroy(ProgressBar *pb) {
    if (pb) {
        free(pb);
    }
}