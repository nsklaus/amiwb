// File: dialog_internal.h
// Internal API for dialog system modules
// This header is included ONLY by dialog_*.c files within dialogs/

#ifndef DIALOG_INTERNAL_H
#define DIALOG_INTERNAL_H

#include "dialog_public.h"
#include "../config.h"
#include "../intuition/itn_public.h"
#include "../../toolkit/inputfield/inputfield.h"
#include "../../toolkit/button/button.h"
#include "../../toolkit/progressbar/progressbar.h"
#include "../../toolkit/listview/listview.h"
#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>
#include <X11/Xft/Xft.h>
#include <stdbool.h>
#include <sys/types.h>
#include <time.h>

// ============================================================================
// Dialog Types (Internal)
// ============================================================================

typedef enum {
    DIALOG_RENAME,
    DIALOG_DELETE_CONFIRM,
    DIALOG_EXECUTE_COMMAND,
    DIALOG_PROGRESS
} DialogType;

// ============================================================================
// Dialog Structure (Internal)
// ============================================================================

struct Dialog {
    Canvas *canvas;                    // Regular WINDOW-type canvas
    DialogType dialog_type;            // Type of dialog
    InputField *input_field;           // Toolkit input field for text entry
    Button *ok_button;                 // Toolkit OK button
    Button *cancel_button;             // Toolkit Cancel button
    char original_name[NAME_SIZE];     // Original filename (for display only)
    char text_buffer[NAME_SIZE];       // For delete confirmation message
    void (*on_ok)(const char *new_name);     // Success callback
    void (*on_cancel)(void);                 // Cancel callback
    struct Dialog *next;               // For multiple dialogs
    void *user_data;                   // Optional user data for callbacks
    XftFont *font;                     // Font for the dialog
};

// ============================================================================
// Dialog Rendering Constants
// ============================================================================

#define DIALOG_MARGIN 20
#define INPUT_HEIGHT 24  // Taller for better text spacing
#define BUTTON_WIDTH 80
#define BUTTON_HEIGHT 25
#define ELEMENT_SPACING 15
#define LABEL_WIDTH 80   // Width for "New Name:" label

// ============================================================================
// dialog_core.c - Lifecycle Management
// ============================================================================

// OWNERSHIP: Returns allocated Dialog - caller must call destroy_dialog()
// Creates basic dialog structure with canvas - caller adds specific widgets
// Returns NULL on failure (graceful degradation - dialog won't appear)
Dialog* dialog_core_create(DialogType type, const char *title, int width, int height);

// Register dialog in global list
void dialog_core_register(Dialog *dialog);

// Destroy dialog and remove from list
void dialog_core_destroy(Dialog *dialog);

// Get dialog for canvas (used by event handlers)
Dialog* dialog_core_get_for_canvas(Canvas *canvas);

// Check if canvas is a dialog
bool dialog_core_is_dialog(Canvas *canvas);

// ============================================================================
// dialog_base.c - Shared Rendering Primitives
// ============================================================================

// Draw inset box for input fields
void dialog_base_draw_inset_box(Picture dest, int x, int y, int w, int h);

// Draw checkerboard pattern for progress dialogs
void dialog_base_draw_checkerboard(Picture dest, int x, int y, int w, int h);

// Calculate layout for dialog widgets
void dialog_base_calculate_layout(Dialog *dialog, int *input_x, int *input_y,
                                  int *input_w, int *ok_x, int *ok_y,
                                  int *cancel_x, int *cancel_y);

// Render text content with word wrapping
void dialog_base_render_text_content(Dialog *dialog, Picture dest,
                                     int input_x, int input_y, int input_w,
                                     int ok_x, int ok_y, int cancel_x, int cancel_y);

// ============================================================================
// dialog_events.c - Event Routing (Internal Dispatch)
// ============================================================================

// Individual dialog type handlers (called by dialog_events.c dispatcher)
bool dialog_rename_handle_key(Dialog *dialog, XKeyEvent *event);
bool dialog_execute_handle_key(Dialog *dialog, XKeyEvent *event);
bool dialog_delete_handle_key(Dialog *dialog, XKeyEvent *event);

bool dialog_rename_handle_button_press(Dialog *dialog, XButtonEvent *event);
bool dialog_execute_handle_button_press(Dialog *dialog, XButtonEvent *event);
bool dialog_delete_handle_button_press(Dialog *dialog, XButtonEvent *event);

bool dialog_rename_handle_button_release(Dialog *dialog, XButtonEvent *event);
bool dialog_execute_handle_button_release(Dialog *dialog, XButtonEvent *event);
bool dialog_delete_handle_button_release(Dialog *dialog, XButtonEvent *event);

bool dialog_rename_handle_motion(Dialog *dialog, XMotionEvent *event);
bool dialog_execute_handle_motion(Dialog *dialog, XMotionEvent *event);
bool dialog_delete_handle_motion(Dialog *dialog, XMotionEvent *event);

// ============================================================================
// dialog_rename.c, dialog_execute.c, dialog_delete.c - Rendering
// ============================================================================

void dialog_rename_render_content(Canvas *canvas, Dialog *dialog);
void dialog_execute_render_content(Canvas *canvas, Dialog *dialog);
void dialog_delete_render_content(Canvas *canvas, Dialog *dialog);

#endif // DIALOG_INTERNAL_H
