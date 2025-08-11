// File: icons.c
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
#define ICON_HEADER_SIZE 20
#define GLOBAL_DEPTH 32 // Match global depth

// TODO: refactor the file to use def_dir and def_foo
static char *def_tool_path = "/usr/local/share/amiwb/icons/def_icons/def_foo.info";
static char *def_drawer_path = "/usr/local/share/amiwb/icons/def_icons/def_dir.info";

static uint16_t read_be16(const uint8_t *p) {
    return (p[0] << 8) | p[1];
}

static uint32_t read_be32(const uint8_t *p) {
    return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
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
    if (!XMatchVisualInfo(dpy, DefaultScreen(dpy), GLOBAL_DEPTH, TrueColor, &vinfo)) {
        fprintf(stderr, "No %d-bit TrueColor visual found for icon\n", GLOBAL_DEPTH);
        return 1;
    }
    Pixmap pixmap = XCreatePixmap(dpy, DefaultRootWindow(dpy), width, height, GLOBAL_DEPTH);
    if (!pixmap) return 1;

    XImage *image = XCreateImage(dpy, vinfo.visual, GLOBAL_DEPTH, ZPixmap, 0, malloc(width * height * 4), width, height, 32, 0);
    if (!image) {
        XFreePixmap(dpy, pixmap);
        return 1;
    }
    memset(image->data, 0, width * height * 4);

    // Icons can use true alpha; index 0 would be transparent. We use a
    // gray fill for now to match classic look; adjust when alpha lands.
    // unsigned long colors[8] = {0x00000000UL, 0xFF000000UL, 0xFFFFFFFFUL, 0xFF6666BBUL, 0xFF999999UL, 0xFFBBBBBBUL, 0xFFBBAA99UL, 0xFFFFAA22UL};

    // icons use gray fill instead of transparency
    unsigned long colors[8] = {0xFFA0A2A0UL, 0xFF000000, 0xFFFFFFFF, 0xFF6666BB, 0xFF999999, 0xFFBBBBBB, 0xFFBBAA99, 0xFFFFAA22};
    int row_bytes = ((width + 15) / 16) * 2;
    long plane_size = row_bytes * height;
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
        int row_bytes = ((width + 15) / 16) * 2;
        long plane_size = row_bytes * height;
        long first_data_size = plane_size * depth;
        int second_header_offset = header_offset + ICON_HEADER_SIZE + first_data_size;
        if (second_header_offset + ICON_HEADER_SIZE > size) {
            XRenderFreePicture(ctx->dpy, icon->normal_picture);
            icon->normal_picture = None;
            XFreePixmap(ctx->dpy, normal_pixmap);  
            free(data);
            return;
        }

        uint16_t sel_width, sel_height, sel_depth;
        if (parse_icon_header(data + second_header_offset, size - second_header_offset, &sel_width, &sel_height, &sel_depth)) {
            XRenderFreePicture(ctx->dpy, icon->normal_picture);
            icon->normal_picture = None;
            XFreePixmap(ctx->dpy, normal_pixmap);  
            free(data);
            return;
        }

        if (sel_width != width || sel_height != height || sel_depth != depth) {
            icon->selected_picture = None;
        } else {
            Pixmap selected_pixmap;
            if (render_icon(ctx->dpy, &selected_pixmap, data + second_header_offset + ICON_HEADER_SIZE, sel_width, sel_height, sel_depth)) {
                XRenderFreePicture(ctx->dpy, icon->normal_picture);
                icon->normal_picture = None;
                XFreePixmap(ctx->dpy, normal_pixmap); 
                free(data);
                return;
            }
            icon->selected_picture = XRenderCreatePicture(ctx->dpy, selected_pixmap, ctx->fmt, 0, NULL);
            XFreePixmap(ctx->dpy, selected_pixmap);
        }
    } else {
        icon->selected_picture = XRenderCreatePicture(ctx->dpy, normal_pixmap, ctx->fmt, 0, NULL);
        XRenderColor tint = {0x0000, 0x0000, 0xFFFF, 0x8000};
        XRenderFillRectangle(ctx->dpy, PictOpOver, icon->selected_picture, &tint, 0, 0, width, height);
    }

    XFreePixmap(ctx->dpy, normal_pixmap);

    icon->width = width;
    icon->height = height;
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