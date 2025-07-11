#include <stdio.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/extensions/shape.h>
#include "render.h"
#include "menus.h"

extern XFontStruct *desktop_font;
extern unsigned long desktop_label_color;

// Render menubar
void render_menubar(Display *dpy) {
    Window menubar = get_menubar_window();
    GC gc = XCreateGC(dpy, menubar, 0, NULL);
    if (!menubar || !gc) return;
    XClearWindow(dpy, menubar);
    XSetForeground(dpy, gc, 0xFF000000);
    XDrawString(dpy, menubar, gc, 5, 15, "menu1", 5);
    XFreeGC(dpy, gc);
    XFlush(dpy);
}

// Render menu
void render_menu(Display *dpy, int highlight_index) {
    Window menu = get_menu_window();
    GC gc = XCreateGC(dpy, menu, 0, NULL);
    if (!menu || !gc) return;
    XClearWindow(dpy, menu);
    static const char *menu_items[] = {"clean icons", "icon info", "", "quit amiwb"};
    for (int i = 0; i < 4; i++) {
        if (menu_items[i][0] == '\0') continue;
        if (i == highlight_index) {
            XSetForeground(dpy, gc, 0xFF000000);
            XFillRectangle(dpy, menu, gc, 0, i * 20, 120, 20);
            XSetForeground(dpy, gc, 0xFFFFFFFF);
        } else {
            XSetForeground(dpy, gc, 0xFF000000);
        }
        XDrawString(dpy, menu, gc, 5, (i + 1) * 20 - 5, menu_items[i], strlen(menu_items[i]));
    }
    XFreeGC(dpy, gc);
    XFlush(dpy);
}

// Render icon
void render_icon(Display *dpy, FileIcon *icon) {
    if (!icon->icon.image || !icon->gc) return;
    XClearWindow(dpy, icon->window);
    XPutImage(dpy, icon->window, icon->gc, icon->icon.image, 0, 0, 0, 0, icon->icon.width, icon->icon.height);
    int label_width = desktop_font ? XTextWidth(desktop_font, icon->label, strlen(icon->label)) : strlen(icon->label) * 8;
    if (desktop_font) XSetFont(dpy, icon->gc, desktop_font->fid);
    XSetForeground(dpy, icon->gc, desktop_label_color);
    XDrawString(dpy, icon->window, icon->gc, (icon->width - label_width) / 2,
                icon->icon.height + 15, icon->label, strlen(icon->label));
    if (icon->shape_mask) {
        XShapeCombineMask(dpy, icon->window, ShapeBounding, 0, 0, icon->shape_mask, ShapeSet);
    }
}
