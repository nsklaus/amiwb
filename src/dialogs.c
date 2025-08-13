// File: dialogs.c
// AmigaOS-style dialog system implementation
#include "dialogs.h"
#include "render.h"
#include "config.h"
#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>
#include <X11/Xft/Xft.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Global dialog list (for multiple dialogs)
static RenameDialog *g_dialogs = NULL;

// Dialog rendering constants
#define DIALOG_MARGIN 20
#define INPUT_HEIGHT 24  // Taller for better text spacing
#define BUTTON_WIDTH 80
#define BUTTON_HEIGHT 25
#define ELEMENT_SPACING 15
#define LABEL_WIDTH 80   // Width for "New Name:" label

// Initialize dialog subsystem
void init_dialogs(void) {
    g_dialogs = NULL;
}

// Clean up all dialogs
void cleanup_dialogs(void) {
    while (g_dialogs) {
        RenameDialog *next = g_dialogs->next;
        if (g_dialogs->canvas) {
            destroy_canvas(g_dialogs->canvas);
        }
        free(g_dialogs);
        g_dialogs = next;
    }
}

// Create and show rename dialog
void show_rename_dialog(const char *old_name, 
                       void (*on_ok)(const char *new_name),
                       void (*on_cancel)(void),
                       void *user_data) {
    if (!old_name || !on_ok || !on_cancel) return;
    
    RenameDialog *dialog = malloc(sizeof(RenameDialog));
    if (!dialog) return;
    
    // Initialize dialog state
    strncpy(dialog->text_buffer, old_name, NAME_MAX);
    dialog->text_buffer[NAME_MAX] = '\0';
    dialog->cursor_pos = strlen(dialog->text_buffer);
    dialog->selection_start = -1;
    dialog->selection_end = -1;
    dialog->visible_start = 0;
    dialog->input_has_focus = false;
    dialog->ok_button_pressed = false;
    dialog->cancel_button_pressed = false;
    dialog->on_ok = on_ok;
    dialog->on_cancel = on_cancel;
    dialog->user_data = user_data;
    
    // Create canvas window (450x160 initial size)  
    dialog->canvas = create_canvas(NULL, 200, 150, 450, 160, DIALOG);
    if (!dialog->canvas) {
        free(dialog);
        return;
    }
    
    // Set dialog properties - create title with filename
    char title[256];
    snprintf(title, sizeof(title), "Rename '%s'", old_name);
    dialog->canvas->title = strdup(title);
    dialog->canvas->bg_color = GRAY;  // Standard dialog gray
    dialog->canvas->disable_scrollbars = true;  // Disable scrollbars for dialogs
    
    // Add to dialog list
    dialog->next = g_dialogs;
    g_dialogs = dialog;
    
    // Show the dialog and set it as active window
    XMapRaised(get_display(), dialog->canvas->win);
    set_active_window(dialog->canvas);  // Make dialog active so it can receive focus
    redraw_canvas(dialog->canvas);
}

// Close and cleanup specific dialog
void close_rename_dialog(RenameDialog *dialog) {
    if (!dialog) return;
    
    // Remove from dialog list
    if (g_dialogs == dialog) {
        g_dialogs = dialog->next;
    } else {
        for (RenameDialog *d = g_dialogs; d; d = d->next) {
            if (d->next == dialog) {
                d->next = dialog->next;
                break;
            }
        }
    }
    
    // Clean up canvas and memory
    if (dialog->canvas) {
        destroy_canvas(dialog->canvas);
    }
    free(dialog);
}

// Check if canvas is a dialog
bool is_dialog_canvas(Canvas *canvas) {
    if (!canvas) return false;
    for (RenameDialog *dialog = g_dialogs; dialog; dialog = dialog->next) {
        if (dialog->canvas == canvas) return true;
    }
    return false;
}

// Get dialog for canvas
RenameDialog *get_dialog_for_canvas(Canvas *canvas) {
    if (!canvas) return NULL;
    for (RenameDialog *dialog = g_dialogs; dialog; dialog = dialog->next) {
        if (dialog->canvas == canvas) return dialog;
    }
    return NULL;
}

// 3D drawing primitives
static void draw_inset_box(Picture dest, int x, int y, int w, int h) {
    Display *dpy = get_display();
    
    // Outer border - inset effect (light source top-left)
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, x, y, 1, h);        // Left outer (white)
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, x, y, w, 1);        // Top outer (white)
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, x+w-1, y, 1, h);    // Right outer (black)
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, x, y+h-1, w, 1);    // Bottom outer (black)
    
    // Inner border - creates the carved effect
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, x+1, y+1, 1, h-2);  // Left inner (black)
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, x+1, y+1, w-2, 1);  // Top inner (black)
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, x+w-2, y+1, 1, h-2);// Right inner (white)
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, x+1, y+h-2, w-2, 1);// Bottom inner (white)
    
    // Gray fill for input area (AmigaOS style)
    XRenderFillRectangle(dpy, PictOpSrc, dest, &GRAY, x+2, y+2, w-4, h-4);
}

static void draw_raised_box(Picture dest, int x, int y, int w, int h, bool pressed) {
    Display *dpy = get_display();
    
    if (!pressed) {
        // Normal raised state
        XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, x, y, 1, h);        // Left (white)
        XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, x, y, w, 1);        // Top (white)
        XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, x+w-1, y, 1, h);    // Right (black)
        XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, x, y+h-1, w, 1);    // Bottom (black)
        // Gray fill
        XRenderFillRectangle(dpy, PictOpSrc, dest, &GRAY, x+1, y+1, w-2, h-2);
    } else {
        // Pressed state - inverted
        XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, x, y, 1, h);        // Left (black)
        XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, x, y, w, 1);        // Top (black)
        XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, x+w-1, y, 1, h);    // Right (white)
        XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, x, y+h-1, w, 1);    // Bottom (white)
        // Blue fill when pressed
        XRenderFillRectangle(dpy, PictOpSrc, dest, &BLUE, x+1, y+1, w-2, h-2);
    }
}

// Calculate layout positions based on current canvas size
static void calculate_layout(RenameDialog *dialog, int *input_x, int *input_y, int *input_w,
                           int *ok_x, int *ok_y, int *cancel_x, int *cancel_y) {
    Canvas *canvas = dialog->canvas;
    
    // Account for window borders in layout calculations
    int content_left = BORDER_WIDTH_LEFT;
    int content_top = BORDER_HEIGHT_TOP;
    int content_width = canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT;
    int content_height = canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM;
    
    // Input box: starts after "New Name:" label, positioned below title with small gap
    *input_x = content_left + DIALOG_MARGIN + LABEL_WIDTH;
    *input_y = content_top + 35;  // Title + 10px gap as requested
    *input_w = content_width - 2 * DIALOG_MARGIN - LABEL_WIDTH;
    
    // Buttons: positioned well below input line with generous spacing
    *ok_x = content_left + DIALOG_MARGIN;
    *ok_y = content_top + 85;  // Fixed position to avoid collisions
    
    *cancel_x = content_left + content_width - DIALOG_MARGIN - BUTTON_WIDTH;
    *cancel_y = *ok_y;
}

// Helper to get font (from render.c)
extern XftFont *get_font(void);

// Forward declarations for internal functions
static void render_input_text(RenameDialog *dialog, XftDraw *draw, XftFont *font, 
                             int text_x, int text_y, int text_width);
static int calculate_visible_end(RenameDialog *dialog, XftFont *font, int available_width);

// Render text content with cursor and selection
static void render_text_content(RenameDialog *dialog, Picture dest, 
                               int input_x, int input_y, int input_w,
                               int ok_x, int ok_y, int cancel_x, int cancel_y) {
    Display *dpy = get_display();
    Canvas *canvas = dialog->canvas;
    XftFont *font = get_font();
    if (!font) return;
    
    // Create XftDraw for text rendering
    XftDraw *draw = XftDrawCreate(dpy, canvas->canvas_buffer, canvas->visual, canvas->colormap);
    if (!draw) return;
    
    // Draw centered title above input box
    XRenderColor text_color = BLACK;
    XftColor xft_text;
    XftColorAllocValue(dpy, canvas->visual, canvas->colormap, &text_color, &xft_text);
    
    char title_text[256];
    snprintf(title_text, sizeof(title_text), "Enter a new name for '%s'.", 
             strlen(dialog->text_buffer) > 0 ? dialog->text_buffer : "file");
    
    XGlyphInfo title_ext;
    XftTextExtentsUtf8(dpy, font, (FcChar8*)title_text, strlen(title_text), &title_ext);
    int content_width = canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT;
    int title_x = BORDER_WIDTH_LEFT + (content_width - title_ext.xOff) / 2;
    int title_y = BORDER_HEIGHT_TOP + 20;
    XftDrawStringUtf8(draw, &xft_text, font, title_x, title_y, 
                     (FcChar8*)title_text, strlen(title_text));
    
    // Draw "New Name:" label to the left of input box (same row)
    int label_x = BORDER_WIDTH_LEFT + DIALOG_MARGIN;
    int label_y = input_y + (INPUT_HEIGHT + font->ascent) / 2 - 2;  // Move up 2 pixels
    XftDrawStringUtf8(draw, &xft_text, font, label_x, label_y, 
                     (FcChar8*)"New Name:", 9);
    
    // Render input box text with cursor and selection 
    // Calculate text Y to center it within the input box
    int text_y = input_y + (INPUT_HEIGHT + font->ascent) / 2 - 2;  // Move up 2 pixels
    // Calculate one character width for proper padding
    XGlyphInfo char_info;
    XftTextExtentsUtf8(dpy, font, (FcChar8*)" ", 1, &char_info);
    int char_width = char_info.xOff;
    render_input_text(dialog, draw, font, input_x + char_width + 2, text_y, input_w - (char_width + 2) * 2);
    
    // Draw button labels
    XRenderColor button_text_color = dialog->ok_button_pressed ? WHITE : BLACK;
    XftColor xft_button;
    XftColorAllocValue(dpy, canvas->visual, canvas->colormap, &button_text_color, &xft_button);
    
    // Center "OK" in button
    XGlyphInfo ok_ext;
    XftTextExtentsUtf8(dpy, font, (FcChar8*)"OK", 2, &ok_ext);
    int ok_text_x = ok_x + (BUTTON_WIDTH - ok_ext.xOff) / 2;
    int ok_text_y = ok_y + (BUTTON_HEIGHT + font->ascent) / 2 - 2;
    XftDrawStringUtf8(draw, &xft_button, font, ok_text_x, ok_text_y, (FcChar8*)"OK", 2);
    
    // Update button text color for Cancel button
    button_text_color = dialog->cancel_button_pressed ? WHITE : BLACK;
    XftColorAllocValue(dpy, canvas->visual, canvas->colormap, &button_text_color, &xft_button);
    
    // Center "Cancel" in button  
    XGlyphInfo cancel_ext;
    XftTextExtentsUtf8(dpy, font, (FcChar8*)"Cancel", 6, &cancel_ext);
    int cancel_text_x = cancel_x + (BUTTON_WIDTH - cancel_ext.xOff) / 2;
    int cancel_text_y = cancel_y + (BUTTON_HEIGHT + font->ascent) / 2 - 2;
    XftDrawStringUtf8(draw, &xft_button, font, cancel_text_x, cancel_text_y, (FcChar8*)"Cancel", 6);
    
    // Clean up
    XftColorFree(dpy, canvas->visual, canvas->colormap, &xft_text);
    XftColorFree(dpy, canvas->visual, canvas->colormap, &xft_button);
    XftDrawDestroy(draw);
}

// Render input text with cursor position and selection highlighting
static void render_input_text(RenameDialog *dialog, XftDraw *draw, XftFont *font, 
                             int text_x, int text_y, int text_width) {
    Display *dpy = get_display();
    Canvas *canvas = dialog->canvas;
    const char *text = dialog->text_buffer;
    int text_len = strlen(text);
    
    // Create both black and white text colors
    XRenderColor black_color = BLACK;
    XRenderColor white_color = WHITE;
    XftColor xft_black, xft_white;
    XftColorAllocValue(dpy, canvas->visual, canvas->colormap, &black_color, &xft_black);
    XftColorAllocValue(dpy, canvas->visual, canvas->colormap, &white_color, &xft_white);
    
    if (text_len == 0) {
        // Empty text, show cursor as a blue rectangle the size of a space character
        if (dialog->input_has_focus && dialog->cursor_pos == 0) {
            XGlyphInfo space_info;
            XftTextExtentsUtf8(dpy, font, (FcChar8*)" ", 1, &space_info);
            XRenderFillRectangle(dpy, PictOpSrc, canvas->canvas_render, &BLUE, 
                               text_x, text_y - font->ascent, space_info.xOff, font->height);
        }
        XftColorFree(dpy, canvas->visual, canvas->colormap, &xft_black);
        XftColorFree(dpy, canvas->visual, canvas->colormap, &xft_white);
        return;
    }
    
    // Calculate visible text range for horizontal scrolling
    int visible_start = dialog->visible_start;
    int visible_end = calculate_visible_end(dialog, font, text_width);
    
    int x = text_x;
    for (int i = visible_start; i < visible_end && i < text_len; i++) {
        char ch[2] = {text[i], '\0'};
        XGlyphInfo glyph_info;
        XftTextExtentsUtf8(dpy, font, (FcChar8*)ch, 1, &glyph_info);
        
        // Check if this character needs special background
        bool is_cursor = (dialog->input_has_focus && i == dialog->cursor_pos);
        bool is_selected = (dialog->selection_start >= 0 && 
                           i >= dialog->selection_start && i < dialog->selection_end);
        
        if (is_cursor || is_selected) {
            // Draw blue background for cursor or selection
            XRenderFillRectangle(dpy, PictOpSrc, canvas->canvas_render, &BLUE,
                               x, text_y - font->ascent, glyph_info.xOff, font->height);
            // Use white text on blue background
            XftDrawStringUtf8(draw, &xft_white, font, x, text_y, (FcChar8*)ch, 1);
        } else {
            // Use black text on normal background
            XftDrawStringUtf8(draw, &xft_black, font, x, text_y, (FcChar8*)ch, 1);
        }
        
        x += glyph_info.xOff;
    }
    
    // Draw cursor after last character if needed (cursor at end of text)
    if (dialog->input_has_focus && dialog->cursor_pos >= text_len && dialog->cursor_pos >= visible_end) {
        XGlyphInfo space_info;
        XftTextExtentsUtf8(dpy, font, (FcChar8*)" ", 1, &space_info);
        XRenderFillRectangle(dpy, PictOpSrc, canvas->canvas_render, &BLUE,
                           x, text_y - font->ascent, space_info.xOff, font->height);
    }
    
    XftColorFree(dpy, canvas->visual, canvas->colormap, &xft_black);
    XftColorFree(dpy, canvas->visual, canvas->colormap, &xft_white);
}

// Calculate the last visible character index for horizontal scrolling
static int calculate_visible_end(RenameDialog *dialog, XftFont *font, int available_width) {
    const char *text = dialog->text_buffer;
    int text_len = strlen(text);
    int visible_start = dialog->visible_start;
    
    int width_used = 0;
    int i;
    for (i = visible_start; i < text_len; i++) {
        char ch[2] = {text[i], '\0'};
        XGlyphInfo glyph_info;
        XftTextExtentsUtf8(get_display(), font, (FcChar8*)ch, 1, &glyph_info);
        
        if (width_used + glyph_info.xOff > available_width) {
            break;
        }
        width_used += glyph_info.xOff;
    }
    
    return i;
}

// Render dialog content
void render_dialog_content(Canvas *canvas) {
    RenameDialog *dialog = get_dialog_for_canvas(canvas);
    if (!dialog) return;
    
    Display *dpy = get_display();
    Picture dest = canvas->canvas_render;
    
    // Clear background to dialog gray
    XRenderFillRectangle(dpy, PictOpSrc, dest, &GRAY, 0, 0, canvas->width, canvas->height);
    
    // Calculate element positions
    int input_x, input_y, input_w, ok_x, ok_y, cancel_x, cancel_y;
    calculate_layout(dialog, &input_x, &input_y, &input_w, &ok_x, &ok_y, &cancel_x, &cancel_y);
    
    // Draw input box with inset 3D effect
    draw_inset_box(dest, input_x, input_y, input_w, INPUT_HEIGHT);
    
    // Draw buttons
    draw_raised_box(dest, ok_x, ok_y, BUTTON_WIDTH, BUTTON_HEIGHT, dialog->ok_button_pressed);
    draw_raised_box(dest, cancel_x, cancel_y, BUTTON_WIDTH, BUTTON_HEIGHT, dialog->cancel_button_pressed);
    
    // Render text and labels
    render_text_content(dialog, dest, input_x, input_y, input_w, ok_x, ok_y, cancel_x, cancel_y);
}

// Event handlers
bool dialogs_handle_key_press(XKeyEvent *event) {
    // Find the dialog for the active window
    Canvas *active = get_active_window();
    if (!active || active->type != DIALOG) return false;
    
    RenameDialog *dialog = get_dialog_for_canvas(active);
    if (!dialog) return false;
    
    KeySym keysym = XLookupKeysym(event, 0);
    
    // Handle Escape key - cancel dialog
    if (keysym == XK_Escape) {
        if (dialog->on_cancel) dialog->on_cancel();
        close_rename_dialog(dialog);
        return true;
    }
    
    // Handle Enter key - accept dialog
    if (keysym == XK_Return || keysym == XK_KP_Enter) {
        if (dialog->on_ok) dialog->on_ok(dialog->text_buffer);
        close_rename_dialog(dialog);
        return true;
    }
    
    // Only handle input if the input box has focus
    if (!dialog->input_has_focus) return false;
    
    // Handle backspace
    if (keysym == XK_BackSpace) {
        if (dialog->cursor_pos > 0) {
            int len = strlen(dialog->text_buffer);
            // Remove character before cursor
            memmove(&dialog->text_buffer[dialog->cursor_pos - 1],
                   &dialog->text_buffer[dialog->cursor_pos],
                   len - dialog->cursor_pos + 1);
            dialog->cursor_pos--;
            redraw_canvas(dialog->canvas);
        }
        return true;
    }
    
    // Handle delete key
    if (keysym == XK_Delete) {
        int len = strlen(dialog->text_buffer);
        if (dialog->cursor_pos < len) {
            // Remove character at cursor
            memmove(&dialog->text_buffer[dialog->cursor_pos],
                   &dialog->text_buffer[dialog->cursor_pos + 1],
                   len - dialog->cursor_pos);
            redraw_canvas(dialog->canvas);
        }
        return true;
    }
    
    // Handle left arrow
    if (keysym == XK_Left) {
        if (dialog->cursor_pos > 0) {
            dialog->cursor_pos--;
            redraw_canvas(dialog->canvas);
        }
        return true;
    }
    
    // Handle right arrow
    if (keysym == XK_Right) {
        if (dialog->cursor_pos < (int)strlen(dialog->text_buffer)) {
            dialog->cursor_pos++;
            redraw_canvas(dialog->canvas);
        }
        return true;
    }
    
    // Handle Home key
    if (keysym == XK_Home) {
        dialog->cursor_pos = 0;
        redraw_canvas(dialog->canvas);
        return true;
    }
    
    // Handle End key
    if (keysym == XK_End) {
        dialog->cursor_pos = strlen(dialog->text_buffer);
        redraw_canvas(dialog->canvas);
        return true;
    }
    
    // Handle regular text input - use XLookupString to properly handle modifiers
    char buffer[32];
    KeySym keysym_ignored;
    int char_count = XLookupString(event, buffer, sizeof(buffer)-1, &keysym_ignored, NULL);
    
    if (char_count > 0) {
        // Filter out control characters but allow printable chars and common symbols
        for (int i = 0; i < char_count; i++) {
            unsigned char ch = (unsigned char)buffer[i];
            if (ch >= 32 && ch <= 126) {  // Printable ASCII range
                int len = strlen(dialog->text_buffer);
                if (len < NAME_MAX) {  // Don't overflow buffer
                    // Insert character at cursor position
                    memmove(&dialog->text_buffer[dialog->cursor_pos + 1],
                           &dialog->text_buffer[dialog->cursor_pos],
                           len - dialog->cursor_pos + 1);
                    dialog->text_buffer[dialog->cursor_pos] = (char)ch;
                    dialog->cursor_pos++;
                    redraw_canvas(dialog->canvas);
                }
                return true;  // Only handle one character at a time
            }
        }
    }
    
    return false;
}

bool dialogs_handle_button_press(XButtonEvent *event) {
    if (!event) return false;
    
    Canvas *canvas = find_canvas(event->window);
    if (!canvas || canvas->type != DIALOG) return false;
    
    RenameDialog *dialog = get_dialog_for_canvas(canvas);
    if (!dialog) return false;
    
    // Get layout positions
    int input_x, input_y, input_w, ok_x, ok_y, cancel_x, cancel_y;
    calculate_layout(dialog, &input_x, &input_y, &input_w, &ok_x, &ok_y, &cancel_x, &cancel_y);
    
    // Check if click is on OK button
    if (event->x >= ok_x && event->x < ok_x + BUTTON_WIDTH &&
        event->y >= ok_y && event->y < ok_y + BUTTON_HEIGHT) {
        dialog->ok_button_pressed = true;
        redraw_canvas(canvas);
        printf("DEBUG: OK button pressed\n");
        return true;
    }
    
    // Check if click is on Cancel button
    if (event->x >= cancel_x && event->x < cancel_x + BUTTON_WIDTH &&
        event->y >= cancel_y && event->y < cancel_y + BUTTON_HEIGHT) {
        dialog->cancel_button_pressed = true;
        redraw_canvas(canvas);
        printf("DEBUG: Cancel button pressed\n");
        return true;
    }
    
    // Check if click is in input box
    if (event->x >= input_x && event->x < input_x + input_w &&
        event->y >= input_y && event->y < input_y + INPUT_HEIGHT) {
        dialog->input_has_focus = true;
        
        // Set cursor position based on click location
        XftFont *font = get_font();
        if (font) {
            // Calculate one character width for proper padding (matching render_text_content)
            XGlyphInfo char_info;
            XftTextExtentsUtf8(get_display(), font, (FcChar8*)" ", 1, &char_info);
            int char_width = char_info.xOff;
            int text_start_x = input_x + char_width + 2;  // Match render_input_text positioning
            int click_offset = event->x - text_start_x;
            
            if (click_offset <= 0) {
                dialog->cursor_pos = 0;
            } else {
                // Find character position by measuring text width
                const char *text = dialog->text_buffer;
                int text_len = strlen(text);
                int width_so_far = 0;
                int pos = 0;
                
                for (pos = 0; pos < text_len; pos++) {
                    char ch[2] = {text[pos], '\0'};
                    XGlyphInfo glyph_info;
                    XftTextExtentsUtf8(get_display(), font, (FcChar8*)ch, 1, &glyph_info);
                    
                    if (width_so_far + glyph_info.xOff/2 > click_offset) {
                        break;
                    }
                    width_so_far += glyph_info.xOff;
                }
                dialog->cursor_pos = pos;
            }
        }
        
        redraw_canvas(canvas);
        printf("DEBUG: Input box clicked, cursor set to position %d\n", dialog->cursor_pos);
        return true;
    }
    
    return false;
}

bool dialogs_handle_button_release(XButtonEvent *event) {
    if (!event) return false;
    
    Canvas *canvas = find_canvas(event->window);
    if (!canvas || canvas->type != DIALOG) return false;
    
    RenameDialog *dialog = get_dialog_for_canvas(canvas);
    if (!dialog) return false;
    
    // Get layout positions
    int input_x, input_y, input_w, ok_x, ok_y, cancel_x, cancel_y;
    calculate_layout(dialog, &input_x, &input_y, &input_w, &ok_x, &ok_y, &cancel_x, &cancel_y);
    
    bool handled = false;
    
    // Check if release is on OK button while it was pressed
    if (dialog->ok_button_pressed && 
        event->x >= ok_x && event->x < ok_x + BUTTON_WIDTH &&
        event->y >= ok_y && event->y < ok_y + BUTTON_HEIGHT) {
        dialog->ok_button_pressed = false;
        printf("DEBUG: OK button released - executing callback\n");
        if (dialog->on_ok) {
            dialog->on_ok(dialog->text_buffer);
        }
        close_rename_dialog(dialog);
        return true;  // Return immediately to avoid use-after-free
    }
    
    // Check if release is on Cancel button while it was pressed
    else if (dialog->cancel_button_pressed && 
             event->x >= cancel_x && event->x < cancel_x + BUTTON_WIDTH &&
             event->y >= cancel_y && event->y < cancel_y + BUTTON_HEIGHT) {
        dialog->cancel_button_pressed = false;
        printf("DEBUG: Cancel button released - executing callback\n");
        if (dialog->on_cancel) {
            dialog->on_cancel();
        }
        close_rename_dialog(dialog);
        return true;  // Return immediately to avoid use-after-free
    }
    
    // Reset button states if we had any pressed (only if dialog wasn't closed)
    if (dialog->ok_button_pressed || dialog->cancel_button_pressed) {
        dialog->ok_button_pressed = false;
        dialog->cancel_button_pressed = false;
        redraw_canvas(canvas);
        handled = true;
    }
    
    return handled;
}

bool dialogs_handle_motion(XMotionEvent *event) {
    // TODO: Implement mouse motion (for text selection)
    return false;
}