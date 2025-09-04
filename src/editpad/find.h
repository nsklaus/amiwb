#ifndef FIND_DIALOG_H
#define FIND_DIALOG_H

#include <X11/Xlib.h>
#include <stdbool.h>
#include "../toolkit/inputfield.h"
#include "../toolkit/button.h"
#include "../amiwb/config.h"
#include "dialog.h"

// Forward declaration
struct EditPad;

typedef struct FindDialog {
    Dialog *base;  // Base dialog
    struct EditPad *editpad;  // Parent EditPad instance
    
    // UI elements
    InputField *find_field;
    
    Button *find_next_button;
    Button *find_prev_button;
    Button *close_button;
    
    // Options (future: add checkboxes when toolkit has them)
    bool case_sensitive;
    bool wrap_around;
    
    // State
    char last_search[NAME_SIZE];
    int last_match_line;
    int last_match_col;
    
} FindDialog;

// Create and destroy
FindDialog* find_dialog_create(struct EditPad *editpad);
void find_dialog_destroy(FindDialog *dialog);

// Show/hide
void find_dialog_show(FindDialog *dialog);
void find_dialog_hide(FindDialog *dialog);

// Handle events
bool find_dialog_handle_event(FindDialog *dialog, XEvent *event);

// Set initial search text (e.g., from selected text)
void find_dialog_set_search_text(FindDialog *dialog, const char *text);

// Search functions
void find_dialog_search_next(FindDialog *dialog);
void find_dialog_search_prev(FindDialog *dialog);

#endif // FIND_DIALOG_H