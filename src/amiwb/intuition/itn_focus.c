// Focus management, active window
// This module handles window activation and focus cycling

#include "../config.h"
#include "itn_internal.h"
#include <X11/Xlib.h>
#include "../menus/menu_public.h"  // For check_for_app_menus
#include "../workbench/wb_internal.h"
#include "../render/rnd_public.h"  // For redraw_canvas (temporary)

// Migration notes:
// find_canvas is now itn_canvas_find_by_window
// get_desktop_canvas is now itn_canvas_get_desktop

// Module-private state
static Canvas *g_active_canvas = NULL;  // Currently active/focused window

void itn_focus_set_active(Canvas *canvas) {
    if (!canvas || (canvas->type != WINDOW && canvas->type != DIALOG)) return;

    Display *dpy = itn_core_get_display();
    if (!dpy) return;

    // CRITICAL: Validate window FIRST, before changing any state!
    // If window is invalid, do nothing - prevents "all windows gray" bug
    // where we deactivate old window but fail to activate new one
    if (!is_window_valid(dpy, canvas->win)) {
        return;
    }

    // Save old active window reference before changing state
    Canvas *old_active = g_active_canvas;

    // CRITICAL: Set new active BEFORE any redraw operations!
    // This ensures itn_focus_get_active() returns the correct value during rendering,
    // so the old window draws with inactive (gray) decorations
    g_active_canvas = canvas;

    // Deactivate old active window (if it exists and is different from new active)
    if (old_active && old_active != canvas &&
        (old_active->type == WINDOW || old_active->type == DIALOG)) {
        // Must redraw decorations to update border color to inactive (gray)
        redraw_canvas(old_active);  // Now itn_focus_get_active() returns 'canvas', not old_active
        DAMAGE_CANVAS(old_active);  // Mark for compositor update
    }

    // Raise window and accumulate damage
    XRaiseWindow(dpy, canvas->win);
    itn_stack_mark_dirty();  // CRITICAL: XRaiseWindow doesn't generate ConfigureNotify!

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
    // Save old active window reference before changing state
    Canvas *old_active = g_active_canvas;

    // CRITICAL: Set to NULL BEFORE any redraw operations!
    // This ensures itn_focus_get_active() returns NULL during rendering,
    // so the window draws with inactive (gray) decorations
    g_active_canvas = NULL;

    // Deactivate the old active window (if it exists)
    if (old_active && (old_active->type == WINDOW || old_active->type == DIALOG)) {
        // Must redraw decorations to show inactive color
        redraw_canvas(old_active);  // Now itn_focus_get_active() returns NULL
        DAMAGE_CANVAS(old_active);
    }

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
            // Apply same filter as get_window_list():
            // Include user-iconified (can be restored) OR visible non-hidden windows
            // Exclude app-hidden windows (e.g., Sublime phantom tabs)
            if (c->user_iconified || (c->comp_mapped && !c->app_hidden)) {
                if (c == g_active_canvas) {
                    current_index = window_count;
                }
                windows[window_count++] = c;
                if (window_count >= MAX_WINDOWS) break;
            }
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
    itn_stack_mark_dirty();  // CRITICAL: XRaiseWindow doesn't generate ConfigureNotify!
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
            // Apply same filter as get_window_list():
            // Include user-iconified (can be restored) OR visible non-hidden windows
            // Exclude app-hidden windows (e.g., Sublime phantom tabs)
            if (c->user_iconified || (c->comp_mapped && !c->app_hidden)) {
                if (c == g_active_canvas) {
                    current_index = window_count;
                }
                windows[window_count++] = c;
                if (window_count >= MAX_WINDOWS) break;
            }
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
    itn_stack_mark_dirty();  // CRITICAL: XRaiseWindow doesn't generate ConfigureNotify!
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
                    itn_stack_mark_dirty();  // CRITICAL: XRaiseWindow doesn't generate ConfigureNotify!
                }
            }
            return;
        }
        window_index++;
    }
}