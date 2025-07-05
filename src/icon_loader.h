#ifndef ICON_LOADER_H   // prevent multiple inclusions of this header
#define ICON_LOADER_H   // define the ICON_LOADER_H macro

#include <X11/Xlib.h>   // Xlib for X11 types (Window, XImage,..)

// define Icon struct for desktop icon properties
typedef struct {

    // Store the X11 window ID for the icon
    Window window;

    // store the icon’s x, y coordinates on the desktop
    int x, y;

    // store the icon’s width and height
    int width, height;

    // store the icon’s image data
    XImage *image;

// close the Icon struct definition
} Icon;

// declare function to load and display an icon from a .info file
int load_do(Display *dpy, Window root, GC gc, const char *name, Icon *icon);

// end the header guard
#endif
