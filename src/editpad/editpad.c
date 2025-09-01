#include "editpad.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>

#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 600
#define MIN_WIDTH 400
#define MIN_HEIGHT 300

// Create the EditPad application
EditPad* editpad_create(Display *display) {
    EditPad *ep = calloc(1, sizeof(EditPad));
    if (!ep) return NULL;
    
    ep->display = display;
    ep->root = DefaultRootWindow(display);
    
    // Load configuration
    editpad_load_config(ep);
    
    // Create main window
    XSetWindowAttributes attrs;
    // Convert GRAY to pixel value (simple gray)
    attrs.background_pixel = 0xa2a2a0;  // Gray color
    attrs.border_pixel = BlackPixel(display, DefaultScreen(display));
    attrs.event_mask = ExposureMask | KeyPressMask | StructureNotifyMask | 
                       FocusChangeMask | PropertyChangeMask;
    
    ep->main_window = XCreateWindow(display, ep->root,
                                   100, 100, WINDOW_WIDTH, WINDOW_HEIGHT,
                                   0, CopyFromParent, InputOutput, CopyFromParent,
                                   CWBackPixel | CWBorderPixel | CWEventMask, &attrs);
    
    // Set window properties
    XTextProperty window_name;
    char *title = "EditPad - Untitled";
    XStringListToTextProperty(&title, 1, &window_name);
    XSetWMName(display, ep->main_window, &window_name);
    XFree(window_name.value);
    
    // Set size hints
    XSizeHints *size_hints = XAllocSizeHints();
    size_hints->flags = PMinSize | PBaseSize;
    size_hints->min_width = MIN_WIDTH;
    size_hints->min_height = MIN_HEIGHT;
    size_hints->base_width = WINDOW_WIDTH;
    size_hints->base_height = WINDOW_HEIGHT;
    XSetWMNormalHints(display, ep->main_window, size_hints);
    XFree(size_hints);
    
    // Set window class (for AmiWB to recognize it)
    XClassHint *class_hint = XAllocClassHint();
    class_hint->res_name = "editpad";
    class_hint->res_class = "EditPad";
    XSetClassHint(display, ep->main_window, class_hint);
    XFree(class_hint);
    
    // Create TextView widget (full window)
    ep->text_view = textview_create(display, ep->main_window,
                                   0, 0, WINDOW_WIDTH, WINDOW_HEIGHT);
    
    // Apply settings
    textview_set_line_numbers(ep->text_view, ep->line_numbers);
    textview_set_word_wrap(ep->text_view, ep->word_wrap);
    
    // Apply color settings from config
    textview_set_selection_colors(ep->text_view, ep->selection_bg, ep->selection_fg);
    textview_set_cursor_color(ep->text_view, ep->cursor_color);
    
    // Set initial state
    ep->untitled = true;
    ep->modified = false;
    strcpy(ep->current_file, "");
    
    // Register with AmiWB for menu override
    // TODO: Set properties for menu/shortcut override
    
    XMapWindow(display, ep->main_window);
    XFlush(display);
    
    return ep;
}

// Destroy EditPad and free resources
void editpad_destroy(EditPad *ep) {
    if (!ep) return;
    
    if (ep->text_view) {
        textview_destroy(ep->text_view);
    }
    
    if (ep->clipboard) {
        free(ep->clipboard);
    }
    
    if (ep->main_window) {
        XDestroyWindow(ep->display, ep->main_window);
    }
    
    free(ep);
}

// Update window title based on file and modified state
void editpad_update_title(EditPad *ep) {
    if (!ep) return;
    
    char title[PATH_SIZE + 32];
    if (ep->untitled) {
        snprintf(title, sizeof(title), "EditPad - Untitled%s",
                ep->modified ? " *" : "");
    } else {
        const char *basename = strrchr(ep->current_file, '/');
        basename = basename ? basename + 1 : ep->current_file;
        snprintf(title, sizeof(title), "EditPad - %s%s",
                basename, ep->modified ? " *" : "");
    }
    
    XTextProperty window_name;
    char *title_ptr = title;
    XStringListToTextProperty(&title_ptr, 1, &window_name);
    XSetWMName(ep->display, ep->main_window, &window_name);
    XFree(window_name.value);
}

// New file
void editpad_new_file(EditPad *ep) {
    if (!ep) return;
    
    // TODO: Check if current file needs saving
    
    textview_set_text(ep->text_view, "");
    ep->untitled = true;
    ep->modified = false;
    strcpy(ep->current_file, "");
    editpad_update_title(ep);
}

// Open file
void editpad_open_file(EditPad *ep, const char *filename) {
    if (!ep || !filename) return;
    
    FILE *f = fopen(filename, "r");
    if (!f) {
        fprintf(stderr, "Cannot open file: %s\n", filename);
        return;
    }
    
    // Read file contents
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char *content = malloc(size + 1);
    if (content) {
        fread(content, 1, size, f);
        content[size] = '\0';
        
        textview_set_text(ep->text_view, content);
        free(content);
        
        strncpy(ep->current_file, filename, PATH_SIZE - 1);
        ep->current_file[PATH_SIZE - 1] = '\0';
        ep->untitled = false;
        ep->modified = false;
        editpad_update_title(ep);
    }
    
    fclose(f);
}

// Save file
void editpad_save_file(EditPad *ep) {
    if (!ep) return;
    
    if (ep->untitled) {
        editpad_save_file_as(ep);
        return;
    }
    
    char *content = textview_get_text(ep->text_view);
    if (!content) return;
    
    FILE *f = fopen(ep->current_file, "w");
    if (f) {
        fputs(content, f);
        fclose(f);
        ep->modified = false;
        editpad_update_title(ep);
    } else {
        fprintf(stderr, "Cannot save file: %s\n", ep->current_file);
    }
    
    free(content);
}

// Save file as (placeholder - needs ReqASL)
void editpad_save_file_as(EditPad *ep) {
    // TODO: Use ReqASL to get filename
    fprintf(stderr, "Save As: ReqASL integration needed\n");
}

// Toggle line numbers
void editpad_toggle_line_numbers(EditPad *ep) {
    if (!ep) return;
    ep->line_numbers = !ep->line_numbers;
    textview_set_line_numbers(ep->text_view, ep->line_numbers);
}

// Toggle word wrap
void editpad_toggle_word_wrap(EditPad *ep) {
    if (!ep) return;
    ep->word_wrap = !ep->word_wrap;
    textview_set_word_wrap(ep->text_view, ep->word_wrap);
}

// Load configuration from ~/.config/amiwb/editpad/editpadrc
void editpad_load_config(EditPad *ep) {
    if (!ep) return;
    
    // Set defaults
    ep->line_numbers = false;
    ep->word_wrap = false;
    ep->tab_width = 4;
    ep->auto_indent = true;
    
    // Default colors
    ep->selection_bg = 0x99CCFF;  // Light blue
    ep->selection_fg = 0x000000;  // Black
    ep->cursor_color = 0x4858B0;  // Blue from config.h
    
    // Try both paths: new editpad subdir and old location
    char config_path[PATH_SIZE];
    FILE *f = NULL;
    
    // First try ~/.config/amiwb/editpad/editpadrc
    snprintf(config_path, sizeof(config_path), "%s/.config/amiwb/editpad/editpadrc", getenv("HOME"));
    f = fopen(config_path, "r");
    
    // Fallback to old location
    if (!f) {
        snprintf(config_path, sizeof(config_path), "%s/.config/amiwb/editpadrc", getenv("HOME"));
        f = fopen(config_path, "r");
    }
    
    if (!f) {
        // Create default config directory and file
        char dir_path[PATH_SIZE];
        snprintf(dir_path, sizeof(dir_path), "%s/.config/amiwb/editpad", getenv("HOME"));
        system("mkdir -p ~/.config/amiwb/editpad");
        
        // Create default config file
        snprintf(config_path, sizeof(config_path), "%s/.config/amiwb/editpad/editpadrc", getenv("HOME"));
        f = fopen(config_path, "w");
        if (f) {
            fprintf(f, "# EditPad Configuration\n");
            fprintf(f, "# Font settings\n");
            fprintf(f, "font = Source Code Pro:style=Bold\n");
            fprintf(f, "fontsize = 11\n");
            fprintf(f, "\n");
            fprintf(f, "# Editor preferences\n");
            fprintf(f, "linenumbers = false\n");
            fprintf(f, "wordwrap = false\n");
            fprintf(f, "tabwidth = 4\n");
            fprintf(f, "autoindent = true\n");
            fprintf(f, "\n");
            fprintf(f, "# Colors (hex RGB values)\n");
            fprintf(f, "selection.bg = #99CCFF\n");
            fprintf(f, "selection.fg = #000000\n");
            fprintf(f, "cursor.color = #4858B0\n");
            fclose(f);
        }
        return;  // Use defaults
    }
    
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '\n') continue;
        
        // Parse key = value pairs
        char *eq = strchr(line, '=');
        if (!eq) continue;
        
        *eq = '\0';
        char *key = line;
        char *value = eq + 1;
        
        // Trim whitespace
        while (*key == ' ' || *key == '\t') key++;
        while (*value == ' ' || *value == '\t') value++;
        char *end = value + strlen(value) - 1;
        while (end > value && (*end == '\n' || *end == ' ' || *end == '\t')) {
            *end-- = '\0';
        }
        
        // Apply settings
        if (strcmp(key, "linenumbers") == 0) {
            ep->line_numbers = (strcmp(value, "true") == 0);
        } else if (strcmp(key, "wordwrap") == 0) {
            ep->word_wrap = (strcmp(value, "true") == 0);
        } else if (strcmp(key, "tabwidth") == 0) {
            ep->tab_width = atoi(value);
        } else if (strcmp(key, "autoindent") == 0) {
            ep->auto_indent = (strcmp(value, "true") == 0);
        } else if (strcmp(key, "font") == 0) {
            // Store font name for TextView to use
            // TODO: Pass to TextView
        } else if (strcmp(key, "fontsize") == 0) {
            // Store font size for TextView to use
            // TODO: Pass to TextView
        } else if (strcmp(key, "selection.bg") == 0) {
            // Parse hex color value
            if (value[0] == '#') value++;
            ep->selection_bg = (unsigned int)strtol(value, NULL, 16);
        } else if (strcmp(key, "selection.fg") == 0) {
            if (value[0] == '#') value++;
            ep->selection_fg = (unsigned int)strtol(value, NULL, 16);
        } else if (strcmp(key, "cursor.color") == 0) {
            if (value[0] == '#') value++;
            ep->cursor_color = (unsigned int)strtol(value, NULL, 16);
        }
    }
    
    fclose(f);
}

// Handle focus change
void editpad_handle_focus_change(EditPad *ep, bool focused) {
    if (!ep) return;
    ep->has_focus = focused;
    
    if (ep->text_view) {
        if (focused) {
            textview_handle_focus_in(ep->text_view);
        } else {
            textview_handle_focus_out(ep->text_view);
        }
    }
    
    // TODO: Notify AmiWB about menu/shortcut override state
}

// Stub implementations for operations
void editpad_undo(EditPad *ep) {
    // TODO: Implement undo
}

void editpad_redo(EditPad *ep) {
    // TODO: Implement redo
}

void editpad_cut(EditPad *ep) {
    // TODO: Implement cut
}

void editpad_copy(EditPad *ep) {
    // TODO: Implement copy
}

void editpad_paste(EditPad *ep) {
    // TODO: Implement paste
}

void editpad_select_all(EditPad *ep) {
    // TODO: Implement select all
}

void editpad_find(EditPad *ep) {
    // TODO: Implement find dialog
}

void editpad_replace(EditPad *ep) {
    // TODO: Implement replace dialog
}

void editpad_goto_line(EditPad *ep) {
    // TODO: Implement goto line dialog
}