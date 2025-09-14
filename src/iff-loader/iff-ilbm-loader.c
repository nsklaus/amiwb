/*
 * IFF ILBM gdk-pixbuf loader
 * Adds support for Amiga IFF ILBM images to GTK applications
 *
 * Based on planar-to-chunky conversion from AmiWB icons.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <arpa/inet.h>  // for ntohl/ntohs
#define GDK_PIXBUF_ENABLE_BACKEND
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk-pixbuf/gdk-pixbuf-io.h>

// IFF chunk IDs
#define ID_FORM 0x464F524D  // 'FORM'
#define ID_ILBM 0x494C424D  // 'ILBM'
#define ID_BMHD 0x424D4844  // 'BMHD'
#define ID_CMAP 0x434D4150  // 'CMAP'
#define ID_BODY 0x424F4459  // 'BODY'

// BMHD - BitMap Header
typedef struct {
    uint16_t width;
    uint16_t height;
    int16_t  x;
    int16_t  y;
    uint8_t  num_planes;
    uint8_t  masking;
    uint8_t  compression;
    uint8_t  pad1;
    uint16_t transparent_color;
    uint8_t  x_aspect;
    uint8_t  y_aspect;
    int16_t  page_width;
    int16_t  page_height;
} __attribute__((packed)) BMHD;

// Context for progressive loading
typedef struct {
    GdkPixbufModuleSizeFunc     size_func;
    GdkPixbufModulePreparedFunc prepared_func;
    GdkPixbufModuleUpdatedFunc  updated_func;
    gpointer                    user_data;

    GdkPixbuf *pixbuf;
    uint8_t   *buffer;
    size_t    buffer_size;
    size_t    buffer_used;

    // IFF parsing state
    gboolean  found_form;
    gboolean  found_ilbm;
    gboolean  header_loaded;
    BMHD      bmhd;
    uint8_t   cmap[256][3];
    int       num_colors;
} IffContext;

// Read 32-bit big-endian value
static uint32_t read_be32(const uint8_t *data) {
    return ntohl(*(uint32_t*)data);
}

// Read 16-bit big-endian value
static uint16_t read_be16(const uint8_t *data) {
    return ntohs(*(uint16_t*)data);
}

// Decompress ByteRun1 compressed data
static int decompress_byterun1(uint8_t *dest, const uint8_t *src, int dest_size, int src_size) {
    int src_pos = 0;
    int dest_pos = 0;

    while (src_pos < src_size && dest_pos < dest_size) {
        int8_t cmd = (int8_t)src[src_pos++];

        if (cmd >= 0) {
            // Copy next n+1 bytes literally
            int count = cmd + 1;
            if (src_pos + count > src_size || dest_pos + count > dest_size)
                break;
            memcpy(&dest[dest_pos], &src[src_pos], count);
            src_pos += count;
            dest_pos += count;
        } else if (cmd != -128) {
            // Repeat next byte -n+1 times
            int count = -cmd + 1;
            if (src_pos >= src_size || dest_pos + count > dest_size)
                break;
            memset(&dest[dest_pos], src[src_pos++], count);
            dest_pos += count;
        }
    }

    return dest_pos;
}

// Convert planar bitmap to chunky RGB
static void planar_to_chunky(uint8_t *rgb_data, const uint8_t *planar_data,
                              int width, int height, int num_planes,
                              uint8_t cmap[][3], int row_stride) {
    int row_bytes = ((width + 15) >> 4) << 1;  // Round up to word boundary
    int plane_size = row_bytes * height;

    for (int y = 0; y < height; y++) {
        uint8_t *out_row = rgb_data + y * row_stride;

        for (int x = 0; x < width; x++) {
            int byte_offset = x >> 3;
            int bit_offset = 7 - (x & 7);
            uint8_t color_index = 0;

            // Combine bits from each plane
            for (int plane = 0; plane < num_planes; plane++) {
                const uint8_t *plane_ptr = planar_data + plane * plane_size;
                uint8_t bit = (plane_ptr[y * row_bytes + byte_offset] >> bit_offset) & 1;
                color_index |= (bit << plane);
            }

            // Write RGB pixel
            *out_row++ = cmap[color_index][0];
            *out_row++ = cmap[color_index][1];
            *out_row++ = cmap[color_index][2];
        }
    }
}

// Parse IFF chunks and load image
static gboolean parse_iff_data(IffContext *context) {
    const uint8_t *data = context->buffer;
    size_t size = context->buffer_used;
    size_t pos = 0;

    // Need at least FORM header (12 bytes)
    if (size < 12)
        return TRUE;  // Need more data

    // Check for FORM ILBM
    if (read_be32(&data[0]) != ID_FORM)
        return FALSE;  // Not IFF

    uint32_t form_size = read_be32(&data[4]);
    if (read_be32(&data[8]) != ID_ILBM)
        return FALSE;  // Not ILBM

    pos = 12;
    context->found_form = TRUE;
    context->found_ilbm = TRUE;

    // Parse chunks
    while (pos + 8 <= size) {
        uint32_t chunk_id = read_be32(&data[pos]);
        uint32_t chunk_size = read_be32(&data[pos + 4]);
        pos += 8;

        // Align to word boundary
        uint32_t padded_size = (chunk_size + 1) & ~1;

        if (pos + padded_size > size)
            return TRUE;  // Need more data

        switch (chunk_id) {
            case ID_BMHD:
                if (chunk_size >= sizeof(BMHD)) {
                    memcpy(&context->bmhd, &data[pos], sizeof(BMHD));
                    // Convert from big-endian
                    context->bmhd.width = read_be16((uint8_t*)&context->bmhd.width);
                    context->bmhd.height = read_be16((uint8_t*)&context->bmhd.height);
                    context->bmhd.x = (int16_t)read_be16((uint8_t*)&context->bmhd.x);
                    context->bmhd.y = (int16_t)read_be16((uint8_t*)&context->bmhd.y);
                    context->bmhd.transparent_color = read_be16((uint8_t*)&context->bmhd.transparent_color);
                    context->bmhd.page_width = (int16_t)read_be16((uint8_t*)&context->bmhd.page_width);
                    context->bmhd.page_height = (int16_t)read_be16((uint8_t*)&context->bmhd.page_height);
                    context->header_loaded = TRUE;
                }
                break;

            case ID_CMAP:
                context->num_colors = chunk_size / 3;
                if (context->num_colors > 256)
                    context->num_colors = 256;
                for (int i = 0; i < context->num_colors; i++) {
                    context->cmap[i][0] = data[pos + i*3];
                    context->cmap[i][1] = data[pos + i*3 + 1];
                    context->cmap[i][2] = data[pos + i*3 + 2];
                }
                break;

            case ID_BODY:
                if (context->header_loaded) {
                    // Create pixbuf if not already created
                    if (!context->pixbuf) {
                        context->pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8,
                                                         context->bmhd.width,
                                                         context->bmhd.height);
                        if (!context->pixbuf)
                            return FALSE;

                        // Notify that we're ready
                        if (context->prepared_func) {
                            (*context->prepared_func)(context->pixbuf, NULL, context->user_data);
                        }
                    }

                    // Decode image data
                    uint8_t *pixels = gdk_pixbuf_get_pixels(context->pixbuf);
                    int row_stride = gdk_pixbuf_get_rowstride(context->pixbuf);
                    int row_bytes = ((context->bmhd.width + 15) >> 4) << 1;
                    int plane_size = row_bytes * context->bmhd.height;
                    int total_size = plane_size * context->bmhd.num_planes;

                    if (context->bmhd.compression == 0) {
                        // Uncompressed
                        if (chunk_size >= total_size) {
                            planar_to_chunky(pixels, &data[pos],
                                           context->bmhd.width, context->bmhd.height,
                                           context->bmhd.num_planes, context->cmap,
                                           row_stride);
                        }
                    } else if (context->bmhd.compression == 1) {
                        // ByteRun1 compression
                        uint8_t *uncompressed = g_malloc(total_size);
                        decompress_byterun1(uncompressed, &data[pos], total_size, chunk_size);
                        planar_to_chunky(pixels, uncompressed,
                                       context->bmhd.width, context->bmhd.height,
                                       context->bmhd.num_planes, context->cmap,
                                       row_stride);
                        g_free(uncompressed);
                    }

                    // Notify update
                    if (context->updated_func) {
                        (*context->updated_func)(context->pixbuf, 0, 0,
                                               context->bmhd.width, context->bmhd.height,
                                               context->user_data);
                    }

                    return TRUE;  // Successfully loaded
                }
                break;
        }

        pos += padded_size;
    }

    return TRUE;  // Need more data or done
}

// GdkPixbuf loader functions
static gpointer gdk_pixbuf__iff_image_begin_load(GdkPixbufModuleSizeFunc size_func,
                                                 GdkPixbufModulePreparedFunc prepared_func,
                                                 GdkPixbufModuleUpdatedFunc updated_func,
                                                 gpointer user_data,
                                                 GError **error) {
    IffContext *context = g_new0(IffContext, 1);
    context->size_func = size_func;
    context->prepared_func = prepared_func;
    context->updated_func = updated_func;
    context->user_data = user_data;

    // Initialize default grayscale palette
    for (int i = 0; i < 256; i++) {
        context->cmap[i][0] = i;
        context->cmap[i][1] = i;
        context->cmap[i][2] = i;
    }

    return context;
}

static gboolean gdk_pixbuf__iff_image_load_increment(gpointer data,
                                                     const guchar *buf,
                                                     guint size,
                                                     GError **error) {
    IffContext *context = (IffContext *)data;

    // Grow buffer
    context->buffer = g_realloc(context->buffer, context->buffer_used + size);
    memcpy(context->buffer + context->buffer_used, buf, size);
    context->buffer_used += size;

    // Try to parse
    if (!parse_iff_data(context)) {
        g_set_error(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
                   "Invalid IFF ILBM file");
        return FALSE;
    }

    return TRUE;
}

static gboolean gdk_pixbuf__iff_image_stop_load(gpointer data, GError **error) {
    IffContext *context = (IffContext *)data;
    gboolean retval = TRUE;

    if (!context->pixbuf) {
        g_set_error(error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
                   "Incomplete IFF ILBM file");
        retval = FALSE;
    }

    g_free(context->buffer);
    g_free(context);
    return retval;
}

// Module entry points
G_MODULE_EXPORT void fill_vtable(GdkPixbufModule *module) {
    module->begin_load = gdk_pixbuf__iff_image_begin_load;
    module->load_increment = gdk_pixbuf__iff_image_load_increment;
    module->stop_load = gdk_pixbuf__iff_image_stop_load;
}

G_MODULE_EXPORT void fill_info(GdkPixbufFormat *info) {
    static GdkPixbufModulePattern signature[] = {
        { "FORM", "    ", 100 },  // IFF signature
        { NULL, NULL, 0 }
    };

    static gchar *mime_types[] = {
        "image/x-ilbm",
        "image/x-iff",
        NULL
    };

    static gchar *extensions[] = {
        "iff",
        "ilbm",
        "lbm",
        NULL
    };

    info->name = "iff";
    info->signature = signature;
    info->description = "IFF ILBM (Amiga Interleaved Bitmap)";
    info->mime_types = mime_types;
    info->extensions = extensions;
    info->flags = GDK_PIXBUF_FORMAT_THREADSAFE;
    info->license = "GPL";
}