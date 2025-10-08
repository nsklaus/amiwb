// File: icon_detect.c
// Format detection and file loading for Amiga icon formats
#include "icon_internal.h"
#include <stdio.h>
#include <stdlib.h>

// Detect icon format by examining file structure
// Returns the detected format and optionally outputs the FORM/marker offset
AmigaIconFormat icon_detect_format(const uint8_t *data, long size, long *form_offset_out) {
    // Initialize output parameter
    if (form_offset_out) {
        *form_offset_out = -1;
    }

    if (size < 78) return AMIGA_ICON_UNKNOWN;

    // Check magic number
    if (icon_read_be16(data) != 0xE310 || icon_read_be16(data + 2) != 1) {
        return AMIGA_ICON_UNKNOWN;
    }

    // Get userData field (offset 0x2C)
    uint32_t user_data = icon_read_be32(data + 0x2C);

    // Check for WIM/MIM markers FIRST (GlowIcon - ToolTypes variant)
    // WIM = Workbench Image, MIM = MagicWB Image
    // These appear in ToolTypes section and indicate GlowIcon encoded in 7-bit ASCII
    // We check these FIRST because they're more specific than generic FORM chunks
    for (long i = 78; i < size - 4; i++) {
        // Check for WIM1= or MIM1= markers
        if (i + 5 <= size) {
            if ((data[i] == 'W' || data[i] == 'M') &&
                data[i+1] == 'I' && data[i+2] == 'M' && data[i+3] == '1' && data[i+4] == '=') {
                // Found WIM1= or MIM1= - this is a GlowIcon ToolTypes variant
                if (form_offset_out) {
                    *form_offset_out = i; // Save marker position for parser
                }
                return AMIGA_ICON_GLOWICON;
            }
        }
    }

    // Check for IM1= marker (NewIcon or GlowIcon fallback)
    // If we find IM1= but no WIM/MIM, try using it as GlowIcon start point
    for (long i = 78; i < size - 4; i++) {
        if (i + 4 <= size) {
            if (data[i] == 'I' && data[i+1] == 'M' && data[i+2] == '1' && data[i+3] == '=') {
                // Found IM1= marker - could be GlowIcon or NewIcon
                if (form_offset_out) {
                    *form_offset_out = i; // Save marker position
                }
                return AMIGA_ICON_GLOWICON; // Try treating as GlowIcon first
            }
        }
    }

    // Check for FORM/ICON LAST (GlowIcon - IFF chunks variant)
    // Scan for FORM signature and capture offset
    for (long i = 78; i < size - 8; i++) {
        if (icon_read_be32(data + i) == 0x464F524D) { // "FORM"
            if (i + 12 <= size && icon_read_be32(data + i + 8) == 0x49434F4E) { // "ICON"
                // Found GlowIcon - save offset for later use
                if (form_offset_out) {
                    *form_offset_out = i;
                }
                return AMIGA_ICON_GLOWICON;
            }
        }
    }

    // TODO: Check for MWB palette

    // Distinguish by userData
    if (user_data == 0) {
        return AMIGA_ICON_OS13;
    } else if (user_data == 1) {
        return AMIGA_ICON_OS3;
    }

    return AMIGA_ICON_UNKNOWN;
}

// Load entire .info file into memory so we can parse planes quickly.
int icon_load_file(const char *name, uint8_t **data, long *size) {
    FILE *fp = fopen(name, "rb");
    if (!fp) return 1;
    fseek(fp, 0, SEEK_END);
    *size = ftell(fp);
    rewind(fp);
    // Allocate 1 extra byte - icon_read_bits() reads 2 bytes at a time for bit shifting
    // Without padding, reading the last byte would overflow by 1 byte
    *data = malloc(*size + 1);
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
int icon_parse_header(const uint8_t *header, long size, uint16_t *width, uint16_t *height, uint16_t *depth) {
    if (size < ICON_HEADER_SIZE) return 1;
    *width = icon_read_be16(header + 4);
    *height = icon_read_be16(header + 6);
    *depth = icon_read_be16(header + 8);
    if (*width == 0 || *height == 0 || *depth == 0 || *depth > 8) return 1;
    return 0;
}
