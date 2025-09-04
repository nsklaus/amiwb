#ifndef TEXTVIEW_H
#define TEXTVIEW_H

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>
#include <stdbool.h>
#include <stdint.h>

// Scrollbar constants
#define VERT_SCROLLBAR_WIDTH 18
#define HORI_SCROLLBAR_HEIGHT 17
#define VERT_ARROW_HEIGHT 17
#define HORI_ARROW_WIDTH 16
#define VERT_KNOB_WIDTH 11
#define HORI_KNOB_HEIGHT 11
#define SCROLLBAR_KNOB_PADDING 2  // Same padding for both scrollbars

// TextView widget for multi-line text editing
typedef struct TextView {
    Display *display;
    Window window;
    Window parent;
    
    // Position and size
    int x, y;
    int width, height;
    
    // Text buffer
    char **lines;           // Array of text lines
    int line_count;         // Number of lines
    int line_capacity;      // Allocated capacity
    
    // Line width cache (to avoid recalculating on every draw)
    int *line_widths;       // Cached width of each line in pixels
    bool need_width_recalc; // Flag to trigger recalculation
    
    // Cursor position
    int cursor_line;        // Current line (0-based)
    int cursor_col;         // Current column (0-based)
    bool cursor_visible;    // For blinking
    
    // Selection
    int sel_start_line;
    int sel_start_col;
    int sel_end_line;
    int sel_end_col;
    bool has_selection;
    
    // Scrolling
    int scroll_x;           // Horizontal scroll offset
    int scroll_y;           // Vertical scroll offset (in lines)
    int visible_lines;      // Number of lines that fit in view
    
    // Vertical scrollbar state
    int scrollbar_knob_y;
    int scrollbar_knob_height;
    bool scrollbar_dragging;
    int scrollbar_drag_offset;
    bool scrollbar_visible;  // Whether vertical scrollbar is needed
    
    // Horizontal scrollbar state
    int h_scrollbar_knob_x;
    int h_scrollbar_knob_width;
    bool h_scrollbar_dragging;
    int h_scrollbar_drag_offset;
    bool h_scrollbar_visible;  // Whether horizontal scrollbar is needed
    int max_line_width;  // Longest line width in pixels
    
    // Display settings
    XftFont *font;
    XftDraw *xft_draw;
    int line_height;        // Calculated from font
    int char_width;         // Average character width
    bool line_numbers;      // Show line numbers
    bool word_wrap;         // Enable word wrap
    int line_number_width;  // Width of line number area
    
    // Colors (stored values for creating XftColors)
    uint32_t bg_color;      // Background (gray)
    uint32_t fg_color;      // Foreground (black)
    uint32_t cursor_color;  // Cursor color
    uint32_t sel_bg_color;  // Selection background
    uint32_t sel_fg_color;  // Selection foreground
    uint32_t line_num_color; // Line number color
    
    // Allocated XftColors (created once, reused for all draws)
    XftColor xft_fg_color;      // Foreground color
    XftColor xft_sel_color;     // Selection foreground color
    XftColor xft_line_num_color; // Line number color
    bool colors_allocated;      // Track if XftColors are allocated
    
    // Previous cursor position (for optimized cursor updates)
    int prev_cursor_line;
    int prev_cursor_col;
    
    // State
    bool has_focus;
    bool modified;
    bool read_only;
    
    // Clipboard support
    char *clipboard_buffer;  // Internal clipboard buffer
    Atom clipboard_atom;     // CLIPBOARD atom
    Atom primary_atom;       // PRIMARY atom  
    Atom targets_atom;       // TARGETS atom
    Atom utf8_atom;          // UTF8_STRING atom
    
    // Callbacks
    void (*on_change)(struct TextView *tv);
    void (*on_cursor_move)(struct TextView *tv);
    void *user_data;
    
    // Syntax highlighting (opaque pointer to avoid circular dependency)
    void *syntax_data;  // Actually TextViewSyntax* but kept opaque
    
    // Undo/Redo support (opaque pointer)
    void *undo_history;  // Actually UndoHistory* but kept opaque
} TextView;

// Creation and destruction
TextView* textview_create(Display *display, Window parent, int x, int y, 
                         int width, int height);
void textview_destroy(TextView *tv);

// Text operations
void textview_set_text(TextView *tv, const char *text);
char* textview_get_text(TextView *tv);  // Caller must free
void textview_insert_char(TextView *tv, char c);
void textview_insert_string(TextView *tv, const char *str);
void textview_delete_char(TextView *tv);  // Delete at cursor
void textview_backspace(TextView *tv);    // Backspace at cursor
void textview_new_line(TextView *tv);

// Cursor movement
void textview_move_cursor(TextView *tv, int line, int col);
void textview_move_cursor_up(TextView *tv);
void textview_move_cursor_down(TextView *tv);
void textview_move_cursor_left(TextView *tv);
void textview_move_cursor_right(TextView *tv);
void textview_move_cursor_home(TextView *tv);     // Start of line
void textview_move_cursor_end(TextView *tv);      // End of line
void textview_move_cursor_page_up(TextView *tv);
void textview_move_cursor_page_down(TextView *tv);

// Selection
void textview_set_selection(TextView *tv, int start_line, int start_col,
                           int end_line, int end_col);
void textview_clear_selection(TextView *tv);
char* textview_get_selection(TextView *tv);  // Caller must free
void textview_delete_selection(TextView *tv);
void textview_select_all(TextView *tv);

// Clipboard operations
void textview_copy(TextView *tv);
void textview_cut(TextView *tv);
void textview_paste(TextView *tv);
void textview_handle_selection_request(TextView *tv, XSelectionRequestEvent *req);
void textview_handle_selection_notify(TextView *tv, XSelectionEvent *sel);

// Undo/Redo operations
void textview_undo(TextView *tv);
void textview_redo(TextView *tv);
bool textview_can_undo(TextView *tv);
bool textview_can_redo(TextView *tv);

// Display
void textview_draw(TextView *tv);
void textview_update_cursor(TextView *tv);  // Optimized cursor-only update
void textview_set_line_numbers(TextView *tv, bool show);
void textview_set_word_wrap(TextView *tv, bool wrap);
void textview_ensure_cursor_visible(TextView *tv);
void textview_update_scrollbar(TextView *tv);

// Color settings
void textview_set_selection_colors(TextView *tv, unsigned int bg, unsigned int fg);
void textview_set_cursor_color(TextView *tv, unsigned int color);

// Syntax highlighting support
// The callback should return an array of color indices (0-15) for each character
// The returned array should be allocated with malloc and will be freed by TextView
typedef void* (*TextViewSyntaxCallback)(void *context, const char *line, int line_num);

void textview_set_syntax_highlight(TextView *tv, void *context,
                                   TextViewSyntaxCallback highlight_func,
                                   uint32_t *palette, int palette_size);
void textview_highlight_all_lines(TextView *tv);

// Event handling
bool textview_handle_key_press(TextView *tv, XKeyEvent *event);
bool textview_handle_button_press(TextView *tv, XButtonEvent *event);
bool textview_handle_button_release(TextView *tv, XButtonEvent *event);
bool textview_handle_motion(TextView *tv, XMotionEvent *event);
bool textview_handle_focus_in(TextView *tv);
bool textview_handle_focus_out(TextView *tv);
bool textview_handle_configure(TextView *tv, XConfigureEvent *event);

// Search operations
bool textview_find_next(TextView *tv, const char *search_text, 
                        bool case_sensitive, bool wrap_around);
bool textview_find_prev(TextView *tv, const char *search_text,
                        bool case_sensitive, bool wrap_around);
void textview_replace_selection(TextView *tv, const char *replacement);
int textview_replace_all(TextView *tv, const char *search_text, 
                        const char *replacement, bool case_sensitive);

#endif // TEXTVIEW_H
