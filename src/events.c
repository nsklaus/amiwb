/* Events implementation: Main event loop for handling X events like configure, expose, key/mouse input, map requests. Manages interactions. */

#include "events.h"
#include "intuition.h"
#include "workbench.h"
#include "render.h"
#include "menus.h"
#include "config.h"
#include <X11/Xlib.h>  
#include <X11/Xutil.h>  // XGetWindowAttributes.
#include <X11/extensions/Xrender.h>  
#include <X11/extensions/Xrandr.h> 
#include <X11/cursorfont.h>  
#include <stdlib.h>  
#include <sys/types.h>  // Types for pwd.
#include <pwd.h>  // getpwuid for user info.
#include <unistd.h>  // getuid for user ID.

// Static globals for drag/resize state (static means file-scope only).
static bool is_dragging = false;  // Flag if icon is being dragged.
static bool is_resizing = false;  // Flag if window is resizing.
static int resize_dir = 0;  // Direction: 0 none, 1 bottom-right, 2 bottom, 3 right.
static Canvas *resize_canvas = NULL;  // Canvas being resized.
static bool is_window_dragging = false;  // Flag if window is moving.
static Canvas *window_drag_canvas = NULL;  // Canvas being moved.
static unsigned long last_click_time = 0;  // Timestamp of last mouse click.
static int last_click_x = 0, last_click_y = 0;  // Position of last click.
static Canvas *last_click_canvas = NULL;  // Canvas of last click.
static int press_x, press_y;  // Initial mouse press position.
static FileIcon *potential_drag_icon = NULL;  // Icon that might be dragged.
static Canvas *drag_canvas = NULL;  // Canvas of dragged icon.
static int drag_start_x, drag_start_y;  // Drag start position.
static int drag_offset_x, drag_offset_y;  // Offset from mouse to icon corner.

// Find free slot in windows array for new canvas.
int find_free_slot(Canvas *windows, int num_windows, int max_windows) {
    for (int i = 0; i < num_windows; i++) {  // Check existing slots.
        if (windows[i].win == None) return i;  // Return if empty.
    }
    if (num_windows < max_windows) return num_windows;  // Append if room.
    return -1;  // No slot.
}

/*****************************************
 * Central event loop: processes all X events in a while loop.
 * Handles window management, input, drawing.
 * XNextEvent blocks until an event arrives,
 * then dispatches based on type. 
 *****************************************/
void handle_events(RenderContext *ctx, Canvas *desktop, Canvas *windows, int *num_windows, Window root, MenuBar *menubar, int randr_event_base) {
    
    int running = 1;  // Loop flag.
    XEvent ev;  // Event structure.
    Cursor cursor = XCreateFontCursor(ctx->dpy, XC_left_ptr);  // Cursor for grabs.

    while (running) {  // Main loop.
        XNextEvent(ctx->dpy, &ev);  // Wait for and get next event.
        if (ev.xany.window == menubar->win || (menubar->submenu_win != None && ev.xany.window == menubar->submenu_win)) {  // If event for menubar or submenu.
            handle_menubar_event(ctx, &ev, menubar, desktop, &running);  // Handle separately.
            continue;
        }
        Canvas *canvas = desktop;  // Default to desktop.
        for (int j = 0; j < *num_windows; j++) {  // Find canvas for event window.
            if (windows[j].win == None) continue;
            if (ev.xany.window == windows[j].win) {
                canvas = &windows[j];
                break;
            }
        }
        if (menubar->menus_open && (ev.type == ButtonPress || ev.type == ButtonRelease) && (ev.xbutton.button == 1 || ev.xbutton.button == 3)) {  // Close menus on LMB/RMB if open.
            close_menus(ctx, menubar);
            draw_menubar(ctx, menubar);
        } else if (ev.type == ButtonPress && ev.xbutton.button == 1 && canvas == desktop) {  // Activate desktop on LMB.
            activate_canvas(ctx, desktop, windows, *num_windows);
        }
        if (ev.type == ConfigureNotify) {  // Window resized/moved event.
            if (ev.xconfigure.window != canvas->win) continue;  // Not our window.

            if (canvas == desktop) {  // Debug for desktop.
                printf("Desktop ConfigureNotify: old %dx%d, new %dx%d\n", canvas->width, canvas->height, ev.xconfigure.width, ev.xconfigure.height);
            }

            canvas->x = ev.xconfigure.x;  // Update position.
            canvas->y = ev.xconfigure.y;
            canvas->width = ev.xconfigure.width;  // Update size.
            canvas->height = ev.xconfigure.height;
            XFreePixmap(ctx->dpy, canvas->backing);  // Free old backing pixmap.
            canvas->backing = XCreatePixmap(ctx->dpy, canvas->win, canvas->width, canvas->height, 32);  // New backing.
            XRenderFreePicture(ctx->dpy, canvas->back_pic);  // Free old picture.
            canvas->back_pic = XRenderCreatePicture(ctx->dpy, canvas->backing, ctx->fmt, 0, NULL);
            XRenderFreePicture(ctx->dpy, canvas->win_pic);
            canvas->win_pic = XRenderCreatePicture(ctx->dpy, canvas->win, ctx->fmt, 0, NULL);
            if (canvas->client_win) {  // Resize client if present.
                XResizeWindow(ctx->dpy, canvas->client_win, canvas->width - BORDER_WIDTH * 2, canvas->height - TITLEBAR_HEIGHT - BORDER_WIDTH);
                XRenderFreePicture(ctx->dpy, canvas->client_pic);
                canvas->client_pic = XRenderCreatePicture(ctx->dpy, canvas->client_win, XRenderFindVisualFormat(ctx->dpy, canvas->client_visual), 0, NULL);
            }
            if (!canvas->client_win && canvas->titlebar_height == 0) align_icons(canvas);  // Realign icons on desktop/folder.
            redraw_canvas(ctx, canvas, NULL);  // Full redraw.
        } else if (ev.type == randr_event_base + RRScreenChangeNotify) {  // Screen resolution change.
            XRRScreenChangeNotifyEvent *rrev = (XRRScreenChangeNotifyEvent *)&ev;  // Cast event.
            XResizeWindow(ctx->dpy, desktop->win, rrev->width, rrev->height - MENUBAR_HEIGHT);  // Resize desktop.
            XResizeWindow(ctx->dpy, menubar->win, rrev->width, MENUBAR_HEIGHT);  // Resize menubar.
            menubar->width = rrev->width;
            draw_menubar(ctx, menubar);
        } else if (ev.type == Expose) {  // Area needs redraw (e.g., uncovered).
            XRectangle rect = {ev.xexpose.x, ev.xexpose.y, ev.xexpose.width, ev.xexpose.height};  // Dirty rect.
            redraw_canvas(ctx, canvas, &rect);  // Redraw only dirty area.
        } else if (ev.type == KeyPress) {  // Keyboard press.
            KeySym keysym = XLookupKeysym(&ev.xkey, 0);  // Get key symbol.
            if (keysym == XK_Escape && canvas != desktop) {  // Esc: close window.
                close_canvas(ctx, canvas, windows, num_windows);
            } else if (keysym == XK_i && canvas != desktop && !canvas->client_win) {  // i: iconify folder.
                iconify_canvas(ctx, canvas, desktop);
            } else if (keysym == XK_r && canvas != desktop && !canvas->client_win) {  // r: refresh icons.
                refresh_icons(ctx, canvas);
            }
        } else if (ev.type == ButtonPress) {  // Mouse button down.
            if (ev.xbutton.button == 1) {  // Left button.
                press_x = ev.xbutton.x;  // Record press pos.
                press_y = ev.xbutton.y;
                if (canvas != desktop) activate_canvas(ctx, canvas, windows, *num_windows);  // Activate non-desktop.
                FileIcon *selected = find_hit_icon(press_x, press_y, canvas);  // Check icon hit.
                if (selected) {  // If icon.
                    XGrabPointer(ctx->dpy, ev.xbutton.window, False, PointerMotionMask | ButtonReleaseMask, GrabModeAsync, GrabModeAsync, None, cursor, CurrentTime);  // Grab mouse for drag.
                    potential_drag_icon = selected;
                    drag_canvas = canvas;
                    drag_start_x = press_x;
                    drag_start_y = press_y;
                    drag_offset_x = press_x - selected->x;
                    drag_offset_y = press_y - selected->y;
                    is_dragging = false;
                } else if (canvas->titlebar_height > 0 && press_y < canvas->titlebar_height) {  // Titlebar: start window drag.

                    XGrabPointer(ctx->dpy, ev.xbutton.window, False, PointerMotionMask | ButtonReleaseMask, GrabModeAsync, GrabModeAsync, None, cursor, CurrentTime);
                    drag_start_x = ev.xbutton.x_root;
                    drag_start_y = ev.xbutton.y_root;
                    is_window_dragging = true;
                    window_drag_canvas = canvas;
                } else if (canvas != desktop) {  // Check borders for resize.
                    if (press_x > canvas->width - BORDER_WIDTH * 2 && press_y > canvas->height - BORDER_WIDTH * 2) resize_dir = 1;  // Bottom-right.
                    else if (press_y > canvas->height - BORDER_WIDTH) resize_dir = 2;  // Bottom.
                    else if (press_x > canvas->width - BORDER_WIDTH) resize_dir = 3;  // Right.
                    if (resize_dir) {  // Start resize grab.
                        XGrabPointer(ctx->dpy, ev.xbutton.window, False, PointerMotionMask | ButtonReleaseMask, GrabModeAsync, GrabModeAsync, None, cursor, CurrentTime);
                        drag_start_x = ev.xbutton.x_root;
                        drag_start_y = ev.xbutton.y_root;
                        is_resizing = true;
                        resize_canvas = canvas;
                    }
                }
            }
        } else if (ev.type == MotionNotify) {  // Mouse move.
            if (is_window_dragging) {  // Move window.
                int dx = ev.xmotion.x_root - drag_start_x;
                int dy = ev.xmotion.y_root - drag_start_y;
                XMoveWindow(ctx->dpy, window_drag_canvas->win, window_drag_canvas->x + dx, window_drag_canvas->y + dy);
                drag_start_x = ev.xmotion.x_root;
                drag_start_y = ev.xmotion.y_root;
            } else if (is_resizing) {  // Resize window.
                int dx = ev.xmotion.x_root - drag_start_x;
                int dy = ev.xmotion.y_root - drag_start_y;
                int new_width = resize_canvas->width + (resize_dir == 2 ? 0 : dx);
                int new_height = resize_canvas->height + (resize_dir == 3 ? 0 : dy);
                if (new_width > 100 && new_height > 100 + TITLEBAR_HEIGHT) {  // Min size check.
                    XResizeWindow(ctx->dpy, resize_canvas->win, new_width, new_height);
                    drag_start_x = ev.xmotion.x_root;
                    drag_start_y = ev.xmotion.y_root;
                }
            } else if (potential_drag_icon && !is_dragging) {  // Check if drag threshold met.
                int dx = abs(ev.xmotion.x - drag_start_x);
                int dy = abs(ev.xmotion.y - drag_start_y);
                if (dx > DRAG_THRESHOLD || dy > DRAG_THRESHOLD) {
                    is_dragging = true;
                }
            }
            if (is_dragging) {  // Update icon position.
                int new_x = ev.xmotion.x - drag_offset_x;
                int new_y = ev.xmotion.y - drag_offset_y;
                int icon_w = potential_drag_icon->width;
                int icon_h = potential_drag_icon->height;
                int canvas_w = drag_canvas->width;
                int canvas_h = drag_canvas->height;
                new_x = new_x < 0 ? 0 : (new_x + icon_w > canvas_w ? canvas_w - icon_w : new_x);  // Clamp x.
                new_y = new_y < 0 ? 0 : (new_y + icon_h > canvas_h ? canvas_h - icon_h : new_y);  // Clamp y.
                if (new_x != potential_drag_icon->x || new_y != potential_drag_icon->y) {  // If changed.
                    int old_x = potential_drag_icon->x;
                    int old_y = potential_drag_icon->y;
                    int min_x = old_x < new_x ? old_x : new_x;
                    int min_y = old_y < new_y ? old_y : new_y;
                    int u_width = (old_x + icon_w > new_x + icon_w ? old_x + icon_w : new_x + icon_w) - min_x;
                    int u_height = (old_y + icon_h > new_y + icon_h ? old_y + icon_h : new_y + icon_h) - min_y;
                    XRectangle union_rect = {min_x, min_y, u_width, u_height};  // Union rect for redraw.
                    potential_drag_icon->x = new_x;
                    potential_drag_icon->y = new_y;
                    redraw_canvas(ctx, drag_canvas, &union_rect);  // Redraw affected area.
                }
            }
        } else if (ev.type == ButtonRelease) {  // Button up.
            if (is_window_dragging) {  // End window drag.
                XUngrabPointer(ctx->dpy, CurrentTime);
                is_window_dragging = false;
                window_drag_canvas = NULL;
            } else if (is_resizing) {  // End resize.
                XUngrabPointer(ctx->dpy, CurrentTime);
                is_resizing = false;
                resize_dir = 0;
                resize_canvas = NULL;
            } else if (potential_drag_icon) {  // End icon interaction.
                XUngrabPointer(ctx->dpy, CurrentTime);
                if (!is_dragging) {  // If click, not drag.
                    unsigned long time = ev.xbutton.time;
                    int mx = press_x;
                    int my = press_y;
                    FileIcon *selected = find_hit_icon(mx, my, canvas);
                    if (selected && (time - last_click_time < DOUBLE_CLICK_TIME) && (abs(mx - last_click_x) < CLICK_TOLERANCE) && (abs(my - last_click_y) < CLICK_TOLERANCE) && (canvas == last_click_canvas)) {  // Double-click check.
                        if (selected->type == TYPE_ICONIFIED) {  // De-iconify window.
                            Canvas *c = selected->iconified_canvas;
                            c->backing = XCreatePixmap(ctx->dpy, c->win, c->width, c->height, 32);
                            c->back_pic = XRenderCreatePicture(ctx->dpy, c->backing, ctx->fmt, 0, NULL);
                            c->win_pic = XRenderCreatePicture(ctx->dpy, c->win, ctx->fmt, 0, NULL);
                            XMapWindow(ctx->dpy, c->win);  // Show frame.
                            XSync(ctx->dpy, False);
                            if (c->client_win) {
                                c->client_pic = XRenderCreatePicture(ctx->dpy, c->client_win, XRenderFindVisualFormat(ctx->dpy, c->client_visual), 0, NULL);
                                XSync(ctx->dpy, False);
                                Atom wm_state = XInternAtom(ctx->dpy, "WM_STATE", False);
                                long data[2] = {1, None};
                                XChangeProperty(ctx->dpy, c->client_win, wm_state, wm_state, 32, PropModeReplace, (unsigned char*)data, 2);  // Set NormalState.
                            } else {
                                for (int i = 0; i < c->num_icons; i++) {
                                    recreate_icon_pixmap(ctx, &c->icons[i], c);  // Recreate icons.
                                }
                            }
                            XSync(ctx->dpy, False);
                            activate_canvas(ctx, c, windows, *num_windows);
                            int idx = selected - canvas->icons;  // Remove icon from desktop.
                            free_icon(ctx->dpy, selected);
                            memmove(&canvas->icons[idx], &canvas->icons[idx + 1], sizeof(FileIcon) * (canvas->num_icons - idx - 1));
                            canvas->num_icons--;
                            redraw_canvas(ctx, canvas, NULL);
                        } else if (selected->type == TYPE_DRAWER) {  // Open folder.
                            int slot = find_free_slot(windows, *num_windows, MAX_WINDOWS);
                            if (slot == -1) continue;
                            Canvas *new_folder = &windows[slot];
                            if (slot == *num_windows) (*num_windows)++;
                            new_folder->x = 100;
                            new_folder->y = 100 + MENUBAR_HEIGHT;
                            new_folder->width = 640 + BORDER_WIDTH * 2;
                            new_folder->height = 480 + TITLEBAR_HEIGHT + BORDER_WIDTH;
                            new_folder->bg_color = BG_COLOR_FOLDER;
                            new_folder->active = true;
                            new_folder->titlebar_height = TITLEBAR_HEIGHT;
                            new_folder->path = strdup(selected->path);  // Copy path.
                            char *path_copy = strdup(new_folder->path);
                            char *base = strrchr(path_copy, '/');  // Get basename.
                            if (base) base++;
                            if (!base || *base == '\0') {
                                if (strcmp(new_folder->path, "/") == 0) {
                                    new_folder->title = strdup("root");
                                } else {
                                    char *home = getenv("HOME");
                                    char *home_slash = malloc(strlen(home) + 2);
                                    strcpy(home_slash, home);
                                    strcat(home_slash, "/");
                                    if (strcmp(new_folder->path, home_slash) == 0) {
                                        struct passwd *pw = getpwuid(getuid());  // Get user name.
                                        new_folder->title = strdup(pw->pw_name);
                                    } else {
                                        new_folder->title = strdup("Folder");
                                    }
                                    free(home_slash);
                                }
                            } else {
                                if (base[strlen(base) - 1] == '/') base[strlen(base) - 1] = '\0';
                                new_folder->title = strdup(base);
                            }
                            free(path_copy);
                            new_folder->client_win = 0;
                            new_folder->client_visual = NULL;
                            XSetWindowAttributes attrs = {0};
                            attrs.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | StructureNotifyMask | KeyPressMask;
                            new_folder->win = create_canvas_window(ctx, root, new_folder->x, new_folder->y, new_folder->width, new_folder->height, &attrs);
                            XMapWindow(ctx->dpy, new_folder->win);
                            new_folder->backing = XCreatePixmap(ctx->dpy, new_folder->win, new_folder->width, new_folder->height, 32);
                            new_folder->back_pic = XRenderCreatePicture(ctx->dpy, new_folder->backing, ctx->fmt, 0, NULL);
                            new_folder->win_pic = XRenderCreatePicture(ctx->dpy, new_folder->win, ctx->fmt, 0, NULL);
                            new_folder->icons = NULL;
                            new_folder->num_icons = 0;
                            scan_icons(ctx, new_folder->path, &new_folder->icons, &new_folder->num_icons, new_folder);  // Scan folder icons.
                            redraw_canvas(ctx, new_folder, NULL);
                            activate_canvas(ctx, new_folder, windows, *num_windows);
                            //printf("Desktop size after opening drawer: %d x %d\n", desktop->width, desktop->height);
                        } // Add else if for TYPE_TOOL to execute later
                    } else {  // Single click update.
                        last_click_time = time;
                        last_click_x = mx;
                        last_click_y = my;
                        last_click_canvas = canvas;
                    }
                }
                potential_drag_icon = NULL;
                drag_canvas = NULL;
                is_dragging = false;
            }
        } else if (ev.type == MapRequest) {  // Client requests mapping (showing).
            Window child = ev.xmaprequest.window;  // Client window.
            // Check if already managed
            bool managed = false;
            for (int j = 0; j < *num_windows; j++) {
                if (windows[j].win == None) continue;
                if (windows[j].client_win == child) {
                    managed = true;
                    break;
                }
            }
            if (managed) continue;
            int slot = find_free_slot(windows, *num_windows, MAX_WINDOWS);
            if (slot == -1) continue;
            Canvas *new_canvas = &windows[slot];
            if (slot == *num_windows) (*num_windows)++;
            new_canvas->path = NULL;
            new_canvas->icons = NULL;
            new_canvas->num_icons = 0;
            new_canvas->bg_color = BG_COLOR_FOLDER;
            new_canvas->active = true;
            new_canvas->titlebar_height = TITLEBAR_HEIGHT;
            new_canvas->client_win = child;
            XWindowAttributes child_attrs;
            XGetWindowAttributes(ctx->dpy, child, &child_attrs);
            new_canvas->x = child_attrs.x;
            new_canvas->y = child_attrs.y + MENUBAR_HEIGHT;
            new_canvas->width = child_attrs.width + BORDER_WIDTH * 2;
            new_canvas->height = child_attrs.height + TITLEBAR_HEIGHT + BORDER_WIDTH;
            XSetWindowAttributes attrs = {0};
            attrs.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | StructureNotifyMask | KeyPressMask;
            new_canvas->win = create_canvas_window(ctx, root, new_canvas->x, new_canvas->y, new_canvas->width, new_canvas->height, &attrs);
            XRenderPictFormat *child_fmt = XRenderFindVisualFormat(ctx->dpy, child_attrs.visual);
            new_canvas->client_pic = XRenderCreatePicture(ctx->dpy, child, child_fmt, 0, NULL);
            new_canvas->client_visual = child_attrs.visual;
            XSelectInput(ctx->dpy, child, StructureNotifyMask | PropertyChangeMask);
            XAddToSaveSet(ctx->dpy, child);
            XReparentWindow(ctx->dpy, child, new_canvas->win, BORDER_WIDTH, TITLEBAR_HEIGHT);

            new_canvas->backing = XCreatePixmap(ctx->dpy, new_canvas->win, new_canvas->width, new_canvas->height, 32);
            new_canvas->back_pic = XRenderCreatePicture(ctx->dpy, new_canvas->backing, ctx->fmt, 0, NULL);
            new_canvas->win_pic = XRenderCreatePicture(ctx->dpy, new_canvas->win, ctx->fmt, 0, NULL);
            Atom net_wm_name = XInternAtom(ctx->dpy, "_NET_WM_NAME", False);
            Atom utf8 = XInternAtom(ctx->dpy, "UTF8_STRING", False);
            unsigned char *prop_data = NULL;
            int format;
            unsigned long type, bytes_after, nitems;
            if (XGetWindowProperty(ctx->dpy, child, net_wm_name, 0, 1024, False, utf8, &type, &format, &nitems, &bytes_after, &prop_data) == Success && prop_data) {
                new_canvas->title = strdup((char *)prop_data);
                XFree(prop_data);
            } else {
                XTextProperty tp;
                if (XGetWMName(ctx->dpy, child, &tp) && tp.value) {
                    new_canvas->title = strdup((char *)tp.value);
                    XFree(tp.value);
                } else {
                    new_canvas->title = strdup("Window");
                }
            }
            XMapWindow(ctx->dpy, child);  // Map client.
            XSync(ctx->dpy, False);
            XMapWindow(ctx->dpy, new_canvas->win);  // Map frame.

            // Set WM_STATE to NormalState on MapRequest
            Atom wm_state = XInternAtom(ctx->dpy, "WM_STATE", False);
            long data[2] = {1, None}; // 1 = NormalState
            XChangeProperty(ctx->dpy, child, wm_state, wm_state, 32, PropModeReplace, (unsigned char*)data, 2);

            redraw_canvas(ctx, new_canvas, NULL);
            activate_canvas(ctx, new_canvas, windows, *num_windows);
            //printf("Desktop size after mapping client: %d x %d\n", desktop->width, desktop->height);
        } else if (ev.type == ConfigureRequest) {  // Client requests size/pos change.
            XConfigureRequestEvent *cre = &ev.xconfigurerequest;
            Canvas *canvas = NULL;
            for (int j = 0; j < *num_windows; j++) {
                if (windows[j].win == None) continue;
                if (windows[j].client_win == cre->window) {
                    canvas = &windows[j];
                    break;
                }
            }
            if (!canvas) {  // Not managed, grant directly.
                XWindowChanges wc = {.x = cre->x, .y = cre->y, .width = cre->width, .height = cre->height, .border_width = cre->border_width, .sibling = cre->above, .stack_mode = cre->detail};
                XConfigureWindow(ctx->dpy, cre->window, cre->value_mask, &wc);
                continue;
            }
            // Handle for frame
            if (cre->value_mask & CWX) canvas->x = cre->x;
            if (cre->value_mask & CWY) canvas->y = cre->y + MENUBAR_HEIGHT;
            if (cre->value_mask & CWWidth) canvas->width = cre->width + BORDER_WIDTH * 2;
            if (cre->value_mask & CWHeight) canvas->height = cre->height + TITLEBAR_HEIGHT + BORDER_WIDTH;
            if (cre->value_mask & CWBorderWidth) { /* ignore */ }
            if (cre->value_mask & (CWSibling | CWStackMode)) { /* ignore for now */ }
            XConfigureWindow(ctx->dpy, canvas->win, cre->value_mask & (CWX | CWY | CWWidth | CWHeight), &(XWindowChanges){.x = canvas->x, .y = canvas->y, .width = canvas->width, .height = canvas->height});  // Configure frame.
            XConfigureWindow(ctx->dpy, cre->window, cre->value_mask & (CWWidth | CWHeight), &(XWindowChanges){.width = canvas->width - BORDER_WIDTH * 2, .height = canvas->height - TITLEBAR_HEIGHT - BORDER_WIDTH});  // Configure client.
            redraw_canvas(ctx, canvas, NULL);
        } else if (ev.type == DestroyNotify) {  // Window destroyed.
            Canvas *canvas = NULL;
            for (int j = 0; j < *num_windows; j++) {
                if (windows[j].win == None) continue;
                if (windows[j].client_win == ev.xdestroywindow.window) {
                    canvas = &windows[j];
                    break;
                }
            }
            if (canvas) {
                canvas->client_win = 0;
                canvas->client_pic = 0;
                close_canvas(ctx, canvas, windows, NULL);  // Close frame.
            }
        } else if (ev.type == UnmapNotify) {  // Client unmapped.
            Canvas *canvas = NULL;
            for (int j = 0; j < *num_windows; j++) {
                if (windows[j].win == None) continue;
                if (windows[j].client_win == ev.xunmap.window) {
                    canvas = &windows[j];
                    break;
                }
            }
            if (canvas) {
                canvas->client_pic = 0;
                redraw_canvas(ctx, canvas, NULL);  // Redraw without client.
            }
        } // Add Damage if added later
    }
}