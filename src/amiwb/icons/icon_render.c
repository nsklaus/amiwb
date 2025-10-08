// File: icon_render.c
// Icon rendering infrastructure and format dispatching
#include "icon_internal.h"
#include <string.h>
#include <stdlib.h>

// Create XRender Picture from Pixmap for compositing
// NOTE: This function takes ownership of the pixmap and frees it
Picture icon_create_picture(Display *dpy, Pixmap pixmap, XRenderPictFormat *fmt) {
    Picture picture = XRenderCreatePicture(dpy, pixmap, fmt, 0, NULL);
    XFreePixmap(dpy, pixmap);  // Pixmap freed here - Picture owns the data now
    return picture;
}

// Create icon rendering context (visual, pixmap, image) - extracts common setup pattern
// OWNERSHIP: Caller must free pixmap (XFreePixmap) and image (XDestroyImage) on error
// NOTE: Image data is allocated but not initialized - caller must fill pixels before use
// Returns 0 on success, 1 on failure
int icon_create_rendering_context(Display *dpy, uint16_t width, uint16_t height,
                                  Pixmap *pixmap_out, XImage **image_out, XVisualInfo *vinfo_out) {
    if (!XMatchVisualInfo(dpy, DefaultScreen(dpy), ICON_RENDER_DEPTH, TrueColor, vinfo_out)) {
        log_error("[ERROR] No %d-bit TrueColor visual found for icon", ICON_RENDER_DEPTH);
        return 1;
    }

    Pixmap pixmap = XCreatePixmap(dpy, DefaultRootWindow(dpy), width, height, ICON_RENDER_DEPTH);
    if (!pixmap) return 1;

    XImage *image = XCreateImage(dpy, vinfo_out->visual, ICON_RENDER_DEPTH, ZPixmap, 0,
                                  malloc(width * height * 4), width, height, 32, 0);
    if (!image) {
        XFreePixmap(dpy, pixmap);
        return 1;
    }

    *pixmap_out = pixmap;
    *image_out = image;
    return 0;
}

// Helper function to clean up partially loaded icon
void icon_cleanup_partial(Display *dpy, FileIcon *icon) {
    if (icon->normal_picture) {
        XRenderFreePicture(dpy, icon->normal_picture);
        icon->normal_picture = None;
    }
    // NOTE: normal_pixmap is freed by the caller at the end of create_icon_images
    // Do NOT free it here to avoid double-free
}

// Create darkened version of icon for selected state
// Darkens by 20% (multiply RGB by 0.8) but keeps alpha unchanged
Pixmap icon_create_darkened_pixmap(Display *dpy, Pixmap src, int width, int height) {
    // Guard against invalid pixmap - don't try to render garbage
    if (src == None || src == 0) {
        return 0;
    }

    // Get the image data from source pixmap
    XImage *src_img = XGetImage(dpy, src, 0, 0, width, height, AllPlanes, ZPixmap);
    if (!src_img) return 0;

    // Create destination pixmap
    Pixmap dark = XCreatePixmap(dpy, src, width, height, 32);

    // Create XImage for the darkened version
    XImage *dark_img = XCreateImage(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                                    32, ZPixmap, 0, NULL, width, height, 32, 0);
    if (!dark_img) {
        XDestroyImage(src_img);
        XFreePixmap(dpy, dark);
        return 0;
    }

    dark_img->data = malloc(dark_img->bytes_per_line * height);
    if (!dark_img->data) {
        XDestroyImage(src_img);
        XDestroyImage(dark_img);
        XFreePixmap(dpy, dark);
        return 0;
    }

    // Darken each pixel by 20% (multiply RGB by 0.8) but keep alpha unchanged
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            unsigned long pixel = XGetPixel(src_img, x, y);

            // Extract ARGB components
            uint8_t a = (pixel >> 24) & 0xFF;
            uint8_t r = (pixel >> 16) & 0xFF;
            uint8_t g = (pixel >> 8) & 0xFF;
            uint8_t b = pixel & 0xFF;

            // Only darken if not transparent
            if (a > 0) {
                // Darken by 20% (multiply by 0.8)
                r = (r * 4) / 5;
                g = (g * 4) / 5;
                b = (b * 4) / 5;
            }

            // Reconstruct pixel
            unsigned long dark_pixel = (a << 24) | (r << 16) | (g << 8) | b;
            XPutPixel(dark_img, x, y, dark_pixel);
        }
    }

    // Put the darkened image into the pixmap
    GC gc = XCreateGC(dpy, dark, 0, NULL);
    XPutImage(dpy, dark, gc, dark_img, 0, 0, 0, 0, width, height);
    XFreeGC(dpy, gc);

    // Cleanup
    XDestroyImage(src_img);
    XDestroyImage(dark_img);

    return dark;
}

// Main planar icon renderer for OS3/MWB formats (variable depth)
// Converts Amiga planar format to chunky RGB and renders to X11 pixmap
int icon_render(Display *dpy, Pixmap *pixmap_out, const uint8_t *data, uint16_t width, uint16_t height, uint16_t depth, AmigaIconFormat format, long data_size) {
    XVisualInfo vinfo;
    Pixmap pixmap;
    XImage *image;
    if (icon_create_rendering_context(dpy, width, height, &pixmap, &image, &vinfo) != 0) {
        return 1;
    }
    memset(image->data, 0, width * height * 4);

    // Icons can use true alpha; index 0 would be transparent. We use a
    // gray fill for now to match classic look; adjust when alpha lands.
    // unsigned long colors[8] = {0x00000000UL, 0xFF000000UL, 0xFFFFFFFFUL, 0xFF6666BBUL, 0xFF999999UL, 0xFFBBBBBBUL, 0xFFBBAA99UL, 0xFFFFAA22UL};

    // Get appropriate color palette based on icon format
    unsigned long colors[8];
    if (format == AMIGA_ICON_OS13) {
        // OS1.3 icons use only 4 colors
        icon_get_os13_palette(colors);
        // Fill remaining slots with black for safety
        colors[4] = 0xFF000000;
        colors[5] = 0xFF000000;
        colors[6] = 0xFF000000;
        colors[7] = 0xFF000000;
    } else {
        // OS3/MWB icons use 8 colors
        icon_get_mwb_palette(colors);
    }
    int row_bytes;
    long plane_size, total_data_size;
    icon_calculate_plane_dimensions(width, height, depth, &row_bytes, &plane_size, &total_data_size);
    const uint8_t *planes = data;

    // Calculate required data size to prevent buffer overrun
    long required_size = plane_size * depth;
    if (data_size < required_size) {
        log_error("[ERROR] Icon data too small: have %ld, need %ld bytes", data_size, required_size);
        XDestroyImage(image);
        XFreePixmap(dpy, pixmap);
        return 1;
    }

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int color = 0;
            for (int p = 0; p < depth; p++) {
                long offset = p * plane_size + y * row_bytes + (x >> 3);
                // Safety check to prevent reading beyond buffer
                if (offset >= data_size) {
                    log_error("[ERROR] Icon data access out of bounds at offset %ld (max %ld)", offset, data_size);
                    XDestroyImage(image);
                    XFreePixmap(dpy, pixmap);
                    return 1;
                }
                uint8_t byte = planes[offset];
                if (byte & (1 << (7 - (x & 7)))) color |= (1 << p);
            }
            XPutPixel(image, x, y, colors[color & 7]);
        }
    }

    GC gc = XCreateGC(dpy, pixmap, 0, NULL);
    XPutImage(dpy, pixmap, gc, image, 0, 0, 0, 0, width, height);
    XFreeGC(dpy, gc);
    XDestroyImage(image);
    *pixmap_out = pixmap;
    return 0;
}
