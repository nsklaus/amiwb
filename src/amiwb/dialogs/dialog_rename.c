// File: dialog_rename.c
// Rename dialog implementation

#include "dialog_internal.h"
#include "../render.h"
#include "../intuition/itn_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Rename Dialog Creation
// ============================================================================

// Create and show rename dialog
void show_rename_dialog(const char *old_name,
                       void (*on_ok)(const char *new_name),
                       void (*on_cancel)(void),
                       void *user_data) {
    if (!old_name || !on_ok || !on_cancel) return;

    // Create title with filename
    char title[NAME_SIZE];
    snprintf(title, sizeof(title), "Rename '%s'", old_name);

    // Create dialog using consistent lifecycle function (returns NULL on failure)
    Dialog *dialog = dialog_core_create(DIALOG_RENAME, title, 450, 160);
    if (!dialog) {
        log_error("[ERROR] Failed to create rename dialog - feature unavailable");
        return;  // Graceful degradation
    }

    // Set callbacks and user data
    strncpy(dialog->original_name, old_name, NAME_SIZE - 1);
    dialog->original_name[NAME_SIZE - 1] = '\0';
    dialog->on_ok = on_ok;
    dialog->on_cancel = on_cancel;
    dialog->user_data = user_data;

    // Create InputField widget for text entry
    dialog->input_field = inputfield_create(0, 0, 100, INPUT_HEIGHT, dialog->font);
    if (!dialog->input_field) {
        dialog_core_destroy(dialog);
        return;
    }

    // Set initial text and cursor
    inputfield_set_text(dialog->input_field, old_name);
    dialog->input_field->cursor_pos = strlen(old_name);
    dialog->input_field->has_focus = true;

    // Create toolkit buttons
    dialog->ok_button = button_create(20, 85, BUTTON_WIDTH, BUTTON_HEIGHT, "OK", dialog->font);
    dialog->cancel_button = button_create(340, 85, BUTTON_WIDTH, BUTTON_HEIGHT, "Cancel", dialog->font);

    // Register dialog in global list
    dialog_core_register(dialog);

    // Show the dialog and set it as active window
    XMapRaised(itn_core_get_display(), dialog->canvas->win);
    itn_focus_set_active(dialog->canvas);

    redraw_canvas(dialog->canvas);
}

// ============================================================================
// Rename Dialog Rendering
// ============================================================================

void dialog_rename_render_content(Canvas *canvas, Dialog *dialog) {
    Display *dpy = itn_core_get_display();
    Picture dest = canvas->canvas_render;

    // Calculate element positions
    int input_x, input_y, input_w, ok_x, ok_y, cancel_x, cancel_y;
    dialog_base_calculate_layout(dialog, &input_x, &input_y, &input_w, &ok_x, &ok_y, &cancel_x, &cancel_y);

    // Draw input box with inset 3D effect
    dialog_base_draw_inset_box(dest, input_x, input_y, input_w, INPUT_HEIGHT);

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

    // Render text and labels (title, "New Name:" label, input field)
    dialog_base_render_text_content(dialog, dest, input_x, input_y, input_w, ok_x, ok_y, cancel_x, cancel_y);
}
