#include "inputfield.h"
#include "../amiwb/config.h"
#include <stdlib.h>
#include <string.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>
#include <X11/Xatom.h>

InputField* inputfield_create(int x, int y, int width, int height) {
    InputField *field = malloc(sizeof(InputField));
    if (!field) {
        printf("[ERROR] malloc failed for InputField structure (size=%zu)\n", sizeof(InputField));
        return NULL;
    }
    
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
    field->disabled = false;  // Initialize disabled state
    field->on_enter = NULL;
    field->on_change = NULL;
    field->user_data = NULL;
    
    return field;
}

void inputfield_destroy(InputField *field) {
    if (!field) {
        return;  // Not fatal - cleanup function
    }
    free(field);
}

void inputfield_set_text(InputField *field, const char *text) {
    if (!field || !text) {
        return;
    }
    
    strncpy(field->text, text, INPUTFIELD_MAX_LENGTH);
    field->text[INPUTFIELD_MAX_LENGTH] = '\0';
    field->cursor_pos = strlen(field->text);
    field->selection_start = -1;
    field->selection_end = -1;
    
    // Reset visible_start to force recalculation to show rightmost part
    field->visible_start = 0;
    
    if (field->on_change) {
        field->on_change(field->text, field->user_data);
    }
}

const char* inputfield_get_text(InputField *field) {
    if (!field) {
        return NULL;
    }
    return field->text;
}

void inputfield_set_callbacks(InputField *field, 
                             void (*on_enter)(const char*, void*),
                             void (*on_change)(const char*, void*),
                             void *user_data) {
    if (!field) {
        return;
    }
    field->on_enter = on_enter;
    field->on_change = on_change;
    field->user_data = user_data;
}

void inputfield_set_focus(InputField *field, bool has_focus) {
    if (!field) {
        return;
    }
    field->has_focus = has_focus;
    if (has_focus) {
        // When getting focus, position cursor at end of text
        field->cursor_pos = strlen(field->text);
    } else {
        field->selection_start = -1;
        field->selection_end = -1;
    }
}

void inputfield_draw(InputField *field, Picture dest, Display *dpy, XftDraw *xft_draw, XftFont *font) {
    if (!field || !dpy || dest == None) {
        return;
    }
    
    int x = field->x;
    int y = field->y;
    int w = field->width;
    int h = field->height;
    
    // Draw inset box with proper Amiga-style borders
    // Inner lines should never overwrite outer lines
    XRenderColor white = WHITE;
    XRenderColor black = BLACK;
    XRenderColor gray = GRAY;
    
    // Outer white border (left and top)
    XRenderFillRectangle(dpy, PictOpSrc, dest, &white, x, y, 1, h);         // Left white
    XRenderFillRectangle(dpy, PictOpSrc, dest, &white, x, y, w, 1);         // Top white
    
    // Inner black border (left and top) - avoiding corners to prevent overlap
    XRenderFillRectangle(dpy, PictOpSrc, dest, &black, x+1, y+1, 1, h-2);   // Left black (skip corners)
    XRenderFillRectangle(dpy, PictOpSrc, dest, &black, x+1, y+1, w-2, 1);   // Top black (skip corners)
    
    // Inner white border (right and bottom) - positioned to not overlap
    XRenderFillRectangle(dpy, PictOpSrc, dest, &white, x+w-2, y+1, 1, h-2); // Right white (skip corners)
    XRenderFillRectangle(dpy, PictOpSrc, dest, &white, x+1, y+h-2, w-2, 1); // Bottom white (skip corners)
    
    // Outer black border (right and bottom)
    XRenderFillRectangle(dpy, PictOpSrc, dest, &black, x+w-1, y, 1, h);     // Right black
    XRenderFillRectangle(dpy, PictOpSrc, dest, &black, x, y+h-1, w, 1);     // Bottom black
    
    // Fill input area - checker pattern if disabled, solid gray if enabled
    if (field->disabled) {
        // First fill the 2-pixel padding area with gray
        XRenderFillRectangle(dpy, PictOpSrc, dest, &gray, x+2, y+2, w-4, h-4);
        
        // Draw checker pattern like workbench inactive scrollbar - 2x2 blocks
        // Fill with 2x2 alternating blocks, with 2px padding from borders
        for (int py = y + 4; py < y + h - 4; py += 2) {
            for (int px = x + 4; px < x + w - 4; px += 2) {
                // Determine if this 2x2 block should be gray or black
                // Based on block position (not pixel position)
                int block_x = (px - x - 4) / 2;
                int block_y = (py - y - 4) / 2;
                
                // Alternate blocks: when block x+y is even, gray; odd, black
                if ((block_x + block_y) % 2 == 0) {
                    // Gray block - already filled, skip
                } else {
                    // Black block
                    XRenderFillRectangle(dpy, PictOpSrc, dest, &black, px, py,
                                       (px + 2 <= x + w - 4) ? 2 : (x + w - 4 - px),
                                       (py + 2 <= y + h - 4) ? 2 : (y + h - 4 - py));
                }
            }
        }
    } else {
        // Normal gray fill for enabled field
        XRenderFillRectangle(dpy, PictOpSrc, dest, &gray, x+2, y+2, w-4, h-4);
    }
    
    // Draw text content with cursor if we have a font
    if (font && xft_draw) {
        // Set up text colors
        XftColor black_color, white_color;
        XRenderColor black = {0x0000, 0x0000, 0x0000, 0xffff};
        XRenderColor white = {0xffff, 0xffff, 0xffff, 0xffff};
        XRenderColor blue = {0x4858, 0x6F6F, 0xB0B0, 0xFFFF};  // From config.h
        
        XftColorAllocValue(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                          DefaultColormap(dpy, DefaultScreen(dpy)),
                          &black, &black_color);
        XftColorAllocValue(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                          DefaultColormap(dpy, DefaultScreen(dpy)),
                          &white, &white_color);
        
        // Calculate text position (vertically centered, left-aligned with padding)
        int text_x = x + 8;  // More padding to prevent border touching
        int text_y = y + (h + font->ascent - font->descent) / 2;
        int available_width = w - 16;  // Leave padding on both sides
        
        int text_len = strlen(field->text);
        
        // Handle empty text with cursor (but not if disabled)
        if (text_len == 0 && field->has_focus && field->cursor_pos == 0 && !field->disabled) {
            // Draw blue rectangle cursor the size of a space
            XGlyphInfo space_info;
            XftTextExtentsUtf8(dpy, font, (FcChar8*)" ", 1, &space_info);
            // Use minimum width of 8 pixels if space has no width
            int cursor_width = space_info.width > 0 ? space_info.width : 8;
            XRenderFillRectangle(dpy, PictOpSrc, dest, &blue,
                               text_x, y + 3, cursor_width, h - 6);
        } else if (text_len > 0) {
            // Calculate visible text range
            int visible_start = 0;
            XGlyphInfo text_extents;
            XGlyphInfo space_info;
            XftTextExtentsUtf8(dpy, font, (FcChar8*)" ", 1, &space_info);
            
            // First, check if the entire text fits
            XftTextExtentsUtf8(dpy, font, (FcChar8*)field->text, text_len, &text_extents);
            
            if (text_extents.width > available_width) {
                // Text doesn't fit - we need to scroll
                
                if (field->has_focus && !field->disabled) {
                    // When focused, keep cursor in view
                    visible_start = field->visible_start;
                    
                    if (field->cursor_pos >= visible_start) {
                        // Measure from visible_start to cursor position
                        int chars_to_measure = field->cursor_pos - visible_start;
                        if (chars_to_measure > 0) {
                            XftTextExtentsUtf8(dpy, font, 
                                             (FcChar8*)&field->text[visible_start],
                                             chars_to_measure, &text_extents);
                        } else {
                            text_extents.width = 0;
                        }
                        
                        // Add space for cursor if at end of text
                        int total_width = text_extents.width;
                        if (field->cursor_pos == text_len) {
                            total_width += space_info.width;
                        }
                        
                        // If cursor is beyond visible area, scroll right
                        while (total_width > available_width && visible_start < field->cursor_pos) {
                            visible_start++;
                            if (field->cursor_pos > visible_start) {
                                XftTextExtentsUtf8(dpy, font,
                                                 (FcChar8*)&field->text[visible_start],
                                                 field->cursor_pos - visible_start, &text_extents);
                                total_width = text_extents.width;
                                if (field->cursor_pos == text_len) {
                                    total_width += space_info.width;
                                }
                            }
                        }
                    } else if (field->cursor_pos < visible_start) {
                        // Cursor is before visible area, scroll left
                        visible_start = field->cursor_pos;
                    }
                } else {
                    // When not focused, show the rightmost part of the text
                    // Start from the end and work backwards until it fits
                    visible_start = text_len;
                    
                    while (visible_start > 0) {
                        XftTextExtentsUtf8(dpy, font, 
                                         (FcChar8*)&field->text[visible_start],
                                         text_len - visible_start, &text_extents);
                        if (text_extents.width <= available_width) {
                            // This portion fits, try including one more character
                            if (visible_start > 0) {
                                XftTextExtentsUtf8(dpy, font, 
                                                 (FcChar8*)&field->text[visible_start - 1],
                                                 text_len - visible_start + 1, &text_extents);
                                if (text_extents.width <= available_width) {
                                    visible_start--;
                                } else {
                                    break;  // Can't fit any more
                                }
                            }
                        } else {
                            visible_start++;  // Too much, go back one
                            break;
                        }
                    }
                }
            }
            
            field->visible_start = visible_start;
            
            // Draw text character by character
            int draw_x = text_x;
            for (int i = visible_start; i < text_len; i++) {
                // Check if we've exceeded available width
                if (draw_x - text_x >= available_width) break;
                
                char ch[2] = {field->text[i], '\0'};
                XGlyphInfo glyph_info;
                XftTextExtentsUtf8(dpy, font, (FcChar8*)ch, 1, &glyph_info);
                
                // Check if this is the cursor position
                bool is_cursor = (field->has_focus && !field->disabled && i == field->cursor_pos);
                
                if (is_cursor) {
                    // Draw blue background for cursor
                    XRenderFillRectangle(dpy, PictOpSrc, dest, &blue,
                                       draw_x, y + 3, glyph_info.width, h - 6);
                    // Draw white text on blue background
                    XftDrawStringUtf8(xft_draw, &white_color, font,
                                    draw_x, text_y, (FcChar8*)ch, 1);
                } else {
                    // Draw black text normally
                    XftDrawStringUtf8(xft_draw, &black_color, font,
                                    draw_x, text_y, (FcChar8*)ch, 1);
                }
                
                draw_x += glyph_info.width;
            }
            
            // Draw cursor at end if it's past the last character
            if (field->has_focus && !field->disabled && field->cursor_pos == text_len) {
                XGlyphInfo space_info;
                XftTextExtentsUtf8(dpy, font, (FcChar8*)" ", 1, &space_info);
                // Use minimum width of 8 pixels if space has no width
                int cursor_width = space_info.width > 0 ? space_info.width : 8;
                // Add 1 pixel of padding before cursor when at end of text
                int cursor_x = draw_x + (text_len > 0 ? 1 : 0);
                // Always draw the cursor at the end when field has focus
                XRenderFillRectangle(dpy, PictOpSrc, dest, &blue,
                                   cursor_x, y + 3, cursor_width, h - 6);
            }
        }
        
        XftColorFree(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                    DefaultColormap(dpy, DefaultScreen(dpy)), &black_color);
        XftColorFree(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                    DefaultColormap(dpy, DefaultScreen(dpy)), &white_color);
    }
}

bool inputfield_handle_click(InputField *field, int click_x, int click_y) {
    if (!field) {
        return false;
    }
    
    // Disabled fields cannot receive focus
    if (field->disabled) {
        return false;
    }
    
    if (click_x >= field->x && click_x < field->x + field->width &&
        click_y >= field->y && click_y < field->y + field->height) {
        field->has_focus = true;
        // Always position cursor at end when field gets focus from click
        field->cursor_pos = strlen(field->text);
        // Reset visible_start to show the end of the text
        field->visible_start = 0;  // Will be adjusted in draw to show cursor
        return true;
    }
    
    field->has_focus = false;
    return false;
}

bool inputfield_handle_key(InputField *field, XKeyEvent *event) {
    if (!field) {
        return false;
    }
    if (!event) {
        return false;
    }
    
    // Disabled fields cannot handle keyboard input
    if (field->disabled) {
        return false;
    }
    if (!field->has_focus) return false;  // Not an error - field doesn't have focus
    
    KeySym keysym = XLookupKeysym(event, 0);
    
    // Handle Ctrl+C for copy
    if ((event->state & ControlMask) && (keysym == XK_c || keysym == XK_C)) {
        // Copy entire field content to clipboard
        Display *dpy = event->display;
        Window root = DefaultRootWindow(dpy);
        
        // Set the clipboard (CLIPBOARD selection)
        Atom clipboard = XInternAtom(dpy, "CLIPBOARD", False);
        XSetSelectionOwner(dpy, clipboard, root, CurrentTime);
        
        // Also set PRIMARY selection for middle-click paste
        XSetSelectionOwner(dpy, XA_PRIMARY, root, CurrentTime);
        
        // Store the text in a property
        Atom utf8_string = XInternAtom(dpy, "UTF8_STRING", False);
        
        XChangeProperty(dpy, root, clipboard,
                       utf8_string, 8, PropModeReplace,
                       (unsigned char *)field->text, strlen(field->text));
        
        XChangeProperty(dpy, root, XA_PRIMARY,
                       XA_STRING, 8, PropModeReplace,
                       (unsigned char *)field->text, strlen(field->text));
        
        XFlush(dpy);
        return true;
    }
    
    // Handle Ctrl+V for paste
    if ((event->state & ControlMask) && (keysym == XK_v || keysym == XK_V)) {
        Display *dpy = event->display;
        Window root = DefaultRootWindow(dpy);
        
        // Try CLIPBOARD first, then PRIMARY
        Atom clipboard = XInternAtom(dpy, "CLIPBOARD", False);
        Atom utf8_string = XInternAtom(dpy, "UTF8_STRING", False);
        
        Window owner = XGetSelectionOwner(dpy, clipboard);
        if (owner == None) {
            // Try PRIMARY selection as fallback
            owner = XGetSelectionOwner(dpy, XA_PRIMARY);
            clipboard = XA_PRIMARY;
        }
        
        if (owner != None) {
            // Request the selection
            Atom actual_type;
            int actual_format;
            unsigned long nitems, bytes_after;
            unsigned char *data = NULL;
            
            // Try to get UTF8_STRING first
            if (XGetWindowProperty(dpy, root, clipboard,
                                 0, INPUTFIELD_MAX_LENGTH, False,
                                 utf8_string, &actual_type, &actual_format,
                                 &nitems, &bytes_after, &data) == Success) {
                if (data) {
                    // Clear current text and set to clipboard content
                    strncpy(field->text, (char *)data, INPUTFIELD_MAX_LENGTH);
                    field->text[INPUTFIELD_MAX_LENGTH] = '\0';
                    field->cursor_pos = strlen(field->text);
                    field->visible_start = 0;  // Reset scroll
                    XFree(data);
                    
                    if (field->on_change) {
                        field->on_change(field->text, field->user_data);
                    }
                    return true;
                }
            }
            
            // Fallback to XA_STRING
            if (XGetWindowProperty(dpy, root, clipboard,
                                 0, INPUTFIELD_MAX_LENGTH, False,
                                 XA_STRING, &actual_type, &actual_format,
                                 &nitems, &bytes_after, &data) == Success) {
                if (data) {
                    // Clear current text and set to clipboard content
                    strncpy(field->text, (char *)data, INPUTFIELD_MAX_LENGTH);
                    field->text[INPUTFIELD_MAX_LENGTH] = '\0';
                    field->cursor_pos = strlen(field->text);
                    field->visible_start = 0;  // Reset scroll
                    XFree(data);
                    
                    if (field->on_change) {
                        field->on_change(field->text, field->user_data);
                    }
                    return true;
                }
            }
        }
        return true;
    }
    
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
    if (!field) {
        return;
    }
    
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
    if (!field) {
        return;
    }
    
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
    if (!field) {
        return;
    }
    
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
    if (!field) {
        return;
    }
    
    int new_pos = field->cursor_pos + delta;
    int len = strlen(field->text);
    
    if (new_pos < 0) new_pos = 0;
    if (new_pos > len) new_pos = len;
    
    field->cursor_pos = new_pos;
    field->selection_start = -1;
    field->selection_end = -1;
}

void inputfield_scroll_to_end(InputField *field) {
    if (!field) {
        return;
    }
    
    // Set cursor to end of text
    int len = strlen(field->text);
    field->cursor_pos = len;
    
    // Reset visible_start so draw function will recalculate to show the end
    field->visible_start = 0;
}

void inputfield_update_size(InputField *field, int new_width) {
    if (!field || new_width <= 0) {
        return;
    }
    
    field->width = new_width;
    
    // Reset visible_start to trigger recalculation in draw
    // This will ensure the rightmost part stays visible
    field->visible_start = 0;
}

void inputfield_set_disabled(InputField *field, bool disabled) {
    if (!field) {
        return;
    }
    
    field->disabled = disabled;
    
    // If disabling, remove focus
    if (disabled) {
        field->has_focus = false;
    }
}