// File: wb_icons_ops.c
// Icon Operations - find, move, metadata, restore operations

#define _POSIX_C_SOURCE 200809L
#include "wb_internal.h"
#include "../config.h"
#include "../render/rnd_public.h"
#include "../events/evt_public.h"
#include "../intuition/itn_public.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <X11/Xlib.h>


// ============================================================================
// Icon Movement
// ============================================================================

void move_icon(FileIcon *icon, int x, int y) {
    if (!icon) return;
    icon->x = max(0, x);
    icon->y = max(0, y);
}

// ============================================================================
// Icon Finding
// ============================================================================

FileIcon *find_icon(Window win, int x, int y) {
    FileIcon **icon_array = wb_icons_array_get();
    int icon_count = wb_icons_array_count();
    
    if (!icon_array || icon_count <= 0) return NULL;
    
    Canvas *c = itn_canvas_find_by_window(win);
    int base_x = 0, base_y = 0, sx = 0, sy = 0;
    if (c) {
        if (c->type == WINDOW) {
            base_x = BORDER_WIDTH_LEFT;
            base_y = BORDER_HEIGHT_TOP;
        }
        sx = c->scroll_x;
        sy = c->scroll_y;
    }
    
    // Iterate from top to bottom (reverse order)
    for (int i = icon_count - 1; i >= 0; i--) {
        FileIcon *ic = icon_array[i];
        if (ic->display_window != win) continue;
        
        int rx = base_x + ic->x - sx;
        int ry = base_y + ic->y - sy;
        
        if (c && c->type == WINDOW && c->view_mode == VIEW_NAMES) {
            // List view - only label text is selectable
            int row_h = 18 + 6;
            int text_left_pad = 6;
            int text_x = base_x + ic->x + text_left_pad;
            int text_w = get_text_width(ic->label ? ic->label : "");
            if (x >= text_x && x <= text_x + text_w && y >= ry && y <= ry + row_h) {
                return ic;
            }
        } else {
            // Icon view - icon + label area
            int w = ic->width;
            int h = ic->height;
            int label_pad = 20;
            if (x >= rx && x <= rx + w && y >= ry && y <= ry + h + label_pad) {
                return ic;
            }
        }
    }
    return NULL;
}

// ============================================================================
// Icon Metadata
// ============================================================================

void set_icon_meta(FileIcon *icon, int x, int y) {
    if (!icon) return;
    icon->x = x;
    icon->y = y;
}

// ============================================================================
// Iconified Window Restoration
// ============================================================================

void wb_icons_restore_iconified(FileIcon *icon) {
    if (!icon || icon->type != TYPE_ICONIFIED) {
        return;
    }
    
    Canvas *canvas = icon->iconified_canvas;
    if (!canvas) {
        return;
    }
    
    // Remap and raise the original window frame
    Display *dpy = itn_core_get_display();
    XMapRaised(dpy, canvas->win);
    XSync(dpy, False);
    
    // Get fresh composite pixmap after mapping
    itn_composite_update_canvas_pixmap(canvas);
    
    // Prevent trailing click from deactivating
    suppress_desktop_deactivate_for_ms(200);
    
    // Wait until frame is viewable
    for (int i = 0; i < 50; ++i) {
        XWindowAttributes wa;
        if (safe_get_window_attributes(dpy, canvas->win, &wa) && wa.map_state == IsViewable) {
            break;
        }
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 };
        nanosleep(&ts, NULL);
    }
    
    itn_focus_set_active(canvas);
    redraw_canvas(canvas);
    
    // Clear press target to prevent crash
    clear_press_target_if_matches(icon->display_window);
    
    // Remove the iconified icon
    destroy_icon(icon);
    
    // Refresh desktop
    Canvas *desktop = itn_canvas_get_desktop();
    if (desktop) {
        refresh_canvas(desktop);
    }
    
    // Re-assert stacking
    XRaiseWindow(dpy, canvas->win);
    XSync(dpy, False);
}

// ============================================================================
// Launch Helper
// ============================================================================

void launch_with_hook(const char *command) {
    if (!command || !*command) return;
    
    pid_t pid = fork();
    if (pid == -1) {
        log_error("[ERROR] fork failed for command: %s", command);
        return;
    } else if (pid == 0) {
        // Child process
        for (int i = 3; i < 256; i++) {
            close(i);
        }
        
        // Inject ReqASL hook
        setenv("LD_PRELOAD", REQASL_HOOK_PATH, 1);
        
        // Execute through shell
        execl("/bin/sh", "sh", "-c", command, NULL);
        _exit(EXIT_FAILURE);
    }
    // Parent continues
}
