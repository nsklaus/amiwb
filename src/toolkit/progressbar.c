// progressbar.c - Progress bar widget implementation
#include "progressbar.h"
#include "toolkit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Create a new progress bar widget
ProgressBar* progressbar_create(Window parent, int x, int y, int width, int height) {
    ProgressBar *pb = calloc(1, sizeof(ProgressBar));
    if (!pb) {
        toolkit_log_error("[ERROR] ProgressBar: Failed to allocate memory (size=%zu)", sizeof(ProgressBar));
        return NULL;
    }
    
    pb->x = x;
    pb->y = y;
    pb->width = width;
    pb->height = height;
    pb->parent_window = parent;
    pb->percent = 0.0f;
    pb->visible = true;
    
    // Default colors - Amiga style
    // Blue fill (from config.h BLUE color)
    pb->bar_color.red = 0x5555;
    pb->bar_color.green = 0x9999;
    pb->bar_color.blue = 0xDDDD;
    pb->bar_color.alpha = 0xFFFF;
    
    // Gray background
    pb->bg_color.red = 0xAAAA;
    pb->bg_color.green = 0xAAAA;
    pb->bg_color.blue = 0xAAAA;
    pb->bg_color.alpha = 0xFFFF;
    
    // Black border (top/left for 3D effect)
    pb->border_dark.red = 0x0000;
    pb->border_dark.green = 0x0000;
    pb->border_dark.blue = 0x0000;
    pb->border_dark.alpha = 0xFFFF;
    
    // White border (bottom/right for 3D effect)
    pb->border_light.red = 0xFFFF;
    pb->border_light.green = 0xFFFF;
    pb->border_light.blue = 0xFFFF;
    pb->border_light.alpha = 0xFFFF;
    
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

// Draw the progress bar with 3D effect
void progressbar_draw(ProgressBar *pb, Display *dpy, Picture dest_picture) {
    if (!pb || !pb->visible || !dpy || !dest_picture) return;
    
    // Draw background/empty area
    XRenderFillRectangle(dpy, PictOpOver, dest_picture, &pb->bg_color,
                         pb->x + 1, pb->y + 1, 
                         pb->width - 2, pb->height - 2);
    
    // Calculate filled width based on percentage
    int filled_width = (int)((pb->width - 2) * (pb->percent / 100.0f));
    
    // Draw filled portion
    if (filled_width > 0) {
        XRenderFillRectangle(dpy, PictOpOver, dest_picture, &pb->bar_color,
                            pb->x + 1, pb->y + 1,
                            filled_width, pb->height - 2);
    }
    
    // Draw 3D borders
    // Top border (dark)
    XRenderFillRectangle(dpy, PictOpOver, dest_picture, &pb->border_dark,
                        pb->x, pb->y, pb->width, 1);
    
    // Left border (dark)
    XRenderFillRectangle(dpy, PictOpOver, dest_picture, &pb->border_dark,
                        pb->x, pb->y, 1, pb->height);
    
    // Bottom border (light)
    XRenderFillRectangle(dpy, PictOpOver, dest_picture, &pb->border_light,
                        pb->x, pb->y + pb->height - 1, pb->width, 1);
    
    // Right border (light)
    XRenderFillRectangle(dpy, PictOpOver, dest_picture, &pb->border_light,
                        pb->x + pb->width - 1, pb->y, 1, pb->height);
}

// Show/hide the progress bar
void progressbar_set_visible(ProgressBar *pb, bool visible) {
    if (!pb) return;
    pb->visible = visible;
}

// Destroy and free the progress bar
void progressbar_destroy(ProgressBar *pb) {
    if (pb) {
        free(pb);
    }
}