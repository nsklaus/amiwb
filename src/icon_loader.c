// icon_loader.c - parse and load amiga icon image

#include <stdio.h>        // standard I/O functions (fprintf, ..)
#include <stdlib.h>       // standard library functions (malloc, ..)
#include <stdint.h>       // standard integer types (uint8_t, ..)
#include <X11/Xlib.h>     // Xlib for X11 functions
#include <X11/Xutil.h>    // Xutil for utility functions (XCreateImage, ..)
#include "icon_loader.h"  // icon loader declarations (Icon struct, load_do)


// define size of icon header in bytes
#define ICON_HEADER_SIZE 20

// define function to read 16-bit big-endian integer
static uint16_t read_be16(const uint8_t *p) {

    // combine two bytes into a 16-bit value
    return (p[0] << 8) | p[1];

}

// define function to load .info file data
static int load_icon_file(const char *name, uint8_t **data, long *size) {


    // declare buffer for filename
    char filename[256];

    // cnstruct filename by appending ".info"
    snprintf(filename, sizeof(filename), "%s.info", name);

    // open file in binary read mode
    FILE *fp = fopen(filename, "rb");

    if (!fp) {
        perror("[load_do] Failed to open file");
        return 1;

    }

    // move file pointer to end
    fseek(fp, 0, SEEK_END);

    // get file size
    *size = ftell(fp);

    // reset file pointer to start
    rewind(fp);

    // allocate memory for file data
    *data = malloc(*size);

    if (!*data) {
        fclose(fp);
        return 1;
    }

    // read file data into buffer
    if (fread(*data, 1, *size, fp) != *size) {

        fprintf(stderr, "[load_do] Failed to read file\n");
        free(*data);
        fclose(fp);
        return 1;
    }

    fclose(fp);
    return 0;
}

// function to parse icon header
static int parse_icon_header(const uint8_t *header, long size, uint16_t *width, uint16_t *height, uint16_t *depth) {

    // check if header size is sufficient
    if (size < ICON_HEADER_SIZE) {

        fprintf(stderr, "[load_do] Invalid icon file: too short\n");
        return 1;
    }

    // read width at offset 4
    *width = read_be16(header + 4);

    // read height at offset 6
    *height = read_be16(header + 6);

    // read depth at offset 8
    *depth = read_be16(header + 8);

    // validate dimensions and depth
    if (*width == 0 || *height == 0 || *depth == 0 || *depth > 8) {

        fprintf(stderr, "[load_do] Invalid dimensions: Width=%d Height=%d Depth=%d\n", *width, *height, *depth);
        return 1;
    }
    return 0;
}

// function to render icon image
static int render_icon(Display *dpy, Window window, GC gc, Icon *icon, const uint8_t *data, uint16_t width, uint16_t height, uint16_t depth) {

    // get default visual for display
    Visual *visual = DefaultVisual(dpy, DefaultScreen(dpy));

    // get default depth for display
    int xdepth = DefaultDepth(dpy, DefaultScreen(dpy));

    // create XImage for icon (32-bit, ZPixmap format)
    icon->image = XCreateImage(dpy, visual, xdepth, ZPixmap, 0, malloc(width * height * 4), width, height, 32, 0);

    if (!icon->image) {
        fprintf(stderr, "[load_do] Failed to create XImage\n");
        return 1;
    }

    // define Amiga MUI style color palette
    unsigned long iconcolor[8] = {11184810, 0, 16777215, 6719675, 10066329, 12303291, 12298905, 16759722};

    // calculate bytes per row (16-bit padded)
    int row_bytes = ((width + 15) / 16) * 2;

    // calculate size of each color plane
    long plane_size = row_bytes * height;

    // point to pixel data
    const uint8_t *planes = data;

    // loop through each row
    for (int y = 0; y < height; ++y) {

        // loop through each pixel
        for (int x = 0; x < width; ++x) {

            // initialize color index
            int color = 0;

            // loop through each color plane
            for (int p = 0; p < depth; ++p) {

                // calculate offset for row/plane
                long row_offset = p * plane_size + y * row_bytes;

                // calculate byte index for pixel
                int byte_index = row_offset + (x >> 3);

                // read byte containing pixel bit
                uint8_t byte = planes[byte_index];

                // calculate bit position (most significant bit first)
                int bit = 7 - (x & 7);

                // set color bit if pixel bit is set
                if (byte & (1 << bit)) color |= (1 << p);

            }
            // set pixel color in XImage
            XPutPixel(icon->image, x, y, iconcolor[color & 7]);
        }
    }
    // draw image on window
    XPutImage(dpy, window, gc, icon->image, 0, 0, 0, 0, width, height);

    // flushe X11 requests to display image
    XFlush(dpy);

    return 0;
}

// main function to load and display icon
int load_do(Display *dpy, Window root, GC gc, const char *name, Icon *icon) {

    // declare pointer for file data
    uint8_t *data;

    // declare variable for file size
    long size;

    // load .info file, returns error if failed
    if (load_icon_file(name, &data, &size)) return 1;

    // read icon type at offset 48
    uint8_t do_type = data[48];

    // set header offset: 134 for disk/drawer, 78 for others
    int header_offset = (do_type == 1 || do_type == 2) ? 78 + 56 : 78;

    // check if file is too short for header
    if (header_offset + ICON_HEADER_SIZE > size) {
        fprintf(stderr, "[load_do] Invalid icon file: too short\n");
        free(data);
        return 1;
    }
    // variables for icon dimensions
    uint16_t width, height, depth;

    // parse header at correct offset, returns error if failed
    if (parse_icon_header(data + header_offset, size - header_offset, &width, &height, &depth)) {
        free(data);
        return 1;
    }

    // set initial icon position
    icon->x = 10;
    icon->y = 10;

    // set icon size
    icon->width = width;
    icon->height = height;

    // create icon window with black border, white background
    icon->window = XCreateSimpleWindow(dpy, root, icon->x, icon->y, width, height, 0,
                                      BlackPixel(dpy, DefaultScreen(dpy)),
                                      WhitePixel(dpy, DefaultScreen(dpy)));

    // allow click, release, motion, and expose events
    XSelectInput(dpy, icon->window, ButtonPressMask | ButtonReleaseMask | PointerMotionMask | ExposureMask);

    // map (show) the icon window
    XMapWindow(dpy, icon->window);

    // ensure icon stays at lowest layer
    XLowerWindow(dpy, icon->window);

    // render icon image at correct offset, returns error if failed
    if (render_icon(dpy, icon->window, gc, icon, data + header_offset + ICON_HEADER_SIZE, width, height, depth)) {
        free(data);
        return 1;
    }
    free(data);
    return 0;
}
