// File: dialogs.c
// AmigaOS-style dialog system implementation
#include "dialogs.h"
#include "render.h"
#include "config.h"
#include "intuition/itn_internal.h"
#include "../toolkit/progressbar/progressbar.h"
#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>
#include <X11/Xft/Xft.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <signal.h>

// Global dialog list (for multiple dialogs)
static Dialog *g_dialogs = NULL;

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

// ============================================================================
// Dialog Lifecycle Management
// ============================================================================

// OWNERSHIP: Returns allocated Dialog - caller must call destroy_dialog()
// Creates basic dialog structure with canvas - caller adds specific widgets
// Returns NULL on failure (graceful degradation - dialog won't appear)
static Dialog* create_dialog(DialogType type, const char* title, int width, int height) {
    if (!title) {
        log_error("[ERROR] create_dialog called with NULL title");
        return NULL;  // Graceful failure
    }

    // Allocate dialog structure
    Dialog* dialog = calloc(1, sizeof(Dialog));
    if (!dialog) {
        log_error("[ERROR] Failed to allocate Dialog structure - dialog will not appear");
        return NULL;  // Graceful failure
    }

    // Initialize basic state
    dialog->dialog_type = type;
    dialog->input_field = NULL;
    dialog->ok_button = NULL;
    dialog->cancel_button = NULL;
    dialog->on_ok = NULL;
    dialog->on_cancel = NULL;
    dialog->user_data = NULL;
    dialog->next = NULL;

    // Get font from render system
    dialog->font = get_font();
    if (!dialog->font) {
        log_error("[ERROR] Failed to get font for dialog - dialog will not appear");
        free(dialog);
        return NULL;  // Graceful failure
    }

    // Create canvas window
    dialog->canvas = create_canvas(NULL, 200, 150, width, height, DIALOG);
    if (!dialog->canvas) {
        log_error("[ERROR] Failed to create canvas for dialog - dialog will not appear");
        free(dialog);
        return NULL;  // Graceful failure
    }

    // Set title
    dialog->canvas->title_base = strdup(title);
    if (!dialog->canvas->title_base) {
        log_error("[ERROR] strdup failed for dialog title: %s - dialog will not appear", title);
        itn_canvas_destroy(dialog->canvas);
        free(dialog);
        return NULL;  // Graceful failure
    }

    // Standard dialog properties
    dialog->canvas->title_change = NULL;
    dialog->canvas->bg_color = GRAY;
    dialog->canvas->disable_scrollbars = true;

    return dialog;
}

// Clean up all dialogs
void cleanup_dialogs(void) {
    while (g_dialogs) {
        Dialog *next = g_dialogs->next;
        if (g_dialogs->canvas) {
            itn_canvas_destroy(g_dialogs->canvas);
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

    // Create title with filename
    char title[NAME_SIZE];
    snprintf(title, sizeof(title), "Rename '%s'", old_name);

    // Create dialog using consistent lifecycle function (returns NULL on failure)
    Dialog *dialog = create_dialog(DIALOG_RENAME, title, 450, 160);
    if (!dialog) {
        log_error("[ERROR] Failed to create rename dialog - feature unavailable");
        return;  // Graceful degradation
    }

    // Set callbacks and user data
    strncpy(dialog->original_name, old_name, NAME_SIZE - 1);
    dialog->original_name[NAME_SIZE - 1] = '\0';
    dialog->on_ok = on_ok;
    dialog->on_cancel = on_cancel;
    dialog->user_data = user_data;

    // Create InputField widget for text entry
    dialog->input_field = inputfield_create(0, 0, 100, INPUT_HEIGHT, dialog->font);
    if (!dialog->input_field) {
        destroy_dialog(dialog);
        return;
    }

    // Set initial text and cursor
    inputfield_set_text(dialog->input_field, old_name);
    dialog->input_field->cursor_pos = strlen(old_name);
    dialog->input_field->has_focus = true;

    // Create toolkit buttons
    dialog->ok_button = button_create(20, 85, BUTTON_WIDTH, BUTTON_HEIGHT, "OK", dialog->font);
    dialog->cancel_button = button_create(340, 85, BUTTON_WIDTH, BUTTON_HEIGHT, "Cancel", dialog->font);

    // Add to dialog list
    dialog->next = g_dialogs;
    g_dialogs = dialog;

    // Show the dialog and set it as active window
    XMapRaised(itn_core_get_display(), dialog->canvas->win);
    itn_focus_set_active(dialog->canvas);

    redraw_canvas(dialog->canvas);
}

// Show execute command dialog
void show_execute_dialog(void (*on_ok)(const char *command),
                        void (*on_cancel)(void)) {
    if (!on_ok || !on_cancel) return;

    // Create dialog using consistent lifecycle function (returns NULL on failure)
    Dialog *dialog = create_dialog(DIALOG_EXECUTE_COMMAND, "Execute", 450, 160);
    if (!dialog) {
        log_error("[ERROR] Failed to create execute dialog - feature unavailable");
        return;  // Graceful degradation
    }

    // Set callbacks
    dialog->original_name[0] = '\0';  // Not used for execute dialog
    dialog->on_ok = on_ok;
    dialog->on_cancel = on_cancel;

    // Create InputField widget for command entry
    dialog->input_field = inputfield_create(0, 0, 100, INPUT_HEIGHT, dialog->font);
    if (!dialog->input_field) {
        destroy_dialog(dialog);
        return;
    }

    // Enable path completion for execute dialog
    inputfield_enable_path_completion(dialog->input_field, true);
    inputfield_set_text(dialog->input_field, "");
    dialog->input_field->has_focus = true;

    // Create toolkit buttons
    dialog->ok_button = button_create(20, 85, BUTTON_WIDTH, BUTTON_HEIGHT, "OK", dialog->font);
    dialog->cancel_button = button_create(340, 85, BUTTON_WIDTH, BUTTON_HEIGHT, "Cancel", dialog->font);

    // Add to dialog list
    dialog->next = g_dialogs;
    g_dialogs = dialog;

    // Show the dialog and set it as active window
    XMapRaised(itn_core_get_display(), dialog->canvas->win);
    itn_focus_set_active(dialog->canvas);
    redraw_canvas(dialog->canvas);
}

// Close and cleanup specific dialog
// OWNERSHIP: Complete cleanup - frees widgets, canvas, and dialog struct
void destroy_dialog(Dialog *dialog) {
    if (!dialog) return;

    // Remove from dialog list
    if (g_dialogs == dialog) {
        g_dialogs = dialog->next;
    } else {
        for (Dialog *d = g_dialogs; d; d = d->next) {
            if (d->next == dialog) {
                d->next = dialog->next;
                break;
            }
        }
    }

    // Clean up InputField widget and its dropdown
    if (dialog->input_field) {
        // Make sure to close any open dropdown first
        if (dialog->input_field->dropdown_open) {
            inputfield_hide_completions(dialog->input_field, itn_core_get_display());
        }
        inputfield_destroy(dialog->input_field);
        dialog->input_field = NULL;
    }

    // Clean up toolkit buttons
    if (dialog->ok_button) {
        button_destroy(dialog->ok_button);
        dialog->ok_button = NULL;
    }
    if (dialog->cancel_button) {
        button_destroy(dialog->cancel_button);
        dialog->cancel_button = NULL;
    }

    // Font is managed by font_manager - don't close it!
    dialog->font = NULL;  // Just clear the reference

    // Clean up canvas and memory
    if (dialog->canvas) {
        itn_canvas_destroy(dialog->canvas);
    }

    // Zero out and free struct
    memset(dialog, 0, sizeof(Dialog));
    free(dialog);
}

// Close dialog by canvas (called from intuition.c when window X button is clicked)
// OWNERSHIP: Canvas is being destroyed externally by intuition.c, so we NULL it
// before calling destroy_dialog() which will then skip canvas cleanup
void close_dialog_by_canvas(Canvas *canvas) {
    if (!canvas) return;

    Dialog *dialog = get_dialog_for_canvas(canvas);
    if (dialog) {
        // Call cancel callback if it exists
        if (dialog->on_cancel) {
            dialog->on_cancel();
        }

        // Canvas is being destroyed by intuition.c, not by us
        // Set to NULL so destroy_dialog() won't try to destroy it
        dialog->canvas = NULL;

        // Now properly cleanup dialog (widgets, struct, etc)
        destroy_dialog(dialog);
    }
}

// ========================
// Completion Helper Functions

// Check if canvas is a dialog
bool is_dialog_canvas(Canvas *canvas) {
    if (!canvas) return false;
    for (Dialog *dialog = g_dialogs; dialog; dialog = dialog->next) {
        if (dialog->canvas == canvas) return true;
    }
    return false;
}

// Get dialog for canvas
Dialog *get_dialog_for_canvas(Canvas *canvas) {
    if (!canvas) return NULL;
    for (Dialog *dialog = g_dialogs; dialog; dialog = dialog->next) {
        if (dialog->canvas == canvas) return dialog;
    }
    return NULL;
}

// 3D drawing primitives
static void draw_inset_box(Picture dest, int x, int y, int w, int h) {
    Display *dpy = itn_core_get_display();
    
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


// Calculate layout positions based on current canvas size
static void calculate_layout(Dialog *dialog, int *input_x, int *input_y, int *input_w,
                           int *ok_x, int *ok_y, int *cancel_x, int *cancel_y) {
    Canvas *canvas = dialog->canvas;
    
    // Account for window borders in layout calculations
    int content_left = BORDER_WIDTH_LEFT;
    int content_top = BORDER_HEIGHT_TOP;
    // Dialogs use client window borders (8px right border)
    int content_width = canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT_CLIENT;
    // int content_height = canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM;  // May be used later
    
    // Input box: starts after "New Name:" label, positioned below title with small gap
    *input_x = content_left + DIALOG_MARGIN + LABEL_WIDTH;
    *input_y = content_top + 35;  // Title + 10px gap as requested
    *input_w = content_width - 2 * DIALOG_MARGIN - LABEL_WIDTH;
    
    // Buttons: positioned at bottom with proper spacing
    // For delete dialog, need more room for the warning text and checker pattern
    int button_y_offset = 85;  // Default for rename dialog
    if (dialog->dialog_type == DIALOG_DELETE_CONFIRM) {
        // Position buttons to leave room for checker pattern below (4px) and decorations
        button_y_offset = 150;  // Higher to leave room for bottom checker and decoration
        
        // Align buttons with inner window boundaries (after 10px checker border)
        int border_thickness = 10;
        *ok_x = content_left + border_thickness;  // Align with inner window left edge
        *ok_y = content_top + button_y_offset;
        
        *cancel_x = content_left + content_width - border_thickness - BUTTON_WIDTH;  // Align with inner window right edge
        *cancel_y = *ok_y;
    } else {
        *ok_x = content_left + DIALOG_MARGIN;
        *ok_y = content_top + button_y_offset;
        
        *cancel_x = content_left + content_width - DIALOG_MARGIN - BUTTON_WIDTH;
        *cancel_y = *ok_y;
    }
}

// Forward declarations for internal functions
static void draw_checkerboard_pattern(Picture dest, int x, int y, int w, int h);

// Render text content with cursor and selection
static void render_text_content(Dialog *dialog, Picture dest, 
                               int input_x, int input_y, int input_w,
                               int ok_x, int ok_y, int cancel_x, int cancel_y) {
    Display *dpy = itn_core_get_display();
    Canvas *canvas = dialog->canvas;
    XftFont *font = get_font();
    if (!font) return;
    
    // Use cached XftDraw for text rendering
    if (!canvas->xft_draw) {
        log_error("[WARNING] No cached XftDraw for rename dialog");
        return;
    }
    
    // Draw centered title above input box
    XRenderColor text_color = BLACK;
    XftColor xft_text;
    XftColorAllocValue(dpy, canvas->visual, canvas->colormap, &text_color, &xft_text);
    
    if (dialog->dialog_type == DIALOG_DELETE_CONFIRM) {
        //int content_width = canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT_CLIENT;
        int line_y = BORDER_HEIGHT_TOP + 30;
        int text_left_x = BORDER_WIDTH_LEFT + 15;  // Left margin inside inner window
        
        /*
        // Line 1: "Warning" centered
        const char *warning = "Warning";
        XGlyphInfo warning_ext;
        XftTextExtentsUtf8(dpy, font, (FcChar8*)warning, strlen(warning), &warning_ext);
        int warning_x = BORDER_WIDTH_LEFT + (content_width - warning_ext.xOff) / 2;
        XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, warning_x, line_y, 
                         (FcChar8*)warning, strlen(warning));
        line_y += 30;
        */


        const char *line1 = "Last call before Willoughby. Beyond this point,";
        const char *line2 = "no return service is available. Files wishing to";
        const char *line3 = "preserve structural integrity should disembark";
        const char *line4 = "immediately. Dear Files and Dirs: Last call,";
        const char *line5 = "Terminus inbound..";


        XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_left_x, line_y,
                 (FcChar8*)line1, strlen(line1));
        line_y += 14;

        XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_left_x, line_y,
                         (FcChar8*)line2, strlen(line2));
        line_y += 14;

        XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_left_x, line_y,
                         (FcChar8*)line3, strlen(line3));
        line_y += 14;

        XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_left_x, line_y,
                         (FcChar8*)line4, strlen(line4));
        line_y += 14;

        XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_left_x, line_y,
                 (FcChar8*)line5, strlen(line5));
        line_y += 35;
        
        const char *line6 = "Is it really Ok to delete:";
        XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_left_x, line_y,
                         (FcChar8*)line6, strlen(line6));
        line_y += 14;
        
        // Line 5: The delete summary (stored in text_buffer) - left aligned
        // This contains formatted text like "3 files and 4 directories?"
        const char *msg = dialog->text_buffer;
        XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_left_x, line_y,
                         (FcChar8*)msg, strlen(msg));

    } else if (dialog->dialog_type == DIALOG_EXECUTE_COMMAND) {
        // For execute dialog, show the command prompt
        const char *title_text = "Enter Command and its Arguments:";
        
        XGlyphInfo title_ext;
        XftTextExtentsUtf8(dpy, font, (FcChar8*)title_text, strlen(title_text), &title_ext);
        int content_width = canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT_CLIENT;
        int title_x = BORDER_WIDTH_LEFT + (content_width - title_ext.xOff) / 2;
        int title_y = BORDER_HEIGHT_TOP + 20;
        XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, title_x, title_y, 
                         (FcChar8*)title_text, strlen(title_text));
        
        // Draw "Command:" label to the left of input box (same row)
        int label_x = BORDER_WIDTH_LEFT + DIALOG_MARGIN;
        int label_y = input_y + (INPUT_HEIGHT + font->ascent) / 2 - 2;  // Move up 2 pixels
        XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, label_x, label_y, 
                         (FcChar8*)"Command:", 8);
        
        // Update InputField position and size if needed
        if (dialog->input_field) {
            dialog->input_field->x = input_x;
            dialog->input_field->y = input_y;
            dialog->input_field->width = input_w;
            dialog->input_field->height = INPUT_HEIGHT;
            
            // Draw the InputField using toolkit function
            inputfield_render(dialog->input_field, canvas->canvas_render, dpy, canvas->xft_draw);
        }
    } else {
        // For rename dialog, show the original prompt
        char title_text[PATH_SIZE];  // May contain file path in message
        int ret = snprintf(title_text, sizeof(title_text), "Enter a new name for '%s'.", 
                 strlen(dialog->original_name) > 0 ? dialog->original_name : "file");
        if (ret >= (int)sizeof(title_text)) {
            log_error("[ERROR] Dialog title too long, using shortened version");
            snprintf(title_text, sizeof(title_text), "Enter a new name.");
        }
        
        XGlyphInfo title_ext;
        XftTextExtentsUtf8(dpy, font, (FcChar8*)title_text, strlen(title_text), &title_ext);
        int content_width = canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT_CLIENT;
        int title_x = BORDER_WIDTH_LEFT + (content_width - title_ext.xOff) / 2;
        int title_y = BORDER_HEIGHT_TOP + 20;
        XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, title_x, title_y, 
                         (FcChar8*)title_text, strlen(title_text));
        
        // Draw "New Name:" label to the left of input box (same row)
        int label_x = BORDER_WIDTH_LEFT + DIALOG_MARGIN;
        int label_y = input_y + (INPUT_HEIGHT + font->ascent) / 2 - 2;  // Move up 2 pixels
        XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, label_x, label_y, 
                         (FcChar8*)"New Name:", 9);
        
        // Update InputField position and size if needed
        if (dialog->input_field) {
            dialog->input_field->x = input_x;
            dialog->input_field->y = input_y;
            dialog->input_field->width = input_w;
            dialog->input_field->height = INPUT_HEIGHT;
            
            // Draw the InputField using toolkit function
            inputfield_render(dialog->input_field, canvas->canvas_render, dpy, canvas->xft_draw);
        }
    }
    
    // Clean up
    XftColorFree(dpy, canvas->visual, canvas->colormap, &xft_text);
    // No need to destroy - using cached XftDraw
}

// Draw checkerboard pattern for delete confirmation dialog
static void draw_checkerboard_pattern(Picture dest, int x, int y, int w, int h) {
    Display *dpy = itn_core_get_display();
    
    // Use same checker size as scrollbars (2x2 pixels)
    int checker_size = 2;
    
    // Draw checkerboard pattern
    for (int row = 0; row < h; row += checker_size) {
        for (int col = 0; col < w; col += checker_size) {
            // Alternate between white and gray based on position
            bool is_white = ((row / checker_size) + (col / checker_size)) % 2 == 0;
            XRenderColor *color = is_white ? &WHITE : &GRAY;
            
            int draw_w = (col + checker_size > w) ? w - col : checker_size;
            int draw_h = (row + checker_size > h) ? h - row : checker_size;
            
            XRenderFillRectangle(dpy, PictOpSrc, dest, color,
                               x + col, y + row, draw_w, draw_h);
        }
    }
}

// Render dialog content
void render_dialog_content(Canvas *canvas) {
    Dialog *dialog = get_dialog_for_canvas(canvas);
    if (!dialog) return;
    
    
    Display *dpy = itn_core_get_display();
    Picture dest = canvas->canvas_render;
    
    // Clear only the content area inside the borders to dialog gray
    int content_x = BORDER_WIDTH_LEFT;
    int content_y = BORDER_HEIGHT_TOP;
    int content_w = canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT_CLIENT;
    int content_h = canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM;
    XRenderFillRectangle(dpy, PictOpSrc, dest, &GRAY, content_x, content_y, content_w, content_h);
    
    // Calculate element positions
    int input_x, input_y, input_w, ok_x, ok_y, cancel_x, cancel_y;
    calculate_layout(dialog, &input_x, &input_y, &input_w, &ok_x, &ok_y, &cancel_x, &cancel_y);
    
    // For delete confirmation, draw checkerboard pattern as border
    if (dialog->dialog_type == DIALOG_DELETE_CONFIRM) {
        // Draw checkerboard pattern as an outline/border (10 pixels thick)
        int border_thickness = 10;
        int content_left = BORDER_WIDTH_LEFT;
        int content_top = BORDER_HEIGHT_TOP;
        int content_width = canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT_CLIENT;
        // int content_height = canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM;  // May be used later
        
        // Bottom area: encompasses the buttons
        // Start 2 pixels above buttons, end 4 pixels below buttons
        int bottom_start_y = ok_y - 2;
        int bottom_height = (ok_y + BUTTON_HEIGHT + 4) - bottom_start_y;
        
        // Top border
        draw_checkerboard_pattern(dest, content_left, content_top, 
                                content_width, border_thickness);
        
        // Left border (only up to where bottom area starts)
        draw_checkerboard_pattern(dest, content_left, 
                                content_top + border_thickness,
                                border_thickness, 
                                bottom_start_y - (content_top + border_thickness));
        
        // Right border (only up to where bottom area starts)
        draw_checkerboard_pattern(dest, 
                                content_left + content_width - border_thickness,
                                content_top + border_thickness,
                                border_thickness,
                                bottom_start_y - (content_top + border_thickness));
        
        // Bottom horizontal area that encompasses the buttons
        draw_checkerboard_pattern(dest, content_left, bottom_start_y,
                                content_width, bottom_height);
        
        // Add 3D inset border around the inner window area (after the checker pattern)
        // This creates a recessed look with light from top-left
        int inner_left = content_left + border_thickness;
        int inner_top = content_top + border_thickness;
        int inner_width = content_width - (2 * border_thickness);
        int inner_height = bottom_start_y - inner_top;
        
        // Left edge - black line (shadow)
        XRenderColor black = {0x0000, 0x0000, 0x0000, 0xffff};
        XRenderFillRectangle(dpy, PictOpOver, dest, &black,
                           inner_left, inner_top, 1, inner_height);
        
        // Top edge - black line (shadow)
        XRenderFillRectangle(dpy, PictOpOver, dest, &black,
                           inner_left, inner_top, inner_width, 1);
        
        // Right edge - double white line (highlight)
        XRenderColor white = {0xffff, 0xffff, 0xffff, 0xffff};
        XRenderFillRectangle(dpy, PictOpOver, dest, &white,
                           inner_left + inner_width - 2, inner_top, 2, inner_height);
        
        // Bottom edge - double white line (highlight)
        XRenderFillRectangle(dpy, PictOpOver, dest, &white,
                           inner_left, inner_top + inner_height - 2, inner_width, 2);
    }
    
    // Draw input box with inset 3D effect (for rename and execute dialogs)
    if (dialog->dialog_type == DIALOG_RENAME || dialog->dialog_type == DIALOG_EXECUTE_COMMAND) {
        draw_inset_box(dest, input_x, input_y, input_w, INPUT_HEIGHT);
    }
    
    // Draw toolkit buttons if they exist, otherwise fall back to old method
    if (dialog->ok_button && dialog->cancel_button) {
        // Update button positions based on layout
        dialog->ok_button->x = ok_x;
        dialog->ok_button->y = ok_y;
        dialog->cancel_button->x = cancel_x;
        dialog->cancel_button->y = cancel_y;

        // Render toolkit buttons
        Display *dpy = itn_core_get_display();
        button_render(dialog->ok_button, dest, dpy, canvas->xft_draw);
        button_render(dialog->cancel_button, dest, dpy, canvas->xft_draw);
    }
    
    // Render text and labels
    render_text_content(dialog, dest, input_x, input_y, input_w, ok_x, ok_y, cancel_x, cancel_y);
}

// Event handlers
bool dialogs_handle_key_press(XKeyEvent *event) {
    // Find the dialog for the active window
    Canvas *active = itn_focus_get_active();
    if (!active || active->type != DIALOG) return false;
    
    Dialog *dialog = get_dialog_for_canvas(active);
    if (!dialog) return false;
    
    // First, let InputField handle the key event
    if (dialog->input_field) {
        bool handled = inputfield_handle_key(dialog->input_field, event);
        if (handled) {
            redraw_canvas(dialog->canvas);
            return true;
        }
    }
    
    // If InputField didn't handle it, check for dialog-level keys
    KeySym keysym = XLookupKeysym(event, 0);
    
    // Handle Escape key - cancel dialog (only if dropdown is not open)
    if (keysym == XK_Escape) {
        if (dialog->input_field && dialog->input_field->dropdown_open) {
            // Let InputField handle Escape to close dropdown
            return false;
        }
        if (dialog->on_cancel) dialog->on_cancel();
        destroy_dialog(dialog);
        return true;
    }
    
    // Handle Enter key - accept dialog (only if dropdown is not open)
    if (keysym == XK_Return || keysym == XK_KP_Enter) {
        if (dialog->input_field && dialog->input_field->dropdown_open) {
            // Let InputField handle Enter to apply completion
            return false;
        }
        if (dialog->input_field && dialog->on_ok) {
            dialog->on_ok(dialog->input_field->text);
        }
        destroy_dialog(dialog);
        return true;
    }
    
    // All other key handling is done by InputField widget
    return false;
}

bool dialogs_handle_button_press(XButtonEvent *event) {
    if (!event) return false;
    
    // First check if this is a click on any InputField's completion dropdown
    for (Dialog *d = g_dialogs; d; d = d->next) {
        if (d->input_field && inputfield_is_completion_window(d->input_field, event->window)) {
            // This is a click on a completion dropdown
            
            // Handle scroll wheel events (Button4 = scroll up, Button5 = scroll down)
            if (event->button == Button4 || event->button == Button5) {
                // Call InputField's scroll handler
                int direction = (event->button == Button4) ? -1 : 1;
                inputfield_handle_dropdown_scroll(d->input_field, direction, itn_core_get_display());
                return true;  // Consume the event
            }
            
            // Only process left click (Button1)
            if (event->button == Button1) {
                if (inputfield_handle_completion_click(d->input_field, event->x, event->y, itn_core_get_display())) {
                    // Selection was made, hide the dropdown
                    inputfield_hide_completions(d->input_field, itn_core_get_display());
                    redraw_canvas(d->canvas);
                    return true;
                }
            }
            return false;
        }
    }
    
    Canvas *canvas = itn_canvas_find_by_window(event->window);
    
    if (!canvas || canvas->type != DIALOG) return false;
    
    Dialog *dialog = get_dialog_for_canvas(canvas);
    if (!dialog) return false;
    
    
    // For delete confirmation dialogs, don't handle input box clicks since there's no input
    if (dialog->dialog_type == DIALOG_DELETE_CONFIRM) {
        // Handle button clicks using toolkit
        if (dialog->ok_button && button_handle_press(dialog->ok_button, event->x, event->y)) {
            redraw_canvas(canvas);
            return true;
        }

        if (dialog->cancel_button && button_handle_press(dialog->cancel_button, event->x, event->y)) {
            redraw_canvas(canvas);
            return true;
        }

        // Let other clicks (title bar, resize) go to intuition
        return false;
    }
    
    // Handle button clicks using toolkit for all dialog types
    if (dialog->ok_button && button_handle_press(dialog->ok_button, event->x, event->y)) {
        redraw_canvas(canvas);
        return true;
    }

    if (dialog->cancel_button && button_handle_press(dialog->cancel_button, event->x, event->y)) {
        redraw_canvas(canvas);
        return true;
    }

    // Get layout positions for input field
    int input_x, input_y, input_w, ok_x, ok_y, cancel_x, cancel_y;
    calculate_layout(dialog, &input_x, &input_y, &input_w, &ok_x, &ok_y, &cancel_x, &cancel_y);
    
    // Check if click is in input box
    if (event->x >= input_x && event->x < input_x + input_w &&
        event->y >= input_y && event->y < input_y + INPUT_HEIGHT) {
        
        // Pass click to InputField widget
        if (dialog->input_field) {
            // Update InputField position
            dialog->input_field->x = input_x;
            dialog->input_field->y = input_y;
            dialog->input_field->width = input_w;
            
            // Handle click in InputField
            if (inputfield_handle_click(dialog->input_field, event->x, event->y)) {
                // Now calculate and set cursor position
                int pos = inputfield_pos_from_x(dialog->input_field, event->x,
                                               itn_core_get_display());
                dialog->input_field->cursor_pos = pos;
                dialog->input_field->mouse_selecting = true;
                dialog->input_field->mouse_select_start = pos;
            }
            
            redraw_canvas(canvas);
        }
        
        return true;
    }
    
    // Let other clicks (title bar, resize) go to intuition
    return false;
}

bool dialogs_handle_button_release(XButtonEvent *event) {
    if (!event) return false;

    Canvas *canvas = itn_canvas_find_by_window(event->window);
    if (!canvas || canvas->type != DIALOG) return false;

    Dialog *dialog = get_dialog_for_canvas(canvas);
    if (!dialog) return false;

    // Handle mouse release for InputField selection
    if (dialog->input_field && dialog->input_field->mouse_selecting) {
        inputfield_handle_mouse_release(dialog->input_field, event->x, event->y);
        redraw_canvas(dialog->canvas);
        return true;
    }

    // Handle button releases using toolkit
    if (dialog->ok_button) {
        if (button_handle_release(dialog->ok_button, event->x, event->y)) {
            // Button was clicked - check if it's activated
            if (button_is_clicked(dialog->ok_button)) {
                if (dialog->on_ok) {
                    // For dialogs with input fields, pass the text
                    if (dialog->input_field) {
                        dialog->on_ok(inputfield_get_text(dialog->input_field));
                    } else {
                        // For delete dialog, pass the message
                        dialog->on_ok(dialog->text_buffer);
                    }
                }
                destroy_dialog(dialog);
                return true;
            }
            redraw_canvas(canvas);
            return true;
        }
    }

    if (dialog->cancel_button) {
        if (button_handle_release(dialog->cancel_button, event->x, event->y)) {
            // Button was clicked - check if it's activated
            if (button_is_clicked(dialog->cancel_button)) {
                if (dialog->on_cancel) {
                    dialog->on_cancel();
                }
                destroy_dialog(dialog);
                return true;
            }
            redraw_canvas(canvas);
            return true;
        }
    }

    return false;
}

bool dialogs_handle_motion(XMotionEvent *event) {
    if (!event) return false;
    
    Canvas *canvas = itn_canvas_find_by_window(event->window);
    if (!canvas || canvas->type != DIALOG) return false;
    
    Dialog *dialog = get_dialog_for_canvas(canvas);
    if (!dialog) return false;
    
    // Handle mouse motion for InputField selection
    if (dialog->input_field && dialog->input_field->mouse_selecting) {
        if (inputfield_handle_mouse_motion(dialog->input_field, event->x, event->y, itn_core_get_display())) {
            redraw_canvas(dialog->canvas);
            return true;
        }
    }
    
    return false;
}

// Delete confirmation dialog - CRITICAL FOR USER DATA SAFETY
static void (*g_delete_confirm_callback)(void) = NULL;
static void (*g_delete_cancel_callback)(void) = NULL;

static void delete_confirm_ok(const char *unused) {
    (void)unused;
    if (g_delete_confirm_callback) {
        void (*callback)(void) = g_delete_confirm_callback;
        g_delete_confirm_callback = NULL;
        g_delete_cancel_callback = NULL;
        callback();
    }
}

static void delete_confirm_cancel(void) {
    if (g_delete_cancel_callback) {
        void (*callback)(void) = g_delete_cancel_callback;
        g_delete_confirm_callback = NULL;
        g_delete_cancel_callback = NULL;
        callback();
    }
}

void show_delete_confirmation(const char *message,
                             void (*on_confirm)(void),
                             void (*on_cancel)(void)) {
    if (!message || !on_confirm) return;

    // Store callbacks globally (simple approach for now)
    g_delete_confirm_callback = on_confirm;
    g_delete_cancel_callback = on_cancel ? on_cancel : delete_confirm_cancel;

    // Create dialog using consistent lifecycle function (returns NULL on failure)
    Dialog *dialog = create_dialog(DIALOG_DELETE_CONFIRM, "Delete Confirmation", 450, 220);
    if (!dialog) {
        log_error("[ERROR] Failed to create delete confirmation dialog - operation cancelled");
        return;  // Graceful degradation - safer to cancel delete than proceed without confirmation
    }

    // Set message text and callbacks
    if (strlen(message) >= NAME_SIZE) {
        log_error("[WARNING] Delete confirmation message truncated: %s", message);
    }
    strncpy(dialog->text_buffer, message, NAME_SIZE - 1);
    dialog->text_buffer[NAME_SIZE - 1] = '\0';
    dialog->on_ok = delete_confirm_ok;
    dialog->on_cancel = delete_confirm_cancel;

    // Create toolkit buttons (different positions for delete dialog)
    dialog->ok_button = button_create(10, 150, BUTTON_WIDTH, BUTTON_HEIGHT, "OK", dialog->font);
    dialog->cancel_button = button_create(340, 150, BUTTON_WIDTH, BUTTON_HEIGHT, "Cancel", dialog->font);

    // Add to dialog list
    dialog->next = g_dialogs;
    g_dialogs = dialog;

    // Show the dialog and set it as active window
    XMapRaised(itn_core_get_display(), dialog->canvas->win);
    itn_focus_set_active(dialog->canvas);
    redraw_canvas(dialog->canvas);
}

// ========================
// Progress Dialog Implementation
// ========================

// Global progress dialog list
static ProgressDialog *g_progress_dialogs = NULL;

// Show progress dialog
ProgressDialog* show_progress_dialog(ProgressOperation op, const char *title) {
    ProgressDialog *dialog = calloc(1, sizeof(ProgressDialog));
    if (!dialog) return NULL;
    
    dialog->operation = op;
    dialog->percent = 0.0f;
    dialog->current_file[0] = '\0';
    dialog->pipe_fd = -1;
    dialog->child_pid = 0;
    dialog->abort_requested = false;
    dialog->on_abort = NULL;

    // Create canvas window (400x164)
    dialog->canvas = create_canvas(NULL, 200, 150, 400, 164, DIALOG);
    if (!dialog->canvas) {
        log_error("[ERROR] show_progress_dialog: failed to create canvas\n");
        free(dialog);
        return NULL;
    }
    
    // Set title based on operation
    const char *op_title = title ? title : 
        (op == PROGRESS_MOVE ? "Moving Files..." :
         op == PROGRESS_COPY ? "Copying Files..." :
         "Deleting Files...");
    
    dialog->canvas->title_base = strdup(op_title);
    dialog->canvas->title_change = NULL;
    dialog->canvas->bg_color = GRAY;
    dialog->canvas->disable_scrollbars = true;
    
    // Add to progress dialog list
    dialog->next = g_progress_dialogs;
    g_progress_dialogs = dialog;
    
    // Show the dialog
    XMapRaised(itn_core_get_display(), dialog->canvas->win);
    itn_focus_set_active(dialog->canvas);
    
    // Force X11 to process the map request
    XSync(itn_core_get_display(), False);
    
    redraw_canvas(dialog->canvas);
    
    // Force an immediate flush to ensure the window is visible
    XFlush(itn_core_get_display());
    
    return dialog;
}

// Update progress dialog
void update_progress_dialog(ProgressDialog *dialog, const char *file, float percent) {
    if (!dialog) return;
    
    if (file) {
        strncpy(dialog->current_file, file, PATH_SIZE - 1);
        dialog->current_file[PATH_SIZE - 1] = '\0';
    }
    
    if (percent >= 0.0f && percent <= 100.0f) {
        dialog->percent = percent;
    }
    
    redraw_canvas(dialog->canvas);
    XFlush(itn_core_get_display());  // Force immediate display update
}

// Close progress dialog
void close_progress_dialog(ProgressDialog *dialog) {
    if (!dialog) return;
    
    // Remove from list
    if (g_progress_dialogs == dialog) {
        g_progress_dialogs = dialog->next;
    } else {
        for (ProgressDialog *d = g_progress_dialogs; d; d = d->next) {
            if (d->next == dialog) {
                d->next = dialog->next;
                break;
            }
        }
    }
    
    // Clean up
    if (dialog->canvas) {
        itn_canvas_destroy(dialog->canvas);
    }
    if (dialog->progress_bar) {
        progressbar_destroy(dialog->progress_bar);
    }
    free(dialog);
}

// Close progress dialog by canvas (called from intuition.c when window X button is clicked)
void close_progress_dialog_by_canvas(Canvas *canvas) {
    if (!canvas) return;
    
    ProgressDialog *dialog = get_progress_dialog_for_canvas(canvas);
    if (dialog) {
        // If there's a child process running, abort it (same as clicking Abort button)
        if (dialog->child_pid > 0) {
            // Set abort flag so child knows to clean up
            dialog->abort_requested = true;
            
            // Send SIGTERM to child process to trigger clean abort
            kill(dialog->child_pid, SIGTERM);
            
            // Give child a moment to clean up (it will remove partial files)
            // The actual cleanup and dialog removal happens in workbench_check_progress_dialogs
            // when it detects the child has exited
            
            // Don't remove from list or free here - let the normal cleanup happen
            // This ensures partial files are properly cleaned up
            return;
        }
        
        // No child process - just clean up the dialog immediately
        // Remove from list
        if (g_progress_dialogs == dialog) {
            g_progress_dialogs = dialog->next;
        } else {
            for (ProgressDialog *d = g_progress_dialogs; d; d = d->next) {
                if (d->next == dialog) {
                    d->next = dialog->next;
                    break;
                }
            }
        }
        
        // Call abort callback if it exists
        if (dialog->on_abort) {
            dialog->on_abort();
        }
        
        // Don't destroy canvas here - intuition.c will do it
        dialog->canvas = NULL;
        
        free(dialog);
    }
}


// Check if canvas is a progress dialog
bool is_progress_dialog(Canvas *canvas) {
    if (!canvas) return false;
    for (ProgressDialog *dialog = g_progress_dialogs; dialog; dialog = dialog->next) {
        if (dialog->canvas == canvas) return true;
    }
    return false;
}

// Get progress dialog for canvas
ProgressDialog* get_progress_dialog_for_canvas(Canvas *canvas) {
    if (!canvas) return NULL;
    for (ProgressDialog *dialog = g_progress_dialogs; dialog; dialog = dialog->next) {
        if (dialog->canvas == canvas) return dialog;
    }
    return NULL;
}

// Get all progress dialogs (for monitoring)
ProgressDialog* get_all_progress_dialogs(void) {
    return g_progress_dialogs;
}

// Add progress dialog to global list
void add_progress_dialog_to_list(ProgressDialog *dialog) {
    if (!dialog) return;
    dialog->next = g_progress_dialogs;
    g_progress_dialogs = dialog;
}

// Remove progress dialog from global list
void remove_progress_dialog_from_list(ProgressDialog *dialog) {
    if (!dialog) return;
    
    if (g_progress_dialogs == dialog) {
        g_progress_dialogs = dialog->next;
    } else {
        for (ProgressDialog *d = g_progress_dialogs; d; d = d->next) {
            if (d->next == dialog) {
                d->next = dialog->next;
                break;
            }
        }
    }
}

// Create progress window (canvas) for a dialog
Canvas* create_progress_window(ProgressOperation op, const char *title) {
    // Determine proper title based on operation
    const char *window_title = title;
    if (!window_title) {
        switch (op) {
            case PROGRESS_COPY: window_title = "Copying Files"; break;
            case PROGRESS_MOVE: window_title = "Moving Files"; break;
            case PROGRESS_DELETE: window_title = "Deleting Files"; break;
            default: window_title = "Progress"; break;
        }
    }
    
    // Center on screen
    Display *dpy = itn_core_get_display();
    int screen = DefaultScreen(dpy);
    int screen_width = DisplayWidth(dpy, screen);
    int screen_height = DisplayHeight(dpy, screen);
    int x = (screen_width - 400) / 2;
    int y = (screen_height - 164) / 2;

    Canvas *canvas = create_canvas(NULL, x, y, 400, 164, DIALOG_PROGRESS);
    if (!canvas) {
        log_error("[ERROR] create_progress_window: failed to create canvas\n");
        return NULL;
    }
    
    // Set title for AmiWB window rendering (XStoreName doesn't work for AmiWB windows)
    canvas->title_base = strdup(window_title);
    
    // DO NOT make it modal - user should be able to continue working
    // Removed modal property setting
    
    // Show, raise and make it the active window
    XMapRaised(dpy, canvas->win);
    itn_focus_set_active(canvas);  // Make this the active window (handles focus and active state)
    XSync(dpy, False);
    
    return canvas;
}

// Render progress dialog content
void render_progress_dialog_content(Canvas *canvas) {
    ProgressDialog *dialog = get_progress_dialog_for_canvas(canvas);
    if (!dialog) {
        return;
    }
    
    Display *dpy = itn_core_get_display();
    Picture dest = canvas->canvas_render;
    
    
    if (dest == None) {
        static bool dest_none_debug_done = false;
        if (!dest_none_debug_done) {
            log_error("[ERROR] render_progress_dialog_content: canvas_render is None!\n");
            dest_none_debug_done = true;
        }
        return;
    }
    
    XftFont *font = get_font();
    if (!font) {
        static bool no_font_debug_done = false;
        if (!no_font_debug_done) {
            log_error("[ERROR] render_progress_dialog_content: no font!\n");
            no_font_debug_done = true;
        }
        return;
    }
    
    // Clear content area to dialog gray
    int content_x = BORDER_WIDTH_LEFT;
    int content_y = BORDER_HEIGHT_TOP;
    int content_w = canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT_CLIENT;
    int content_h = canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM;
    
    XRenderFillRectangle(dpy, PictOpSrc, dest, &GRAY, content_x, content_y, content_w, content_h);
    
    // Use cached XftDraw
    if (!canvas->xft_draw) {
        static bool no_xft_debug_done = false;
        if (!no_xft_debug_done) {
            log_error("[ERROR] render_progress_dialog_content: canvas->xft_draw is NULL!\n");
            no_xft_debug_done = true;
        }
        return;
    }
    

    XRenderColor text_color = BLACK;
    XftColor xft_text;
    XftColorAllocValue(dpy, canvas->visual, canvas->colormap, &text_color, &xft_text);

    // Line 1: Current file (at top, with operation prefix)
    int text_y = content_y + 20;
    const char *op_prefix = dialog->operation == PROGRESS_MOVE ? "Moving: " :
                           dialog->operation == PROGRESS_COPY ? "Copying: " :
                           dialog->operation == PROGRESS_DELETE ? "Deleting: " :
                           dialog->operation == PROGRESS_EXTRACT ? "File: " :
                           "";

    char display_text[PATH_SIZE + 20];
    snprintf(display_text, sizeof(display_text), "%s%s", op_prefix, dialog->current_file);

    // Truncate with "..." if too long
    XGlyphInfo text_ext;
    XftTextExtentsUtf8(dpy, font, (FcChar8*)display_text, strlen(display_text), &text_ext);
    int max_width = content_w - 40;  // Leave margin

    if (text_ext.xOff > max_width) {
        // Truncate and add ...
        int len = strlen(display_text);
        while (len > 3) {
            display_text[len - 1] = '\0';
            display_text[len - 2] = '.';
            display_text[len - 3] = '.';
            display_text[len - 4] = '.';
            len -= 4;
            XftTextExtentsUtf8(dpy, font, (FcChar8*)display_text, strlen(display_text), &text_ext);
            if (text_ext.xOff <= max_width) break;
        }
    }

    XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, content_x + 20, text_y,
                     (FcChar8*)display_text, strlen(display_text));

    // Line 2: Bytes and file count (below filename)
    int info_y = text_y + font->height + 2;
    char info_text[256];

    // Check if totals are known yet (child process may still be counting)
    if (dialog->bytes_total == -1 || dialog->files_total == -1) {
        // Still counting files and bytes in child process
        snprintf(info_text, sizeof(info_text), "Calculating size...");
    } else {
        // Format bytes in human-readable form
        if (dialog->bytes_total < 1024 * 1024) {
            // Show in KB for small files
            double bytes_kb = dialog->bytes_done / 1024.0;
            double total_kb = dialog->bytes_total / 1024.0;
            snprintf(info_text, sizeof(info_text), "%.1f KB / %.1f KB  (%d/%d files)",
                    bytes_kb, total_kb, dialog->files_done, dialog->files_total);
        } else if (dialog->bytes_total < 1024 * 1024 * 1024) {
            // Show in MB
            double bytes_mb = dialog->bytes_done / (1024.0 * 1024.0);
            double total_mb = dialog->bytes_total / (1024.0 * 1024.0);
            snprintf(info_text, sizeof(info_text), "%.1f MB / %.1f MB  (%d/%d files)",
                    bytes_mb, total_mb, dialog->files_done, dialog->files_total);
        } else {
            // Show in GB for large files
            double bytes_gb = dialog->bytes_done / (1024.0 * 1024.0 * 1024.0);
            double total_gb = dialog->bytes_total / (1024.0 * 1024.0 * 1024.0);
            snprintf(info_text, sizeof(info_text), "%.2f GB / %.2f GB  (%d/%d files)",
                    bytes_gb, total_gb, dialog->files_done, dialog->files_total);
        }
    }

    XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, content_x + 20, info_y,
                     (FcChar8*)info_text, strlen(info_text));

    // Progress bar position - 10px closer to info text
    int bar_x = content_x + 20;
    int bar_y = info_y + font->height - 8;
    int bar_width = content_w - 40;  // Resizes with window
    int bar_height = (font->height * 2) - 8;

    // Create progress bar widget lazily if it doesn't exist
    if (!dialog->progress_bar) {
        dialog->progress_bar = progressbar_create(bar_x, bar_y, bar_width, bar_height, font);
        if (dialog->progress_bar) {
            progressbar_set_show_percentage(dialog->progress_bar, true);
        }
    }

    // Update and render progress bar using toolkit widget
    if (dialog->progress_bar) {
        progressbar_set_percent(dialog->progress_bar, dialog->percent);
        progressbar_render(dialog->progress_bar, dest, dpy, canvas->xft_draw);
    }

    // Abort button - centered horizontally, 10px below bar
    int button_x = content_x + (content_w - BUTTON_WIDTH) / 2;
    int button_y = bar_y + bar_height + 10;
    
    // Draw button with 3D effect
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE,
                        button_x, button_y, 1, BUTTON_HEIGHT);  // Left
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE,
                        button_x, button_y, BUTTON_WIDTH, 1);  // Top
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK,
                        button_x + BUTTON_WIDTH - 1, button_y, 1, BUTTON_HEIGHT);  // Right
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK,
                        button_x, button_y + BUTTON_HEIGHT - 1, BUTTON_WIDTH, 1);  // Bottom
    XRenderFillRectangle(dpy, PictOpSrc, dest, &GRAY,
                        button_x + 1, button_y + 1, BUTTON_WIDTH - 2, BUTTON_HEIGHT - 2);  // Fill
    
    // Draw "Abort" text
    XGlyphInfo abort_ext;
    XftTextExtentsUtf8(dpy, font, (FcChar8*)"Abort", 5, &abort_ext);
    int abort_text_x = button_x + (BUTTON_WIDTH - abort_ext.xOff) / 2;
    int abort_text_y = button_y + (BUTTON_HEIGHT + font->ascent) / 2 - 2;
    XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, abort_text_x, abort_text_y,
                     (FcChar8*)"Abort", 5);
    
    XftColorFree(dpy, canvas->visual, canvas->colormap, &xft_text);
}