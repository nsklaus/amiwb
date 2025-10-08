// File: dialog_events.c
// Unified event routing for all dialog types
// Dispatches X11 events to appropriate dialog handlers

#include "dialog_internal.h"
#include "../render.h"
#include "../intuition/itn_internal.h"
#include <X11/keysym.h>

// ============================================================================
// Key Event Handling
// ============================================================================

bool dialogs_handle_key_press(XKeyEvent *event) {
    // Find the dialog for the active window
    Canvas *active = itn_focus_get_active();
    if (!active || active->type != DIALOG) return false;

    Dialog *dialog = get_dialog_for_canvas(active);
    if (!dialog) return false;

    // First, let InputField handle the key event
    if (dialog->input_field) {
        bool handled = inputfield_handle_key(dialog->input_field, event);
        if (handled) {
            redraw_canvas(dialog->canvas);
            return true;
        }
    }

    // If InputField didn't handle it, check for dialog-level keys
    KeySym keysym = XLookupKeysym(event, 0);

    // Handle Escape key - cancel dialog (only if dropdown is not open)
    if (keysym == XK_Escape) {
        if (dialog->input_field && dialog->input_field->dropdown_open) {
            // Let InputField handle Escape to close dropdown
            return false;
        }
        if (dialog->on_cancel) dialog->on_cancel();
        destroy_dialog(dialog);
        return true;
    }

    // Handle Enter key - accept dialog (only if dropdown is not open)
    if (keysym == XK_Return || keysym == XK_KP_Enter) {
        if (dialog->input_field && dialog->input_field->dropdown_open) {
            // Let InputField handle Enter to apply completion
            return false;
        }
        if (dialog->input_field && dialog->on_ok) {
            dialog->on_ok(dialog->input_field->text);
        }
        destroy_dialog(dialog);
        return true;
    }

    // All other key handling is done by InputField widget
    return false;
}

// ============================================================================
// Button Press Event Handling
// ============================================================================

bool dialogs_handle_button_press(XButtonEvent *event) {
    if (!event) return false;

    // First check if this is a click on any InputField's completion dropdown
    // This must be checked before canvas lookup since dropdown is a separate window
    Dialog *all_dialogs = dialog_core_get_for_canvas(NULL);  // Get first dialog
    for (Dialog *d = all_dialogs; d; d = d->next) {
        if (d->input_field && inputfield_is_completion_window(d->input_field, event->window)) {
            // This is a click on a completion dropdown

            // Handle scroll wheel events (Button4 = scroll up, Button5 = scroll down)
            if (event->button == Button4 || event->button == Button5) {
                // Call InputField's scroll handler
                int direction = (event->button == Button4) ? -1 : 1;
                inputfield_handle_dropdown_scroll(d->input_field, direction, itn_core_get_display());
                return true;  // Consume the event
            }

            // Only process left click (Button1)
            if (event->button == Button1) {
                if (inputfield_handle_completion_click(d->input_field, event->x, event->y, itn_core_get_display())) {
                    // Selection was made, hide the dropdown
                    inputfield_hide_completions(d->input_field, itn_core_get_display());
                    redraw_canvas(d->canvas);
                    return true;
                }
            }
            return false;
        }
    }

    Canvas *canvas = itn_canvas_find_by_window(event->window);

    if (!canvas || canvas->type != DIALOG) return false;

    Dialog *dialog = get_dialog_for_canvas(canvas);
    if (!dialog) return false;


    // For delete confirmation dialogs, don't handle input box clicks since there's no input
    if (dialog->dialog_type == DIALOG_DELETE_CONFIRM) {
        // Handle button clicks using toolkit
        if (dialog->ok_button && button_handle_press(dialog->ok_button, event->x, event->y)) {
            redraw_canvas(canvas);
            return true;
        }

        if (dialog->cancel_button && button_handle_press(dialog->cancel_button, event->x, event->y)) {
            redraw_canvas(canvas);
            return true;
        }

        // Let other clicks (title bar, resize) go to intuition
        return false;
    }

    // Handle button clicks using toolkit for all dialog types
    if (dialog->ok_button && button_handle_press(dialog->ok_button, event->x, event->y)) {
        redraw_canvas(canvas);
        return true;
    }

    if (dialog->cancel_button && button_handle_press(dialog->cancel_button, event->x, event->y)) {
        redraw_canvas(canvas);
        return true;
    }

    // Get layout positions for input field
    int input_x, input_y, input_w, ok_x, ok_y, cancel_x, cancel_y;
    dialog_base_calculate_layout(dialog, &input_x, &input_y, &input_w, &ok_x, &ok_y, &cancel_x, &cancel_y);

    // Check if click is in input box
    if (event->x >= input_x && event->x < input_x + input_w &&
        event->y >= input_y && event->y < input_y + INPUT_HEIGHT) {

        // Pass click to InputField widget
        if (dialog->input_field) {
            // Update InputField position
            dialog->input_field->x = input_x;
            dialog->input_field->y = input_y;
            dialog->input_field->width = input_w;

            // Handle click in InputField
            if (inputfield_handle_click(dialog->input_field, event->x, event->y)) {
                // Now calculate and set cursor position
                int pos = inputfield_pos_from_x(dialog->input_field, event->x,
                                               itn_core_get_display());
                dialog->input_field->cursor_pos = pos;
                dialog->input_field->mouse_selecting = true;
                dialog->input_field->mouse_select_start = pos;
            }

            redraw_canvas(canvas);
        }

        return true;
    }

    // Let other clicks (title bar, resize) go to intuition
    return false;
}

// ============================================================================
// Button Release Event Handling
// ============================================================================

bool dialogs_handle_button_release(XButtonEvent *event) {
    if (!event) return false;

    Canvas *canvas = itn_canvas_find_by_window(event->window);
    if (!canvas || canvas->type != DIALOG) return false;

    Dialog *dialog = get_dialog_for_canvas(canvas);
    if (!dialog) return false;

    // Handle mouse release for InputField selection
    if (dialog->input_field && dialog->input_field->mouse_selecting) {
        inputfield_handle_mouse_release(dialog->input_field, event->x, event->y);
        redraw_canvas(dialog->canvas);
        return true;
    }

    // Handle button releases using toolkit
    if (dialog->ok_button) {
        if (button_handle_release(dialog->ok_button, event->x, event->y)) {
            // Button was clicked - check if it's activated
            if (button_is_clicked(dialog->ok_button)) {
                if (dialog->on_ok) {
                    // For dialogs with input fields, pass the text
                    if (dialog->input_field) {
                        dialog->on_ok(inputfield_get_text(dialog->input_field));
                    } else {
                        // For delete dialog, pass the message
                        dialog->on_ok(dialog->text_buffer);
                    }
                }
                destroy_dialog(dialog);
                return true;
            }
            redraw_canvas(canvas);
            return true;
        }
    }

    if (dialog->cancel_button) {
        if (button_handle_release(dialog->cancel_button, event->x, event->y)) {
            // Button was clicked - check if it's activated
            if (button_is_clicked(dialog->cancel_button)) {
                if (dialog->on_cancel) {
                    dialog->on_cancel();
                }
                destroy_dialog(dialog);
                return true;
            }
            redraw_canvas(canvas);
            return true;
        }
    }

    return false;
}

// ============================================================================
// Motion Event Handling
// ============================================================================

bool dialogs_handle_motion(XMotionEvent *event) {
    if (!event) return false;

    Canvas *canvas = itn_canvas_find_by_window(event->window);
    if (!canvas || canvas->type != DIALOG) return false;

    Dialog *dialog = get_dialog_for_canvas(canvas);
    if (!dialog) return false;

    // Handle mouse motion for InputField selection
    if (dialog->input_field && dialog->input_field->mouse_selecting) {
        if (inputfield_handle_mouse_motion(dialog->input_field, event->x, event->y, itn_core_get_display())) {
            redraw_canvas(dialog->canvas);
            return true;
        }
    }

    return false;
}
