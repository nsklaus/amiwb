// File: rnd_wallpaper.c
// Wallpaper loading and caching for desktop and window backgrounds
// Uses Imlib2 for image loading, caches as Pixmap and XRender Picture for fast drawing

#include "rnd_internal.h"
#include "../intuition/itn_public.h"
#include "../amiwbrc.h"
#include <Imlib2.h>
#include <X11/extensions/Xrender.h>
#include <string.h>

// Load image with Imlib2 into a full-screen Pixmap; tile if requested.
static Pixmap load_wallpaper_to_pixmap(Display *dpy, int screen_num, const char *path, bool tile) {
    if (!path || strlen(path) == 0) return None;
    Imlib_Image img = imlib_load_image(path);
    if (!img) {
        log_error("[ERROR] Failed to load wallpaper: %s", path);
        return None;
    }
    imlib_context_set_image(img);
    int img_width = imlib_image_get_width();
    int img_height = imlib_image_get_height();

    int screen_width = DisplayWidth(dpy, screen_num);
    int screen_height = DisplayHeight(dpy, screen_num);

    Pixmap pixmap = XCreatePixmap(dpy, RootWindow(dpy, screen_num), screen_width, screen_height, DefaultDepth(dpy, screen_num));

    imlib_context_set_drawable(pixmap);
    if (!tile) {
        imlib_render_image_on_drawable_at_size(0, 0, screen_width, screen_height);
    } else {
        for (int y = 0; y < screen_height; y += img_height) {
            for (int x = 0; x < screen_width; x += img_width) {
                imlib_render_image_on_drawable(x, y);
            }
        }
    }

    imlib_free_image();
    return pixmap;
}

// Public API: (re)load wallpapers into RenderContext so background
// draws fast without re-scaling images each frame.
void render_load_wallpapers(void) {
    RenderContext *ctx = get_render_context();
    if (!ctx) return;
    Display *dpy = ctx->dpy;
    int scr = DefaultScreen(dpy);

    // Free previous pixmaps and cached Pictures if any
    if (ctx->desk_img != None) {
        XFreePixmap(dpy, ctx->desk_img);
        ctx->desk_img = None;
    }
    if (ctx->desk_picture != None) {
        XRenderFreePicture(dpy, ctx->desk_picture);
        ctx->desk_picture = None;
    }
    if (ctx->wind_img != None) {
        XFreePixmap(dpy, ctx->wind_img);
        ctx->wind_img = None;
    }
    if (ctx->wind_picture != None) {
        XRenderFreePicture(dpy, ctx->wind_picture);
        ctx->wind_picture = None;
    }

    // Get config for wallpaper settings
    const AmiwbConfig *cfg = get_config();

    // Load desktop background if configured
    if (cfg->desktop_background[0]) {
        ctx->desk_img = load_wallpaper_to_pixmap(dpy, scr, cfg->desktop_background, cfg->desktop_tiling);
        // Create cached Picture for desktop wallpaper
        if (ctx->desk_img != None) {
            Visual *visual = DefaultVisual(dpy, DefaultScreen(dpy));
            XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy, visual);
            if (fmt) {
                ctx->desk_picture = XRenderCreatePicture(dpy, ctx->desk_img, fmt, 0, NULL);
            }
        }
    }
    // Load window background if configured
    if (cfg->window_background[0]) {
        ctx->wind_img = load_wallpaper_to_pixmap(dpy, scr, cfg->window_background, cfg->window_tiling);
        // Create cached Picture for window wallpaper
        if (ctx->wind_img != None) {
            Visual *visual = DefaultVisual(dpy, DefaultScreen(dpy));
            XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy, visual);
            if (fmt) {
                ctx->wind_picture = XRenderCreatePicture(dpy, ctx->wind_img, fmt, 0, NULL);
            }
        }
    }
}
