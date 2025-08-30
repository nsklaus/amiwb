// progressbar.h - Progress bar widget for AmiWB toolkit
#ifndef PROGRESSBAR_H
#define PROGRESSBAR_H

#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>
#include <stdbool.h>

typedef struct {
    // Position and dimensions
    int x, y, width, height;
    
    // Progress state
    float percent;  // 0.0 to 100.0
    bool visible;
    
    // Colors
    XRenderColor bar_color;      // Fill color (usually BLUE)
    XRenderColor bg_color;       // Background/empty color
    XRenderColor border_dark;    // Top/left border (black)
    XRenderColor border_light;   // Bottom/right border (white)
    
    // Parent window for drawing
    Window parent_window;
} ProgressBar;

// Create a new progress bar widget
ProgressBar* progressbar_create(Window parent, int x, int y, int width, int height);

// Update the progress percentage (0.0 to 100.0)
void progressbar_set_percent(ProgressBar *pb, float percent);

// Draw the progress bar
void progressbar_draw(ProgressBar *pb, Display *dpy, Picture dest_picture);

// Show/hide the progress bar
void progressbar_set_visible(ProgressBar *pb, bool visible);

// Destroy and free the progress bar
void progressbar_destroy(ProgressBar *pb);

#endif // PROGRESSBAR_H