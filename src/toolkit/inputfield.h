#ifndef TOOLKIT_INPUTFIELD_H
#define TOOLKIT_INPUTFIELD_H

#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>
#include <X11/Xft/Xft.h>
#include <stdbool.h>

#define INPUTFIELD_MAX_LENGTH 256

typedef struct InputField {
    int x, y;
    int width, height;
    char text[INPUTFIELD_MAX_LENGTH + 1];
    int cursor_pos;
    int selection_start;
    int selection_end;
    int visible_start;
    bool has_focus;
    void (*on_enter)(const char *text, void *user_data);
    void (*on_change)(const char *text, void *user_data);
    void *user_data;
} InputField;

InputField* inputfield_create(int x, int y, int width, int height);
void inputfield_destroy(InputField *field);
void inputfield_set_text(InputField *field, const char *text);
const char* inputfield_get_text(InputField *field);
void inputfield_set_callbacks(InputField *field, 
                             void (*on_enter)(const char*, void*),
                             void (*on_change)(const char*, void*),
                             void *user_data);
void inputfield_set_focus(InputField *field, bool has_focus);
void inputfield_draw(InputField *field, Picture dest, Display *dpy, XftDraw *xft_draw, XftFont *font);
bool inputfield_handle_click(InputField *field, int click_x, int click_y);
bool inputfield_handle_key(InputField *field, XKeyEvent *event);
void inputfield_insert_char(InputField *field, char c);
void inputfield_delete_char(InputField *field);
void inputfield_backspace(InputField *field);
void inputfield_move_cursor(InputField *field, int delta);
void inputfield_scroll_to_end(InputField *field);
void inputfield_update_size(InputField *field, int new_width);

#endif