#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include "icons.h"
#include "events.h"

#define ICON_HEADER_SIZE 20

// Read 16-bit big-endian value
static uint16_t read_be16(const uint8_t *p) {
    return (p[0] << 8) | p[1];
}

// Load icon file data
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

// Parse icon header
static int parse_icon_header(const uint8_t *header, long size, uint16_t *width, uint16_t *height, uint16_t *depth) {
    if (size < ICON_HEADER_SIZE) return 1;
    *width = read_be16(header + 4);
    *height = read_be16(header + 6);
    *depth = read_be16(header + 8);
    if (*width == 0 || *height == 0 || *depth == 0 || *depth > 8) return 1;
    return 0;
}

// Render icon into XImage
static int render_icon(Display *dpy, Icon *icon, const uint8_t *data, uint16_t width, uint16_t height, uint16_t depth) {
    XVisualInfo vinfo;
    if (!XMatchVisualInfo(dpy, DefaultScreen(dpy), 32, TrueColor, &vinfo)) return 1;
    icon->image = XCreateImage(dpy, vinfo.visual, 32, ZPixmap, 0, malloc(width * height * 4), width, height, 32, 0);
    if (!icon->image) return 1;
    memset(icon->image->data, 0, width * height * 4);
    unsigned long colors[] = {0xFFAAAAAA, 0xFF000000, 0xFFFFFFFF, 0xFF6666BB, 0xFF999999, 0xFFBBBBBB, 0xFFBBAA99, 0xFFFFAA22};
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
            XPutPixel(icon->image, x, y, colors[color & 7]);
        }
    }
    icon->width = width;
    icon->height = height;
    return 0;
}

// Load icon from .info file
int load_icon(Display *dpy, const char *name, Icon *icon) {
    uint8_t *data;
    long size;
    if (load_icon_file(name, &data, &size)) return 1;
    int header_offset = (data[48] == 1 || data[48] == 2) ? 78 + 56 : 78;
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

// Truncate filename for display
static char *truncate_filename(const char *filename) {
    int len = strlen(filename);
    if (len <= 15) return strdup(filename);
    char *truncated = malloc(16);
    if (!truncated) return NULL;
    strncpy(truncated, filename, 12);
    truncated[12] = '\0';
    strcat(truncated, "...");
    return truncated;
}

// Add icon to array
void add_icon(Display *dpy, Window parent, const char *path, const char *filename, int is_dir,
              FileIcon **icons, int *num_icons, unsigned long label_color, int is_desktop,
              XFontStruct *font) {
    FileIcon *new_icons = realloc(*icons, (*num_icons + 1) * sizeof(FileIcon));
    if (!new_icons) return;
    *icons = new_icons;
    FileIcon *icon = &(*icons)[*num_icons];
    icon->filename = strdup(filename);
    icon->path = strdup(path);
    icon->label = is_desktop ? strdup(filename) : truncate_filename(filename);
    if (!icon->filename || !icon->path || !icon->label) {
        free(icon->filename);
        free(icon->path);
        free(icon->label);
        return;
    }
    icon->type = is_dir ? 1 : (strlen(filename) > 5 && strcmp(filename + strlen(filename) - 5, ".info") == 0 ? 2 : 0);
    icon->x = (*num_icons % 5) * 74 + 10;
    icon->y = (*num_icons / 5) * 74 + (is_desktop ? 40 : 10);
    icon->gc = NULL;
    icon->shape_mask = None;

    char full_path[512];
    char *icon_path = (icon->type == 2 || !is_desktop) ? full_path : def_tool_path;
    snprintf(full_path, sizeof(full_path), "%s/%s%s", path, filename, icon->type == 2 ? "" : ".info");
    if (icon->type != 2 && is_desktop) {
        FILE *fp = fopen(full_path, "rb");
        if (!fp) icon_path = is_dir ? def_drawer_path : def_tool_path;
        else fclose(fp);
    }
    if (load_icon(dpy, icon_path, &icon->icon)) {
        free(icon->filename);
        free(icon->path);
        free(icon->label);
        return;
    }

    int label_width = font ? XTextWidth(font, icon->label, strlen(icon->label)) : strlen(icon->label) * 8;
    icon->width = icon->icon.width > label_width ? icon->icon.width : label_width;
    icon->height = icon->icon.height + 20;

    XSetWindowAttributes attrs = { .background_pixel = None, .border_pixel = 0 };
    XVisualInfo vinfo;
    int depth = DefaultDepth(dpy, DefaultScreen(dpy));
    Visual *visual = DefaultVisual(dpy, DefaultScreen(dpy));
    if (XMatchVisualInfo(dpy, DefaultScreen(dpy), 32, TrueColor, &vinfo)) {
        visual = vinfo.visual;
        depth = vinfo.depth;
        attrs.colormap = XCreateColormap(dpy, parent, vinfo.visual, AllocNone);
    }
    icon->window = XCreateWindow(dpy, parent, icon->x, icon->y, icon->width, icon->height, 0,
                                 depth, InputOutput, visual,
                                 CWColormap | CWBackPixel | CWBorderPixel, &attrs);
    XGCValues gc_values = { .graphics_exposures = False, .font = font ? font->fid : XLoadFont(dpy, "fixed"), .foreground = label_color };
    icon->gc = XCreateGC(dpy, icon->window, GCGraphicsExposures | GCFont | GCForeground, &gc_values);
    if (!icon->gc) {
        XDestroyWindow(dpy, icon->window);
        free_icon(dpy, icon);
        return;
    }
    if (!is_desktop) {
        attrs.override_redirect = True;
        XChangeWindowAttributes(dpy, icon->window, CWOverrideRedirect, &attrs);
    }
    XSelectInput(dpy, icon->window, ButtonPressMask | ButtonReleaseMask | PointerMotionMask | ExposureMask);
    icon->shape_mask = XCreatePixmap(dpy, icon->window, icon->width, icon->height, 1);
    GC shape_gc = XCreateGC(dpy, icon->shape_mask, 0, NULL);
    if (!shape_gc) {
        XDestroyWindow(dpy, icon->window);
        free_icon(dpy, icon);
        return;
    }
    XSetForeground(dpy, shape_gc, 0);
    XFillRectangle(dpy, icon->shape_mask, shape_gc, 0, 0, icon->width, icon->height);
    XSetForeground(dpy, shape_gc, 1);
    XFillRectangle(dpy, icon->shape_mask, shape_gc, 0, 0, icon->icon.width, icon->icon.height);
    if (font) XSetFont(dpy, shape_gc, font->fid);
    XSetForeground(dpy, shape_gc, label_color);
    XDrawString(dpy, icon->shape_mask, shape_gc, (icon->width - label_width) / 2, icon->icon.height + 15, icon->label, strlen(icon->label));
    XShapeCombineMask(dpy, icon->window, ShapeBounding, 0, 0, icon->shape_mask, ShapeSet);
    XFreeGC(dpy, shape_gc);
    XMapWindow(dpy, icon->window);
    restack_windows(dpy);  // Ensure icon at bottom
    (*num_icons)++;
}

// Scan directory for icons
void scan_icons(Display *dpy, Window parent, const char *path, FileIcon **icons,
                int *num_icons, unsigned long label_color, int is_desktop, XFontStruct *font) {
    DIR *dir = opendir(path);
    if (!dir) return;
    struct dirent *entry;
    char *files[1024];
    int file_types[1024], file_count = 0;
    while ((entry = readdir(dir)) && file_count < 1024) {
        if (entry->d_name[0] == '.') continue;
        if (entry->d_type == DT_REG || entry->d_type == DT_DIR) {
            files[file_count] = strdup(entry->d_name);
            file_types[file_count] = (entry->d_type == DT_DIR) ? 1 : (strlen(entry->d_name) > 5 && strcmp(entry->d_name + strlen(entry->d_name) - 5, ".info") == 0) ? 2 : 0;
            file_count++;
        }
    }
    closedir(dir);

    for (int i = 0; i < file_count; i++) {
        if (file_types[i] == 2) {
            char base_name[256];
            strncpy(base_name, files[i], sizeof(base_name) - 1);
            base_name[strlen(base_name) - 5] = '\0';
            int has_match = 0;
            for (int j = 0; j < file_count; j++) {
                if (i != j && (file_types[j] == 0 || file_types[j] == 1) && strcmp(base_name, files[j]) == 0) {
                    has_match = 1;
                    break;
                }
            }
            if (!has_match) add_icon(dpy, parent, path, files[i], 0, icons, num_icons, label_color, is_desktop, font);
        } else {
            add_icon(dpy, parent, path, files[i], file_types[i] == 1, icons, num_icons, label_color, is_desktop, font);
        }
    }
    for (int i = 0; i < file_count; i++) free(files[i]);
}

// Free icon resources
void free_icon(Display *dpy, FileIcon *icon) {
    if (icon->icon.image) XDestroyImage(icon->icon.image);
    if (icon->gc) XFreeGC(dpy, icon->gc);
    if (icon->shape_mask) XFreePixmap(dpy, icon->shape_mask);
    free(icon->filename);
    free(icon->path);
    free(icon->label);
}
