#include "find.h"
#include "editpad.h"
#include "../toolkit/textview.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// Button callbacks
static void find_next_callback(void *user_data);
static void find_prev_callback(void *user_data);
static void close_callback(void *user_data);

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
    // Field positions
    int field_x = 80;
    int field_width = 280;
    int button_width = 80;
    int button_height = 25;
    
    // Find field at y=40
    find->find_field = dialog_add_field(find->base, field_x, 40, field_width, 25);
    if (find->find_field) {
        inputfield_set_focus(find->find_field, true);
    }
    
    // Find Next and Find Prev buttons at y=80
    int button_y = 80;
    int button_spacing = 10;
    int find_buttons_width = (button_width * 2) + button_spacing;
    int find_buttons_x = (find->base->width - find_buttons_width) / 2;
    
    find->find_next_button = dialog_add_button(find->base, 
        find_buttons_x, button_y, button_width, button_height,
        "Find Next", find_next_callback);
    
    find->find_prev_button = dialog_add_button(find->base,
        find_buttons_x + button_width + button_spacing, button_y, button_width, button_height,
        "Find Prev", find_prev_callback);
    
    // Cancel button centered at bottom with breathing room (25px from bottom)
    int cancel_y = find->base->height - button_height - 25;
    int cancel_x = (find->base->width - button_width) / 2;
    
    find->close_button = dialog_add_button(find->base,
        cancel_x, cancel_y, button_width, button_height,
        "Cancel", close_callback);
    
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
    
    // TODO: Implement actual search when TextView has search support
    // For now, just store the search text
    strncpy(dialog->last_search, search_text, sizeof(dialog->last_search) - 1);
}

// Search previous
void find_dialog_search_prev(FindDialog *dialog) {
    if (!dialog || !dialog->find_field || !dialog->editpad || !dialog->editpad->text_view) return;
    
    const char *search_text = inputfield_get_text(dialog->find_field);
    if (!search_text || !*search_text) return;
    
    // TODO: Implement actual search when TextView has search support
    // For now, just store the search text
    strncpy(dialog->last_search, search_text, sizeof(dialog->last_search) - 1);
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

static void close_callback(void *user_data) {
    Dialog *base = (Dialog*)user_data;
    if (!base) return;
    
    // Destroy the window like the X button does
    // This will trigger DestroyNotify which cleans up the dialog
    if (base->xft_draw) {
        XftDrawDestroy(base->xft_draw);
        base->xft_draw = NULL;
    }
    XDestroyWindow(base->display, base->window);
}