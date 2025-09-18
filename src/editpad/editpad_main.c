#include "editpad.h"
#include "find.h"
#include "font_manager.h"
#include "../toolkit/toolkit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

// Global log path - set from config or use default
static char g_log_path[PATH_SIZE] = "~/.config/amiwb/editpad.log";

// Set the log path (called from editpad_load_config)
void editpad_set_log_path(const char *path) {
    if (path && *path) {
        // Store old path to check if it changed
        char old_path[PATH_SIZE];
        strncpy(old_path, g_log_path, PATH_SIZE - 1);
        old_path[PATH_SIZE - 1] = '\0';

        strncpy(g_log_path, path, PATH_SIZE - 1);
        g_log_path[PATH_SIZE - 1] = '\0';
    }
}

// Initialize log file with timestamp header (truncate on each run)
void editpad_log_init(void) {
    char path_buf[1024];
    const char *cfg = g_log_path;
    // Expand leading ~ in the configured path for fopen()
    if (cfg && strncmp(cfg, "~/", 2) == 0) {
        const char *home = getenv("HOME");
        if (home) {
            snprintf(path_buf, sizeof(path_buf), "%s/%s", home, cfg + 2);
        } else {
            snprintf(path_buf, sizeof(path_buf), "%s", cfg); // fallback
        }
    } else {
        snprintf(path_buf, sizeof(path_buf), "%s", cfg ? cfg : "editpad.log");
    }
    FILE *lf = fopen(path_buf, "w"); // overwrite each run
    if (lf) {
        // Header with timestamp - only thing that goes in log during normal operation
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        char ts[128];
        strftime(ts, sizeof(ts), "%a %d %b %Y - %H:%M", &tm);
        fprintf(lf, "EditPad log file, started on: %s\n", ts);
        fprintf(lf, "----------------------------------------\n");
        fclose(lf);  // Close immediately - no fd inheritance
    }
}

// Early log initialization - use default path before config is loaded
void editpad_log_init_early(void) {
    // Use a temporary default path in current directory
    strncpy(g_log_path, "editpad.log", PATH_SIZE - 1);
    g_log_path[PATH_SIZE - 1] = '\0';

    // Initialize the log file
    editpad_log_init();
}

// Error logging function - only logs actual errors
void log_error(const char *format, ...) {
    char log_path[1024];
    const char *cfg = g_log_path;
    
    // Expand leading ~ in the configured path
    if (cfg && strncmp(cfg, "~/", 2) == 0) {
        const char *home = getenv("HOME");
        if (!home) return;  // Silent fail - no logs if no home
        snprintf(log_path, sizeof(log_path), "%s/%s", home, cfg + 2);
    } else {
        snprintf(log_path, sizeof(log_path), "%s", cfg ? cfg : "editpad.log");
    }
    
    // Open, write, close immediately - no fd inheritance
    FILE *log = fopen(log_path, "a");
    if (!log) return;  // Silent fail - don't break on log errors
    
    va_list args;
    va_start(args, format);
    vfprintf(log, format, args);
    va_end(args);
    fprintf(log, "\n");
    
    fclose(log);
}

// Main event loop for EditPad
void editpad_run(EditPad *ep) {
    if (!ep) return;
    
    XEvent event;
    bool running = true;
    
    while (running) {
        XNextEvent(ep->display, &event);
        
        // Check if event is for our windows
        if (event.xany.window == ep->main_window) {
            // Set initial title on MapNotify or FocusIn (not on first event)
            if (!ep->initial_title_set && (event.type == MapNotify || event.type == FocusIn)) {
                ep->modified = false;
                if (ep->text_view) {
                    ep->text_view->modified = false;
                }
                editpad_update_title(ep);
                ep->initial_title_set = true;
            }
            
            switch (event.type) {
                case Expose:
                    if (event.xexpose.count == 0) {
                        // Set initial title on first expose (after AmiWB has managed the window)
                        if (!ep->initial_title_set) {
                            // Ensure modified is false for initial title
                            ep->modified = false;
                            if (ep->text_view) {
                                ep->text_view->modified = false;
                            }
                            editpad_update_title(ep);
                            ep->initial_title_set = true;
                        }
                        // Redraw status bar if needed
                        // TextView handles its own expose events
                    }
                    break;
                    
                case ConfigureNotify:
                    // Window resized - resize TextView to full window
                    if (ep->text_view) {
                        XResizeWindow(ep->display, ep->text_view->window,
                                    event.xconfigure.width,
                                    event.xconfigure.height);
                        // TextView will handle its own ConfigureNotify internally
                    }
                    break;
                    
                case FocusIn:
                    editpad_handle_focus_change(ep, true);
                    break;
                    
                case FocusOut:
                    editpad_handle_focus_change(ep, false);
                    break;
                    
                // Note: This old KeyPress handler is no longer used
                // The newer handler below (line 233+) properly delegates to TextView
                /*
                case KeyPress: {
                    // Old handler - commented out to avoid conflicts
                    break;
                }
                */
                    
                
                case ClientMessage:
                    // Check for window close
                    if ((Atom)event.xclient.data.l[0] == XInternAtom(ep->display, "WM_DELETE_WINDOW", False)) {
                        running = false;
                    }
                    // Check for menu selection from AmiWB
                    else if (event.xclient.message_type == XInternAtom(ep->display, "_AMIWB_MENU_SELECT", False)) {
                        int menu_index = event.xclient.data.l[0];
                        int item_index = event.xclient.data.l[1];
                        int parent_menu = event.xclient.data.l[2];  // Parent menu (for submenus)
                        int is_submenu = event.xclient.data.l[3];   // 1 if this is a submenu selection

                        log_error("[DEBUG] Menu event: menu=%d, item=%d, parent=%d, is_sub=%d",
                                  menu_index, item_index, parent_menu, is_submenu);

                        // Handle File menu (index 0)
                        if (!is_submenu && menu_index == 0) {
                            switch (item_index) {
                                case 0:  // New
                                    editpad_new_file(ep);
                                    break;
                                case 1:  // Open
                                    // Launch ReqASL to select file
                                    FILE *fp = popen("reqasl --mode open", "r");
                                    if (fp) {
                                        char filepath[PATH_SIZE];
                                        if (fgets(filepath, sizeof(filepath), fp)) {
                                            // Remove newline
                                            filepath[strcspn(filepath, "\n")] = 0;
                                            if (strlen(filepath) > 0) {
                                                editpad_open_file(ep, filepath);
                                            }
                                        }
                                        pclose(fp);
                                    }
                                    break;
                                case 2:  // Save
                                    editpad_save_file(ep);
                                    break;
                                case 3:  // Save As
                                    editpad_save_file_as(ep);
                                    break;
                                case 4:  // Quit
                                    running = false;
                                    break;
                            }
                        }
                        // Handle Edit menu (index 1)
                        else if (!is_submenu && menu_index == 1) {
                            switch (item_index) {
                                case 0:  // Cut
                                    editpad_cut(ep);
                                    break;
                                case 1:  // Copy
                                    editpad_copy(ep);
                                    break;
                                case 2:  // Paste
                                    editpad_paste(ep);
                                    break;
                                case 3:  // Select All
                                    editpad_select_all(ep);
                                    break;
                                case 4:  // Undo
                                    editpad_undo(ep);
                                    break;
                            }
                        }
                        // Handle Search menu (index 2)
                        else if (!is_submenu && menu_index == 2) {
                            switch (item_index) {
                                case 0:  // Find
                                    editpad_find(ep);
                                    break;
                                case 1:  // Goto Line
                                    editpad_goto_line(ep);
                                    break;
                            }
                        }
                        // Handle View menu (index 3)
                        else if (!is_submenu && menu_index == 3) {
                            switch (item_index) {
                                case 0:  // WordWrap
                                    editpad_toggle_word_wrap(ep);
                                    break;
                                case 1:  // LineNumbers
                                    editpad_toggle_line_numbers(ep);
                                    break;
                                case 2:  // Syntax > (has submenu)
                                    // Submenu selections come as menu_index=2, item_index=submenu_item
                                    // This is handled separately below
                                    break;
                            }
                        }
                        // Handle Syntax submenu selections
                        // When a submenu item is selected, AmiWB sends:
                        // data.l[0] = parent item index (2 for "Syntax")
                        // data.l[1] = selected item in submenu
                        // data.l[2] = parent menu index (3 for "View")
                        // data.l[3] = 1 (submenu flag)
                        else if (is_submenu && parent_menu == 3 && menu_index == 2) {  // View->Syntax submenu
                            Language lang = LANG_NONE;
                            switch (item_index) {
                                case 0:  // None
                                    lang = LANG_NONE;
                                    break;
                                case 1:  // C/C++
                                    lang = LANG_C;
                                    break;
                                case 2:  // Python
                                    lang = LANG_PYTHON;
                                    break;
                                case 3:  // Shell
                                    lang = LANG_SHELL;
                                    break;
                                case 4:  // JavaScript
                                    lang = LANG_JAVASCRIPT;
                                    break;
                                case 5:  // Makefile
                                    lang = LANG_MAKEFILE;
                                    break;
                                case 6:  // Markdown
                                    lang = LANG_MARKDOWN;
                                    break;
                            }

                            // Set the syntax language and refresh
                            editpad_set_syntax_language(ep, lang);
                        }
                    }
                    break;
            }
        } else if (ep->text_view && event.xany.window == ep->text_view->window) {
            // TextView events
            switch (event.type) {
                case Expose:
                    if (event.xexpose.count == 0) {
                        textview_draw(ep->text_view);
                    }
                    break;
                    
                case SelectionRequest: {
                    // Pass to TextView for clipboard handling
                    XSelectionRequestEvent req = event.xselectionrequest;
                    textview_handle_selection_request(ep->text_view, &req);
                    break;
                }
                
                case SelectionNotify: {
                    // Pass to TextView for clipboard handling
                    XSelectionEvent sel = event.xselection;
                    textview_handle_selection_notify(ep->text_view, &sel);
                    if (ep->text_view->modified) {
                        ep->modified = true;
                        editpad_update_title(ep);
                    }
                    break;
                }
                
                case KeyPress: {
                    // Check for Super key shortcuts first (editor-specific only)
                    if (event.xkey.state & Mod4Mask) {  // Super key pressed
                        KeySym keysym;
                        char buffer[32];
                        XLookupString(&event.xkey, buffer, sizeof(buffer), &keysym, NULL);
                        
                        
                        // Skip clipboard and undo/redo shortcuts - let TextView handle them
                        if (!(keysym == XK_c || keysym == XK_C ||  // Copy
                              keysym == XK_x || keysym == XK_X ||  // Cut
                              keysym == XK_v || keysym == XK_V ||  // Paste
                              keysym == XK_a || keysym == XK_A ||  // Select All
                              keysym == XK_z || keysym == XK_Z ||  // Undo
                              keysym == XK_r || keysym == XK_R)) { // Redo
                            
                            bool handled = true;
                            switch (keysym) {
                            // Note: Cut/Copy/Paste/SelectAll are now handled by TextView
                            case XK_s:  // Super+S - Save
                                if (event.xkey.state & ShiftMask) {
                                    editpad_save_file_as(ep);  // Super+Shift+S
                                } else {
                                    editpad_save_file(ep);
                                }
                                break;
                            case XK_o:  // Super+O - Open
                                FILE *fp = popen("reqasl --mode open", "r");
                                if (fp) {
                                    char filepath[PATH_SIZE];
                                    if (fgets(filepath, sizeof(filepath), fp)) {
                                        filepath[strcspn(filepath, "\n")] = 0;
                                        if (strlen(filepath) > 0) {
                                            editpad_open_file(ep, filepath);
                                        }
                                    }
                                    pclose(fp);
                                }
                                break;
                            case XK_n:  // Super+N - New
                                editpad_new_file(ep);
                                break;
                            // Super+Z and Super+R now handled by TextView for undo/redo
                            case XK_f:  // Super+F - Find
                                editpad_find(ep);
                                break;
                            case XK_h:  // Super+H - Replace
                                editpad_replace(ep);
                                break;
                            case XK_g:  // Super+G - Goto Line
                                editpad_goto_line(ep);
                                break;
                            case XK_l:  // Super+L - Toggle Line Numbers
                                editpad_toggle_line_numbers(ep);
                                break;
                            case XK_w:  // Super+W - Toggle Word Wrap
                                editpad_toggle_word_wrap(ep);
                                break;
                            case XK_q:  // Super+Q - Quit
                                running = false;
                                break;
                            default:
                                handled = false;
                                break;
                            }
                            
                            if (handled) {
                                break;  // Don't pass to TextView
                            }
                        }
                    }
                    
                    // Pass non-shortcut keys to TextView
                    if (textview_handle_key_press(ep->text_view, &event.xkey)) {
                        // Mark as modified if text changed, but only after initial title is set
                        if (!ep->modified && ep->text_view->modified && ep->initial_title_set) {
                            ep->modified = true;
                            editpad_update_title(ep);
                            editpad_update_menu_states(ep);  // Update menu states after text change
                        } else if (!ep->initial_title_set && !ep->modified) {
                            // First key event - set initial title
                            ep->initial_title_set = true;
                            editpad_update_title(ep);
                        }
                        editpad_update_menu_states(ep);  // Always update menu states after key press
                    }
                    break;
                }
                    
                case ButtonPress:
                    textview_handle_button_press(ep->text_view, &event.xbutton);
                    break;
                    
                case ButtonRelease:
                    textview_handle_button_release(ep->text_view, &event.xbutton);
                    editpad_update_menu_states(ep);  // Update menu states after selection change
                    break;
                    
                case MotionNotify:
                    textview_handle_motion(ep->text_view, &event.xmotion);
                    break;
                    
                case FocusIn:
                    textview_handle_focus_in(ep->text_view);
                    break;
                    
                case FocusOut:
                    textview_handle_focus_out(ep->text_view);
                    break;
                    
                case ConfigureNotify:
                    textview_handle_configure(ep->text_view, &event.xconfigure);
                    break;
            }
        } else if (ep->find_dialog) {
            // Check if event is for Find dialog
            FindDialog *find = (FindDialog*)ep->find_dialog;
            if (find->base && event.xany.window == find->base->window) {
                // If the window was destroyed by AmiWB, clean up our structures
                if (event.type == DestroyNotify) {
                    // Window is gone - free our dialog structures to prevent memory leak
                    find_dialog_destroy(find);
                    ep->find_dialog = NULL;
                    continue;
                }
                
                // Let AmiWB handle all window management (close, iconify, etc.)
                // We only handle the dialog's internal events (buttons, input, etc.)
                if (find_dialog_handle_event(find, &event)) {
                    // Event was handled by dialog
                    continue;
                }
            }
        }
    }
}

int main(int argc, char *argv[]) {
    // Open X display
    Display *display = XOpenDisplay(NULL);
    if (!display) {
        log_error("[ERROR] Cannot open X display");
        return 1;
    }

    // Initialize font system
    if (!editpad_font_init(display)) {
        fprintf(stderr, "[ERROR] Failed to initialize font system\n");
        XCloseDisplay(display);
        return 1;
    }

    // PHASE 1: Early log init with default path and register callback
    editpad_log_init_early();
    toolkit_set_log_callback(log_error);

    // Create EditPad (this will load config and update log path)
    EditPad *ep = editpad_create(display);
    if (!ep) {
        log_error("[ERROR] Failed to create EditPad");
        XCloseDisplay(display);
        return 1;
    }

    // PHASE 2: DON'T reinitialize - the config already set the path
    // and we don't want to truncate the log again
    // The early messages are already in the log file
    
    // Set up WM_DELETE_WINDOW
    Atom wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, ep->main_window, &wm_delete, 1);
    
    // Open file if specified on command line
    if (argc > 1) {
        editpad_open_file(ep, argv[1]);
    }
    
    // Run main event loop
    editpad_run(ep);
    
    // Cleanup
    editpad_destroy(ep);
    editpad_font_cleanup();
    XCloseDisplay(display);

    return 0;
}