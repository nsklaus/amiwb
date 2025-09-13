#ifndef TOOLKIT_INPUTFIELD_H
#define TOOLKIT_INPUTFIELD_H

#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>
#include <X11/Xft/Xft.h>
#include <stdbool.h>
#include "../amiwb/config.h"  // For NAME_SIZE

#define INPUTFIELD_MAX_LENGTH NAME_SIZE

typedef struct InputField {
    int x, y;
    int width, height;
    char text[INPUTFIELD_MAX_LENGTH + 1];
    char name[64];  // Debug name for the field
    int cursor_pos;
    int selection_start;
    int selection_end;
    int visible_start;
    bool has_focus;
    bool disabled;  // Checker pattern, completely unavailable
    bool readonly;  // Can select/copy but not edit
    void (*on_enter)(const char *text, void *user_data);
    void (*on_change)(const char *text, void *user_data);
    void *user_data;
    XftFont *font;  // Font to use for this InputField (borrowed from app, don't free)
    
    // Mouse selection support
    bool mouse_selecting;          // True when mouse button is down for selection
    int mouse_select_start;        // Position where mouse selection started
    
    // Path completion support
    bool enable_path_completion;  // Enable filesystem path autocompletion
    char completion_base_dir[512]; // Base directory for file completion (empty = use current dir)
    Window completion_window;      // X11 window for dropdown (0 if not shown)
    bool dropdown_open;            // True when dropdown is visible and active
    char **completion_candidates;  // Array of completion strings
    int completion_count;          // Number of completion candidates
    int completion_selected;       // Currently selected completion (-1 if none)
    char completion_prefix[INPUTFIELD_MAX_LENGTH];  // The prefix being completed
    int completion_prefix_len;     // Length of prefix in text buffer
} InputField;

InputField* inputfield_create(int x, int y, int width, int height, XftFont *font);
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
bool inputfield_handle_mouse_motion(InputField *field, int x, int y, Display *dpy);
bool inputfield_handle_mouse_release(InputField *field, int x, int y);
bool inputfield_handle_key(InputField *field, XKeyEvent *event);
void inputfield_insert_char(InputField *field, char c);
void inputfield_delete_char(InputField *field);
void inputfield_backspace(InputField *field);
void inputfield_move_cursor(InputField *field, int delta);
int inputfield_pos_from_x(InputField *field, int x, Display *dpy, XftFont *font);
void inputfield_scroll_to_end(InputField *field);
void inputfield_update_size(InputField *field, int new_width);
void inputfield_set_disabled(InputField *field, bool disabled);
void inputfield_set_readonly(InputField *field, bool readonly);

// Path completion functions
void inputfield_enable_path_completion(InputField *field, bool enable);
void inputfield_set_completion_base_dir(InputField *field, const char *dir);
void inputfield_show_completions(InputField *field, Display *dpy, Window parent_window);
void inputfield_show_completions_at(InputField *field, Display *dpy, Window parent_window, int x, int y);
void inputfield_hide_completions(InputField *field, Display *dpy);
bool inputfield_handle_completion_click(InputField *field, int x, int y);
bool inputfield_handle_dropdown_scroll(InputField *field, int direction, Display *dpy);
void inputfield_apply_completion(InputField *field, int index);
bool inputfield_is_completion_window(InputField *field, Window window);
void inputfield_redraw_completion(InputField *field, Display *dpy);
bool inputfield_has_dropdown_open(InputField *field);

#endif