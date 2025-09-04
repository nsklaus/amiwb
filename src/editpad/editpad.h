#ifndef EDITPAD_H
#define EDITPAD_H

#include <X11/Xlib.h>
#include <stdbool.h>
#include "../toolkit/textview.h"
#include "../amiwb/config.h"
#include "syntax_highlight.h"

typedef struct {
    Display *display;
    Window root;
    Window main_window;
    TextView *text_view;
    
    // File info
    char current_file[PATH_SIZE];
    bool modified;
    bool untitled;
    
    // Settings (from editpadrc)
    bool line_numbers;
    bool word_wrap;
    int tab_width;
    bool auto_indent;
    
    // Color settings (from editpadrc)
    unsigned int selection_bg;  // Selection background color
    unsigned int selection_fg;  // Selection foreground color
    unsigned int cursor_color;  // Cursor color
    
    // Syntax highlighting
    SyntaxHighlight *syntax;
    
    // Menu state
    bool has_focus;
    
} EditPad;

// Main functions
EditPad* editpad_create(Display *display);
void editpad_destroy(EditPad *ep);
void editpad_run(EditPad *ep);

// File operations
void editpad_new_file(EditPad *ep);
void editpad_open_file(EditPad *ep, const char *filename);
void editpad_save_file(EditPad *ep);
void editpad_save_file_as(EditPad *ep);

// Edit operations
void editpad_undo(EditPad *ep);
void editpad_redo(EditPad *ep);
void editpad_cut(EditPad *ep);
void editpad_copy(EditPad *ep);
void editpad_paste(EditPad *ep);
void editpad_select_all(EditPad *ep);

// Search operations
void editpad_find(EditPad *ep);
void editpad_replace(EditPad *ep);
void editpad_goto_line(EditPad *ep);

// View operations
void editpad_toggle_line_numbers(EditPad *ep);
void editpad_toggle_word_wrap(EditPad *ep);

// Window operations
void editpad_update_title(EditPad *ep);
void editpad_handle_focus_change(EditPad *ep, bool focused);

// Configuration
void editpad_load_config(EditPad *ep);

// Logging (defined in editpad_main.c)
void log_error(const char *format, ...);
void editpad_set_log_path(const char *path);

#endif // EDITPAD_H