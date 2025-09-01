#ifndef TEXTVIEW_H
#define TEXTVIEW_H

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>
#include <stdbool.h>
#include <stdint.h>

// Scrollbar constants
#define TEXTVIEW_SCROLLBAR_WIDTH 20
#define TEXTVIEW_ARROW_HEIGHT 17

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
    
    // Scrollbar state
    int scrollbar_knob_y;
    int scrollbar_knob_height;
    bool scrollbar_dragging;
    int scrollbar_drag_offset;
    bool scrollbar_visible;  // Whether scrollbar is needed
    
    // Display settings
    XftFont *font;
    XftDraw *xft_draw;
    int line_height;        // Calculated from font
    int char_width;         // Average character width
    bool line_numbers;      // Show line numbers
    bool word_wrap;         // Enable word wrap
    int line_number_width;  // Width of line number area
    
    // Colors
    uint32_t bg_color;      // Background (gray)
    uint32_t fg_color;      // Foreground (black)
    uint32_t cursor_color;  // Cursor color
    uint32_t sel_bg_color;  // Selection background
    uint32_t sel_fg_color;  // Selection foreground
    uint32_t line_num_color; // Line number color
    
    // Syntax highlighting
    void *current_syntax;   // Points to SyntaxDef (forward declaration)
    int **line_colors;      // Color for each character on each line
    bool *lines_dirty;      // Which lines need re-highlighting
    
    // State
    bool has_focus;
    bool modified;
    bool read_only;
    
    // Callbacks
    void (*on_change)(struct TextView *tv);
    void (*on_cursor_move)(struct TextView *tv);
    void *user_data;
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

// Display
void textview_draw(TextView *tv);
void textview_set_line_numbers(TextView *tv, bool show);
void textview_set_word_wrap(TextView *tv, bool wrap);
void textview_ensure_cursor_visible(TextView *tv);
void textview_update_scrollbar(TextView *tv);

// Color settings
void textview_set_selection_colors(TextView *tv, unsigned int bg, unsigned int fg);
void textview_set_cursor_color(TextView *tv, unsigned int color);

// Event handling
bool textview_handle_key_press(TextView *tv, XKeyEvent *event);
bool textview_handle_button_press(TextView *tv, XButtonEvent *event);
bool textview_handle_button_release(TextView *tv, XButtonEvent *event);
bool textview_handle_motion(TextView *tv, XMotionEvent *event);
bool textview_handle_focus_in(TextView *tv);
bool textview_handle_focus_out(TextView *tv);
bool textview_handle_configure(TextView *tv, XConfigureEvent *event);

// Syntax highlighting (implemented later)
void textview_set_syntax(TextView *tv, void *syntax_def);
void textview_update_highlighting(TextView *tv);

#endif // TEXTVIEW_H