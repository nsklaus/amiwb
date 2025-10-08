// File: icon_internal.h
// Internal API shared between icon modules
#ifndef ICON_INTERNAL_H
#define ICON_INTERNAL_H

#include "icon_public.h"
#include "../config.h"
#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>
#include <stdint.h>

// ============================================================================
// Icon Format Detection
// ============================================================================

typedef enum {
    AMIGA_ICON_UNKNOWN = 0,
    AMIGA_ICON_OS13,      // OS 1.3 (userData = 0)
    AMIGA_ICON_OS3,       // OS 3.x (userData = 1, no FORM)
    AMIGA_ICON_MWB,       // MagicWB (OS3 with specific palette)
    AMIGA_ICON_GLOWICON,  // Has FORM/ICON chunk
    AMIGA_ICON_NEWICON,   // Has IM1=/IM2= in tooltypes
    AMIGA_ICON_OS4        // OS4 (future)
} AmigaIconFormat;

// ============================================================================
// IFF/FORM Constants and Structures (GlowIcon/ColorIcon)
// ============================================================================

#define IFF_FORM_ID 0x464F524D  // 'FORM'
#define IFF_ICON_ID 0x49434F4E  // 'ICON'
#define IFF_FACE_ID 0x46414345  // 'FACE'
#define IFF_IMAG_ID 0x494D4147  // 'IMAG'

typedef struct {
    uint8_t width_minus_1;
    uint8_t height_minus_1;
    uint8_t flags;
    uint8_t aspect_ratio;
    uint16_t max_palette_minus_1;
} ColorIconFace;

typedef struct {
    uint8_t transparent_index;
    uint8_t num_colors_minus_1;
    uint8_t flags;
    uint8_t image_compression;
    uint8_t palette_compression;
    uint8_t depth;
    uint16_t image_size_minus_1;
    uint16_t palette_size_minus_1;
} ColorIconImage;

// ============================================================================
// AICON Constants and Structures (PNG-based format)
// ============================================================================

#define AICON_MAGIC 0x4149434F4E  // 'AICON' (5 bytes)
#define AICON_VERSION 1

#define SECTION_PNG_NORMAL   1
#define SECTION_PNG_SELECTED 2
#define SECTION_METADATA     3

typedef struct {
    char magic[5];
    uint8_t version;
    uint16_t num_sections;
} __attribute__((packed)) AiconHeader;

typedef struct {
    uint32_t type;
    uint32_t offset;
    uint32_t size;
} __attribute__((packed)) AiconSectionEntry;

typedef struct {
    int16_t x;
    int16_t y;
    uint8_t flags;
    uint8_t reserved[3];
} __attribute__((packed)) AiconMetadata;

// ============================================================================
// Binary Parsing Utilities (icon_parser.c)
// ============================================================================

uint16_t icon_read_be16(const uint8_t *p);
uint32_t icon_read_be32(const uint8_t *p);
uint32_t icon_read_iff_id(const uint8_t *p);
uint8_t icon_read_bits(const uint8_t *data, int bit_count, int bit_offset);
void icon_calculate_plane_dimensions(uint16_t width, uint16_t height, uint16_t depth,
                                     int *row_bytes, long *plane_size, long *total_data_size);

// ============================================================================
// Format Detection & File Loading (icon_detect.c)
// ============================================================================

AmigaIconFormat icon_detect_format(const uint8_t *data, long size, long *form_offset_out);
int icon_load_file(const char *name, uint8_t **data, long *size);
int icon_parse_header(const uint8_t *header, long size, uint16_t *width, uint16_t *height, uint16_t *depth);

// ============================================================================
// Rendering Infrastructure (icon_render.c)
// ============================================================================

Picture icon_create_picture(Display *dpy, Pixmap pixmap, XRenderPictFormat *fmt);
int icon_create_rendering_context(Display *dpy, uint16_t width, uint16_t height,
                                  Pixmap *pixmap_out, XImage **image_out, XVisualInfo *vinfo_out);
Pixmap icon_create_darkened_pixmap(Display *dpy, Pixmap src, int width, int height);
void icon_cleanup_partial(Display *dpy, FileIcon *icon);
void icon_free_pictures(FileIcon* icon);

// Main rendering dispatcher
int icon_render(Display *dpy, Pixmap *pixmap_out, const uint8_t *data, uint16_t width,
               uint16_t height, uint16_t depth, AmigaIconFormat format, long data_size);

// ============================================================================
// OS 1.3 Format (icon_os13.c)
// ============================================================================

void icon_get_os13_palette(unsigned long colors[4]);
void icon_get_mwb_palette(unsigned long colors[8]);
int icon_render_os13(Display *dpy, Pixmap *pixmap_out, const uint8_t *data,
                     uint16_t width, uint16_t height);

// ============================================================================
// GlowIcon/ColorIcon Format (icon_glowicon.c)
// ============================================================================

int icon_parse_glowicon(Display *dpy, const uint8_t *data, long size, long offset,
                        Pixmap *normal_out, Pixmap *selected_out,
                        uint16_t *width_out, uint16_t *height_out,
                        uint16_t *sel_width_out, uint16_t *sel_height_out,
                        XRenderPictFormat *fmt, const char *icon_path);

// ============================================================================
// AICON Format (icon_aicon.c)
// ============================================================================

int icon_load_aicon(FileIcon *icon, RenderContext *ctx,
                    const uint8_t *data, long size);

#endif
