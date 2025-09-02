#include "editpad.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

// Main event loop for EditPad
void editpad_run(EditPad *ep) {
    if (!ep) return;
    
    XEvent event;
    bool running = true;
    
    while (running) {
        XNextEvent(ep->display, &event);
        
        // Check if event is for our windows
        if (event.xany.window == ep->main_window) {
            switch (event.type) {
                case Expose:
                    if (event.xexpose.count == 0) {
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
                    
                case KeyPress: {
                    KeySym keysym;
                    char buffer[32];
                    XLookupString(&event.xkey, buffer, sizeof(buffer), &keysym, NULL);
                    
                    // Check for shortcuts (Super key combinations)
                    if (event.xkey.state & Mod4Mask) {  // Super key
                        switch (keysym) {
                            case XK_n:  // Super+N - New
                                editpad_new_file(ep);
                                break;
                            case XK_o:  // Super+O - Open
                                // TODO: Launch ReqASL
                                break;
                            case XK_s:  // Super+S - Save
                                if (event.xkey.state & ShiftMask) {
                                    editpad_save_file_as(ep);  // Super+Shift+S
                                } else {
                                    editpad_save_file(ep);
                                }
                                break;
                            case XK_q:  // Super+Q - Quit
                                running = false;
                                break;
                            case XK_z:  // Super+Z - Undo
                                if (event.xkey.state & ShiftMask) {
                                    editpad_redo(ep);  // Super+Shift+Z
                                } else {
                                    editpad_undo(ep);
                                }
                                break;
                            case XK_x:  // Super+X - Cut
                                editpad_cut(ep);
                                break;
                            case XK_c:  // Super+C - Copy
                                editpad_copy(ep);
                                break;
                            case XK_v:  // Super+V - Paste
                                editpad_paste(ep);
                                break;
                            case XK_a:  // Super+A - Select All
                                editpad_select_all(ep);
                                break;
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
                        }
                    }
                    break;
                }
                    
                
                case ClientMessage:
                    // Check for window close
                    if ((Atom)event.xclient.data.l[0] == XInternAtom(ep->display, "WM_DELETE_WINDOW", False)) {
                        running = false;
                    }
                    // Check for menu selection from AmiWB
                    else if (event.xclient.message_type == XInternAtom(ep->display, "_AMIWB_MENU_SELECT", False)) {
                        int menu_index = event.xclient.data.l[0];
                        int item_index = event.xclient.data.l[1];
                        
                        fprintf(stderr, "[EditPad] Menu selection: menu=%d, item=%d\n", menu_index, item_index);
                        
                        // Handle File menu (index 0)
                        if (menu_index == 0) {
                            switch (item_index) {
                                case 0:  // New
                                    editpad_new_file(ep);
                                    break;
                                case 1:  // Open
                                    // Launch ReqASL to select file
                                    fprintf(stderr, "[EditPad] Launching ReqASL for file open\n");
                                    FILE *fp = popen("reqasl --mode open", "r");
                                    if (fp) {
                                        char filepath[PATH_SIZE];
                                        if (fgets(filepath, sizeof(filepath), fp)) {
                                            // Remove newline
                                            filepath[strcspn(filepath, "\n")] = 0;
                                            if (strlen(filepath) > 0) {
                                                fprintf(stderr, "[EditPad] Opening file: %s\n", filepath);
                                                editpad_open_file(ep, filepath);
                                            }
                                        }
                                        pclose(fp);
                                    }
                                    break;
                                case 2:  // Save
                                    editpad_save_file(ep);
                                    break;
                                case 3:  // SaveAs
                                    editpad_save_file_as(ep);
                                    break;
                                case 5:  // Quit (skip separator at 4)
                                    running = false;
                                    break;
                            }
                        }
                        // Handle Edit menu (index 1)
                        else if (menu_index == 1) {
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
                                case 4:  // SelectAll (skip separator at 3)
                                    editpad_select_all(ep);
                                    break;
                                case 6:  // Undo (skip separator at 5)
                                    editpad_undo(ep);
                                    break;
                            }
                        }
                        // Handle Search menu (index 2)
                        else if (menu_index == 2) {
                            switch (item_index) {
                                case 0:  // Find
                                    editpad_find(ep);
                                    break;
                                case 2:  // Replace
                                    editpad_replace(ep);
                                    break;
                                case 3:  // GotoLine
                                    editpad_goto_line(ep);
                                    break;
                            }
                        }
                        // Handle View menu (index 3)
                        else if (menu_index == 3) {
                            switch (item_index) {
                                case 0:  // WordWrap
                                    editpad_toggle_word_wrap(ep);
                                    break;
                                case 1:  // LineNumbers
                                    editpad_toggle_line_numbers(ep);
                                    break;
                            }
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
                        
                        fprintf(stderr, "[EditPad] Super key pressed, keysym=0x%lx (%c)\n", keysym, (char)keysym);
                        
                        // Skip clipboard shortcuts - let TextView handle them
                        if (!(keysym == XK_c || keysym == XK_C ||  // Copy
                              keysym == XK_x || keysym == XK_X ||  // Cut
                              keysym == XK_v || keysym == XK_V ||  // Paste
                              keysym == XK_a || keysym == XK_A)) { // Select All
                            
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
                                fprintf(stderr, "[EditPad] Launching ReqASL for file open\n");
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
                            case XK_z:  // Super+Z - Undo
                                if (event.xkey.state & ShiftMask) {
                                    editpad_redo(ep);  // Super+Shift+Z
                                } else {
                                    editpad_undo(ep);
                                }
                                break;
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
                        // Mark as modified if text changed
                        if (!ep->modified && ep->text_view->modified) {
                            ep->modified = true;
                            editpad_update_title(ep);
                        }
                    }
                    break;
                }
                    
                case ButtonPress:
                    textview_handle_button_press(ep->text_view, &event.xbutton);
                    break;
                    
                case ButtonRelease:
                    textview_handle_button_release(ep->text_view, &event.xbutton);
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
        }
    }
}

int main(int argc, char *argv[]) {
    // Open X display
    Display *display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "Cannot open X display\n");
        return 1;
    }
    
    // Create EditPad
    EditPad *ep = editpad_create(display);
    if (!ep) {
        fprintf(stderr, "Failed to create EditPad\n");
        XCloseDisplay(display);
        return 1;
    }
    
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
    XCloseDisplay(display);
    
    return 0;
}