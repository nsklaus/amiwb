// Focus management, active window
// This module handles window activation and focus cycling

#include "../config.h"
#include "itn_internal.h"
#include "../render_public.h"
#include <X11/Xlib.h>
#include "../menus/menu_public.h"  // For check_for_app_menus
#include "../workbench/wb_internal.h"
#include "../render.h"  // For redraw_canvas (temporary)

// Migration notes:
// find_canvas is now itn_canvas_find_by_window
// get_desktop_canvas is now itn_canvas_get_desktop

// Module-private state
static Canvas *g_active_canvas = NULL;  // Currently active/focused window

void itn_focus_set_active(Canvas *canvas) {
    if (!canvas || (canvas->type != WINDOW && canvas->type != DIALOG)) return;

    Display *dpy = itn_core_get_display();
    if (!dpy) return;

    // Deactivate all other windows
    int count = itn_manager_get_count();
    for (int i = 0; i < count; i++) {
        Canvas *o = itn_manager_get_canvas(i);
        if (!o) continue;

        if ((o->type == WINDOW || o->type == DIALOG) && o != canvas) {
            if (o->active) {
                o->active = false;
                // Must redraw decorations to update border color
                redraw_canvas(o);  // This updates the window pixmap with new decoration colors
                DAMAGE_CANVAS(o);  // Then mark for compositor update
            }
        }
    }

    // Set new active
    g_active_canvas = canvas;
    canvas->active = true;

    // Validate canvas window before attempting focus operations
    // Protects against asynchronous window destruction or corruption
    if (!is_window_valid(dpy, canvas->win)) {
        return;
    }

    // Raise window and accumulate damage
    XRaiseWindow(dpy, canvas->win);

    // Set X11 focus (safe_set_input_focus handles validation and BadMatch errors)
    Window focus = (canvas->client_win != None) ? canvas->client_win : canvas->win;
    safe_set_input_focus(dpy, focus, RevertToParent, CurrentTime);

    // Redraw decorations with active color
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
    int count = itn_manager_get_count();
    for (int i = 0; i < count; i++) {
        Canvas *c = itn_manager_get_canvas(i);
        if (c && c->active) {
            c->active = false;
            // Must redraw decorations to show inactive color
            redraw_canvas(c);  // Updates the window pixmap with gray decoration
            DAMAGE_CANVAS(c);
        }
    }
    g_active_canvas = NULL;

    // Restore system menus when no window is active (desktop focused)
    restore_system_menu();

    SCHEDULE_FRAME();
}

void itn_focus_cycle_next(void) {
    // Build list of eligible windows (WINDOW and DIALOG types)
    Canvas *windows[MAX_WINDOWS];
    int window_count = 0;
    int current_index = -1;

    int count = itn_manager_get_count();
    for (int i = 0; i < count; i++) {
        Canvas *c = itn_manager_get_canvas(i);
        if (c && (c->type == WINDOW || c->type == DIALOG)) {
            if (c == g_active_canvas) {
                current_index = window_count;
            }
            windows[window_count++] = c;
            if (window_count >= MAX_WINDOWS) break;
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
            FileIcon **icon_array = wb_icons_array_get();
            int icon_count = wb_icons_array_count();
            for (int i = 0; i < icon_count; i++) {
                FileIcon *ic = icon_array[i];
                if (ic && ic->type == TYPE_ICONIFIED && ic->iconified_canvas == next_window) {
                    wb_icons_restore_iconified(ic);
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
    Canvas *windows[MAX_WINDOWS];
    int window_count = 0;
    int current_index = -1;

    int count = itn_manager_get_count();
    for (int i = 0; i < count; i++) {
        Canvas *c = itn_manager_get_canvas(i);
        if (c && (c->type == WINDOW || c->type == DIALOG)) {
            if (c == g_active_canvas) {
                current_index = window_count;
            }
            windows[window_count++] = c;
            if (window_count >= MAX_WINDOWS) break;
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
            FileIcon **icon_array = wb_icons_array_get();
            int icon_count = wb_icons_array_count();
            for (int i = 0; i < icon_count; i++) {
                FileIcon *ic = icon_array[i];
                if (ic && ic->type == TYPE_ICONIFIED && ic->iconified_canvas == prev_window) {
                    wb_icons_restore_iconified(ic);
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
    if (index < 0 || index >= itn_manager_get_count()) return;

    // Count WINDOW/DIALOG type canvases
    int window_index = 0;
    int count = itn_manager_get_count();
    for (int i = 0; i < count; i++) {
        Canvas *c = itn_manager_get_canvas(i);
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
                    FileIcon **icon_array = wb_icons_array_get();
                    int icon_count = wb_icons_array_count();
                    for (int j = 0; j < icon_count; j++) {
                        FileIcon *ic = icon_array[j];
                        if (ic && ic->type == TYPE_ICONIFIED && ic->iconified_canvas == c) {
                            wb_icons_restore_iconified(ic);
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