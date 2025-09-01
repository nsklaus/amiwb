#include "editpad.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>

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
                    
                case KeyPress:
                    if (textview_handle_key_press(ep->text_view, &event.xkey)) {
                        // Mark as modified if text changed
                        if (!ep->modified && ep->text_view->modified) {
                            ep->modified = true;
                            editpad_update_title(ep);
                        }
                    }
                    break;
                    
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