// Menu System - Rendering Helpers Module
// Helper functions for menu addon rendering

#include "menu_internal.h"
#include "menu_public.h"
#include "../font_manager.h"
#include "../config.h"
#include "../render.h"

// ============================================================================
// Rendering Helpers for Addons
// ============================================================================

// Helper: Render text on menubar at given position
// Returns the width of the rendered text
// This is used by menu addons to display their information in logo mode
int menu_render_text(RenderContext *ctx, Canvas *menubar, const char *text, int x, int y) {
    if (!ctx || !menubar || !text) return 0;

    XftFont *font = font_manager_get();
    if (!font || !menubar->xft_draw) return 0;

    // Allocate text color
    XftColor text_color;
    XRenderColor black = BLACK;
    XftColorAllocValue(ctx->dpy, menubar->visual, menubar->colormap, &black, &text_color);

    // Measure text width
    XGlyphInfo extents;
    XftTextExtentsUtf8(ctx->dpy, font, (FcChar8 *)text, strlen(text), &extents);

    // Calculate y position centered in menubar
    int text_y = font->ascent + (MENU_ITEM_HEIGHT - font->height) / 2 - 1;  // Raised by 1 pixel

    // Render text
    XftDrawStringUtf8(menubar->xft_draw, &text_color, font, x, text_y,
                      (FcChar8 *)text, strlen(text));

    // Free color
    XftColorFree(ctx->dpy, menubar->visual, menubar->colormap, &text_color);

    return extents.xOff;
}

// Helper: Measure text width without rendering
// Used by addons to calculate their required space
int menu_measure_text(RenderContext *ctx, const char *text) {
    if (!ctx || !text) return 0;

    XftFont *font = font_manager_get();
    if (!font) return 0;

    XGlyphInfo extents;
    XftTextExtentsUtf8(ctx->dpy, font, (FcChar8 *)text, strlen(text), &extents);

    return extents.xOff;
}
