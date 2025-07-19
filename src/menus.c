/* Menus implementation: Creates/drawing menubar/submenus, handles menu events, activates items. Manages UI menus. */

#include "menus.h"
#include "render.h"
#include "workbench.h"
#include "config.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrender.h>
#include <X11/Xft/Xft.h>
#include <X11/cursorfont.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>  // fork.
#include <sys/types.h>  // pid_t.

// Menu labels.
static const char *menu_labels[] = {"Workbench", "Window", "Icons", "Tools"};
static const int num_menus = sizeof(menu_labels) / sizeof(menu_labels[0]);

// Submenus.
static const char *submenu_workbench[] = {"Execute ..", "Open..", "", "Quit AmiWB"};
static const int num_workbench = sizeof(submenu_workbench) / sizeof(submenu_workbench[0]);

static const char *submenu_window[] = {"Open", "Close", "Iconify"};
static const int num_window = sizeof(submenu_window) / sizeof(submenu_window[0]);

static const char *submenu_icons[] = {"Clean icons", "Rename"};
static const int num_icons = sizeof(submenu_icons) / sizeof(submenu_icons[0]);

static const char *submenu_tools[] = {"Reset Amiwb", "Shell"};
static const int num_tools = sizeof(submenu_tools) / sizeof(submenu_tools[0]);

static const char **submenus[] = {submenu_workbench, submenu_window, submenu_icons, submenu_tools};
static const int *num_items[] = {&num_workbench, &num_window, &num_icons, &num_tools};

// Get text width using Xft.
static int get_text_width(RenderContext *ctx, const char *text) {
    XGlyphInfo extents;
    XftTextExtentsUtf8(ctx->dpy, ctx->font, (FcChar8 *)text, strlen(text), &extents);
    return extents.xOff;
}

// Menu width with padding.
static int get_menu_width(RenderContext *ctx, const char *label) {
    return get_text_width(ctx, label) + 10;
}

// Max submenu item width.
static int get_submenu_width(RenderContext *ctx, int menu_idx) {
    int max_w = get_text_width(ctx, "          ");
    for (int i = 0; i < *num_items[menu_idx]; i++) {
        if (strlen(submenus[menu_idx][i]) == 0) continue;
        int w = get_text_width(ctx, submenus[menu_idx][i]) + 10;
        if (w > max_w) max_w = w;
    }
    return max_w;
}

// Activate menu item action.
static void activate_item(RenderContext *ctx, MenuBar *menubar, int item_idx, Canvas *active_canvas, Canvas *desktop, int *running) {
    int menu_idx = menubar->submenu_menu;
    const char *item = submenus[menu_idx][item_idx];
    if (strcmp(item, "Quit AmiWB") == 0) {
        *running = 0;  // Exit loop.
    } else if (strcmp(item, "Shell") == 0) {
        if (fork() == 0) {  // Child process.
            system("xrdb ~/.Xresources"); // system() blocks until the command completes and resume the program
            execlp("xterm", "xterm", NULL);  // execlp() replaces the program entirely with the new process
            exit(1);
        }
    } else if (strcmp(item, "Clean icons") == 0) {
        Canvas *target = active_canvas;
        if (!active_canvas || active_canvas->titlebar_height == 0) target = desktop;
        if (target && !target->client_win) {
            align_icons(target);  // Realign.
            redraw_canvas(ctx, target, NULL);
        }
    } else if (strcmp(item, "Iconify") == 0) {
        if (active_canvas && active_canvas->titlebar_height > 0) {
            iconify_canvas(ctx, active_canvas, desktop);
        }
    } // Stub others
    close_menus(ctx, menubar);
    draw_menubar(ctx, menubar);
}

// Create menubar window.
void create_menubar(RenderContext *ctx, Window root, MenuBar *menubar) {
    menubar->width = DisplayWidth(ctx->dpy, DefaultScreen(ctx->dpy));
    menubar->menus_open = false;
    menubar->hovered_menu = -1;
    menubar->hovered_item = -1;
    menubar->submenu_win = None;
    menubar->menubar_bg = (XRenderColor){0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}; // White
    menubar->menubar_fg = (XRenderColor){0x0000, 0x0000, 0x0000, 0xFFFF}; // Black
    menubar->highlight_bg = (XRenderColor){0x0000, 0x0000, 0x0000, 0xFFFF}; // Black
    menubar->highlight_fg = (XRenderColor){0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}; // White
    menubar->gray_fg = (XRenderColor){0x8888, 0x8888, 0x8888, 0xFFFF}; // Gray
    menubar->menu_spacing = get_text_width(ctx, "     "); // 5 spaces
    XSetWindowAttributes attrs = {0};
    attrs.event_mask = EnterWindowMask | LeaveWindowMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | StructureNotifyMask;
    menubar->win = create_canvas_window(ctx, root, 0, 0, menubar->width, MENUBAR_HEIGHT, &attrs);
    XMapWindow(ctx->dpy, menubar->win);
    menubar->backing = XCreatePixmap(ctx->dpy, menubar->win, menubar->width, MENUBAR_HEIGHT, 32);
    menubar->back_pic = XRenderCreatePicture(ctx->dpy, menubar->backing, ctx->fmt, 0, NULL);
    menubar->win_pic = XRenderCreatePicture(ctx->dpy, menubar->win, ctx->fmt, 0, NULL);
    draw_menubar(ctx, menubar);
}

// Draw menubar content.
void draw_menubar(RenderContext *ctx, MenuBar *menubar) {
    XRenderFillRectangle(ctx->dpy, PictOpSrc, menubar->back_pic, &menubar->menubar_bg, 0, 0, menubar->width, MENUBAR_HEIGHT);
    XftDraw *draw = XftDrawCreate(ctx->dpy, menubar->backing, ctx->visual, ctx->cmap);
    if (!menubar->menus_open) {
        XftColor text_fg;
        text_fg.color = menubar->menubar_fg;
        XftDrawStringUtf8(draw, &text_fg, ctx->font, 15, 15, (FcChar8 *)"AmiDesktop", strlen("AmiDesktop"));  // Default label.
    }
    if (menubar->menus_open) {
        int x = 15;
        for (int i = 0; i < num_menus; i++) {
            bool highlighted = (i == menubar->hovered_menu);
            if (highlighted) {
                XRenderFillRectangle(ctx->dpy, PictOpSrc, menubar->back_pic, &menubar->highlight_bg, x, 0, get_menu_width(ctx, menu_labels[i]), MENUBAR_HEIGHT);
            }
            XftColor text_col;
            text_col.color = highlighted ? menubar->highlight_fg : menubar->menubar_fg;
            XftDrawStringUtf8(draw, &text_col, ctx->font, x + 5, 15, (FcChar8 *)menu_labels[i], strlen(menu_labels[i]));
            x += get_menu_width(ctx, menu_labels[i]) + menubar->menu_spacing;
        }
    }
    XftDrawDestroy(draw);
    XRenderComposite(ctx->dpy, PictOpSrc, menubar->back_pic, None, menubar->win_pic, 0, 0, 0, 0, 0, 0, menubar->width, MENUBAR_HEIGHT);
    XSync(ctx->dpy, False);
}

// Draw open submenu.
void draw_submenu(RenderContext *ctx, MenuBar *menubar) {
    int menu_idx = menubar->submenu_menu;
    int num = *num_items[menu_idx];
    menubar->submenu_width = get_submenu_width(ctx, menu_idx);
    menubar->submenu_height = num * MENU_ITEM_HEIGHT;
    if (menubar->submenu_win == None) {
        XSetWindowAttributes attrs = {0};
        attrs.colormap = ctx->cmap;
        attrs.border_pixel = 0;
        attrs.background_pixel = 0;
        attrs.override_redirect = True;
        attrs.event_mask = PointerMotionMask | ButtonPressMask | ButtonReleaseMask | LeaveWindowMask | EnterWindowMask;
        menubar->submenu_win = XCreateWindow(ctx->dpy, DefaultRootWindow(ctx->dpy), menubar->submenu_x, MENUBAR_HEIGHT, menubar->submenu_width, menubar->submenu_height, 0, 32, InputOutput, ctx->visual, CWColormap | CWBorderPixel | CWBackPixel | CWOverrideRedirect | CWEventMask, &attrs);
        Cursor cursor = XCreateFontCursor(ctx->dpy, XC_left_ptr);
        XDefineCursor(ctx->dpy, menubar->submenu_win, cursor);
        menubar->submenu_backing = XCreatePixmap(ctx->dpy, menubar->submenu_win, menubar->submenu_width, menubar->submenu_height, 32);
        menubar->submenu_back_pic = XRenderCreatePicture(ctx->dpy, menubar->submenu_backing, ctx->fmt, 0, NULL);
        menubar->submenu_win_pic = XRenderCreatePicture(ctx->dpy, menubar->submenu_win, ctx->fmt, 0, NULL);
        XMapRaised(ctx->dpy, menubar->submenu_win);  // Map and raise.
    }
    XRenderFillRectangle(ctx->dpy, PictOpSrc, menubar->submenu_back_pic, &menubar->menubar_bg, 0, 0, menubar->submenu_width, menubar->submenu_height);
    XftDraw *draw = XftDrawCreate(ctx->dpy, menubar->submenu_backing, ctx->visual, ctx->cmap);
    for (int i = 0; i < num; i++) {
        const char *item = submenus[menu_idx][i];
        if (strlen(item) == 0) { // Separator
            XRenderColor sep_color = {0x8888, 0x8888, 0x8888, 0xFFFF};
            XRenderFillRectangle(ctx->dpy, PictOpSrc, menubar->submenu_back_pic, &sep_color, 0, i * MENU_ITEM_HEIGHT + MENU_ITEM_HEIGHT / 2, menubar->submenu_width, 1);
            continue;
        }
        bool highlighted = (i == menubar->hovered_item);
        bool is_enabled = true;
        if (menu_idx == 1 && strcmp(item, "Iconify") == 0) {
            is_enabled = (ctx->active_canvas && ctx->active_canvas->titlebar_height > 0);
        }
        if (!is_enabled) highlighted = false;
        if (highlighted) {
            XRenderFillRectangle(ctx->dpy, PictOpSrc, menubar->submenu_back_pic, &menubar->highlight_bg, 0, i * MENU_ITEM_HEIGHT, menubar->submenu_width, MENU_ITEM_HEIGHT);
        }
        XftColor text_col;
        text_col.color = is_enabled ? (highlighted ? menubar->highlight_fg : menubar->menubar_fg) : menubar->gray_fg;
        XftDrawStringUtf8(draw, &text_col, ctx->font, 5, i * MENU_ITEM_HEIGHT + 15, (FcChar8 *)item, strlen(item));
    }
    XftDrawDestroy(draw);
    XRenderComposite(ctx->dpy, PictOpSrc, menubar->submenu_back_pic, None, menubar->submenu_win_pic, 0, 0, 0, 0, 0, 0, menubar->submenu_width, menubar->submenu_height);
    XSync(ctx->dpy, False);
}

// Close open menus.
void close_menus(RenderContext *ctx, MenuBar *menubar) {
    menubar->menus_open = false;
    menubar->hovered_menu = -1;
    menubar->hovered_item = -1;
    if (menubar->submenu_win != None) {
        XUnmapWindow(ctx->dpy, menubar->submenu_win);
        XSync(ctx->dpy, False);
        XRenderFreePicture(ctx->dpy, menubar->submenu_win_pic);
        XRenderFreePicture(ctx->dpy, menubar->submenu_back_pic);
        XDestroyWindow(ctx->dpy, menubar->submenu_win);
        XFreePixmap(ctx->dpy, menubar->submenu_backing);
        XSync(ctx->dpy, False);
        menubar->submenu_win = None;
    }
}

// Handle events for menubar/submenu.
void handle_menubar_event(RenderContext *ctx, XEvent *ev, MenuBar *menubar, Canvas *desktop, int *running) {
    if (ev->type == ConfigureNotify) {  // Resize.
        menubar->width = ev->xconfigure.width;
        XFreePixmap(ctx->dpy, menubar->backing);
        menubar->backing = XCreatePixmap(ctx->dpy, menubar->win, menubar->width, MENUBAR_HEIGHT, 32);
        XRenderFreePicture(ctx->dpy, menubar->back_pic);
        menubar->back_pic = XRenderCreatePicture(ctx->dpy, menubar->backing, ctx->fmt, 0, NULL);
        XRenderFreePicture(ctx->dpy, menubar->win_pic);
        menubar->win_pic = XRenderCreatePicture(ctx->dpy, menubar->win, ctx->fmt, 0, NULL);
        draw_menubar(ctx, menubar);
    } else if (ev->type == ButtonPress) {
        if (ev->xbutton.button == 3) { // RMB toggle menus.
            if (!menubar->menus_open) {
                menubar->menus_open = true;
                draw_menubar(ctx, menubar);
            } else {
                close_menus(ctx, menubar);
                draw_menubar(ctx, menubar);
            }
        } else if (ev->xbutton.button == 1) { // LMB activate or close.
            if (menubar->menus_open) {
                if (ev->xany.window == menubar->submenu_win && menubar->hovered_item != -1) {
                    activate_item(ctx, menubar, menubar->hovered_item, ctx->active_canvas, desktop, running);
                    return;
                }
                close_menus(ctx, menubar);
                draw_menubar(ctx, menubar);
            }
        }
    } else if (ev->type == MotionNotify) {
        int x = ev->xmotion.x;
        int y = ev->xmotion.y;
        if (ev->xany.window == menubar->win) {
            if (menubar->menus_open) {
                int menu_x = 15;
                menubar->hovered_menu = -1;
                for (int i = 0; i < num_menus; i++) {
                    int w = get_menu_width(ctx, menu_labels[i]);
                    if (x >= menu_x && x < menu_x + w && y >= 0 && y < MENUBAR_HEIGHT) {
                        menubar->hovered_menu = i;
                        break;
                    }
                    menu_x += get_menu_width(ctx, menu_labels[i]) + menubar->menu_spacing;
                }
                draw_menubar(ctx, menubar);
                if (menubar->hovered_menu != -1) {
                    if (menubar->submenu_menu != menubar->hovered_menu || menubar->submenu_win == None) {
                        if (menubar->submenu_win != None) {  // Close old submenu.
                            XUnmapWindow(ctx->dpy, menubar->submenu_win);
                            XSync(ctx->dpy, False);
                            XRenderFreePicture(ctx->dpy, menubar->submenu_win_pic);
                            XRenderFreePicture(ctx->dpy, menubar->submenu_back_pic);
                            XDestroyWindow(ctx->dpy, menubar->submenu_win);
                            XFreePixmap(ctx->dpy, menubar->submenu_backing);
                            XSync(ctx->dpy, False);
                            menubar->submenu_win = None;
                        }
                        menubar->submenu_menu = menubar->hovered_menu;
                        menubar->submenu_x = 15;
                        for (int j = 0; j < menubar->hovered_menu; j++) {
                            menubar->submenu_x += get_menu_width(ctx, menu_labels[j]) + menubar->menu_spacing;
                        }
                        menubar->hovered_item = -1;
                        draw_submenu(ctx, menubar);  // Draw new.
                    }
                } else {
                    menubar->hovered_item = -1;
                    // Do not close submenu if mouse leaves menubar but enters submenu
                    int root_x, root_y;
                    Window root_child, child;
                    unsigned int mask;
                    XQueryPointer(ctx->dpy, menubar->win, &root_child, &child, &root_x, &root_y, &x, &y, &mask);
                    if (child == menubar->submenu_win) return;
                    if (menubar->submenu_win != None) {  // Close.
                        XUnmapWindow(ctx->dpy, menubar->submenu_win);
                        XSync(ctx->dpy, False);
                        XRenderFreePicture(ctx->dpy, menubar->submenu_win_pic);
                        XRenderFreePicture(ctx->dpy, menubar->submenu_back_pic);
                        XDestroyWindow(ctx->dpy, menubar->submenu_win);
                        XFreePixmap(ctx->dpy, menubar->submenu_backing);
                        XSync(ctx->dpy, False);
                        menubar->submenu_win = None;
                    }
                }
            }
        } else if (ev->xany.window == menubar->submenu_win) {
            menubar->hovered_item = y / MENU_ITEM_HEIGHT;
            if (strlen(submenus[menubar->submenu_menu][menubar->hovered_item]) == 0) menubar->hovered_item = -1; // Separator
            draw_submenu(ctx, menubar);
        }
    } else if (ev->type == LeaveNotify) {
        if (ev->xany.window == menubar->win) {
            int root_x, root_y, win_x, win_y;
            Window root_child, child;
            unsigned int mask;
            XQueryPointer(ctx->dpy, DefaultRootWindow(ctx->dpy), &root_child, &child, &root_x, &root_y, &win_x, &win_y, &mask);
            if (child == menubar->submenu_win) return;
            menubar->hovered_menu = -1;
            draw_menubar(ctx, menubar);
            close_menus(ctx, menubar);
        } else if (ev->xany.window == menubar->submenu_win) {
            int root_x, root_y, win_x, win_y;
            Window root_child, child;
            unsigned int mask;
            XQueryPointer(ctx->dpy, DefaultRootWindow(ctx->dpy), &root_child, &child, &root_x, &root_y, &win_x, &win_y, &mask);
            if (child == menubar->win) {
                menubar->hovered_item = -1;
                draw_submenu(ctx, menubar);
                return;
            }
            menubar->hovered_item = -1;
            close_menus(ctx, menubar);
            draw_menubar(ctx, menubar);
        }
    } else if (ev->type == EnterNotify) {
        if (ev->xany.window == menubar->submenu_win) {
            // Keep open
        }
    } else if (ev->type == Expose) {
        draw_menubar(ctx, menubar);
        if (menubar->submenu_win != None) draw_submenu(ctx, menubar);
    }
}