// File: dialog_base.c
// Shared rendering primitives for all dialog types
// Single source of truth for common visual elements

#include "dialog_internal.h"
#include "../render.h"
#include "../intuition/itn_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// 3D Drawing Primitives
// ============================================================================

// Draw inset box for input fields (AmigaOS style)
void dialog_base_draw_inset_box(Picture dest, int x, int y, int w, int h) {
    Display *dpy = itn_core_get_display();

    // Outer border - inset effect (light source top-left)
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, x, y, 1, h);        // Left outer (white)
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, x, y, w, 1);        // Top outer (white)
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, x+w-1, y, 1, h);    // Right outer (black)
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, x, y+h-1, w, 1);    // Bottom outer (black)

    // Inner border - creates the carved effect
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, x+1, y+1, 1, h-2);  // Left inner (black)
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, x+1, y+1, w-2, 1);  // Top inner (black)
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, x+w-2, y+1, 1, h-2);// Right inner (white)
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, x+1, y+h-2, w-2, 1);// Bottom inner (white)

    // Gray fill for input area (AmigaOS style)
    XRenderFillRectangle(dpy, PictOpSrc, dest, &GRAY, x+2, y+2, w-4, h-4);
}

// Draw checkerboard pattern for delete confirmation dialog
void dialog_base_draw_checkerboard(Picture dest, int x, int y, int w, int h) {
    Display *dpy = itn_core_get_display();

    // Use same checker size as scrollbars (2x2 pixels)
    int checker_size = 2;

    // Draw checkerboard pattern
    for (int row = 0; row < h; row += checker_size) {
        for (int col = 0; col < w; col += checker_size) {
            // Alternate between white and gray based on position
            bool is_white = ((row / checker_size) + (col / checker_size)) % 2 == 0;
            XRenderColor *color = is_white ? &WHITE : &GRAY;

            int draw_w = (col + checker_size > w) ? w - col : checker_size;
            int draw_h = (row + checker_size > h) ? h - row : checker_size;

            XRenderFillRectangle(dpy, PictOpSrc, dest, color,
                               x + col, y + row, draw_w, draw_h);
        }
    }
}

// ============================================================================
// Layout Calculation
// ============================================================================

// Calculate layout positions based on current canvas size
void dialog_base_calculate_layout(Dialog *dialog, int *input_x, int *input_y, int *input_w,
                                  int *ok_x, int *ok_y, int *cancel_x, int *cancel_y) {
    Canvas *canvas = dialog->canvas;

    // Account for window borders in layout calculations
    int content_left = BORDER_WIDTH_LEFT;
    int content_top = BORDER_HEIGHT_TOP;
    // Dialogs use client window borders (8px right border)
    int content_width = canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT_CLIENT;
    // int content_height = canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM;  // May be used later

    // Input box: starts after "New Name:" label, positioned below title with small gap
    *input_x = content_left + DIALOG_MARGIN + LABEL_WIDTH;
    *input_y = content_top + 35;  // Title + 10px gap as requested
    *input_w = content_width - 2 * DIALOG_MARGIN - LABEL_WIDTH;

    // Buttons: positioned at bottom with proper spacing
    // For delete dialog, need more room for the warning text and checker pattern
    int button_y_offset = 85;  // Default for rename dialog
    if (dialog->dialog_type == DIALOG_DELETE_CONFIRM) {
        // Position buttons to leave room for checker pattern below (4px) and decorations
        button_y_offset = 150;  // Higher to leave room for bottom checker and decoration

        // Align buttons with inner window boundaries (after 10px checker border)
        int border_thickness = 10;
        *ok_x = content_left + border_thickness;  // Align with inner window left edge
        *ok_y = content_top + button_y_offset;

        *cancel_x = content_left + content_width - border_thickness - BUTTON_WIDTH;  // Align with inner window right edge
        *cancel_y = *ok_y;
    } else {
        *ok_x = content_left + DIALOG_MARGIN;
        *ok_y = content_top + button_y_offset;

        *cancel_x = content_left + content_width - DIALOG_MARGIN - BUTTON_WIDTH;
        *cancel_y = *ok_y;
    }
}

// ============================================================================
// Text Rendering
// ============================================================================

// Render text content with cursor and selection
// This function handles dialog-specific text rendering (titles, labels, messages)
void dialog_base_render_text_content(Dialog *dialog, Picture dest,
                                     int input_x, int input_y, int input_w,
                                     int ok_x, int ok_y, int cancel_x, int cancel_y) {
    Display *dpy = itn_core_get_display();
    Canvas *canvas = dialog->canvas;
    XftFont *font = get_font();
    if (!font) return;

    // Use cached XftDraw for text rendering
    if (!canvas->xft_draw) {
        log_error("[WARNING] No cached XftDraw for dialog");
        return;
    }

    // Draw centered title above input box
    XRenderColor text_color = BLACK;
    XftColor xft_text;
    XftColorAllocValue(dpy, canvas->visual, canvas->colormap, &text_color, &xft_text);

    if (dialog->dialog_type == DIALOG_DELETE_CONFIRM) {
        //int content_width = canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT_CLIENT;
        int line_y = BORDER_HEIGHT_TOP + 30;
        int text_left_x = BORDER_WIDTH_LEFT + 15;  // Left margin inside inner window

        /*
        // Line 1: "Warning" centered
        const char *warning = "Warning";
        XGlyphInfo warning_ext;
        XftTextExtentsUtf8(dpy, font, (FcChar8*)warning, strlen(warning), &warning_ext);
        int warning_x = BORDER_WIDTH_LEFT + (content_width - warning_ext.xOff) / 2;
        XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, warning_x, line_y,
                         (FcChar8*)warning, strlen(warning));
        line_y += 30;
        */


        const char *line1 = "Last call before Willoughby. Beyond this point,";
        const char *line2 = "no return service is available. Files wishing to";
        const char *line3 = "preserve structural integrity should disembark";
        const char *line4 = "immediately. Dear Files and Dirs: Last call,";
        const char *line5 = "Terminus inbound..";


        XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_left_x, line_y,
                 (FcChar8*)line1, strlen(line1));
        line_y += 14;

        XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_left_x, line_y,
                         (FcChar8*)line2, strlen(line2));
        line_y += 14;

        XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_left_x, line_y,
                         (FcChar8*)line3, strlen(line3));
        line_y += 14;

        XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_left_x, line_y,
                         (FcChar8*)line4, strlen(line4));
        line_y += 14;

        XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_left_x, line_y,
                 (FcChar8*)line5, strlen(line5));
        line_y += 35;

        const char *line6 = "Is it really Ok to delete:";
        XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_left_x, line_y,
                         (FcChar8*)line6, strlen(line6));
        line_y += 14;

        // Line 5: The delete summary (stored in text_buffer) - left aligned
        // This contains formatted text like "3 files and 4 directories?"
        const char *msg = dialog->text_buffer;
        XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_left_x, line_y,
                         (FcChar8*)msg, strlen(msg));

    } else if (dialog->dialog_type == DIALOG_EXECUTE_COMMAND) {
        // For execute dialog, show the command prompt
        const char *title_text = "Enter Command and its Arguments:";

        XGlyphInfo title_ext;
        XftTextExtentsUtf8(dpy, font, (FcChar8*)title_text, strlen(title_text), &title_ext);
        int content_width = canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT_CLIENT;
        int title_x = BORDER_WIDTH_LEFT + (content_width - title_ext.xOff) / 2;
        int title_y = BORDER_HEIGHT_TOP + 20;
        XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, title_x, title_y,
                         (FcChar8*)title_text, strlen(title_text));

        // Draw "Command:" label to the left of input box (same row)
        int label_x = BORDER_WIDTH_LEFT + DIALOG_MARGIN;
        int label_y = input_y + (INPUT_HEIGHT + font->ascent) / 2 - 2;  // Move up 2 pixels
        XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, label_x, label_y,
                         (FcChar8*)"Command:", 8);

        // Update InputField position and size if needed
        if (dialog->input_field) {
            dialog->input_field->x = input_x;
            dialog->input_field->y = input_y;
            dialog->input_field->width = input_w;
            dialog->input_field->height = INPUT_HEIGHT;

            // Draw the InputField using toolkit function
            inputfield_render(dialog->input_field, canvas->canvas_render, dpy, canvas->xft_draw);
        }
    } else {
        // For rename dialog, show the original prompt
        char title_text[PATH_SIZE];  // May contain file path in message
        int ret = snprintf(title_text, sizeof(title_text), "Enter a new name for '%s'.",
                 strlen(dialog->original_name) > 0 ? dialog->original_name : "file");
        if (ret >= (int)sizeof(title_text)) {
            log_error("[ERROR] Dialog title too long, using shortened version");
            snprintf(title_text, sizeof(title_text), "Enter a new name.");
        }

        XGlyphInfo title_ext;
        XftTextExtentsUtf8(dpy, font, (FcChar8*)title_text, strlen(title_text), &title_ext);
        int content_width = canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT_CLIENT;
        int title_x = BORDER_WIDTH_LEFT + (content_width - title_ext.xOff) / 2;
        int title_y = BORDER_HEIGHT_TOP + 20;
        XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, title_x, title_y,
                         (FcChar8*)title_text, strlen(title_text));

        // Draw "New Name:" label to the left of input box (same row)
        int label_x = BORDER_WIDTH_LEFT + DIALOG_MARGIN;
        int label_y = input_y + (INPUT_HEIGHT + font->ascent) / 2 - 2;  // Move up 2 pixels
        XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, label_x, label_y,
                         (FcChar8*)"New Name:", 9);

        // Update InputField position and size if needed
        if (dialog->input_field) {
            dialog->input_field->x = input_x;
            dialog->input_field->y = input_y;
            dialog->input_field->width = input_w;
            dialog->input_field->height = INPUT_HEIGHT;

            // Draw the InputField using toolkit function
            inputfield_render(dialog->input_field, canvas->canvas_render, dpy, canvas->xft_draw);
        }
    }

    // Clean up
    XftColorFree(dpy, canvas->visual, canvas->colormap, &xft_text);
    // No need to destroy - using cached XftDraw
}
