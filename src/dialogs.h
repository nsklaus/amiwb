// File: dialogs.h
// AmigaOS-style dialog system for AmiWB
// Implements rename dialog as regular WINDOW canvas with custom content rendering
#ifndef DIALOGS_H
#define DIALOGS_H

#include "intuition.h"
#include <stdbool.h>
#include <limits.h>

// Use system filename length limit
#ifndef NAME_MAX
#define NAME_MAX 255
#endif

// Dialog state structure
typedef struct RenameDialog {
    Canvas *canvas;                    // Regular WINDOW-type canvas
    char text_buffer[NAME_MAX + 1];    // Filename buffer
    int cursor_pos;                    // Current cursor position
    int selection_start;               // Text selection start (-1 if no selection)
    int selection_end;                 // Text selection end
    int visible_start;                 // First visible character (for horizontal scrolling)
    bool input_has_focus;              // True if input box has focus
    bool ok_button_pressed;            // True while OK button is pressed
    bool cancel_button_pressed;        // True while Cancel button is pressed
    void (*on_ok)(const char *new_name);     // Success callback
    void (*on_cancel)(void);                 // Cancel callback
    struct RenameDialog *next;         // For multiple dialogs
    void *user_data;                   // Optional user data for callbacks
} RenameDialog;

// Dialog lifecycle
void init_dialogs(void);
void cleanup_dialogs(void);

// Show rename dialog
void show_rename_dialog(const char *old_name, 
                       void (*on_ok)(const char *new_name),
                       void (*on_cancel)(void),
                       void *user_data);

// Close specific dialog
void close_rename_dialog(RenameDialog *dialog);

// Event handlers (called from events.c)
bool dialogs_handle_key_press(XKeyEvent *event);
bool dialogs_handle_button_press(XButtonEvent *event);
bool dialogs_handle_button_release(XButtonEvent *event);
bool dialogs_handle_motion(XMotionEvent *event);

// Query functions
bool is_dialog_canvas(Canvas *canvas);
RenameDialog *get_dialog_for_canvas(Canvas *canvas);

// Rendering (called from render.c)
void render_dialog_content(Canvas *canvas);

#endif