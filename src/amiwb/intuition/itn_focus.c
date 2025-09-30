// Focus management, active window
// This module handles window activation and focus cycling

#include "../config.h"
#include "itn_internal.h"
#include <X11/Xlib.h>
#include "../menus.h"  // For check_for_app_menus
#include "../workbench.h"  // For icon functions
#include "../render.h"  // For redraw_canvas (temporary)

// External references (temporary during migration)
extern Canvas **canvas_array;
extern int canvas_count;
extern void redraw_canvas(Canvas *c);  // From render.c
extern void check_for_app_menus(Window w);  // From menus.c
extern void restore_system_menu(void);  // From menus.c
extern FileIcon **get_icon_array(void);  // From workbench.c
extern int get_icon_count(void);  // From workbench.c
extern void restore_iconified(FileIcon *icon);  // From workbench.c
// find_canvas is now itn_canvas_find_by_window
// get_desktop_canvas is now itn_canvas_get_desktop

void itn_focus_set_active(Canvas *canvas) {
    if (!canvas || (canvas->type != WINDOW && canvas->type != DIALOG)) return;

    Display *dpy = itn_core_get_display();
    if (!dpy) return;

    // Deactivate all other windows
    for (int i = 0; i < canvas_count; i++) {
        Canvas *o = canvas_array[i];
        if ((o->type == WINDOW || o->type == DIALOG) && o != canvas) {
            if (o->active) {
                o->active = false;
                // Must redraw decorations to update border color
                extern void redraw_canvas(Canvas *c);
                redraw_canvas(o);  // This updates the window pixmap with new decoration colors
                DAMAGE_CANVAS(o);  // Then mark for compositor update
            }
        }
    }

    // Set new active
    g_active_canvas = canvas;
    canvas->active = true;

    // Raise window and accumulate damage
    XRaiseWindow(dpy, canvas->win);

    // Set X11 focus (safe_set_input_focus handles validation and BadMatch errors)
    Window focus = (canvas->client_win != None) ? canvas->client_win : canvas->win;
    safe_set_input_focus(dpy, focus, RevertToParent, CurrentTime);

    // Redraw decorations with active color
    extern void redraw_canvas(Canvas *c);
    redraw_canvas(canvas);  // This updates the window pixmap with new decoration colors

    // Damage new active window
    DAMAGE_CANVAS(canvas);

    // Check for app menus on client windows
    if (canvas->client_win != None) {
        check_for_app_menus(canvas->client_win);
    } else {
        // Workbench window - restore system menus
        restore_system_menu();
    }

    XSync(dpy, False);

    // Schedule frame to render all damage
    SCHEDULE_FRAME();
}

Canvas *itn_focus_get_active(void) {
    return g_active_canvas;
}

void itn_focus_deactivate_all(void) {
    extern void redraw_canvas(Canvas *c);

    for (int i = 0; i < canvas_count; i++) {
        Canvas *c = canvas_array[i];
        if (c && c->active) {
            c->active = false;
            // Must redraw decorations to show inactive color
            redraw_canvas(c);  // Updates the window pixmap with gray decoration
            DAMAGE_CANVAS(c);
        }
    }
    g_active_canvas = NULL;
    SCHEDULE_FRAME();
}

void itn_focus_cycle_next(void) {
    // Build list of eligible windows (WINDOW and DIALOG types)
    Canvas *windows[256];  // Max 256 windows
    int window_count = 0;
    int current_index = -1;

    for (int i = 0; i < canvas_count; i++) {
        Canvas *c = canvas_array[i];
        if (c && (c->type == WINDOW || c->type == DIALOG)) {
            if (c == g_active_canvas) {
                current_index = window_count;
            }
            windows[window_count++] = c;
            if (window_count >= 256) break;
        }
    }

    // Need at least 2 windows to cycle
    if (window_count < 2) return;

    Display *dpy = itn_core_get_display();
    if (!dpy) return;

    // Calculate next index (wrap around)
    int next_index = (current_index + 1) % window_count;
    Canvas *next_window = windows[next_index];

    // Check if window is iconified (not visible)
    XWindowAttributes attrs;
    if (safe_get_window_attributes(dpy, next_window->win, &attrs)) {
        if (attrs.map_state != IsViewable) {
            // Window is iconified - find and restore it
            FileIcon **icon_array = get_icon_array();
            int icon_count = get_icon_count();
            for (int i = 0; i < icon_count; i++) {
                FileIcon *ic = icon_array[i];
                if (ic && ic->type == TYPE_ICONIFIED && ic->iconified_canvas == next_window) {
                    restore_iconified(ic);
                    return;
                }
            }
        }
    }

    // Window is visible - activate it
    itn_focus_set_active(next_window);
    XRaiseWindow(dpy, next_window->win);
    SCHEDULE_FRAME();
}

void itn_focus_cycle_prev(void) {
    // Build list of eligible windows (WINDOW and DIALOG types)
    Canvas *windows[256];  // Max 256 windows
    int window_count = 0;
    int current_index = -1;

    for (int i = 0; i < canvas_count; i++) {
        Canvas *c = canvas_array[i];
        if (c && (c->type == WINDOW || c->type == DIALOG)) {
            if (c == g_active_canvas) {
                current_index = window_count;
            }
            windows[window_count++] = c;
            if (window_count >= 256) break;
        }
    }

    // Need at least 2 windows to cycle
    if (window_count < 2) return;

    Display *dpy = itn_core_get_display();
    if (!dpy) return;

    // Calculate previous index (wrap around)
    int prev_index = (current_index - 1 + window_count) % window_count;
    Canvas *prev_window = windows[prev_index];

    // Check if window is iconified (not visible)
    XWindowAttributes attrs;
    if (safe_get_window_attributes(dpy, prev_window->win, &attrs)) {
        if (attrs.map_state != IsViewable) {
            // Window is iconified - find and restore it
            FileIcon **icon_array = get_icon_array();
            int icon_count = get_icon_count();
            for (int i = 0; i < icon_count; i++) {
                FileIcon *ic = icon_array[i];
                if (ic && ic->type == TYPE_ICONIFIED && ic->iconified_canvas == prev_window) {
                    restore_iconified(ic);
                    return;
                }
            }
        }
    }

    // Window is visible - activate it
    itn_focus_set_active(prev_window);
    XRaiseWindow(dpy, prev_window->win);
    SCHEDULE_FRAME();
}

// Find next is now handled directly in cycle functions using array

void itn_focus_activate_window_behind(Canvas *current) {
    if (!current) return;

    Display *dpy = itn_core_get_display();
    if (!dpy) return;

    Window root_ret, parent_ret, *children = NULL;
    unsigned int n = 0;
    if (!XQueryTree(dpy, DefaultRootWindow(dpy), &root_ret, &parent_ret, &children, &n)) return;

    int idx = -1;
    for (unsigned int i = 0; i < n; i++) {
        if (children[i] == current->win) { idx = i; break; }
    }

    if (idx >= 0) {
        for (int i = idx - 1; i >= 0; i--) {
            Canvas *c = itn_canvas_find_by_window(children[i]);
            if (c && (c->type == WINDOW || c->type == DIALOG) && c != current) {
                itn_focus_set_active(c);
                if (children) XFree(children);
                return;
            }
        }
    }

    for (unsigned int i = 0; i < n && (int)i < idx; i++) {
        Canvas *c = itn_canvas_find_by_window(children[i]);
        if (c && (c->type == WINDOW || c->type == DIALOG) && c != current) {
            itn_focus_set_active(c);
            break;
        }
    }

    if (children) XFree(children);
}

void itn_focus_select_next(Canvas *closing_canvas) {
    if (g_active_canvas == closing_canvas) g_active_canvas = NULL;

    Display *dpy = itn_core_get_display();
    if (!dpy) return;

    Window root_return, parent_return;
    Window *children;
    unsigned int nchildren;
    if (XQueryTree(dpy, DefaultRootWindow(dpy), &root_return, &parent_return,
                   &children, &nchildren)) {
        // Search for the topmost visible window that is not the closing canvas
        for (int i = nchildren - 1; i >= 0; i--) {
            Canvas *next_canvas = itn_canvas_find_by_window(children[i]);

            if (next_canvas && next_canvas != closing_canvas &&
                (next_canvas->type == WINDOW || next_canvas->type == DIALOG)) {

                // Set the new active window
                itn_focus_set_active(next_canvas);

                XFree(children);
                return;
            }
        }
        XFree(children);
    }

    // If no window found, set active to desktop
    if (!g_active_canvas) {
        g_active_canvas = itn_canvas_get_desktop();
    }
}

void itn_focus_activate_by_index(int index) {
    if (index < 0 || index >= canvas_count) return;

    // Count WINDOW/DIALOG type canvases
    int window_index = 0;
    for (int i = 0; i < canvas_count; i++) {
        Canvas *c = canvas_array[i];
        if (!c || (c->type != WINDOW && c->type != DIALOG)) continue;

        if (window_index == index) {
            itn_focus_set_active(c);

            Display *dpy = itn_core_get_display();
            if (!dpy) return;

            // Check if iconified
            XWindowAttributes attrs;
            if (safe_get_window_attributes(dpy, c->win, &attrs)) {
                if (attrs.map_state != IsViewable) {
                    // Find and restore iconified window
                    FileIcon **icon_array = get_icon_array();
                    int icon_count = get_icon_count();
                    for (int j = 0; j < icon_count; j++) {
                        FileIcon *ic = icon_array[j];
                        if (ic && ic->type == TYPE_ICONIFIED && ic->iconified_canvas == c) {
                            restore_iconified(ic);
                            break;
                        }
                    }
                } else {
                    XRaiseWindow(dpy, c->win);
                }
            }
            return;
        }
        window_index++;
    }
}