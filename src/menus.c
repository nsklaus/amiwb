#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include "menus.h"
#include "events.h"

#define MENUBAR_HEIGHT 20
#define MENU_WIDTH 120
#define MENU_ITEM_HEIGHT 20

static Window menubar_window = 0, menu_window = 0;
static GC menubar_gc = NULL;
static XFontStruct *menubar_font = NULL;
static const char *menu_items[] = {"clean icons", "icon info", "", "quit amiwb"};
static const int num_menu_items = 4;

// Initialize menubar and menu
void menus_init(Display *dpy, Window root, int screen, XFontStruct *font) {
    menubar_font = font;
    menubar_window = XCreateSimpleWindow(dpy, root, 0, 0, 800, MENUBAR_HEIGHT, 0,
                                        BlackPixel(dpy, screen), 0xFFFFFFFF);
    XSetWindowAttributes attrs = { .override_redirect = True };
    XChangeWindowAttributes(dpy, menubar_window, CWOverrideRedirect, &attrs);
    XSelectInput(dpy, menubar_window, ButtonPressMask | ButtonReleaseMask | PointerMotionMask | ExposureMask);
    menubar_gc = XCreateGC(dpy, menubar_window, 0, NULL);
    if (menubar_gc && menubar_font) XSetFont(dpy, menubar_gc, menubar_font->fid);
    XMapWindow(dpy, menubar_window);
    XRaiseWindow(dpy, menubar_window);

    menu_window = XCreateSimpleWindow(dpy, root, 5, MENUBAR_HEIGHT, MENU_WIDTH, num_menu_items * MENU_ITEM_HEIGHT, 0,
                                     BlackPixel(dpy, screen), 0xFFFFFFFF);
    XChangeWindowAttributes(dpy, menu_window, CWOverrideRedirect, &attrs);
    XSelectInput(dpy, menu_window, ButtonPressMask | ButtonReleaseMask | PointerMotionMask | ExposureMask);
    XRaiseWindow(dpy, menu_window);  // Ensure menu is above all windows
}

// Get menubar window
Window get_menubar_window(void) {
    return menubar_window;
}

// Get menu window
Window get_menu_window(void) {
    return menu_window;
}

// Handle menu selection (LMB)
int handle_menu_selection(Display *dpy, int x, int y) {
    if (y < 0 || y >= num_menu_items * MENU_ITEM_HEIGHT) return 0;
    int item_index = y / MENU_ITEM_HEIGHT;
    if (item_index >= num_menu_items || menu_items[item_index][0] == '\0') return 0;
    XUnmapWindow(dpy, menu_window);
    if (item_index == 0) clean_icons(dpy);
    else if (item_index == 1) printf("[menus] INFO: Icon info placeholder selected\n");
    else if (item_index == 3) quit_amiwb(dpy);
    return 1;
}

// Clean up menu resources
void cleanup_menus(Display *dpy) {
    if (menu_window) {
        XDestroyWindow(dpy, menu_window);
        menu_window = 0;
    }
    if (menubar_window) {
        XDestroyWindow(dpy, menubar_window);
        menubar_window = 0;
    }
    if (menubar_gc) {
        XFreeGC(dpy, menubar_gc);
        menubar_gc = NULL;
    }
}
