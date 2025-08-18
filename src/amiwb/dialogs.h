// File: dialogs.h
// AmigaOS-style dialog system for AmiWB
// Implements rename dialog as regular WINDOW canvas with custom content rendering
#ifndef DIALOGS_H
#define DIALOGS_H

#include "config.h"
#include "intuition.h"
#include <stdbool.h>
#include <limits.h>

// Buffer sizes are now defined in config.h

// Dialog types
typedef enum {
    DIALOG_RENAME,
    DIALOG_DELETE_CONFIRM,
    DIALOG_EXECUTE_COMMAND
} DialogType;

// Dialog state structure
typedef struct RenameDialog {
    Canvas *canvas;                    // Regular WINDOW-type canvas
    DialogType dialog_type;            // Type of dialog (rename or delete confirmation)
    char text_buffer[NAME_SIZE];    // Filename buffer (or message for delete)
    char original_name[NAME_SIZE];  // Original filename (for display only)
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
    
    // Completion support for execute dialog
    Canvas *completion_dropdown;       // Dropdown window for completions
    char **completion_candidates;      // Array of completion strings
    int completion_count;              // Number of completion candidates
    int completion_selected;           // Currently selected completion (-1 if none)
    char completion_prefix[NAME_SIZE];  // The prefix being completed
    int completion_prefix_len;         // Length of prefix in text buffer
} RenameDialog;

// Dialog lifecycle
void init_dialogs(void);
void cleanup_dialogs(void);

// Show rename dialog
void show_rename_dialog(const char *old_name, 
                       void (*on_ok)(const char *new_name),
                       void (*on_cancel)(void),
                       void *user_data);

// Show delete confirmation dialog - CRITICAL FOR DATA SAFETY
void show_delete_confirmation(const char *message,
                             void (*on_confirm)(void),
                             void (*on_cancel)(void));

// Show execute command dialog
void show_execute_dialog(void (*on_ok)(const char *command),
                        void (*on_cancel)(void));

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