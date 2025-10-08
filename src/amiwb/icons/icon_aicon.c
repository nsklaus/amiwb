// File: icon_aicon.c
// AICON format support (PNG-based modern icon format)
#include "icon_internal.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <Imlib2.h>

// Load AICON format (PNG-based container with normal/selected states)
// Uses Imlib2 for PNG decoding and rendering
int icon_load_aicon(FileIcon *icon, RenderContext *ctx,
                    const uint8_t *data, long size) {
    // Verify header size
    if (size < sizeof(AiconHeader)) {
        log_error("[ERROR] AICON file too small: %s", icon->path);
        return -1;
    }

    const AiconHeader *hdr = (const AiconHeader *)data;

    // Verify magic
    if (memcmp(hdr->magic, "AICON", 5) != 0) {
        log_error("[ERROR] Invalid AICON magic in %s", icon->path);
        return -1;
    }

    // Check version
    if (hdr->version != AICON_VERSION) {
        log_error("[ERROR] Unsupported AICON version %d in %s", hdr->version, icon->path);
        return -1;
    }

    // Read directory
    const uint8_t *dir_ptr = data + sizeof(AiconHeader);
    const AiconSectionEntry *entries = (const AiconSectionEntry *)dir_ptr;

    const uint8_t *png1_data = NULL, *png2_data = NULL;
    uint32_t png1_size = 0, png2_size = 0;

    // Find PNG sections
    for (int i = 0; i < hdr->num_sections; i++) {
        uint32_t offset = entries[i].offset;
        uint32_t sec_size = entries[i].size;

        // Validate offset and size
        if (offset + sec_size > size) {
            log_error("[ERROR] Invalid AICON section offset in %s", icon->path);
            return -1;
        }

        switch (entries[i].type) {
        case SECTION_PNG_NORMAL:
            png1_data = data + offset;
            png1_size = sec_size;
            break;

        case SECTION_PNG_SELECTED:
            png2_data = data + offset;
            png2_size = sec_size;
            break;

        case SECTION_METADATA:
            // Metadata handling could be added here (position, etc.)
            break;
        }
    }

    // Must have at least normal PNG
    if (!png1_data || png1_size == 0) {
        log_error("[ERROR] AICON missing normal PNG in %s", icon->path);
        return -1;
    }

    // Load PNG using Imlib2 (via temporary file, as Imlib2 doesn't support memory loading)
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/amiwb_aicon_%d.png", getpid());

    FILE *tmp_file = fopen(tmp_path, "wb");
    if (!tmp_file) {
        log_error("[ERROR] Failed to create temp file for AICON");
        return -1;
    }
    fwrite(png1_data, 1, png1_size, tmp_file);
    fclose(tmp_file);

    // Load with Imlib2
    Imlib_Image img1 = imlib_load_image(tmp_path);
    unlink(tmp_path);  // Delete temp file

    if (!img1) {
        log_error("[ERROR] Failed to decode PNG in AICON %s", icon->path);
        return -1;
    }

    // Get dimensions
    imlib_context_set_image(img1);
    int width = imlib_image_get_width();
    int height = imlib_image_get_height();

    // Get 32-bit TrueColor visual for alpha compositing (same as classic icons)
    XVisualInfo vinfo;
    Pixmap normal_pixmap;
    XImage *unused_image;
    if (icon_create_rendering_context(ctx->dpy, width, height, &normal_pixmap, &unused_image, &vinfo) != 0) {
        imlib_free_image();
        return -1;
    }
    XDestroyImage(unused_image);  // Imlib2 renders directly to pixmap, don't need XImage

    // Configure Imlib2 context
    imlib_context_set_display(ctx->dpy);
    imlib_context_set_visual(vinfo.visual);
    imlib_context_set_colormap(DefaultColormap(ctx->dpy, DefaultScreen(ctx->dpy)));
    imlib_context_set_drawable(normal_pixmap);
    imlib_context_set_image(img1);

    // Render to pixmap
    imlib_render_image_on_drawable(0, 0);

    // Create Picture for compositing
    icon->normal_picture = icon_create_picture(ctx->dpy, normal_pixmap, ctx->fmt);
    icon->width = width;
    icon->height = height;

    imlib_free_image();

    // Load selected state (if provided)
    Pixmap selected_pixmap = None;

    if (png2_data && png2_size > 0) {
        // Load second PNG
        tmp_file = fopen(tmp_path, "wb");
        if (tmp_file) {
            fwrite(png2_data, 1, png2_size, tmp_file);
            fclose(tmp_file);

            Imlib_Image img2 = imlib_load_image(tmp_path);
            unlink(tmp_path);

            if (img2) {
                imlib_context_set_image(img2);

                selected_pixmap = XCreatePixmap(ctx->dpy, DefaultRootWindow(ctx->dpy),
                                               width, height, ICON_RENDER_DEPTH);
                if (selected_pixmap) {
                    imlib_context_set_drawable(selected_pixmap);
                    imlib_render_image_on_drawable(0, 0);
                }

                imlib_free_image();
            }
        }
    }

    // If no selected PNG provided, create darkened version
    if (selected_pixmap == None) {
        selected_pixmap = icon_create_darkened_pixmap(ctx->dpy, normal_pixmap, width, height);
    }

    icon->selected_picture = icon_create_picture(ctx->dpy, selected_pixmap, ctx->fmt);
    icon->sel_width = width;
    icon->sel_height = height;
    icon->current_picture = icon->normal_picture;

    // Successfully loaded AICON
    return 0;
}
