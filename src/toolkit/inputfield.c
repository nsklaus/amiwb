#include "inputfield.h"
#include <stdlib.h>
#include <string.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>

// AmigaOS colors
static XRenderColor GRAY  = {0x9999, 0x9999, 0x9999, 0xffff};
static XRenderColor WHITE = {0xffff, 0xffff, 0xffff, 0xffff};
static XRenderColor BLACK = {0x0000, 0x0000, 0x0000, 0xffff};
static XRenderColor DARK  = {0x5555, 0x5555, 0x5555, 0xffff};
// BLUE will be used for cursor caret in rename dialog
// static XRenderColor BLUE  = {0x5555, 0x8888, 0xffff, 0xffff};

InputField* inputfield_create(int x, int y, int width, int height) {
    InputField *field = malloc(sizeof(InputField));
    if (!field) return NULL;
    
    field->x = x;
    field->y = y;
    field->width = width;
    field->height = height;
    field->text[0] = '\0';
    field->cursor_pos = 0;
    field->selection_start = -1;
    field->selection_end = -1;
    field->visible_start = 0;
    field->has_focus = false;
    field->on_enter = NULL;
    field->on_change = NULL;
    field->user_data = NULL;
    
    return field;
}

void inputfield_destroy(InputField *field) {
    if (!field) return;
    free(field);
}

void inputfield_set_text(InputField *field, const char *text) {
    if (!field || !text) return;
    
    strncpy(field->text, text, INPUTFIELD_MAX_LENGTH);
    field->text[INPUTFIELD_MAX_LENGTH] = '\0';
    field->cursor_pos = strlen(field->text);
    field->selection_start = -1;
    field->selection_end = -1;
    
    if (field->on_change) {
        field->on_change(field->text, field->user_data);
    }
}

const char* inputfield_get_text(InputField *field) {
    return field ? field->text : "";
}

void inputfield_set_callbacks(InputField *field, 
                             void (*on_enter)(const char*, void*),
                             void (*on_change)(const char*, void*),
                             void *user_data) {
    if (!field) return;
    field->on_enter = on_enter;
    field->on_change = on_change;
    field->user_data = user_data;
}

void inputfield_set_focus(InputField *field, bool has_focus) {
    if (!field) return;
    field->has_focus = has_focus;
    if (!has_focus) {
        field->selection_start = -1;
        field->selection_end = -1;
    }
}

void inputfield_draw(InputField *field, Picture dest, Display *dpy, XftDraw *xft_draw, XftFont *font) {
    if (!field || !dpy) return;
    
    int x = field->x;
    int y = field->y;
    int w = field->width;
    int h = field->height;
    
    // Draw inset box (input field appearance)
    XRenderFillRectangle(dpy, PictOpSrc, dest, &DARK, x, y, w, h);          // Outer border
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, x+1, y+1, 1, h-2);   // Left inner (black)
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, x+1, y+1, w-2, 1);   // Top inner (black)
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, x+w-2, y+1, 1, h-2); // Right inner (white)
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, x+1, y+h-2, w-2, 1); // Bottom inner (white)
    
    // Gray fill for input area
    XRenderFillRectangle(dpy, PictOpSrc, dest, &GRAY, x+2, y+2, w-4, h-4);
    
    // Draw text content with cursor if we have a font
    if (font && xft_draw && field->text[0] != '\0') {
        // Set up text color (black on gray background)
        XftColor text_color;
        XRenderColor black = {0x0000, 0x0000, 0x0000, 0xffff};
        XftColorAllocValue(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                          DefaultColormap(dpy, DefaultScreen(dpy)),
                          &black, &text_color);
        
        // Calculate text position (vertically centered, left-aligned with padding)
        int text_x = x + 4;
        int text_y = y + (h + font->ascent - font->descent) / 2;
        
        // Draw the text
        XftDrawStringUtf8(xft_draw, &text_color, font,
                         text_x, text_y,
                         (FcChar8*)field->text, strlen(field->text));
        
        // Draw cursor if field has focus
        if (field->has_focus && field->cursor_pos >= 0) {
            // Calculate cursor position
            XGlyphInfo extents;
            if (field->cursor_pos > 0) {
                XftTextExtentsUtf8(dpy, font, (FcChar8*)field->text, 
                                  field->cursor_pos, &extents);
                text_x += extents.width;
            }
            
            // Draw cursor line
            XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK,
                               text_x, y + 3, 1, h - 6);
        }
        
        XftColorFree(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                    DefaultColormap(dpy, DefaultScreen(dpy)), &text_color);
    }
}

bool inputfield_handle_click(InputField *field, int click_x, int click_y) {
    if (!field) return false;
    
    if (click_x >= field->x && click_x < field->x + field->width &&
        click_y >= field->y && click_y < field->y + field->height) {
        field->has_focus = true;
        // TODO: Calculate cursor position from click position
        return true;
    }
    
    field->has_focus = false;
    return false;
}

bool inputfield_handle_key(InputField *field, XKeyEvent *event) {
    if (!field || !field->has_focus) return false;
    
    KeySym keysym = XLookupKeysym(event, 0);
    
    // Handle special keys
    switch (keysym) {
        case XK_Return:
        case XK_KP_Enter:
            if (field->on_enter) {
                field->on_enter(field->text, field->user_data);
            }
            return true;
            
        case XK_BackSpace:
            inputfield_backspace(field);
            return true;
            
        case XK_Delete:
        case XK_KP_Delete:
            inputfield_delete_char(field);
            return true;
            
        case XK_Left:
        case XK_KP_Left:
            inputfield_move_cursor(field, -1);
            return true;
            
        case XK_Right:
        case XK_KP_Right:
            inputfield_move_cursor(field, 1);
            return true;
            
        case XK_Home:
        case XK_KP_Home:
            field->cursor_pos = 0;
            return true;
            
        case XK_End:
        case XK_KP_End:
            field->cursor_pos = strlen(field->text);
            return true;
    }
    
    // Handle regular text input
    char buffer[32];
    KeySym keysym_ignored;
    int char_count = XLookupString(event, buffer, sizeof(buffer)-1, &keysym_ignored, NULL);
    
    if (char_count > 0) {
        buffer[char_count] = '\0';
        for (int i = 0; i < char_count; i++) {
            if (buffer[i] >= 32 && buffer[i] < 127) {  // Printable ASCII
                inputfield_insert_char(field, buffer[i]);
            }
        }
        return true;
    }
    
    return false;
}

void inputfield_insert_char(InputField *field, char c) {
    if (!field) return;
    
    int len = strlen(field->text);
    if (len >= INPUTFIELD_MAX_LENGTH) return;
    
    // Delete selection if any
    if (field->selection_start >= 0 && field->selection_end > field->selection_start) {
        int sel_len = field->selection_end - field->selection_start;
        (void)sel_len;  // Currently unused but may be needed for future enhancements
        memmove(&field->text[field->selection_start], 
                &field->text[field->selection_end],
                len - field->selection_end + 1);
        field->cursor_pos = field->selection_start;
        field->selection_start = -1;
        field->selection_end = -1;
        len = strlen(field->text);
    }
    
    // Insert character at cursor position
    if (field->cursor_pos < len) {
        memmove(&field->text[field->cursor_pos + 1],
                &field->text[field->cursor_pos],
                len - field->cursor_pos + 1);
    }
    field->text[field->cursor_pos] = c;
    field->cursor_pos++;
    field->text[len + 1] = '\0';
    
    if (field->on_change) {
        field->on_change(field->text, field->user_data);
    }
}

void inputfield_delete_char(InputField *field) {
    if (!field) return;
    
    int len = strlen(field->text);
    if (field->cursor_pos < len) {
        memmove(&field->text[field->cursor_pos],
                &field->text[field->cursor_pos + 1],
                len - field->cursor_pos);
        
        if (field->on_change) {
            field->on_change(field->text, field->user_data);
        }
    }
}

void inputfield_backspace(InputField *field) {
    if (!field) return;
    
    if (field->cursor_pos > 0) {
        int len = strlen(field->text);
        memmove(&field->text[field->cursor_pos - 1],
                &field->text[field->cursor_pos],
                len - field->cursor_pos + 1);
        field->cursor_pos--;
        
        if (field->on_change) {
            field->on_change(field->text, field->user_data);
        }
    }
}

void inputfield_move_cursor(InputField *field, int delta) {
    if (!field) return;
    
    int new_pos = field->cursor_pos + delta;
    int len = strlen(field->text);
    
    if (new_pos < 0) new_pos = 0;
    if (new_pos > len) new_pos = len;
    
    field->cursor_pos = new_pos;
    field->selection_start = -1;
    field->selection_end = -1;
}