#ifndef EDITPAD_DIALOG_H
#define EDITPAD_DIALOG_H

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>
#include <X11/extensions/Xrender.h>
#include <stdbool.h>
#include "../toolkit/button.h"
#include "../toolkit/inputfield.h"

// Dialog types
typedef enum {
    DIALOG_FIND,
    DIALOG_GOTO_LINE,
    DIALOG_ABOUT
} DialogType;

// Base dialog structure
typedef struct Dialog {
    Display *display;
    Window window;
    Window parent_window;
    DialogType type;
    
    // Dialog properties
    int x, y;
    int width, height;
    char *title;
    
    // State
    bool visible;
    bool modal;
    
    // Rendering resources
    Picture picture;
    XftDraw *xft_draw;
    XftFont *font;
    XftColor fg_color;
    XftColor bg_color;
    XftColor border_color;
    
    // Common widgets
    Button **buttons;
    int button_count;
    InputField **fields;
    int field_count;
    
    // User data pointer for callbacks
    void *user_data;
    
    // Dialog-specific data (for subclasses)
    void *dialog_data;
    
} Dialog;

// Dialog creation and management
Dialog* dialog_create(Display *display, Window parent, DialogType type);
void dialog_destroy(Dialog *dialog);
void dialog_show(Dialog *dialog);
void dialog_hide(Dialog *dialog);
void dialog_center_on_parent(Dialog *dialog);

// Widget management
Button* dialog_add_button(Dialog *dialog, int x, int y, int width, int height, 
                         const char *label, void (*callback)(void*));
InputField* dialog_add_field(Dialog *dialog, int x, int y, int width, int height);

// Event handling
bool dialog_handle_event(Dialog *dialog, XEvent *event);
bool dialog_handle_key_press(Dialog *dialog, XKeyEvent *event);
bool dialog_handle_button_press(Dialog *dialog, XButtonEvent *event);
bool dialog_handle_button_release(Dialog *dialog, XButtonEvent *event);
void dialog_handle_expose(Dialog *dialog);

// Drawing
void dialog_draw(Dialog *dialog);

// Specific dialog types (implemented in separate files)
void* dialog_create_find(Dialog *base);
void* dialog_create_goto_line(Dialog *base);
void* dialog_create_about(Dialog *base);

#endif // EDITPAD_DIALOG_H