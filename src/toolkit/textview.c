#include "textview.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xrender.h>

#define INITIAL_LINE_CAPACITY 100
#define LINE_CHUNK_SIZE 100
#define TAB_WIDTH 4
#define PATH_SIZE 512

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
    
    // Create the window with gray background
    XSetWindowAttributes attrs;
    attrs.background_pixel = 0xa2a2a0;  // Gray color
    attrs.border_pixel = BlackPixel(display, DefaultScreen(display));
    attrs.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | 
                       ButtonReleaseMask | PointerMotionMask | 
                       FocusChangeMask | StructureNotifyMask;
    
    tv->window = XCreateWindow(display, parent, x, y, width, height, 0,
                              CopyFromParent, InputOutput, CopyFromParent,
                              CWBackPixel | CWBorderPixel | CWEventMask, &attrs);
    
    // Initialize text buffer
    tv->line_capacity = INITIAL_LINE_CAPACITY;
    tv->lines = calloc(tv->line_capacity, sizeof(char*));
    tv->lines_dirty = calloc(tv->line_capacity, sizeof(bool));
    
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
    
    XMapWindow(display, tv->window);
    return tv;
}

// Destroy TextView and free resources
void textview_destroy(TextView *tv) {
    if (!tv) return;
    
    // Free text lines
    for (int i = 0; i < tv->line_count; i++) {
        free(tv->lines[i]);
        if (tv->line_colors && tv->line_colors[i]) {
            free(tv->line_colors[i]);
        }
    }
    free(tv->lines);
    free(tv->lines_dirty);
    if (tv->line_colors) free(tv->line_colors);
    
    // Free clipboard buffer
    if (tv->clipboard_buffer) free(tv->clipboard_buffer);
    
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
            tv->lines_dirty = realloc(tv->lines_dirty, tv->line_capacity * sizeof(bool));
        }
        
        // Copy line (strip \r if present)
        int len = end - start;
        // Check if line ends with \r (CRLF)
        if (len > 0 && start[len - 1] == '\r') {
            len--;  // Remove the \r
        }
        
        tv->lines[tv->line_count] = malloc(len + 1);
        if (len > 0) {
            strncpy(tv->lines[tv->line_count], start, len);
        }
        tv->lines[tv->line_count][len] = '\0';
        tv->lines_dirty[tv->line_count] = true;
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
    tv->lines_dirty[tv->cursor_line] = true;
    
    tv->cursor_col++;
    tv->modified = true;
    
    if (tv->on_change) tv->on_change(tv);
    textview_ensure_cursor_visible(tv);
    textview_update_scrollbar(tv);
    textview_draw(tv);
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
        tv->lines_dirty = realloc(tv->lines_dirty, tv->line_capacity * sizeof(bool));
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
        tv->lines_dirty[i] = tv->lines_dirty[i - 1];
    }
    
    // Insert new line
    tv->lines[tv->cursor_line + 1] = new_line;
    tv->lines_dirty[tv->cursor_line] = true;
    tv->lines_dirty[tv->cursor_line + 1] = true;
    tv->line_count++;
    
    // Move cursor to start of new line
    tv->cursor_line++;
    tv->cursor_col = 0;
    tv->modified = true;
    
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
        
        // Shift characters left
        memmove(line + tv->cursor_col - 1, line + tv->cursor_col, 
                len - tv->cursor_col + 1);
        
        tv->cursor_col--;
        tv->lines_dirty[tv->cursor_line] = true;
    } else if (tv->cursor_line > 0) {
        // Join with previous line
        char *prev_line = tv->lines[tv->cursor_line - 1];
        char *current_line = tv->lines[tv->cursor_line];
        int prev_len = strlen(prev_line);
        
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
            tv->lines_dirty[i] = tv->lines_dirty[i + 1];
        }
        tv->line_count--;
        
        // Move cursor to join point
        tv->cursor_line--;
        tv->cursor_col = prev_len;
        tv->lines_dirty[tv->cursor_line] = true;
    }
    
    tv->modified = true;
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
        tv->lines_dirty[tv->cursor_line] = true;
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
            tv->lines_dirty[i] = tv->lines_dirty[i + 1];
        }
        tv->line_count--;
        tv->lines_dirty[tv->cursor_line] = true;
    }
    
    tv->modified = true;
    if (tv->on_change) tv->on_change(tv);
    textview_ensure_cursor_visible(tv);
    textview_update_scrollbar(tv);
    textview_draw(tv);
}

// Basic cursor movement functions
void textview_move_cursor_left(TextView *tv) {
    if (!tv) return;
    
    if (tv->cursor_col > 0) {
        tv->cursor_col--;
    } else if (tv->cursor_line > 0) {
        tv->cursor_line--;
        tv->cursor_col = strlen(tv->lines[tv->cursor_line]);
    }
    
    if (tv->on_cursor_move) tv->on_cursor_move(tv);
    textview_ensure_cursor_visible(tv);
    textview_update_cursor(tv);
}

void textview_move_cursor_right(TextView *tv) {
    if (!tv) return;
    
    int line_len = strlen(tv->lines[tv->cursor_line]);
    if (tv->cursor_col < line_len) {
        tv->cursor_col++;
    } else if (tv->cursor_line < tv->line_count - 1) {
        tv->cursor_line++;
        tv->cursor_col = 0;
    }
    
    if (tv->on_cursor_move) tv->on_cursor_move(tv);
    textview_ensure_cursor_visible(tv);
    textview_update_cursor(tv);
}

void textview_move_cursor_up(TextView *tv) {
    if (!tv || tv->cursor_line == 0) return;
    
    tv->cursor_line--;
    int line_len = strlen(tv->lines[tv->cursor_line]);
    if (tv->cursor_col > line_len) {
        tv->cursor_col = line_len;
    }
    
    if (tv->on_cursor_move) tv->on_cursor_move(tv);
    textview_ensure_cursor_visible(tv);
    textview_update_cursor(tv);
}

void textview_move_cursor_down(TextView *tv) {
    if (!tv || tv->cursor_line >= tv->line_count - 1) return;
    
    tv->cursor_line++;
    int line_len = strlen(tv->lines[tv->cursor_line]);
    if (tv->cursor_col > line_len) {
        tv->cursor_col = line_len;
    }
    
    if (tv->on_cursor_move) tv->on_cursor_move(tv);
    textview_ensure_cursor_visible(tv);
    textview_update_cursor(tv);
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
    int x_start = tv->line_numbers ? tv->line_number_width : 5;
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
    
    // Clear window
    XClearWindow(tv->display, tv->window);
    
    // Check if colors are allocated
    if (!tv->colors_allocated) {
        return;  // Can't draw without colors
    }
    
    // Calculate starting position
    int x_start = 5;  // Small margin
    if (tv->line_numbers) {
        // Calculate width needed for line numbers
        char line_num_str[32];
        snprintf(line_num_str, sizeof(line_num_str), "%d ", tv->line_count);
        XGlyphInfo extents;
        XftTextExtentsUtf8(tv->display, tv->font, (FcChar8*)line_num_str, 
                       strlen(line_num_str), &extents);
        tv->line_number_width = extents.xOff + 10;
        x_start = tv->line_number_width;
    }
    
    // Calculate maximum content height for scrollbar visibility
    int content_height = tv->line_count * tv->line_height;
    int viewable_height = tv->height;
    if (tv->h_scrollbar_visible) viewable_height -= HORI_SCROLLBAR_HEIGHT;
    
    // Calculate maximum line width for horizontal scrollbar
    tv->max_line_width = 0;
    for (int i = 0; i < tv->line_count; i++) {
        if (tv->lines[i] && *tv->lines[i]) {
            XGlyphInfo extents;
            XftTextExtentsUtf8(tv->display, tv->font, (FcChar8*)tv->lines[i],
                          strlen(tv->lines[i]), &extents);
            int line_width = x_start + extents.xOff + 10;  // Include margins
            if (line_width > tv->max_line_width) {
                tv->max_line_width = line_width;
            }
        }
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
    
    // Draw visible lines
    int y = tv->line_height;
    
    // Defensive check for visible_lines
    int lines_to_draw = tv->visible_lines;
    if (lines_to_draw <= 0) {
        fprintf(stderr, "[DEBUG] TextView: visible_lines is %d, forcing to viewport calculation\n", lines_to_draw);
        lines_to_draw = tv->height / 16;  // Use default line height
        if (lines_to_draw <= 0) lines_to_draw = 1;
    }
    
    for (int i = tv->scroll_y; 
         i < tv->line_count && i < tv->scroll_y + lines_to_draw; 
         i++) {
        
        // Draw line number if enabled
        if (tv->line_numbers) {
            char line_num[32];
            snprintf(line_num, sizeof(line_num), "%4d", i + 1);
            XftDrawStringUtf8(tv->xft_draw, &tv->xft_line_num_color, tv->font,
                          5, y - 2, (FcChar8*)line_num, strlen(line_num));
        }
        
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
                    
                    XRenderFreePicture(tv->display, dest);
                }
            }
        }
        
        // Draw the text line with horizontal scrolling
        if (line && *line) {
            // Apply horizontal scroll offset
            int text_x = x_start - tv->scroll_x;
            
            // Set clipping to prevent text from appearing in scrollbar area
            XRectangle clip_rect;
            clip_rect.x = 0;
            clip_rect.y = 0;
            clip_rect.width = tv->width;
            clip_rect.height = tv->height;
            if (tv->scrollbar_visible) clip_rect.width -= VERT_SCROLLBAR_WIDTH;
            if (tv->h_scrollbar_visible) clip_rect.height -= HORI_SCROLLBAR_HEIGHT;
            
            Region clip_region = XCreateRegion();
            XUnionRectWithRegion(&clip_rect, clip_region, clip_region);
            XftDrawSetClipRectangles(tv->xft_draw, 0, 0, &clip_rect, 1);
            
            XftDrawStringUtf8(tv->xft_draw, &tv->xft_fg_color, tv->font,
                          text_x, y - 2, (FcChar8*)line, strlen(line));
            
            // Reset clipping
            XftDrawSetClip(tv->xft_draw, NULL);
            XDestroyRegion(clip_region);
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
            
            if (cursor_x >= 0 && cursor_x < viewport_width) {
                // Draw blue rectangle cursor like InputField
                // Get size of a space character for cursor width
                XGlyphInfo space_info;
                XftTextExtentsUtf8(tv->display, tv->font, (FcChar8*)" ", 1, &space_info);
                int cursor_width = space_info.xOff > 0 ? space_info.xOff : 8;
                
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
    
    // Colors are now persistent - no cleanup needed here
}

// Update only the cursor position (optimized drawing)
void textview_update_cursor(TextView *tv) {
    if (!tv || !tv->xft_draw || !tv->font || !tv->colors_allocated) return;
    
    // Calculate line height for drawing
    int line_height = tv->line_height;
    int x_start = tv->line_numbers ? tv->line_number_width : 5;
    
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
        
        int y = (line_idx - tv->scroll_y + 1) * line_height;
        const char *line = tv->lines[line_idx];
        
        // Clear just this line's area (with extra pixels for complete cursor cleanup)
        // Cursor draws from (y - line_height + 2) to (y + 2), so we need to clear that fully
        // But stop before the black separator line
        XClearArea(tv->display, tv->window, 0, y - line_height,  // Start at line top
                   viewport_width,  // Don't clear separator or scrollbar
                   line_height + 3, False);  // +3 to ensure we clear through y+2
        
        // Set clipping to prevent drawing into scrollbar area
        XRectangle clip_rect;
        clip_rect.x = 0;
        clip_rect.y = y - line_height;  // Match the clear area
        clip_rect.width = viewport_width;
        clip_rect.height = line_height + 3;  // Match the clear area
        Region clip_region = XCreateRegion();
        XUnionRectWithRegion(&clip_rect, clip_region, clip_region);
        XftDrawSetClipRectangles(tv->xft_draw, 0, 0, &clip_rect, 1);
        
        // Redraw line number if enabled
        if (tv->line_numbers) {
            char line_num[32];
            snprintf(line_num, sizeof(line_num), "%4d", line_idx + 1);
            XftDrawStringUtf8(tv->xft_draw, &tv->xft_line_num_color, tv->font,
                            5, y - 2, (FcChar8*)line_num, strlen(line_num));
        }
        
        // Draw the text with horizontal scroll offset
        if (line && *line) {
            int text_x = x_start - tv->scroll_x;
            XftDrawStringUtf8(tv->xft_draw, &tv->xft_fg_color, tv->font,
                            text_x, y - 2, (FcChar8*)line, strlen(line));
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
    } else if (tv->prev_cursor_col != tv->cursor_col) {
        // Cursor moved within same line, just redraw that line
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
    
    // Check for clipboard shortcuts (Super or Ctrl key)
    if ((event->state & Mod4Mask) || (event->state & ControlMask)) {
        fprintf(stderr, "[TextView] Mod4/Ctrl detected, keysym=0x%lx\n", keysym);
        switch (keysym) {
            case XK_c:  // Copy
            case XK_C:
            case XK_y:  // Alternative copy for testing
            case XK_Y:
                fprintf(stderr, "[TextView] Copy triggered (key=%c)\n", (char)keysym);
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
    int x_start = tv->line_numbers ? tv->line_number_width : 5;
    int clicked_col = 0;
    
    // Adjust click X position for horizontal scroll
    int adjusted_x = event->x + tv->scroll_x;
    
    if (adjusted_x > x_start && clicked_line < tv->line_count) {
        // Get the line text
        const char *line = tv->lines[clicked_line];
        int line_len = strlen(line);
        int x_pos = x_start;
        
        // Find which character was clicked
        for (int i = 0; i < line_len; i++) {
            XGlyphInfo extents;
            XftTextExtentsUtf8(tv->display, tv->font, (FcChar8*)&line[i], 1, &extents);
            int char_width = extents.xOff;
            
            // Check if click is within this character
            if (adjusted_x < x_pos + char_width / 2) {
                clicked_col = i;
                break;
            }
            x_pos += char_width;
            clicked_col = i + 1;
        }
    }
    
    // Clear any existing selection on click
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
    textview_update_cursor(tv);
    
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
    
    // CRITICAL: Never allow text selection in scrollbar areas!
    if (tv->scrollbar_visible && event->x >= tv->width - VERT_SCROLLBAR_WIDTH) {
        return true;  // Consume event - no text selection in vertical scrollbar!
    }
    if (tv->h_scrollbar_visible && event->y >= tv->height - HORI_SCROLLBAR_HEIGHT) {
        return true;  // Consume event - no text selection in horizontal scrollbar!
    }
    
    // Calculate which line/col we're over
    int motion_line = tv->scroll_y + (event->y / tv->line_height);
    if (motion_line >= tv->line_count) motion_line = tv->line_count - 1;
    if (motion_line < 0) motion_line = 0;
    
    int x_start = tv->line_numbers ? tv->line_number_width : 5;
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
    
    // Auto-scroll if mouse is near edges or outside window
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
            // Faster scroll when further from edge
            int speed = (event->y < 0) ? 3 : 1;
            tv->scroll_y -= speed;
            if (tv->scroll_y < 0) tv->scroll_y = 0;
            need_scroll = true;
        }
    } else if (event->y >= viewport_height || event->y > viewport_height - scroll_zone) {
        // Near bottom of viewport (not including horizontal scrollbar area)
        int max_scroll = tv->line_count - tv->visible_lines;
        if (max_scroll < 0) max_scroll = 0;
        if (tv->scroll_y < max_scroll) {
            // Faster scroll when further from edge
            int speed = (event->y >= viewport_height) ? 3 : 1;
            tv->scroll_y += speed;
            if (tv->scroll_y > max_scroll) tv->scroll_y = max_scroll;
            need_scroll = true;
        }
    }
    
    // Horizontal scrolling - auto-scroll when selection extends beyond viewport
    if (event->x < 0 || event->x < scroll_zone) {
        // Near left edge - scroll left
        if (tv->scroll_x > 0) {
            int speed = (event->x < 0) ? 20 : 5;  // Faster for horizontal
            tv->scroll_x -= speed;
            if (tv->scroll_x < 0) tv->scroll_x = 0;
            need_scroll = true;
        }
    } else if (event->x >= viewport_width || event->x > viewport_width - scroll_zone) {
        // Near right edge of viewport (not including vertical scrollbar)
        int max_scroll = tv->max_line_width - viewport_width;
        if (max_scroll > 0 && tv->scroll_x < max_scroll) {
            int speed = (event->x >= viewport_width) ? 20 : 5;  // Faster for horizontal
            tv->scroll_x += speed;
            if (tv->scroll_x > max_scroll) tv->scroll_x = max_scroll;
            need_scroll = true;
        }
    }
    
    if (need_scroll) {
        textview_update_scrollbar(tv);
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
    
    // Redraw
    textview_draw(tv);
}

void textview_set_syntax(TextView *tv, void *syntax_def) {
    if (!tv) return;
    tv->current_syntax = syntax_def;
    // Mark all lines for re-highlighting
    for (int i = 0; i < tv->line_count; i++) {
        tv->lines_dirty[i] = true;
    }
}

void textview_update_highlighting(TextView *tv) {
    // TODO: Implement syntax highlighting
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
    int x_start = tv->line_numbers ? tv->line_number_width : 5;
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
