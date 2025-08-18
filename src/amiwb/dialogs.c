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
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>

// Global dialog list (for multiple dialogs)
static RenameDialog *g_dialogs = NULL;

// Forward declarations
static void hide_completion_dropdown(RenameDialog *dialog);

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
    dialog->dialog_type = DIALOG_RENAME;
    if (strlen(old_name) >= NAME_SIZE) {
        printf("[WARNING] Name too long, will be truncated: %s\n", old_name);
    }
    strncpy(dialog->text_buffer, old_name, NAME_SIZE - 1);
    dialog->text_buffer[NAME_SIZE - 1] = '\0';
    strncpy(dialog->original_name, old_name, NAME_SIZE - 1);
    dialog->original_name[NAME_SIZE - 1] = '\0';
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
    
    // Initialize completion fields
    dialog->completion_dropdown = NULL;
    dialog->completion_candidates = NULL;
    dialog->completion_count = 0;
    dialog->completion_selected = -1;
    dialog->completion_prefix[0] = '\0';
    dialog->completion_prefix_len = 0;
    
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

// Show execute command dialog
void show_execute_dialog(void (*on_ok)(const char *command),
                        void (*on_cancel)(void)) {
    if (!on_ok || !on_cancel) return;
    
    RenameDialog *dialog = malloc(sizeof(RenameDialog));
    if (!dialog) return;
    
    // Initialize dialog state
    dialog->dialog_type = DIALOG_EXECUTE_COMMAND;
    dialog->text_buffer[0] = '\0';  // Start with empty command
    dialog->original_name[0] = '\0';  // Not used for execute dialog
    dialog->cursor_pos = 0;
    dialog->selection_start = -1;
    dialog->selection_end = -1;
    dialog->visible_start = 0;
    dialog->input_has_focus = false;
    dialog->ok_button_pressed = false;
    dialog->cancel_button_pressed = false;
    dialog->on_ok = on_ok;
    dialog->on_cancel = on_cancel;
    dialog->user_data = NULL;
    
    // Initialize completion fields
    dialog->completion_dropdown = NULL;
    dialog->completion_candidates = NULL;
    dialog->completion_count = 0;
    dialog->completion_selected = -1;
    dialog->completion_prefix[0] = '\0';
    dialog->completion_prefix_len = 0;
    
    // Create canvas window (450x160 initial size)
    dialog->canvas = create_canvas(NULL, 200, 150, 450, 160, DIALOG);
    if (!dialog->canvas) {
        free(dialog);
        return;
    }
    
    // Set dialog properties
    dialog->canvas->title = strdup("Execute");
    dialog->canvas->bg_color = GRAY;  // Standard dialog gray
    dialog->canvas->disable_scrollbars = true;  // Disable scrollbars for dialogs
    
    // Add to dialog list
    dialog->next = g_dialogs;
    g_dialogs = dialog;
    
    // Show the dialog and set it as active window
    XMapRaised(get_display(), dialog->canvas->win);
    set_active_window(dialog->canvas);
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
    
    // Clean up completion dropdown first
    hide_completion_dropdown(dialog);
    
    // Clean up canvas and memory
    if (dialog->canvas) {
        destroy_canvas(dialog->canvas);
    }
    free(dialog);
}

// ========================
// Completion Helper Functions
// ========================

// Free completion candidates
static void free_completion_candidates(RenameDialog *dialog) {
    if (dialog->completion_candidates) {
        for (int i = 0; i < dialog->completion_count; i++) {
            free(dialog->completion_candidates[i]);
        }
        free(dialog->completion_candidates);
        dialog->completion_candidates = NULL;
        dialog->completion_count = 0;
        dialog->completion_selected = -1;
    }
}

// Hide and destroy completion dropdown
static void hide_completion_dropdown(RenameDialog *dialog) {
    if (dialog->completion_dropdown) {
        destroy_canvas(dialog->completion_dropdown);
        dialog->completion_dropdown = NULL;
    }
    free_completion_candidates(dialog);
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
        char *result = malloc(home_len + path_len + 1);  // +1 for null terminator
        if (!result) {
            printf("[ERROR] Failed to allocate memory for path expansion\n");
            return strdup(path);  // Return original on failure
        }
        
        // Safe string copy with snprintf
        snprintf(result, home_len + path_len + 1, "%s%s", home, path + 1);
        return result;
    }
    
    return strdup(path);
}

// Find completions for the given partial path
static void find_completions(RenameDialog *dialog, const char *partial) {
    free_completion_candidates(dialog);
    
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
            printf("[ERROR] Directory path too long\n");
            free(expanded);
            return;
        }
        strncpy(dir_path, expanded, dir_len);
        dir_path[dir_len] = '\0';
        
        // Safe copy of prefix
        strncpy(prefix, last_slash + 1, NAME_SIZE - 1);
        prefix[NAME_SIZE - 1] = '\0';
    } else {
        // No directory, use current directory
        strncpy(dir_path, "./", PATH_SIZE - 1);
        dir_path[PATH_SIZE - 1] = '\0';
        
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
    dialog->completion_candidates = malloc(capacity * sizeof(char *));
    dialog->completion_count = 0;
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        // Skip . and ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        
        // Check if name starts with prefix (case-insensitive)
        if (strncasecmp(entry->d_name, prefix, strlen(prefix)) == 0) {
            // Grow array if needed
            if (dialog->completion_count >= capacity) {
                capacity *= 2;
                dialog->completion_candidates = realloc(dialog->completion_candidates,
                                                       capacity * sizeof(char *));
            }
            
            // Check if it's a directory
            char full_path[PATH_SIZE];
            int ret = snprintf(full_path, sizeof(full_path), "%s%s", expanded_dir, entry->d_name);
            if (ret >= PATH_SIZE) {
                printf("[WARNING] Path too long for completion, skipping: %s%s\n", expanded_dir, entry->d_name);
                continue;
            }
            struct stat st;
            bool is_dir = (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode));
            
            // Add to candidates (with / suffix for directories)
            if (is_dir) {
                dialog->completion_candidates[dialog->completion_count] = 
                    malloc(strlen(entry->d_name) + 2);
                sprintf(dialog->completion_candidates[dialog->completion_count], 
                        "%s/", entry->d_name);
            } else {
                dialog->completion_candidates[dialog->completion_count] = 
                    strdup(entry->d_name);
            }
            dialog->completion_count++;
        }
    }
    
    closedir(dir);
    
    // Sort completions
    if (dialog->completion_count > 0) {
        qsort(dialog->completion_candidates, dialog->completion_count,
              sizeof(char *), completion_compare);
    }
    
    // Store the prefix info safely
    strncpy(dialog->completion_prefix, partial, sizeof(dialog->completion_prefix) - 1);
    dialog->completion_prefix[sizeof(dialog->completion_prefix) - 1] = '\0';
    dialog->completion_prefix_len = strlen(partial) - strlen(prefix);
    
    free(expanded);
    free(expanded_dir);
}

// Apply selected completion to text buffer
static void apply_completion(RenameDialog *dialog, int index) {
    if (index < 0 || index >= dialog->completion_count) return;
    
    // Build the completed path safely
    char completed[PATH_SIZE];
    if (dialog->completion_prefix_len >= PATH_SIZE) {
        printf("[ERROR] Completion prefix too long\n");
        return;
    }
    strncpy(completed, dialog->completion_prefix, dialog->completion_prefix_len);
    completed[dialog->completion_prefix_len] = '\0';
    
    // Safe concatenation
    size_t current_len = strlen(completed);
    size_t candidate_len = strlen(dialog->completion_candidates[index]);
    if (current_len + candidate_len >= PATH_SIZE) {
        printf("[ERROR] Completed path too long\n");
        return;
    }
    strncat(completed, dialog->completion_candidates[index], PATH_SIZE - current_len - 1);
    
    // Replace text buffer safely
    strncpy(dialog->text_buffer, completed, sizeof(dialog->text_buffer) - 1);
    dialog->text_buffer[sizeof(dialog->text_buffer) - 1] = '\0';
    dialog->cursor_pos = strlen(dialog->text_buffer);
    
    // Hide dropdown
    hide_completion_dropdown(dialog);
    
    // Redraw dialog
    redraw_canvas(dialog->canvas);
}

// Show completion dropdown
static void show_completion_dropdown(RenameDialog *dialog) {
    if (dialog->completion_count == 0) return;
    
    // If only one completion, apply it directly
    if (dialog->completion_count == 1) {
        apply_completion(dialog, 0);
        return;
    }
    
    // Calculate dropdown position and size
    int x = dialog->canvas->x + BORDER_WIDTH_LEFT + DIALOG_MARGIN + 80; // Below input box
    int y = dialog->canvas->y + BORDER_HEIGHT_TOP + 60;
    int width = 350;  // Match input box width
    int item_height = 20;
    int max_items = 10;  // Show max 10 items
    int show_items = dialog->completion_count < max_items ? dialog->completion_count : max_items;
    int height = show_items * item_height + 4;  // +4 for borders
    
    // Create dropdown canvas
    if (dialog->completion_dropdown) {
        destroy_canvas(dialog->completion_dropdown);
    }
    
    dialog->completion_dropdown = create_canvas(NULL, x, y, width, height, MENU);
    if (!dialog->completion_dropdown) return;
    
    // Initialize selection to first item
    dialog->completion_selected = 0;
    
    // Map and raise the dropdown
    XMapRaised(get_display(), dialog->completion_dropdown->win);
    redraw_canvas(dialog->completion_dropdown);
}

// Check if canvas is a completion dropdown
bool is_completion_dropdown(Canvas *canvas) {
    if (!canvas || canvas->type != MENU) return false;
    for (RenameDialog *dialog = g_dialogs; dialog; dialog = dialog->next) {
        if (dialog->completion_dropdown == canvas) return true;
    }
    return false;
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

// Helper to get font (from render.c)
extern XftFont *get_font(void);

// Forward declarations for internal functions
static void render_input_text(RenameDialog *dialog, XftFont *font, 
                             int text_x, int text_y, int text_width);
static int calculate_visible_end(RenameDialog *dialog, XftFont *font, int available_width);
static void draw_checkerboard_pattern(Picture dest, int x, int y, int w, int h);

// Render text content with cursor and selection
static void render_text_content(RenameDialog *dialog, Picture dest, 
                               int input_x, int input_y, int input_w,
                               int ok_x, int ok_y, int cancel_x, int cancel_y) {
    Display *dpy = get_display();
    Canvas *canvas = dialog->canvas;
    XftFont *font = get_font();
    if (!font) return;
    
    // Use cached XftDraw for text rendering
    if (!canvas->xft_draw) {
        printf("[WARNING] No cached XftDraw for rename dialog\n");
        return;
    }
    
    // Draw centered title above input box
    XRenderColor text_color = BLACK;
    XftColor xft_text;
    XftColorAllocValue(dpy, canvas->visual, canvas->colormap, &text_color, &xft_text);
    
    if (dialog->dialog_type == DIALOG_DELETE_CONFIRM) {
        int content_width = canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT;
        int line_y = BORDER_HEIGHT_TOP + 30;
        int text_left_x = BORDER_WIDTH_LEFT + 15;  // Left margin inside inner window
        
        // Line 1: "Warning" centered
        const char *warning = "Warning";
        XGlyphInfo warning_ext;
        XftTextExtentsUtf8(dpy, font, (FcChar8*)warning, strlen(warning), &warning_ext);
        int warning_x = BORDER_WIDTH_LEFT + (content_width - warning_ext.xOff) / 2;
        XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, warning_x, line_y, 
                         (FcChar8*)warning, strlen(warning));
        line_y += 30;
        
        // Line 2: "Once something is deleted it's" - left aligned
        const char *line2 = "Once something is deleted it's";
        XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_left_x, line_y,
                         (FcChar8*)line2, strlen(line2));
        line_y += 18;
        
        // Line 3: "going straight to willoughby!" - left aligned
        const char *line3 = "going straight to willoughby!";
        XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_left_x, line_y,
                         (FcChar8*)line3, strlen(line3));
        line_y += 25;
        
        // Line 4: "Is it really Ok to delete:" - left aligned
        const char *line4 = "Is it really Ok to delete:";
        XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_left_x, line_y,
                         (FcChar8*)line4, strlen(line4));
        line_y += 18;
        
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
        int content_width = canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT;
        int title_x = BORDER_WIDTH_LEFT + (content_width - title_ext.xOff) / 2;
        int title_y = BORDER_HEIGHT_TOP + 20;
        XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, title_x, title_y, 
                         (FcChar8*)title_text, strlen(title_text));
        
        // Draw "Command:" label to the left of input box (same row)
        int label_x = BORDER_WIDTH_LEFT + DIALOG_MARGIN;
        int label_y = input_y + (INPUT_HEIGHT + font->ascent) / 2 - 2;  // Move up 2 pixels
        XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, label_x, label_y, 
                         (FcChar8*)"Command:", 8);
        
        // Render input box text with cursor and selection 
        // Calculate text Y to center it within the input box
        int text_y = input_y + (INPUT_HEIGHT + font->ascent) / 2 - 2;  // Move up 2 pixels
        // Calculate one character width for proper padding
        XGlyphInfo char_info;
        XftTextExtentsUtf8(dpy, font, (FcChar8*)" ", 1, &char_info);
        int char_width = char_info.xOff;
        render_input_text(dialog, font, input_x + char_width + 2, text_y, input_w - (char_width + 2) * 2);
    } else {
        // For rename dialog, show the original prompt
        char title_text[256];
        int ret = snprintf(title_text, sizeof(title_text), "Enter a new name for '%s'.", 
                 strlen(dialog->original_name) > 0 ? dialog->original_name : "file");
        if (ret >= (int)sizeof(title_text)) {
            printf("[ERROR] Dialog title too long, using shortened version\n");
            snprintf(title_text, sizeof(title_text), "Enter a new name.");
        }
        
        XGlyphInfo title_ext;
        XftTextExtentsUtf8(dpy, font, (FcChar8*)title_text, strlen(title_text), &title_ext);
        int content_width = canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT;
        int title_x = BORDER_WIDTH_LEFT + (content_width - title_ext.xOff) / 2;
        int title_y = BORDER_HEIGHT_TOP + 20;
        XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, title_x, title_y, 
                         (FcChar8*)title_text, strlen(title_text));
        
        // Draw "New Name:" label to the left of input box (same row)
        int label_x = BORDER_WIDTH_LEFT + DIALOG_MARGIN;
        int label_y = input_y + (INPUT_HEIGHT + font->ascent) / 2 - 2;  // Move up 2 pixels
        XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, label_x, label_y, 
                         (FcChar8*)"New Name:", 9);
        
        // Render input box text with cursor and selection 
        // Calculate text Y to center it within the input box
        int text_y = input_y + (INPUT_HEIGHT + font->ascent) / 2 - 2;  // Move up 2 pixels
        // Calculate one character width for proper padding
        XGlyphInfo char_info;
        XftTextExtentsUtf8(dpy, font, (FcChar8*)" ", 1, &char_info);
        int char_width = char_info.xOff;
        render_input_text(dialog, font, input_x + char_width + 2, text_y, input_w - (char_width + 2) * 2);
    }
    
    // Draw button labels
    XRenderColor button_text_color = dialog->ok_button_pressed ? WHITE : BLACK;
    XftColor xft_button;
    XftColorAllocValue(dpy, canvas->visual, canvas->colormap, &button_text_color, &xft_button);
    
    // Center "OK" in button
    XGlyphInfo ok_ext;
    XftTextExtentsUtf8(dpy, font, (FcChar8*)"OK", 2, &ok_ext);
    int ok_text_x = ok_x + (BUTTON_WIDTH - ok_ext.xOff) / 2;
    int ok_text_y = ok_y + (BUTTON_HEIGHT + font->ascent) / 2 - 2;
    XftDrawStringUtf8(canvas->xft_draw, &xft_button, font, ok_text_x, ok_text_y, (FcChar8*)"OK", 2);
    
    // Update button text color for Cancel button
    button_text_color = dialog->cancel_button_pressed ? WHITE : BLACK;
    XftColorAllocValue(dpy, canvas->visual, canvas->colormap, &button_text_color, &xft_button);
    
    // Center "Cancel" in button  
    XGlyphInfo cancel_ext;
    XftTextExtentsUtf8(dpy, font, (FcChar8*)"Cancel", 6, &cancel_ext);
    int cancel_text_x = cancel_x + (BUTTON_WIDTH - cancel_ext.xOff) / 2;
    int cancel_text_y = cancel_y + (BUTTON_HEIGHT + font->ascent) / 2 - 2;
    XftDrawStringUtf8(canvas->xft_draw, &xft_button, font, cancel_text_x, cancel_text_y, (FcChar8*)"Cancel", 6);
    
    // Clean up
    XftColorFree(dpy, canvas->visual, canvas->colormap, &xft_text);
    XftColorFree(dpy, canvas->visual, canvas->colormap, &xft_button);
    // No need to destroy - using cached XftDraw
}

// Render input text with cursor position and selection highlighting
static void render_input_text(RenameDialog *dialog, XftFont *font, 
                             int text_x, int text_y, int text_width) {
    Display *dpy = get_display();
    Canvas *canvas = dialog->canvas;
    if (!canvas->xft_draw) return;
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
            XftDrawStringUtf8(canvas->xft_draw, &xft_white, font, x, text_y, (FcChar8*)ch, 1);
        } else {
            // Use black text on normal background
            XftDrawStringUtf8(canvas->xft_draw, &xft_black, font, x, text_y, (FcChar8*)ch, 1);
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

// Draw checkerboard pattern for delete confirmation dialog
static void draw_checkerboard_pattern(Picture dest, int x, int y, int w, int h) {
    Display *dpy = get_display();
    
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

// Render completion dropdown content
void render_completion_dropdown(Canvas *canvas) {
    if (!canvas) return;
    
    // Find the dialog that owns this dropdown
    RenameDialog *dialog = NULL;
    for (RenameDialog *d = g_dialogs; d; d = d->next) {
        if (d->completion_dropdown == canvas) {
            dialog = d;
            break;
        }
    }
    
    if (!dialog || dialog->completion_count == 0) return;
    
    Display *dpy = get_display();
    Picture dest = canvas->canvas_render;
    
    // Clear background to gray (standard Amiga dialog color)
    XRenderFillRectangle(dpy, PictOpSrc, dest, &GRAY, 0, 0, canvas->width, canvas->height);
    
    // Draw border
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, 0, 0, canvas->width, 1);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, 0, 0, 1, canvas->height);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, canvas->width - 1, 0, 1, canvas->height);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, 0, canvas->height - 1, canvas->width, 1);
    
    // Load font
    XftFont *font = get_font();
    if (!font) return;
    
    // Use cached XftDraw for text rendering
    if (!canvas->xft_draw) {
        printf("[WARNING] No cached XftDraw for delete dialog\n");
        return;
    }
    
    // Render items
    int item_height = 20;
    int max_items = (canvas->height - 4) / item_height;
    int start_item = 0;
    
    // Scroll to show selected item if needed
    if (dialog->completion_selected >= max_items) {
        start_item = dialog->completion_selected - max_items + 1;
    }
    
    for (int i = 0; i < max_items && start_item + i < dialog->completion_count; i++) {
        int y = 2 + i * item_height;
        int item_index = start_item + i;
        
        // Highlight selected item
        if (item_index == dialog->completion_selected) {
            XRenderFillRectangle(dpy, PictOpSrc, dest, &BLUE, 
                               2, y, canvas->width - 4, item_height);
        }
        
        // Draw text
        XRenderColor text_color = (item_index == dialog->completion_selected) ? WHITE : BLACK;
        XftColor xft_text;
        XftColorAllocValue(dpy, canvas->visual, canvas->colormap, &text_color, &xft_text);
        
        int text_x = 5;
        int text_y = y + item_height - 5;
        
        XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_x, text_y,
                         (FcChar8*)dialog->completion_candidates[item_index],
                         strlen(dialog->completion_candidates[item_index]));
        
        XftColorFree(dpy, canvas->visual, canvas->colormap, &xft_text);
    }
    
    // No need to destroy - using cached XftDraw
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
    
    // For delete confirmation, draw checkerboard pattern as border
    if (dialog->dialog_type == DIALOG_DELETE_CONFIRM) {
        // Draw checkerboard pattern as an outline/border (10 pixels thick)
        int border_thickness = 10;
        int content_left = BORDER_WIDTH_LEFT;
        int content_top = BORDER_HEIGHT_TOP;
        int content_width = canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT;
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
    
    // Handle keys differently when dropdown is shown
    if (dialog->completion_dropdown) {
        if (keysym == XK_Down) {
            dialog->completion_selected++;
            if (dialog->completion_selected >= dialog->completion_count) {
                dialog->completion_selected = 0;
            }
            redraw_canvas(dialog->completion_dropdown);
            return true;
        } else if (keysym == XK_Up) {
            dialog->completion_selected--;
            if (dialog->completion_selected < 0) {
                dialog->completion_selected = dialog->completion_count - 1;
            }
            redraw_canvas(dialog->completion_dropdown);
            return true;
        } else if (keysym == XK_Return || keysym == XK_KP_Enter) {
            // Apply selected completion and close dropdown
            if (dialog->completion_selected >= 0) {
                apply_completion(dialog, dialog->completion_selected);
            }
            return true;
        } else if (keysym == XK_Escape) {
            // Just hide dropdown, don't close dialog
            hide_completion_dropdown(dialog);
            redraw_canvas(dialog->canvas);
            return true;
        }
    }
    
    // Handle Escape key - cancel dialog (only when dropdown not shown)
    if (keysym == XK_Escape) {
        if (dialog->on_cancel) dialog->on_cancel();
        close_rename_dialog(dialog);
        return true;
    }
    
    // Handle Enter key - accept dialog (only when dropdown not shown)
    if (keysym == XK_Return || keysym == XK_KP_Enter) {
        if (dialog->on_ok) dialog->on_ok(dialog->text_buffer);
        close_rename_dialog(dialog);
        return true;
    }
    
    // Only handle input if the input box has focus
    if (!dialog->input_has_focus) return false;
    
    // Handle Tab key for completion (execute dialog only)
    if (keysym == XK_Tab && dialog->dialog_type == DIALOG_EXECUTE_COMMAND) {
        // Find completions for current text
        find_completions(dialog, dialog->text_buffer);
        show_completion_dropdown(dialog);
        return true;
    }
    
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
                if (len < NAME_SIZE - 1) {  // Don't overflow buffer
                    // Insert character at cursor position
                    memmove(&dialog->text_buffer[dialog->cursor_pos + 1],
                           &dialog->text_buffer[dialog->cursor_pos],
                           len - dialog->cursor_pos + 1);
                    dialog->text_buffer[dialog->cursor_pos] = (char)ch;
                    dialog->cursor_pos++;
                    
                    // Hide dropdown when typing
                    if (dialog->completion_dropdown) {
                        hide_completion_dropdown(dialog);
                    }
                    
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
    
    // Check if this is a click on a completion dropdown
    if (canvas && is_completion_dropdown(canvas)) {
        // Find the dialog that owns this dropdown
        RenameDialog *dialog = NULL;
        for (RenameDialog *d = g_dialogs; d; d = d->next) {
            if (d->completion_dropdown == canvas) {
                dialog = d;
                break;
            }
        }
        
        if (dialog) {
            // Calculate which item was clicked
            int item_height = 20;
            int clicked_item = (event->y - 2) / item_height;
            
            if (clicked_item >= 0 && clicked_item < dialog->completion_count) {
                // Apply the clicked completion
                apply_completion(dialog, clicked_item);
                return true;
            }
        }
        return false;
    }
    
    if (!canvas || canvas->type != DIALOG) return false;
    
    RenameDialog *dialog = get_dialog_for_canvas(canvas);
    if (!dialog) return false;
    
    
    // For delete confirmation dialogs, don't handle input box clicks since there's no input
    if (dialog->dialog_type == DIALOG_DELETE_CONFIRM) {
        // Only handle button clicks for delete confirmation
        int ok_x, ok_y, cancel_x, cancel_y;
        int dummy_x, dummy_y, dummy_w;
        calculate_layout(dialog, &dummy_x, &dummy_y, &dummy_w, &ok_x, &ok_y, &cancel_x, &cancel_y);
        
        // Check if click is on OK button
        if (event->x >= ok_x && event->x < ok_x + BUTTON_WIDTH &&
            event->y >= ok_y && event->y < ok_y + BUTTON_HEIGHT) {
            dialog->ok_button_pressed = true;
            redraw_canvas(canvas);
            return true;
        }
        
        // Check if click is on Cancel button
        if (event->x >= cancel_x && event->x < cancel_x + BUTTON_WIDTH &&
            event->y >= cancel_y && event->y < cancel_y + BUTTON_HEIGHT) {
            dialog->cancel_button_pressed = true;
            redraw_canvas(canvas);
            return true;
        }
        
        // Let other clicks (title bar, resize) go to intuition
        return false;
    }
    
    // Get layout positions for rename dialog
    int input_x, input_y, input_w, ok_x, ok_y, cancel_x, cancel_y;
    calculate_layout(dialog, &input_x, &input_y, &input_w, &ok_x, &ok_y, &cancel_x, &cancel_y);
    
    // Check if click is on OK button
    if (event->x >= ok_x && event->x < ok_x + BUTTON_WIDTH &&
        event->y >= ok_y && event->y < ok_y + BUTTON_HEIGHT) {
        dialog->ok_button_pressed = true;
        redraw_canvas(canvas);
        return true;
    }
    
    // Check if click is on Cancel button
    if (event->x >= cancel_x && event->x < cancel_x + BUTTON_WIDTH &&
        event->y >= cancel_y && event->y < cancel_y + BUTTON_HEIGHT) {
        dialog->cancel_button_pressed = true;
        redraw_canvas(canvas);
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
        return true;
    }
    
    // Let other clicks (title bar, resize) go to intuition
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
    
    RenameDialog *dialog = malloc(sizeof(RenameDialog));
    if (!dialog) return;
    
    // Initialize dialog state for delete confirmation
    dialog->dialog_type = DIALOG_DELETE_CONFIRM;
    if (strlen(message) >= NAME_SIZE) {
        printf("[WARNING] Delete confirmation message truncated: %s\n", message);
    }
    strncpy(dialog->text_buffer, message, NAME_SIZE - 1);
    dialog->text_buffer[NAME_SIZE - 1] = '\0';
    dialog->cursor_pos = 0;
    dialog->selection_start = -1;
    dialog->selection_end = -1;
    dialog->visible_start = 0;
    dialog->input_has_focus = false;  // No input for delete dialog
    dialog->ok_button_pressed = false;
    dialog->cancel_button_pressed = false;
    dialog->on_ok = delete_confirm_ok;
    dialog->on_cancel = delete_confirm_cancel;
    dialog->user_data = NULL;
    
    // Initialize completion fields (not used for delete dialog, but must be initialized)
    dialog->completion_dropdown = NULL;
    dialog->completion_candidates = NULL;
    dialog->completion_count = 0;
    dialog->completion_selected = -1;
    dialog->completion_prefix[0] = '\0';
    dialog->completion_prefix_len = 0;
    
    // Create canvas window (400x219 for delete, taller to fit warning text + checker + decorations)  
    dialog->canvas = create_canvas(NULL, 200, 150, 400, 219, DIALOG);
    if (!dialog->canvas) {
        free(dialog);
        return;
    }
    
    // Set dialog properties
    dialog->canvas->title = strdup("Delete Confirmation");
    dialog->canvas->bg_color = GRAY;  // Standard dialog gray
    dialog->canvas->disable_scrollbars = true;  // Disable scrollbars for dialogs
    
    // Add to dialog list
    dialog->next = g_dialogs;
    g_dialogs = dialog;
    
    // Show the dialog and set it as active window
    XMapRaised(get_display(), dialog->canvas->win);
    set_active_window(dialog->canvas);
    redraw_canvas(dialog->canvas);
}