// File: icon_core.c
// Public API implementation for icon lifecycle management
#include "icon_internal.h"
#include "../intuition/itn_public.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Default icon paths for fallback
static char *def_tool_path = "/usr/local/share/amiwb/icons/def_icons/def_foo.info";
static char *def_drawer_path = "/usr/local/share/amiwb/icons/def_icons/def_dir.info";

// Free Pictures only - used internally and by other modules
// OWNERSHIP: Frees Pictures only - does NOT free icon struct or paths
void icon_free_pictures(FileIcon* icon) {
    if (!icon) return;
    Display *dpy = itn_core_get_display();
    if (!dpy) return;
    if (icon->normal_picture) XRenderFreePicture(dpy, icon->normal_picture);
    if (icon->selected_picture) XRenderFreePicture(dpy, icon->selected_picture);
    icon->normal_picture = None;
    icon->selected_picture = None;
    icon->current_picture = None;
}

// Load icon images from .info file and create Pictures
// This is the main rendering function that ties all format handlers together
void create_icon_images(FileIcon *icon, RenderContext *ctx) {
    if (!icon || !ctx) return;

    const char *icon_path = icon->path;
    if (!strstr(icon_path, ".info")) {
        icon_path = (icon->type == TYPE_DRAWER || icon->type == TYPE_ICONIFIED) ?
                    def_drawer_path : def_tool_path;
    }

    uint8_t *data;
    long size;

    if (icon_load_file(icon_path, &data, &size)) {
        log_error("[ERROR] icons.c:create_icon_images() - Failed to load icon file: %s", icon_path);
        return;
    }

    // Check for AICON format (PNG container)
    if (size >= 5 && memcmp(data, "AICON", 5) == 0) {
        icon_load_aicon(icon, ctx, data, size);
        free(data);
        return;
    }

    // Check for Amiga format (classic DiskObject)
    if (size < 78 || icon_read_be16(data) != 0xE310 || icon_read_be16(data + 2) != 1) {
        log_error("[ERROR] icons.c:create_icon_images() - Invalid icon header in %s", icon_path);
        free(data);
        return;
    }


    // Detect icon format and capture FORM offset if present
    long form_offset = -1;
    AmigaIconFormat format = icon_detect_format(data, size, &form_offset);

    int ic_type = data[0x30];
    int has_drawer_data = (ic_type == 1 || ic_type == 2);
    int header_offset = 78 + (has_drawer_data ? 56 : 0);


    if (header_offset + ICON_HEADER_SIZE > size) {
        free(data);
        return;
    }

    uint16_t width, height, depth;
    Pixmap normal_pixmap = None;

    // Read the icon header values - but first check depth to see if header is valid
    depth = icon_read_be16(data + header_offset + 8);

    // Valid depth range is 1-8 for classic Amiga icons
    if (depth == 0 || depth > 8) {
        // Invalid classic header (depth out of range)
        // This will trigger either GlowIcon parsing or def_foo fallback
        width = 0;
        height = 0;
    } else {
        // Valid classic format
        width = icon_read_be16(data + header_offset + 4);
        height = icon_read_be16(data + header_offset + 6);
    }

    // IMPORTANT: Check for ColorIcon/GlowIcon BEFORE falling back to def_foo
    // Many GlowIcons have invalid/placeholder classic icons but valid FORM ICON chunks
    int has_invalid_classic = (depth == 0xFFFF || depth == 0 || depth > 8 || width == 0 || height == 0 || width > 256 || height > 256);

    // Check if this is a valid classic icon or just placeholder data
    if (has_invalid_classic) {
            // Before falling back to def_foo, check if we detected a GlowIcon
            // (format detection already scanned for FORM signature)
            if (format != AMIGA_ICON_GLOWICON) {
                // No GlowIcon, fall back to def_foo
                free(data);

                // Load def_foo.info data instead
                uint8_t *fallback_data;
                long fallback_size;
                if (icon_load_file(def_tool_path, &fallback_data, &fallback_size)) {
                    // def_foo should always exist as part of installation
                    icon->normal_picture = None;
                    icon->selected_picture = None;
                    return;
                }

                // Use the fallback data instead
                data = fallback_data;
                size = fallback_size;

                // Re-parse the header with def_foo data
                if (size < 78 || icon_read_be16(data) != 0xE310 || icon_read_be16(data + 2) != 1) {
                    free(data);
                    return;
                }

                // Re-detect format and re-calculate offsets for def_foo
                // (def_foo.info should not have FORM chunks, so form_offset stays -1)
                format = icon_detect_format(data, size, &form_offset);
                ic_type = data[0x30];
                has_drawer_data = (ic_type == 1 || ic_type == 2);
                header_offset = 78 + (has_drawer_data ? 56 : 0);

                // Re-read dimensions from def_foo
                depth = icon_read_be16(data + header_offset + 8);
                if (depth == 0xFFFF || depth == 0 || depth > 8) {
                    // def_foo should be valid!
                    free(data);
                    icon->normal_picture = None;
                    icon->selected_picture = None;
                    return;
                }
                width = icon_read_be16(data + header_offset + 4);
                height = icon_read_be16(data + header_offset + 6);
                has_invalid_classic = 0;  // def_foo has valid classic icon
            }
            // If format==AMIGA_ICON_GLOWICON, skip def_foo fallback and continue to GlowIcon parsing
    }

    // Now render the classic icon (either the original or def_foo fallback)
    // Skip rendering if we have invalid classic but will use GlowIcon instead
    normal_pixmap = None;
    if (!has_invalid_classic) {
        // Calculate how much data is available after the header
        long first_image_data_size = size - (header_offset + ICON_HEADER_SIZE);
        if (first_image_data_size < 0) first_image_data_size = 0;

        if (icon_render(ctx->dpy, &normal_pixmap, data + header_offset + ICON_HEADER_SIZE, width, height, depth, format, first_image_data_size)) {
            free(data);
            return;
        }
        icon->normal_picture = XRenderCreatePicture(ctx->dpy, normal_pixmap, ctx->fmt, 0, NULL);
    }

    uint32_t has_selected = icon_read_be32(data + 0x1A);
    if (!has_invalid_classic && has_selected && icon->normal_picture) {
        int row_bytes;
        long plane_size, first_data_size;
        icon_calculate_plane_dimensions(width, height, depth, &row_bytes, &plane_size, &first_data_size);
        int second_header_offset = header_offset + ICON_HEADER_SIZE + first_data_size;
        if (second_header_offset + ICON_HEADER_SIZE > size) {
            icon_cleanup_partial(ctx->dpy, icon);
            free(data);
            return;
        }

        uint16_t sel_width, sel_height, sel_depth;
        if (icon_parse_header(data + second_header_offset, size - second_header_offset, &sel_width, &sel_height, &sel_depth)) {
            icon_cleanup_partial(ctx->dpy, icon);
            free(data);
            return;
        }

        // Allow different sized selected images
        Pixmap selected_pixmap;
        // Calculate how much data is available for second image
        long second_image_data_size = size - (second_header_offset + ICON_HEADER_SIZE);
        if (second_image_data_size < 0) second_image_data_size = 0;

        if (icon_render(ctx->dpy, &selected_pixmap, data + second_header_offset + ICON_HEADER_SIZE, sel_width, sel_height, sel_depth, format, second_image_data_size)) {
            icon_cleanup_partial(ctx->dpy, icon);
            free(data);
            return;
        }
        icon->selected_picture = icon_create_picture(ctx->dpy, selected_pixmap, ctx->fmt);
        icon->sel_width = sel_width;
        icon->sel_height = sel_height;
    } else if (!has_invalid_classic && icon->normal_picture && normal_pixmap != None && form_offset < 0) {
        // No selected image - create darkened version like AmigaOS
        // But skip if GlowIcon will be parsed later (form_offset >= 0)
        Pixmap dark_pixmap = icon_create_darkened_pixmap(ctx->dpy, normal_pixmap, width, height);
        if (dark_pixmap) {
            // Ownership transfer: icon_create_picture() takes and frees dark_pixmap
            icon->selected_picture = icon_create_picture(ctx->dpy, dark_pixmap, ctx->fmt);
        } else {
            // Fallback to same image if darkening fails
            icon->selected_picture = icon->normal_picture;
        }
        icon->sel_width = width;
        icon->sel_height = height;
    }

    if (normal_pixmap != None) {
        XFreePixmap(ctx->dpy, normal_pixmap);
        normal_pixmap = None;  // Prevent use-after-free
    }

    // Only set dimensions if they're valid
    if (width > 0 && width <= 256 && height > 0 && height <= 256) {
        icon->width = width;
        icon->height = height;
    } else {
        // Will get dimensions from GlowIcon if present
        icon->width = 0;
        icon->height = 0;
    }

    // Check for ColorIcon/GlowIcon after the classic icon data
    // Calculate where we are in the file after classic icon(s)
    long classic_end = header_offset + ICON_HEADER_SIZE;
    int row_bytes;
    long plane_size, first_data_size;

    // Calculate the size of classic icon data
    if (width > 0 && height > 0 && depth > 0 && depth <= 8 && width <= 256 && height <= 256) {
        // Valid classic icon dimensions
        icon_calculate_plane_dimensions(width, height, depth, &row_bytes, &plane_size, &first_data_size);
        classic_end += first_data_size;
    } else {
        // Invalid or no classic icon data
        // Icons with depth=0xFFFF may still have dummy space reserved
        // Search for FORM signature to find actual GlowIcon data
        first_data_size = 0;

        // Scan for FORM signature starting from current position
        for (long offset = classic_end; offset + 12 <= size; offset++) {
            if (icon_read_be32(data + offset) == 0x464F524D) { // "FORM"
                classic_end = offset;
                break;
            }
        }
    }

    if (has_selected) {
        // Skip second classic icon
        int second_header_offset = header_offset + ICON_HEADER_SIZE + first_data_size;
        if (second_header_offset + ICON_HEADER_SIZE <= size) {
            uint16_t sel_width2, sel_height2, sel_depth2;
            if (!icon_parse_header(data + second_header_offset, size - second_header_offset,
                                   &sel_width2, &sel_height2, &sel_depth2)) {
                int sel_row_bytes;
                long sel_plane_size, sel_data_size;
                icon_calculate_plane_dimensions(sel_width2, sel_height2, sel_depth2,
                                               &sel_row_bytes, &sel_plane_size, &sel_data_size);
                classic_end = second_header_offset + ICON_HEADER_SIZE + sel_data_size;
            }
        }
    }

    // Skip any DefaultTool and ToolTypes data
    uint32_t has_default_tool = icon_read_be32(data + 0x32);
    uint32_t has_tooltypes = icon_read_be32(data + 0x36);

    // DefaultTool: 4-byte length, string data (length-1 bytes), zero byte
    if (has_default_tool && classic_end + 4 <= size) {
        uint32_t str_len = icon_read_be32(data + classic_end);
        if (str_len > 0 && str_len < (size - classic_end)) {
            classic_end += 4 + str_len;  // 4 for length + string data (includes zero terminator)
        }
    }

    // ToolTypes: stored as (count+1)*4, then array of strings
    if (has_tooltypes && classic_end + 4 <= size) {
        uint32_t tooltypes_value = icon_read_be32(data + classic_end);
        classic_end += 4;

        if (tooltypes_value > 0) {
            // Convert to actual count: (value/4) - 1
            uint32_t tooltypes_count = (tooltypes_value / 4) - 1;

            if (tooltypes_count > 0 && tooltypes_count < 100) {  // Sanity check
                // Each tooltip is a 4-byte length followed by string data
                for (uint32_t i = 0; i < tooltypes_count && classic_end + 4 <= size; i++) {
                    uint32_t str_len = icon_read_be32(data + classic_end);
                    classic_end += 4;
                    if (str_len > 0 && str_len < (size - classic_end)) {
                        classic_end += str_len;  // String data (includes zero terminator)
                    }
                }
            }
        }
    }

    // Skip DrawerData2 if present
    int ic_type2 = data[0x30];
    uint32_t user_data = icon_read_be32(data + 0x2E);
    if ((ic_type2 == 1 || ic_type2 == 2) && user_data && classic_end + 6 <= size) {
        classic_end += 6;  // DrawerData2 is 6 bytes
    }

    // Now check for GlowIcon using the offset we already detected
    // (no need to scan again - we captured the offset during format detection)
    if (form_offset >= 0 && form_offset + 4 <= size) {
        // GlowIcon detected - parse it silently unless there's an error

        Pixmap color_normal = 0, color_selected = 0;
        uint16_t color_width = 0, color_height = 0;
        uint16_t color_sel_width = 0, color_sel_height = 0;

        int parse_result = icon_parse_glowicon(ctx->dpy, data, size, form_offset,
                               &color_normal, &color_selected,
                               &color_width, &color_height,
                               &color_sel_width, &color_sel_height,
                               ctx->fmt, icon_path);


            if (parse_result == 0) {

            // Use ColorIcon instead of classic icon
            if (color_normal) {
                // Free classic icon Pictures
                if (icon->normal_picture) {
                    XRenderFreePicture(ctx->dpy, icon->normal_picture);
                }
                if (icon->selected_picture && icon->selected_picture != icon->normal_picture) {
                    XRenderFreePicture(ctx->dpy, icon->selected_picture);
                }

                // Use ColorIcon
                icon->normal_picture = XRenderCreatePicture(ctx->dpy, color_normal, ctx->fmt, 0, NULL);
                icon->width = color_width;
                icon->height = color_height;

                if (color_selected) {
                    icon->selected_picture = XRenderCreatePicture(ctx->dpy, color_selected, ctx->fmt, 0, NULL);
                    icon->sel_width = color_sel_width;
                    icon->sel_height = color_sel_height;
                    XFreePixmap(ctx->dpy, color_selected);
                } else {
                    // No selected image - create darkened version
                    Pixmap dark_pixmap = icon_create_darkened_pixmap(ctx->dpy, color_normal, color_width, color_height);
                    if (dark_pixmap) {
                        // Ownership transfer: icon_create_picture() takes and frees dark_pixmap
                        icon->selected_picture = icon_create_picture(ctx->dpy, dark_pixmap, ctx->fmt);
                    } else {
                        // Fallback to same image if darkening fails
                        icon->selected_picture = icon->normal_picture;
                    }
                    icon->sel_width = color_width;
                    icon->sel_height = color_height;
                }

                XFreePixmap(ctx->dpy, color_normal);

                // Set current_picture for GlowIcon
                icon->current_picture = icon->normal_picture;
            }
        }
    }

    // Set current_picture if not already set (classic icon case)
    if (!icon->current_picture) {
        icon->current_picture = icon->normal_picture;
    }

    // Handle special case: OS3 icons with depth=0xFFFF but valid bitmap data
    // These icons have no FORM chunk but do have bitmap data at fixed offset
    if (!icon->normal_picture && !icon->current_picture) {

        // Check if this might be an old-style icon with non-standard header
        uint32_t user_data = icon_read_be32(data + 0x2C);  // userData field at offset 0x2C indicates OS version


        // Handle both OS1.x (userData=0) and OS3.x (userData=1) icons
        if (user_data == 0 || user_data == 1) {  // OS 1.x or OS 2.x/3.x icon
            // Get dimensions from DiskObject Gadget structure (always valid)
            // These are the actual icon dimensions shown in Workbench
            uint16_t do_width = icon_read_be16(data + 0x0C);
            uint16_t do_height = icon_read_be16(data + 0x0E);

            // For OS1.3 icons, the actual icon dimensions come from the Gadget structure
            // The Image structure may not exist or may have invalid data
            uint16_t img_width = do_width;  // Use Gadget dimensions
            uint16_t img_height = do_height;
            uint16_t img_depth = 2;  // OS1.3 icons typically have 2 bitplanes
            uint32_t has_image_data = 1;  // Assume we have data

            // Check if we have a valid Image structure at the expected location
            // For OS1.3, the structure might be at 0x86 even for non-drawer icons
            bool has_image_at_86 = false;

            // Check if there's a valid Image structure at 0x86
            if (user_data == 0 && size >= 0x86 + 20) {
                uint16_t test_width = icon_read_be16(data + 0x8A);
                uint16_t test_height = icon_read_be16(data + 0x8C);
                uint16_t test_depth = icon_read_be16(data + 0x8E);
                // Check if these look like valid dimensions
                if (test_width > 0 && test_width <= 256 &&
                    test_height > 0 && test_height <= 256 &&
                    test_depth > 0 && test_depth <= 8) {
                    has_image_at_86 = true;
                }
            }

            if (user_data == 0 && has_image_at_86) {
                // For OS1.x icons with Image structure at 0x86
                // Use the Image structure dimensions as they're more accurate
                img_width = icon_read_be16(data + 0x8A);
                img_height = icon_read_be16(data + 0x8C);
                img_depth = icon_read_be16(data + 0x8E);
                has_image_data = icon_read_be32(data + 0x90);

            } else if (user_data == 1 && size >= 98) {
                // OS2.x/3.x: Image structure at offset 78 (0x4E) is valid
                img_width = icon_read_be16(data + 82);  // 0x52
                img_height = icon_read_be16(data + 84); // 0x54
                img_depth = icon_read_be16(data + 86);  // 0x56
                has_image_data = icon_read_be32(data + 88); // 0x58
            }

            // Validate dimensions and check if we have image data
            if (img_width > 0 && img_width <= 256 &&
                img_height > 0 && img_height <= 256 &&
                has_image_data != 0) {

                // Spec says: Image data follows Image structure
                const uint8_t *bitmap_start;

                if (user_data == 0 && has_image_at_86 && size >= 0x9A) {
                    // For icons with DrawerData, first Image is at 0x86
                    // Bitmap data starts at 0x9A (0x86 + 20 byte Image structure)
                    bitmap_start = data + 0x9A;
                } else if (user_data == 0) {
                    // OS1.3 icon without DrawerData - bitmap starts directly at 0x4E
                    // There's no Image structure header for OS1.3 icons!
                    bitmap_start = data + 0x4E;
                } else {
                    // OS2.x/3.x icon - bitmap follows the Image header at 0x4E
                    bitmap_start = data + 0x62;  // 0x4E + 20 (Image structure size)
                }

                Pixmap icon_pixmap = None;
                int render_result = 1;

                if (user_data == 0) {
                    // OS1.3 icon - use special renderer with transparent background
                    render_result = icon_render_os13(ctx->dpy, &icon_pixmap, bitmap_start, img_width, img_height);
                    } else {
                        // OS3 icon - use standard renderer
                        // Calculate available data size for this image
                        long available_data = size - (bitmap_start - data);
                        if (available_data < 0) available_data = 0;
                        render_result = icon_render(ctx->dpy, &icon_pixmap, bitmap_start, img_width, img_height, img_depth, format, available_data);
                    }

                    if (!render_result) {
                        icon->normal_picture = XRenderCreatePicture(ctx->dpy, icon_pixmap, ctx->fmt, 0, NULL);
                        icon->width = img_width;
                        icon->height = img_height;

                        // Check for selected image
                        uint32_t has_sel = icon_read_be32(data + 0x1A);
                        if (has_sel) {
                            // Calculate offset for second image
                            // First image bitmap size depends on actual dimensions
                            int row_bytes = ((img_width + 15) >> 4) << 1;
                            long plane_size = row_bytes * img_height;
                            long first_img_size = plane_size * img_depth;

                            // For OS1.x icon with Image at 0x86: second Image follows first image's data
                            if (user_data == 0 && has_image_at_86) {
                                // Spec says: second image follows first image's bitmap data
                                // First image bitmap at 0x9A, size = plane_size * depth = 248 * 2 = 496
                                // Second Image structure at 0x9A + 496 = 0x28A
                                long selected_offset = 0x9A + first_img_size;


                                // Check if there's another Image structure or just raw bitmap
                                if (selected_offset + 20 <= size) {
                                    // Try to read as Image structure
                                    uint16_t sel_width = icon_read_be16(data + selected_offset + 4);
                                    uint16_t sel_height = icon_read_be16(data + selected_offset + 6);
                                    uint16_t sel_depth = icon_read_be16(data + selected_offset + 8);
                                    uint32_t sel_has_data = icon_read_be32(data + selected_offset + 10);


                                    // Check if it looks like a valid Image structure
                                    if (sel_width > 0 && sel_width <= 256 &&
                                        sel_height > 0 && sel_height <= 256 &&
                                        sel_depth > 0 && sel_depth < 9 && sel_has_data) {
                                        // It's an Image structure, bitmap follows it
                                        // Bitmap starts immediately after the Image structure
                                        const uint8_t *sel_bitmap = data + selected_offset + 20;


                                        Pixmap sel_pixmap = None;
                                        int sel_result = icon_render_os13(ctx->dpy, &sel_pixmap, sel_bitmap,
                                                                         sel_width, sel_height);
                                        if (!sel_result) {
                                            icon->selected_picture = XRenderCreatePicture(ctx->dpy, sel_pixmap,
                                                                                         ctx->fmt, 0, NULL);
                                            icon->sel_width = sel_width;
                                            icon->sel_height = sel_height;
                                            XFreePixmap(ctx->dpy, sel_pixmap);
                                        }
                                    } else {
                                        // Not an Image structure, try as raw bitmap with same dimensions
                                        const uint8_t *sel_bitmap = data + selected_offset;


                                        Pixmap sel_pixmap = None;
                                        int sel_result = icon_render_os13(ctx->dpy, &sel_pixmap, sel_bitmap,
                                                                         img_width, img_height);
                                        if (!sel_result) {
                                            icon->selected_picture = XRenderCreatePicture(ctx->dpy, sel_pixmap,
                                                                                         ctx->fmt, 0, NULL);
                                            icon->sel_width = img_width;
                                            icon->sel_height = img_height;
                                            XFreePixmap(ctx->dpy, sel_pixmap);
                                        }
                                    }
                                }
                            } else if (user_data == 0 && img_depth == 2 && has_image_data == 1) {
                                // Original code for other OS1.3 icons
                                long second_bitmap_offset = 0x2B4;


                                if (second_bitmap_offset + first_img_size <= size) {
                                    // Verify there's actual data there (not all zeros/FFs)
                                    const uint8_t *sel_bitmap = data + second_bitmap_offset;
                                    int has_valid_data = 0;

                                    // Check if it looks like valid bitmap data
                                    for (int i = 0; i < 32 && i < first_img_size; i++) {
                                        if (sel_bitmap[i] != 0x00 && sel_bitmap[i] != 0xFF) {
                                            has_valid_data = 1;
                                            break;
                                        }
                                    }

                                    // Also check the pattern at 0x2A8 matches expected (from hex dump)
                                    if (!has_valid_data && size > 0x2B0) {
                                        // The hex dump shows: 0000 0000 ffff fffe at 0x2A8
                                        if (sel_bitmap[0] == 0x00 && sel_bitmap[1] == 0x00 &&
                                            sel_bitmap[4] == 0xFF && sel_bitmap[5] == 0xFF) {
                                            has_valid_data = 1;
                                        }
                                    }

                                    if (has_valid_data) {

                                        Pixmap sel_pixmap = None;
                                        int sel_result = icon_render_os13(ctx->dpy, &sel_pixmap, sel_bitmap, img_width, img_height);

                                        if (!sel_result) {
                                            icon->selected_picture = XRenderCreatePicture(ctx->dpy, sel_pixmap, ctx->fmt, 0, NULL);
                                            icon->sel_width = img_width;
                                            icon->sel_height = img_height;
                                            XFreePixmap(ctx->dpy, sel_pixmap);
                                        }
                                    }
                                }
                            } else {
                                // Normal case - second image has Image header
                                long second_img_offset = 98 + first_img_size;

                                // Second image also has a 20-byte Image header
                                if (second_img_offset + 20 <= size) {
                                    uint16_t sel_width = icon_read_be16(data + second_img_offset + 4);
                                    uint16_t sel_height = icon_read_be16(data + second_img_offset + 6);
                                    uint16_t sel_depth = icon_read_be16(data + second_img_offset + 8);
                                    uint32_t sel_has_data = icon_read_be32(data + second_img_offset + 10);

                                    if (sel_width > 0 && sel_width <= 256 &&
                                        sel_height > 0 && sel_height <= 256 &&
                                        sel_has_data != 0) {

                                        const uint8_t *sel_bitmap = data + second_img_offset + 20;
                                        Pixmap sel_pixmap = None;
                                        // Calculate available data for selected image
                                        long sel_data_size = size - (second_img_offset + 20);
                                        if (sel_data_size < 0) sel_data_size = 0;
                                        int sel_result = icon_render(ctx->dpy, &sel_pixmap, sel_bitmap, sel_width, sel_height, sel_depth, format, sel_data_size);

                                        if (!sel_result) {
                                            icon->selected_picture = XRenderCreatePicture(ctx->dpy, sel_pixmap, ctx->fmt, 0, NULL);
                                            icon->sel_width = sel_width;
                                            icon->sel_height = sel_height;
                                            XFreePixmap(ctx->dpy, sel_pixmap);
                                        }
                                    }
                                }
                            }
                        }

                        if (!icon->selected_picture) {
                            icon->selected_picture = icon->normal_picture;
                            icon->sel_width = img_width;
                            icon->sel_height = img_height;
                        }

                        icon->current_picture = icon->normal_picture;
                        XFreePixmap(ctx->dpy, icon_pixmap);
                    }
                }
            }
        }


    free(data);
}

// Create and initialize a FileIcon structure with loaded images
// OWNERSHIP: Returns allocated FileIcon - caller must call destroy_file_icon()
FileIcon* create_file_icon(const char* path, int x, int y, IconType type,
                           Window display_window, RenderContext* ctx) {
    if (!path || !ctx) return NULL;

    // Allocate icon structure
    FileIcon* icon = calloc(1, sizeof(FileIcon));
    if (!icon) {
        log_error("[ERROR] calloc failed for FileIcon structure - icon will not appear");
        return NULL;  // Graceful failure
    }

    // Duplicate path string
    icon->path = strdup(path);
    if (!icon->path) {
        log_error("[ERROR] strdup failed for icon path - icon will not appear");
        free(icon);
        return NULL;  // Graceful failure
    }

    // Extract label from path (basename without .info extension)
    const char* base = strrchr(path, '/');
    const char* label_src = base ? base + 1 : path;
    icon->label = strdup(label_src);
    if (!icon->label) {
        log_error("[ERROR] strdup failed for icon label - icon will not appear");
        free(icon->path);
        free(icon);
        return NULL;  // Graceful failure
    }

    // Initialize fields
    icon->type = type;
    icon->x = x;
    icon->y = y;
    icon->display_window = display_window;
    icon->selected = false;
    icon->last_click_time = 0;
    icon->iconified_canvas = NULL;
    icon->render_error_logged = false;

    // Load icon images from .info file
    create_icon_images(icon, ctx);
    icon->current_picture = icon->normal_picture;

    return icon;
}

// Complete cleanup - frees Pictures, paths, label, and icon struct
// OWNERSHIP: Frees everything including the FileIcon structure itself
void destroy_file_icon(FileIcon* icon) {
    if (!icon) return;

    // Free rendering resources
    icon_free_pictures(icon);

    // Free string resources
    if (icon->path) {
        free(icon->path);
        icon->path = NULL;
    }
    if (icon->label) {
        free(icon->label);
        icon->label = NULL;
    }

    // Zero out and free struct
    memset(icon, 0, sizeof(FileIcon));
    free(icon);
}
