#include "button.h"
#include "../amiwb/config.h"
#include <stdlib.h>
#include <string.h>
#include <X11/Xft/Xft.h>

Button* button_create(int x, int y, int width, int height, const char *label, XftFont *font) {
    Button *button = malloc(sizeof(Button));
    if (!button) {
        fprintf(stderr, "[ERROR] Button: Failed to allocate memory (size=%zu)\n", sizeof(Button));
        return NULL;
    }

    button->x = x;
    button->y = y;
    button->width = width;
    button->height = height;
    button->font = font;  // Store the font reference

    if (label) {
        button->label = strdup(label);
        if (!button->label) {
            free(button);
            return NULL;
        }
    } else {
        button->label = NULL;
    }

    button->pressed = false;
    button->hover = false;
    button->clicked = false;
    button->on_click = NULL;
    button->user_data = NULL;

    return button;
}

void button_destroy(Button *button) {
    if (!button) {
        return;  // Not fatal - cleanup function
    }
    if (button->label) free(button->label);
    free(button);
}

void button_set_callback(Button *button, void (*on_click)(void*), void *user_data) {
    if (!button) {
        return;
    }
    button->on_click = on_click;
    button->user_data = user_data;
}

void button_set_pressed(Button *button, bool pressed) {
    if (!button) {
        return;
    }
    button->pressed = pressed;
}

void button_render(Button *button, Picture dest, Display *dpy, XftDraw *xft_draw) {
    if (!button || !dpy || dest == None || !xft_draw) {
        return;
    }
    
    int x = button->x;
    int y = button->y;
    int w = button->width;
    int h = button->height;
    
    // Get colors from config.h
    XRenderColor gray = GRAY;
    XRenderColor white = WHITE;
    XRenderColor black = BLACK;
    XRenderColor dark = {0x5555, 0x5555, 0x5555, 0xffff};  // Dark gray
    XRenderColor blue = BLUE;
    
    if (button->pressed) {
        // Pressed state - inset appearance
        XRenderFillRectangle(dpy, PictOpSrc, dest, &dark, x, y, w, h);      // Outer border
        XRenderFillRectangle(dpy, PictOpSrc, dest, &black, x, y, w-1, 1);   // Top (black)
        XRenderFillRectangle(dpy, PictOpSrc, dest, &black, x, y, 1, h-1);   // Left (black)
        XRenderFillRectangle(dpy, PictOpSrc, dest, &white, x+w-1, y, 1, h); // Right (white)
        XRenderFillRectangle(dpy, PictOpSrc, dest, &white, x, y+h-1, w, 1); // Bottom (white)
        XRenderFillRectangle(dpy, PictOpSrc, dest, &blue, x+1, y+1, w-2, h-2);
    } else {
        // Normal raised state
        XRenderFillRectangle(dpy, PictOpSrc, dest, &dark, x, y, w, h);      // Outer border
        XRenderFillRectangle(dpy, PictOpSrc, dest, &white, x, y, w-1, 1);   // Top (white)
        XRenderFillRectangle(dpy, PictOpSrc, dest, &white, x, y, 1, h-1);   // Left (white)
        XRenderFillRectangle(dpy, PictOpSrc, dest, &black, x+w-1, y, 1, h); // Right (black)
        XRenderFillRectangle(dpy, PictOpSrc, dest, &black, x, y+h-1, w, 1); // Bottom (black)
        XRenderFillRectangle(dpy, PictOpSrc, dest, &gray, x+1, y+1, w-2, h-2);
    }
    
    // Draw label text if we have a font
    if (button->label && button->font) {
        // Calculate text position for centering
        XGlyphInfo extents;
        XftTextExtentsUtf8(dpy, button->font, (FcChar8*)button->label,
                          strlen(button->label), &extents);
        
        int text_x = x + (w - extents.width) / 2;
        int text_y = y + (h + button->font->ascent - button->font->descent) / 2;
        
        // Adjust position if button is pressed
        if (button->pressed) {
            text_x += 1;
            text_y += 1;
        }
        
        // Set up text color (black on gray background)
        XftColor text_color;
        XRenderColor black = {0x0000, 0x0000, 0x0000, 0xffff};
        XftColorAllocValue(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                          DefaultColormap(dpy, DefaultScreen(dpy)),
                          &black, &text_color);
        
        // Draw the text
        XftDrawStringUtf8(xft_draw, &text_color, button->font,
                         text_x, text_y,
                         (FcChar8*)button->label, strlen(button->label));
        
        XftColorFree(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                    DefaultColormap(dpy, DefaultScreen(dpy)), &text_color);
    }
}

bool button_handle_press(Button *button, int click_x, int click_y) {
    if (!button) {
        return false;
    }

    if (click_x >= button->x && click_x < button->x + button->width &&
        click_y >= button->y && click_y < button->y + button->height) {
        button->pressed = true;
        button->clicked = false;  // Reset clicked state
        return true;
    }
    return false;
}

bool button_handle_release(Button *button, int click_x, int click_y) {
    if (!button) {
        return false;
    }

    bool was_pressed = button->pressed;
    button->pressed = false;

    if (was_pressed &&
        click_x >= button->x && click_x < button->x + button->width &&
        click_y >= button->y && click_y < button->y + button->height) {
        button->clicked = true;  // Mark as clicked
        if (button->on_click) {
            button->on_click(button->user_data);
        }
        return true;
    }
    return false;
}

bool button_is_clicked(Button *button) {
    if (!button) {
        return false;
    }
    bool was_clicked = button->clicked;
    button->clicked = false;  // Reset after checking
    return was_clicked;
}