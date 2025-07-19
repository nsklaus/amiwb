/* Menus header: Defines menubar structure and functions for creation, drawing, handling events. For top menu system. */

#ifndef MENUS_H
#define MENUS_H

#include "intuition.h"

// Menubar struct.
typedef struct {
    Window win; // Menubar window.
    Pixmap backing; // Backing pixmap.
    Picture back_pic; // Backing picture.
    Picture win_pic; // Window picture.
    int width; // Width.
    bool menus_open; // Menus open flag.
    int hovered_menu; // Hovered menu index (-1 none).
    int hovered_item; // Hovered submenu item (-1 none).
    Window submenu_win; // Submenu window.
    Pixmap submenu_backing; // Submenu backing.
    Picture submenu_back_pic; // Submenu backing picture.
    Picture submenu_win_pic; // Submenu window picture.
    int submenu_x; // Submenu x.
    int submenu_width; // Submenu width.
    int submenu_height; // Submenu height.
    int submenu_menu; // Current submenu menu index.
    int menu_spacing; // Dynamic spacing.
    XRenderColor menubar_bg; // Menubar bg.
    XRenderColor menubar_fg; // Menubar fg.
    XRenderColor highlight_bg; // Highlight bg.
    XRenderColor highlight_fg; // Highlight fg.
    XRenderColor gray_fg; // Gray fg for disabled items.
} MenuBar;

// Function prototypes.
void create_menubar(RenderContext *ctx, Window root, MenuBar *menubar); 
void draw_menubar(RenderContext *ctx, MenuBar *menubar); 
void draw_submenu(RenderContext *ctx, MenuBar *menubar); 
void handle_menubar_event(RenderContext *ctx, XEvent *ev, MenuBar *menubar, Canvas *desktop, int *running); 
void close_menus(RenderContext *ctx, MenuBar *menubar); 

#endif