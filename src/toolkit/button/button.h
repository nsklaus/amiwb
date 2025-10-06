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
    bool clicked;  // True when button was clicked (press and release)
    XftFont *font;      // Font to use for rendering (borrowed from app, don't free)
    Visual *visual;     // Visual for rendering (borrowed from canvas, cached from xft_draw)
    Colormap colormap;  // Colormap for rendering (borrowed from canvas, cached from xft_draw)
    void (*on_click)(void *user_data);
    void *user_data;
} Button;

Button* button_create(int x, int y, int width, int height, const char *label, XftFont *font);
void button_destroy(Button *button);
void button_set_callback(Button *button, void (*on_click)(void*), void *user_data);
void button_set_pressed(Button *button, bool pressed);
void button_render(Button *button, Picture dest, Display *dpy, XftDraw *xft_draw);
bool button_handle_press(Button *button, int click_x, int click_y);
bool button_handle_release(Button *button, int click_x, int click_y);
bool button_is_clicked(Button *button);

#endif