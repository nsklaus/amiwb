/* Icons implementation: Loads, parses, renders icons from files, manages resources, and recreates pixmaps with labels. Handles icon data. */

#include "icons.h"
#include "intuition.h"
#include "config.h"
#include <stdio.h>  // File I/O.
#include <stdlib.h>  // Memory alloc.
#include <string.h>  // String ops.
#include <stdint.h>  // Uint types.

char *def_tool_path;  // Path to default icons
char *def_drawer_path;  
char *iconify_path;  // Iconify icon.

// Read big-endian 16-bit value from bytes.
static uint16_t read_be16(const uint8_t *p) {
    return (p[0] << 8) | p[1];
}

// Load file data into buffer.
static int load_icon_file(const char *name, uint8_t **data, long *size) {
    FILE *fp = fopen(name, "rb");  // Open binary read.
    if (!fp) return 1;  // Fail.
    fseek(fp, 0, SEEK_END);  // Seek to end.
    *size = ftell(fp);  // Get size.
    rewind(fp);  // Back to start.
    *data = malloc(*size);  // Alloc buffer.
    if (!*data || fread(*data, 1, *size, fp) != *size) {  // Read check.
        free(*data);
        fclose(fp);
        return 1;
    }
    fclose(fp);
    return 0;
}

// Parse icon header for width, height, depth.
static int parse_icon_header(const uint8_t *header, long size, uint16_t *width, uint16_t *height, uint16_t *depth) {
    if (size < ICON_HEADER_SIZE) return 1;  // Too small.
    *width = read_be16(header + 4);
    *height = read_be16(header + 6);
    *depth = read_be16(header + 8);
    if (*width == 0 || *height == 0 || *depth == 0 || *depth > 8) return 1;  // Invalid.
    return 0;
}

// Render icon data to XImage with palette.
static int render_icon(Display *dpy, Icon *icon, const uint8_t *data, uint16_t width, uint16_t height, uint16_t depth) {
    XVisualInfo vinfo;
    if (!XMatchVisualInfo(dpy, DefaultScreen(dpy), 32, TrueColor, &vinfo)) return 1;  // Find 32-bit visual.
    icon->image = XCreateImage(dpy, vinfo.visual, 32, ZPixmap, 0, malloc(width * height * 4), width, height, 32, 0);  // Create image.
    if (!icon->image) return 1;
    memset(icon->image->data, 0, width * height * 4);  // Clear.
    unsigned long colors[8] = {0x00000000, 0xFF000000, 0xFFFFFFFF, 0xFF6666BB, 0xFF999999, 0xFFBBBBBB, 0xFFBBAA99, 0xFFFFAA22};  // Amiga-like palette.
    int row_bytes = ((width + 15) / 16) * 2;  // Row bytes for planar data.
    long plane_size = row_bytes * height;  // Plane size.
    const uint8_t *planes = data;  // Data planes.
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int color = 0;
            for (int p = 0; p < depth; p++) {  // Build color from planes.
                long offset = p * plane_size + y * row_bytes + (x >> 3);
                uint8_t byte = planes[offset];
                if (byte & (1 << (7 - (x & 7)))) color |= (1 << p);
            }
            XPutPixel(icon->image, x, y, colors[color & 7]);  // Set pixel.
        }
    }
    icon->width = width;
    icon->height = height;
    return 0;
}

// Load icon from file, parse, render.
int load_icon(Display *dpy, const char *name, Icon *icon) {
    uint8_t *data;
    long size;
    if (load_icon_file(name, &data, &size)) return 1;
    int header_offset = (data[48] == 1 || data[48] == 2) ? 78 + 56 : 78;  // Offset based on format.
    if (header_offset + ICON_HEADER_SIZE > size) {
        free(data);
        return 1;
    }
    uint16_t width, height, depth;
    if (parse_icon_header(data + header_offset, size - header_offset, &width, &height, &depth) ||
        render_icon(dpy, icon, data + header_offset + ICON_HEADER_SIZE, width, height, depth)) {
        free(data);
        return 1;
    }
    free(data);
    return 0;
}

// Free icon resources.
void free_icon(Display *dpy, FileIcon *icon) {
    if (icon->picture) XRenderFreePicture(dpy, icon->picture);
    if (icon->pixmap) XFreePixmap(dpy, icon->pixmap);
    if (icon->icon.image) XDestroyImage(icon->icon.image);
    free(icon->filename);
    free(icon->path);
    free(icon->label);
    free(icon->display_label);
    free(icon->icon_path);
}

// Recreate icon pixmap with image and label.
void recreate_icon_pixmap(RenderContext *ctx, FileIcon *icon, Canvas *canvas) {
    if (icon->picture) {
        XRenderFreePicture(ctx->dpy, icon->picture);
        icon->picture = 0;
    }
    if (icon->pixmap) {
        XFreePixmap(ctx->dpy, icon->pixmap);
        icon->pixmap = 0;
    }
    Icon base_icon;
    if (load_icon(ctx->dpy, icon->icon_path, &base_icon)) return;  // Load base.
    icon->icon.width = base_icon.width;
    icon->icon.height = base_icon.height;
    icon->pixmap = XCreatePixmap(ctx->dpy, canvas->win, icon->width, icon->height, 32);
    XRenderPictureAttributes pa = {.repeat = True};
    icon->picture = XRenderCreatePicture(ctx->dpy, icon->pixmap, ctx->fmt, CPRepeat, &pa);
    XRenderColor clear = {0, 0, 0, 0};
    XRenderFillRectangle(ctx->dpy, PictOpSrc, icon->picture, &clear, 0, 0, icon->width, icon->height);
    GC gc = XCreateGC(ctx->dpy, icon->pixmap, 0, NULL);
    int img_x = (icon->width - base_icon.width) / 2;
    XPutImage(ctx->dpy, icon->pixmap, gc, base_icon.image, 0, 0, img_x, 0, base_icon.width, base_icon.height);
    XFreeGC(ctx->dpy, gc);
    XDestroyImage(base_icon.image);
    XftDraw *draw = XftDrawCreate(ctx->dpy, icon->pixmap, ctx->visual, ctx->cmap);  // Draw for text.
    XGlyphInfo extents;
    XftTextExtentsUtf8(ctx->dpy, ctx->font, (FcChar8 *)icon->display_label, strlen(icon->display_label), &extents);
    int text_width = extents.xOff;
    int text_x = (icon->width - text_width) / 2;
    int text_height = ctx->font->height;
    XftDrawStringUtf8(draw, &ctx->label_color, ctx->font, text_x, icon->icon.height + text_height + 2, (FcChar8 *)icon->display_label, strlen(icon->display_label));
    XftDrawDestroy(draw);
    XSync(ctx->dpy, False);
}