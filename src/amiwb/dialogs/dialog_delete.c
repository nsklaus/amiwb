// File: dialog_delete.c
// Delete confirmation dialog implementation
// CRITICAL FOR USER DATA SAFETY - Last chance before file deletion

#include "dialog_internal.h"
#include "../render.h"
#include "../intuition/itn_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Module-Private State (Callback Storage)
// ============================================================================

// Delete confirmation dialog - CRITICAL FOR USER DATA SAFETY
// Store callbacks globally (simple approach for single delete dialog)
static void (*g_delete_confirm_callback)(void) = NULL;
static void (*g_delete_cancel_callback)(void) = NULL;

// ============================================================================
// Callback Wrappers
// ============================================================================

static void delete_confirm_ok(const char *unused) {
    (void)unused;
    if (g_delete_confirm_callback) {
        void (*callback)(void) = g_delete_confirm_callback;
        g_delete_confirm_callback = NULL;
        g_delete_cancel_callback = NULL;
        callback();
    }
}

static void delete_confirm_cancel(void) {
    if (g_delete_cancel_callback) {
        void (*callback)(void) = g_delete_cancel_callback;
        g_delete_confirm_callback = NULL;
        g_delete_cancel_callback = NULL;
        callback();
    }
}

// ============================================================================
// Delete Confirmation Dialog Creation
// ============================================================================

void show_delete_confirmation(const char *message,
                             void (*on_confirm)(void),
                             void (*on_cancel)(void)) {
    if (!message || !on_confirm) return;

    // Store callbacks globally (simple approach for now)
    g_delete_confirm_callback = on_confirm;
    g_delete_cancel_callback = on_cancel ? on_cancel : delete_confirm_cancel;

    // Create dialog using consistent lifecycle function (returns NULL on failure)
    Dialog *dialog = dialog_core_create(DIALOG_DELETE_CONFIRM, "Delete Confirmation", 450, 220);
    if (!dialog) {
        log_error("[ERROR] Failed to create delete confirmation dialog - operation cancelled");
        return;  // Graceful degradation - safer to cancel delete than proceed without confirmation
    }

    // Set message text and callbacks
    if (strlen(message) >= NAME_SIZE) {
        log_error("[WARNING] Delete confirmation message truncated: %s", message);
    }
    strncpy(dialog->text_buffer, message, NAME_SIZE - 1);
    dialog->text_buffer[NAME_SIZE - 1] = '\0';
    dialog->on_ok = delete_confirm_ok;
    dialog->on_cancel = delete_confirm_cancel;

    // Create toolkit buttons (different positions for delete dialog)
    dialog->ok_button = button_create(10, 150, BUTTON_WIDTH, BUTTON_HEIGHT, "OK", dialog->font);
    dialog->cancel_button = button_create(340, 150, BUTTON_WIDTH, BUTTON_HEIGHT, "Cancel", dialog->font);

    // Register dialog in global list
    dialog_core_register(dialog);

    // Show the dialog and set it as active window
    XMapRaised(itn_core_get_display(), dialog->canvas->win);
    itn_focus_set_active(dialog->canvas);
    redraw_canvas(dialog->canvas);
}

// ============================================================================
// Delete Confirmation Dialog Rendering
// ============================================================================

void dialog_delete_render_content(Canvas *canvas, Dialog *dialog) {
    Display *dpy = itn_core_get_display();
    Picture dest = canvas->canvas_render;

    // Calculate element positions
    int input_x, input_y, input_w, ok_x, ok_y, cancel_x, cancel_y;
    dialog_base_calculate_layout(dialog, &input_x, &input_y, &input_w, &ok_x, &ok_y, &cancel_x, &cancel_y);

    // Draw checkerboard pattern as border (10 pixels thick)
    int border_thickness = 10;
    int content_left = BORDER_WIDTH_LEFT;
    int content_top = BORDER_HEIGHT_TOP;
    int content_width = canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT_CLIENT;
    // int content_height = canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM;  // May be used later

    // Bottom area: encompasses the buttons
    // Start 2 pixels above buttons, end 4 pixels below buttons
    int bottom_start_y = ok_y - 2;
    int bottom_height = (ok_y + BUTTON_HEIGHT + 4) - bottom_start_y;

    // Top border
    dialog_base_draw_checkerboard(dest, content_left, content_top,
                            content_width, border_thickness);

    // Left border (only up to where bottom area starts)
    dialog_base_draw_checkerboard(dest, content_left,
                            content_top + border_thickness,
                            border_thickness,
                            bottom_start_y - (content_top + border_thickness));

    // Right border (only up to where bottom area starts)
    dialog_base_draw_checkerboard(dest,
                            content_left + content_width - border_thickness,
                            content_top + border_thickness,
                            border_thickness,
                            bottom_start_y - (content_top + border_thickness));

    // Bottom horizontal area that encompasses the buttons
    dialog_base_draw_checkerboard(dest, content_left, bottom_start_y,
                            content_width, bottom_height);

    // Add 3D inset border around the inner window area (after the checker pattern)
    // This creates a recessed look with light from top-left
    int inner_left = content_left + border_thickness;
    int inner_top = content_top + border_thickness;
    int inner_width = content_width - (2 * border_thickness);
    int inner_height = bottom_start_y - inner_top;

    // Left edge - black line (shadow)
    XRenderColor black = {0x0000, 0x0000, 0x0000, 0xffff};
    XRenderFillRectangle(dpy, PictOpOver, dest, &black,
                       inner_left, inner_top, 1, inner_height);

    // Top edge - black line (shadow)
    XRenderFillRectangle(dpy, PictOpOver, dest, &black,
                       inner_left, inner_top, inner_width, 1);

    // Right edge - double white line (highlight)
    XRenderColor white = {0xffff, 0xffff, 0xffff, 0xffff};
    XRenderFillRectangle(dpy, PictOpOver, dest, &white,
                       inner_left + inner_width - 2, inner_top, 2, inner_height);

    // Bottom edge - double white line (highlight)
    XRenderFillRectangle(dpy, PictOpOver, dest, &white,
                       inner_left, inner_top + inner_height - 2, inner_width, 2);

    // Draw toolkit buttons if they exist
    if (dialog->ok_button && dialog->cancel_button) {
        // Update button positions based on layout
        dialog->ok_button->x = ok_x;
        dialog->ok_button->y = ok_y;
        dialog->cancel_button->x = cancel_x;
        dialog->cancel_button->y = cancel_y;

        // Render toolkit buttons
        button_render(dialog->ok_button, dest, dpy, canvas->xft_draw);
        button_render(dialog->cancel_button, dest, dpy, canvas->xft_draw);
    }

    // Render warning text and delete message
    dialog_base_render_text_content(dialog, dest, input_x, input_y, input_w, ok_x, ok_y, cancel_x, cancel_y);
}
