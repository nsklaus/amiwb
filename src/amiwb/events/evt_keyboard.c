// File: evt_keyboard.c
// Keyboard event handling for AmiWB event system
// Routes keyboard shortcuts to appropriate subsystems and handles global key grabs

#include "evt_internal.h"
#include "../menus/menu_public.h"
#include "../workbench/wb_public.h"
#include "../dialogs/dialog_public.h"
#include "../amiwbrc.h"  // For get_config()
#include "../render.h"   // For redraw_canvas()
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/XF86keysym.h>
#include <stdlib.h>  // For system()

// ============================================================================
// X11 Key Grab Setup (Public API)
// ============================================================================

// Grab global shortcuts at X11 level so applications can't intercept them
void grab_global_shortcuts(Display *display, Window root) {
    // Only grab shortcuts that MUST work even when other apps have focus:

    // Super key combinations for window management - ALWAYS grabbed
    XGrabKey(display, XKeysymToKeycode(display, XK_q),
             Mod4Mask | ShiftMask, root, True, GrabModeAsync, GrabModeAsync);  // Super+Shift+Q (Quit)
    XGrabKey(display, XKeysymToKeycode(display, XK_r),
             Mod4Mask | ShiftMask, root, True, GrabModeAsync, GrabModeAsync);  // Super+Shift+R (Restart)
    XGrabKey(display, XKeysymToKeycode(display, XK_s),
             Mod4Mask | ShiftMask, root, True, GrabModeAsync, GrabModeAsync);  // Super+Shift+S (Suspend)
    XGrabKey(display, XKeysymToKeycode(display, XK_d),
             Mod4Mask | ShiftMask, root, True, GrabModeAsync, GrabModeAsync);  // Super+Shift+D (Debug/Metrics)

    // Workbench operations - ALWAYS grabbed
    XGrabKey(display, XKeysymToKeycode(display, XK_e),
             Mod4Mask, root, True, GrabModeAsync, GrabModeAsync);  // Super+E (Execute)
    XGrabKey(display, XKeysymToKeycode(display, XK_l),
             Mod4Mask, root, True, GrabModeAsync, GrabModeAsync);  // Super+L (Requester)

    // Window management - ALWAYS grabbed
    XGrabKey(display, XKeysymToKeycode(display, XK_q),
             Mod4Mask, root, True, GrabModeAsync, GrabModeAsync);  // Super+Q (Close)
    XGrabKey(display, XKeysymToKeycode(display, XK_m),
             Mod4Mask, root, True, GrabModeAsync, GrabModeAsync);  // Super+M (cycle next)
    XGrabKey(display, XKeysymToKeycode(display, XK_m),
             Mod4Mask | ShiftMask, root, True, GrabModeAsync, GrabModeAsync);  // Super+Shift+M (cycle prev)

    // View Modes - ALWAYS grabbed (but only active when no client has focus)
    // Grab both QWERTY numbers and AZERTY characters
    XGrabKey(display, XKeysymToKeycode(display, XK_1),
             Mod4Mask, root, True, GrabModeAsync, GrabModeAsync);  // Super+1 (QWERTY)
    XGrabKey(display, XKeysymToKeycode(display, 0x26),  // ampersand &
             Mod4Mask, root, True, GrabModeAsync, GrabModeAsync);  // Super+& (AZERTY position 1)
    XGrabKey(display, XKeysymToKeycode(display, XK_2),
             Mod4Mask, root, True, GrabModeAsync, GrabModeAsync);  // Super+2 (QWERTY)
    XGrabKey(display, XKeysymToKeycode(display, 0xe9),  // eacute é
             Mod4Mask, root, True, GrabModeAsync, GrabModeAsync);  // Super+é (AZERTY position 2)
    XGrabKey(display, XKeysymToKeycode(display, XK_3),
             Mod4Mask, root, True, GrabModeAsync, GrabModeAsync);  // Super+3 (QWERTY)
    XGrabKey(display, XKeysymToKeycode(display, 0x22),  // quotedbl "
             Mod4Mask, root, True, GrabModeAsync, GrabModeAsync);  // Super+" (AZERTY position 3)
    XGrabKey(display, XKeysymToKeycode(display, XK_4),
             Mod4Mask, root, True, GrabModeAsync, GrabModeAsync);  // Super+4 (QWERTY)
    XGrabKey(display, XKeysymToKeycode(display, 0x27),  // apostrophe '
             Mod4Mask, root, True, GrabModeAsync, GrabModeAsync);  // Super+' (AZERTY position 4)

    // Workbench operations - DO NOT GRAB
    // Super+R (Rename), Super+; (Cleanup), Super+P (Parent), Super+O (Open),
    // Super+C (Copy), Super+D (Delete), Super+N (New), Super+A (Select All)
    // These will only work when AmiWB or its windows have focus,
    // allowing client apps to use these shortcuts for their own purposes

    // Media keys - grab with AnyModifier so they work everywhere
    XGrabKey(display, XKeysymToKeycode(display, XF86XK_MonBrightnessUp),
             AnyModifier, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(display, XKeysymToKeycode(display, XF86XK_MonBrightnessDown),
             AnyModifier, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(display, XKeysymToKeycode(display, XF86XK_AudioRaiseVolume),
             AnyModifier, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(display, XKeysymToKeycode(display, XF86XK_AudioLowerVolume),
             AnyModifier, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(display, XKeysymToKeycode(display, XF86XK_AudioMute),
             AnyModifier, root, True, GrabModeAsync, GrabModeAsync);
}

// ============================================================================
// Keyboard Event Dispatcher (Public API)
// ============================================================================

// Dispatch key press to appropriate subsystem based on modifiers and focus
void handle_key_press(XKeyEvent *event) {
    /*
    // keypress debug removed
    KeySym keysym = XLookupKeysym(event, 0);
    if (keysym == XK_Escape) {
        running = false;
        return;
    }
    */

    // Check for global shortcuts (Super/Windows key + letter)
    KeySym keysym = XLookupKeysym(event, 0);

    // Handle media keys first - they should work regardless of other modifiers
    // Get config for media key commands
    const AmiwbConfig *cfg = get_config();

    if (keysym == XF86XK_MonBrightnessUp) {
        if (cfg->brightness_up_cmd[0]) {  // If command is configured
            system(cfg->brightness_up_cmd);
        }
        // No fallback - user must configure it
        return;
    }
    if (keysym == XF86XK_MonBrightnessDown) {
        if (cfg->brightness_down_cmd[0]) {  // If command is configured
            system(cfg->brightness_down_cmd);
        }
        // No fallback - user must configure it
        return;
    }
    if (keysym == XF86XK_AudioRaiseVolume) {
        if (cfg->volume_up_cmd[0]) {  // If command is configured
            system(cfg->volume_up_cmd);
        }
        // No fallback - user must configure it
        return;
    }
    if (keysym == XF86XK_AudioLowerVolume) {
        if (cfg->volume_down_cmd[0]) {  // If command is configured
            system(cfg->volume_down_cmd);
        }
        // No fallback - user must configure it
        return;
    }
    if (keysym == XF86XK_AudioMute) {
        if (cfg->volume_mute_cmd[0]) {  // If command is configured
            system(cfg->volume_mute_cmd);
        }
        // No fallback - user must configure it
        return;
    }

    if (event->state & Mod4Mask) {  // Super/Windows key is pressed
        if (event->state & ShiftMask) {
            // Super+Shift combinations
            // Super+Shift+Q: Quit AmiWB
            if (keysym == XK_q || keysym == XK_Q) {
                handle_quit_request();
                return;
            }
            // Super+Shift+R: Restart AmiWB
            if (keysym == XK_r || keysym == XK_R) {
                handle_restart_request();
                return;
            }
            // Super+Shift+S: Suspend
            if (keysym == XK_s || keysym == XK_S) {
                handle_suspend_request();
                return;
            }
            // Super+Shift+M: Cycle to previous window
            if (keysym == XK_m || keysym == XK_M) {
                itn_focus_cycle_prev();
                return;
            }
            // Super+Shift+D: Performance metrics debug
            if (keysym == XK_d || keysym == XK_D) {
                log_error("[METRICS] Performance snapshot requested");
                itn_render_log_metrics();
                return;
            }
        } else {
            // Super-only combinations (no Shift)
            // Super+E: Execute command
            if (keysym == XK_e || keysym == XK_E) {
                trigger_execute_action();
                return;
            }
            // Super+L: Launch requester (reqasl)
            if (keysym == XK_l || keysym == XK_L) {
                trigger_requester_action();
                return;
            }
            // Super+R: Rename selected icon (only if no client window has focus)
            if (keysym == XK_r || keysym == XK_R) {
                Canvas *active = itn_focus_get_active();
                if (!active || active->client_win == None) {
                    trigger_rename_action();
                    return;
                }
                // Let client window handle it
            }
            // Super+I: Icon Information (only if no client window has focus)
            if (keysym == XK_i || keysym == XK_I) {
                Canvas *active = itn_focus_get_active();
                if (!active || active->client_win == None) {
                    trigger_icon_info_action();
                    return;
                }
                // Let client window handle it
            }
            // Super+;: Clean up icons (only if no client window has focus)
            if (keysym == XK_semicolon) {
                Canvas *active = itn_focus_get_active();
                if (!active || active->client_win == None) {
                    trigger_cleanup_action();
                    return;
                }
                // Let client window handle it
            }
            // Super+H: Refresh active window or desktop
            if (keysym == XK_h || keysym == XK_H) {
                trigger_refresh_action();
                return;
            }
            // Super+Q: Close active window
            if (keysym == XK_q || keysym == XK_Q) {
                trigger_close_action();
                return;
            }
            // Super+P: Open parent directory (only if no client window has focus)
            if (keysym == XK_p || keysym == XK_P) {
                Canvas *active = itn_focus_get_active();
                if (!active || active->client_win == None) {
                    trigger_parent_action();
                    return;
                }
                // Let client window handle it
            }
            // Super+O: Open selected icon (only if no client window has focus)
            if (keysym == XK_o || keysym == XK_O) {
                Canvas *active = itn_focus_get_active();
                if (!active || active->client_win == None) {
                    trigger_open_action();
                    return;
                }
                // Let client window handle it
            }
            // Super+C: Copy selected icon (only if no client window has focus)
            if (keysym == XK_c || keysym == XK_C) {
                Canvas *active = itn_focus_get_active();
                if (!active || active->client_win == None) {
                    // Only trigger icon copy if it's a Workbench window
                    trigger_copy_action();
                    return;
                }
                // Let client window handle it
            }
            // Super+D: Delete selected icon (only if no client window has focus)
            if (keysym == XK_d || keysym == XK_D) {
                Canvas *active = itn_focus_get_active();
                if (!active || active->client_win == None) {
                    trigger_delete_action();
                    return;
                }
                // Let client window handle it
            }
            // Super+N: New Drawer (only if no client window has focus)
            if (keysym == XK_n || keysym == XK_N) {
                Canvas *active = itn_focus_get_active();
                if (!active || active->client_win == None) {
                    trigger_new_drawer_action();
                    return;
                }
                // Let client window handle it (New file in editors)
            }
            // Super+A: Select Contents (only if no client window has focus)
            if (keysym == XK_a || keysym == XK_A) {
                Canvas *active = itn_focus_get_active();
                if (!active || active->client_win == None) {
                    trigger_select_contents_action();
                    return;
                }
                // Let client window handle it (Select All in text editors)
            }
            // Super+M: Cycle to next window
            if (keysym == XK_m || keysym == XK_M) {
                itn_focus_cycle_next();
                return;
            }
        }

        // View Modes shortcuts - work with or without Shift (for AZERTY support)
        // Super+1: Icons view mode (only if no client window has focus)
        // QWERTY: XK_1 (0x31), AZERTY: ampersand (0x26 = &)
        if (keysym == XK_1 || keysym == 0x26) {
            Canvas *active = itn_focus_get_active();
            if (!active || active->client_win == None) {
                Canvas *target = active ? active : itn_canvas_get_desktop();
                if (target) {
                    set_canvas_view_mode(target, VIEW_ICONS);
                    update_view_modes_checkmarks();
                }
                return;
            }
        }
        // Super+2: Names view mode (only if no client window has focus)
        // QWERTY: XK_2 (0x32), AZERTY: eacute (0xe9 = é)
        if (keysym == XK_2 || keysym == 0xe9) {
            Canvas *active = itn_focus_get_active();
            if (!active || active->client_win == None) {
                Canvas *target = active ? active : itn_canvas_get_desktop();
                if (target && target->type != DESKTOP) {  // Names mode not available for desktop
                    set_canvas_view_mode(target, VIEW_NAMES);
                    update_view_modes_checkmarks();
                }
                return;
            }
        }
        // Super+3: Toggle hidden files (only if no client window has focus)
        // QWERTY: XK_3 (0x33), AZERTY: quotedbl (0x22 = ")
        if (keysym == XK_3 || keysym == 0x22) {
            Canvas *active = itn_focus_get_active();
            if (!active || active->client_win == None) {
                Canvas *target = active ? active : itn_canvas_get_desktop();
                if (target) {
                    // Toggle global hidden files state
                    bool new_state = !get_global_show_hidden_state();
                    set_global_show_hidden_state(new_state);
                    target->show_hidden = new_state;

                    // Refresh directory view
                    if (target->path) {
                        refresh_canvas_from_directory(target, target->path);
                    } else if (target->type == DESKTOP) {
                        const char *home = getenv("HOME");
                        if (home) {
                            char desktop_path[PATH_SIZE];
                            snprintf(desktop_path, sizeof(desktop_path), "%s/Desktop", home);
                            refresh_canvas_from_directory(target, desktop_path);
                        }
                    }
                    if (target->type == WINDOW) {
                        wb_layout_apply_view(target);
                        compute_max_scroll(target);
                    }
                    redraw_canvas(target);
                    update_view_modes_checkmarks();
                }
                return;
            }
        }
        // Super+4: Toggle spatial mode (only if no client window has focus)
        // QWERTY: XK_4 (0x34), AZERTY: apostrophe (0x27 = ')
        if (keysym == XK_4 || keysym == 0x27) {
            Canvas *active = itn_focus_get_active();
            if (!active || active->client_win == None) {
                set_spatial_mode(!get_spatial_mode());
                update_view_modes_checkmarks();
                return;
            }
        }

        // Add more global shortcuts here in the future
    }

    // Check if active window is a dialog and route keyboard events to it
    Canvas *active = itn_focus_get_active();
    if (active && active->type == DIALOG) {
        bool dialog_consumed = false;

        // Check if it's an iconinfo dialog
        if (is_iconinfo_canvas(active)) {
            dialog_consumed = iconinfo_handle_key_press(event);
        } else {
            // Try other dialog types
            dialog_consumed = dialogs_handle_key_press(event);
        }

        if (dialog_consumed) {
            return;
        }
    }

    menu_handle_key_press(event);
}
