// File: icon_glowicon.c
// GlowIcon/ColorIcon format support (IFF FORM/ICON chunks)
#include "icon_internal.h"
#include <string.h>
#include <stdlib.h>

// Parse GlowIcon format (IFF FORM/ICON or ToolTypes encoded)
// This is a large cohesive function that handles RLE decompression, palette parsing,
// and transparency - kept together per coding_guidelines.md
int icon_parse_glowicon(Display *dpy, const uint8_t *data, long size, long offset,
                        Pixmap *normal_out, Pixmap *selected_out,
                        uint16_t *width_out, uint16_t *height_out,
                        uint16_t *sel_width_out, uint16_t *sel_height_out,
                        XRenderPictFormat *fmt, const char *icon_path) {

    if (offset + 12 > size) return 1;  // Need at least FORM header or marker

    // Check if offset points to ToolTypes markers (WIM/MIM/IM) instead of FORM chunks
    if (offset + 4 <= size) {
        // Check for WIM1=, MIM1=, or IM1= markers
        if ((data[offset] == 'W' || data[offset] == 'M' || data[offset] == 'I') &&
            data[offset+1] == 'I' && data[offset+2] == 'M' && data[offset+3] == '1' &&
            offset + 5 <= size && data[offset+4] == '=') {
            // This is ToolTypes encoding - not yet implemented
            log_error("[ERROR] icons.c:icon_parse_glowicon() - ToolTypes encoding (WIM/MIM/IM1) not yet implemented in %s", icon_path);
            return 1;  // Return error for now until we implement the decoder
        }
    }

    // Continue with FORM ICON chunk parsing
    if (icon_read_iff_id(data + offset) != IFF_FORM_ID) return 1;
    uint32_t form_size = icon_read_be32(data + offset + 4);
    if (icon_read_iff_id(data + offset + 8) != IFF_ICON_ID) return 1;

    long pos = offset + 12;
    long form_end = offset + 8 + form_size;
    if (form_end > size) form_end = size;

    ColorIconFace current_face = {0};  // Track current FACE for next IMAG
    int has_face = 0;
    int state_count = 0;
    Pixmap pixmaps[2] = {0, 0};
    uint16_t widths[2] = {0, 0};
    uint16_t heights[2] = {0, 0};

    // Storage for the first image's palette to reuse if needed
    uint32_t first_palette[256];
    int first_palette_colors = 0;

    // Parse IFF chunks
    while (pos + 8 <= form_end) {
        uint32_t chunk_id = icon_read_iff_id(data + pos);
        uint32_t chunk_size = icon_read_be32(data + pos + 4);
        pos += 8;


        if (chunk_id == IFF_FACE_ID && chunk_size >= 6) {
            // Update current_face for the next IMAG chunk
            current_face.width_minus_1 = data[pos];
            current_face.height_minus_1 = data[pos + 1];
            current_face.flags = data[pos + 2];
            current_face.aspect_ratio = data[pos + 3];
            current_face.max_palette_minus_1 = icon_read_be16(data + pos + 4);
            has_face = 1;
        }
        else if (chunk_id == IFF_IMAG_ID) {
            if (!has_face) {
            } else if (state_count >= 2) {
            } else {
                // Process IMAG
            ColorIconImage img;
            if (pos + 10 > form_end) {
                break;
            }

            img.transparent_index = data[pos];
            img.num_colors_minus_1 = data[pos + 1];
            img.flags = data[pos + 2];
            img.image_compression = data[pos + 3];
            img.palette_compression = data[pos + 4];
            img.depth = data[pos + 5];
            img.image_size_minus_1 = icon_read_be16(data + pos + 6);
            img.palette_size_minus_1 = icon_read_be16(data + pos + 8);

            // Use current_face dimensions for this IMAG
            uint16_t width = current_face.width_minus_1 + 1;
            uint16_t height = current_face.height_minus_1 + 1;
            uint16_t num_colors = img.num_colors_minus_1 + 1;
            uint16_t image_size = img.image_size_minus_1 + 1;
            uint16_t palette_size = img.palette_size_minus_1 + 1;


            long image_offset = pos + 10;
            long palette_offset = image_offset + image_size;


            // Only check palette bounds if the flags indicate there's a palette
            if ((img.flags & 2) && palette_size > 0) {
                // Has palette flag is set and palette_size > 0
                if (palette_offset + palette_size > form_end) {
                    break;
                }
            } else if (!(img.flags & 2)) {
                // No palette flag - ignore palette_size field completely
                palette_size = 0;  // Force to 0 since there's no palette
            }

            // Also check that image data doesn't extend beyond form
            if (image_offset + image_size > form_end) {
                break;
            }

            // Decompress pixels if needed
            uint8_t *pixels = malloc(width * height);
            if (!pixels) break;

            if (img.image_compression == 0) {
                // Uncompressed
                if (image_offset + image_size <= form_end) {
                    memcpy(pixels, data + image_offset, width * height);
                }
            } else {
                // RLE compressed - bit-aligned
                int pixel_count = 0;
                int bit_offset = 0;
                int max_bits = (image_size - 1) * 8;


                while (bit_offset < max_bits && pixel_count < width * height) {
                    uint8_t ctrl = icon_read_bits(data + image_offset, 8, bit_offset);
                    bit_offset += 8;

                    if (ctrl > 128) {
                        // Repeat next value (257 - ctrl) times
                        uint8_t value = icon_read_bits(data + image_offset, img.depth, bit_offset);
                        bit_offset += img.depth;
                        int repeat = 257 - ctrl;
                        for (int i = 0; i < repeat && pixel_count < width * height; i++) {
                            pixels[pixel_count++] = value;
                        }
                    } else if (ctrl < 128) {
                        // Copy next (ctrl + 1) values
                        int copy_count = ctrl + 1;
                        for (int i = 0; i < copy_count && pixel_count < width * height; i++) {
                            pixels[pixel_count++] = icon_read_bits(data + image_offset, img.depth, bit_offset);
                            bit_offset += img.depth;
                        }
                    }
                    // Note: JavaScript ignores ctrl == 128
                }

            }

            // Parse palette
            uint32_t palette[256];
            memset(palette, 0, sizeof(palette));


            // Check if this is an unusual case without palette
            if (!(img.flags & 2)) {
                if (state_count == 0) {
                    // First image without palette - use grayscale
                    for (int i = 0; i < 256; i++) {
                        uint8_t gray = i;
                        palette[i] = 0xFF000000 | (gray << 16) | (gray << 8) | gray;
                    }
                } else if (state_count == 1 && first_palette_colors > 0) {
                    // Second image without palette - reuse first image's palette
                    memcpy(palette, first_palette, sizeof(first_palette));
                    num_colors = first_palette_colors;
                } else {
                    // Fallback to grayscale
                    for (int i = 0; i < 256; i++) {
                        uint8_t gray = i;
                        palette[i] = 0xFF000000 | (gray << 16) | (gray << 8) | gray;
                    }
                }
            } else if (img.flags & 2) {  // Has palette
                if (img.palette_compression == 0) {
                    // Uncompressed RGB palette
                    for (int i = 0; i < num_colors && i < 256 && palette_offset + i*3 + 2 < form_end; i++) {
                        uint8_t r = data[palette_offset + i*3];
                        uint8_t g = data[palette_offset + i*3 + 1];
                        uint8_t b = data[palette_offset + i*3 + 2];
                        palette[i] = 0xFF000000 | (r << 16) | (g << 8) | b;
                    }
                } else {
                    // RLE compressed palette
                    uint8_t rgb[768];
                    int rgb_count = 0;
                    int bit_offset = 0;
                    int max_bits = (palette_size - 1) * 8;
                    // BUG FIX: rgb_count is counting RGB bytes, need num_colors * 3 for the limit
                    int max_rgb_bytes = num_colors * 3;

                    while (bit_offset < max_bits && rgb_count < max_rgb_bytes) {
                        uint8_t ctrl = icon_read_bits(data + palette_offset, 8, bit_offset);
                        bit_offset += 8;

                        if (ctrl > 128) {
                            uint8_t value = icon_read_bits(data + palette_offset, 8, bit_offset);
                            bit_offset += 8;
                            int repeat = 257 - ctrl;
                            for (int i = 0; i < repeat && rgb_count < max_rgb_bytes; i++) {
                                rgb[rgb_count++] = value;
                            }
                        } else if (ctrl < 128) {
                            int copy_count = ctrl + 1;
                            for (int i = 0; i < copy_count && rgb_count < max_rgb_bytes; i++) {
                                rgb[rgb_count++] = icon_read_bits(data + palette_offset, 8, bit_offset);
                                bit_offset += 8;
                            }
                        }
                    }

                    for (int i = 0; i < num_colors && i < 256 && i*3 + 2 < rgb_count; i++) {
                        palette[i] = 0xFF000000 | (rgb[i*3] << 16) | (rgb[i*3+1] << 8) | rgb[i*3+2];
                    }
                }
            }

            // Handle transparent color
            // The transparentIndex field tells us which palette entry should be transparent
            // If flags & 1, then transparentIndex is valid
            uint8_t transparent_idx = 255;  // Invalid by default
            if (img.flags & 1) {
                transparent_idx = img.transparent_index;
            }

            // Save first image's palette for potential reuse
            if (state_count == 0 && (img.flags & 2)) {
                memcpy(first_palette, palette, sizeof(first_palette));
                first_palette_colors = num_colors;
            }

            // Apply transparency to the specified index
            // When flags & 1, the transparentIndex field is valid
            if ((img.flags & 1) && transparent_idx < num_colors) {
                palette[transparent_idx] = 0x00000000;
            }

            // Don't skip transparent selected images - they're valid in AmigaOS
            // The transparency is meant to show through to highlight color


            // Create X11 pixmap
            XVisualInfo vinfo;
            Pixmap pixmap;
            XImage *image;
            if (icon_create_rendering_context(dpy, width, height, &pixmap, &image, &vinfo) == 0) {
                // Count pixel index usage for debugging
                int pixel_0_count = 0;
                int pixel_trans_count = 0;

                for (int y = 0; y < height; y++) {
                    for (int x = 0; x < width; x++) {
                        uint8_t pixel = pixels[y * width + x];
                        if (pixel == 0) pixel_0_count++;
                        if (pixel == transparent_idx) pixel_trans_count++;
                        XPutPixel(image, x, y, palette[pixel]);
                    }
                }

                GC gc = XCreateGC(dpy, pixmap, 0, NULL);
                XPutImage(dpy, pixmap, gc, image, 0, 0, 0, 0, width, height);
                XFreeGC(dpy, gc);
                XDestroyImage(image);

                pixmaps[state_count] = pixmap;
                widths[state_count] = width;
                heights[state_count] = height;

                // Debug: Check transparency for problematic icons
                const char *basename = strrchr(icon_path, '/');
                if (basename) basename++; else basename = icon_path;

                int debug_icon = (strstr(basename, "raamdisk.info") != NULL ||
                                strstr(basename, "Install.info") != NULL ||
                                strstr(basename, "OS3.9-Installation.info") != NULL ||
                                strstr(basename, "contribution.info") != NULL);

                if (debug_icon || state_count == 1) {
                            int transparent_count = 0;
                            int black_count = 0;
                            int white_count = 0;
                            int colored_count = 0;
                            int wb_bg_count = 0;  // Workbench background color

                            for (int y = 0; y < height; y++) {
                                for (int x = 0; x < width; x++) {
                                    uint8_t pixel_idx = pixels[y * width + x];
                                    uint32_t color = palette[pixel_idx];
                                    uint8_t alpha = (color >> 24) & 0xFF;
                                    uint8_t r = (color >> 16) & 0xFF;
                                    uint8_t g = (color >> 8) & 0xFF;
                                    uint8_t b = color & 0xFF;

                                    if (pixel_idx == transparent_idx) {
                                        wb_bg_count++;  // These show workbench background
                                    } else if (alpha == 0) {
                                        transparent_count++;
                                    } else if (r == 0 && g == 0 && b == 0) {
                                        black_count++;
                                    } else if (r == 255 && g == 255 && b == 255) {
                                        white_count++;
                                    } else {
                                        colored_count++;
                                    }
                                }
                            }


                            if (state_count == 1) {  // Selected image
                                if (transparent_count == width * height) {
                                    log_error("[WARNING] Selected image is fully transparent!");
                                } else if (transparent_count > (width * height * 0.9)) {
                                    log_error("[WARNING] Selected image is %d%% transparent",
                                           (transparent_count * 100) / (width * height));
                                }
                            }
                        }

                state_count++;
            }

            free(pixels);
            }  // End of IMAG processing
        }

        // Move to next chunk (word-aligned)
        pos += chunk_size;
        if (chunk_size & 1) pos++;
    }

    // Return results
    if (state_count > 0) {
        *normal_out = pixmaps[0];
        *width_out = widths[0];
        *height_out = heights[0];

        if (state_count > 1) {
            *selected_out = pixmaps[1];
            *sel_width_out = widths[1];
            *sel_height_out = heights[1];
        } else {
            *selected_out = 0;
        }
        return 0;
    }

    // Cleanup on failure
    for (int i = 0; i < 2; i++) {
        if (pixmaps[i]) XFreePixmap(dpy, pixmaps[i]);
    }
    return 1;
}
