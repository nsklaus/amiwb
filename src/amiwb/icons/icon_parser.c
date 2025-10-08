// File: icon_parser.c
// Binary parsing utilities for icon formats
#include "icon_internal.h"

// Convert big-endian 16-bit value to host byte order
uint16_t icon_read_be16(const uint8_t *p) {
    // Convert big-endian 16-bit value without bit shifting
    return (uint16_t)(p[0] * 256 + p[1]);
}

// Convert big-endian 32-bit value to host byte order
uint32_t icon_read_be32(const uint8_t *p) {
    // Convert big-endian 32-bit value without bit shifting
    return (uint32_t)(p[0] * 16777216 + p[1] * 65536 + p[2] * 256 + p[3]);
}

// Read a 4-byte IFF chunk ID
uint32_t icon_read_iff_id(const uint8_t *p) {
    return icon_read_be32(p);
}

// Read bits from bit-aligned data (for RLE decompression)
uint8_t icon_read_bits(const uint8_t *data, int bit_count, int bit_offset) {
    int byte_offset = bit_offset / 8;
    int bit_in_byte = bit_offset % 8;
    uint16_t value = (data[byte_offset] << 8) | data[byte_offset + 1];
    value >>= (16 - bit_in_byte - bit_count);
    return value & ((1 << bit_count) - 1);
}

// Helper function to calculate icon plane dimensions (Amiga planar format)
void icon_calculate_plane_dimensions(uint16_t width, uint16_t height, uint16_t depth,
                                     int *row_bytes, long *plane_size, long *total_data_size) {
    *row_bytes = ((width + 15) / 16) * 2;
    *plane_size = (*row_bytes) * height;
    *total_data_size = (*plane_size) * depth;
}
