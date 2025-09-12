#include "inputfield.h"
#include "../amiwb/config.h"
#include <stdlib.h>
#include <string.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xrender.h>
#include <fontconfig/fontconfig.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>

// Forward declaration for path completion dropdown rendering
static void draw_completion_dropdown(InputField *field, Display *dpy);

InputField* inputfield_create(int x, int y, int width, int height, XftFont *font) {
    InputField *field = malloc(sizeof(InputField));
    if (!field) {
        fprintf(stderr, "[ERROR] InputField: Failed to allocate memory (size=%zu)\n", sizeof(InputField));
        return NULL;
    }
    
    field->x = x;
    field->y = y;
    field->width = width;
    field->height = height;
    field->text[0] = '\0';
    field->name[0] = '\0';  // Initialize name as empty
    field->cursor_pos = 0;
    field->selection_start = -1;
    field->selection_end = -1;
    field->visible_start = 0;
    field->has_focus = false;
    field->disabled = false;  // Initialize disabled state
    field->readonly = false;  // Initialize readonly state
    field->on_enter = NULL;
    field->on_change = NULL;
    field->user_data = NULL;
    field->font = font;  // Store the font pointer (borrowed from app)
    
    // Initialize path completion fields
    field->enable_path_completion = false;
    field->completion_base_dir[0] = '\0';  // Initialize base dir as empty
    field->completion_window = 0;
    field->dropdown_open = false;
    field->completion_candidates = NULL;
    field->completion_count = 0;
    field->completion_selected = -1;
    field->completion_prefix[0] = '\0';
    field->completion_prefix_len = 0;
    
    return field;
}

void inputfield_destroy(InputField *field) {
    if (!field) {
        return;  // Not fatal - cleanup function
    }
    
    // Clean up completion resources
    if (field->completion_candidates) {
        for (int i = 0; i < field->completion_count; i++) {
            free(field->completion_candidates[i]);
        }
        free(field->completion_candidates);
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
    
    // Fill input area - checker pattern if disabled, solid gray if enabled or readonly
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
        // Normal gray fill for enabled or readonly field
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
            
            // Draw text in chunks for proper font rendering
            
            // Draw text before cursor
            if (field->cursor_pos > visible_start) {
                int chars_before = field->cursor_pos - visible_start;
                XftDrawStringUtf8(xft_draw, &black_color, font, text_x, text_y,
                                 (FcChar8*)&field->text[visible_start], chars_before);
            }
            
            // Draw cursor character with blue background (if not at end)
            if (field->has_focus && !field->disabled && field->cursor_pos < text_len) {
                // Measure position where cursor starts
                XGlyphInfo before_info;
                if (field->cursor_pos > visible_start) {
                    XftTextExtentsUtf8(dpy, font, (FcChar8*)&field->text[visible_start], 
                                      field->cursor_pos - visible_start, &before_info);
                } else {
                    before_info.width = 0;
                }
                int cursor_x = text_x + before_info.width;
                
                // Draw blue background and white character
                char cursor_ch[2] = {field->text[field->cursor_pos], '\0'};
                XGlyphInfo cursor_info;
                XftTextExtentsUtf8(dpy, font, (FcChar8*)cursor_ch, 1, &cursor_info);
                XRenderFillRectangle(dpy, PictOpSrc, dest, &blue,
                                    cursor_x, y + 3, cursor_info.width, h - 6);
                XftDrawStringUtf8(xft_draw, &white_color, font,
                                 cursor_x, text_y, (FcChar8*)cursor_ch, 1);
            }
            
            // Draw text after cursor
            if (field->cursor_pos + 1 <= text_len) {
                // Measure where to start
                XGlyphInfo before_info;
                int chars_to_skip = field->cursor_pos + 1 - visible_start;
                if (chars_to_skip > 0) {
                    XftTextExtentsUtf8(dpy, font, (FcChar8*)&field->text[visible_start],
                                      chars_to_skip, &before_info);
                } else {
                    before_info.width = 0;
                }
                int after_x = text_x + before_info.width;
                
                // Only draw if there's text after cursor
                if (field->cursor_pos + 1 < text_len) {
                    XftDrawStringUtf8(xft_draw, &black_color, font, after_x, text_y,
                                     (FcChar8*)&field->text[field->cursor_pos + 1],
                                     text_len - field->cursor_pos - 1);
                }
            }
            
            // Draw cursor at end if it's past the last character
            if (field->has_focus && !field->disabled && field->cursor_pos == text_len) {
                // Measure position of cursor at end
                XGlyphInfo full_info;
                if (text_len > visible_start) {
                    XftTextExtentsUtf8(dpy, font, (FcChar8*)&field->text[visible_start],
                                      text_len - visible_start, &full_info);
                } else {
                    full_info.width = 0;
                }
                
                XGlyphInfo space_info;
                XftTextExtentsUtf8(dpy, font, (FcChar8*)" ", 1, &space_info);
                // Use minimum width of 8 pixels if space has no width
                int cursor_width = space_info.width > 0 ? space_info.width : 8;
                // Add 1 pixel of padding before cursor when at end of text
                int cursor_x = text_x + full_info.width + (text_len > 0 ? 1 : 0);
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
    
    // Don't remove focus here - let the caller handle that
    // field->has_focus = false;  // REMOVED - caller should manage focus
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
    
    // Handle Super+C for copy (Amiga style)
    if ((event->state & Mod4Mask) && (keysym == XK_c || keysym == XK_C)) {
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
    
    // Handle Super+V for paste (Amiga style)
    if ((event->state & Mod4Mask) && (keysym == XK_v || keysym == XK_V)) {
        // Block paste if readonly
        if (field->readonly) {
            return true;  // Consume event but don't paste
        }
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
            // If completion dropdown is visible, apply selected completion
            if (field->dropdown_open && field->completion_count > 0) {
                inputfield_apply_completion(field, field->completion_selected);
                inputfield_hide_completions(field, event->display);
                return true;
            }
            // Otherwise, normal Enter behavior
            if (field->on_enter) {
                field->on_enter(field->text, field->user_data);
            }
            // Automatically lose focus when Enter is pressed
            field->has_focus = false;
            return true;
            
        case XK_BackSpace:
            if (!field->readonly) {
                inputfield_backspace(field);
            }
            return true;
            
        case XK_Delete:
        case XK_KP_Delete:
            if (!field->readonly) {
                inputfield_delete_char(field);
            }
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
            
        case XK_Tab:
            // Trigger path completion if enabled
            if (field->enable_path_completion && !field->readonly) {
                // Get the parent window from the event
                Window parent = event->window;
                inputfield_show_completions(field, event->display, parent);
                return true;
            }
            break;
            
        case XK_Escape:
            // Hide completion dropdown if visible
            if (field->dropdown_open) {
                inputfield_hide_completions(field, event->display);
                return true;
            }
            break;
            
        case XK_Up:
        case XK_KP_Up:
            // Navigate completion dropdown up
            if (field->dropdown_open && field->completion_count > 0) {
                if (field->completion_selected > 0) {
                    field->completion_selected--;
                    draw_completion_dropdown(field, event->display);
                }
                return true;
            }
            break;
            
        case XK_Down:
        case XK_KP_Down:
            // Navigate completion dropdown down
            if (field->dropdown_open && field->completion_count > 0) {
                if (field->completion_selected < field->completion_count - 1) {
                    field->completion_selected++;
                    draw_completion_dropdown(field, event->display);
                }
                return true;
            }
            break;
    }
    
    // Handle regular text input (blocked if readonly)
    if (!field->readonly) {
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

void inputfield_set_readonly(InputField *field, bool readonly) {
    if (!field) {
        return;
    }
    
    field->readonly = readonly;
}

// ========== Path Completion Functions ==========

// Enable or disable path completion
void inputfield_enable_path_completion(InputField *field, bool enable) {
    if (!field) return;
    field->enable_path_completion = enable;
    
    // If disabling, clean up any active completion
    if (!enable && field->completion_window) {
        inputfield_hide_completions(field, NULL);
    }
}

// Set base directory for completion (for File field to use Path field's directory)
void inputfield_set_completion_base_dir(InputField *field, const char *dir) {
    if (!field) return;
    
    if (dir && dir[0] != '\0') {
        strncpy(field->completion_base_dir, dir, PATH_SIZE - 1);
        field->completion_base_dir[PATH_SIZE - 1] = '\0';
        
        // Ensure it ends with /
        size_t len = strlen(field->completion_base_dir);
        if (len > 0 && len < PATH_SIZE - 1 && field->completion_base_dir[len - 1] != '/') {
            field->completion_base_dir[len] = '/';
            field->completion_base_dir[len + 1] = '\0';
        }
    } else {
        field->completion_base_dir[0] = '\0';
    }
}

// Free completion candidates
static void free_completion_candidates(InputField *field) {
    if (field->completion_candidates) {
        for (int i = 0; i < field->completion_count; i++) {
            free(field->completion_candidates[i]);
        }
        free(field->completion_candidates);
        field->completion_candidates = NULL;
        field->completion_count = 0;
        field->completion_selected = -1;
    }
}

// Compare function for sorting completions
static int completion_compare(const void *a, const void *b) {
    return strcasecmp(*(const char **)a, *(const char **)b);
}

// Expand tilde to home directory
static char *expand_tilde(const char *path) {
    if (path[0] != '~') return strdup(path);
    
    const char *home = getenv("HOME");
    if (!home) return strdup(path);
    
    if (path[1] == '\0' || path[1] == '/') {
        // ~/... or just ~
        size_t home_len = strlen(home);
        size_t path_len = strlen(path + 1);
        char *result = malloc(home_len + path_len + 1);
        if (!result) {
            fprintf(stderr, "[ERROR] InputField: Failed to allocate memory for path expansion\n");
            return strdup(path);
        }
        
        snprintf(result, home_len + path_len + 1, "%s%s", home, path + 1);
        return result;
    }
    
    return strdup(path);
}

// Find completions for the given partial path
static void find_completions(InputField *field, const char *partial) {
    free_completion_candidates(field);
    
    // Expand tilde if present
    char *expanded = expand_tilde(partial);
    
    // Split into directory and prefix
    char dir_path[PATH_SIZE];
    char prefix[NAME_SIZE];
    
    char *last_slash = strrchr(expanded, '/');
    if (last_slash) {
        // Has directory component
        size_t dir_len = last_slash - expanded + 1;
        if (dir_len >= PATH_SIZE) {
            fprintf(stderr, "[ERROR] InputField: Directory path too long\n");
            free(expanded);
            return;
        }
        strncpy(dir_path, expanded, dir_len);
        dir_path[dir_len] = '\0';
        
        strncpy(prefix, last_slash + 1, NAME_SIZE - 1);
        prefix[NAME_SIZE - 1] = '\0';
    } else {
        // No directory, use base dir if set, otherwise current directory
        if (field->completion_base_dir[0] != '\0') {
            strncpy(dir_path, field->completion_base_dir, PATH_SIZE - 1);
            dir_path[PATH_SIZE - 1] = '\0';
        } else {
            strncpy(dir_path, "./", PATH_SIZE - 1);
            dir_path[PATH_SIZE - 1] = '\0';
        }
        
        strncpy(prefix, expanded, NAME_SIZE - 1);
        prefix[NAME_SIZE - 1] = '\0';
    }
    
    // Expand the directory path
    char *expanded_dir = expand_tilde(dir_path);
    
    // Open directory
    DIR *dir = opendir(expanded_dir);
    if (!dir) {
        free(expanded);
        free(expanded_dir);
        return;
    }
    
    // Allocate initial space for candidates
    int capacity = 16;
    field->completion_candidates = malloc(capacity * sizeof(char *));
    field->completion_count = 0;
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        // Skip . and ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        
        // Check if name starts with prefix (case-insensitive)
        if (strncasecmp(entry->d_name, prefix, strlen(prefix)) == 0) {
            // Grow array if needed
            if (field->completion_count >= capacity) {
                capacity *= 2;
                field->completion_candidates = realloc(field->completion_candidates,
                                                      capacity * sizeof(char *));
            }
            
            // Check if it's a directory
            char full_path[PATH_SIZE];
            int ret = snprintf(full_path, sizeof(full_path), "%s%s", expanded_dir, entry->d_name);
            if (ret >= PATH_SIZE) {
                fprintf(stderr, "[WARNING] InputField: Path too long for completion, skipping: %s%s\n", expanded_dir, entry->d_name);
                continue;
            }
            struct stat st;
            bool is_dir = (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode));
            
            // Add to candidates (with / suffix for directories)
            if (is_dir) {
                field->completion_candidates[field->completion_count] = 
                    malloc(strlen(entry->d_name) + 2);
                sprintf(field->completion_candidates[field->completion_count], 
                       "%s/", entry->d_name);
            } else {
                field->completion_candidates[field->completion_count] = 
                    strdup(entry->d_name);
            }
            field->completion_count++;
        }
    }
    
    closedir(dir);
    
    // Sort completions
    if (field->completion_count > 0) {
        qsort(field->completion_candidates, field->completion_count,
              sizeof(char *), completion_compare);
    }
    
    // Store the prefix info
    strncpy(field->completion_prefix, partial, sizeof(field->completion_prefix) - 1);
    field->completion_prefix[sizeof(field->completion_prefix) - 1] = '\0';
    field->completion_prefix_len = strlen(partial) - strlen(prefix);
    
    free(expanded);
    free(expanded_dir);
}

// Apply selected completion to text buffer
void inputfield_apply_completion(InputField *field, int index) {
    if (!field || index < 0 || index >= field->completion_count) return;
    
    
    // Build the completed path
    char completed[PATH_SIZE];
    if (field->completion_prefix_len >= PATH_SIZE) {
        fprintf(stderr, "[ERROR] InputField: Completion prefix too long\n");
        return;
    }
    strncpy(completed, field->completion_prefix, field->completion_prefix_len);
    completed[field->completion_prefix_len] = '\0';
    
    // Safe concatenation
    size_t current_len = strlen(completed);
    size_t candidate_len = strlen(field->completion_candidates[index]);
    if (current_len + candidate_len >= PATH_SIZE) {
        fprintf(stderr, "[ERROR] InputField: Completed path too long\n");
        return;
    }
    strncat(completed, field->completion_candidates[index], PATH_SIZE - current_len - 1);
    
    
    // Replace text buffer
    strncpy(field->text, completed, INPUTFIELD_MAX_LENGTH);
    field->text[INPUTFIELD_MAX_LENGTH] = '\0';
    field->cursor_pos = strlen(field->text);
    field->visible_start = 0;  // Reset scroll
    
    // Note: Don't hide dropdown here - caller will do it with proper Display pointer
    
    // Notify change
    if (field->on_change) {
        field->on_change(field->text, field->user_data);
    }
}

// Draw completion dropdown content
static void draw_completion_dropdown(InputField *field, Display *dpy) {
    if (!field || !field->completion_window || field->completion_count == 0) return;
    
    // Get window dimensions
    Window root_return;
    int x_return, y_return;
    unsigned int width, height, border_width_return, depth_return;
    XGetGeometry(dpy, field->completion_window, &root_return, 
                 &x_return, &y_return, &width, &height, 
                 &border_width_return, &depth_return);
    
    // Create XRender Picture for proper rendering
    Visual *visual = DefaultVisual(dpy, DefaultScreen(dpy));
    XRenderPictFormat *format = XRenderFindVisualFormat(dpy, visual);
    Pixmap pixmap = XCreatePixmap(dpy, field->completion_window, width, height, 
                                  DefaultDepth(dpy, DefaultScreen(dpy)));
    Picture picture = XRenderCreatePicture(dpy, pixmap, format, 0, NULL);
    
    // Define colors from config.h
    XRenderColor gray = {0xa0a0, 0xa2a2, 0xa0a0, 0xffff};  // GRAY from config.h
    XRenderColor black = {0x0000, 0x0000, 0x0000, 0xFFFF}; // BLACK from config.h
    XRenderColor white = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}; // WHITE from config.h
    XRenderColor blue = {0x4858, 0x6F6F, 0xB0B0, 0xFFFF};  // BLUE from config.h
    
    // Clear background to gray
    XRenderFillRectangle(dpy, PictOpSrc, picture, &gray, 0, 0, width, height);
    
    // Draw black border
    XRenderFillRectangle(dpy, PictOpSrc, picture, &black, 0, 0, width, 1);        // Top
    XRenderFillRectangle(dpy, PictOpSrc, picture, &black, 0, 0, 1, height);       // Left
    XRenderFillRectangle(dpy, PictOpSrc, picture, &black, width-1, 0, 1, height); // Right
    XRenderFillRectangle(dpy, PictOpSrc, picture, &black, 0, height-1, width, 1); // Bottom
    
    // Create XftDraw for text rendering
    XftDraw *xft_draw = XftDrawCreate(dpy, pixmap, visual, 
                                      DefaultColormap(dpy, DefaultScreen(dpy)));
    
    // Use the font stored in the InputField (passed from the app)
    if (!field->font) {
        // Can't render without font
        XftDrawDestroy(xft_draw);
        XRenderFreePicture(dpy, picture);
        XFreePixmap(dpy, pixmap);
        return;
    }
    
    // Draw completion items
    int item_height = 20;
    int max_items = (height - 4) / item_height;
    int start_item = 0;
    
    // Scroll to show selected item if needed
    if (field->completion_selected >= max_items) {
        start_item = field->completion_selected - max_items + 1;
    }
    
    // Prepare XftColors
    XftColor black_color, white_color;
    XftColorAllocValue(dpy, visual, DefaultColormap(dpy, DefaultScreen(dpy)), &black, &black_color);
    XftColorAllocValue(dpy, visual, DefaultColormap(dpy, DefaultScreen(dpy)), &white, &white_color);
    
    for (int i = 0; i < max_items && start_item + i < field->completion_count; i++) {
        int y = 2 + i * item_height;
        int item_index = start_item + i;
        
        // Highlight selected item with blue background
        if (item_index == field->completion_selected) {
            XRenderFillRectangle(dpy, PictOpSrc, picture, &blue, 
                               2, y, width - 4, item_height);
        }
        
        // Draw text
        int text_x = 5;
        int text_y = y + item_height - 5;
        
        XftColor *text_color = (item_index == field->completion_selected) ? &white_color : &black_color;
        XftDrawStringUtf8(xft_draw, text_color, field->font, text_x, text_y,
                         (FcChar8*)field->completion_candidates[item_index],
                         strlen(field->completion_candidates[item_index]));
    }
    
    // Copy to window
    GC gc = XCreateGC(dpy, field->completion_window, 0, NULL);
    XCopyArea(dpy, pixmap, field->completion_window, gc, 0, 0, width, height, 0, 0);
    XFreeGC(dpy, gc);
    
    // Clean up (do NOT close the font - it belongs to the app)
    XftColorFree(dpy, visual, DefaultColormap(dpy, DefaultScreen(dpy)), &black_color);
    XftColorFree(dpy, visual, DefaultColormap(dpy, DefaultScreen(dpy)), &white_color);
    XftDrawDestroy(xft_draw);
    XRenderFreePicture(dpy, picture);
    XFreePixmap(dpy, pixmap);
    XFlush(dpy);
}

// Show completion dropdown at specific coordinates relative to parent window
void inputfield_show_completions_at(InputField *field, Display *dpy, Window parent_window, int x, int y) {
    if (!field || !dpy) return;
    
    // Find completions for current text
    find_completions(field, field->text);
    
    if (field->completion_count == 0) {
        return;  // No completions found
    }
    
    // If only one completion, apply it directly
    if (field->completion_count == 1) {
        inputfield_apply_completion(field, 0);
        return;
    }
    
    // Calculate dropdown size
    int item_height = 20;
    int max_items = 5;  // Show max 5 items to avoid going under window borders
    int show_items = field->completion_count < max_items ? field->completion_count : max_items;
    int dropdown_height = show_items * item_height + 4;  // +4 for borders
    
    // Create or move/resize dropdown window
    if (!field->completion_window) {
        Window root = DefaultRootWindow(dpy);
        
        // Convert parent-relative coordinates to screen coordinates
        int screen_x, screen_y;
        Window child_return;
        XTranslateCoordinates(dpy, parent_window, root, x, y, 
                            &screen_x, &screen_y, &child_return);
        
        // Create window as child of ROOT (like Execute dialog does)
        XSetWindowAttributes attrs;
        attrs.override_redirect = True;
        attrs.background_pixel = 0xA0A2A0;  // Gray background
        attrs.border_pixel = BlackPixel(dpy, DefaultScreen(dpy));
        attrs.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | 
                           PointerMotionMask | EnterWindowMask | LeaveWindowMask;
        
        field->completion_window = XCreateWindow(dpy, root,  // ROOT window, like Execute dialog
            screen_x, screen_y,  // Screen coordinates
            field->width, dropdown_height,
            0,  // No border width (we draw our own)
            CopyFromParent, InputOutput, CopyFromParent,
            CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWEventMask,
            &attrs);
    } else {
        // Convert coordinates and resize existing window
        Window root = DefaultRootWindow(dpy);
        int screen_x, screen_y;
        Window child_return;
        XTranslateCoordinates(dpy, parent_window, root, x, y, 
                            &screen_x, &screen_y, &child_return);
        XMoveResizeWindow(dpy, field->completion_window, 
                         screen_x, screen_y, field->width, dropdown_height);
    }
    
    // Initialize selection to first item
    field->completion_selected = 0;
    
    // Map and raise the dropdown
    XMapRaised(dpy, field->completion_window);
    field->dropdown_open = true;
    
    // Draw the content
    draw_completion_dropdown(field, dpy);
}

// Show completion dropdown (uses field's relative coordinates)
void inputfield_show_completions(InputField *field, Display *dpy, Window parent_window) {
    if (!field || !dpy) return;
    // Use field coordinates with proper Y position BELOW the field
    inputfield_show_completions_at(field, dpy, parent_window, field->x, field->y + field->height);
}

// Hide completion dropdown
void inputfield_hide_completions(InputField *field, Display *dpy) {
    if (!field) return;
    
    field->dropdown_open = false;
    
    if (field->completion_window && dpy) {
        XUnmapWindow(dpy, field->completion_window);
        XDestroyWindow(dpy, field->completion_window);
        field->completion_window = 0;
        XFlush(dpy);
    }
    
    free_completion_candidates(field);
}

// Handle click on completion dropdown
bool inputfield_handle_completion_click(InputField *field, int x, int y) {
    if (!field || !field->completion_window || field->completion_count == 0) {
        return false;
    }
    
    
    // Adjust for border (2 pixels)
    if (y < 2) {
        return false;
    }
    
    int item_height = 20;
    int index = (y - 2) / item_height;
    
    
    // Check if index is valid
    if (index >= 0 && index < field->completion_count) {
        inputfield_apply_completion(field, index);
        return true;
    }
    
    return false;
}

// Check if a window belongs to this field's completion dropdown
bool inputfield_is_completion_window(InputField *field, Window window) {
    return field && field->completion_window && field->completion_window == window;
}

// Redraw completion dropdown (for expose events)
void inputfield_redraw_completion(InputField *field, Display *dpy) {
    if (field && field->completion_window && field->completion_count > 0) {
        draw_completion_dropdown(field, dpy);
    }
}

// Check if dropdown is currently open
bool inputfield_has_dropdown_open(InputField *field) {
    return field && field->dropdown_open;
}