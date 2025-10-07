// File: dialogs.h
// AmigaOS-style dialog system for AmiWB
// Implements rename dialog as regular WINDOW canvas with custom content rendering
#ifndef DIALOGS_H
#define DIALOGS_H

#include "config.h"
#include "intuition/itn_public.h"
#include "../toolkit/inputfield/inputfield.h"
#include "../toolkit/button/button.h"
#include "../toolkit/progressbar/progressbar.h"
#include <stdbool.h>
#include <limits.h>

// Buffer sizes are now defined in config.h

// Dialog types
typedef enum {
    DIALOG_RENAME,
    DIALOG_DELETE_CONFIRM,
    DIALOG_EXECUTE_COMMAND,
    DIALOG_PROGRESS
} DialogType;

// Progress operation types
typedef enum {
    PROGRESS_COPY,
    PROGRESS_MOVE,
    PROGRESS_DELETE,
    PROGRESS_EXTRACT
} ProgressOperation;

// Dialog state structure
typedef struct Dialog {
    Canvas *canvas;                    // Regular WINDOW-type canvas
    DialogType dialog_type;            // Type of dialog (rename or delete confirmation)
    InputField *input_field;           // Toolkit input field for text entry
    Button *ok_button;                 // Toolkit OK button
    Button *cancel_button;             // Toolkit Cancel button
    char original_name[NAME_SIZE];     // Original filename (for display only)
    char text_buffer[NAME_SIZE];       // For delete confirmation message
    void (*on_ok)(const char *new_name);     // Success callback
    void (*on_cancel)(void);                 // Cancel callback
    struct Dialog *next;               // For multiple dialogs
    void *user_data;                   // Optional user data for callbacks
    XftFont *font;                     // Font for the dialog (shared with InputField)
} Dialog;

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

// Dialog lifecycle - consistent create/destroy pattern
void destroy_dialog(Dialog *dialog);  // Complete cleanup
void close_dialog_by_canvas(Canvas *canvas);  // Special case: called when canvas is destroyed

// Event handlers (called from events.c)
bool dialogs_handle_key_press(XKeyEvent *event);
bool dialogs_handle_button_press(XButtonEvent *event);
bool dialogs_handle_button_release(XButtonEvent *event);
bool dialogs_handle_motion(XMotionEvent *event);

// Query functions
bool is_dialog_canvas(Canvas *canvas);
Dialog *get_dialog_for_canvas(Canvas *canvas);

// Rendering (called from render.c)
void render_dialog_content(Canvas *canvas);

// Progress dialog structure
typedef struct ProgressDialog {
    Canvas *canvas;
    ProgressBar *progress_bar;      // Toolkit progress bar widget
    ProgressOperation operation;
    char current_file[PATH_SIZE];
    float percent;
    int files_done;
    int files_total;
    off_t bytes_done;
    off_t bytes_total;
    int pipe_fd;
    pid_t child_pid;
    time_t start_time;              // When operation started
    bool abort_requested;
    void (*on_abort)(void);
    struct ProgressDialog *next;
} ProgressDialog;

// Progress dialog functions
ProgressDialog* show_progress_dialog(ProgressOperation op, const char *title);
void update_progress_dialog(ProgressDialog *dialog, const char *file, float percent);
void close_progress_dialog(ProgressDialog *dialog);
void close_progress_dialog_by_canvas(Canvas *canvas);
bool is_progress_dialog(Canvas *canvas);
ProgressDialog* get_progress_dialog_for_canvas(Canvas *canvas);
void render_progress_dialog_content(Canvas *canvas);

// Progress dialog list management
void add_progress_dialog_to_list(ProgressDialog *dialog);
ProgressDialog *get_all_progress_dialogs(void);
void remove_progress_dialog_from_list(ProgressDialog *dialog);

#endif