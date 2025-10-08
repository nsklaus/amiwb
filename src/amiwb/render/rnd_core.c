// File: rnd_core.c
// Render system lifecycle and font management
// Initialization, cleanup, and text utilities

#include "rnd_internal.h"
#include "../intuition/itn_public.h"
#include "../font_manager.h"
#include <X11/Xft/Xft.h>

// Global UI colors (private to module)
static XftColor text_color_black;
static XftColor text_color_white;

// Initialize rendering resources. Requires RenderContext from
// init_intuition(). If font is not ready yet, callers should guard
// text drawing (redraw_canvas() already does).
void init_render(void) {
    RenderContext *ctx = get_render_context();
    if (!ctx) {
        log_error("[ERROR] Failed to get render_context (call init_intuition first)");
        return;
    }

    // Initialize the unified font system
    if (!font_manager_init(ctx->dpy)) {
        log_error("[ERROR] Font manager initialization failed - AmiWB will run without text rendering");
        // Graceful degradation: continue without fonts (text won't render but graphics will work)
        return;
    }

    // Now that we have a render context and font, load wallpapers and refresh desktop
    render_load_wallpapers();

    // Initialize cached checkerboard pattern for scrollbars
    create_checkerboard_pattern(ctx);

    Canvas *desk = itn_canvas_get_desktop();
    if (desk) redraw_canvas(desk);

    // Initialize colors
    text_color_black.color = BLACK;
    text_color_white.color = WHITE;
}

// Clean up rendering resources
void cleanup_render(void) {
    RenderContext *ctx = get_render_context();
    if (!ctx) return;

    // Clean up cached checkerboard patterns
    if (ctx->checker_active_picture != None) {
        XRenderFreePicture(ctx->dpy, ctx->checker_active_picture);
        ctx->checker_active_picture = None;
    }
    if (ctx->checker_active_pixmap != None) {
        XFreePixmap(ctx->dpy, ctx->checker_active_pixmap);
        ctx->checker_active_pixmap = None;
    }
    if (ctx->checker_inactive_picture != None) {
        XRenderFreePicture(ctx->dpy, ctx->checker_inactive_picture);
        ctx->checker_inactive_picture = None;
    }
    if (ctx->checker_inactive_pixmap != None) {
        XFreePixmap(ctx->dpy, ctx->checker_inactive_pixmap);
        ctx->checker_inactive_pixmap = None;
    }

    font_manager_cleanup(is_restarting());
    if (text_color_black.pixel) {
        XftColorFree(ctx->dpy, DefaultVisual(ctx->dpy, DefaultScreen(ctx->dpy)),
                     DefaultColormap(ctx->dpy, DefaultScreen(ctx->dpy)), &text_color_black);
    }
    if (text_color_white.pixel) {
        XftColorFree(ctx->dpy, DefaultVisual(ctx->dpy, DefaultScreen(ctx->dpy)),
                     DefaultColormap(ctx->dpy, DefaultScreen(ctx->dpy)), &text_color_white);
    }
    // Cleanup render resources
}

// Get width in pixels of UTF-8 text string
int get_text_width(const char *text) {
    return font_manager_text_width(text);
}

// Provide access to the loaded UI font
XftFont *get_font(void) {
    return font_manager_get();
}
