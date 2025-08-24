// File: icons.c
#include "config.h"
#include "intuition.h"
#include "icons.h"
#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// Icon parsing constants. We render into a 32-bit pixmap so XRender
// can alpha-composite icons consistently across visuals.
// ICON_HEADER_SIZE and ICON_RENDER_DEPTH are defined in config.h

// IFF FORM/ICON ColorIcon (GlowIcon) structures
#define IFF_FORM_ID 0x464F524D  // 'FORM'
#define IFF_ICON_ID 0x49434F4E  // 'ICON'
#define IFF_FACE_ID 0x46414345  // 'FACE'
#define IFF_IMAG_ID 0x494D4147  // 'IMAG'

typedef struct {
    uint8_t width_minus_1;      // Width - 1
    uint8_t height_minus_1;     // Height - 1  
    uint8_t flags;              // Flags
    uint8_t aspect_ratio;       // Aspect ratio
    uint16_t max_palette_minus_1; // Max palette size - 1
} ColorIconFace;

typedef struct {
    uint8_t transparent_index;  // Transparent color index
    uint8_t num_colors_minus_1; // Number of colors - 1
    uint8_t flags;              // Bit 0: has transparent, Bit 1: has palette
    uint8_t image_compression;  // 0=none, 1=RLE
    uint8_t palette_compression;// 0=none, 1=RLE
    uint8_t depth;              // Bits per pixel
    uint16_t image_size_minus_1;  // Image data size - 1
    uint16_t palette_size_minus_1;// Palette data size - 1
} ColorIconImage;

// TODO: refactor the file to use def_dir and def_foo
static char *def_tool_path = "/usr/local/share/amiwb/icons/def_icons/def_foo.info";
static char *def_drawer_path = "/usr/local/share/amiwb/icons/def_icons/def_dir.info";

static uint16_t read_be16(const uint8_t *p) {
    // Convert big-endian 16-bit value without bit shifting
    return (uint16_t)(p[0] * 256 + p[1]);
}

static uint32_t read_be32(const uint8_t *p) {
    // Convert big-endian 32-bit value without bit shifting
    return (uint32_t)(p[0] * 16777216 + p[1] * 65536 + p[2] * 256 + p[3]);
}

// Read a 4-byte IFF chunk ID
static uint32_t read_iff_id(const uint8_t *p) {
    return read_be32(p);
}

// Read bits from bit-aligned data (for RLE decompression)
static uint8_t read_bits(const uint8_t *data, int bit_count, int bit_offset) {
    int byte_offset = bit_offset / 8;
    int bit_in_byte = bit_offset % 8;
    uint16_t value = (data[byte_offset] << 8) | data[byte_offset + 1];
    value >>= (16 - bit_in_byte - bit_count);
    return value & ((1 << bit_count) - 1);
}

// Helper function to calculate icon plane dimensions
static void calculate_icon_plane_dimensions(uint16_t width, uint16_t height, uint16_t depth, 
                                          int *row_bytes, long *plane_size, long *total_data_size) {
    *row_bytes = ((width + 15) / 16) * 2;
    *plane_size = (*row_bytes) * height;
    *total_data_size = (*plane_size) * depth;
}

// Helper function to create pixmap and render context picture
static Picture create_icon_picture(Display *dpy, Pixmap pixmap, XRenderPictFormat *fmt) {
    Picture picture = XRenderCreatePicture(dpy, pixmap, fmt, 0, NULL);
    XFreePixmap(dpy, pixmap);
    return picture;
}

// Helper function to clean up partially loaded icon
static void cleanup_partial_icon(Display *dpy, FileIcon *icon, Pixmap normal_pixmap) {
    if (icon->normal_picture) {
        XRenderFreePicture(dpy, icon->normal_picture);
        icon->normal_picture = None;
    }
    if (normal_pixmap) {
        XFreePixmap(dpy, normal_pixmap);
    }
}

// Helper function to get default icon color palette
static void get_icon_color_palette(unsigned long colors[8]) {
    // Icons use gray fill instead of transparency  
    colors[0] = 0xFFA0A2A0UL; // Background gray
    colors[1] = 0xFF000000;   // Black
    colors[2] = 0xFFFFFFFF;   // White
    colors[3] = 0xFF6666BB;   // Blue
    colors[4] = 0xFF999999;   // Gray
    colors[5] = 0xFFBBBBBB;   // Light gray
    colors[6] = 0xFFBBAA99;   // Brown
    colors[7] = 0xFFFFAA22;   // Orange
}

// Load entire .info file into memory so we can parse planes quickly.
static int load_icon_file(const char *name, uint8_t **data, long *size) {
    FILE *fp = fopen(name, "rb");
    if (!fp) return 1;
    fseek(fp, 0, SEEK_END);
    *size = ftell(fp);
    rewind(fp);
    *data = malloc(*size);
    if (!*data || fread(*data, 1, *size, fp) != *size) {
        free(*data);
        fclose(fp);
        return 1;
    }
    fclose(fp);
    return 0;
}

// Read the bitmap header after the icon drawer/tool metadata. Sanity
// checks width/height/depth so we don't overrun buffers.
static int parse_icon_header(const uint8_t *header, long size, uint16_t *width, uint16_t *height, uint16_t *depth) {
    if (size < ICON_HEADER_SIZE) return 1;
    *width = read_be16(header + 4);
    *height = read_be16(header + 6);
    *depth = read_be16(header + 8);
    if (*width == 0 || *height == 0 || *depth == 0 || *depth > 8) return 1;
    return 0;
}

// Convert Amiga planar icon data to an ARGB pixmap the server can use.
// Colors are basic and can be refined later; keep it fast and simple.
static int render_icon(Display *dpy, Pixmap *pixmap_out, const uint8_t *data, uint16_t width, uint16_t height, uint16_t depth) {
    XVisualInfo vinfo;
    if (!XMatchVisualInfo(dpy, DefaultScreen(dpy), ICON_RENDER_DEPTH, TrueColor, &vinfo)) {
        fprintf(stderr, "No %d-bit TrueColor visual found for icon\n", ICON_RENDER_DEPTH);
        return 1;
    }
    Pixmap pixmap = XCreatePixmap(dpy, DefaultRootWindow(dpy), width, height, ICON_RENDER_DEPTH);
    if (!pixmap) return 1;

    XImage *image = XCreateImage(dpy, vinfo.visual, ICON_RENDER_DEPTH, ZPixmap, 0, malloc(width * height * 4), width, height, 32, 0);
    if (!image) {
        XFreePixmap(dpy, pixmap);
        return 1;
    }
    memset(image->data, 0, width * height * 4);

    // Icons can use true alpha; index 0 would be transparent. We use a
    // gray fill for now to match classic look; adjust when alpha lands.
    // unsigned long colors[8] = {0x00000000UL, 0xFF000000UL, 0xFFFFFFFFUL, 0xFF6666BBUL, 0xFF999999UL, 0xFFBBBBBBUL, 0xFFBBAA99UL, 0xFFFFAA22UL};

    // Get standard icon color palette
    unsigned long colors[8];
    get_icon_color_palette(colors);
    int row_bytes;
    long plane_size, total_data_size;
    calculate_icon_plane_dimensions(width, height, depth, &row_bytes, &plane_size, &total_data_size);
    const uint8_t *planes = data;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int color = 0;
            for (int p = 0; p < depth; p++) {
                long offset = p * plane_size + y * row_bytes + (x >> 3);
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

// Parse and render ColorIcon/GlowIcon from IFF FORM ICON chunk
static int parse_coloricon(Display *dpy, const uint8_t *data, long size, long offset, 
                          Pixmap *normal_out, Pixmap *selected_out,
                          uint16_t *width_out, uint16_t *height_out,
                          uint16_t *sel_width_out, uint16_t *sel_height_out,
                          XRenderPictFormat *fmt, const char *icon_path) {
    
    if (offset + 12 > size) return 1;  // Need at least FORM header
    
    if (read_iff_id(data + offset) != IFF_FORM_ID) return 1;
    uint32_t form_size = read_be32(data + offset + 4);
    if (read_iff_id(data + offset + 8) != IFF_ICON_ID) return 1;
    
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
        uint32_t chunk_id = read_iff_id(data + pos);
        uint32_t chunk_size = read_be32(data + pos + 4);
        pos += 8;
        
        
        if (chunk_id == IFF_FACE_ID && chunk_size >= 6) {
            // Update current_face for the next IMAG chunk
            current_face.width_minus_1 = data[pos];
            current_face.height_minus_1 = data[pos + 1];
            current_face.flags = data[pos + 2];
            current_face.aspect_ratio = data[pos + 3];
            current_face.max_palette_minus_1 = read_be16(data + pos + 4);
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
            img.image_size_minus_1 = read_be16(data + pos + 6);
            img.palette_size_minus_1 = read_be16(data + pos + 8);
            
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
                    uint8_t ctrl = read_bits(data + image_offset, 8, bit_offset);
                    bit_offset += 8;
                    
                    if (ctrl > 128) {
                        // Repeat next value (257 - ctrl) times
                        uint8_t value = read_bits(data + image_offset, img.depth, bit_offset);
                        bit_offset += img.depth;
                        int repeat = 257 - ctrl;
                        for (int i = 0; i < repeat && pixel_count < width * height; i++) {
                            pixels[pixel_count++] = value;
                        }
                    } else if (ctrl < 128) {
                        // Copy next (ctrl + 1) values
                        int copy_count = ctrl + 1;
                        for (int i = 0; i < copy_count && pixel_count < width * height; i++) {
                            pixels[pixel_count++] = read_bits(data + image_offset, img.depth, bit_offset);
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
                        uint8_t ctrl = read_bits(data + palette_offset, 8, bit_offset);
                        bit_offset += 8;
                        
                        if (ctrl > 128) {
                            uint8_t value = read_bits(data + palette_offset, 8, bit_offset);
                            bit_offset += 8;
                            int repeat = 257 - ctrl;
                            for (int i = 0; i < repeat && rgb_count < max_rgb_bytes; i++) {
                                rgb[rgb_count++] = value;
                            }
                        } else if (ctrl < 128) {
                            int copy_count = ctrl + 1;
                            for (int i = 0; i < copy_count && rgb_count < max_rgb_bytes; i++) {
                                rgb[rgb_count++] = read_bits(data + palette_offset, 8, bit_offset);
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
            if (XMatchVisualInfo(dpy, DefaultScreen(dpy), ICON_RENDER_DEPTH, TrueColor, &vinfo)) {
                Pixmap pixmap = XCreatePixmap(dpy, DefaultRootWindow(dpy), width, height, ICON_RENDER_DEPTH);
                if (pixmap) {
                    XImage *image = XCreateImage(dpy, vinfo.visual, ICON_RENDER_DEPTH, ZPixmap, 0, 
                                                malloc(width * height * 4), width, height, 32, 0);
                    if (image) {
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
                                    printf("[WARNING] Selected image is fully transparent!\n");
                                } else if (transparent_count > (width * height * 0.9)) {
                                    printf("[WARNING] Selected image is %d%% transparent\n", 
                                           (transparent_count * 100) / (width * height));
                                }
                            }
                        }
                        
                        state_count++;
                    } else {
                        XFreePixmap(dpy, pixmap);
                    }
                }
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

// Create a darkened copy of an image for selected state
// Only darkens non-transparent pixels
static Pixmap create_darkened_pixmap(Display *dpy, Pixmap src, int width, int height) {
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

// Build XRender Pictures for normal/selected from a .info source. If
// the given path is not a .info, fall back to drawer/tool defaults.
void create_icon_images(FileIcon *icon, RenderContext *ctx) {
    if (!icon || !ctx) return;

    const char *icon_path = icon->path;
    if (!strstr(icon_path, ".info")) {
        icon_path = (icon->type == TYPE_DRAWER || icon->type == TYPE_ICONIFIED) ?
                    def_drawer_path : def_tool_path;
    }

    uint8_t *data;
    long size;
    if (load_icon_file(icon_path, &data, &size)) return;

    if (size < 78 || read_be16(data) != 0xE310 || read_be16(data + 2) != 1) {
        free(data);
        return;
    }

    int ic_type = data[0x30];
    int has_drawer_data = (ic_type == 1 || ic_type == 2);
    int header_offset = 78 + (has_drawer_data ? 56 : 0);
    if (header_offset + ICON_HEADER_SIZE > size) {
        free(data);
        return;
    }

    uint16_t width, height, depth;
    if (parse_icon_header(data + header_offset, size - header_offset, &width, &height, &depth)) {
        free(data);
        return;
    }

    Pixmap normal_pixmap;
    if (render_icon(ctx->dpy, &normal_pixmap, data + header_offset + ICON_HEADER_SIZE, width, height, depth)) {
        free(data);
        return;
    }
    icon->normal_picture = XRenderCreatePicture(ctx->dpy, normal_pixmap, ctx->fmt, 0, NULL);

    uint32_t has_selected = read_be32(data + 0x1A);
    if (has_selected) {
        int row_bytes;
        long plane_size, first_data_size;
        calculate_icon_plane_dimensions(width, height, depth, &row_bytes, &plane_size, &first_data_size);
        int second_header_offset = header_offset + ICON_HEADER_SIZE + first_data_size;
        if (second_header_offset + ICON_HEADER_SIZE > size) {
            cleanup_partial_icon(ctx->dpy, icon, normal_pixmap);
            free(data);
            return;
        }

        uint16_t sel_width, sel_height, sel_depth;
        if (parse_icon_header(data + second_header_offset, size - second_header_offset, &sel_width, &sel_height, &sel_depth)) {
            cleanup_partial_icon(ctx->dpy, icon, normal_pixmap);
            free(data);
            return;
        }

        // Allow different sized selected images
        Pixmap selected_pixmap;
        if (render_icon(ctx->dpy, &selected_pixmap, data + second_header_offset + ICON_HEADER_SIZE, sel_width, sel_height, sel_depth)) {
            cleanup_partial_icon(ctx->dpy, icon, normal_pixmap);
            free(data);
            return;
        }
        icon->selected_picture = create_icon_picture(ctx->dpy, selected_pixmap, ctx->fmt);
        icon->sel_width = sel_width;
        icon->sel_height = sel_height;
    } else {
        // No selected image - create darkened version like AmigaOS
        Pixmap dark_pixmap = create_darkened_pixmap(ctx->dpy, normal_pixmap, width, height);
        if (dark_pixmap) {
            icon->selected_picture = create_icon_picture(ctx->dpy, dark_pixmap, ctx->fmt);
            XFreePixmap(ctx->dpy, dark_pixmap);
        } else {
            // Fallback to same image if darkening fails
            icon->selected_picture = icon->normal_picture;
        }
        icon->sel_width = width;
        icon->sel_height = height;
    }

    XFreePixmap(ctx->dpy, normal_pixmap);

    icon->width = width;
    icon->height = height;
    
    // Check for ColorIcon/GlowIcon after the classic icon data
    // Calculate where we are in the file after classic icon(s)
    long classic_end = header_offset + ICON_HEADER_SIZE;
    int row_bytes;
    long plane_size, first_data_size;
    calculate_icon_plane_dimensions(width, height, depth, &row_bytes, &plane_size, &first_data_size);
    classic_end += first_data_size;
    
    if (has_selected) {
        // Skip second classic icon
        int second_header_offset = header_offset + ICON_HEADER_SIZE + first_data_size;
        if (second_header_offset + ICON_HEADER_SIZE <= size) {
            uint16_t sel_width2, sel_height2, sel_depth2;
            if (!parse_icon_header(data + second_header_offset, size - second_header_offset, 
                                   &sel_width2, &sel_height2, &sel_depth2)) {
                int sel_row_bytes;
                long sel_plane_size, sel_data_size;
                calculate_icon_plane_dimensions(sel_width2, sel_height2, sel_depth2, 
                                               &sel_row_bytes, &sel_plane_size, &sel_data_size);
                classic_end = second_header_offset + ICON_HEADER_SIZE + sel_data_size;
            }
        }
    }
    
    // Skip any DefaultTool and ToolTypes data
    uint32_t has_default_tool = read_be32(data + 0x32);
    uint32_t has_tooltypes = read_be32(data + 0x36);
    
    // DefaultTool: 4-byte length, string data (length-1 bytes), zero byte
    if (has_default_tool && classic_end + 4 <= size) {
        uint32_t str_len = read_be32(data + classic_end);
        if (str_len > 0 && str_len < (size - classic_end)) {
            classic_end += 4 + str_len;  // 4 for length + string data (includes zero terminator)
        }
    }
    
    // ToolTypes: stored as (count+1)*4, then array of strings
    if (has_tooltypes && classic_end + 4 <= size) {
        uint32_t tooltypes_value = read_be32(data + classic_end);
        classic_end += 4;
        
        if (tooltypes_value > 0) {
            // Convert to actual count: (value/4) - 1
            uint32_t tooltypes_count = (tooltypes_value / 4) - 1;
            
            if (tooltypes_count > 0 && tooltypes_count < 100) {  // Sanity check
                // Each tooltip is a 4-byte length followed by string data
                for (uint32_t i = 0; i < tooltypes_count && classic_end + 4 <= size; i++) {
                    uint32_t str_len = read_be32(data + classic_end);
                    classic_end += 4;
                    if (str_len > 0 && str_len < (size - classic_end)) {
                        classic_end += str_len;  // String data (includes zero terminator)
                    }
                }
            }
        }
    }
    
    // Skip DrawerData2 if present
    int ic_type2 = data[0x30];
    uint32_t user_data = read_be32(data + 0x2E);
    if ((ic_type2 == 1 || ic_type2 == 2) && user_data && classic_end + 6 <= size) {
        classic_end += 6;  // DrawerData2 is 6 bytes
    }
    
    // Now check for ColorIcon
    if (classic_end + 12 <= size) {
        uint32_t form_id = read_iff_id(data + classic_end);
        if (form_id == IFF_FORM_ID) {
            uint32_t form_size = read_be32(data + classic_end + 4);
            uint32_t icon_id = read_iff_id(data + classic_end + 8);
            printf("[INFO] *** GLOWICON FOUND *** in %s\n", icon_path);
            printf("[INFO]   FORM size: %u bytes, type: %s\n", form_size, 
                   icon_id == IFF_ICON_ID ? "ICON" : "OTHER");
            
            Pixmap color_normal = 0, color_selected = 0;
            uint16_t color_width = 0, color_height = 0;
            uint16_t color_sel_width = 0, color_sel_height = 0;
            
            if (!parse_coloricon(ctx->dpy, data, size, classic_end, 
                               &color_normal, &color_selected,
                               &color_width, &color_height,
                               &color_sel_width, &color_sel_height,
                               ctx->fmt, icon_path)) {
                printf("[INFO]   GlowIcon parsed: %ux%u (selected: %ux%u)\n",
                       color_width, color_height, color_sel_width, color_sel_height);
            
            // Use ColorIcon instead of classic icon
            if (color_normal) {
                // Free classic icon Pictures
                if (icon->normal_picture) {
                    XRenderFreePicture(ctx->dpy, icon->normal_picture);
                }
                if (icon->selected_picture && icon->selected_picture != icon->normal_picture) {
                    XRenderFreePicture(ctx->dpy, icon->selected_picture);
                }
                
                // Use ColorIcon
                icon->normal_picture = XRenderCreatePicture(ctx->dpy, color_normal, ctx->fmt, 0, NULL);
                icon->width = color_width;
                icon->height = color_height;
                
                if (color_selected) {
                    icon->selected_picture = XRenderCreatePicture(ctx->dpy, color_selected, ctx->fmt, 0, NULL);
                    icon->sel_width = color_sel_width;
                    icon->sel_height = color_sel_height;
                    XFreePixmap(ctx->dpy, color_selected);
                } else {
                    // No selected image - create darkened version
                    Pixmap dark_pixmap = create_darkened_pixmap(ctx->dpy, color_normal, color_width, color_height);
                    if (dark_pixmap) {
                        icon->selected_picture = XRenderCreatePicture(ctx->dpy, dark_pixmap, ctx->fmt, 0, NULL);
                        XFreePixmap(ctx->dpy, dark_pixmap);
                    } else {
                        // Fallback to same image if darkening fails
                        icon->selected_picture = icon->normal_picture;
                    }
                    icon->sel_width = color_width;
                    icon->sel_height = color_height;
                }
                
                XFreePixmap(ctx->dpy, color_normal);
                printf("[INFO]   GlowIcon ACTIVE for %s\n", icon_path);
            } else {
                printf("[ERROR]   GlowIcon parse FAILED - using classic icon\n");
            }
        } else {
            printf("[ERROR]   GlowIcon parse returned error\n");
        }
    }
}
    
    icon->current_picture = icon->normal_picture;
    free(data);
}


void free_icon(FileIcon *icon) {
    if (!icon) return;
    Display *dpy = get_display();
    if (!dpy) return;
    if (icon->normal_picture) XRenderFreePicture(dpy, icon->normal_picture);
    if (icon->selected_picture) XRenderFreePicture(dpy, icon->selected_picture);
    icon->normal_picture = None;
    icon->selected_picture = None;
    icon->current_picture = None;
}