#include <stdio.h>
#include <X11/Xlib.h>
#include "decoration.h"

#define BUTTON_SIZE 15
#define BORDER 2
#define TITLE_HEIGHT 20
#define BOTTOM_HEIGHT 20

// Update frame decorations during resize
void update_frame_decorations(Display *dpy, FrameInfo *frame, int new_w, int new_h) {
    if (!frame || !frame->frame.window || !XGetWindowAttributes(dpy, frame->frame.window, &(XWindowAttributes){})) return;
    if (new_w < 100) new_w = 100;
    if (new_h < 100) new_h = 100;
    int new_button_x = new_w - BUTTON_SIZE - BORDER;
    int new_button_y = new_h - BOTTOM_HEIGHT - BORDER;
    if (frame->resize_button.window) {
        XMoveWindow(dpy, frame->resize_button.window, new_button_x, new_button_y);
        XSetWindowBackground(dpy, frame->resize_button.window, 0xFF00FF00);
        XClearWindow(dpy, frame->resize_button.window);
    }
    if (frame->iconify_button.window) {
        XMoveWindow(dpy, frame->iconify_button.window, new_w - BUTTON_SIZE - BORDER - BUTTON_SIZE - 2, 2);
        XSetWindowBackground(dpy, frame->iconify_button.window, 0xFFFFFF00);
        XClearWindow(dpy, frame->iconify_button.window);
    }
    if (frame->lower_button.window) {
        XMoveWindow(dpy, frame->lower_button.window, new_w - BUTTON_SIZE - BORDER, 2);
        XSetWindowBackground(dpy, frame->lower_button.window, 0xFF9F9F9F);
        XClearWindow(dpy, frame->lower_button.window);
    }
    XResizeWindow(dpy, frame->frame.window, new_w, new_h);
    if (frame->client.window) {
        XWindowChanges changes = { .width = new_w - 2 * BORDER, .height = new_h - TITLE_HEIGHT - BOTTOM_HEIGHT };
        if (changes.width < 50) changes.width = 50;
        if (changes.height < 50) changes.height = 50;
        if (XGetWindowAttributes(dpy, frame->client.window, &(XWindowAttributes){})) {
            XConfigureWindow(dpy, frame->client.window, CWWidth | CWHeight, &changes);
        }
    }
    frame->frame.width = new_w;
    frame->frame.height = new_h;
    frame->resize_button.x = new_button_x;
    frame->resize_button.y = new_button_y;
    frame->iconify_button.x = new_w - BUTTON_SIZE - BORDER - BUTTON_SIZE - 2;
    frame->lower_button.x = new_w - BUTTON_SIZE - BORDER;
}
