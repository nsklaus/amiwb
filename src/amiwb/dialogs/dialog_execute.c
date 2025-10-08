// File: dialog_execute.c
// Execute command dialog implementation

#include "dialog_internal.h"
#include "../render/rnd_public.h"
#include "../intuition/itn_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Execute Dialog Creation
// ============================================================================

// Show execute command dialog
void show_execute_dialog(void (*on_ok)(const char *command),
                        void (*on_cancel)(void)) {
    if (!on_ok || !on_cancel) return;

    // Create dialog using consistent lifecycle function (returns NULL on failure)
    Dialog *dialog = dialog_core_create(DIALOG_EXECUTE_COMMAND, "Execute", 450, 160);
    if (!dialog) {
        log_error("[ERROR] Failed to create execute dialog - feature unavailable");
        return;  // Graceful degradation
    }

    // Set callbacks
    dialog->original_name[0] = '\0';  // Not used for execute dialog
    dialog->on_ok = on_ok;
    dialog->on_cancel = on_cancel;

    // Create InputField widget for command entry
    dialog->input_field = inputfield_create(0, 0, 100, INPUT_HEIGHT, dialog->font);
    if (!dialog->input_field) {
        dialog_core_destroy(dialog);
        return;
    }

    // Enable path completion for execute dialog
    inputfield_enable_path_completion(dialog->input_field, true);
    inputfield_set_text(dialog->input_field, "");
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
// Execute Dialog Rendering
// ============================================================================

void dialog_execute_render_content(Canvas *canvas, Dialog *dialog) {
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

    // Render text and labels (title, "Command:" label, input field)
    dialog_base_render_text_content(dialog, dest, input_x, input_y, input_w, ok_x, ok_y, cancel_x, cancel_y);
}
