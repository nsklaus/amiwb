#ifndef TOOLKIT_BUTTON_H
#define TOOLKIT_BUTTON_H

#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>
#include <X11/Xft/Xft.h>
#include <stdbool.h>

typedef struct Button {
    int x, y;
    int width, height;
    char *label;
    bool pressed;
    bool hover;
    void (*on_click)(void *user_data);
    void *user_data;
} Button;

Button* button_create(int x, int y, int width, int height, const char *label);
void button_destroy(Button *button);
void button_set_callback(Button *button, void (*on_click)(void*), void *user_data);
void button_set_pressed(Button *button, bool pressed);
void button_draw(Button *button, Picture dest, Display *dpy, XftDraw *xft_draw, XftFont *font);
bool button_handle_click(Button *button, int click_x, int click_y);
bool button_handle_release(Button *button, int click_x, int click_y);

#endif