#include "textview.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xrender.h>

// Define minimal types for syntax highlighting support
// The actual syntax engine is provided by the application (e.g., EditPad)
typedef enum { SYNTAX_NORMAL = 0, SYNTAX_MAX = 16 } SyntaxColor;
typedef struct SyntaxHighlight SyntaxHighlight;  // Opaque pointer

#define INITIAL_LINE_CAPACITY 100
#define LINE_CHUNK_SIZE 100
#define TAB_WIDTH 4
#define PATH_SIZE 512
#define HIGHLIGHT_BUFFER 10  // Lines above/below viewport to pre-highlight

// Line highlighting cache
typedef struct {
    SyntaxColor *colors;     // Array of colors per character
    bool dirty;              // Needs re-highlighting
    int last_version;        // Text version when highlighted
} LineHighlight;

// Callback function type for syntax highlighting
typedef SyntaxColor* (*SyntaxHighlightFunc)(void *context, const char *line, int line_num);

// Forward declarations for static functions
static void draw_line_with_colors(TextView *tv, int line_num, int x, int y);
static void update_visible_highlighting(TextView *tv);
static void invalidate_lines(TextView *tv, int start_line, int end_line);
static void highlight_line_cached(TextView *tv, int line_num);

// Extended TextView structure fields for syntax highlighting
typedef struct {
    // Syntax highlighting
    void *syntax_context;               // Opaque context (e.g., SyntaxHighlight from EditPad)
    SyntaxHighlightFunc highlight_func; // Function to call for highlighting
    uint32_t color_palette[SYNTAX_MAX]; // RGB colors
    LineHighlight *line_cache;         // Cache per line
    int cache_size;                    // Number of cached lines
    int text_version;                  // Increments on edit
    XftColor xft_colors[SYNTAX_MAX];  // Pre-allocated Xft colors
    bool xft_colors_allocated[SYNTAX_MAX]; // Track which are allocated
} TextViewSyntax;

// =============================================================================
// UNDO/REDO STRUCTURES
// =============================================================================

#define MAX_UNDO_LEVELS 1000  // Maximum undo history

typedef enum {
    UNDO_INSERT_CHAR,    // Single character insertion
    UNDO_DELETE_CHAR,    // Single character deletion (backspace)
    UNDO_INSERT_TEXT,    // Text insertion (paste, etc.)
    UNDO_DELETE_TEXT,    // Text deletion (selection delete, etc.)
    UNDO_NEWLINE,        // Line break insertion
    UNDO_JOIN_LINES,     // Lines joined (backspace at line start)
} UndoType;

typedef struct UndoEntry {
    UndoType type;
    int line;           // Line where operation occurred
    int col;            // Column where operation occurred
    char *text;         // Text that was added/removed
    int text_len;       // Length of text
    
    // For multi-line operations
    int end_line;       // End line for multi-line ops
    int end_col;        // End column for multi-line ops
    
    struct UndoEntry *next;
    struct UndoEntry *prev;
} UndoEntry;

typedef struct {
    UndoEntry *current;     // Current position in history
    UndoEntry *head;        // Oldest entry (start of list)
    UndoEntry *tail;        // Newest entry (end of list)
    int count;              // Number of entries
    int max_count;          // Maximum entries allowed
    bool in_undo_redo;      // Flag to prevent recording during undo/redo
} UndoHistory;

// Forward declarations for undo functions
static void record_undo(TextView *tv, UndoType type, int line, int col, 
                       const char *text, int text_len);
static void clear_redo_history(TextView *tv);
static UndoEntry* create_undo_entry(UndoType type, int line, int col, 
                                   const char *text, int text_len);
static void free_undo_entry(UndoEntry *entry);

// Create a new TextView widget
TextView* textview_create(Display *display, Window parent, int x, int y, 
                         int width, int height) {
    TextView *tv = calloc(1, sizeof(TextView));
    if (!tv) return NULL;
    
    tv->display = display;
    tv->parent = parent;
    tv->x = x;
    tv->y = y;
    tv->width = width;
    tv->height = height;
    
    // Create the window WITHOUT automatic background clearing
    XSetWindowAttributes attrs;
    attrs.background_pixmap = None;  // Don't automatically clear on expose
    attrs.border_pixel = BlackPixel(display, DefaultScreen(display));
    attrs.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | 
                       ButtonReleaseMask | PointerMotionMask | 
                       FocusChangeMask | StructureNotifyMask;
    
    tv->window = XCreateWindow(display, parent, x, y, width, height, 0,
                              CopyFromParent, InputOutput, CopyFromParent,
                              CWBackPixmap | CWBorderPixel | CWEventMask, &attrs);
    
    // Initialize text buffer
    tv->line_capacity = INITIAL_LINE_CAPACITY;
    tv->lines = calloc(tv->line_capacity, sizeof(char*));
    
    // Initialize line width cache
    tv->line_widths = calloc(tv->line_capacity, sizeof(int));
    tv->need_width_recalc = true;
    
    // Start with one empty line
    tv->lines[0] = strdup("");
    tv->line_count = 1;
    
    // Initialize cursor
    tv->cursor_line = 0;
    tv->cursor_col = 0;
    tv->cursor_visible = true;
    tv->prev_cursor_line = 0;
    tv->prev_cursor_col = 0;
    
    // No selection initially
    tv->has_selection = false;
    
    // Initialize vertical scrollbar state
    tv->scrollbar_visible = false;
    tv->scrollbar_dragging = false;
    tv->scrollbar_knob_y = 0;
    tv->scrollbar_knob_height = 0;
    
    // Initialize horizontal scrollbar state
    tv->h_scrollbar_visible = false;
    tv->h_scrollbar_dragging = false;
    tv->h_scrollbar_knob_x = 0;
    tv->h_scrollbar_knob_width = 0;
    tv->max_line_width = 0;
    
    // Colors (defaults, can be overridden)
    tv->bg_color = 0xa2a2a0;  // Gray
    tv->fg_color = 0x000000;  // Black
    tv->cursor_color = 0x4858B0;  // Blue from config.h
    tv->sel_bg_color = 0x99CCFF;  // Light blue selection
    tv->sel_fg_color = 0x000000;  // Black
    tv->line_num_color = 0x666666;  // Dark gray for line numbers
    
    // Load font - use config from editpadrc or default to system font
    // Default to SourceCodePro-Bold from config.h SYSFONT
    char font_path[PATH_SIZE];
    snprintf(font_path, sizeof(font_path), "%s/%s", 
             "/usr/local/share/amiwb", "fonts/SourceCodePro-Bold.otf");
    
    tv->font = XftFontOpenName(display, DefaultScreen(display),
                              "Source Code Pro:style=Bold:size=11");
    
    if (!tv->font) {
        // Fallback to monospace
        tv->font = XftFontOpen(display, DefaultScreen(display),
                              XFT_FAMILY, XftTypeString, "monospace",
                              XFT_SIZE, XftTypeDouble, 11.0,
                              NULL);
    }
    
    // Calculate font metrics
    if (tv->font) {
        XGlyphInfo extents;
        XftTextExtentsUtf8(display, tv->font, (FcChar8*)"M", 1, &extents);
        tv->char_width = extents.xOff;
        tv->line_height = tv->font->height + 2;  // Small padding
        
        // Defensive check: ensure line_height is reasonable
        if (tv->line_height <= 0) {
            fprintf(stderr, "[ERROR] TextView: Invalid line_height calculated: %d\n", tv->line_height);
            tv->line_height = 16;  // Fallback to a reasonable default
        }
        
        tv->visible_lines = tv->height / tv->line_height;
        if (tv->visible_lines <= 0) {
            tv->visible_lines = 1;  // At least 1 line must be visible
        }
    } else {
        // No font loaded - use defaults
        fprintf(stderr, "[WARNING] TextView: No font loaded, using defaults\n");
        tv->char_width = 8;
        tv->line_height = 16;
        tv->visible_lines = tv->height / tv->line_height;
        if (tv->visible_lines <= 0) {
            tv->visible_lines = 1;
        }
    }
    
    // Initialize clipboard atoms
    tv->clipboard_atom = XInternAtom(display, "CLIPBOARD", False);
    tv->primary_atom = XA_PRIMARY;
    tv->targets_atom = XInternAtom(display, "TARGETS", False);
    tv->utf8_atom = XInternAtom(display, "UTF8_STRING", False);
    tv->clipboard_buffer = NULL;
    
    // Initialize undo history
    tv->undo_history = calloc(1, sizeof(UndoHistory));
    if (tv->undo_history) {
        UndoHistory *history = (UndoHistory*)tv->undo_history;
        history->max_count = MAX_UNDO_LEVELS;
        history->in_undo_redo = false;
    }
    
    // Initialize scrollbar
    textview_update_scrollbar(tv);
    
    // Create XftDraw for rendering
    Visual *visual = DefaultVisual(display, DefaultScreen(display));
    Colormap colormap = DefaultColormap(display, DefaultScreen(display));
    tv->xft_draw = XftDrawCreate(display, tv->window, visual, colormap);
    
    // Allocate XftColors once (reused for all drawing)
    if (tv->xft_draw) {
        XRenderColor render_color;
        
        // Foreground color (black)
        render_color.red = 0;
        render_color.green = 0;
        render_color.blue = 0;
        render_color.alpha = 0xFFFF;
        XftColorAllocValue(display, visual, colormap, &render_color, &tv->xft_fg_color);
        
        // Selection foreground color (white)
        render_color.red = 0xFFFF;
        render_color.green = 0xFFFF;
        render_color.blue = 0xFFFF;
        XftColorAllocValue(display, visual, colormap, &render_color, &tv->xft_sel_color);
        
        // Line number color (dark gray)
        render_color.red = 0x6666;
        render_color.green = 0x6666;
        render_color.blue = 0x6666;
        XftColorAllocValue(display, visual, colormap, &render_color, &tv->xft_line_num_color);
        
        tv->colors_allocated = true;
    } else {
        tv->colors_allocated = false;
    }
    
    // Default settings
    tv->line_numbers = false;
    tv->word_wrap = false;
    tv->line_number_width = 0;
    tv->read_only = false;
    tv->modified = false;
    
    // Initialize syntax highlighting data
    tv->syntax_data = NULL;  // Will be allocated when syntax is set
    
    XMapWindow(display, tv->window);
    return tv;
}

// Destroy TextView and free resources
void textview_destroy(TextView *tv) {
    if (!tv) return;
    
    // Free text lines
    for (int i = 0; i < tv->line_count; i++) {
        free(tv->lines[i]);
    }
    free(tv->lines);
    
    // Free line width cache
    if (tv->line_widths) free(tv->line_widths);
    
    // Free clipboard buffer
    if (tv->clipboard_buffer) free(tv->clipboard_buffer);
    
    // Free undo history
    if (tv->undo_history) {
        UndoHistory *history = (UndoHistory*)tv->undo_history;
        
        // Free all undo entries
        UndoEntry *entry = history->head;
        while (entry) {
            UndoEntry *next = entry->next;
            free_undo_entry(entry);
            entry = next;
        }
        
        free(history);
        tv->undo_history = NULL;
    }
    
    // Free syntax highlighting resources
    if (tv->syntax_data) {
        TextViewSyntax *syn = (TextViewSyntax*)tv->syntax_data;
        
        // Free line cache
        if (syn->line_cache) {
            for (int i = 0; i < syn->cache_size; i++) {
                if (syn->line_cache[i].colors) {
                    free(syn->line_cache[i].colors);
                }
            }
            free(syn->line_cache);
        }
        
        // Free allocated XftColors
        for (int i = 0; i < SYNTAX_MAX; i++) {
            if (syn->xft_colors_allocated[i]) {
                XftColorFree(tv->display, DefaultVisual(tv->display, DefaultScreen(tv->display)),
                           DefaultColormap(tv->display, DefaultScreen(tv->display)), &syn->xft_colors[i]);
            }
        }
        
        free(syn);
        tv->syntax_data = NULL;
    }
    
    // Free X resources
    if (tv->colors_allocated) {
        XftColorFree(tv->display, DefaultVisual(tv->display, DefaultScreen(tv->display)),
                     DefaultColormap(tv->display, DefaultScreen(tv->display)), &tv->xft_fg_color);
        XftColorFree(tv->display, DefaultVisual(tv->display, DefaultScreen(tv->display)),
                     DefaultColormap(tv->display, DefaultScreen(tv->display)), &tv->xft_sel_color);
        XftColorFree(tv->display, DefaultVisual(tv->display, DefaultScreen(tv->display)),
                     DefaultColormap(tv->display, DefaultScreen(tv->display)), &tv->xft_line_num_color);
    }
    if (tv->xft_draw) XftDrawDestroy(tv->xft_draw);
    if (tv->font) XftFontClose(tv->display, tv->font);
    if (tv->window) XDestroyWindow(tv->display, tv->window);
    
    free(tv);
}

// Set the entire text content
void textview_set_text(TextView *tv, const char *text) {
    if (!tv || !text) return;
    
    // Clear existing lines
    for (int i = 0; i < tv->line_count; i++) {
        free(tv->lines[i]);
        tv->lines[i] = NULL;
    }
    
    // Split text into lines
    tv->line_count = 0;
    const char *start = text;
    const char *end;
    
    while (*start) {
        end = strchr(start, '\n');
        if (!end) end = start + strlen(start);
        
        // Grow buffer if needed
        if (tv->line_count >= tv->line_capacity) {
            tv->line_capacity += LINE_CHUNK_SIZE;
            tv->lines = realloc(tv->lines, tv->line_capacity * sizeof(char*));
            tv->line_widths = realloc(tv->line_widths, tv->line_capacity * sizeof(int));
        }
        
        // Copy line (strip \r if present)
        int len = end - start;
        // Check if line ends with \r (CRLF)
        if (len > 0 && start[len - 1] == '\r') {
            len--;  // Remove the \r
        }
        
        // Count tabs to calculate expanded length
        int tab_count = 0;
        for (int i = 0; i < len; i++) {
            if (start[i] == '\t') tab_count++;
        }
        
        // Allocate space for expanded line (each tab becomes up to TAB_WIDTH spaces)
        int expanded_len = len + (tab_count * (TAB_WIDTH - 1));
        tv->lines[tv->line_count] = malloc(expanded_len + 1);
        
        // Copy line, expanding tabs to spaces
        int dest_pos = 0;
        for (int src_pos = 0; src_pos < len; src_pos++) {
            if (start[src_pos] == '\t') {
                // Expand tab to spaces up to next tab stop
                int spaces_to_add = TAB_WIDTH - (dest_pos % TAB_WIDTH);
                for (int j = 0; j < spaces_to_add; j++) {
                    tv->lines[tv->line_count][dest_pos++] = ' ';
                }
            } else {
                tv->lines[tv->line_count][dest_pos++] = start[src_pos];
            }
        }
        tv->lines[tv->line_count][dest_pos] = '\0';
        tv->line_count++;
        
        start = (*end == '\n') ? end + 1 : end;
    }
    
    // Ensure at least one line
    if (tv->line_count == 0) {
        tv->lines[0] = strdup("");
        tv->line_count = 1;
    }
    
    // Reset cursor and scroll position
    tv->cursor_line = 0;
    tv->cursor_col = 0;
    tv->scroll_x = 0;
    tv->scroll_y = 0;
    tv->has_selection = false;
    tv->modified = false;
    tv->need_width_recalc = true;  // Need to recalculate line widths
    
    // Invalidate all syntax highlighting
    if (tv->syntax_data) {
        invalidate_lines(tv, 0, tv->line_count - 1);
    }
    
    // Update scrollbar and redraw
    textview_update_scrollbar(tv);
    textview_draw(tv);
}

// Get all text as a single string
char* textview_get_text(TextView *tv) {
    if (!tv) return NULL;
    
    // Calculate total size needed
    size_t total_size = 1;  // For null terminator
    for (int i = 0; i < tv->line_count; i++) {
        total_size += strlen(tv->lines[i]);
        if (i < tv->line_count - 1) total_size++;  // For newline
    }
    
    char *result = malloc(total_size);
    if (!result) return NULL;
    
    // Concatenate all lines
    char *ptr = result;
    for (int i = 0; i < tv->line_count; i++) {
        strcpy(ptr, tv->lines[i]);
        ptr += strlen(tv->lines[i]);
        if (i < tv->line_count - 1) {
            *ptr++ = '\n';
        }
    }
    *ptr = '\0';
    
    return result;
}

// Insert a character at the cursor position
void textview_insert_char(TextView *tv, char c) {
    if (!tv || tv->read_only) return;
    
    // If there's a selection, delete it first
    if (tv->has_selection) {
        textview_delete_selection(tv);
        tv->has_selection = false;
    }
    
    char *line = tv->lines[tv->cursor_line];
    int len = strlen(line);
    
    // Grow line buffer
    char *new_line = malloc(len + 2);
    if (tv->cursor_col > 0) {
        strncpy(new_line, line, tv->cursor_col);
    }
    new_line[tv->cursor_col] = c;
    if (tv->cursor_col < len) {
        strcpy(new_line + tv->cursor_col + 1, line + tv->cursor_col);
    } else {
        new_line[tv->cursor_col + 1] = '\0';
    }
    
    free(tv->lines[tv->cursor_line]);
    tv->lines[tv->cursor_line] = new_line;
    
    // Record undo for this character insertion
    char char_str[2] = {c, '\0'};
    record_undo(tv, UNDO_INSERT_CHAR, tv->cursor_line, tv->cursor_col, char_str, 1);
    
    tv->cursor_col++;
    tv->modified = true;
    tv->need_width_recalc = true;  // Line content changed
    
    // Invalidate syntax highlighting for this line
    if (tv->syntax_data) {
        invalidate_lines(tv, tv->cursor_line, tv->cursor_line);
    }
    
    if (tv->on_change) tv->on_change(tv);
    
    // Check if scrolling is needed
    int old_scroll_x = tv->scroll_x;
    textview_ensure_cursor_visible(tv);
    textview_update_scrollbar(tv);
    
    // If horizontal scrolling occurred, need full redraw
    if (tv->scroll_x != old_scroll_x) {
        textview_draw(tv);
    } else {
        // Just update the current line (optimized redraw)
        textview_update_cursor(tv);
    }
}

// Insert a new line at cursor
void textview_new_line(TextView *tv) {
    if (!tv || tv->read_only) return;
    
    // If there's a selection, delete it first
    if (tv->has_selection) {
        textview_delete_selection(tv);
        tv->has_selection = false;
    }
    
    // Grow buffer if needed
    if (tv->line_count >= tv->line_capacity) {
        tv->line_capacity += LINE_CHUNK_SIZE;
        tv->lines = realloc(tv->lines, tv->line_capacity * sizeof(char*));
    }
    
    // Split current line at cursor
    char *current = tv->lines[tv->cursor_line];
    int len = strlen(current);
    
    // Create new line with text after cursor
    char *new_line;
    if (tv->cursor_col < len) {
        new_line = strdup(current + tv->cursor_col);
        current[tv->cursor_col] = '\0';  // Truncate current line
    } else {
        new_line = strdup("");
    }
    
    // Shift lines down
    for (int i = tv->line_count; i > tv->cursor_line + 1; i--) {
        tv->lines[i] = tv->lines[i - 1];
    }
    
    // Insert new line
    tv->lines[tv->cursor_line + 1] = new_line;
    tv->line_count++;
    
    // Record undo for newline
    record_undo(tv, UNDO_NEWLINE, tv->cursor_line, tv->cursor_col, "\n", 1);
    
    // Move cursor to start of new line
    tv->cursor_line++;
    tv->cursor_col = 0;
    tv->modified = true;
    
    // Invalidate syntax highlighting for affected lines
    if (tv->syntax_data) {
        invalidate_lines(tv, tv->cursor_line - 1, tv->line_count - 1);
    }
    
    if (tv->on_change) tv->on_change(tv);
    textview_ensure_cursor_visible(tv);
    textview_update_scrollbar(tv);
    textview_draw(tv);
}

// Backspace at cursor
void textview_backspace(TextView *tv) {
    if (!tv || tv->read_only) return;
    
    // If there's a selection, delete it instead of doing backspace
    if (tv->has_selection) {
        textview_delete_selection(tv);
        tv->has_selection = false;
        tv->modified = true;
        if (tv->on_change) tv->on_change(tv);
        textview_ensure_cursor_visible(tv);
        textview_update_scrollbar(tv);
        textview_draw(tv);
        return;
    }
    
    if (tv->cursor_col > 0) {
        // Delete character before cursor
        char *line = tv->lines[tv->cursor_line];
        int len = strlen(line);
        
        // Save the character being deleted for undo
        char deleted_char[2] = {line[tv->cursor_col - 1], '\0'};
        record_undo(tv, UNDO_DELETE_CHAR, tv->cursor_line, tv->cursor_col - 1, deleted_char, 1);
        
        // Shift characters left
        memmove(line + tv->cursor_col - 1, line + tv->cursor_col, 
                len - tv->cursor_col + 1);
        
        tv->cursor_col--;
    } else if (tv->cursor_line > 0) {
        // Join with previous line
        char *prev_line = tv->lines[tv->cursor_line - 1];
        char *current_line = tv->lines[tv->cursor_line];
        int prev_len = strlen(prev_line);
        
        // Record undo for line join (save current line content)
        record_undo(tv, UNDO_JOIN_LINES, tv->cursor_line - 1, prev_len, current_line, strlen(current_line));
        
        // Concatenate lines
        char *new_line = malloc(prev_len + strlen(current_line) + 1);
        strcpy(new_line, prev_line);
        strcat(new_line, current_line);
        
        free(prev_line);
        free(current_line);
        tv->lines[tv->cursor_line - 1] = new_line;
        
        // Shift lines up
        for (int i = tv->cursor_line; i < tv->line_count - 1; i++) {
            tv->lines[i] = tv->lines[i + 1];
        }
        tv->line_count--;
        
        // Move cursor to join point
        tv->cursor_line--;
        tv->cursor_col = prev_len;
    }
    
    tv->modified = true;
    
    // Invalidate syntax highlighting for affected lines
    if (tv->syntax_data) {
        invalidate_lines(tv, tv->cursor_line, tv->line_count - 1);
    }
    
    if (tv->on_change) tv->on_change(tv);
    textview_ensure_cursor_visible(tv);
    textview_update_scrollbar(tv);
    textview_draw(tv);
}

// Delete key at cursor
void textview_delete_key(TextView *tv) {
    if (!tv || tv->read_only) return;
    
    // If there's a selection, delete it
    if (tv->has_selection) {
        textview_delete_selection(tv);
        tv->has_selection = false;
        tv->modified = true;
        if (tv->on_change) tv->on_change(tv);
        textview_ensure_cursor_visible(tv);
        textview_update_scrollbar(tv);
        textview_draw(tv);
        return;
    }
    
    char *line = tv->lines[tv->cursor_line];
    int len = strlen(line);
    
    if (tv->cursor_col < len) {
        // Delete character at cursor
        memmove(line + tv->cursor_col, line + tv->cursor_col + 1, 
                len - tv->cursor_col);
    } else if (tv->cursor_line < tv->line_count - 1) {
        // Join with next line
        char *next_line = tv->lines[tv->cursor_line + 1];
        
        // Concatenate lines
        char *new_line = malloc(len + strlen(next_line) + 1);
        strcpy(new_line, line);
        strcat(new_line, next_line);
        
        free(line);
        free(next_line);
        tv->lines[tv->cursor_line] = new_line;
        
        // Shift lines up
        for (int i = tv->cursor_line + 1; i < tv->line_count - 1; i++) {
            tv->lines[i] = tv->lines[i + 1];
        }
        tv->line_count--;
    }
    
    tv->modified = true;
    
    // Invalidate syntax highlighting for affected lines
    if (tv->syntax_data) {
        invalidate_lines(tv, tv->cursor_line, tv->line_count - 1);
    }
    
    if (tv->on_change) tv->on_change(tv);
    textview_ensure_cursor_visible(tv);
    textview_update_scrollbar(tv);
    textview_draw(tv);
}

// Basic cursor movement functions
void textview_move_cursor_left(TextView *tv) {
    if (!tv) return;
    
    int old_scroll_x = tv->scroll_x;
    int old_scroll_y = tv->scroll_y;
    
    if (tv->cursor_col > 0) {
        tv->cursor_col--;
    } else if (tv->cursor_line > 0) {
        tv->cursor_line--;
        tv->cursor_col = strlen(tv->lines[tv->cursor_line]);
    }
    
    if (tv->on_cursor_move) tv->on_cursor_move(tv);
    textview_ensure_cursor_visible(tv);
    
    // If scrolling occurred, need full redraw
    if (tv->scroll_x != old_scroll_x || tv->scroll_y != old_scroll_y) {
        textview_draw(tv);
    } else {
        textview_update_cursor(tv);
    }
}

void textview_move_cursor_right(TextView *tv) {
    if (!tv) return;
    
    int old_scroll_x = tv->scroll_x;
    int old_scroll_y = tv->scroll_y;
    
    int line_len = strlen(tv->lines[tv->cursor_line]);
    if (tv->cursor_col < line_len) {
        tv->cursor_col++;
    } else if (tv->cursor_line < tv->line_count - 1) {
        tv->cursor_line++;
        tv->cursor_col = 0;
    }
    
    if (tv->on_cursor_move) tv->on_cursor_move(tv);
    textview_ensure_cursor_visible(tv);
    
    // If scrolling occurred, need full redraw
    if (tv->scroll_x != old_scroll_x || tv->scroll_y != old_scroll_y) {
        textview_draw(tv);
    } else {
        textview_update_cursor(tv);
    }
}

void textview_move_cursor_up(TextView *tv) {
    if (!tv || tv->cursor_line == 0) return;
    
    int old_scroll_y = tv->scroll_y;
    tv->cursor_line--;
    int line_len = strlen(tv->lines[tv->cursor_line]);
    if (tv->cursor_col > line_len) {
        tv->cursor_col = line_len;
    }
    
    if (tv->on_cursor_move) tv->on_cursor_move(tv);
    textview_ensure_cursor_visible(tv);
    
    // If scrolling occurred, need full redraw
    if (tv->scroll_y != old_scroll_y) {
        textview_draw(tv);
    } else {
        textview_update_cursor(tv);
    }
}

void textview_move_cursor_down(TextView *tv) {
    if (!tv || tv->cursor_line >= tv->line_count - 1) return;
    
    int old_scroll_y = tv->scroll_y;
    tv->cursor_line++;
    int line_len = strlen(tv->lines[tv->cursor_line]);
    if (tv->cursor_col > line_len) {
        tv->cursor_col = line_len;
    }
    
    if (tv->on_cursor_move) tv->on_cursor_move(tv);
    textview_ensure_cursor_visible(tv);
    
    // If scrolling occurred, need full redraw
    if (tv->scroll_y != old_scroll_y) {
        textview_draw(tv);
    } else {
        textview_update_cursor(tv);
    }
}

void textview_move_cursor_home(TextView *tv) {
    if (!tv) return;
    tv->cursor_col = 0;
    if (tv->on_cursor_move) tv->on_cursor_move(tv);
    textview_ensure_cursor_visible(tv);
    textview_update_cursor(tv);
}

void textview_move_cursor_end(TextView *tv) {
    if (!tv) return;
    tv->cursor_col = strlen(tv->lines[tv->cursor_line]);
    if (tv->on_cursor_move) tv->on_cursor_move(tv);
    textview_ensure_cursor_visible(tv);
    textview_update_cursor(tv);
}

// Ensure cursor is visible in viewport
void textview_ensure_cursor_visible(TextView *tv) {
    if (!tv) return;
    
    // Vertical scrolling
    if (tv->cursor_line < tv->scroll_y) {
        tv->scroll_y = tv->cursor_line;
    } else if (tv->cursor_line >= tv->scroll_y + tv->visible_lines) {
        tv->scroll_y = tv->cursor_line - tv->visible_lines + 1;
    }
    
    // Horizontal scrolling
    int x_start = tv->line_numbers ? tv->line_number_width : 8;
    int cursor_x = x_start;
    
    // Calculate cursor x position
    if (tv->cursor_line < tv->line_count && tv->lines[tv->cursor_line]) {
        const char *line = tv->lines[tv->cursor_line];
        if (tv->cursor_col > 0) {
            int chars_to_measure = (tv->cursor_col > strlen(line)) ? strlen(line) : tv->cursor_col;
            if (chars_to_measure > 0) {
                XGlyphInfo extents;
                XftTextExtentsUtf8(tv->display, tv->font, (FcChar8*)line,
                               chars_to_measure, &extents);
                cursor_x += extents.xOff;
            }
        }
    }
    
    // Calculate viewport width
    int viewport_width = tv->width;
    if (tv->scrollbar_visible) viewport_width -= VERT_SCROLLBAR_WIDTH;
    
    // Adjust horizontal scroll to keep cursor visible
    if (cursor_x - tv->scroll_x < 20) {  // Too far left
        tv->scroll_x = cursor_x - 20;
        if (tv->scroll_x < 0) tv->scroll_x = 0;
    } else if (cursor_x - tv->scroll_x > viewport_width - 20) {  // Too far right
        tv->scroll_x = cursor_x - viewport_width + 20;
        int max_scroll = tv->max_line_width - viewport_width;
        if (max_scroll < 0) max_scroll = 0;
        if (tv->scroll_x > max_scroll) tv->scroll_x = max_scroll;
    }
}

// Draw the TextView
void textview_draw(TextView *tv) {
    if (!tv || !tv->xft_draw || !tv->font) return;
    
    // Fill background manually since we disabled auto-clear
    // Using XFillRectangle is faster than XClearWindow
    XSetForeground(tv->display, DefaultGC(tv->display, DefaultScreen(tv->display)), 0xa2a2a0);
    XFillRectangle(tv->display, tv->window, 
                   DefaultGC(tv->display, DefaultScreen(tv->display)),
                   0, 0, tv->width, tv->height);
    
    // Check if colors are allocated
    if (!tv->colors_allocated) {
        return;  // Can't draw without colors
    }
    
    // Calculate starting position with consistent padding
    int text_padding = 8;  // Consistent padding between border and text
    int x_start = text_padding;  // Default padding from left edge
    
    if (tv->line_numbers) {
        // Calculate width needed for line numbers
        // Determine how many digits we need for the largest line number
        int digits = 1;
        int temp = tv->line_count;
        while (temp >= 10) {
            digits++;
            temp /= 10;
        }
        // Use at least 4 digits for consistency, but more if needed
        if (digits < 4) digits = 4;
        
        // Create format string like "%4d" or "%6d" based on digit count
        char format[16];
        snprintf(format, sizeof(format), "%%%dd", digits);
        
        // Calculate the width using the actual format we'll use for drawing
        char line_num_str[32];
        snprintf(line_num_str, sizeof(line_num_str), format, tv->line_count);
        XGlyphInfo extents;
        XftTextExtentsUtf8(tv->display, tv->font, (FcChar8*)line_num_str, 
                       strlen(line_num_str), &extents);
        tv->line_number_width = extents.xOff + 10;
        x_start = tv->line_number_width + text_padding;
    }
    
    // Calculate maximum content height for scrollbar visibility
    int content_height = tv->line_count * tv->line_height;
    int viewable_height = tv->height;
    if (tv->h_scrollbar_visible) viewable_height -= HORI_SCROLLBAR_HEIGHT;
    
    // Calculate maximum line width for horizontal scrollbar
    // Only recalculate if needed (text changed)
    if (tv->need_width_recalc) {
        tv->max_line_width = 0;
        for (int i = 0; i < tv->line_count; i++) {
            if (tv->lines[i] && *tv->lines[i]) {
                XGlyphInfo extents;
                XftTextExtentsUtf8(tv->display, tv->font, (FcChar8*)tv->lines[i],
                              strlen(tv->lines[i]), &extents);
                int line_width = x_start + extents.xOff + 10;  // Include margins
                tv->line_widths[i] = line_width;  // Cache the width
                if (line_width > tv->max_line_width) {
                    tv->max_line_width = line_width;
                }
            } else {
                tv->line_widths[i] = x_start + 10;  // Empty line width
            }
        }
        tv->need_width_recalc = false;  // Clear the flag
    }
    
    // Determine if we need scrollbars
    int viewable_width = tv->width;
    if (tv->scrollbar_visible) viewable_width -= VERT_SCROLLBAR_WIDTH;
    
    // Update scrollbar visibility based on content
    bool need_v_scrollbar = (content_height > viewable_height);
    bool need_h_scrollbar = (tv->max_line_width > viewable_width);
    
    // If adding one scrollbar reduces space enough to need the other, show both
    if (need_v_scrollbar && !need_h_scrollbar) {
        // Check if vertical scrollbar causes horizontal overflow
        if (tv->max_line_width > (tv->width - VERT_SCROLLBAR_WIDTH)) {
            need_h_scrollbar = true;
        }
    }
    if (need_h_scrollbar && !need_v_scrollbar) {
        // Check if horizontal scrollbar causes vertical overflow
        if (content_height > (tv->height - HORI_SCROLLBAR_HEIGHT)) {
            need_v_scrollbar = true;
        }
    }
    
    tv->scrollbar_visible = need_v_scrollbar;
    tv->h_scrollbar_visible = need_h_scrollbar;
    
    // Draw visible lines (start 2 pixels higher to create bottom padding)
    int y = tv->line_height - 2;
    
    // Defensive check for visible_lines
    int lines_to_draw = tv->visible_lines;
    if (lines_to_draw <= 0) {
        fprintf(stderr, "[DEBUG] TextView: visible_lines is %d, forcing to viewport calculation\n", lines_to_draw);
        lines_to_draw = tv->height / 16;  // Use default line height
        if (lines_to_draw <= 0) lines_to_draw = 1;
    }
    
    // First pass: Draw all line numbers without clipping
    if (tv->line_numbers) {
        // Calculate format for line numbers (same as in width calculation)
        int digits = 1;
        int temp = tv->line_count;
        while (temp >= 10) {
            digits++;
            temp /= 10;
        }
        if (digits < 4) digits = 4;
        
        char format[16];
        snprintf(format, sizeof(format), "%%%dd", digits);
        
        int line_y = tv->line_height - 2;
        for (int i = tv->scroll_y; 
             i < tv->line_count && i < tv->scroll_y + lines_to_draw; 
             i++) {
            char line_num[32];
            snprintf(line_num, sizeof(line_num), format, i + 1);
            XftDrawStringUtf8(tv->xft_draw, &tv->xft_line_num_color, tv->font,
                          5, line_y - 2, (FcChar8*)line_num, strlen(line_num));
            line_y += tv->line_height;
        }
    }
    
    // Set up clipping for main text area (after drawing all line numbers)
    XRectangle clip_rect;
    clip_rect.x = tv->line_numbers ? tv->line_number_width : 0;
    clip_rect.y = 0;
    clip_rect.width = tv->width - clip_rect.x;
    clip_rect.height = tv->height;
    if (tv->scrollbar_visible) clip_rect.width -= VERT_SCROLLBAR_WIDTH;
    if (tv->h_scrollbar_visible) clip_rect.height -= HORI_SCROLLBAR_HEIGHT;
    
    // Apply clipping to prevent text from drawing into scrollbars
    XftDrawSetClipRectangles(tv->xft_draw, 0, 0, &clip_rect, 1);
    
    // Update syntax highlighting for visible lines before drawing
    if (tv->syntax_data) {
        update_visible_highlighting(tv);
    }
    
    // Second pass: Draw text content with clipping
    for (int i = tv->scroll_y; 
         i < tv->line_count && i < tv->scroll_y + lines_to_draw; 
         i++) {
        
        // Check if this line has selection
        const char *line = tv->lines[i];
        int line_len = line ? strlen(line) : 0;
        
        if (tv->has_selection) {
            // Determine selection range for this line
            int sel_start_line = tv->sel_start_line;
            int sel_start_col = tv->sel_start_col;
            int sel_end_line = tv->sel_end_line;
            int sel_end_col = tv->sel_end_col;
            
            // Normalize selection (start before end)
            if (sel_end_line < sel_start_line || 
                (sel_end_line == sel_start_line && sel_end_col < sel_start_col)) {
                int tmp = sel_start_line;
                sel_start_line = sel_end_line;
                sel_end_line = tmp;
                tmp = sel_start_col;
                sel_start_col = sel_end_col;
                sel_end_col = tmp;
            }
            
            // Check if current line is in selection
            if (i >= sel_start_line && i <= sel_end_line) {
                int draw_start = 0;
                int draw_end = line_len;
                
                if (i == sel_start_line) draw_start = sel_start_col;
                if (i == sel_end_line) draw_end = sel_end_col;
                
                if (draw_start < line_len && draw_end > 0 && draw_start < draw_end) {
                    // Draw selection background using XRender
                    Picture dest = XRenderCreatePicture(tv->display, tv->window,
                        XRenderFindVisualFormat(tv->display, DefaultVisual(tv->display, DefaultScreen(tv->display))),
                        0, NULL);
                    
                    // Calculate selection rectangle position with horizontal scroll
                    int sel_x = x_start - tv->scroll_x;
                    if (draw_start > 0) {
                        XGlyphInfo extents;
                        XftTextExtentsUtf8(tv->display, tv->font, (FcChar8*)line, draw_start, &extents);
                        sel_x += extents.xOff;
                    }
                    
                    int sel_width = 0;
                    if (draw_end > draw_start) {
                        XGlyphInfo extents;
                        XftTextExtentsUtf8(tv->display, tv->font, (FcChar8*)&line[draw_start], 
                                      draw_end - draw_start, &extents);
                        sel_width = extents.xOff;
                    }
                    
                    // Clip selection to text area (don't draw into line numbers or scrollbar)
                    int clip_left = tv->line_numbers ? tv->line_number_width : 0;
                    int clip_right = tv->width;
                    if (tv->scrollbar_visible) clip_right -= VERT_SCROLLBAR_WIDTH;
                    
                    // Adjust selection drawing to stay within bounds
                    if (sel_x < clip_left) {
                        sel_width -= (clip_left - sel_x);
                        sel_x = clip_left;
                    }
                    if (sel_x + sel_width > clip_right) {
                        sel_width = clip_right - sel_x;
                    }
                    
                    // Only draw if there's something visible after clipping
                    if (sel_width > 0 && sel_x < clip_right) {
                        // Convert stored color to XRenderColor
                        XRenderColor sel_bg = {
                            ((tv->sel_bg_color >> 16) & 0xFF) * 0x101,  // Red
                            ((tv->sel_bg_color >> 8) & 0xFF) * 0x101,   // Green
                            (tv->sel_bg_color & 0xFF) * 0x101,          // Blue
                            0xFFFF                                       // Alpha
                        };
                        XRenderFillRectangle(tv->display, PictOpOver, dest, &sel_bg,
                                           sel_x, y - tv->line_height + 2,
                                           sel_width, tv->line_height);
                    }
                    
                    XRenderFreePicture(tv->display, dest);
                }
            }
        }
        
        // Draw the text line with horizontal scrolling
        if (line && *line) {
            // Apply horizontal scroll offset
            int text_x = x_start - tv->scroll_x;
            
            // Use syntax-aware drawing if available
            if (tv->syntax_data) {
                draw_line_with_colors(tv, i, text_x, y - 2);
            } else {
                XftDrawStringUtf8(tv->xft_draw, &tv->xft_fg_color, tv->font,
                              text_x, y - 2, (FcChar8*)line, strlen(line));
            }
        }
        
        // Draw cursor if on this line and focused
        if (tv->has_focus && i == tv->cursor_line) {
            int cursor_x = x_start - tv->scroll_x;  // Apply horizontal scroll
            
            // Calculate cursor position
            if (tv->cursor_col > 0) {
                int chars_to_measure = (tv->cursor_col > strlen(line)) ? strlen(line) : tv->cursor_col;
                if (chars_to_measure > 0) {
                    XGlyphInfo extents;
                    XftTextExtentsUtf8(tv->display, tv->font, (FcChar8*)line,
                                   chars_to_measure, &extents);
                    cursor_x += extents.xOff;
                }
            }
            
            // Only draw cursor if it's visible in the viewport
            int viewport_width = tv->width;
            if (tv->scrollbar_visible) viewport_width -= VERT_SCROLLBAR_WIDTH;
            
            // Clip cursor to text area bounds
            int clip_left = tv->line_numbers ? tv->line_number_width : 0;
            int clip_right = tv->width;
            if (tv->scrollbar_visible) clip_right -= VERT_SCROLLBAR_WIDTH;
            
            if (cursor_x >= clip_left && cursor_x < clip_right) {
                // Draw blue rectangle cursor like InputField
                // Get size of a space character for cursor width
                XGlyphInfo space_info;
                XftTextExtentsUtf8(tv->display, tv->font, (FcChar8*)" ", 1, &space_info);
                int cursor_width = space_info.xOff > 0 ? space_info.xOff : 8;
                
                // Clip cursor width if it extends beyond bounds
                if (cursor_x + cursor_width > clip_right) {
                    cursor_width = clip_right - cursor_x;
                }
                
                // Only draw if cursor is at least partially visible
                if (cursor_width > 0) {
                    // Use XRender to draw blue rectangle
                    Picture dest = XRenderCreatePicture(tv->display, tv->window,
                        XRenderFindVisualFormat(tv->display, DefaultVisual(tv->display, DefaultScreen(tv->display))),
                        0, NULL);
                    
                    // Convert stored cursor color to XRenderColor
                    XRenderColor cursor_color = {
                        ((tv->cursor_color >> 16) & 0xFF) * 0x101,  // Red
                        ((tv->cursor_color >> 8) & 0xFF) * 0x101,   // Green
                        (tv->cursor_color & 0xFF) * 0x101,          // Blue
                        0xFFFF                                       // Alpha
                    };
                    XRenderFillRectangle(tv->display, PictOpSrc, dest, &cursor_color,
                                       cursor_x, y - tv->line_height + 2, 
                                       cursor_width, tv->line_height);
                    
                    XRenderFreePicture(tv->display, dest);
                }
                
                // If cursor is on a character, redraw that character in white
                if (tv->cursor_col < strlen(line)) {
                    // Handle UTF-8 multi-byte characters
                    unsigned char c = (unsigned char)line[tv->cursor_col];
                    int char_len = 1;
                    if (c >= 0xC0 && c <= 0xDF) char_len = 2;
                    else if (c >= 0xE0 && c <= 0xEF) char_len = 3;
                    else if (c >= 0xF0 && c <= 0xF7) char_len = 4;
                    
                    // Make sure we don't read past the end of the line
                    if (tv->cursor_col + char_len > strlen(line)) {
                        char_len = strlen(line) - tv->cursor_col;
                    }
                    
                    XftDrawStringUtf8(tv->xft_draw, &tv->xft_sel_color, tv->font,
                                 cursor_x, y - 2, (FcChar8*)&line[tv->cursor_col], char_len);
                }
            }
        }
        
        y += tv->line_height;
    }
    
    // Reset clipping before drawing scrollbars
    XftDrawSetClip(tv->xft_draw, NULL);
    
    // Draw scrollbars if needed
    if (tv->scrollbar_visible || tv->h_scrollbar_visible) {
        // Get XRender Picture for drawing
        Picture dest = XRenderCreatePicture(tv->display, tv->window,
            XRenderFindVisualFormat(tv->display, DefaultVisual(tv->display, DefaultScreen(tv->display))),
            0, NULL);
        
        // Colors for scrollbar - using config.h values
        XRenderColor white = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
        XRenderColor black = {0x0000, 0x0000, 0x0000, 0xFFFF};
        XRenderColor gray = {0xa2a2, 0xa2a2, 0xa0a0, 0xFFFF};
        
        // Draw vertical scrollbar if visible
        if (tv->scrollbar_visible) {
            int sb_x = tv->width - VERT_SCROLLBAR_WIDTH;
            int sb_w = VERT_SCROLLBAR_WIDTH;
            int sb_h = tv->height;  // Always full height - no adjustment
            
            // Draw black separator line on the left of scrollbar (full height)
            XRenderFillRectangle(tv->display, PictOpSrc, dest, &black, 
                               sb_x - 1, 0, 1, sb_h);
            
            // Calculate positions for the three elements
            int track_y = 0;
            int track_h = sb_h - (VERT_ARROW_HEIGHT * 2);
            int arrow_up_y = sb_h - (VERT_ARROW_HEIGHT * 2);
            int arrow_down_y = sb_h - VERT_ARROW_HEIGHT;
            
            // 1. Draw scrollbar track
            XRenderFillRectangle(tv->display, PictOpSrc, dest, &gray, sb_x, track_y, sb_w, sb_h);
            
            // Track borders
            XRenderFillRectangle(tv->display, PictOpSrc, dest, &white, sb_x, track_y, 1, track_h);
            //XRenderFillRectangle(tv->display, PictOpSrc, dest, &white, sb_x, track_y, sb_w, 1);
            XRenderFillRectangle(tv->display, PictOpSrc, dest, &black, sb_x + sb_w - 1, track_y, 1, track_h);
            XRenderFillRectangle(tv->display, PictOpSrc, dest, &black, sb_x, track_y + track_h - 1, sb_w, 1);
            
            // Vertical scrollbar knob (11px wide with 2px padding on each side)
            if (tv->scrollbar_knob_height > 0) {
                // Calculate knob position with padding
                int knob_x = sb_x + SCROLLBAR_KNOB_PADDING + ((sb_w - 2*SCROLLBAR_KNOB_PADDING - VERT_KNOB_WIDTH) / 2);
                int knob_y = tv->scrollbar_knob_y + track_y + SCROLLBAR_KNOB_PADDING;
                
                XRenderFillRectangle(tv->display, PictOpSrc, dest, &black,
                                   knob_x, knob_y,
                                   VERT_KNOB_WIDTH, tv->scrollbar_knob_height - 2*SCROLLBAR_KNOB_PADDING);
            }
            
            // 2. Draw up arrow button
            XRenderFillRectangle(tv->display, PictOpSrc, dest, &gray,
                               sb_x, arrow_up_y, sb_w, VERT_ARROW_HEIGHT);
            // Borders
            XRenderFillRectangle(tv->display, PictOpSrc, dest, &white, sb_x, arrow_up_y, 1, VERT_ARROW_HEIGHT);
            XRenderFillRectangle(tv->display, PictOpSrc, dest, &white, sb_x, arrow_up_y, sb_w, 1);
            XRenderFillRectangle(tv->display, PictOpSrc, dest, &black, sb_x + sb_w - 1, arrow_up_y, 1, VERT_ARROW_HEIGHT);
            XRenderFillRectangle(tv->display, PictOpSrc, dest, &black, sb_x, arrow_up_y + VERT_ARROW_HEIGHT - 1, sb_w, 1);
            // Up arrow
            int arrow_offset = (VERT_ARROW_HEIGHT - 5) / 2;
            for (int i = 0; i < 5; i++) {
                XRenderFillRectangle(tv->display, PictOpSrc, dest, &black,
                                   sb_x + sb_w/2 - i, arrow_up_y + arrow_offset + i, i*2+1, 1);
            }
            
            // 3. Draw down arrow button
            XRenderFillRectangle(tv->display, PictOpSrc, dest, &gray,
                               sb_x, arrow_down_y, sb_w, VERT_ARROW_HEIGHT);
            // Borders
            XRenderFillRectangle(tv->display, PictOpSrc, dest, &white, sb_x, arrow_down_y, 1, VERT_ARROW_HEIGHT);
            XRenderFillRectangle(tv->display, PictOpSrc, dest, &white, sb_x, arrow_down_y, sb_w, 1);
            XRenderFillRectangle(tv->display, PictOpSrc, dest, &black, sb_x + sb_w - 1, arrow_down_y, 1, VERT_ARROW_HEIGHT);
            XRenderFillRectangle(tv->display, PictOpSrc, dest, &black, sb_x, arrow_down_y + VERT_ARROW_HEIGHT - 1, sb_w, 1);
            // Down arrow
            for (int i = 0; i < 5; i++) {
                XRenderFillRectangle(tv->display, PictOpSrc, dest, &black,
                                   sb_x + sb_w/2 - i, arrow_down_y + arrow_offset + (4-i), i*2+1, 1);
            }
        }
        
        // Draw horizontal scrollbar if visible
        if (tv->h_scrollbar_visible) {
            int sb_y = tv->height - HORI_SCROLLBAR_HEIGHT;
            int sb_h = HORI_SCROLLBAR_HEIGHT;
            int sb_w = tv->width;
            if (tv->scrollbar_visible) sb_w -= VERT_SCROLLBAR_WIDTH;  // Stop where vertical scrollbar begins
            
            // Draw black separator line on top of horizontal scrollbar
            XRenderFillRectangle(tv->display, PictOpSrc, dest, &black, 
                               0, sb_y - 1, sb_w, 1);
            
            // Calculate positions for the three elements
            int track_x = 0;
            int track_w = sb_w - (HORI_ARROW_WIDTH * 2);
            int arrow_left_x = sb_w - (HORI_ARROW_WIDTH * 2);
            int arrow_right_x = sb_w - HORI_ARROW_WIDTH;
            
            // 1. Draw scrollbar track
            XRenderFillRectangle(tv->display, PictOpSrc, dest, &gray, track_x, sb_y, sb_w, sb_h);
            
            // Track borders
            XRenderFillRectangle(tv->display, PictOpSrc, dest, &white, track_x, sb_y, track_w, 1);
            XRenderFillRectangle(tv->display, PictOpSrc, dest, &white, track_x, sb_y, 1, sb_h);
            XRenderFillRectangle(tv->display, PictOpSrc, dest, &black, track_x + track_w - 1, sb_y, 1, sb_h);
            XRenderFillRectangle(tv->display, PictOpSrc, dest, &black, track_x, sb_y + sb_h - 1, track_w, 1);
            
            // Horizontal scrollbar knob (11px high with 2px padding on each side)
            if (tv->h_scrollbar_knob_width > 0) {
                // Calculate knob position with padding
                int knob_x = tv->h_scrollbar_knob_x + track_x + SCROLLBAR_KNOB_PADDING;
                int knob_y = sb_y + SCROLLBAR_KNOB_PADDING + ((sb_h - 2*SCROLLBAR_KNOB_PADDING - HORI_KNOB_HEIGHT) / 2);
                
                XRenderFillRectangle(tv->display, PictOpSrc, dest, &black,
                                   knob_x, knob_y,
                                   tv->h_scrollbar_knob_width - 2*SCROLLBAR_KNOB_PADDING, HORI_KNOB_HEIGHT);
            }
            
            // 2. Draw left arrow button
            XRenderFillRectangle(tv->display, PictOpSrc, dest, &gray,
                               arrow_left_x, sb_y, HORI_ARROW_WIDTH, sb_h);
            // Borders
            XRenderFillRectangle(tv->display, PictOpSrc, dest, &white, arrow_left_x, sb_y, HORI_ARROW_WIDTH, 1);
            XRenderFillRectangle(tv->display, PictOpSrc, dest, &white, arrow_left_x, sb_y, 1, sb_h);
            XRenderFillRectangle(tv->display, PictOpSrc, dest, &black, arrow_left_x + HORI_ARROW_WIDTH - 1, sb_y, 1, sb_h);
            XRenderFillRectangle(tv->display, PictOpSrc, dest, &black, arrow_left_x, sb_y + sb_h - 1, HORI_ARROW_WIDTH, 1);
            // Left arrow
            int arrow_offset = (HORI_ARROW_WIDTH - 5) / 2;
            for (int i = 0; i < 5; i++) {
                XRenderFillRectangle(tv->display, PictOpSrc, dest, &black,
                                   arrow_left_x + arrow_offset + i, sb_y + sb_h/2 - i, 1, i*2+1);
            }
            
            // 3. Draw right arrow button
            XRenderFillRectangle(tv->display, PictOpSrc, dest, &gray,
                               arrow_right_x, sb_y, HORI_ARROW_WIDTH, sb_h);
            // Borders
            XRenderFillRectangle(tv->display, PictOpSrc, dest, &white, arrow_right_x, sb_y, HORI_ARROW_WIDTH, 1);
            XRenderFillRectangle(tv->display, PictOpSrc, dest, &white, arrow_right_x, sb_y, 1, sb_h);
            XRenderFillRectangle(tv->display, PictOpSrc, dest, &black, arrow_right_x + HORI_ARROW_WIDTH - 1, sb_y, 1, sb_h);
            XRenderFillRectangle(tv->display, PictOpSrc, dest, &black, arrow_right_x, sb_y + sb_h - 1, HORI_ARROW_WIDTH, 1);
            // Right arrow
            for (int i = 0; i < 5; i++) {
                XRenderFillRectangle(tv->display, PictOpSrc, dest, &black,
                                   arrow_right_x + arrow_offset + (4-i), sb_y + sb_h/2 - i, 1, i*2+1);
            }
        }
        
        // No corner square - scrollbars meet edge-to-edge
        
        XRenderFreePicture(tv->display, dest);
    }
    
}

// Update only the cursor position (optimized drawing)
void textview_update_cursor(TextView *tv) {
    if (!tv || !tv->xft_draw || !tv->font || !tv->colors_allocated) return;
    
    // Calculate line height for drawing
    int line_height = tv->line_height;
    int x_start = tv->line_numbers ? (tv->line_number_width + 8) : 8;
    
    // Calculate viewport width (excluding scrollbar AND separator line)
    int viewport_width = tv->width;
    if (tv->scrollbar_visible) {
        viewport_width -= (VERT_SCROLLBAR_WIDTH + 1);  // -1 for the black separator line
    }
    
    // Helper function to draw a single line
    auto void draw_line_at(int line_idx, bool with_cursor);
    void draw_line_at(int line_idx, bool with_cursor) {
        if (line_idx < 0 || line_idx >= tv->line_count) return;
        if (line_idx < tv->scroll_y || line_idx >= tv->scroll_y + tv->visible_lines) return;
        
        int y = (line_idx - tv->scroll_y + 1) * line_height - 2;  // Match the -2 offset from textview_draw
        const char *line = tv->lines[line_idx];
        
        // Clear just this line's area (cursor is drawn from y - line_height + 2 to y + 2)
        // Don't clear the line number area - start after it
        int clear_x = tv->line_numbers ? tv->line_number_width : 0;
        // Fill with gray background (can't use XClearArea since we disabled auto-background)
        XSetForeground(tv->display, DefaultGC(tv->display, DefaultScreen(tv->display)), 0xa2a2a0);
        XFillRectangle(tv->display, tv->window,
                      DefaultGC(tv->display, DefaultScreen(tv->display)),
                      clear_x, y - line_height + 2,  // Start where cursor starts
                      viewport_width - clear_x,
                      line_height);  // Clear exactly line_height pixels (cursor height)
        
        // Set clipping to prevent drawing into scrollbar area and line numbers
        XRectangle clip_rect;
        clip_rect.x = tv->line_numbers ? tv->line_number_width : 0;  // Protect line number area
        clip_rect.y = y - line_height + 2;  // Match the clear area
        clip_rect.width = viewport_width - clip_rect.x;  // Adjust width based on x start
        clip_rect.height = line_height;  // Match the clear area
        Region clip_region = XCreateRegion();
        XUnionRectWithRegion(&clip_rect, clip_region, clip_region);
        XftDrawSetClipRectangles(tv->xft_draw, 0, 0, &clip_rect, 1);
        
        // Redraw line number if enabled
        if (tv->line_numbers) {
            // Calculate format for line numbers (same as in textview_draw)
            int digits = 1;
            int temp = tv->line_count;
            while (temp >= 10) {
                digits++;
                temp /= 10;
            }
            if (digits < 4) digits = 4;
            
            char format[16];
            snprintf(format, sizeof(format), "%%%dd", digits);
            
            char line_num[32];
            snprintf(line_num, sizeof(line_num), format, line_idx + 1);
            XftDrawStringUtf8(tv->xft_draw, &tv->xft_line_num_color, tv->font,
                            5, y - 2, (FcChar8*)line_num, strlen(line_num));
        }
        
        // Draw the text with horizontal scroll offset
        if (line && *line) {
            int text_x = x_start - tv->scroll_x;
            
            // Use syntax-aware drawing if available
            if (tv->syntax_data) {
                // Ensure this line is highlighted before drawing
                TextViewSyntax *syn = (TextViewSyntax*)tv->syntax_data;
                if (syn->highlight_func && syn->syntax_context) {
                    highlight_line_cached(tv, line_idx);
                }
                draw_line_with_colors(tv, line_idx, text_x, y - 2);
            } else {
                XftDrawStringUtf8(tv->xft_draw, &tv->xft_fg_color, tv->font,
                                text_x, y - 2, (FcChar8*)line, strlen(line));
            }
        }
        
        // Draw cursor if this is the cursor line and requested
        if (with_cursor && tv->has_focus && line_idx == tv->cursor_line) {
            // Calculate cursor X position WITHOUT scroll for actual position
            int cursor_actual_x = x_start;
            
            // Calculate cursor position based on column
            if (tv->cursor_col > 0 && line) {
                int chars_to_measure = (tv->cursor_col > strlen(line)) ? strlen(line) : tv->cursor_col;
                if (chars_to_measure > 0) {
                    XGlyphInfo extents;
                    XftTextExtentsUtf8(tv->display, tv->font, (FcChar8*)line,
                                     chars_to_measure, &extents);
                    cursor_actual_x += extents.xOff;
                }
            }
            
            // Now apply scroll offset for drawing position
            int cursor_draw_x = cursor_actual_x - tv->scroll_x;
            
            // Only draw if cursor is visible in viewport
            if (cursor_draw_x >= 0 && cursor_draw_x < viewport_width) {
                XGlyphInfo space_info;
                XftTextExtentsUtf8(tv->display, tv->font, (FcChar8*)" ", 1, &space_info);
                int cursor_width = space_info.xOff > 0 ? space_info.xOff : 8;
                
                // Ensure cursor doesn't extend into scrollbar
                if (cursor_draw_x + cursor_width > viewport_width) {
                    cursor_width = viewport_width - cursor_draw_x;
                }
                
                Picture dest = XRenderCreatePicture(tv->display, tv->window,
                    XRenderFindVisualFormat(tv->display, DefaultVisual(tv->display, DefaultScreen(tv->display))),
                    0, NULL);
                
                XRenderColor cursor_color = {
                    ((tv->cursor_color >> 16) & 0xFF) * 0x101,
                    ((tv->cursor_color >> 8) & 0xFF) * 0x101,
                    (tv->cursor_color & 0xFF) * 0x101,
                    0xFFFF
                };
                XRenderFillRectangle(tv->display, PictOpSrc, dest, &cursor_color,
                                   cursor_draw_x, y - line_height + 2,
                                   cursor_width, line_height);  // Full line height
                
                XRenderFreePicture(tv->display, dest);
                
                // Redraw character under cursor in white
                if (tv->cursor_col < strlen(line)) {
                    unsigned char c = (unsigned char)line[tv->cursor_col];
                    int char_len = 1;
                    if (c >= 0xC0 && c <= 0xDF) char_len = 2;
                    else if (c >= 0xE0 && c <= 0xEF) char_len = 3;
                    else if (c >= 0xF0 && c <= 0xF7) char_len = 4;
                    
                    if (tv->cursor_col + char_len > strlen(line)) {
                        char_len = strlen(line) - tv->cursor_col;
                    }
                    
                    XftDrawStringUtf8(tv->xft_draw, &tv->xft_sel_color, tv->font,
                                    cursor_draw_x, y - 2, (FcChar8*)&line[tv->cursor_col], char_len);
                }
            }
        }
        
        // Reset clipping
        XftDrawSetClip(tv->xft_draw, NULL);
        XDestroyRegion(clip_region);
    }
    
    // If cursor moved to a different line, redraw both old and new lines
    if (tv->prev_cursor_line != tv->cursor_line) {
        draw_line_at(tv->prev_cursor_line, false);  // Clear old cursor
        draw_line_at(tv->cursor_line, true);        // Draw new cursor
    } else {
        // Same line - always redraw to ensure cursor is visible
        // This handles both column changes and clicking on same position
        draw_line_at(tv->cursor_line, true);
    }
    
    // Update previous position
    tv->prev_cursor_line = tv->cursor_line;
    tv->prev_cursor_col = tv->cursor_col;
}

// Handle key press events
bool textview_handle_key_press(TextView *tv, XKeyEvent *event) {
    if (!tv) return false;
    
    KeySym keysym;
    char buffer[32];
    int len = XLookupString(event, buffer, sizeof(buffer), &keysym, NULL);
    
    // Check for Super key shortcuts
    if (event->state & Mod4Mask) {
        switch (keysym) {
            case XK_c:  // Copy
            case XK_C:
                textview_copy(tv);
                return true;
                
            case XK_x:  // Cut
            case XK_X:
                textview_cut(tv);
                return true;
                
            case XK_v:  // Paste
            case XK_V:
                textview_paste(tv);
                return true;
                
            case XK_a:  // Select All
            case XK_A:
                textview_select_all(tv);
                return true;
                
            case XK_z:  // Undo
            case XK_Z:
                textview_undo(tv);
                return true;
                
            case XK_r:  // Redo
            case XK_R:
                textview_redo(tv);
                return true;
        }
    }
    
    // Handle Page Up/Down
    if (keysym == XK_Page_Up) {
        textview_move_cursor_page_up(tv);
        return true;
    } else if (keysym == XK_Page_Down) {
        textview_move_cursor_page_down(tv);
        return true;
    }
    
    // Handle special keys
    switch (keysym) {
        case XK_Left:
            textview_move_cursor_left(tv);
            return true;
            
        case XK_Right:
            textview_move_cursor_right(tv);
            return true;
            
        case XK_Up:
            textview_move_cursor_up(tv);
            return true;
            
        case XK_Down:
            textview_move_cursor_down(tv);
            return true;
            
        case XK_Home:
            textview_move_cursor_home(tv);
            return true;
            
        case XK_End:
            textview_move_cursor_end(tv);
            return true;
            
        case XK_Return:
        case XK_KP_Enter:
            textview_new_line(tv);
            return true;
            
        case XK_BackSpace:
            textview_backspace(tv);
            return true;
            
        case XK_Delete:
            textview_delete_key(tv);
            return true;
            
        case XK_Tab:
            // Insert spaces for tab
            for (int i = 0; i < TAB_WIDTH; i++) {
                textview_insert_char(tv, ' ');
            }
            return true;
            
        default:
            // Handle printable characters
            if (len > 0 && buffer[0] >= 32 && buffer[0] < 127) {
                textview_insert_char(tv, buffer[0]);
                return true;
            }
            break;
    }
    
    return false;
}

// Handle mouse button press
bool textview_handle_button_press(TextView *tv, XButtonEvent *event) {
    if (!tv) return false;
    
    // Block extra mouse buttons (6-9) - we only handle 1-5
    if (event->button > 5) {
        return true;  // Consume event but do nothing
    }
    
    // Handle mousewheel scrolling (buttons 4 and 5)
    if (event->button == 4) {  // Wheel up
        if (tv->scroll_y > 0) {
            tv->scroll_y -= 3;  // Scroll 3 lines at a time
            if (tv->scroll_y < 0) tv->scroll_y = 0;
            textview_update_scrollbar(tv);
            textview_draw(tv);
        }
        return true;
    } else if (event->button == 5) {  // Wheel down
        int max_scroll = tv->line_count - tv->visible_lines;
        if (max_scroll < 0) max_scroll = 0;
        if (tv->scroll_y < max_scroll) {
            tv->scroll_y += 3;  // Scroll 3 lines at a time
            if (tv->scroll_y > max_scroll) tv->scroll_y = max_scroll;
            textview_update_scrollbar(tv);
            textview_draw(tv);
        }
        return true;
    }
    
    // Only handle left, middle, right buttons for other operations
    if (event->button > 3) {
        return false;  // Not our event
    }
    
    // Check if click is in vertical scrollbar area
    if (tv->scrollbar_visible && event->x >= tv->width - VERT_SCROLLBAR_WIDTH) {
        // Vertical scrollbar uses full height
        int sb_h = tv->height;
        
        // Fix arrow positions to match draw function
        int arrow_up_y = sb_h - (VERT_ARROW_HEIGHT * 2);
        int arrow_down_y = sb_h - VERT_ARROW_HEIGHT;
            
            // Check for arrow clicks
            if (event->y >= arrow_up_y && event->y < arrow_down_y) {
                // Up arrow clicked
                if (tv->scroll_y > 0) {
                    tv->scroll_y--;
                    textview_update_scrollbar(tv);
                    textview_draw(tv);
                }
            } else if (event->y >= arrow_down_y) {
                // Down arrow clicked
                int max_scroll = tv->line_count - tv->visible_lines;
                if (max_scroll < 0) max_scroll = 0;
                if (tv->scroll_y < max_scroll) {
                    tv->scroll_y++;
                    textview_update_scrollbar(tv);
                    textview_draw(tv);
                }
            } else if (event->y < arrow_up_y) {
                // Track or knob clicked - start dragging
                int knob_top = tv->scrollbar_knob_y;
                int knob_bottom = knob_top + tv->scrollbar_knob_height;
                
                if (event->y >= knob_top && event->y < knob_bottom) {
                    // Clicked on knob - start dragging
                    tv->scrollbar_dragging = true;
                    tv->scrollbar_drag_offset = event->y - knob_top;
                } else {
                    // Clicked on track - page up/down
                    if (event->y < knob_top) {
                        // Page up
                        tv->scroll_y -= tv->visible_lines;
                        if (tv->scroll_y < 0) tv->scroll_y = 0;
                    } else {
                        // Page down
                        tv->scroll_y += tv->visible_lines;
                        int max_scroll = tv->line_count - tv->visible_lines;
                        if (max_scroll < 0) max_scroll = 0;
                        if (tv->scroll_y > max_scroll) tv->scroll_y = max_scroll;
                    }
                    textview_update_scrollbar(tv);
                    textview_draw(tv);
                }
            }
        // ALWAYS return true for ANY click in scrollbar area to prevent text selection
        return true;
    }
    
    // Check if click is in horizontal scrollbar area
    if (tv->h_scrollbar_visible && event->y >= tv->height - HORI_SCROLLBAR_HEIGHT) {
        // Adjust width if vertical scrollbar is visible
        int sb_w = tv->width;
        if (tv->scrollbar_visible) sb_w -= VERT_SCROLLBAR_WIDTH;
        
        // Only handle if click is within horizontal scrollbar bounds
        if (event->x < sb_w) {
            // Calculate arrow positions
            int arrow_left_x = sb_w - (HORI_ARROW_WIDTH * 2);
            int arrow_right_x = sb_w - HORI_ARROW_WIDTH;
            
            // Check for arrow clicks
            if (event->x >= arrow_left_x && event->x < arrow_right_x) {
                // Left arrow clicked
                if (tv->scroll_x > 0) {
                    tv->scroll_x -= tv->char_width * 3;  // Scroll by 3 characters
                    if (tv->scroll_x < 0) tv->scroll_x = 0;
                    textview_update_scrollbar(tv);
                    textview_draw(tv);
                }
            } else if (event->x >= arrow_right_x) {
                // Right arrow clicked
                int viewable_width = tv->width;
                if (tv->scrollbar_visible) viewable_width -= VERT_SCROLLBAR_WIDTH;
                int max_scroll = tv->max_line_width - viewable_width;
                if (max_scroll < 0) max_scroll = 0;
                if (tv->scroll_x < max_scroll) {
                    tv->scroll_x += tv->char_width * 3;  // Scroll by 3 characters
                    if (tv->scroll_x > max_scroll) tv->scroll_x = max_scroll;
                    textview_update_scrollbar(tv);
                    textview_draw(tv);
                }
            } else if (event->x < arrow_left_x) {
                // Track or knob clicked - start dragging
                int knob_left = tv->h_scrollbar_knob_x;
                int knob_right = knob_left + tv->h_scrollbar_knob_width;
                
                if (event->x >= knob_left && event->x < knob_right) {
                    // Clicked on knob - start dragging
                    tv->h_scrollbar_dragging = true;
                    tv->h_scrollbar_drag_offset = event->x - knob_left;
                } else {
                    // Clicked on track - page left/right
                    int viewable_width = tv->width;
                    if (tv->scrollbar_visible) viewable_width -= VERT_SCROLLBAR_WIDTH;
                    
                    if (event->x < knob_left) {
                        // Page left
                        tv->scroll_x -= viewable_width / 2;
                        if (tv->scroll_x < 0) tv->scroll_x = 0;
                    } else {
                        // Page right
                        tv->scroll_x += viewable_width / 2;
                        int max_scroll = tv->max_line_width - viewable_width;
                        if (max_scroll < 0) max_scroll = 0;
                        if (tv->scroll_x > max_scroll) tv->scroll_x = max_scroll;
                    }
                    textview_update_scrollbar(tv);
                    textview_draw(tv);
                }
            }
        }
        // ALWAYS return true for ANY click in horizontal scrollbar area
        return true;
    }
    // Calculate which line was clicked
    int clicked_line = tv->scroll_y + (event->y / tv->line_height);
    if (clicked_line >= tv->line_count) {
        clicked_line = tv->line_count - 1;
    }
    if (clicked_line < 0) clicked_line = 0;
    
    // Calculate column more accurately using font metrics
    int x_start = tv->line_numbers ? (tv->line_number_width + 8) : 8;
    int clicked_col = 0;
    
    // Adjust click X position for horizontal scroll
    int adjusted_x = event->x + tv->scroll_x;
    
    if (adjusted_x > x_start && clicked_line < tv->line_count) {
        // Get the line text
        const char *line = tv->lines[clicked_line];
        int line_len = strlen(line);
        int x_pos = x_start;
        
        // Find which character was clicked
        bool found = false;
        for (int i = 0; i < line_len; i++) {
            XGlyphInfo extents;
            XftTextExtentsUtf8(tv->display, tv->font, (FcChar8*)&line[i], 1, &extents);
            int char_width = extents.xOff;
            
            // If click is anywhere within this character, place cursor before it
            // This allows clicking on a character to position cursor for deletion
            if (adjusted_x < x_pos + char_width) {
                // Click is on this character - place cursor before it
                clicked_col = i;
                found = true;
                break;
            }
            x_pos += char_width;
        }
        
        // If we didn't find a character (click is past end of line)
        if (!found) {
            clicked_col = line_len;
        }
    } else if (adjusted_x <= x_start) {
        // Click before text starts - cursor at beginning
        clicked_col = 0;
    }
    
    // Clear any existing selection on click
    bool had_selection = tv->has_selection;
    tv->has_selection = false;
    
    // Set cursor position
    tv->cursor_line = clicked_line;
    tv->cursor_col = clicked_col;
    
    // Start selection from this point (for drag)
    tv->sel_start_line = clicked_line;
    tv->sel_start_col = clicked_col;
    tv->sel_end_line = clicked_line;
    tv->sel_end_col = clicked_col;
    
    if (tv->on_cursor_move) tv->on_cursor_move(tv);
    
    // If we had a selection, need to redraw to clear it visually
    if (had_selection) {
        textview_draw(tv);
    } else {
        textview_update_cursor(tv);
    }
    
    return true;
}

// Handle focus events
bool textview_handle_focus_in(TextView *tv) {
    if (!tv) return false;
    tv->has_focus = true;
    textview_update_cursor(tv);  // Just show/hide cursor
    return true;
}

bool textview_handle_focus_out(TextView *tv) {
    if (!tv) return false;
    tv->has_focus = false;
    textview_update_cursor(tv);  // Just hide cursor
    return true;
}

// Toggle line numbers
void textview_set_line_numbers(TextView *tv, bool show) {
    if (!tv) return;
    tv->line_numbers = show;
    textview_draw(tv);
}

// Toggle word wrap
void textview_set_word_wrap(TextView *tv, bool wrap) {
    if (!tv) return;
    tv->word_wrap = wrap;
    // TODO: Implement word wrapping logic
    textview_draw(tv);
}

// Handle button release
bool textview_handle_button_release(TextView *tv, XButtonEvent *event) {
    if (!tv) return false;
    bool was_dragging = tv->scrollbar_dragging || tv->h_scrollbar_dragging;
    tv->scrollbar_dragging = false;
    tv->h_scrollbar_dragging = false;
    return was_dragging;
}

// Handle mouse motion (for selection dragging)
bool textview_handle_motion(TextView *tv, XMotionEvent *event) {
    if (!tv) return false;
    
    // Handle vertical scrollbar dragging
    if (tv->scrollbar_dragging) {
        int sb_h = tv->height;  // Always full height
        int track_h = sb_h - (VERT_ARROW_HEIGHT * 2);
        int usable_track = track_h - 2*SCROLLBAR_KNOB_PADDING;
        int rel_y = event->y - tv->scrollbar_drag_offset;
        int available_space = usable_track - tv->scrollbar_knob_height;
        
        if (available_space > 0) {
            // Calculate new scroll position
            int scrollable_lines = tv->line_count - tv->visible_lines;
            tv->scroll_y = rel_y * scrollable_lines / available_space;
            
            if (tv->scroll_y < 0) tv->scroll_y = 0;
            if (tv->scroll_y > scrollable_lines) tv->scroll_y = scrollable_lines;
            
            textview_update_scrollbar(tv);
            textview_draw(tv);
        }
        return true;
    }
    
    // Handle horizontal scrollbar dragging
    if (tv->h_scrollbar_dragging) {
        int sb_w = tv->width;
        if (tv->scrollbar_visible) sb_w -= VERT_SCROLLBAR_WIDTH;
        int track_w = sb_w - (HORI_ARROW_WIDTH * 2);
        int usable_track = track_w - 2*SCROLLBAR_KNOB_PADDING;
        int rel_x = event->x - tv->h_scrollbar_drag_offset;
        int available_space = usable_track - tv->h_scrollbar_knob_width;
        
        if (available_space > 0) {
            // Calculate new scroll position
            int viewable_width = tv->width;
            if (tv->scrollbar_visible) viewable_width -= VERT_SCROLLBAR_WIDTH;
            int max_scroll = tv->max_line_width - viewable_width;
            if (max_scroll > 0) {
                tv->scroll_x = rel_x * max_scroll / available_space;
                
                if (tv->scroll_x < 0) tv->scroll_x = 0;
                if (tv->scroll_x > max_scroll) tv->scroll_x = max_scroll;
                
                textview_update_scrollbar(tv);
                textview_draw(tv);
            }
        }
        return true;
    }
    
    // Only handle text selection if button is pressed (dragging)
    if (!(event->state & Button1Mask)) return false;
    
    // Check if mouse is over scrollbar areas
    bool over_v_scrollbar = tv->scrollbar_visible && event->x >= tv->width - VERT_SCROLLBAR_WIDTH;
    bool over_h_scrollbar = tv->h_scrollbar_visible && event->y >= tv->height - HORI_SCROLLBAR_HEIGHT;
    bool over_scrollbar = over_v_scrollbar || over_h_scrollbar;
    
    // FIRST: Handle auto-scroll for selection (runs even when over scrollbars!)
    // This ensures smooth selection scrolling continues when mouse enters scrollbar area
    int scroll_zone = 30;  // Pixels from edge to trigger scrolling
    bool need_scroll = false;
    
    // Calculate viewport bounds (excluding scrollbars)
    int viewport_width = tv->width;
    int viewport_height = tv->height;
    if (tv->scrollbar_visible) viewport_width -= VERT_SCROLLBAR_WIDTH;
    if (tv->h_scrollbar_visible) viewport_height -= HORI_SCROLLBAR_HEIGHT;
    
    // Vertical scrolling - check against viewport height, not total height
    if (event->y < 0 || event->y < scroll_zone) {
        // Above window or near top - scroll up
        if (tv->scroll_y > 0) {
            // Faster scroll when further from edge or over scrollbar
            int speed = (event->y < 0 || over_scrollbar) ? 3 : 1;
            tv->scroll_y -= speed;
            if (tv->scroll_y < 0) tv->scroll_y = 0;
            need_scroll = true;
        }
    } else if (event->y >= viewport_height || event->y > viewport_height - scroll_zone || over_h_scrollbar) {
        // Near bottom of viewport or over horizontal scrollbar - turbo scroll!
        int max_scroll = tv->line_count - tv->visible_lines;
        if (max_scroll < 0) max_scroll = 0;
        if (tv->scroll_y < max_scroll) {
            // Faster scroll when further from edge or over scrollbar (turbo zone!)
            int speed = (event->y >= viewport_height || over_h_scrollbar) ? 3 : 1;
            tv->scroll_y += speed;
            if (tv->scroll_y > max_scroll) tv->scroll_y = max_scroll;
            need_scroll = true;
        }
    }
    
    // Horizontal scrolling - auto-scroll when selection extends beyond viewport
    if (event->x < 0 || event->x < scroll_zone) {
        // Near left edge - scroll left
        if (tv->scroll_x > 0) {
            int speed = (event->x < 0 || over_scrollbar) ? 20 : 5;  // Faster for horizontal
            tv->scroll_x -= speed;
            if (tv->scroll_x < 0) tv->scroll_x = 0;
            need_scroll = true;
        }
    } else if (event->x >= viewport_width || event->x > viewport_width - scroll_zone || over_v_scrollbar) {
        // Near right edge of viewport or over vertical scrollbar - turbo scroll!
        int max_scroll = tv->max_line_width - viewport_width;
        if (max_scroll > 0 && tv->scroll_x < max_scroll) {
            int speed = (event->x >= viewport_width || over_v_scrollbar) ? 20 : 5;  // Turbo when over scrollbar
            tv->scroll_x += speed;
            if (tv->scroll_x > max_scroll) tv->scroll_x = max_scroll;
            need_scroll = true;
        }
    }
    
    if (need_scroll) {
        textview_update_scrollbar(tv);
    }
    
    // SECOND: Update selection only if NOT over scrollbar areas
    // This prevents accidental selection changes when mouse is over scrollbars
    if (!over_scrollbar) {
        // Calculate which line/col we're over
        int motion_line = tv->scroll_y + (event->y / tv->line_height);
        if (motion_line >= tv->line_count) motion_line = tv->line_count - 1;
        if (motion_line < 0) motion_line = 0;
        
        int x_start = tv->line_numbers ? tv->line_number_width : 8;
        int motion_col = 0;
        
        // Adjust mouse X position for horizontal scroll
        int adjusted_x = event->x + tv->scroll_x;
        
        if (adjusted_x > x_start && motion_line < tv->line_count) {
            const char *line = tv->lines[motion_line];
            int line_len = strlen(line);
            int x_pos = x_start;
            
            for (int i = 0; i < line_len; i++) {
                XGlyphInfo extents;
                XftTextExtentsUtf8(tv->display, tv->font, (FcChar8*)&line[i], 1, &extents);
                int char_width = extents.xOff;
                
                if (adjusted_x < x_pos + char_width / 2) {
                    motion_col = i;
                    break;
                }
                x_pos += char_width;
                motion_col = i + 1;
            }
        }
        
        // Update selection end point
        tv->sel_end_line = motion_line;
        tv->sel_end_col = motion_col;
        
        // Mark as having selection if we've actually moved
        if (tv->sel_start_line != tv->sel_end_line || 
            tv->sel_start_col != tv->sel_end_col) {
            tv->has_selection = true;
        }
        
        // Move cursor to end of selection
        tv->cursor_line = motion_line;
        tv->cursor_col = motion_col;
    }
    
    textview_draw(tv);
    return true;
}

void textview_delete_char(TextView *tv) {
    // TODO: Implement delete at cursor
}

void textview_insert_string(TextView *tv, const char *str) {
    if (!tv || !str) return;
    while (*str) {
        if (*str == '\n') {
            textview_new_line(tv);
        } else {
            textview_insert_char(tv, *str);
        }
        str++;
    }
}

void textview_move_cursor(TextView *tv, int line, int col) {
    if (!tv) return;
    if (line >= 0 && line < tv->line_count) {
        tv->cursor_line = line;
        int line_len = strlen(tv->lines[line]);
        tv->cursor_col = (col < 0) ? 0 : (col > line_len) ? line_len : col;
        if (tv->on_cursor_move) tv->on_cursor_move(tv);
        textview_ensure_cursor_visible(tv);
        textview_draw(tv);
    }
}

void textview_move_cursor_page_up(TextView *tv) {
    if (!tv) return;
    tv->cursor_line -= tv->visible_lines;
    if (tv->cursor_line < 0) tv->cursor_line = 0;
    int line_len = strlen(tv->lines[tv->cursor_line]);
    if (tv->cursor_col > line_len) tv->cursor_col = line_len;
    if (tv->on_cursor_move) tv->on_cursor_move(tv);
    textview_ensure_cursor_visible(tv);
    textview_update_cursor(tv);
}

void textview_move_cursor_page_down(TextView *tv) {
    if (!tv) return;
    tv->cursor_line += tv->visible_lines;
    if (tv->cursor_line >= tv->line_count) tv->cursor_line = tv->line_count - 1;
    int line_len = strlen(tv->lines[tv->cursor_line]);
    if (tv->cursor_col > line_len) tv->cursor_col = line_len;
    if (tv->on_cursor_move) tv->on_cursor_move(tv);
    textview_ensure_cursor_visible(tv);
    textview_update_cursor(tv);
}

void textview_set_selection(TextView *tv, int start_line, int start_col,
                           int end_line, int end_col) {
    if (!tv) return;
    tv->sel_start_line = start_line;
    tv->sel_start_col = start_col;
    tv->sel_end_line = end_line;
    tv->sel_end_col = end_col;
    tv->has_selection = true;
    textview_draw(tv);
}

void textview_clear_selection(TextView *tv) {
    if (!tv) return;
    tv->has_selection = false;
    textview_draw(tv);
}

char* textview_get_selection(TextView *tv) {
    if (!tv || !tv->has_selection) return NULL;
    
    // Normalize selection (start should be before end)
    int start_line = tv->sel_start_line;
    int start_col = tv->sel_start_col;
    int end_line = tv->sel_end_line;
    int end_col = tv->sel_end_col;
    
    
    // Swap if selection is backwards
    if (start_line > end_line || (start_line == end_line && start_col > end_col)) {
        int tmp = start_line;
        start_line = end_line;
        end_line = tmp;
        tmp = start_col;
        start_col = end_col;
        end_col = tmp;
    }
    
    // Validate bounds
    if (start_line < 0 || start_line >= tv->line_count ||
        end_line < 0 || end_line >= tv->line_count) {
        return NULL;
    }
    
    // Calculate total size needed
    int total_size = 0;
    if (start_line == end_line) {
        // Single line selection
        int len = strlen(tv->lines[start_line]);
        if (start_col > len) start_col = len;
        if (end_col > len) end_col = len;
        total_size = end_col - start_col + 1;
    } else {
        // Multi-line selection
        // First line
        int first_len = strlen(tv->lines[start_line]);
        if (start_col > first_len) start_col = first_len;
        total_size += first_len - start_col + 1; // +1 for newline
        
        // Middle lines
        for (int i = start_line + 1; i < end_line; i++) {
            total_size += strlen(tv->lines[i]) + 1; // +1 for newline
        }
        
        // Last line
        int last_len = strlen(tv->lines[end_line]);
        if (end_col > last_len) end_col = last_len;
        total_size += end_col + 1; // +1 for null terminator
    }
    
    // Allocate buffer
    char *selection = malloc(total_size);
    if (!selection) return NULL;
    
    char *p = selection;
    
    if (start_line == end_line) {
        // Single line selection
        int len = end_col - start_col;
        if (len > 0) {
            memcpy(p, tv->lines[start_line] + start_col, len);
            p += len;
        }
    } else {
        // Multi-line selection
        // First line
        strcpy(p, tv->lines[start_line] + start_col);
        p += strlen(tv->lines[start_line] + start_col);
        *p++ = '\n';
        
        // Middle lines
        for (int i = start_line + 1; i < end_line; i++) {
            strcpy(p, tv->lines[i]);
            p += strlen(tv->lines[i]);
            *p++ = '\n';
        }
        
        // Last line
        memcpy(p, tv->lines[end_line], end_col);
        p += end_col;
    }
    
    *p = '\0';
    return selection;
}

void textview_delete_selection(TextView *tv) {
    if (!tv || !tv->has_selection || tv->read_only) return;
    
    // Normalize selection (start should be before end)
    int start_line = tv->sel_start_line;
    int start_col = tv->sel_start_col;
    int end_line = tv->sel_end_line;
    int end_col = tv->sel_end_col;
    
    // Swap if selection is backwards
    if (start_line > end_line || (start_line == end_line && start_col > end_col)) {
        int tmp = start_line;
        start_line = end_line;
        end_line = tmp;
        tmp = start_col;
        start_col = end_col;
        end_col = tmp;
    }
    
    // Validate bounds
    if (start_line < 0 || start_line >= tv->line_count ||
        end_line < 0 || end_line >= tv->line_count) {
        return;
    }
    
    if (start_line == end_line) {
        // Single line deletion
        char *line = tv->lines[start_line];
        int len = strlen(line);
        if (start_col > len) start_col = len;
        if (end_col > len) end_col = len;
        
        // Create new line with deleted portion removed
        int new_len = len - (end_col - start_col);
        char *new_line = malloc(new_len + 1);
        if (new_line) {
            strncpy(new_line, line, start_col);
            strcpy(new_line + start_col, line + end_col);
            free(tv->lines[start_line]);
            tv->lines[start_line] = new_line;
        }
    } else {
        // Multi-line deletion
        // Combine first and last line
        char *first_line = tv->lines[start_line];
        char *last_line = tv->lines[end_line];
        int first_len = strlen(first_line);
        int last_len = strlen(last_line);
        
        if (start_col > first_len) start_col = first_len;
        if (end_col > last_len) end_col = last_len;
        
        int new_len = start_col + (last_len - end_col);
        char *new_line = malloc(new_len + 1);
        if (new_line) {
            strncpy(new_line, first_line, start_col);
            strcpy(new_line + start_col, last_line + end_col);
            free(tv->lines[start_line]);
            tv->lines[start_line] = new_line;
            
            // Delete lines in between
            for (int i = start_line + 1; i <= end_line; i++) {
                free(tv->lines[i]);
            }
            
            // Shift remaining lines up
            int lines_to_delete = end_line - start_line;
            for (int i = end_line + 1; i < tv->line_count; i++) {
                tv->lines[i - lines_to_delete] = tv->lines[i];
            }
            tv->line_count -= lines_to_delete;
        }
    }
    
    // Move cursor to start of deletion
    tv->cursor_line = start_line;
    tv->cursor_col = start_col;
    
    // Clear selection
    tv->has_selection = false;
    tv->modified = true;
    
    // Invalidate syntax highlighting for affected lines
    if (tv->syntax_data) {
        invalidate_lines(tv, start_line, tv->line_count - 1);
    }
    
    // Redraw
    textview_draw(tv);
}


// Set selection colors
void textview_set_selection_colors(TextView *tv, unsigned int bg, unsigned int fg) {
    if (!tv) return;
    tv->sel_bg_color = bg;
    tv->sel_fg_color = fg;
    textview_draw(tv);
}

// Set cursor color
void textview_set_cursor_color(TextView *tv, unsigned int color) {
    if (!tv) return;
    tv->cursor_color = color;
    textview_draw(tv);
}

// Handle window resize
bool textview_handle_configure(TextView *tv, XConfigureEvent *event) {
    if (!tv) return false;
    
    // Update dimensions
    tv->width = event->width;
    tv->height = event->height;
    
    // Recalculate visible lines
    if (tv->line_height > 0) {
        tv->visible_lines = tv->height / tv->line_height;
        if (tv->visible_lines <= 0) {
            fprintf(stderr, "[DEBUG] TextView: Calculated visible_lines as %d, forcing to 1\n", tv->visible_lines);
            tv->visible_lines = 1;
        }
    } else {
        fprintf(stderr, "[WARNING] TextView: line_height is %d during resize, using default\n", tv->line_height);
        tv->visible_lines = tv->height / 16;  // Use default
        if (tv->visible_lines <= 0) tv->visible_lines = 1;
    }
    
    // Update scrollbar and redraw
    textview_update_scrollbar(tv);
    textview_draw(tv);
    
    return true;
}

// Update scrollbar dimensions based on content
void textview_update_scrollbar(TextView *tv) {
    if (!tv) return;
    
    // Recalculate visible lines based on current height
    int viewable_height = tv->height;
    if (tv->h_scrollbar_visible) viewable_height -= HORI_SCROLLBAR_HEIGHT;
    if (tv->line_height > 0) {
        tv->visible_lines = viewable_height / tv->line_height;
        if (tv->visible_lines <= 0) {
            fprintf(stderr, "[DEBUG] TextView: In scrollbar update, visible_lines = %d, forcing to 1\n", tv->visible_lines);
            tv->visible_lines = 1;
        }
    } else {
        fprintf(stderr, "[WARNING] TextView: line_height is %d in scrollbar update\n", tv->line_height);
        tv->visible_lines = viewable_height / 16;
        if (tv->visible_lines <= 0) tv->visible_lines = 1;
    }
    
    // Calculate maximum line width for horizontal scrollbar
    int x_start = tv->line_numbers ? tv->line_number_width : 8;
    tv->max_line_width = 0;
    for (int i = 0; i < tv->line_count; i++) {
        if (tv->lines[i] && *tv->lines[i]) {
            XGlyphInfo extents;
            XftTextExtentsUtf8(tv->display, tv->font, (FcChar8*)tv->lines[i],
                          strlen(tv->lines[i]), &extents);
            int line_width = x_start + extents.xOff + 10;
            if (line_width > tv->max_line_width) {
                tv->max_line_width = line_width;
            }
        }
    }
    
    // Determine scrollbar visibility
    int content_height = tv->line_count * tv->line_height;
    int viewable_width = tv->width;
    
    // Check if we need scrollbars
    bool need_v_scrollbar = (content_height > tv->height);
    bool need_h_scrollbar = (tv->max_line_width > tv->width);
    
    // If adding one scrollbar reduces space enough to need the other, show both
    if (need_v_scrollbar && !need_h_scrollbar) {
        if (tv->max_line_width > (tv->width - VERT_SCROLLBAR_WIDTH)) {
            need_h_scrollbar = true;
        }
    }
    if (need_h_scrollbar && !need_v_scrollbar) {
        if (content_height > (tv->height - HORI_SCROLLBAR_HEIGHT)) {
            need_v_scrollbar = true;
        }
    }
    
    tv->scrollbar_visible = need_v_scrollbar;
    tv->h_scrollbar_visible = need_h_scrollbar;
    
    // Update vertical scrollbar knob
    if (tv->scrollbar_visible) {
        int sb_h = tv->height;  // Always full height
        int track_height = sb_h - (VERT_ARROW_HEIGHT * 2);
        
        if (tv->line_count <= tv->visible_lines) {
            // Full knob when nothing to scroll (fills track minus padding)
            tv->scrollbar_knob_height = track_height - 2*SCROLLBAR_KNOB_PADDING;
            tv->scrollbar_knob_y = 0;
        } else {
            // Calculate knob size proportional to visible vs total (with padding)
            int usable_track = track_height - 2*SCROLLBAR_KNOB_PADDING;
            tv->scrollbar_knob_height = (tv->visible_lines * usable_track) / tv->line_count;
            if (tv->scrollbar_knob_height < 20) tv->scrollbar_knob_height = 20;
            
            // Calculate knob position (already accounts for padding in drawing)
            int scrollable_lines = tv->line_count - tv->visible_lines;
            if (scrollable_lines > 0) {
                int available_space = usable_track - tv->scrollbar_knob_height;
                tv->scrollbar_knob_y = (tv->scroll_y * available_space) / scrollable_lines;
            } else {
                tv->scrollbar_knob_y = 0;
            }
        }
    }
    
    // Update horizontal scrollbar knob
    if (tv->h_scrollbar_visible) {
        int sb_w = tv->width;
        if (tv->scrollbar_visible) sb_w -= VERT_SCROLLBAR_WIDTH;
        int track_width = sb_w - (HORI_ARROW_WIDTH * 2);
        
        viewable_width = tv->width;
        if (tv->scrollbar_visible) viewable_width -= VERT_SCROLLBAR_WIDTH;
        
        if (tv->max_line_width <= viewable_width) {
            // Full knob when nothing to scroll (fills track minus padding)
            tv->h_scrollbar_knob_width = track_width - 2*SCROLLBAR_KNOB_PADDING;
            tv->h_scrollbar_knob_x = 0;
        } else {
            // Calculate knob size proportional to visible vs total (with padding)
            int usable_track = track_width - 2*SCROLLBAR_KNOB_PADDING;
            tv->h_scrollbar_knob_width = (viewable_width * usable_track) / tv->max_line_width;
            if (tv->h_scrollbar_knob_width < 20) tv->h_scrollbar_knob_width = 20;
            
            // Calculate knob position (already accounts for padding in drawing)
            int max_scroll = tv->max_line_width - viewable_width;
            if (max_scroll > 0) {
                int available_space = usable_track - tv->h_scrollbar_knob_width;
                tv->h_scrollbar_knob_x = (tv->scroll_x * available_space) / max_scroll;
            } else {
                tv->h_scrollbar_knob_x = 0;
            }
        }
    }
}

// Select all text in the TextView
void textview_select_all(TextView *tv) {
    if (!tv || tv->line_count == 0) return;
    
    // Find the length of the last line
    int last_line_index = tv->line_count - 1;
    int last_line_len = strlen(tv->lines[last_line_index]);
    
    // Select from beginning to end
    textview_set_selection(tv, 0, 0, last_line_index, last_line_len);
}

// Copy selected text to clipboard
void textview_copy(TextView *tv) {
    if (!tv || !tv->has_selection) return;
    
    // Get selected text
    char *selection = textview_get_selection(tv);
    if (!selection) return;
    
    // Store in clipboard buffer
    if (tv->clipboard_buffer) {
        free(tv->clipboard_buffer);
    }
    tv->clipboard_buffer = selection;
    
    fprintf(stderr, "[TextView] Copied to clipboard: '%s' (%zu bytes)\n", 
            tv->clipboard_buffer, strlen(tv->clipboard_buffer));
    
    // Claim ownership of both PRIMARY and CLIPBOARD selections
    XSetSelectionOwner(tv->display, tv->primary_atom, tv->window, CurrentTime);
    XSetSelectionOwner(tv->display, tv->clipboard_atom, tv->window, CurrentTime);
}

// Cut selected text to clipboard
void textview_cut(TextView *tv) {
    if (!tv || !tv->has_selection || tv->read_only) return;
    
    // Copy to clipboard first
    textview_copy(tv);
    
    // Then delete the selection
    textview_delete_selection(tv);
    tv->modified = true;
}

// Request paste from clipboard
void textview_paste(TextView *tv) {
    if (!tv || tv->read_only) return;
    
    // Request the CLIPBOARD selection (prefer over PRIMARY)
    Window owner = XGetSelectionOwner(tv->display, tv->clipboard_atom);
    
    if (owner == None) {
        // Try PRIMARY selection as fallback
        owner = XGetSelectionOwner(tv->display, tv->primary_atom);
        if (owner == None) {
            return;  // No clipboard data available
        }
        // Request PRIMARY selection
        XConvertSelection(tv->display, tv->primary_atom, tv->utf8_atom,
                         tv->clipboard_atom, tv->window, CurrentTime);
    } else {
        // Request CLIPBOARD selection
        XConvertSelection(tv->display, tv->clipboard_atom, tv->utf8_atom,
                         tv->clipboard_atom, tv->window, CurrentTime);
    }
}

// Handle selection request from another application
void textview_handle_selection_request(TextView *tv, XSelectionRequestEvent *req) {
    if (!tv) return;
    
    XSelectionEvent reply;
    reply.type = SelectionNotify;
    reply.display = req->display;
    reply.requestor = req->requestor;
    reply.selection = req->selection;
    reply.target = req->target;
    reply.time = req->time;
    reply.property = None;
    
    if (tv->clipboard_buffer) {
        if (req->target == tv->utf8_atom) {
            // Provide UTF8 text
            XChangeProperty(tv->display, req->requestor, req->property,
                          tv->utf8_atom, 8, PropModeReplace,
                          (unsigned char*)tv->clipboard_buffer, 
                          strlen(tv->clipboard_buffer));
            reply.property = req->property;
        } else if (req->target == XA_STRING) {
            // Provide plain text
            XChangeProperty(tv->display, req->requestor, req->property,
                          XA_STRING, 8, PropModeReplace,
                          (unsigned char*)tv->clipboard_buffer, 
                          strlen(tv->clipboard_buffer));
            reply.property = req->property;
        } else if (req->target == tv->targets_atom) {
            // List supported targets
            Atom targets[] = { tv->utf8_atom, XA_STRING };
            XChangeProperty(tv->display, req->requestor, req->property,
                          XA_ATOM, 32, PropModeReplace,
                          (unsigned char*)targets, 2);
            reply.property = req->property;
        }
    }
    
    XSendEvent(tv->display, req->requestor, False, NoEventMask, (XEvent*)&reply);
}

// Handle selection notify when we receive clipboard data
void textview_handle_selection_notify(TextView *tv, XSelectionEvent *sel) {
    if (!tv || tv->read_only) return;
    
    if (sel->property != None) {
        Atom actual_type;
        int actual_format;
        unsigned long nitems, bytes_after;
        unsigned char *data = NULL;
        
        // Get the clipboard data
        if (XGetWindowProperty(tv->display, tv->window, sel->property,
                             0, (~0L), True, AnyPropertyType,
                             &actual_type, &actual_format,
                             &nitems, &bytes_after, &data) == Success && data) {
            
            // Insert the text at cursor position
            textview_insert_string(tv, (char*)data);
            tv->modified = true;
            
            XFree(data);
        }
    }
}

// =============================================================================
// SYNTAX HIGHLIGHTING IMPLEMENTATION
// =============================================================================

// Helper: Draw a line with per-character syntax colors
static void draw_line_with_colors(TextView *tv, int line_num, int x, int y) {
    if (!tv || !tv->syntax_data || line_num >= tv->line_count) {
        // No syntax highlighting - fall back to normal drawing
        if (tv->lines[line_num] && *tv->lines[line_num]) {
            XftDrawStringUtf8(tv->xft_draw, &tv->xft_fg_color, tv->font,
                            x, y, (FcChar8*)tv->lines[line_num], 
                            strlen(tv->lines[line_num]));
        }
        return;
    }
    
    TextViewSyntax *syn = (TextViewSyntax*)tv->syntax_data;
    if (!syn->highlight_func || !syn->line_cache || line_num >= syn->cache_size) {
        // No highlighting available - use normal drawing
        if (tv->lines[line_num] && *tv->lines[line_num]) {
            XftDrawStringUtf8(tv->xft_draw, &tv->xft_fg_color, tv->font,
                            x, y, (FcChar8*)tv->lines[line_num], 
                            strlen(tv->lines[line_num]));
        }
        return;
    }
    
    LineHighlight *lh = &syn->line_cache[line_num];
    if (!lh->colors) {
        // Colors not available - use normal drawing
        if (tv->lines[line_num] && *tv->lines[line_num]) {
            XftDrawStringUtf8(tv->xft_draw, &tv->xft_fg_color, tv->font,
                            x, y, (FcChar8*)tv->lines[line_num], 
                            strlen(tv->lines[line_num]));
        }
        return;
    }
    
    // Draw text character by character with appropriate colors
    char *line = tv->lines[line_num];
    if (!line || !*line) return;
    
    int current_x = x;
    int len = strlen(line);
    
    // Group consecutive characters with same color for efficiency
    int start = 0;
    while (start < len) {
        SyntaxColor color = lh->colors[start];
        int end = start + 1;
        
        // Find run of same color
        while (end < len && lh->colors[end] == color) {
            end++;
        }
        
        // Get the appropriate XftColor
        XftColor *xft_color = &tv->xft_fg_color;  // Default
        if (color < SYNTAX_MAX && syn->xft_colors_allocated[color]) {
            xft_color = &syn->xft_colors[color];
        }
        
        // Draw this run of characters
        XftDrawStringUtf8(tv->xft_draw, xft_color, tv->font,
                        current_x, y, (FcChar8*)&line[start], end - start);
        
        // Calculate width of what we just drew
        if (end > start) {
            XGlyphInfo extents;
            XftTextExtentsUtf8(tv->display, tv->font, 
                             (FcChar8*)&line[start], end - start, &extents);
            current_x += extents.xOff;
        }
        
        start = end;
    }
}

// Helper: Ensure cache size is sufficient
static void ensure_cache_size(TextView *tv, int lines_needed) {
    if (!tv->syntax_data) return;
    
    TextViewSyntax *syn = (TextViewSyntax*)tv->syntax_data;
    
    if (syn->cache_size < lines_needed) {
        int old_size = syn->cache_size;
        syn->line_cache = realloc(syn->line_cache, 
                                 lines_needed * sizeof(LineHighlight));
        if (!syn->line_cache) return;  // Out of memory
        
        // Initialize new entries
        for (int i = old_size; i < lines_needed; i++) {
            syn->line_cache[i].colors = NULL;
            syn->line_cache[i].dirty = true;
            syn->line_cache[i].last_version = -1;
        }
        syn->cache_size = lines_needed;
    }
}

// Helper: Highlight a single line with caching
static void highlight_line_cached(TextView *tv, int line_num) {
    if (!tv->syntax_data || line_num >= tv->line_count) return;
    
    TextViewSyntax *syn = (TextViewSyntax*)tv->syntax_data;
    
    // Ensure cache is big enough
    ensure_cache_size(tv, line_num + 1);
    if (!syn->line_cache) return;
    
    LineHighlight *lh = &syn->line_cache[line_num];
    
    // Check if needs update
    if (!lh->dirty && lh->last_version == syn->text_version) {
        return; // Use cached data
    }
    
    // Get line text
    char *line = tv->lines[line_num];
    if (!line) return;
    
    // Free old colors
    if (lh->colors) {
        free(lh->colors);
        lh->colors = NULL;
    }
    
    // Call the highlight function if provided
    if (syn->highlight_func && syn->syntax_context) {
        lh->colors = syn->highlight_func(syn->syntax_context, line, line_num);
    } else {
        // No syntax highlighting - use default colors
        int len = strlen(line);
        lh->colors = calloc(len + 1, sizeof(SyntaxColor));
        // All SYNTAX_NORMAL (0) by default
    }
    
    lh->dirty = false;
    lh->last_version = syn->text_version;
}

// Helper: Update highlighting for visible lines
static void update_visible_highlighting(TextView *tv) {
    if (!tv->syntax_data) return;
    
    TextViewSyntax *syn = (TextViewSyntax*)tv->syntax_data;
    if (!syn->highlight_func || !syn->syntax_context) return;
    
    // Calculate visible range with buffer
    int first = tv->scroll_y - HIGHLIGHT_BUFFER;
    int last = tv->scroll_y + tv->visible_lines + HIGHLIGHT_BUFFER;
    
    first = (first < 0) ? 0 : first;
    last = (last >= tv->line_count) ? tv->line_count - 1 : last;
    
    // Ensure cache covers visible range
    ensure_cache_size(tv, last + 1);
    
    // Highlight dirty lines in visible range
    for (int i = first; i <= last; i++) {
        if (syn->line_cache && i < syn->cache_size) {
            if (syn->line_cache[i].dirty) {
                highlight_line_cached(tv, i);
            }
        }
    }
}

// Helper: Invalidate lines in cache
static void invalidate_lines(TextView *tv, int start_line, int end_line) {
    if (!tv->syntax_data) return;
    
    TextViewSyntax *syn = (TextViewSyntax*)tv->syntax_data;
    
    // Increment version to detect stale cache
    syn->text_version++;
    
    // Mark lines as dirty
    for (int i = start_line; i <= end_line && i < syn->cache_size; i++) {
        if (syn->line_cache && i >= 0) {
            syn->line_cache[i].dirty = true;
        }
    }
    
    // Multi-line constructs might affect following lines
    if (syn->highlight_func && end_line < tv->line_count - 1) {
        // Be conservative: mark rest of visible area dirty
        int last_visible = tv->scroll_y + tv->visible_lines + HIGHLIGHT_BUFFER;
        for (int i = end_line + 1; i <= last_visible && i < syn->cache_size; i++) {
            if (syn->line_cache && i >= 0) {
                syn->line_cache[i].dirty = true;
            }
        }
    }
}

// Set syntax highlighting context with callback
void textview_set_syntax_highlight(TextView *tv, void *context,
                                   TextViewSyntaxCallback highlight_func,
                                   uint32_t *palette, int palette_size) {
    if (!tv) return;
    
    // Allocate syntax data if needed
    if (!tv->syntax_data) {
        tv->syntax_data = calloc(1, sizeof(TextViewSyntax));
        if (!tv->syntax_data) return;  // Out of memory
    }
    
    TextViewSyntax *syn = (TextViewSyntax*)tv->syntax_data;
    
    // Store context and callback
    syn->syntax_context = context;
    syn->highlight_func = (SyntaxHighlightFunc)highlight_func;
    
    // Copy color palette
    if (palette && palette_size > 0) {
        int count = (palette_size < SYNTAX_MAX) ? palette_size : SYNTAX_MAX;
        memcpy(syn->color_palette, palette, count * sizeof(uint32_t));
        
        // Allocate XftColors for the palette
        for (int i = 0; i < count; i++) {
            // Free old color if allocated
            if (syn->xft_colors_allocated[i]) {
                XftColorFree(tv->display, 
                           DefaultVisual(tv->display, DefaultScreen(tv->display)),
                           DefaultColormap(tv->display, DefaultScreen(tv->display)), 
                           &syn->xft_colors[i]);
            }
            
            // Allocate new color
            XRenderColor render_color = {
                .red = ((syn->color_palette[i] >> 16) & 0xFF) * 257,
                .green = ((syn->color_palette[i] >> 8) & 0xFF) * 257,
                .blue = (syn->color_palette[i] & 0xFF) * 257,
                .alpha = 0xFFFF
            };
            
            syn->xft_colors_allocated[i] = XftColorAllocValue(tv->display,
                DefaultVisual(tv->display, DefaultScreen(tv->display)),
                DefaultColormap(tv->display, DefaultScreen(tv->display)),
                &render_color, &syn->xft_colors[i]);
        }
    }
    
    // Invalidate all lines
    if (tv->line_count > 0) {
        invalidate_lines(tv, 0, tv->line_count - 1);
    }
    
    // Trigger redraw
    textview_draw(tv);
}

// Force re-highlighting of all visible lines
void textview_highlight_all_lines(TextView *tv) {
    if (!tv || !tv->syntax_data) return;
    
    TextViewSyntax *syn = (TextViewSyntax*)tv->syntax_data;
    if (!syn->highlight_func || !syn->syntax_context) return;
    
    // Mark all lines as dirty
    if (syn->line_cache) {
        for (int i = 0; i < syn->cache_size; i++) {
            syn->line_cache[i].dirty = true;
        }
    }
    
    // Update highlighting for visible area
    update_visible_highlighting(tv);
    
    // Trigger redraw
    textview_draw(tv);
}

// =============================================================================
// UNDO/REDO IMPLEMENTATION
// =============================================================================

// Create a new undo entry
static UndoEntry* create_undo_entry(UndoType type, int line, int col, 
                                   const char *text, int text_len) {
    UndoEntry *entry = calloc(1, sizeof(UndoEntry));
    if (!entry) return NULL;
    
    entry->type = type;
    entry->line = line;
    entry->col = col;
    entry->text_len = text_len;
    
    if (text && text_len > 0) {
        entry->text = malloc(text_len + 1);
        if (entry->text) {
            memcpy(entry->text, text, text_len);
            entry->text[text_len] = '\0';
        }
    }
    
    return entry;
}

// Free an undo entry
static void free_undo_entry(UndoEntry *entry) {
    if (!entry) return;
    if (entry->text) free(entry->text);
    free(entry);
}

// Clear all redo entries (called when new edit is made after undo)
static void clear_redo_history(TextView *tv) {
    if (!tv || !tv->undo_history) return;
    
    UndoHistory *history = (UndoHistory*)tv->undo_history;
    
    // If we're at the tail, there's no redo history to clear
    if (history->current == history->tail) return;
    
    // Delete all entries after current
    UndoEntry *entry = history->current ? history->current->next : history->head;
    while (entry) {
        UndoEntry *next = entry->next;
        
        // Remove from list
        if (entry->prev) entry->prev->next = NULL;
        history->tail = entry->prev;
        history->count--;
        
        free_undo_entry(entry);
        entry = next;
    }
}

// Record an undo operation
static void record_undo(TextView *tv, UndoType type, int line, int col, 
                       const char *text, int text_len) {
    if (!tv || !tv->undo_history) return;
    
    UndoHistory *history = (UndoHistory*)tv->undo_history;
    
    // Don't record if we're in undo/redo operation
    if (history->in_undo_redo) return;
    
    // Clear any redo history
    clear_redo_history(tv);
    
    // Create new entry
    UndoEntry *entry = create_undo_entry(type, line, col, text, text_len);
    if (!entry) return;
    
    // Add to list
    if (!history->head) {
        // First entry
        history->head = history->tail = entry;
    } else {
        // Add to tail
        entry->prev = history->tail;
        history->tail->next = entry;
        history->tail = entry;
    }
    
    history->current = entry;
    history->count++;
    
    // Remove oldest entries if we exceed max
    while (history->count > history->max_count && history->head) {
        UndoEntry *old = history->head;
        history->head = old->next;
        if (history->head) {
            history->head->prev = NULL;
        } else {
            history->tail = NULL;
        }
        history->count--;
        free_undo_entry(old);
    }
}

// Undo the last operation
void textview_undo(TextView *tv) {
    if (!tv || !tv->undo_history || tv->read_only) return;
    
    UndoHistory *history = (UndoHistory*)tv->undo_history;
    if (!history->current) return;
    
    UndoEntry *entry = history->current;
    history->in_undo_redo = true;  // Prevent recording
    
    // Restore cursor position first
    tv->cursor_line = entry->line;
    tv->cursor_col = entry->col;
    
    // Perform the reverse operation
    switch (entry->type) {
        case UNDO_INSERT_CHAR:
            // Undo character insertion - delete it
            if (tv->cursor_col < strlen(tv->lines[tv->cursor_line])) {
                char *line = tv->lines[tv->cursor_line];
                int len = strlen(line);
                memmove(line + tv->cursor_col, line + tv->cursor_col + 1, 
                        len - tv->cursor_col);
            }
            break;
            
        case UNDO_DELETE_CHAR:
            // Undo character deletion - reinsert it
            if (entry->text && entry->text_len > 0) {
                char *line = tv->lines[tv->cursor_line];
                int len = strlen(line);
                char *new_line = malloc(len + 2);
                if (tv->cursor_col > 0) {
                    strncpy(new_line, line, tv->cursor_col);
                }
                new_line[tv->cursor_col] = entry->text[0];
                strcpy(new_line + tv->cursor_col + 1, line + tv->cursor_col);
                free(tv->lines[tv->cursor_line]);
                tv->lines[tv->cursor_line] = new_line;
            }
            break;
            
        case UNDO_NEWLINE:
            // Undo newline - join lines
            if (tv->cursor_line < tv->line_count - 1) {
                char *current = tv->lines[tv->cursor_line];
                char *next = tv->lines[tv->cursor_line + 1];
                char *joined = malloc(strlen(current) + strlen(next) + 1);
                strcpy(joined, current);
                strcat(joined, next);
                
                free(current);
                free(next);
                tv->lines[tv->cursor_line] = joined;
                
                // Shift lines up
                for (int i = tv->cursor_line + 1; i < tv->line_count - 1; i++) {
                    tv->lines[i] = tv->lines[i + 1];
                }
                tv->line_count--;
            }
            break;
            
        case UNDO_JOIN_LINES:
            // Undo line join - split lines
            if (entry->text) {
                // Need to split the line at cursor position
                char *line = tv->lines[tv->cursor_line];
                
                // Grow line array if needed
                if (tv->line_count >= tv->line_capacity) {
                    tv->line_capacity += LINE_CHUNK_SIZE;
                    tv->lines = realloc(tv->lines, tv->line_capacity * sizeof(char*));
                }
                
                // Shift lines down
                for (int i = tv->line_count; i > tv->cursor_line + 1; i--) {
                    tv->lines[i] = tv->lines[i - 1];
                }
                
                // Split the line
                char *first = malloc(tv->cursor_col + 1);
                strncpy(first, line, tv->cursor_col);
                first[tv->cursor_col] = '\0';
                
                char *second = strdup(line + tv->cursor_col);
                
                free(tv->lines[tv->cursor_line]);
                tv->lines[tv->cursor_line] = first;
                tv->lines[tv->cursor_line + 1] = second;
                tv->line_count++;
            }
            break;
            
        default:
            break;
    }
    
    // Move current pointer back
    history->current = entry->prev;
    
    history->in_undo_redo = false;
    tv->modified = true;
    
    // Update display
    if (tv->syntax_data) {
        invalidate_lines(tv, 0, tv->line_count - 1);
    }
    textview_ensure_cursor_visible(tv);
    textview_update_scrollbar(tv);
    textview_draw(tv);
}

// Redo the last undone operation
void textview_redo(TextView *tv) {
    if (!tv || !tv->undo_history || tv->read_only) return;
    
    UndoHistory *history = (UndoHistory*)tv->undo_history;
    
    // Find the next entry to redo
    UndoEntry *entry = history->current ? history->current->next : history->head;
    if (!entry) return;
    
    history->in_undo_redo = true;  // Prevent recording
    
    // Restore cursor position
    tv->cursor_line = entry->line;
    tv->cursor_col = entry->col;
    
    // Perform the operation again
    switch (entry->type) {
        case UNDO_INSERT_CHAR:
            // Redo character insertion
            if (entry->text && entry->text_len > 0) {
                char *line = tv->lines[tv->cursor_line];
                int len = strlen(line);
                char *new_line = malloc(len + 2);
                if (tv->cursor_col > 0) {
                    strncpy(new_line, line, tv->cursor_col);
                }
                new_line[tv->cursor_col] = entry->text[0];
                strcpy(new_line + tv->cursor_col + 1, line + tv->cursor_col);
                free(tv->lines[tv->cursor_line]);
                tv->lines[tv->cursor_line] = new_line;
                tv->cursor_col++;
            }
            break;
            
        case UNDO_DELETE_CHAR:
            // Redo character deletion
            if (tv->cursor_col < strlen(tv->lines[tv->cursor_line])) {
                char *line = tv->lines[tv->cursor_line];
                int len = strlen(line);
                memmove(line + tv->cursor_col, line + tv->cursor_col + 1, 
                        len - tv->cursor_col);
            }
            break;
            
        case UNDO_NEWLINE:
            // Redo newline insertion
            {
                // Grow line array if needed
                if (tv->line_count >= tv->line_capacity) {
                    tv->line_capacity += LINE_CHUNK_SIZE;
                    tv->lines = realloc(tv->lines, tv->line_capacity * sizeof(char*));
                }
                
                char *current = tv->lines[tv->cursor_line];
                
                // Shift lines down
                for (int i = tv->line_count; i > tv->cursor_line + 1; i--) {
                    tv->lines[i] = tv->lines[i - 1];
                }
                
                // Split the line
                tv->lines[tv->cursor_line] = malloc(tv->cursor_col + 1);
                strncpy(tv->lines[tv->cursor_line], current, tv->cursor_col);
                tv->lines[tv->cursor_line][tv->cursor_col] = '\0';
                
                tv->lines[tv->cursor_line + 1] = strdup(current + tv->cursor_col);
                free(current);
                
                tv->line_count++;
                tv->cursor_line++;
                tv->cursor_col = 0;
            }
            break;
            
        case UNDO_JOIN_LINES:
            // Redo line join - basically join the lines again
            if (tv->cursor_line < tv->line_count - 1) {
                char *current = tv->lines[tv->cursor_line];
                char *next = tv->lines[tv->cursor_line + 1];
                char *joined = malloc(strlen(current) + strlen(next) + 1);
                strcpy(joined, current);
                strcat(joined, next);
                
                free(current);
                free(next);
                tv->lines[tv->cursor_line] = joined;
                
                // Shift lines up
                for (int i = tv->cursor_line + 1; i < tv->line_count - 1; i++) {
                    tv->lines[i] = tv->lines[i + 1];
                }
                tv->line_count--;
            }
            break;
            
        default:
            break;
    }
    
    // Move current pointer forward
    history->current = entry;
    
    history->in_undo_redo = false;
    tv->modified = true;
    
    // Update display
    if (tv->syntax_data) {
        invalidate_lines(tv, 0, tv->line_count - 1);
    }
    textview_ensure_cursor_visible(tv);
    textview_update_scrollbar(tv);
    textview_draw(tv);
}

// Check if undo is available
bool textview_can_undo(TextView *tv) {
    if (!tv || !tv->undo_history) return false;
    UndoHistory *history = (UndoHistory*)tv->undo_history;
    return history->current != NULL;
}

// Check if redo is available
bool textview_can_redo(TextView *tv) {
    if (!tv || !tv->undo_history) return false;
    UndoHistory *history = (UndoHistory*)tv->undo_history;
    return (history->current != history->tail) && 
           (history->current ? history->current->next : history->head) != NULL;
}
