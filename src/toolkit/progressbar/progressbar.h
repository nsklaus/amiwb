// progressbar.h - Progress bar widget for AmiWB toolkit
#ifndef PROGRESSBAR_H
#define PROGRESSBAR_H

#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>
#include <X11/Xft/Xft.h>
#include <stdbool.h>

typedef struct {
    // Position and dimensions
    int x, y, width, height;

    // Progress state
    float percent;  // 0.0 to 100.0
    bool visible;
    bool show_percentage;  // Show "XX%" text centered on bar

    // Colors
    XRenderColor bar_color;      // Fill color (usually BLUE)
    XRenderColor bg_color;       // Background/empty color
    XRenderColor border_dark;    // Top/left border (black)
    XRenderColor border_light;   // Bottom/right border (white)

    // Font for percentage text (borrowed reference - don't free)
    XftFont *font;

    // Visual and colormap (cached from xft_draw on first render - don't free)
    Visual *visual;
    Colormap colormap;

    // Parent window for drawing
    Window parent_window;
} ProgressBar;

// Create a new progress bar widget
// height: Explicit height in pixels (recommended: (font->height * 2) - 8 for AmigaOS style)
// font: Font for percentage text (can be NULL if show_percentage is false)
ProgressBar* progressbar_create(int x, int y, int width, int height, XftFont *font);

// Update the progress percentage (0.0 to 100.0)
void progressbar_set_percent(ProgressBar *pb, float percent);

// Show/hide the progress bar
void progressbar_set_visible(ProgressBar *pb, bool visible);

// Show/hide the percentage text
void progressbar_set_show_percentage(ProgressBar *pb, bool show);

// Render the progress bar (follows toolkit convention like button_render)
void progressbar_render(ProgressBar *pb, Picture dest, Display *dpy, XftDraw *xft_draw);

// Destroy and free the progress bar
void progressbar_destroy(ProgressBar *pb);

#endif // PROGRESSBAR_H