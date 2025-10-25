#include "dialog.h"
#include "editpad.h"  // For log_error
#include "font_manager.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <fontconfig/fontconfig.h>

// Create a new dialog
Dialog* dialog_create(Display *display, Window parent, DialogType type) {
    if (!display) return NULL;
    
    Dialog *dialog = calloc(1, sizeof(Dialog));
    if (!dialog) return NULL;
    
    dialog->display = display;
    dialog->parent_window = parent;
    dialog->type = type;
    dialog->modal = true;  // Dialogs are modal by default
    
    // Set default size based on type
    switch (type) {
        case DIALOG_FIND:
            dialog->width = 450;
            dialog->height = 100;  // Reduced to half for compact layout
            dialog->title = strdup("Find");
            break;
        case DIALOG_GOTO_LINE:
            dialog->width = 300;
            dialog->height = 140;
            dialog->title = strdup("Go to Line");
            break;
        case DIALOG_ABOUT:
            dialog->width = 350;
            dialog->height = 200;
            dialog->title = strdup("About EditPad");
            break;
        default:
            dialog->width = 400;
            dialog->height = 200;
            dialog->title = strdup("Dialog");
            break;
    }
    
    // Create the window
    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);
    
    // Create window attributes
    XSetWindowAttributes attrs;
    attrs.background_pixmap = None;  // Disable X11 auto-clear (prevents flickering)
    attrs.border_pixel = BlackPixel(display, screen);
    attrs.event_mask = ExposureMask | KeyPressMask | ButtonPressMask |
                       ButtonReleaseMask | PointerMotionMask | StructureNotifyMask;

    dialog->window = XCreateWindow(display, root,
        0, 0, dialog->width, dialog->height,
        0, CopyFromParent, InputOutput, CopyFromParent,
        CWBackPixmap | CWBorderPixel | CWEventMask, &attrs);
    
    if (!dialog->window) {
        free(dialog->title);
        free(dialog);
        return NULL;
    }
    
    // Set window properties
    XTextProperty window_name;
    char *title_ptr = dialog->title;
    XStringListToTextProperty(&title_ptr, 1, &window_name);
    XSetWMName(display, dialog->window, &window_name);
    XFree(window_name.value);
    
    // Also set AmiWB's custom title property
    Atom amiwb_title = XInternAtom(display, "_AMIWB_TITLE_CHANGE", False);
    XChangeProperty(display, dialog->window, amiwb_title, XA_STRING, 8,
                   PropModeReplace, (unsigned char *)dialog->title, 
                   strlen(dialog->title));
    
    // Set transient for parent
    if (parent) {
        XSetTransientForHint(display, dialog->window, parent);
    }

    // Tag with EditPad's app type for menu persistence
    Atom app_type_atom = XInternAtom(display, "_AMIWB_APP_TYPE", False);
    XChangeProperty(display, dialog->window, app_type_atom, XA_STRING, 8,
                   PropModeReplace, (unsigned char *)"EDITPAD", 7);

    // Set window type to dialog
    Atom window_type = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
    Atom dialog_type = XInternAtom(display, "_NET_WM_WINDOW_TYPE_DIALOG", False);
    XChangeProperty(display, dialog->window, window_type, XA_ATOM, 32,
                   PropModeReplace, (unsigned char*)&dialog_type, 1);

    // Set WM_DELETE_WINDOW protocol so we get notified when user clicks close
    Atom wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, dialog->window, &wm_delete, 1);
    
    // Create rendering resources
    XRenderPictFormat *format = XRenderFindVisualFormat(display, DefaultVisual(display, screen));
    if (format) {
        XRenderPictureAttributes pa = {0};
        dialog->picture = XRenderCreatePicture(display, dialog->window, format, 0, &pa);
    }
    
    dialog->xft_draw = XftDrawCreate(display, dialog->window,
                                     DefaultVisual(display, screen),
                                     DefaultColormap(display, screen));
    
    // Load the system font - same as AmiWB uses
    // Build the font path like AmiWB does
    // Get font from font manager
    dialog->font = editpad_font_get();
    if (!dialog->font) {
        // Fatal error - font system not initialized
        free(dialog);
        return NULL;
    }
    
    // Initialize colors - use BLACK and GRAY like AmiWB
    XftColorAllocName(display, DefaultVisual(display, screen),
                      DefaultColormap(display, screen), "black", &dialog->fg_color);
    
    // GRAY background: RGB(0xa0a0, 0xa2a2, 0xa0a0) normalized to 0-1
    XRenderColor gray_color = {0xa0a0, 0xa2a2, 0xa0a0, 0xffff};
    XftColorAllocValue(display, DefaultVisual(display, screen),
                       DefaultColormap(display, screen), &gray_color, &dialog->bg_color);
    
    XftColorAllocName(display, DefaultVisual(display, screen),
                      DefaultColormap(display, screen), "#808080", &dialog->border_color);
    
    // Initialize widget arrays
    dialog->buttons = NULL;
    dialog->button_count = 0;
    dialog->fields = NULL;
    dialog->field_count = 0;
    
    // Center on parent by default
    dialog_center_on_parent(dialog);
    
    return dialog;
}

// Destroy dialog and free all resources
void dialog_destroy(Dialog *dialog) {
    if (!dialog) return;
    
    // Destroy widgets
    for (int i = 0; i < dialog->button_count; i++) {
        if (dialog->buttons[i]) {
            button_destroy(dialog->buttons[i]);
        }
    }
    free(dialog->buttons);
    
    for (int i = 0; i < dialog->field_count; i++) {
        if (dialog->fields[i]) {
            inputfield_destroy(dialog->fields[i]);
        }
    }
    free(dialog->fields);
    
    // Free rendering resources
    // XftDraw should have been destroyed before the window in ClientMessage handler
    // But check anyway in case dialog_destroy is called in other contexts
    if (dialog->xft_draw) {
        XftDrawDestroy(dialog->xft_draw);
        dialog->xft_draw = NULL;
    }
    
    // Font is managed by font_manager - don't close it!
    dialog->font = NULL;
    
    XftColorFree(dialog->display, DefaultVisual(dialog->display, DefaultScreen(dialog->display)),
                 DefaultColormap(dialog->display, DefaultScreen(dialog->display)), &dialog->fg_color);
    XftColorFree(dialog->display, DefaultVisual(dialog->display, DefaultScreen(dialog->display)),
                 DefaultColormap(dialog->display, DefaultScreen(dialog->display)), &dialog->bg_color);
    XftColorFree(dialog->display, DefaultVisual(dialog->display, DefaultScreen(dialog->display)),
                 DefaultColormap(dialog->display, DefaultScreen(dialog->display)), &dialog->border_color);
    
    // Don't destroy window - AmiWB handles that
    // We just clean up our resources
    
    if (dialog->title) {
        free(dialog->title);
    }
    
    free(dialog);
}

// Show dialog
void dialog_show(Dialog *dialog) {
    if (!dialog) return;
    
    dialog_center_on_parent(dialog);
    XMapRaised(dialog->display, dialog->window);
    dialog->visible = true;
    
    // Set input focus to first field if available
    if (dialog->field_count > 0 && dialog->fields[0]) {
        XSetInputFocus(dialog->display, dialog->window, RevertToParent, CurrentTime);
        inputfield_set_focus(dialog->fields[0], true);
    }
}

// Hide dialog
void dialog_hide(Dialog *dialog) {
    if (!dialog) return;
    
    // Use XWithdrawWindow instead of XUnmapWindow for managed windows
    XWithdrawWindow(dialog->display, dialog->window, DefaultScreen(dialog->display));
    XFlush(dialog->display);  // Force the withdrawal to happen immediately
    dialog->visible = false;
}

// Center dialog on parent window
void dialog_center_on_parent(Dialog *dialog) {
    if (!dialog) return;
    
    int parent_x = 0, parent_y = 0;
    unsigned int parent_width = 0, parent_height = 0;
    
    if (dialog->parent_window) {
        XWindowAttributes attrs;
        if (XGetWindowAttributes(dialog->display, dialog->parent_window, &attrs)) {
            Window child;
            XTranslateCoordinates(dialog->display, dialog->parent_window,
                                RootWindow(dialog->display, DefaultScreen(dialog->display)),
                                0, 0, &parent_x, &parent_y, &child);
            parent_width = attrs.width;
            parent_height = attrs.height;
        }
    } else {
        // Center on screen
        parent_width = DisplayWidth(dialog->display, DefaultScreen(dialog->display));
        parent_height = DisplayHeight(dialog->display, DefaultScreen(dialog->display));
    }
    
    dialog->x = parent_x + (parent_width - dialog->width) / 2;
    dialog->y = parent_y + (parent_height - dialog->height) / 2;
    
    XMoveWindow(dialog->display, dialog->window, dialog->x, dialog->y);
}

// Add a button to the dialog
Button* dialog_add_button(Dialog *dialog, int x, int y, int width, int height,
                          const char *label, void (*callback)(void*)) {
    if (!dialog) return NULL;
    
    Button *button = button_create(x, y, width, height, label, dialog->font);
    if (!button) return NULL;
    
    if (callback) {
        button_set_callback(button, callback, dialog);
    }
    
    // Resize button array
    Button **new_buttons = realloc(dialog->buttons, 
                                  sizeof(Button*) * (dialog->button_count + 1));
    if (!new_buttons) {
        button_destroy(button);
        return NULL;
    }
    
    dialog->buttons = new_buttons;
    dialog->buttons[dialog->button_count] = button;
    dialog->button_count++;
    
    return button;
}

// Add an input field to the dialog
InputField* dialog_add_field(Dialog *dialog, int x, int y, int width, int height) {
    if (!dialog) return NULL;
    
    InputField *field = inputfield_create(x, y, width, height, dialog->font);
    if (!field) return NULL;
    
    // Resize field array
    InputField **new_fields = realloc(dialog->fields,
                                     sizeof(InputField*) * (dialog->field_count + 1));
    if (!new_fields) {
        inputfield_destroy(field);
        return NULL;
    }
    
    dialog->fields = new_fields;
    dialog->fields[dialog->field_count] = field;
    dialog->field_count++;
    
    return field;
}

// Handle events
bool dialog_handle_event(Dialog *dialog, XEvent *event) {
    if (!dialog || !event) return false;
    
    switch (event->type) {
        case Expose:
            if (event->xexpose.count == 0) {
                dialog_handle_expose(dialog);
            }
            return true;
            
        case KeyPress:
            return dialog_handle_key_press(dialog, &event->xkey);
            
        case ButtonPress:
            return dialog_handle_button_press(dialog, &event->xbutton);
            
        case ButtonRelease:
            return dialog_handle_button_release(dialog, &event->xbutton);
            
        case ConfigureNotify:
            // Update dialog size if window was resized
            dialog->width = event->xconfigure.width;
            dialog->height = event->xconfigure.height;
            return true;
            
        case ClientMessage:
            // Handle WM_DELETE_WINDOW from window manager
            if (event->xclient.message_type == XInternAtom(dialog->display, "WM_PROTOCOLS", False)) {
                Atom wm_delete = XInternAtom(dialog->display, "WM_DELETE_WINDOW", False);
                if ((Atom)event->xclient.data.l[0] == wm_delete) {
                    // Window manager requested close
                    // First destroy XftDraw while window still exists
                    if (dialog->xft_draw) {
                        XftDrawDestroy(dialog->xft_draw);
                        dialog->xft_draw = NULL;  // Prevent double-free
                    }
                    // Now destroy the window
                    // This tells AmiWB we're cooperating with the close request
                    // The DestroyNotify event will trigger cleanup in editpad_main
                    XDestroyWindow(dialog->display, dialog->window);
                    dialog->visible = false;
                    return true;
                }
            }
            return false;
    }
    
    return false;
}

// Handle keyboard input
bool dialog_handle_key_press(Dialog *dialog, XKeyEvent *event) {
    if (!dialog || !event) return false;
    
    KeySym keysym;
    char buffer[32];
    XLookupString(event, buffer, sizeof(buffer), &keysym, NULL);
    
    // Escape closes dialog (destroy like X button and Cancel button)
    if (keysym == XK_Escape) {
        // Destroy XftDraw first, then window
        if (dialog->xft_draw) {
            XftDrawDestroy(dialog->xft_draw);
            dialog->xft_draw = NULL;
        }
        XDestroyWindow(dialog->display, dialog->window);
        dialog->visible = false;
        return true;
    }
    
    // Tab cycles through fields
    if (keysym == XK_Tab) {
        if (dialog->field_count > 1) {
            // Find currently focused field
            int current = -1;
            for (int i = 0; i < dialog->field_count; i++) {
                if (dialog->fields[i] && dialog->fields[i]->has_focus) {
                    current = i;
                    break;
                }
            }
            
            if (current >= 0) {
                inputfield_set_focus(dialog->fields[current], false);
                int next = (current + 1) % dialog->field_count;
                inputfield_set_focus(dialog->fields[next], true);
                dialog_draw(dialog);
            }
        }
        return true;
    }
    
    // Pass to focused field
    for (int i = 0; i < dialog->field_count; i++) {
        if (dialog->fields[i] && dialog->fields[i]->has_focus) {
            if (inputfield_handle_key(dialog->fields[i], event)) {
                dialog_draw(dialog);
                return true;
            }
        }
    }
    
    return false;
}

// Handle mouse button press
bool dialog_handle_button_press(Dialog *dialog, XButtonEvent *event) {
    if (!dialog || !event) return false;
    
    // Check fields first
    for (int i = 0; i < dialog->field_count; i++) {
        if (dialog->fields[i]) {
            // Clear focus on all fields
            bool was_focused = dialog->fields[i]->has_focus;
            inputfield_set_focus(dialog->fields[i], false);
            
            // Set focus on clicked field
            if (inputfield_handle_click(dialog->fields[i], event->x, event->y)) {
                inputfield_set_focus(dialog->fields[i], true);
                if (!was_focused) {
                    dialog_draw(dialog);
                }
                return true;
            }
        }
    }
    
    // Check buttons
    for (int i = 0; i < dialog->button_count; i++) {
        if (dialog->buttons[i]) {
            if (button_handle_press(dialog->buttons[i], event->x, event->y)) {
                dialog_draw(dialog);
                return true;
            }
        }
    }
    
    return false;
}

// Handle mouse button release
bool dialog_handle_button_release(Dialog *dialog, XButtonEvent *event) {
    if (!dialog || !event) return false;
    
    // Check buttons
    for (int i = 0; i < dialog->button_count; i++) {
        if (dialog->buttons[i]) {
            if (button_handle_release(dialog->buttons[i], event->x, event->y)) {
                dialog_draw(dialog);
                return true;
            }
        }
    }
    
    return false;
}

// Handle expose events
void dialog_handle_expose(Dialog *dialog) {
    if (!dialog) return;
    dialog_draw(dialog);
}

// Draw the dialog
void dialog_draw(Dialog *dialog) {
    if (!dialog || !dialog->xft_draw || !dialog->picture) return;

    // Fill background with gray (matching ReqASL/TextView pattern)
    XRenderColor gray = {0xa0a0, 0xa2a2, 0xa0a0, 0xffff};
    XRenderFillRectangle(dialog->display, PictOpSrc, dialog->picture, &gray,
                        0, 0, dialog->width, dialog->height);
    
    // Draw widgets
    for (int i = 0; i < dialog->field_count; i++) {
        if (dialog->fields[i]) {
            inputfield_render(dialog->fields[i], dialog->picture, dialog->display,
                            dialog->xft_draw);
        }
    }
    
    for (int i = 0; i < dialog->button_count; i++) {
        if (dialog->buttons[i]) {
            button_render(dialog->buttons[i], dialog->picture, dialog->display,
                       dialog->xft_draw);
        }
    }
    
    // Special handling for Find dialog - draw the "Find" and "Replace" labels (right-aligned)
    if (dialog->type == DIALOG_FIND && dialog->font) {
        // Measure text for right alignment
        XGlyphInfo extents_find, extents_replace;
        XftTextExtentsUtf8(dialog->display, dialog->font,
                          (XftChar8*)"Find", 4, &extents_find);
        XftTextExtentsUtf8(dialog->display, dialog->font,
                          (XftChar8*)"Replace", 7, &extents_replace);

        // Right-align labels at x=70 (10px before input field at x=80)
        // This creates a visual gap between label and field
        int label_right_edge = 70;

        // Draw "Find" label at y=20, right-aligned
        // +18 offset for vertical centering with input field baseline
        XftDrawString8(dialog->xft_draw, &dialog->fg_color, dialog->font,
                      label_right_edge - extents_find.width, 20 + 18,
                      (XftChar8*)"Find", 4);

        // Draw "Replace" label at y=55, right-aligned
        XftDrawString8(dialog->xft_draw, &dialog->fg_color, dialog->font,
                      label_right_edge - extents_replace.width, 55 + 18,
                      (XftChar8*)"Replace", 7);
    }
    
    XFlush(dialog->display);
}