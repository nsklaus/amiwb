// File: dialog_core.c
// Dialog system lifecycle and registration management
// Handles dialog list management, creation, destruction, and lookup

#include "dialog_internal.h"
#include "../render.h"
#include "../intuition/itn_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

// ============================================================================
// Module-Private State (Encapsulated)
// ============================================================================

// Global dialog list (for multiple dialogs)
static Dialog *g_dialogs = NULL;

// ============================================================================
// Public Lifecycle Functions
// ============================================================================

// Initialize dialog subsystem
void init_dialogs(void) {
    g_dialogs = NULL;
}

// Clean up all dialogs
void cleanup_dialogs(void) {
    while (g_dialogs) {
        Dialog *next = g_dialogs->next;
        if (g_dialogs->canvas) {
            itn_canvas_destroy(g_dialogs->canvas);
        }
        free(g_dialogs);
        g_dialogs = next;
    }
}

// ============================================================================
// Dialog Creation and Registration
// ============================================================================

// OWNERSHIP: Returns allocated Dialog - caller must call dialog_core_destroy()
// Creates basic dialog structure with canvas - caller adds specific widgets
// Returns NULL on failure (graceful degradation - dialog won't appear)
Dialog* dialog_core_create(DialogType type, const char* title, int width, int height) {
    if (!title) {
        log_error("[ERROR] dialog_core_create called with NULL title");
        return NULL;  // Graceful failure
    }

    // Allocate dialog structure
    Dialog* dialog = calloc(1, sizeof(Dialog));
    if (!dialog) {
        log_error("[ERROR] Failed to allocate Dialog structure - dialog will not appear");
        return NULL;  // Graceful failure
    }

    // Initialize basic state
    dialog->dialog_type = type;
    dialog->input_field = NULL;
    dialog->ok_button = NULL;
    dialog->cancel_button = NULL;
    dialog->on_ok = NULL;
    dialog->on_cancel = NULL;
    dialog->user_data = NULL;
    dialog->next = NULL;

    // Get font from render system
    dialog->font = get_font();
    if (!dialog->font) {
        log_error("[ERROR] Failed to get font for dialog - dialog will not appear");
        free(dialog);
        return NULL;  // Graceful failure
    }

    // Create canvas window
    dialog->canvas = create_canvas(NULL, 200, 150, width, height, DIALOG);
    if (!dialog->canvas) {
        log_error("[ERROR] Failed to create canvas for dialog - dialog will not appear");
        free(dialog);
        return NULL;  // Graceful failure
    }

    // Set title
    dialog->canvas->title_base = strdup(title);
    if (!dialog->canvas->title_base) {
        log_error("[ERROR] strdup failed for dialog title: %s - dialog will not appear", title);
        itn_canvas_destroy(dialog->canvas);
        free(dialog);
        return NULL;  // Graceful failure
    }

    // Standard dialog properties
    dialog->canvas->title_change = NULL;
    dialog->canvas->bg_color = GRAY;
    dialog->canvas->disable_scrollbars = true;

    return dialog;
}

// Register dialog in global list (after widgets are created)
void dialog_core_register(Dialog *dialog) {
    if (!dialog) return;

    dialog->next = g_dialogs;
    g_dialogs = dialog;
}

// ============================================================================
// Dialog Destruction
// ============================================================================

// Close and cleanup specific dialog
// OWNERSHIP: Complete cleanup - frees widgets, canvas, and dialog struct
void destroy_dialog(Dialog *dialog) {
    if (!dialog) return;

    // Remove from dialog list
    if (g_dialogs == dialog) {
        g_dialogs = dialog->next;
    } else {
        for (Dialog *d = g_dialogs; d; d = d->next) {
            if (d->next == dialog) {
                d->next = dialog->next;
                break;
            }
        }
    }

    // Clean up InputField widget and its dropdown
    if (dialog->input_field) {
        // Make sure to close any open dropdown first
        if (dialog->input_field->dropdown_open) {
            inputfield_hide_completions(dialog->input_field, itn_core_get_display());
        }
        inputfield_destroy(dialog->input_field);
        dialog->input_field = NULL;
    }

    // Clean up toolkit buttons
    if (dialog->ok_button) {
        button_destroy(dialog->ok_button);
        dialog->ok_button = NULL;
    }
    if (dialog->cancel_button) {
        button_destroy(dialog->cancel_button);
        dialog->cancel_button = NULL;
    }

    // Font is managed by font_manager - don't close it!
    dialog->font = NULL;  // Just clear the reference

    // Clean up canvas and memory
    if (dialog->canvas) {
        itn_canvas_destroy(dialog->canvas);
    }

    // Zero out and free struct
    memset(dialog, 0, sizeof(Dialog));
    free(dialog);
}

// Wrapper for public API
void dialog_core_destroy(Dialog *dialog) {
    destroy_dialog(dialog);
}

// ============================================================================
// Dialog Lookup and Query
// ============================================================================

// Check if canvas is a dialog
bool is_dialog_canvas(Canvas *canvas) {
    if (!canvas) return false;
    for (Dialog *dialog = g_dialogs; dialog; dialog = dialog->next) {
        if (dialog->canvas == canvas) return true;
    }
    return false;
}

// Public wrapper
bool dialog_core_is_dialog(Canvas *canvas) {
    return is_dialog_canvas(canvas);
}

// Get dialog for canvas
Dialog* get_dialog_for_canvas(Canvas *canvas) {
    if (!canvas) return NULL;
    for (Dialog *dialog = g_dialogs; dialog; dialog = dialog->next) {
        if (dialog->canvas == canvas) return dialog;
    }
    return NULL;
}

// Public wrapper
Dialog* dialog_core_get_for_canvas(Canvas *canvas) {
    return get_dialog_for_canvas(canvas);
}

// Close dialog by canvas (called from intuition.c when window X button is clicked)
// OWNERSHIP: Canvas is being destroyed externally by intuition.c, so we NULL it
// before calling destroy_dialog() which will then skip canvas cleanup
void close_dialog_by_canvas(Canvas *canvas) {
    if (!canvas) return;

    Dialog *dialog = get_dialog_for_canvas(canvas);
    if (dialog) {
        // Call cancel callback if it exists
        if (dialog->on_cancel) {
            dialog->on_cancel();
        }

        // Canvas is being destroyed by intuition.c, not by us
        // Set to NULL so destroy_dialog() won't try to destroy it
        dialog->canvas = NULL;

        // Now properly cleanup dialog (widgets, struct, etc)
        destroy_dialog(dialog);
    }
}

// ============================================================================
// Rendering Dispatcher
// ============================================================================

// Main rendering dispatcher - routes to appropriate dialog-specific renderer
void render_dialog_content(Canvas *canvas) {
    Dialog *dialog = get_dialog_for_canvas(canvas);
    if (!dialog) return;

    Display *dpy = itn_core_get_display();
    Picture dest = canvas->canvas_render;

    // Clear only the content area inside the borders to dialog gray
    int content_x = BORDER_WIDTH_LEFT;
    int content_y = BORDER_HEIGHT_TOP;
    int content_w = canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT_CLIENT;
    int content_h = canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM;
    XRenderFillRectangle(dpy, PictOpSrc, dest, &GRAY, content_x, content_y, content_w, content_h);

    // Dispatch to dialog-specific renderer based on type
    switch (dialog->dialog_type) {
        case DIALOG_RENAME:
            dialog_rename_render_content(canvas, dialog);
            break;

        case DIALOG_EXECUTE_COMMAND:
            dialog_execute_render_content(canvas, dialog);
            break;

        case DIALOG_DELETE_CONFIRM:
            dialog_delete_render_content(canvas, dialog);
            break;

        default:
            log_error("[ERROR] render_dialog_content: Unknown dialog type %d", dialog->dialog_type);
            break;
    }
}
