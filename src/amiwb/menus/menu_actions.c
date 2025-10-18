// Menu System - Action Handlers Module
// All menu action implementations and main selection dispatcher

#include "menu_internal.h"
#include "menu_public.h"
#include "../intuition/itn_public.h"
#include "../workbench/wb_public.h"
#include "../workbench/wb_internal.h"
#include "../dialogs/dialog_public.h"
#include "../dialogs/dialog_public.h"
#include "../events/evt_public.h"
#include "../config.h"
#include "../diskdrives.h"
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

// ============================================================================
// Global State for Actions
// ============================================================================

// Rename state
static FileIcon *g_rename_icon = NULL;

// Delete operation state  
static FileIcon *g_pending_delete_icons[256];
static int g_pending_delete_count = 0;
static Canvas *g_pending_delete_canvas = NULL;

// ============================================================================
// Forward Declarations
// ============================================================================

static void rename_file_ok_callback(const char *new_name);
static void rename_file_cancel_callback(void);
static void execute_command_ok_callback(const char *command);
static void execute_command_cancel_callback(void);
static void execute_pending_deletes(void);
static void cancel_pending_deletes(void);
static void open_file_or_directory(FileIcon *icon);


// ============================================================================
// Rename File Callbacks
// ============================================================================

static void rename_file_ok_callback(const char *new_name) {
    // Use the global icon that was set when dialog was shown
    FileIcon *icon = g_rename_icon;
    
    
    if (!icon || !new_name || strlen(new_name) == 0) {
        return;
    }
    
    // Additional validation - check if icon is still valid
    bool icon_valid = false;
    for (int i = 0; i < wb_icons_array_count(); i++) {
        if (wb_icons_array_get()[i] == icon) {
            icon_valid = true;
            break;
        }
    }
    if (!icon_valid) {
        log_error("[ERROR] Rename failed: icon no longer valid");
        return;
    }
    
    // Construct paths
    char old_path[PATH_SIZE];
    char new_path[PATH_SIZE]; 
    char *dir_path = strdup(icon->path);
    char *filename = strrchr(dir_path, '/');
    if (filename) *filename = '\0';  // Remove filename, keep directory
    
    snprintf(old_path, sizeof(old_path), "%s", icon->path);
    snprintf(new_path, sizeof(new_path), "%s/%s", dir_path, new_name);
    
    // Attempt rename with safety checks
    if (access(new_path, F_OK) == 0) {
        log_error("[ERROR] Rename failed: file '%s' already exists", new_name);
    } else if (rename(old_path, new_path) == 0) {
        // Success: update icon
        free(icon->label);
        icon->label = strdup(new_name);
        free(icon->path);
        icon->path = strdup(new_path);
        
        // Check for sidecar .info file and rename it too
        char old_info_path[PATH_SIZE + 10];  // +10 for ".info" suffix
        char new_info_path[PATH_SIZE + 10];
        snprintf(old_info_path, sizeof(old_info_path), "%s.info", old_path);
        snprintf(new_info_path, sizeof(new_info_path), "%s.info", new_path);
        
        // If sidecar .info exists, rename it too
        if (access(old_info_path, F_OK) == 0) {
            if (rename(old_info_path, new_info_path) != 0) {
                // Log warning but don't fail the whole operation
                log_error("[WARNING] Could not rename sidecar .info file: %s", strerror(errno));
            }
        }
        
        // Recalculate label width after rename
        XftFont *font = get_font();
        if (icon->label && font) {
            RenderContext *ctx = get_render_context();
            if (ctx) {
                XGlyphInfo extents;
                XftTextExtentsUtf8(ctx->dpy, font, (FcChar8 *)icon->label, strlen(icon->label), &extents);
                icon->label_width = extents.xOff;
            }
        }
        
        // Refresh display WITHOUT full directory reload - just update this icon
        Canvas *canvas = itn_canvas_find_by_window(icon->display_window);
        if (canvas && canvas->path) {
            // Just redraw the canvas with the updated icon label
            redraw_canvas(canvas);

            // Let compositor handle updates through events
            XSync(itn_core_get_display(), False);
        }
        
    } else {
    }
    
    free(dir_path);
    g_rename_icon = NULL;  // Clear the global icon reference
}

static void rename_file_cancel_callback(void) {
    // Rename cancelled
    g_rename_icon = NULL;  // Clear the global icon reference
}

// ============================================================================
// Execute Command Callbacks
// ============================================================================

static void execute_command_ok_callback(const char *command);
static void execute_command_cancel_callback(void);

// Execute command dialog callback - run the command
static void execute_command_ok_callback(const char *command) {
    launch_with_hook(command);
}

static void execute_command_cancel_callback(void) {
    // Nothing to do - dialog will be closed automatically
}

// ============================================================================
// Delete Operation Callbacks
// ============================================================================

static void execute_pending_deletes(void) {
    if (!g_pending_delete_canvas || g_pending_delete_count == 0) {
        log_error("[ERROR] No pending deletes or canvas lost!");
        return;
    }
    
    int delete_count = 0;
    bool need_layout_update = false;
    
    for (int i = 0; i < g_pending_delete_count; i++) {
        FileIcon *selected = g_pending_delete_icons[i];
        
        // CRITICAL: Verify icon still exists and belongs to same window
        bool icon_still_valid = false;
        FileIcon **icon_array = wb_icons_array_get();
        int icon_count = wb_icons_array_count();
        for (int j = 0; j < icon_count; j++) {
            if (icon_array[j] == selected && 
                icon_array[j]->display_window == g_pending_delete_canvas->win) {
                icon_still_valid = true;
                break;
            }
        }
        
        if (!icon_still_valid) {
            log_error("[WARNING] Icon no longer valid, skipping");
            continue;
        }
        
        // Skip System, Home icons, and iconified windows
        if (strcmp(selected->label, "System") == 0 || 
            strcmp(selected->label, "Home") == 0) {
            continue;
        }
        
        if (selected->type == TYPE_ICONIFIED) {
            continue;
        }
        
        // Save path before operations as destroy_icon will free it
        char saved_path[PATH_SIZE];
        snprintf(saved_path, sizeof(saved_path), "%s", selected->path);
        
        // Execute delete using progress-enabled function
        int result = wb_progress_perform_operation(2, // FILE_OP_DELETE = 2
                                                         saved_path, 
                                                         NULL,  // No destination for delete
                                                         "Deleting Files...");
        
        // Check if file was actually deleted
        if (result != 0 && access(saved_path, F_OK) != 0) {
            result = 0;  // Force success since file is gone
        }
        
        if (result == 0) {
            // Also delete the sidecar .info file if it exists
            char sidecar_path[PATH_SIZE + 10];  // PATH_SIZE + room for ".info"
            snprintf(sidecar_path, sizeof(sidecar_path), "%s.info", saved_path);
            if (access(sidecar_path, F_OK) == 0) {
                // Sidecar exists, delete it
                if (unlink(sidecar_path) != 0) {
                    log_error("[WARNING] Failed to delete sidecar: %s\n", sidecar_path);
                }
            }
            
            destroy_icon(selected);
            delete_count++;
            if (g_pending_delete_canvas->view_mode == VIEW_NAMES) {
                need_layout_update = true;
            }
        } else {
        }
    }
    
    // Update display once
    if (delete_count > 0 && g_pending_delete_canvas) {
        if (need_layout_update) {
            wb_layout_apply_view(g_pending_delete_canvas);
        }
        wb_layout_compute_bounds(g_pending_delete_canvas);
        compute_max_scroll(g_pending_delete_canvas);
        redraw_canvas(g_pending_delete_canvas);
        // Let compositor handle updates through events
        XSync(itn_core_get_display(), False);
    }
    
    // Clear pending state
    g_pending_delete_count = 0;
    g_pending_delete_canvas = NULL;
}

static void cancel_pending_deletes(void) {
    // Delete operation cancelled
    g_pending_delete_count = 0;
    g_pending_delete_canvas = NULL;
}

// ============================================================================
// File/Directory Opening Helper
// ============================================================================

static void open_file_or_directory(FileIcon *icon) {
    if (!icon) return;
    
    // Handle different icon types
    if (icon->type == TYPE_DRAWER) {
        // Directories (including System and Home) - open within AmiWB
        if (!icon->path) return;
        
        // Check if window for this path already exists
        Canvas *existing = find_window_by_path(icon->path);
        if (existing) {
            itn_focus_set_active(existing);
            XRaiseWindow(itn_core_get_display(), existing->win);
            redraw_canvas(existing);
        } else {
            // Create new window for directory
            Canvas *new_window = create_canvas(icon->path, 100, 100, 640, 480, WINDOW);
            if (new_window) {
                refresh_canvas_from_directory(new_window, icon->path);
                wb_layout_apply_view(new_window);
                compute_max_scroll(new_window);
                redraw_canvas(new_window);
            }
        }
    } else if (icon->type == TYPE_ICONIFIED) {
        // Use the existing restore_iconified function from workbench.c
        // It properly handles window restoration and icon cleanup
        wb_icons_restore_iconified(icon);
    } else if (icon->type == TYPE_FILE) {
        // Use the existing open_file function from workbench.c
        open_file(icon);
    }
}


// ============================================================================
// Main Menu Selection Dispatcher
// ============================================================================

void handle_menu_selection(Menu *menu, int item_index) {
    const char *item = menu->items[item_index];
    
    // Handle window list menu (parent_index == -1)
    if (menu->parent_index == -1) {
        // Use window_refs array instead of string comparison
        // This fixes both bugs: off-by-one and multiple instances
        Canvas *target = menu->window_refs[item_index];

        if (!target) {
            // NULL means "Desktop" option
            iconify_all_windows();
        } else {
            // Activate the window directly using its Canvas pointer
            // Check if window is iconified and restore it first
            FileIcon **icon_array = wb_icons_array_get();
            int icon_count = wb_icons_array_count();

            bool was_iconified = false;
            for (int i = 0; i < icon_count; i++) {
                FileIcon *ic = icon_array[i];
                if (ic && ic->type == TYPE_ICONIFIED && ic->iconified_canvas == target) {
                    wb_icons_restore_iconified(ic);
                    was_iconified = true;
                    break;
                }
            }

            if (!was_iconified) {
                // Not iconified - just activate it
                itn_focus_set_active(target);
            }
        }
        
        // Close the menu
        Menu *active = get_active_menu();
        if (active && active->canvas) {
            RenderContext *ctx = get_render_context();
            if (ctx) {
                XSync(ctx->dpy, False);
                if (active->canvas->win != None) {
                    clear_press_target_if_matches(active->canvas->win);
                    safe_unmap_window(ctx->dpy, active->canvas->win);
                    XSync(ctx->dpy, False);
                }
                itn_canvas_destroy(active->canvas);
                active->canvas = NULL;  // Prevent double-free

                // Free the temporary window menu
                if (active->items) {
                    for (int i = 0; i < active->item_count; i++) {
                        if (active->items[i]) free(active->items[i]);
                    }
                    free(active->items);
                }
                if (active->shortcuts) {
                    for (int i = 0; i < active->item_count; i++) {
                        if (active->shortcuts[i]) free(active->shortcuts[i]);
                    }
                    free(active->shortcuts);
                }
                if (active->enabled) free(active->enabled);
                free(active);

                menu_core_set_active_menu(NULL);
            }
        }
        return;
    }
    
    // Handle app menu selections
    if (is_app_menu_active()) {
        Window app_win = get_app_menu_window();
        if (app_win != None) {
            // For nested submenus, we need to send additional info
            if (menu->parent_menu && menu->parent_menu->parent_menu) {
                // This is a nested submenu - send parent menu index in data.l[2]
                Display *dpy = itn_core_get_display();
                if (dpy) {
                    XEvent event;
                    memset(&event, 0, sizeof(event));
                    event.type = ClientMessage;
                    event.xclient.window = app_win;
                    event.xclient.message_type = XInternAtom(dpy, "_AMIWB_MENU_SELECT", False);
                    event.xclient.format = 32;
                    event.xclient.data.l[0] = menu->parent_index;  // Parent item index (e.g., 2 for "Syntax")
                    event.xclient.data.l[1] = item_index;  // Which item in submenu
                    event.xclient.data.l[2] = menu->parent_menu->parent_index;  // Parent menu index (e.g., 3 for "View")
                    event.xclient.data.l[3] = 1;  // Flag: this is a submenu selection

                    XSendEvent(dpy, app_win, False, NoEventMask, &event);
                    XFlush(dpy);
                }
            } else {
                // Regular top-level menu selection
                send_menu_selection_to_app(app_win, menu->parent_index, item_index);
            }

            // Close menus after selection
            if (get_show_menus_state()) {
                toggle_menubar_state();
            }
            return;
        }
    }
    
    // If this is a nested submenu under Windows, handle here
    Menu *menubar = get_menubar_menu();
    if (menu->parent_menu && menu->parent_menu->parent_menu == menubar &&
        menu->parent_menu->parent_index == 1) {
        // Determine which child: by parent_index in Windows submenu
        if (menu->parent_index == 6) { // View Modes (now at index 6)
            Canvas *target = itn_focus_get_active();
            if (!target) {
                // No active window - use desktop
                target = itn_canvas_get_desktop();
            }
            if (target) {
                if (strcmp(item, "Icons") == 0) {
                    set_canvas_view_mode(target, VIEW_ICONS);
                    update_view_modes_checkmarks();  // Update checkmarks to reflect new state
                } else if (strcmp(item, "Names") == 0) {
                    set_canvas_view_mode(target, VIEW_NAMES);
                    update_view_modes_checkmarks();  // Update checkmarks to reflect new state
                } else if (strcmp(item, "Hidden") == 0) {
                    // Toggle global hidden files state
                    bool new_state = !get_global_show_hidden_state();
                    set_global_show_hidden_state(new_state);

                    // Apply to current target window
                    target->show_hidden = new_state;

                    // Refresh directory view to apply hidden filter
                    if (target->path) {
                        refresh_canvas_from_directory(target, target->path);
                    } else if (target->type == DESKTOP) {
                        // Desktop uses ~/Desktop as its path
                        const char *home = getenv("HOME");
                        if (home) {
                            char desktop_path[PATH_SIZE];
                            snprintf(desktop_path, sizeof(desktop_path), "%s/Desktop", home);
                            refresh_canvas_from_directory(target, desktop_path);
                        }
                    }
                    // Layout only applies to workbench windows, not desktop
                    if (target->type == WINDOW) {
                        wb_layout_apply_view(target);
                        compute_max_scroll(target);
                    }
                    redraw_canvas(target);
                    update_view_modes_checkmarks();  // Update checkmarks to reflect new state
                } else if (strcmp(item, "Spatial") == 0) {
                    // Toggle spatial mode
                    set_spatial_mode(!get_spatial_mode());
                    update_view_modes_checkmarks();  // Update checkmarks to reflect new state
                }
            }
        } else if (menu->parent_index == 6) { // Cycle
            if (strcmp(item, "Next") == 0) {
                itn_focus_cycle_next();
            } else if (strcmp(item, "Previous") == 0) {
                itn_focus_cycle_prev();
            }
        }
        return;
    }
    if (menu->parent_menu != menubar) return;  // Only top-level or handled above

    switch (menu->parent_index) {
        case 0:  // Workbench
            if (strcmp(item, "Execute") == 0) {
                trigger_execute_action();
            } else if (strcmp(item, "Requester") == 0) {
                trigger_requester_action();
            } else if (strcmp(item, "Settings") == 0) {
                // TODO: Open settings dialog or file
            } else if (strcmp(item, "About") == 0) {
                show_about_dialog();
            } else if (strcmp(item, "Suspend") == 0) {
                handle_suspend_request();
            
            } else if (strcmp(item, "Restart AmiWB") == 0) {
                handle_restart_request();
                return;

            } else if (strcmp(item, "Quit AmiWB") == 0) {
                handle_quit_request();
                return;
            }
            break;

        case 1:  // Window
            if (strcmp(item, "New Drawer") == 0) {
                trigger_new_drawer_action();
            } else if (strcmp(item, "Open Parent") == 0) {
                trigger_parent_action();
            } else if (strcmp(item, "Close") == 0) {
                trigger_close_action();
            } else if (strcmp(item, "Select Contents") == 0) {
                trigger_select_contents_action();
            } else if (strcmp(item, "Clean Up") == 0) {
                trigger_cleanup_action();
            } else if (strcmp(item, "Refresh") == 0) {
                trigger_refresh_action();
            } else if (strcmp(item, "Show") == 0) {
                // TODO: toggle hidden items
            } else if (strcmp(item, "View Icons") == 0) {
                Canvas *aw = itn_focus_get_active();
                if (aw) set_canvas_view_mode(aw, VIEW_ICONS);
            } else if (strcmp(item, "View Names") == 0) {
                Canvas *aw = itn_focus_get_active();
                if (aw) set_canvas_view_mode(aw, VIEW_NAMES);
            }
            break;

        case 2:  // Icons
            if (strcmp(item, "Open") == 0) {
                trigger_open_action();
            } else if (strcmp(item, "Copy") == 0) {
                trigger_copy_action();
            } else if (strcmp(item, "Rename") == 0) {
                trigger_rename_action();
            } else if (strcmp(item, "Extract") == 0) {
                trigger_extract_action();
            } else if (strcmp(item, "Eject") == 0) {
                trigger_eject_action();
            } else if (strcmp(item, "Information") == 0) {
                trigger_icon_info_action();
            } else if (strcmp(item, "delete") == 0) {
                trigger_delete_action();
            }
            break;

        case 3:  // Tools
            if (strcmp(item, "Text Editor") == 0) {
                launch_with_hook("editpad");
            } else if (strcmp(item, "XCalc") == 0) {
                launch_with_hook("xcalc");
            } else if (strcmp(item, "Shell") == 0) {
                launch_with_hook("kitty"); 

            } else if (strcmp(item, "Debug Console") == 0) {
                // Open a terminal that tails the configured log file live.
                // Uses config.h LOG_FILE_PATH and kitty.
                #if LOGGING_ENABLED
                // Embed LOG_FILE_PATH into the shell; $HOME in the macro will expand in sh -lc
                launch_with_hook("sh -lc 'exec kitty -e sh -lc "
                       "\"tail -f \\\"" LOG_FILE_PATH "\\\"\"'");
                #else
                launch_with_hook("sh -lc 'exec kitty -e sh -lc "
                       "\"echo Logging is disabled in config.h; echo Enable LOGGING_ENABLED and rebuild.; echo; read -p '""'Press Enter to close'""' \"\"\"'");
                #endif
            }

            break;

        default:
            // Check if this is a custom menu
            if (menu->parent_index >= 4 && menu->is_custom && menu->commands) {
                // Execute the custom command for this item
                if (item_index < menu->item_count && menu->commands[item_index]) {
                    execute_custom_command(menu->commands[item_index]);
                }
            }
            break;
    }
    if (get_show_menus_state()) {
        toggle_menubar_state();
    }
}

// ============================================================================
// Workbench Actions
// ============================================================================

void trigger_cleanup_action(void) {
    Canvas *active_window = itn_focus_get_active();
    
    // Clean up active window if it exists, otherwise clean up desktop
    if (active_window && active_window->type == WINDOW) {
        icon_cleanup(active_window);
        compute_max_scroll(active_window);
        redraw_canvas(active_window);
    } else {
        Canvas *desktop = itn_canvas_get_desktop();
        if (desktop) {
            icon_cleanup(desktop);
            compute_max_scroll(desktop);
            redraw_canvas(desktop);
        }
    }
}

void trigger_refresh_action(void) {
    Canvas *active_window = itn_focus_get_active();
    Canvas *target = active_window;
    
    // If no active window, use desktop
    if (!target || target->type != WINDOW) {
        target = itn_canvas_get_desktop();
    }
    
    if (target) {
        // Apply current global show_hidden state to the window
        target->show_hidden = get_global_show_hidden_state();
        
        // Refresh the directory contents
        if (target->path) {
            refresh_canvas_from_directory(target, target->path);
        } else if (target->type == DESKTOP) {
            // Desktop uses ~/Desktop as its path
            const char *home = getenv("HOME");
            if (home) {
                char desktop_path[PATH_SIZE];
                snprintf(desktop_path, sizeof(desktop_path), "%s/Desktop", home);
                refresh_canvas_from_directory(target, desktop_path);
            }
        }
        
        // icon_cleanup is now called inside refresh_canvas_from_directory
    }
}

void trigger_close_action(void) {
    Canvas *active_window = itn_focus_get_active();
    
    // Only close if there's an active window (not desktop)
    if (active_window && active_window->type == WINDOW) {
        // Use destroy_canvas which handles both client and non-client windows properly
        itn_canvas_destroy(active_window);
    }
}

void trigger_parent_action(void) {
    Canvas *active_window = itn_focus_get_active();
    
    // Only works if there's an active window with a path
    if (active_window && active_window->type == WINDOW && active_window->path) {
        // Get parent directory path
        char parent_path[PATH_SIZE];
        snprintf(parent_path, sizeof(parent_path), "%s", active_window->path);
        
        // Remove trailing slash if present
        size_t len = strlen(parent_path);
        if (len > 1 && parent_path[len - 1] == '/') {
            parent_path[len - 1] = '\0';
        }
        
        // Find last slash to get parent directory
        char *last_slash = strrchr(parent_path, '/');
        if (last_slash && last_slash != parent_path) {
            *last_slash = '\0';  // Truncate at last slash
        } else if (last_slash == parent_path) {
            // We're at root, keep the single slash
            parent_path[1] = '\0';
        } else {
            // No slash found or already at root
            return;
        }
        
        // Non-spatial mode: reuse current window
        if (!get_spatial_mode()) {
            // Update the current window's path to parent
            if (active_window->path) free(active_window->path);
            active_window->path = strdup(parent_path);
            
            // Update window title
            const char *dir_name = strrchr(parent_path, '/');
            if (dir_name && *(dir_name + 1)) dir_name++;
            else dir_name = parent_path;
            if (active_window->title_base) free(active_window->title_base);
            active_window->title_base = strdup(dir_name);

            // Recalculate cached title width (cache invalidation after title change)
            itn_decorations_recalc_title_width(active_window);

            // Refresh with parent directory
            refresh_canvas_from_directory(active_window, parent_path);
            
            // Reset scroll (icon_cleanup now called inside refresh_canvas_from_directory)
            active_window->scroll_x = 0;
            active_window->scroll_y = 0;
            redraw_canvas(active_window);
        } else {
            // Spatial mode: check if window for parent path already exists
            Canvas *existing = find_window_by_path(parent_path);
            if (existing) {
                itn_focus_set_active(existing);
                XRaiseWindow(itn_core_get_display(), existing->win);
                redraw_canvas(existing);
            } else {
                // Create new window for parent directory
                Canvas *parent_window = create_canvas(parent_path, 
                    active_window->x + 30, active_window->y + 30,
                    640, 480, WINDOW);
                if (parent_window) {
                    refresh_canvas_from_directory(parent_window, parent_path);
                    wb_layout_apply_view(parent_window);
                    compute_max_scroll(parent_window);
                    redraw_canvas(parent_window);
                }
            }
        }
    }
}

// ============================================================================
// Icon Actions
// ============================================================================

void trigger_open_action(void) {
    // Get the selected icon from active window or desktop
    FileIcon *selected = NULL;
    Canvas *aw = itn_focus_get_active();
    Canvas *check_canvas = NULL;
    
    // If no active window, check desktop
    if (!aw || aw->type == DESKTOP) {
        check_canvas = itn_canvas_get_desktop();
    } else if (aw->type == WINDOW) {
        check_canvas = aw;
    }
    
    if (check_canvas) {
        FileIcon **icon_array = wb_icons_array_get();
        int icon_count = wb_icons_array_count();
        for (int i = 0; i < icon_count; i++) {
            FileIcon *icon = icon_array[i];
            if (icon && icon->selected && icon->display_window == check_canvas->win) {
                selected = icon;
                break;
            }
        }
    }
    
    if (selected) {
        // Save label before calling open_file_or_directory as it may destroy the icon
        char label_copy[256];
        snprintf(label_copy, sizeof(label_copy), "%s", selected->label);
        
        open_file_or_directory(selected);
        // Use saved label as icon may have been destroyed
    } else {
    }
}

void trigger_copy_action(void) {
    // Get the selected icon from active window or desktop
    FileIcon *selected = NULL;
    Canvas *aw = itn_focus_get_active();
    Canvas *target_canvas = NULL;
    
    // If no active window, check desktop
    if (!aw || aw->type == DESKTOP) {
        target_canvas = itn_canvas_get_desktop();
    } else if (aw->type == WINDOW) {
        target_canvas = aw;
    }
    
    if (target_canvas) {
        FileIcon **icon_array = wb_icons_array_get();
        int icon_count = wb_icons_array_count();
        for (int i = 0; i < icon_count; i++) {
            FileIcon *icon = icon_array[i];
            if (icon && icon->selected && icon->display_window == target_canvas->win) {
                selected = icon;
                break;
            }
        }
    }
    
    if (selected && selected->path) {
        // Check restrictions - cannot copy System, Home, or iconified windows
        if (strcmp(selected->label, "System") == 0 || strcmp(selected->label, "Home") == 0) {
            return;
        }
        if (selected->type == TYPE_ICONIFIED) {
            return;
        }
        
        // Generate copy name
        char copy_path[PATH_SIZE];
        char base_name[NAME_SIZE];
        char *last_slash = strrchr(selected->path, '/');
        char *dir_path = NULL;
        
        
        if (last_slash) {
            size_t dir_len = last_slash - selected->path;
            dir_path = malloc(dir_len + 2);
            if (!dir_path) {
                log_error("[ERROR] Failed to allocate memory for directory path - copy cancelled");
                return;  // Graceful degradation: abort copy operation
            }
            snprintf(dir_path, dir_len + 1, "%.*s", (int)dir_len, selected->path);
            snprintf(base_name, NAME_SIZE, "%s", last_slash + 1);
            // Extract directory and base name
        } else {
            dir_path = strdup(".");
            if (!dir_path) {
                log_error("[ERROR] strdup failed for directory path - copy cancelled");
                return;  // Graceful degradation: abort copy operation
            }
            snprintf(base_name, NAME_SIZE, "%s", selected->path);
            // Use current directory
        }
        
        // Find available copy name with prefix pattern: copy_myfile, copy1_myfile, copy2_myfile...
        int copy_num = 0;
        do {
            if (copy_num == 0) {
                // Ensure we don't overflow: dir_path + "/" + "copy_" + base_name
                if (strlen(dir_path) + strlen(base_name) + 7 < sizeof(copy_path)) {
                    snprintf(copy_path, sizeof(copy_path), "%s/copy_%s", dir_path, base_name);
                } else {
                    log_error("[ERROR] Path too long for copy operation");
                    free(dir_path);
                    return;
                }
            } else {
                // Ensure we don't overflow: dir_path + "/" + "copy" + num + "_" + base_name
                if (strlen(dir_path) + strlen(base_name) + 12 < sizeof(copy_path)) {
                    snprintf(copy_path, sizeof(copy_path), "%s/copy%d_%s", dir_path, copy_num, base_name);
                } else {
                    log_error("[ERROR] Path too long for copy operation");
                    free(dir_path);
                    return;
                }
            }
            copy_num++;
        } while (access(copy_path, F_OK) == 0 && copy_num < 100);
        
        // Save path before refresh as it will destroy the icon
        char saved_path[PATH_SIZE];
        snprintf(saved_path, sizeof(saved_path), "%s", selected->path);
        
        // Check if source has a sidecar .info file
        char src_info_path[PATH_SIZE + 10];  // PATH_SIZE + room for ".info"
        char dst_info_path[PATH_SIZE + 10];  // PATH_SIZE + room for ".info"
        bool has_sidecar = false;
        
        if (strlen(selected->path) < PATH_SIZE && strlen(copy_path) < PATH_SIZE) {
            snprintf(src_info_path, sizeof(src_info_path), "%s.info", selected->path);
            struct stat info_stat;
            if (stat(src_info_path, &info_stat) == 0) {
                has_sidecar = true;
                snprintf(dst_info_path, sizeof(dst_info_path), "%s.info", copy_path);
            }
        }
        
        // Find a good position for the new icon
        int new_x = selected->x + 110;
        int new_y = selected->y;
        
        if (target_canvas) {
            // Check for overlaps and adjust position
            FileIcon **icon_array = wb_icons_array_get();
            int icon_count = wb_icons_array_count();
            bool position_occupied = true;
            int attempts = 0;
            
            while (position_occupied && attempts < 10) {
                position_occupied = false;
                for (int i = 0; i < icon_count; i++) {
                    FileIcon *other = icon_array[i];
                    if (other && other != selected && 
                        other->display_window == target_canvas->win) {
                        if (abs(other->x - new_x) < 100 && abs(other->y - new_y) < 80) {
                            position_occupied = true;
                            if (attempts < 5) {
                                new_x += 110;
                            } else {
                                new_x = selected->x + 110;
                                new_y += 80;
                            }
                            break;
                        }
                    }
                }
                attempts++;
            }
        }
        
        // Prepare icon metadata for deferred creation
        typedef struct {
            enum { MSG_START, MSG_PROGRESS, MSG_COMPLETE, MSG_ERROR } type;
            time_t start_time;
            int files_done;
            int files_total;
            char current_file[NAME_SIZE];          // Filename only
            size_t bytes_done;
            size_t bytes_total;
            char dest_path[FULL_SIZE];             // Full path with potential extensions
            char dest_dir[PATH_SIZE];              // Directory only
            bool create_icon;
            bool has_sidecar;
            char sidecar_src[FULL_SIZE];           // Full path + ".info" suffix
            char sidecar_dst[FULL_SIZE];           // Full path + ".info" suffix
            int icon_x, icon_y;
            Window target_window;
        } ProgressMessage;
        
        ProgressMessage icon_metadata = {0};
        icon_metadata.create_icon = (target_canvas != NULL);
        icon_metadata.has_sidecar = has_sidecar;
        icon_metadata.icon_x = new_x;
        icon_metadata.icon_y = new_y;
        icon_metadata.target_window = target_canvas ? target_canvas->win : None;
        snprintf(icon_metadata.dest_path, sizeof(icon_metadata.dest_path), "%s", copy_path);
        snprintf(icon_metadata.dest_dir, sizeof(icon_metadata.dest_dir), "%s", dir_path);
        if (has_sidecar) {
            snprintf(icon_metadata.sidecar_src, sizeof(icon_metadata.sidecar_src), "%s", src_info_path);
            snprintf(icon_metadata.sidecar_dst, sizeof(icon_metadata.sidecar_dst), "%s", dst_info_path);
        }
        
        // Use extended file operation with icon metadata
        int result = wb_progress_perform_operation_ex(0, // FILE_OP_COPY = 0
                                                            selected->path, 
                                                            copy_path, 
                                                            "Copying Files...",
                                                            &icon_metadata);
        
        if (result == 0) {
            // Copy started successfully - icon will be created when it completes
            // Copy started successfully
        } else {
            log_error("[ERROR] Copy failed for: %s", saved_path);  // Use saved path
        }
        
        free(dir_path);
    }
}

void trigger_extract_action(void) {
    // Get the selected icon from active window or desktop
    FileIcon *selected = NULL;
    Canvas *aw = itn_focus_get_active();
    Canvas *target_canvas = NULL;
    
    // If no active window, check desktop
    if (!aw || aw->type == DESKTOP) {
        target_canvas = itn_canvas_get_desktop();
    } else if (aw->type == WINDOW) {
        target_canvas = aw;
    }
    
    if (target_canvas) {
        FileIcon **icon_array = wb_icons_array_get();
        int icon_count = wb_icons_array_count();
        for (int i = 0; i < icon_count; i++) {
            FileIcon *icon = icon_array[i];
            if (icon && icon->selected && icon->display_window == target_canvas->win) {
                selected = icon;
                break;
            }
        }
    }
    
    if (selected && selected->path) {
        // Check if the selected file is an archive
        const char *ext = strrchr(selected->path, '.');
        if (!ext) return;
        ext++; // Skip the dot
        
        // Check for supported archive formats
        const char *archive_exts[] = {
            "lha", "lzh", "zip", "tar", "gz", "tgz", "bz2", "tbz",
            "xz", "txz", "rar", "7z", NULL
        };
        
        bool is_archive = false;
        for (int i = 0; archive_exts[i]; i++) {
            if (strcasecmp(ext, archive_exts[i]) == 0) {
                is_archive = true;
                break;
            }
        }
        
        // Also check for compound extensions
        const char *name = strrchr(selected->path, '/');
        name = name ? name + 1 : selected->path;
        if (strstr(name, ".tar.gz") || strstr(name, ".tar.bz2") || strstr(name, ".tar.xz")) {
            is_archive = true;
        }
        
        if (is_archive) {
            // Call the extraction function, passing the canvas so we know where to create the icon
            extract_file_at_path(selected->path, target_canvas);
        }
    }
}

void trigger_eject_action(void) {
    // Get the selected icon from active window or desktop
    FileIcon *selected = NULL;
    Canvas *aw = itn_focus_get_active();
    Canvas *target_canvas = NULL;
    
    // If no active window, check desktop
    if (!aw || aw->type == DESKTOP) {
        target_canvas = itn_canvas_get_desktop();
    } else if (aw->type == WINDOW) {
        target_canvas = aw;
    }
    
    if (target_canvas) {
        FileIcon **icon_array = wb_icons_array_get();
        int icon_count = wb_icons_array_count();
        for (int i = 0; i < icon_count; i++) {
            FileIcon *icon = icon_array[i];
            if (icon && icon->selected && icon->display_window == target_canvas->win) {
                selected = icon;
                break;
            }
        }
    }
    
    // Only eject if it's a TYPE_DEVICE icon
    if (selected && selected->type == TYPE_DEVICE) {
        // Import eject_drive function from diskdrives.h
        eject_drive(selected);
    }
}

void trigger_delete_action(void) {
    Canvas *aw = itn_focus_get_active();
    Canvas *target_canvas = NULL;
    
    // If no active window, check desktop
    if (!aw || aw->type == DESKTOP) {
        target_canvas = itn_canvas_get_desktop();
    } else if (aw->type == WINDOW) {
        target_canvas = aw;
    }
    
    if (!target_canvas) return;
    
    // CRITICAL: Clear any previous pending deletes
    g_pending_delete_count = 0;
    g_pending_delete_canvas = target_canvas;
    
    // Collect ALL selected icons FROM THIS WINDOW ONLY
    FileIcon **icon_array = wb_icons_array_get();
    int icon_count = wb_icons_array_count();
    for (int i = 0; i < icon_count && g_pending_delete_count < 256; i++) {
        FileIcon *icon = icon_array[i];
        // CRITICAL: Only from target canvas window!
        if (icon && icon->selected && icon->display_window == target_canvas->win) {
            g_pending_delete_icons[g_pending_delete_count++] = icon;
        }
    }
    
    // Check if anything selected
    if (g_pending_delete_count == 0) return;
    
    // Count files and directories separately for proper message formatting
    int file_count = 0;
    int dir_count = 0;
    for (int i = 0; i < g_pending_delete_count; i++) {
        FileIcon *icon = g_pending_delete_icons[i];
        if (icon->type == TYPE_DRAWER) {
            dir_count++;
        } else {
            file_count++;
        }
    }
    
    // Build confirmation message with proper grammar
    char message[256];
    if (file_count > 0 && dir_count > 0) {
        // Both files and directories
        if (file_count == 1 && dir_count == 1) {
            snprintf(message, sizeof(message), "1 file and 1 directory?");
        } else if (file_count == 1) {
            snprintf(message, sizeof(message), "1 file and %d directories?", dir_count);
        } else if (dir_count == 1) {
            snprintf(message, sizeof(message), "%d files and 1 directory?", file_count);
        } else {
            snprintf(message, sizeof(message), "%d files and %d directories?", 
                    file_count, dir_count);
        }
    } else if (file_count > 0) {
        // Only files
        if (file_count == 1) {
            snprintf(message, sizeof(message), "1 file?");
        } else {
            snprintf(message, sizeof(message), "%d files?", file_count);
        }
    } else {
        // Only directories
        if (dir_count == 1) {
            snprintf(message, sizeof(message), "1 directory?");
        } else {
            snprintf(message, sizeof(message), "%d directories?", dir_count);
        }
    }
    
    // Show confirmation dialog - CRITICAL FOR DATA SAFETY
    show_delete_confirmation(message, execute_pending_deletes, cancel_pending_deletes);
}

void trigger_execute_action(void) {
    show_execute_dialog(execute_command_ok_callback, execute_command_cancel_callback);
}

void trigger_requester_action(void) {
    // Launch reqasl in the background
    launch_with_hook("reqasl");
}

void trigger_rename_action(void) {
    Canvas *active_window = itn_focus_get_active();
    FileIcon *selected = NULL;
    
    // Check conditions for rename:
    // 1. Active window with selected icon, OR
    // 2. No active window but desktop has selected icon
    
    if (active_window && active_window->type == WINDOW) {
        // Active window exists - get selected icon from it
        selected = get_selected_icon_from_canvas(active_window);
    } else if (!active_window) {
        // No active window - check desktop for selected icon
        Canvas *desktop = itn_canvas_get_desktop();
        if (desktop) {
            selected = get_selected_icon_from_canvas(desktop);
        }
    }
    // If active_window exists but is not WINDOW type, do nothing
    
    // Show rename dialog only if proper conditions are met
    if (selected && selected->label && selected->path) {
        // Check restrictions - cannot rename System, Home, or iconified windows
        if (strcmp(selected->label, "System") == 0 || strcmp(selected->label, "Home") == 0) {
            return;
        }
        if (selected->type == TYPE_ICONIFIED) {
            return;
        }
        
        // Store the icon globally so the callback can access it
        g_rename_icon = selected;
        show_rename_dialog(selected->label, rename_file_ok_callback, rename_file_cancel_callback, selected);
    } else {
    }
}

void trigger_icon_info_action(void) {
    Canvas *active_window = itn_focus_get_active();
    FileIcon *selected = NULL;
    
    // Check conditions for icon info:
    // 1. Active window with selected icon, OR
    // 2. No active window but desktop has selected icon
    
    if (active_window && active_window->type == WINDOW) {
        // Active window exists - get selected icon from it
        selected = get_selected_icon_from_canvas(active_window);
    } else if (!active_window) {
        // No active window - check desktop for selected icon
        Canvas *desktop = itn_canvas_get_desktop();
        if (desktop) {
            selected = get_selected_icon_from_canvas(desktop);
        }
    }
    
    // Show icon info dialog if an icon is selected
    if (selected) {
        show_icon_info_dialog(selected);
    }
}


// ============================================================================
// Selection and Creation Actions
// ============================================================================

void trigger_select_contents_action(void) {
    Canvas *target_canvas = NULL;
    
    // Determine which canvas to select icons in
    Canvas *active_window = itn_focus_get_active();
    
    if (active_window && active_window->type == WINDOW) {
        target_canvas = active_window;
    } else {
        // No active window - use desktop
        target_canvas = itn_canvas_get_desktop();
    }
    
    if (!target_canvas) return;
    
    // Get all icons and check if any are already selected in this canvas
    FileIcon **icon_array = wb_icons_array_get();
    int icon_count = wb_icons_array_count();
    bool has_selected = false;
    
    // First pass: check if any icons are selected
    for (int i = 0; i < icon_count; i++) {
        FileIcon *icon = icon_array[i];
        if (icon && icon->display_window == target_canvas->win && icon->selected) {
            has_selected = true;
            break;
        }
    }
    
    // Second pass: toggle selection
    // If some are selected, deselect all. If none selected, select all.
    bool new_state = !has_selected;
    
    for (int i = 0; i < icon_count; i++) {
        FileIcon *icon = icon_array[i];
        if (icon && icon->display_window == target_canvas->win) {
            // Don't select System or Home icons on desktop
            if (target_canvas->type == DESKTOP && 
                (strcmp(icon->label, "System") == 0 || strcmp(icon->label, "Home") == 0)) {
                continue;
            }
            icon->selected = new_state;
            // Update the icon's picture to show selection state
            icon->current_picture = new_state ? icon->selected_picture : icon->normal_picture;
        }
    }
    
    // Redraw the canvas to show selection changes
    redraw_canvas(target_canvas);
}

void trigger_new_drawer_action(void) {
    // Determine where to create the new drawer
    Canvas *active_window = itn_focus_get_active();
    Canvas *target_canvas = NULL;

    if (active_window && active_window->type == WINDOW) {
        // Create in active window's directory
        target_canvas = active_window;
    } else {
        // Create on desktop
        target_canvas = itn_canvas_get_desktop();
    }

    if (target_canvas) {
        // Delegate to workbench module - proper separation of concerns
        workbench_create_new_drawer(target_canvas);
    }
}

// ============================================================================
// System Actions
// ============================================================================

void handle_quit_request(void) {
    // Enter shutdown mode: silence X errors from teardown
    begin_shutdown();

    // CRITICAL: Stop event loop FIRST before closing display
    // Otherwise event loop continues and calls XPending() on freed display
    quit_event_loop();

    // Menus/workbench use canvases; keep render/Display alive until after compositor shut down
    // First, stop compositing (uses the Display)
    itn_core_shutdown_compositor();
    // Then tear down UI modules
    cleanup_menus();
    cleanup_workbench();
    // Finally close Display and render resources
    cleanup_intuition();
    cleanup_render();
}

void handle_suspend_request(void) {
    launch_with_hook("systemctl suspend");
}

void handle_restart_request(void) {
    restart_amiwb();
}

// ============================================================================
// Custom Command Execution
// ============================================================================

void execute_custom_command(const char *cmd) {
    launch_with_hook(cmd);
}
