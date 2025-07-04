#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include "icon_loader.h"

// Constants for Amiga .info file structure
#define ICON_HEADER_OFFSET 78
#define ICON_HEADER_SIZE 20

// Read 16-bit big-endian integer
static uint16_t read_be16(const uint8_t *p) {
    return (p[0] << 8) | p[1];
}

// Load .info file
static int load_icon_file(const char *name, uint8_t **data, long *size) {
    char filename[256];
    snprintf(filename, sizeof(filename), "%s.info", name);
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        perror("[load_do] Failed to open file");
        return 1;
    }
    fseek(fp, 0, SEEK_END);
    *size = ftell(fp);
    rewind(fp);
    *data = malloc(*size);
    if (!*data) {
        fclose(fp);
        return 1;
    }
    if (fread(*data, 1, *size, fp) != *size) {
        fprintf(stderr, "[load_do] Failed to read file\n");
        free(*data);
        fclose(fp);
        return 1;
    }
    fclose(fp);
    return 0;
}

// Parse icon header
static int parse_icon_header(const uint8_t *data, long size, uint16_t *width, uint16_t *height, uint16_t *depth) {
    if (ICON_HEADER_OFFSET + ICON_HEADER_SIZE > size) {
        fprintf(stderr, "[load_do] Invalid icon file: too short\n");
        return 1;
    }
    *width = read_be16(data + ICON_HEADER_OFFSET + 4);
    *height = read_be16(data + ICON_HEADER_OFFSET + 6);
    *depth = read_be16(data + ICON_HEADER_OFFSET + 8);
    if (*width == 0 || *height == 0 || *depth == 0 || *depth > 8) {
        fprintf(stderr, "[load_do] Invalid dimensions: Width=%d Height=%d Depth=%d\n", *width, *height, *depth);
        return 1;
    }
    return 0;
}

// Render icon
static int render_icon(Display *dpy, Window window, GC gc, Icon *icon, const uint8_t *data, uint16_t width, uint16_t height, uint16_t depth) {
    Visual *visual = DefaultVisual(dpy, DefaultScreen(dpy));
    int xdepth = DefaultDepth(dpy, DefaultScreen(dpy));
    icon->image = XCreateImage(dpy, visual, xdepth, ZPixmap, 0, malloc(width * height * 4), width, height, 32, 0);
    if (!icon->image) {
        fprintf(stderr, "[load_do] Failed to create XImage\n");
        return 1;
    }
    unsigned long iconcolor[8] = {11184810, 0, 16777215, 6719675, 10066329, 12303291, 12298905, 16759722};
    int row_bytes = ((width + 15) / 16) * 2;
    long plane_size = row_bytes * height;
    const uint8_t *planes = data + ICON_HEADER_OFFSET + ICON_HEADER_SIZE;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int color = 0;
            for (int p = 0; p < depth; ++p) {
                long row_offset = p * plane_size + y * row_bytes;
                int byte_index = row_offset + (x >> 3);
                uint8_t byte = planes[byte_index];
                int bit = 7 - (x & 7); // MSB-first
                if (byte & (1 << bit)) color |= (1 << p);
            }
            XPutPixel(icon->image, x, y, iconcolor[color & 7]);
        }
    }
    XPutImage(dpy, window, gc, icon->image, 0, 0, 0, 0, width, height);
    XFlush(dpy);
    return 0;
}

// Main function to load and display icon
int load_do(Display *dpy, Window root, GC gc, const char *name, Icon *icon) {
    uint8_t *data;
    long size;
    if (load_icon_file(name, &data, &size)) return 1;
    uint16_t width, height, depth;
    if (parse_icon_header(data, size, &width, &height, &depth)) {
        free(data);
        return 1;
    }
    icon->x = 10;
    icon->y = 10;
    icon->width = width;
    icon->height = height;
    icon->window = XCreateSimpleWindow(dpy, root, icon->x, icon->y, width, height, 0,
                                      BlackPixel(dpy, DefaultScreen(dpy)),
                                      WhitePixel(dpy, DefaultScreen(dpy)));
    XSelectInput(dpy, icon->window, ButtonPressMask | ExposureMask);
    XMapWindow(dpy, icon->window);
    XLowerWindow(dpy, icon->window); // Ensure icon stays at bottom
    if (render_icon(dpy, icon->window, gc, icon, data, width, height, depth)) {
        free(data);
        return 1;
    }
    free(data);
    return 0;
}