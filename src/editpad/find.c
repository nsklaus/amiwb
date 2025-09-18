#include "find.h"
#include "editpad.h"
#include "../toolkit/textview.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// Button callbacks
static void find_next_callback(void *user_data);
static void replace_once_callback(void *user_data);
static void replace_all_callback(void *user_data);
static void find_prev_callback(void *user_data);

// Create find dialog
FindDialog* find_dialog_create(EditPad *editpad) {
    if (!editpad || !editpad->display) return NULL;
    
    FindDialog *find = calloc(1, sizeof(FindDialog));
    if (!find) return NULL;
    
    find->editpad = editpad;
    
    // Create base dialog
    find->base = dialog_create(editpad->display, editpad->main_window, DIALOG_FIND);
    if (!find->base) {
        free(find);
        return NULL;
    }
    
    // Store FindDialog pointer in base dialog
    find->base->dialog_data = find;
    
    // Create UI elements
    // Layout constants
    int field_x = 80;  // X position for input fields
    int field_width = 200;  // Shorter to make room for buttons
    int button_width = 60;  // Smaller buttons
    int button_height = 25;
    int button_spacing = 5;
    int field_height = 25;
    
    // Calculate button positions (after the input field)
    int buttons_x = field_x + field_width + 10;
    
    // Line 1: Find field and Next/Prev buttons - centered vertically at y=20
    int find_y = 20;  // Moved up for compact 100px height dialog
    find->find_field = dialog_add_field(find->base, field_x, find_y, field_width, field_height);
    if (find->find_field) {
        inputfield_set_focus(find->find_field, true);
    }

    find->find_next_button = dialog_add_button(find->base,
        buttons_x, find_y, button_width, button_height,
        "Next", find_next_callback);

    find->find_prev_button = dialog_add_button(find->base,
        buttons_x + button_width + button_spacing, find_y, button_width, button_height,
        "Prev", find_prev_callback);

    // Line 2: Replace field and Once/All buttons at y=55
    int replace_y = 55;  // Compact spacing for 100px height dialog
    find->replace_field = dialog_add_field(find->base, field_x, replace_y, field_width, field_height);
    
    find->replace_once_button = dialog_add_button(find->base,
        buttons_x, replace_y, button_width, button_height,
        "Once", replace_once_callback);
    
    find->replace_all_button = dialog_add_button(find->base,
        buttons_x + button_width + button_spacing, replace_y, button_width, button_height,
        "All", replace_all_callback);

    // Cancel button removed - users can close with window X button or ESC key
    
    // Initialize state
    find->case_sensitive = false;
    find->wrap_around = true;
    find->last_match_line = -1;
    find->last_match_col = -1;
    memset(find->last_search, 0, sizeof(find->last_search));
    
    return find;
}

// Destroy find dialog
void find_dialog_destroy(FindDialog *dialog) {
    if (!dialog) return;
    
    if (dialog->base) {
        dialog_destroy(dialog->base);
    }
    
    free(dialog);
}

// Show find dialog
void find_dialog_show(FindDialog *dialog) {
    if (!dialog || !dialog->base) return;
    
    // Get selected text from TextView if any
    if (dialog->editpad && dialog->editpad->text_view) {
        char *selected = textview_get_selection(dialog->editpad->text_view);
        if (selected && *selected) {
            // Limit to single line for search
            char *newline = strchr(selected, '\n');
            if (newline) *newline = '\0';
            
            find_dialog_set_search_text(dialog, selected);
            free(selected);
        }
    }
    
    // Reset search position
    dialog->last_match_line = -1;
    dialog->last_match_col = -1;
    
    dialog_show(dialog->base);
}

// Hide find dialog
void find_dialog_hide(FindDialog *dialog) {
    if (!dialog || !dialog->base) return;
    dialog_hide(dialog->base);
}

// Handle events
bool find_dialog_handle_event(FindDialog *dialog, XEvent *event) {
    if (!dialog || !dialog->base) return false;
    
    // Special handling for Enter key in find field
    if (event->type == KeyPress) {
        KeySym keysym;
        char buffer[32];
        XLookupString(&event->xkey, buffer, sizeof(buffer), &keysym, NULL);
        
        if (keysym == XK_Return || keysym == XK_KP_Enter) {
            // Enter triggers Find Next
            find_next_callback(dialog->base);
            return true;
        }
    }
    
    return dialog_handle_event(dialog->base, event);
}

// Set initial search text
void find_dialog_set_search_text(FindDialog *dialog, const char *text) {
    if (!dialog || !dialog->find_field || !text) return;
    
    inputfield_set_text(dialog->find_field, text);
    
    // Select all text for easy replacement
    const char *field_text = inputfield_get_text(dialog->find_field);
    if (field_text && *field_text) {
        dialog->find_field->cursor_pos = strlen(field_text);
        dialog->find_field->selection_start = 0;
    }
}


// Search next
void find_dialog_search_next(FindDialog *dialog) {
    if (!dialog || !dialog->find_field || !dialog->editpad || !dialog->editpad->text_view) return;
    
    const char *search_text = inputfield_get_text(dialog->find_field);
    if (!search_text || !*search_text) return;
    
    // Store the search text
    strncpy(dialog->last_search, search_text, sizeof(dialog->last_search) - 1);
    
    // Perform the search
    bool found = textview_find_next(dialog->editpad->text_view, search_text, 
                                    dialog->case_sensitive, dialog->wrap_around);
    
    if (!found) {
        // TODO: Show "Not found" message (when we have status bar or message dialog)
    }
}

// Search previous
void find_dialog_search_prev(FindDialog *dialog) {
    if (!dialog || !dialog->find_field || !dialog->editpad || !dialog->editpad->text_view) return;
    
    const char *search_text = inputfield_get_text(dialog->find_field);
    if (!search_text || !*search_text) return;
    
    // Store the search text
    strncpy(dialog->last_search, search_text, sizeof(dialog->last_search) - 1);
    
    // Perform the search
    bool found = textview_find_prev(dialog->editpad->text_view, search_text,
                                    dialog->case_sensitive, dialog->wrap_around);
    
    if (!found) {
        // TODO: Show "Not found" message (when we have status bar or message dialog)
    }
}

// Button callbacks
static void find_next_callback(void *user_data) {
    Dialog *base = (Dialog*)user_data;
    if (!base || !base->dialog_data) return;
    
    FindDialog *find = (FindDialog*)base->dialog_data;
    find_dialog_search_next(find);
}

static void find_prev_callback(void *user_data) {
    Dialog *base = (Dialog*)user_data;
    if (!base || !base->dialog_data) return;
    
    FindDialog *find = (FindDialog*)base->dialog_data;
    find_dialog_search_prev(find);
}

static void replace_once_callback(void *user_data) {
    Dialog *base = (Dialog*)user_data;
    if (!base || !base->dialog_data) return;
    
    FindDialog *find = (FindDialog*)base->dialog_data;
    if (!find->editpad || !find->editpad->text_view) return;
    
    const char *search_text = inputfield_get_text(find->find_field);
    const char *replace_text = inputfield_get_text(find->replace_field);
    
    if (!search_text || !*search_text) return;
    if (!replace_text) replace_text = "";  // Allow empty replacement
    
    TextView *tv = find->editpad->text_view;
    
    // Check if current selection matches search text
    char *selected = textview_get_selection(tv);
    if (selected) {
        // If selection matches search text, replace it
        if ((find->case_sensitive && strcmp(selected, search_text) == 0) ||
            (!find->case_sensitive && strcasecmp(selected, search_text) == 0)) {
            textview_replace_selection(tv, replace_text);
        }
        free(selected);
    }
    
    // Find next occurrence
    find_dialog_search_next(find);
}

static void replace_all_callback(void *user_data) {
    Dialog *base = (Dialog*)user_data;
    if (!base || !base->dialog_data) return;
    
    FindDialog *find = (FindDialog*)base->dialog_data;
    if (!find->editpad || !find->editpad->text_view) return;
    
    const char *search_text = inputfield_get_text(find->find_field);
    const char *replace_text = inputfield_get_text(find->replace_field);
    
    if (!search_text || !*search_text) return;
    if (!replace_text) replace_text = "";  // Allow empty replacement
    
    // Replace all occurrences
    int count = textview_replace_all(find->editpad->text_view, search_text,
                                     replace_text, find->case_sensitive);
    
    // TODO: Show count message (e.g., "Replaced 5 occurrences")
    (void)count;  // Suppress unused warning for now
}