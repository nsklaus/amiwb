#include "editpad.h"
#include "find.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <fontconfig/fontconfig.h>

#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 600
#define MIN_WIDTH 400
#define MIN_HEIGHT 300

// Wrapper function for TextView syntax highlighting callback
// This adapts the SyntaxHighlight interface to TextView's callback needs
static void* editpad_syntax_callback(void *context, const char *line, int line_num) {
    SyntaxHighlight *syntax = (SyntaxHighlight*)context;
    if (!syntax || !line) return NULL;
    
    // Call the syntax highlighting engine
    // Note: syntax_highlight_line returns SyntaxColor* which is what TextView expects
    return syntax_highlight_line(syntax, line, line_num);
}

// Create the EditPad application
EditPad* editpad_create(Display *display) {
    EditPad *ep = calloc(1, sizeof(EditPad));
    if (!ep) return NULL;
    
    ep->display = display;
    ep->root = DefaultRootWindow(display);
    
    // Load configuration
    editpad_load_config(ep);
    
    
    // Initialize syntax highlighting
    ep->syntax = syntax_create();
    if (ep->syntax) {
        // Load colors from config (same path as editpadrc)
        char config_path[PATH_SIZE];
        snprintf(config_path, PATH_SIZE, "%s/.config/amiwb/editpad/editpadrc", getenv("HOME"));
        syntax_load_colors(ep->syntax, config_path);
    }
    
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
    
    // Set window properties - just the base app name
    // The dynamic title will be set by editpad_update_title() after mapping
    XTextProperty window_name;
    char *title = "EditPad";
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
    
    // Create font with 75 DPI (same as reqasl and amiwb)
    char font_path[PATH_SIZE];
    snprintf(font_path, sizeof(font_path), "%s/%s", 
             "/usr/local/share/amiwb", "fonts/SourceCodePro-Bold.otf");
    
    FcPattern *pattern = FcPatternCreate();
    if (pattern) {
        FcPatternAddString(pattern, FC_FILE, (const FcChar8 *)font_path);
        FcPatternAddDouble(pattern, FC_SIZE, 12.0);  // Size 12 like reqasl/amiwb
        // FcPatternAddInteger(pattern, FC_WEIGHT, FC_WEIGHT_BOLD);  // No need to specify weight - font file is already Bold variant
        FcPatternAddDouble(pattern, FC_DPI, 75);  // Force 75 DPI like reqasl/amiwb
        FcConfigSubstitute(NULL, pattern, FcMatchPattern);
        XftDefaultSubstitute(display, DefaultScreen(display), pattern);
        ep->font = XftFontOpenPattern(display, pattern);
        // Note: pattern is now owned by the font, don't destroy it
    }
    
    if (!ep->font) {
        // Fallback to monospace with DPI setting
        ep->font = XftFontOpen(display, DefaultScreen(display),
                              XFT_FAMILY, XftTypeString, "monospace",
                              XFT_SIZE, XftTypeDouble, 12.0,  // Size 12 like reqasl/amiwb
                              XFT_DPI, XftTypeDouble, 75.0,  // Force 75 DPI
                              NULL);
    }
    
    if (!ep->font) {
        log_error("[ERROR] Failed to load font for EditPad");
        XDestroyWindow(display, ep->main_window);
        free(ep);
        return NULL;
    }
    
    // Create TextView widget (full window) with our font
    ep->text_view = textview_create(display, ep->main_window,
                                   0, 0, WINDOW_WIDTH, WINDOW_HEIGHT, ep->font);
    
    // Apply settings
    textview_set_line_numbers(ep->text_view, ep->line_numbers);
    textview_set_word_wrap(ep->text_view, ep->word_wrap);
    
    // Apply color settings from config
    textview_set_selection_colors(ep->text_view, ep->selection_bg, ep->selection_fg);
    textview_set_cursor_color(ep->text_view, ep->cursor_color);
    
    // Set initial state
    ep->untitled = true;
    ep->modified = false;
    ep->initial_title_set = false;
    strcpy(ep->current_file, "");
    
    // Register with AmiWB for menu substitution
    // Set X11 properties to identify this as a toolkit app
    Atom app_type_atom = XInternAtom(display, "_AMIWB_APP_TYPE", False);
    Atom menu_data_atom = XInternAtom(display, "_AMIWB_MENU_DATA", False);
    
    // Set app type
    const char *app_type = "EditPad";
    XChangeProperty(display, ep->main_window, app_type_atom,
                   XA_STRING, 8, PropModeReplace,
                   (unsigned char*)app_type, strlen(app_type));
    
    // Set menu data (simple format for now)
    const char *menu_data = "File:New,Open,Save,Save As,Quit|Edit:Cut,Copy,Paste,Select All,Undo|Search:Find,Goto Line|View:Word Wrap,Line Numbers";
    XChangeProperty(display, ep->main_window, menu_data_atom,
                   XA_STRING, 8, PropModeReplace,
                   (unsigned char*)menu_data, strlen(menu_data));
    
    // Set initial title BEFORE mapping the window
    Atom title_change_atom = XInternAtom(display, "_AMIWB_TITLE_CHANGE", False);
    Atom utf8_string = XInternAtom(display, "UTF8_STRING", False);
    const char *initial_title = "New File";
    XChangeProperty(display, ep->main_window, title_change_atom,
                   utf8_string, 8, PropModeReplace,
                   (unsigned char*)initial_title, strlen(initial_title));
    
    XMapWindow(display, ep->main_window);
    XFlush(display);
    XSync(display, False);  // Ensure window is mapped
    
    // Don't set the title here - let it be set after the first expose event
    // when AmiWB has had a chance to manage the window
    
    // Set initial menu states
    editpad_update_menu_states(ep);
    
    return ep;
}

// Destroy EditPad and free resources
void editpad_destroy(EditPad *ep) {
    if (!ep) return;
    
    // Destroy dialogs
    if (ep->find_dialog) {
        find_dialog_destroy((FindDialog*)ep->find_dialog);
    }
    
    if (ep->syntax) {
        syntax_destroy(ep->syntax);
    }
    
    if (ep->text_view) {
        textview_destroy(ep->text_view);
    }
    
    if (ep->font) {
        XftFontClose(ep->display, ep->font);
    }
    
    if (ep->main_window) {
        XDestroyWindow(ep->display, ep->main_window);
    }
    
    free(ep);
}

// Update menu item states based on what operations are available
void editpad_update_menu_states(EditPad *ep) {
    if (!ep) return;
    
    // Build menu state string
    // Format: "menu_index,item_index,enabled;menu_index,item_index,enabled;..."
    // Edit menu is index 1, items: Cut(0), Copy(1), Paste(2), SelectAll(3), Undo(4)
    char menu_states[512];
    char *p = menu_states;
    int remaining = sizeof(menu_states);
    
    // Check what operations are available
    bool can_undo = ep->text_view ? textview_can_undo(ep->text_view) : false;
    bool has_selection = ep->text_view ? ep->text_view->has_selection : false;
    bool has_text = false;
    if (ep->text_view) {
        // Check if there's any text in the buffer
        char *content = textview_get_text(ep->text_view);
        if (content) {
            has_text = (strlen(content) > 0);
            free(content);
        }
    }
    
    // Update Edit menu items
    // Cut (0) - enabled if there's a selection
    int written = snprintf(p, remaining, "1,0,%d;", has_selection ? 1 : 0);
    p += written; remaining -= written;
    
    // Copy (1) - enabled if there's a selection  
    written = snprintf(p, remaining, "1,1,%d;", has_selection ? 1 : 0);
    p += written; remaining -= written;
    
    // Paste (2) - always enabled (clipboard might have content from other apps)
    written = snprintf(p, remaining, "1,2,1;");
    p += written; remaining -= written;
    
    // Select All (3) - enabled only if there's text to select
    written = snprintf(p, remaining, "1,3,%d;", has_text ? 1 : 0);
    p += written; remaining -= written;
    
    // Undo (4) - enabled based on undo history
    written = snprintf(p, remaining, "1,4,%d;", can_undo ? 1 : 0);
    p += written; remaining -= written;
    
    // Update Search menu items (menu index 2)
    // Find (0) - enabled only if there's text to search
    written = snprintf(p, remaining, "2,0,%d;", has_text ? 1 : 0);
    p += written; remaining -= written;
    
    // Goto Line (1) - enabled only if there's text
    written = snprintf(p, remaining, "2,1,%d;", has_text ? 1 : 0);
    p += written; remaining -= written;
    
    // Update View menu items (menu index 3)
    // Word Wrap (0) - disabled (not implemented)
    written = snprintf(p, remaining, "3,0,0;");
    p += written; remaining -= written;
    
    // Line Numbers (1) - always enabled
    written = snprintf(p, remaining, "3,1,1");
    
    // Set the property
    Atom menu_states_atom = XInternAtom(ep->display, "_AMIWB_MENU_STATES", False);
    XChangeProperty(ep->display, ep->main_window, menu_states_atom,
                   XA_STRING, 8, PropModeReplace,
                   (unsigned char*)menu_states, strlen(menu_states));
    XFlush(ep->display);
}

// Update window title based on file and modified state
void editpad_update_title(EditPad *ep) {
    if (!ep) return;
    
    // Build the full title for standard WMs
    char full_title[PATH_SIZE + 32];
    if (ep->untitled) {
        snprintf(full_title, sizeof(full_title), "EditPad - Untitled%s",
                ep->modified ? " *" : "");
    } else {
        const char *basename = strrchr(ep->current_file, '/');
        basename = basename ? basename + 1 : ep->current_file;
        snprintf(full_title, sizeof(full_title), "EditPad - %s%s",
                basename, ep->modified ? " *" : "");
    }
    
    // Set standard WM_NAME for compatibility with other WMs
    XTextProperty window_name;
    char *title_ptr = full_title;
    XStringListToTextProperty(&title_ptr, 1, &window_name);
    XSetWMName(ep->display, ep->main_window, &window_name);
    XFree(window_name.value);
    
    // Also set AmiWB's dynamic title property (just the filename part)
    char dynamic_title[PATH_SIZE + 8];
    if (ep->untitled) {
        snprintf(dynamic_title, sizeof(dynamic_title), "%sNew File",
                ep->modified ? "* " : "");
    } else {
        const char *basename = strrchr(ep->current_file, '/');
        basename = basename ? basename + 1 : ep->current_file;
        snprintf(dynamic_title, sizeof(dynamic_title), "%s%s",
                ep->modified ? "* " : "", basename);
    }
    // Set the _AMIWB_TITLE_CHANGE property for AmiWB's dynamic title system
    Atom amiwb_title_atom = XInternAtom(ep->display, "_AMIWB_TITLE_CHANGE", False);
    Atom utf8_string = XInternAtom(ep->display, "UTF8_STRING", False);
    XChangeProperty(ep->display, ep->main_window, amiwb_title_atom,
                   utf8_string, 8, PropModeReplace,
                   (unsigned char*)dynamic_title, strlen(dynamic_title));
    XFlush(ep->display);  // Force immediate property update
    XSync(ep->display, False);  // Also sync to ensure it's processed
}

// New file
void editpad_new_file(EditPad *ep) {
    if (!ep) return;
    
    // TODO: Check if current file needs saving
    
    textview_set_text(ep->text_view, "");
    ep->untitled = true;
    ep->modified = false;
    strcpy(ep->current_file, "");
    
    // Clear syntax highlighting
    if (ep->syntax) {
        syntax_set_language(ep->syntax, LANG_NONE);
        // Clear syntax highlighting in TextView
        textview_set_syntax_highlight(ep->text_view, NULL, NULL, NULL, 0);
    }
    
    editpad_update_title(ep);
}

// Open file
void editpad_open_file(EditPad *ep, const char *filename) {
    if (!ep || !filename) return;
    
    FILE *f = fopen(filename, "rb");  // Open in binary mode for proper size
    if (!f) {
        log_error("[ERROR] EditPad: Cannot open file: %s (errno=%d: %s)", 
                filename, errno, strerror(errno));
        return;
    }
    
    // Read file contents
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    
    if (size < 0) {
        log_error("[ERROR] EditPad: Invalid file size %ld", size);
        fclose(f);
        return;
    }
    
    if (size == 0) {
        log_error("[WARNING] EditPad: File is empty");
        textview_set_text(ep->text_view, "");
        strncpy(ep->current_file, filename, PATH_SIZE - 1);
        ep->current_file[PATH_SIZE - 1] = '\0';
        ep->untitled = false;
        ep->modified = false;
        editpad_update_title(ep);
        fclose(f);
        return;
    }
    
    char *content = malloc(size + 1);
    if (!content) {
        log_error("[ERROR] EditPad: Failed to allocate %ld bytes for file content", size + 1);
        fclose(f);
        return;
    }
    
    size_t bytes_read = fread(content, 1, size, f);
    content[size] = '\0';
    
    
    if (bytes_read != size) {
        log_error("[WARNING] EditPad: Read size mismatch (expected %ld, got %zu)", 
                size, bytes_read);
    }
    
    // Count lines and check for UTF-8
    int line_count = 1;
    int null_bytes = 0;
    int utf8_sequences = 0;
    for (long i = 0; i < size; i++) {
        if (content[i] == '\n') line_count++;
        if (content[i] == '\0') null_bytes++;
        // Check for UTF-8 multi-byte sequences
        if ((unsigned char)content[i] >= 0xC0 && (unsigned char)content[i] <= 0xFD) {
            utf8_sequences++;
        }
    }
    
    if (null_bytes > 0) {
        log_error("[WARNING] EditPad: File contains %d NULL bytes (might be binary)", null_bytes);
    }
    if (utf8_sequences > 0) {
    }
    
    textview_set_text(ep->text_view, content);
    
    // Verify what TextView received
    char *loaded_text = textview_get_text(ep->text_view);
    if (loaded_text) {
        int tv_lines = 1;
        for (char *p = loaded_text; *p; p++) {
            if (*p == '\n') tv_lines++;
        }
        free(loaded_text);
    } else {
        log_error("[WARNING] EditPad: TextView returned NULL text after loading");
    }
    
    free(content);
    
    strncpy(ep->current_file, filename, PATH_SIZE - 1);
    ep->current_file[PATH_SIZE - 1] = '\0';
    ep->untitled = false;
    ep->modified = false;
    
    // Detect language and set syntax highlighting
    if (ep->syntax) {
        Language lang = syntax_detect_language(filename);
        syntax_set_language(ep->syntax, lang);
        
        // Pass syntax context to TextView using the new callback API
        uint32_t palette[SYNTAX_MAX];
        for (int i = 0; i < SYNTAX_MAX; i++) {
            palette[i] = syntax_get_color(ep->syntax, i);
        }
        
        textview_set_syntax_highlight(ep->text_view, ep->syntax, 
                                     (TextViewSyntaxCallback)editpad_syntax_callback,
                                     palette, SYNTAX_MAX);
        
        // Highlight all lines
        textview_highlight_all_lines(ep->text_view);
    }
    
    editpad_update_title(ep);
    
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
        log_error("[ERROR] Cannot save file: %s", ep->current_file);
    }
    
    free(content);
}

// Save file as - launch ReqASL in save mode
void editpad_save_file_as(EditPad *ep) {
    if (!ep) return;
    
    // Launch ReqASL in save mode to get filename
    
    // Build command with initial path if we have a current file
    char command[PATH_SIZE * 2];
    if (!ep->untitled && strlen(ep->current_file) > 0) {
        // Get directory from current file
        char dir[PATH_SIZE];
        strncpy(dir, ep->current_file, PATH_SIZE - 1);
        dir[PATH_SIZE - 1] = '\0';
        char *last_slash = strrchr(dir, '/');
        if (last_slash) {
            *last_slash = '\0';
            snprintf(command, sizeof(command), "reqasl --mode save --path \"%s\"", dir);
        } else {
            snprintf(command, sizeof(command), "reqasl --mode save");
        }
    } else {
        snprintf(command, sizeof(command), "reqasl --mode save");
    }
    
    FILE *fp = popen(command, "r");
    if (fp) {
        char filepath[PATH_SIZE];
        if (fgets(filepath, sizeof(filepath), fp)) {
            // Remove newline
            filepath[strcspn(filepath, "\n")] = 0;
            if (strlen(filepath) > 0) {
                // Saving file as
                
                // Get text from TextView
                char *content = textview_get_text(ep->text_view);
                if (content) {
                    // Save to the new file
                    FILE *f = fopen(filepath, "w");
                    if (f) {
                        fputs(content, f);
                        fclose(f);
                        
                        // Update EditPad state
                        strncpy(ep->current_file, filepath, PATH_SIZE - 1);
                        ep->current_file[PATH_SIZE - 1] = '\0';
                        ep->untitled = false;
                        ep->modified = false;
                        editpad_update_title(ep);
                        
                        // File saved successfully
                    } else {
                        log_error("[ERROR] Failed to save file: %s", filepath);
                    }
                    free(content);
                }
            } else {
                // Save As cancelled
            }
        }
        pclose(fp);
    } else {
        log_error("[ERROR] Failed to launch ReqASL for save dialog");
    }
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
    if (f) {
        // Config file opened successfully
    }
    
    // Fallback to old location
    if (!f) {
        snprintf(config_path, sizeof(config_path), "%s/.config/amiwb/editpadrc", getenv("HOME"));
        f = fopen(config_path, "r");
        if (f) {
            // Config file opened from fallback location
        }
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
            fprintf(f, "\n");
            fprintf(f, "# Log file path (can use ~ for home directory)\n");
            fprintf(f, "log_path = ~/.config/amiwb/editpad.log\n");
            fprintf(f, "\n");
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
        
        // Trim whitespace from key and value
        while (*key == ' ' || *key == '\t') key++;
        char *key_end = eq - 1;
        while (key_end > key && (*key_end == ' ' || *key_end == '\t')) key_end--;
        *(key_end + 1) = '\0';
        
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
        } else if (strcmp(key, "log_path") == 0) {
            // Set the global log path
            editpad_set_log_path(value);
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
            // Always give keyboard focus to TextView when EditPad becomes active
            XSetInputFocus(ep->display, ep->text_view->window, 
                          RevertToParent, CurrentTime);
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
    if (!ep || !ep->text_view) return;
    
    // TextView handles the clipboard operation
    textview_cut(ep->text_view);
    if (ep->text_view->modified) {
        ep->modified = true;
        editpad_update_title(ep);
    }
}

void editpad_copy(EditPad *ep) {
    if (!ep || !ep->text_view) return;
    
    // TextView handles the clipboard operation
    textview_copy(ep->text_view);
}

void editpad_paste(EditPad *ep) {
    if (!ep || !ep->text_view) return;
    
    // TextView handles the clipboard operation
    textview_paste(ep->text_view);
    // Modified flag will be set by TextView event handler
}

void editpad_select_all(EditPad *ep) {
    if (!ep || !ep->text_view) return;
    
    // TextView handles the selection
    textview_select_all(ep->text_view);
}

void editpad_find(EditPad *ep) {
    if (!ep) return;
    
    // Create Find dialog if not exists
    if (!ep->find_dialog) {
        ep->find_dialog = find_dialog_create(ep);
    }
    
    if (ep->find_dialog) {
        find_dialog_show((FindDialog*)ep->find_dialog);
    }
}

void editpad_replace(EditPad *ep) {
    // TODO: Implement replace dialog
}

void editpad_goto_line(EditPad *ep) {
    // TODO: Implement goto line dialog
}