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
    
    // No selection initially
    tv->has_selection = false;
    
    // Initialize scrollbar state
    tv->scrollbar_visible = false;
    tv->scrollbar_dragging = false;
    tv->scrollbar_knob_y = 0;
    tv->scrollbar_knob_height = 0;
    
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
        XftTextExtents8(display, tv->font, (FcChar8*)"M", 1, &extents);
        tv->char_width = extents.xOff;
        tv->line_height = tv->font->height + 2;  // Small padding
        tv->visible_lines = tv->height / tv->line_height;
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
        
        // Copy line
        int len = end - start;
        tv->lines[tv->line_count] = malloc(len + 1);
        strncpy(tv->lines[tv->line_count], start, len);
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
    
    // Reset cursor
    tv->cursor_line = 0;
    tv->cursor_col = 0;
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
    textview_draw(tv);
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
    textview_draw(tv);
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
    textview_draw(tv);
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
    textview_draw(tv);
}

void textview_move_cursor_home(TextView *tv) {
    if (!tv) return;
    tv->cursor_col = 0;
    if (tv->on_cursor_move) tv->on_cursor_move(tv);
    textview_ensure_cursor_visible(tv);
    textview_draw(tv);
}

void textview_move_cursor_end(TextView *tv) {
    if (!tv) return;
    tv->cursor_col = strlen(tv->lines[tv->cursor_line]);
    if (tv->on_cursor_move) tv->on_cursor_move(tv);
    textview_ensure_cursor_visible(tv);
    textview_draw(tv);
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
    
    // Horizontal scrolling (if needed later)
    // TODO: Implement horizontal scrolling
}

// Draw the TextView
void textview_draw(TextView *tv) {
    if (!tv || !tv->xft_draw || !tv->font) return;
    
    // Clear window
    XClearWindow(tv->display, tv->window);
    
    // Setup colors
    XftColor fg_color, sel_color, line_num_color;
    XRenderColor render_color;
    
    // Normal text color (black)
    render_color.red = 0;
    render_color.green = 0;
    render_color.blue = 0;
    render_color.alpha = 0xFFFF;
    XftColorAllocValue(tv->display, DefaultVisual(tv->display, DefaultScreen(tv->display)),
                       DefaultColormap(tv->display, DefaultScreen(tv->display)),
                       &render_color, &fg_color);
    
    // Selection color (white on blue)
    render_color.red = 0xFFFF;
    render_color.green = 0xFFFF;
    render_color.blue = 0xFFFF;
    XftColorAllocValue(tv->display, DefaultVisual(tv->display, DefaultScreen(tv->display)),
                       DefaultColormap(tv->display, DefaultScreen(tv->display)),
                       &render_color, &sel_color);
    
    // Line number color (dark gray)
    render_color.red = 0x6666;
    render_color.green = 0x6666;
    render_color.blue = 0x6666;
    XftColorAllocValue(tv->display, DefaultVisual(tv->display, DefaultScreen(tv->display)),
                       DefaultColormap(tv->display, DefaultScreen(tv->display)),
                       &render_color, &line_num_color);
    
    // Calculate starting position
    int x_start = 5;  // Small margin
    if (tv->line_numbers) {
        // Calculate width needed for line numbers
        char line_num_str[32];
        snprintf(line_num_str, sizeof(line_num_str), "%d ", tv->line_count);
        XGlyphInfo extents;
        XftTextExtents8(tv->display, tv->font, (FcChar8*)line_num_str, 
                       strlen(line_num_str), &extents);
        tv->line_number_width = extents.xOff + 10;
        x_start = tv->line_number_width;
    }
    
    // Draw visible lines
    int y = tv->line_height;
    for (int i = tv->scroll_y; 
         i < tv->line_count && i < tv->scroll_y + tv->visible_lines; 
         i++) {
        
        // Draw line number if enabled
        if (tv->line_numbers) {
            char line_num[32];
            snprintf(line_num, sizeof(line_num), "%4d", i + 1);
            XftDrawString8(tv->xft_draw, &line_num_color, tv->font,
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
                    
                    // Calculate selection rectangle position
                    int sel_x = x_start;
                    if (draw_start > 0) {
                        XGlyphInfo extents;
                        XftTextExtents8(tv->display, tv->font, (FcChar8*)line, draw_start, &extents);
                        sel_x += extents.xOff;
                    }
                    
                    int sel_width = 0;
                    if (draw_end > draw_start) {
                        XGlyphInfo extents;
                        XftTextExtents8(tv->display, tv->font, (FcChar8*)&line[draw_start], 
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
        
        // Draw the text line
        if (line && *line) {
            XftDrawString8(tv->xft_draw, &fg_color, tv->font,
                          x_start, y - 2, (FcChar8*)line, strlen(line));
        }
        
        // Draw cursor if on this line and focused
        if (tv->has_focus && i == tv->cursor_line) {
            int cursor_x = x_start;
            
            // Calculate cursor position
            if (tv->cursor_col > 0) {
                int chars_to_measure = (tv->cursor_col > strlen(line)) ? strlen(line) : tv->cursor_col;
                if (chars_to_measure > 0) {
                    XGlyphInfo extents;
                    XftTextExtents8(tv->display, tv->font, (FcChar8*)line,
                                   chars_to_measure, &extents);
                    cursor_x += extents.xOff;
                }
            }
            
            // Draw blue rectangle cursor like InputField
            // Get size of a space character for cursor width
            XGlyphInfo space_info;
            XftTextExtents8(tv->display, tv->font, (FcChar8*)" ", 1, &space_info);
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
                char ch[2] = {line[tv->cursor_col], '\0'};
                XftDrawString8(tv->xft_draw, &sel_color, tv->font,
                             cursor_x, y - 2, (FcChar8*)ch, 1);
            }
        }
        
        y += tv->line_height;
    }
    
    // Draw scrollbar if needed
    if (tv->scrollbar_visible) {
        // Get XRender Picture for drawing
        Picture dest = XRenderCreatePicture(tv->display, tv->window,
            XRenderFindVisualFormat(tv->display, DefaultVisual(tv->display, DefaultScreen(tv->display))),
            0, NULL);
        
        // Colors for scrollbar
        XRenderColor white = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
        XRenderColor black = {0x0000, 0x0000, 0x0000, 0xFFFF};
        XRenderColor gray = {0xa2a2, 0xa2a2, 0xa0a0, 0xFFFF};
        
        int sb_x = tv->width - TEXTVIEW_SCROLLBAR_WIDTH;
        int sb_w = TEXTVIEW_SCROLLBAR_WIDTH;
        
        // Draw black separator line on the left of scrollbar (full height)
        XRenderFillRectangle(tv->display, PictOpSrc, dest, &black, 
                           sb_x - 1, 0, 1, tv->height);
        
        // Calculate positions for the three elements
        int track_y = 0;
        int track_h = tv->height - (TEXTVIEW_ARROW_HEIGHT * 2);
        int arrow_up_y = tv->height - (TEXTVIEW_ARROW_HEIGHT * 2);
        int arrow_down_y = tv->height - TEXTVIEW_ARROW_HEIGHT;
        
        // 1. Draw scrollbar track (full height)
        XRenderFillRectangle(tv->display, PictOpSrc, dest, &gray, sb_x, track_y, sb_w, tv->height);
        
        // Track borders
        XRenderFillRectangle(tv->display, PictOpSrc, dest, &white, sb_x, track_y, 1, track_h);
        XRenderFillRectangle(tv->display, PictOpSrc, dest, &white, sb_x, track_y, sb_w, 1);
        XRenderFillRectangle(tv->display, PictOpSrc, dest, &black, sb_x + sb_w - 1, track_y, 1, track_h);
        XRenderFillRectangle(tv->display, PictOpSrc, dest, &black, sb_x, track_y + track_h - 1, sb_w, 1);
        
        // Scrollbar knob (black, 14px wide centered)
        int knob_width = 14;
        int knob_x_offset = (sb_w - knob_width) / 2;
        if (tv->scrollbar_knob_height > 0) {
            XRenderFillRectangle(tv->display, PictOpSrc, dest, &black,
                               sb_x + knob_x_offset, tv->scrollbar_knob_y + track_y,
                               knob_width, tv->scrollbar_knob_height);
        }
        
        // 2. Draw up arrow button
        XRenderFillRectangle(tv->display, PictOpSrc, dest, &gray,
                           sb_x, arrow_up_y, sb_w, TEXTVIEW_ARROW_HEIGHT);
        // Borders
        XRenderFillRectangle(tv->display, PictOpSrc, dest, &white, sb_x, arrow_up_y, 1, TEXTVIEW_ARROW_HEIGHT);
        XRenderFillRectangle(tv->display, PictOpSrc, dest, &white, sb_x, arrow_up_y, sb_w, 1);
        XRenderFillRectangle(tv->display, PictOpSrc, dest, &black, sb_x + sb_w - 1, arrow_up_y, 1, TEXTVIEW_ARROW_HEIGHT);
        XRenderFillRectangle(tv->display, PictOpSrc, dest, &black, sb_x, arrow_up_y + TEXTVIEW_ARROW_HEIGHT - 1, sb_w, 1);
        // Up arrow
        int arrow_offset = (TEXTVIEW_ARROW_HEIGHT - 5) / 2;
        for (int i = 0; i < 5; i++) {
            XRenderFillRectangle(tv->display, PictOpSrc, dest, &black,
                               sb_x + sb_w/2 - i, arrow_up_y + arrow_offset + i, i*2+1, 1);
        }
        
        // 3. Draw down arrow button
        XRenderFillRectangle(tv->display, PictOpSrc, dest, &gray,
                           sb_x, arrow_down_y, sb_w, TEXTVIEW_ARROW_HEIGHT);
        // Borders
        XRenderFillRectangle(tv->display, PictOpSrc, dest, &white, sb_x, arrow_down_y, 1, TEXTVIEW_ARROW_HEIGHT);
        XRenderFillRectangle(tv->display, PictOpSrc, dest, &white, sb_x, arrow_down_y, sb_w, 1);
        XRenderFillRectangle(tv->display, PictOpSrc, dest, &black, sb_x + sb_w - 1, arrow_down_y, 1, TEXTVIEW_ARROW_HEIGHT);
        XRenderFillRectangle(tv->display, PictOpSrc, dest, &black, sb_x, arrow_down_y + TEXTVIEW_ARROW_HEIGHT - 1, sb_w, 1);
        // Down arrow
        for (int i = 0; i < 5; i++) {
            XRenderFillRectangle(tv->display, PictOpSrc, dest, &black,
                               sb_x + sb_w/2 - i, arrow_down_y + arrow_offset + (4-i), i*2+1, 1);
        }
        
        XRenderFreePicture(tv->display, dest);
    }
    
    // Cleanup colors
    XftColorFree(tv->display, DefaultVisual(tv->display, DefaultScreen(tv->display)),
                DefaultColormap(tv->display, DefaultScreen(tv->display)), &fg_color);
    XftColorFree(tv->display, DefaultVisual(tv->display, DefaultScreen(tv->display)),
                DefaultColormap(tv->display, DefaultScreen(tv->display)), &sel_color);
    XftColorFree(tv->display, DefaultVisual(tv->display, DefaultScreen(tv->display)),
                DefaultColormap(tv->display, DefaultScreen(tv->display)), &line_num_color);
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
    
    // Check if click is in scrollbar area - ANY click here must be consumed
    if (tv->scrollbar_visible && event->x >= tv->width - TEXTVIEW_SCROLLBAR_WIDTH) {
        // Fix arrow positions to match draw function
        int arrow_up_y = tv->height - (TEXTVIEW_ARROW_HEIGHT * 2);
        int arrow_down_y = tv->height - TEXTVIEW_ARROW_HEIGHT;
        
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
        } else if (event->y < arrow_up_y) {  // Changed from track_h to arrow_up_y
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
    // Calculate which line was clicked
    int clicked_line = tv->scroll_y + (event->y / tv->line_height);
    if (clicked_line >= tv->line_count) {
        clicked_line = tv->line_count - 1;
    }
    if (clicked_line < 0) clicked_line = 0;
    
    // Calculate column more accurately using font metrics
    int x_start = tv->line_numbers ? tv->line_number_width : 5;
    int clicked_col = 0;
    
    if (event->x > x_start && clicked_line < tv->line_count) {
        // Get the line text
        const char *line = tv->lines[clicked_line];
        int line_len = strlen(line);
        int x_pos = x_start;
        
        // Find which character was clicked
        for (int i = 0; i < line_len; i++) {
            XGlyphInfo extents;
            XftTextExtents8(tv->display, tv->font, (FcChar8*)&line[i], 1, &extents);
            int char_width = extents.xOff;
            
            // Check if click is within this character
            if (event->x < x_pos + char_width / 2) {
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
    textview_draw(tv);
    
    return true;
}

// Handle focus events
bool textview_handle_focus_in(TextView *tv) {
    if (!tv) return false;
    tv->has_focus = true;
    textview_draw(tv);
    return true;
}

bool textview_handle_focus_out(TextView *tv) {
    if (!tv) return false;
    tv->has_focus = false;
    textview_draw(tv);
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
    bool was_dragging = tv->scrollbar_dragging;
    tv->scrollbar_dragging = false;
    return was_dragging;
}

// Handle mouse motion (for selection dragging)
bool textview_handle_motion(TextView *tv, XMotionEvent *event) {
    if (!tv) return false;
    
    // Handle scrollbar dragging
    if (tv->scrollbar_dragging) {
        int track_h = tv->height - (TEXTVIEW_ARROW_HEIGHT * 2);
        int rel_y = event->y - tv->scrollbar_drag_offset;
        int available_space = track_h - tv->scrollbar_knob_height - 2;
        
        if (available_space > 0) {
            // Calculate new scroll position
            int scrollable_lines = tv->line_count - tv->visible_lines;
            tv->scroll_y = (rel_y - 1) * scrollable_lines / available_space;
            
            if (tv->scroll_y < 0) tv->scroll_y = 0;
            if (tv->scroll_y > scrollable_lines) tv->scroll_y = scrollable_lines;
            
            textview_update_scrollbar(tv);
            textview_draw(tv);
        }
        return true;
    }
    
    // Only handle text selection if button is pressed (dragging)
    if (!(event->state & Button1Mask)) return false;
    
    // CRITICAL: Never allow text selection in scrollbar area!
    if (tv->scrollbar_visible && event->x >= tv->width - TEXTVIEW_SCROLLBAR_WIDTH) {
        return true;  // Consume event - no text selection in scrollbar!
    }
    
    // Calculate which line/col we're over
    int motion_line = tv->scroll_y + (event->y / tv->line_height);
    if (motion_line >= tv->line_count) motion_line = tv->line_count - 1;
    if (motion_line < 0) motion_line = 0;
    
    int x_start = tv->line_numbers ? tv->line_number_width : 5;
    int motion_col = 0;
    
    if (event->x > x_start && motion_line < tv->line_count) {
        const char *line = tv->lines[motion_line];
        int line_len = strlen(line);
        int x_pos = x_start;
        
        for (int i = 0; i < line_len; i++) {
            XGlyphInfo extents;
            XftTextExtents8(tv->display, tv->font, (FcChar8*)&line[i], 1, &extents);
            int char_width = extents.xOff;
            
            if (event->x < x_pos + char_width / 2) {
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
    
    // Check if mouse is outside window or near edges
    if (event->y < 0 || event->y < scroll_zone) {
        // Above window or near top - scroll up
        if (tv->scroll_y > 0) {
            // Faster scroll when further from edge
            int speed = (event->y < 0) ? 3 : 1;
            tv->scroll_y -= speed;
            if (tv->scroll_y < 0) tv->scroll_y = 0;
            need_scroll = true;
        }
    } else if (event->y >= tv->height || event->y > tv->height - scroll_zone) {
        // Below window or near bottom - scroll down
        int max_scroll = tv->line_count - tv->visible_lines;
        if (max_scroll < 0) max_scroll = 0;
        if (tv->scroll_y < max_scroll) {
            // Faster scroll when further from edge
            int speed = (event->y >= tv->height) ? 3 : 1;
            tv->scroll_y += speed;
            if (tv->scroll_y > max_scroll) tv->scroll_y = max_scroll;
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
    textview_draw(tv);
}

void textview_move_cursor_page_down(TextView *tv) {
    if (!tv) return;
    tv->cursor_line += tv->visible_lines;
    if (tv->cursor_line >= tv->line_count) tv->cursor_line = tv->line_count - 1;
    int line_len = strlen(tv->lines[tv->cursor_line]);
    if (tv->cursor_col > line_len) tv->cursor_col = line_len;
    if (tv->on_cursor_move) tv->on_cursor_move(tv);
    textview_ensure_cursor_visible(tv);
    textview_draw(tv);
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
    if (tv->line_height > 0) {
        tv->visible_lines = tv->height / tv->line_height;
    }
    
    // Scrollbar is ALWAYS visible (like ReqASL)
    tv->scrollbar_visible = true;
    
    // If content doesn't exceed view, knob fills the track
    if (tv->line_count <= tv->visible_lines) {
        // Full knob when nothing to scroll
        int track_height = tv->height - (TEXTVIEW_ARROW_HEIGHT * 2) - 2;
        tv->scrollbar_knob_height = track_height;
        tv->scrollbar_knob_y = 1;
        return;
    }
    
    // Calculate scrollbar track height (minus arrow buttons)
    int track_height = tv->height - (TEXTVIEW_ARROW_HEIGHT * 2) - 2;
    
    // Calculate knob size proportional to visible vs total
    tv->scrollbar_knob_height = (tv->visible_lines * track_height) / tv->line_count;
    if (tv->scrollbar_knob_height < 20) tv->scrollbar_knob_height = 20; // Minimum knob size
    
    // Calculate knob position
    int scrollable_lines = tv->line_count - tv->visible_lines;
    if (scrollable_lines > 0) {
        int available_space = track_height - tv->scrollbar_knob_height - 2;
        tv->scrollbar_knob_y = 1 + (tv->scroll_y * available_space) / scrollable_lines;
    } else {
        tv->scrollbar_knob_y = 1;
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