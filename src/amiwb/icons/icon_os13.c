// File: icon_os13.c
// OS 1.3 classic icon format support
#include "icon_internal.h"
#include <string.h>

// Helper function to get MagicWB 8-color palette
void icon_get_mwb_palette(unsigned long colors[8]) {
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

// Helper function to get OS1.3 icon color palette
// Based on WB13Palette from Amiga-Icon-converter-master/icon.js line 40-45
// IMPORTANT: Index 0 is transparent in WB icons (see line 330 in icon.js)!
void icon_get_os13_palette(unsigned long colors[4]) {
    // WB1.3 palette - corrected for AmiWB appearance:
    // Index 0 is always transparent in rendered icons
    // Swapped black and white for correct appearance
    // Using AmiWB's BLUE color from config.h for consistency
    colors[0] = 0x00000000;   // Transparent (alpha=0)
    colors[1] = 0xFF000000;   // Black: RGB(0,0,0) - was white in original
    colors[2] = 0xFFFFFFFF;   // White: RGB(255,255,255) - was black in original
    colors[3] = 0xFF486FB0;   // AmiWB Blue: RGB(72,111,176) from config.h BLUE
}

// Render OS1.3 icon (2 bitplanes, 4 colors, transparent background)
int icon_render_os13(Display *dpy, Pixmap *pixmap_out, const uint8_t *data,
                     uint16_t width, uint16_t height) {
    Pixmap pixmap;
    XImage *image;
    XVisualInfo vinfo;
    if (icon_create_rendering_context(dpy, width, height, &pixmap, &image, &vinfo) != 0) {
        return 1;
    }
    memset(image->data, 0, width * height * 4);

    // OS1.3 color palette
    unsigned long colors[4];
    icon_get_os13_palette(colors);

    // OS1.3 icons always have 2 bitplanes
    int row_bytes = ((width + 15) >> 4) << 1;
    long plane_size = row_bytes * height;

    // The JavaScript reads ALL bytes sequentially, including padding
    // But we need to know where plane 1 starts after plane 0 + padding
    // For Trashcan2: plane 0 at 0x9A-0x192, plane 1 at 0x1D0
    // That's 0x1D0 - 0x9A = 0x136 = 310 bytes offset for plane 1

    // For OS1.3 icons, the second bitplane immediately follows the first
    long second_plane_offset = plane_size;


    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int color = 0;

            // Read plane 0
            long offset = y * row_bytes + (x >> 3);
            uint8_t byte = data[offset];
            if (byte & (1 << (7 - (x & 7)))) color |= 1;

            // Read plane 1 from adjusted offset
            offset = second_plane_offset + y * row_bytes + (x >> 3);
            byte = data[offset];
            if (byte & (1 << (7 - (x & 7)))) color |= 2;

            XPutPixel(image, x, y, colors[color]);
        }
    }

    GC gc = XCreateGC(dpy, pixmap, 0, NULL);
    XPutImage(dpy, pixmap, gc, image, 0, 0, 0, 0, width, height);
    XFreeGC(dpy, gc);
    XDestroyImage(image);
    *pixmap_out = pixmap;
    return 0;
}
