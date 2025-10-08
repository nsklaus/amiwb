// File: rnd_surface.c
// Canvas surface lifecycle management
// Handles creation/destruction of offscreen buffers (Pixmap) and XRender Pictures

#include "rnd_internal.h"
#include "../intuition/itn_public.h"
#include <X11/extensions/Xrender.h>

// Destroy pixmap and XRender Pictures attached to a canvas
void render_destroy_canvas_surfaces(Canvas *canvas) {
    if (!canvas) return;
    RenderContext *ctx = get_render_context();
    if (!ctx) return;

    // Ensure all pending operations complete before cleanup

    if (canvas->canvas_render != None) {
        XRenderFreePicture(ctx->dpy, canvas->canvas_render);
        canvas->canvas_render = None;
    }
    if (canvas->window_render != None) {
        XRenderFreePicture(ctx->dpy, canvas->window_render);
        canvas->window_render = None;
    }
    // Destroy XftDraw before freeing the pixmap it references
    if (canvas->xft_draw) {
        XftDrawDestroy(canvas->xft_draw);
        canvas->xft_draw = NULL;
    }
    if (canvas->canvas_buffer != None) {
        XFreePixmap(ctx->dpy, canvas->canvas_buffer);
        canvas->canvas_buffer = None;
    }
}

// Recreate pixmap and XRender Pictures based on current canvas size/visual
void render_recreate_canvas_surfaces(Canvas *canvas) {
    if (!canvas) return;
    RenderContext *ctx = get_render_context();
    if (!ctx) return;

    if (canvas->width <= 0 || canvas->height <= 0) return;


    // Free existing resources first
    render_destroy_canvas_surfaces(canvas);

    // Buffer sizing strategy depends on canvas type:
    // - DESKTOP/MENU: Always match current size (screen-fixed, no interactive resize)
    // - WINDOW/DIALOG: Preserve larger dimensions to optimize interactive drag-resize
    int buffer_width, buffer_height;
    if (canvas->type == DESKTOP || canvas->type == MENU) {
        // Screen-fixed canvases: Always use exact current dimensions
        buffer_width = canvas->width;
        buffer_height = canvas->height;
    } else {
        // Resizable canvases: Preserve larger dimensions to avoid reallocation
        buffer_width = (canvas->buffer_width > canvas->width) ? canvas->buffer_width : canvas->width;
        buffer_height = (canvas->buffer_height > canvas->height) ? canvas->buffer_height : canvas->height;
    }

    // Update buffer dimensions
    canvas->buffer_width = buffer_width;
    canvas->buffer_height = buffer_height;

    // Create offscreen pixmap using buffer dimensions
    // XCreatePixmap: Creates an offscreen drawable (like a canvas in memory)
    // We draw everything to this invisible pixmap first, then copy to window
    // This prevents flickering - technique called "double buffering"
    canvas->canvas_buffer = XCreatePixmap(ctx->dpy, canvas->win,
        buffer_width, buffer_height, canvas->depth);
    if (!canvas->canvas_buffer) return;

    // Picture format for the offscreen buffer
    XRenderPictFormat *fmt = XRenderFindVisualFormat(ctx->dpy, canvas->visual);
    if (!fmt) { render_destroy_canvas_surfaces(canvas); return; }

    // Create XRender Picture for the offscreen buffer
    // This wraps our pixmap with rendering capabilities (compositing, transforms)
    // The 0 and NULL mean "no special attributes" - we use defaults
    canvas->canvas_render = XRenderCreatePicture(ctx->dpy, canvas->canvas_buffer, fmt, 0, NULL);
    if (!canvas->canvas_render) { render_destroy_canvas_surfaces(canvas); return; }

    // Create or recreate XftDraw for text rendering - cache it to avoid recreation in render loops
    if (canvas->xft_draw) {
        XftDrawDestroy(canvas->xft_draw);
        canvas->xft_draw = NULL;
    }

    // Client windows draw directly to window, others draw to buffer
    if (canvas->client_win != None) {
        // Client window: XftDraw targets the window directly
        canvas->xft_draw = XftDrawCreate(ctx->dpy, canvas->win,
            canvas->visual ? canvas->visual : DefaultVisual(ctx->dpy, DefaultScreen(ctx->dpy)),
            canvas->colormap);
    } else {
        // Workbench/Desktop: XftDraw targets the buffer
        canvas->xft_draw = XftDrawCreate(ctx->dpy, canvas->canvas_buffer,
            canvas->visual ? canvas->visual : DefaultVisual(ctx->dpy, DefaultScreen(ctx->dpy)),
            canvas->colormap);
    }

    if (!canvas->xft_draw) {
        log_error("[WARNING] Failed to create XftDraw for canvas");
    }

    // For the on-screen window picture, desktop uses root visual
    Visual *win_visual = (canvas->type == DESKTOP)
        ? DefaultVisual(ctx->dpy, DefaultScreen(ctx->dpy))
        : canvas->visual;
    XRenderPictFormat *wfmt = XRenderFindVisualFormat(ctx->dpy, win_visual);
    if (!wfmt) { render_destroy_canvas_surfaces(canvas); return; }

    // Create XRender Picture for the actual window
    // This is our destination for compositing - what the user sees
    // Desktop windows use root visual, others use their own visual
    canvas->window_render = XRenderCreatePicture(ctx->dpy, canvas->win, wfmt, 0, NULL);
    if (!canvas->window_render) { render_destroy_canvas_surfaces(canvas); return; }
}
