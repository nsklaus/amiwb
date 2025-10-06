// File: iconinfo.c
// Icon Information dialog implementation
#include "iconinfo.h"
#include "render.h"
#include "config.h"
#include "workbench/wb_public.h"
#include "intuition/itn_internal.h"
#include "../toolkit/button.h"
#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>
#include <X11/Xft/Xft.h>
#include <X11/keysym.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <sys/wait.h>
#include <pwd.h>
#include <grp.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <libgen.h>

// Global dialog list
static IconInfoDialog *g_iconinfo_dialogs = NULL;

// External functions
extern XftFont *get_font(void);

// Forward declarations
static void load_file_info(IconInfoDialog *dialog);
static Picture create_2x_icon(FileIcon *icon);
static void format_file_size(off_t size, char *buffer, size_t bufsize);
static void format_permissions(mode_t mode, char *buffer, size_t bufsize);
static void save_file_changes(IconInfoDialog *dialog);

// Initialize icon info subsystem
void init_iconinfo(void) {
    g_iconinfo_dialogs = NULL;
}

// Clean up all icon info dialogs
void cleanup_iconinfo(void) {
    cleanup_all_iconinfo_dialogs();
}

// Show icon information dialog for given icon
void show_icon_info_dialog(FileIcon *icon) {
    if (!icon) {
        log_error("[WARNING] show_icon_info_dialog called with NULL icon");
        return;
    }
    
    
    // Create dialog structure
    IconInfoDialog *dialog = calloc(1, sizeof(IconInfoDialog));
    if (!dialog) {
        log_error("[ERROR] Failed to allocate IconInfoDialog for %s", icon->label);
        return;
    }
    
    dialog->icon = icon;
    
    // Create canvas window (as DIALOG type for proper window management)
    dialog->canvas = create_canvas(NULL, 100, 100, ICONINFO_WIDTH, ICONINFO_HEIGHT, DIALOG);

    // Set minimum window size to initial size
    if (dialog->canvas) {
        dialog->canvas->min_width = ICONINFO_WIDTH;
        dialog->canvas->min_height = ICONINFO_HEIGHT;
        dialog->canvas->resize_x_allowed = true;
        dialog->canvas->resize_y_allowed = true;
    }
    if (!dialog->canvas) {
        log_error("[ERROR] Failed to create canvas for IconInfoDialog: %s", icon->label);
        free(dialog);
        return;
    }
    
    // Set window title
    dialog->canvas->title_base = strdup("Icon Info");
    dialog->canvas->title_change = NULL;
    dialog->canvas->bg_color = GRAY;
    dialog->canvas->disable_scrollbars = true;
    
    // Create 2x scaled icon
    dialog->icon_2x = create_2x_icon(icon);
    dialog->icon_display_size = ICONINFO_ICON_SIZE * 2;
    
    // Create input fields (accounting for window borders)
    // Align with "Size:" label which is at icon_x + 128 + 20 = approx 163
    int field_x = ICONINFO_MARGIN + dialog->icon_display_size + 20;  // Same x as "Size:" label
    int field_width = ICONINFO_WIDTH - field_x - ICONINFO_MARGIN;  // End before right edge
    int y_pos = BORDER_HEIGHT_TOP + ICONINFO_MARGIN - 1;  // Slightly above to avoid icon frame
    
    // Name field (editable)
    dialog->name_field = inputfield_create(field_x, y_pos, field_width, 20, get_font());
    if (dialog->name_field) {
        snprintf(dialog->name_field->name, sizeof(dialog->name_field->name), "Filename");
        inputfield_set_text(dialog->name_field, icon->label);
    } else {
        log_error("[WARNING] Failed to create name field");
    }
    
    y_pos = BORDER_HEIGHT_TOP + dialog->icon_display_size + 40;  // Below icon
    
    // Comment field (editable) - same x alignment as name field
    dialog->comment_field = inputfield_create(field_x,  // Same x as name field and "Size:" 
                                            y_pos, 
                                            field_width,  // Same width as name field
                                            20, get_font());
    if (dialog->comment_field) {
        snprintf(dialog->comment_field->name, sizeof(dialog->comment_field->name), "Comment");
        inputfield_set_text(dialog->comment_field, "");  // Empty by default
    }
    
    // Comment listview - below the comment input field
    y_pos += 25;  // Move down past the input field
    dialog->comment_list = listview_create(field_x, y_pos, field_width, 80);  // 4 lines * 20px each
    if (dialog->comment_list) {
        // Set up callback for when user clicks a comment line
        listview_set_callbacks(dialog->comment_list, NULL, NULL, dialog);
    }
    
    // Path field (read-only, for copying) - shows directory only
    // THIS IS WRONG - let's position it properly later in render
    // For now, use the same positioning as name field
    dialog->path_field = inputfield_create(field_x,
                                          y_pos + 200,  // Temporary - will be repositioned in render
                                          field_width,
                                          20, get_font());
    if (dialog->path_field) {
        snprintf(dialog->path_field->name, sizeof(dialog->path_field->name), "Filepath");
        // Extract directory path (without filename)
        char dir_path[PATH_SIZE];
        strncpy(dir_path, icon->path, sizeof(dir_path) - 1);
        dir_path[sizeof(dir_path) - 1] = '\0';
        char *last_slash = strrchr(dir_path, '/');
        if (last_slash && last_slash != dir_path) {
            *(last_slash + 1) = '\0';  // Keep the trailing slash
        }
        inputfield_set_text(dialog->path_field, dir_path);
        // Set as readonly - can select/copy but not edit
        inputfield_set_readonly(dialog->path_field, true);
    }
    
    y_pos += 25;  // Move down for next field (consistent 25px gap)
    
    // Opens with field (editable) - same alignment as above fields
    dialog->app_field = inputfield_create(field_x,
                                         y_pos,
                                         field_width,
                                         20, get_font());
    if (dialog->app_field) {
        snprintf(dialog->app_field->name, sizeof(dialog->app_field->name), "Run with");
    }
    
    // Load file information
    load_file_info(dialog);
    
    // Add to dialog list
    dialog->next = g_iconinfo_dialogs;
    g_iconinfo_dialogs = dialog;
    
    // Show the dialog (canvas is already mapped by create_canvas, just set focus)
    itn_focus_set_active(dialog->canvas);
    redraw_canvas(dialog->canvas);
}

// Load file information into dialog
static void load_file_info(IconInfoDialog *dialog) {
    if (!dialog || !dialog->icon) return;
    
    struct stat st;
    if (stat(dialog->icon->path, &st) == 0) {
        // Check if directory
        dialog->is_directory = S_ISDIR(st.st_mode);
        
        // Format size
        if (dialog->is_directory) {
            snprintf(dialog->size_text, sizeof(dialog->size_text), "[Get Size]");
        } else {
            format_file_size(st.st_size, dialog->size_text, sizeof(dialog->size_text));
        }
        
        // Format permissions
        format_permissions(st.st_mode, dialog->perms_text, sizeof(dialog->perms_text));
        
        // Get owner and group names
        struct passwd *pw = getpwuid(st.st_uid);
        if (pw) {
            strncpy(dialog->owner_text, pw->pw_name, sizeof(dialog->owner_text) - 1);
        } else {
            snprintf(dialog->owner_text, sizeof(dialog->owner_text), "%d", st.st_uid);
        }
        
        struct group *gr = getgrgid(st.st_gid);
        if (gr) {
            strncpy(dialog->group_text, gr->gr_name, sizeof(dialog->group_text) - 1);
        } else {
            snprintf(dialog->group_text, sizeof(dialog->group_text), "%d", st.st_gid);
        }
        
        // Format dates (swapped - mtime is usually older, ctime is usually newer)
        struct tm *tm;
        
        // Modified time (when content was last changed)
        tm = localtime(&st.st_mtime);
        strftime(dialog->created_text, sizeof(dialog->created_text), 
                "%Y-%m-%d at %H:%M", tm);
        
        // Status change time (when metadata was last changed)
        tm = localtime(&st.st_ctime);
        strftime(dialog->modified_text, sizeof(dialog->modified_text),
                "%Y-%m-%d at %H:%M", tm);
        
        // Set permission checkboxes
        dialog->perm_user_read = (st.st_mode & S_IRUSR) != 0;
        dialog->perm_user_write = (st.st_mode & S_IWUSR) != 0;
        dialog->perm_user_exec = (st.st_mode & S_IXUSR) != 0;
        dialog->perm_group_read = (st.st_mode & S_IRGRP) != 0;
        dialog->perm_group_write = (st.st_mode & S_IWGRP) != 0;
        dialog->perm_group_exec = (st.st_mode & S_IXGRP) != 0;
        dialog->perm_other_read = (st.st_mode & S_IROTH) != 0;
        dialog->perm_other_write = (st.st_mode & S_IWOTH) != 0;
        dialog->perm_other_exec = (st.st_mode & S_IXOTH) != 0;
    }
    
    // Path is now displayed as plain text, not an input field
    
    // Try to read comment from extended attributes
    char comment[PATH_SIZE] = {0};  // Larger buffer for multiple lines
    ssize_t len = getxattr(dialog->icon->path, "user.comment", comment, sizeof(comment) - 1);
    if (len > 0) {
        comment[len] = '\0';
        // Split comment by newlines and add to listview
        if (dialog->comment_list) {
            char *line = strtok(comment, "\n");
            while (line) {
                if (strlen(line) > 0) {
                    listview_add_item(dialog->comment_list, line, false, NULL);
                }
                line = strtok(NULL, "\n");
            }
        }
        // Leave comment field empty as requested
    }
    
    // Get default application using xdg-mime for files (not directories)
    if (!dialog->is_directory && dialog->app_field) {
        // Get MIME type for the file
        char cmd[PATH_SIZE * 2];
        snprintf(cmd, sizeof(cmd), "xdg-mime query filetype '%s' 2>/dev/null", dialog->icon->path);
        
        FILE *fp = popen(cmd, "r");
        if (fp) {
            char mimetype[NAME_SIZE];
            if (fgets(mimetype, sizeof(mimetype), fp)) {
                // Remove newline
                size_t len = strlen(mimetype);
                if (len > 0 && mimetype[len-1] == '\n') {
                    mimetype[len-1] = '\0';
                }
                
                // Get default application for this MIME type
                snprintf(cmd, sizeof(cmd), "xdg-mime query default '%s' 2>/dev/null", mimetype);
                pclose(fp);
                
                fp = popen(cmd, "r");
                if (fp) {
                    char desktop_file[NAME_SIZE];
                    if (fgets(desktop_file, sizeof(desktop_file), fp)) {
                        // Remove newline and .desktop extension
                        len = strlen(desktop_file);
                        if (len > 0 && desktop_file[len-1] == '\n') {
                            desktop_file[len-1] = '\0';
                        }
                        if (strstr(desktop_file, ".desktop")) {
                            *strstr(desktop_file, ".desktop") = '\0';
                        }
                        
                        // Set the app field to the application name
                        inputfield_set_text(dialog->app_field, desktop_file);
                    }
                    pclose(fp);
                }
            } else {
                pclose(fp);
            }
        }
    }
    
    // Disable "Opens with" field for directories
    if (dialog->is_directory && dialog->app_field) {
        inputfield_set_disabled(dialog->app_field, true);
    }
}

// Save changes made in the dialog
static void save_file_changes(IconInfoDialog *dialog) {
    if (!dialog || !dialog->icon) return;
    
    bool needs_refresh = false;
    
    // 1. Check if filename changed and rename if needed
    if (dialog->name_field) {
        const char *new_name = inputfield_get_text(dialog->name_field);
        if (new_name && strcmp(new_name, dialog->icon->label) != 0) {
            // Build new path
            char new_path[PATH_SIZE];
            char *last_slash = strrchr(dialog->icon->path, '/');
            if (last_slash) {
                size_t dir_len = last_slash - dialog->icon->path;
                snprintf(new_path, sizeof(new_path), "%.*s/%s", 
                        (int)dir_len, dialog->icon->path, new_name);
                
                // Rename the file
                if (rename(dialog->icon->path, new_path) == 0) {
                    // Rename successful - silent success per logging rules
                    needs_refresh = true;
                } else {
                    log_error("[WARNING] Failed to rename '%s' to '%s': %s", 
                             dialog->icon->path, new_path, strerror(errno));
                }
            }
        }
    }
    
    // 2. Save comment to extended attributes (combine listview items)
    if (dialog->comment_list && dialog->comment_list->item_count > 0) {
        // Build multi-line comment from listview items
        char combined_comment[PATH_SIZE] = {0};
        size_t offset = 0;
        
        for (int i = 0; i < dialog->comment_list->item_count; i++) {
            const char *line = dialog->comment_list->items[i].text;
            size_t line_len = strlen(line);
            
            if (offset + line_len + 1 < sizeof(combined_comment)) {
                if (offset > 0) {
                    combined_comment[offset++] = '\n';
                }
                memcpy(combined_comment + offset, line, line_len + 1);  // +1 for null terminator
                offset += line_len;
            }
        }
        
        // Set the comment xattr
        if (offset > 0) {
            if (setxattr(dialog->icon->path, "user.comment", combined_comment, 
                        offset, 0) == -1) {
                log_error("[WARNING] Failed to set comment xattr: %s", strerror(errno));
            }
        }
    } else {
        // Remove comment if no items in list
        removexattr(dialog->icon->path, "user.comment");
    }
    
    // 3. Update default application (xdg-mime) for files only
    if (!dialog->is_directory && dialog->app_field) {
        const char *app = inputfield_get_text(dialog->app_field);
        if (app && *app) {
            // Get MIME type for the file
            char cmd[PATH_SIZE * 2];
            snprintf(cmd, sizeof(cmd), "xdg-mime query filetype '%s' 2>/dev/null", 
                    dialog->icon->path);
            
            FILE *fp = popen(cmd, "r");
            if (fp) {
                char mimetype[NAME_SIZE];
                if (fgets(mimetype, sizeof(mimetype), fp)) {
                    // Remove newline
                    size_t len = strlen(mimetype);
                    if (len > 0 && mimetype[len-1] == '\n') {
                        mimetype[len-1] = '\0';
                    }
                    
                    // Set default application for this MIME type
                    // Add .desktop extension if not present
                    char desktop_file[NAME_SIZE];
                    if (strstr(app, ".desktop")) {
                        snprintf(desktop_file, sizeof(desktop_file), "%s", app);
                    } else {
                        snprintf(desktop_file, sizeof(desktop_file), "%s.desktop", app);
                    }
                    
                    snprintf(cmd, sizeof(cmd), "xdg-mime default '%s' '%s' 2>/dev/null", 
                            desktop_file, mimetype);
                    int result = system(cmd);
                    if (result == 0) {
                        // Default app set successfully - silent success
                    } else {
                        log_error("[WARNING] Failed to set default app for %s", mimetype);
                    }
                }
                pclose(fp);
            }
        }
    }
    
    // TODO: Refresh the workbench window if needed
    // For now, user needs to manually refresh after rename
    if (needs_refresh) {
        // File renamed - manual refresh may be needed
    }
}

// Format file size for display
static void format_file_size(off_t size, char *buffer, size_t bufsize) {
    if (size < 1024) {
        snprintf(buffer, bufsize, "%ld bytes", size);
    } else if (size < 1024 * 1024) {
        snprintf(buffer, bufsize, "%.1f KB", size / 1024.0);
    } else if (size < 1024 * 1024 * 1024) {
        snprintf(buffer, bufsize, "%.1f MB", size / (1024.0 * 1024.0));
    } else {
        snprintf(buffer, bufsize, "%.1f GB", size / (1024.0 * 1024.0 * 1024.0));
    }
}

// Format permissions for display
static void format_permissions(mode_t mode, char *buffer, size_t bufsize) {
    snprintf(buffer, bufsize, "%c%c%c%c%c%c%c%c%c",
            (mode & S_IRUSR) ? 'r' : '-',
            (mode & S_IWUSR) ? 'w' : '-',
            (mode & S_IXUSR) ? 'x' : '-',
            (mode & S_IRGRP) ? 'r' : '-',
            (mode & S_IWGRP) ? 'w' : '-',
            (mode & S_IXGRP) ? 'x' : '-',
            (mode & S_IROTH) ? 'r' : '-',
            (mode & S_IWOTH) ? 'w' : '-',
            (mode & S_IXOTH) ? 'x' : '-');
}

// Create 2x scaled icon picture
static Picture create_2x_icon(FileIcon *icon) {
    if (!icon) {
        log_error("[WARNING] create_2x_icon called with NULL icon");
        return None;
    }
    
    Display *dpy = itn_core_get_display();
    if (!dpy) {
        log_error("[ERROR] create_2x_icon: NULL display");
        return None;
    }
    
    int size = ICONINFO_ICON_SIZE * 2;

    // Create pixmap for 2x icon with 32-bit depth for ARGB transparency
    // CRITICAL: Pixmap depth MUST match Picture format depth to avoid BadMatch
    Pixmap pixmap = XCreatePixmap(dpy, DefaultRootWindow(dpy),
                                 size, size,
                                 32);  // 32-bit depth for ARGB32 format

    XRenderPictFormat *fmt = XRenderFindStandardFormat(dpy, PictStandardARGB32);
    if (!fmt) {
        log_error("[ERROR] XRenderFindStandardFormat(ARGB32) failed");
        XFreePixmap(dpy, pixmap);
        return None;
    }

    Picture dest = XRenderCreatePicture(dpy, pixmap, fmt, 0, NULL);
    if (dest == None) {
        log_error("[ERROR] XRenderCreatePicture failed for 2x icon");
        XFreePixmap(dpy, pixmap);
        return None;
    }
    
    // Clear with transparent
    XRenderColor clear = {0, 0, 0, 0};
    XRenderFillRectangle(dpy, PictOpSrc, dest, &clear, 0, 0, size, size);
    
    // Just composite the icon without transform - XRender will scale automatically
    Picture src = icon->selected ? icon->selected_picture : icon->normal_picture;
    if (src != None) {
        // Source is ICONINFO_ICON_SIZE, dest is size (2x)
        // This should scale the icon up by 2x
        XRenderComposite(dpy, PictOpOver, src, None, dest,
                        0, 0, 0, 0, 0, 0, size, size);
    } else {
        log_error("[WARNING] Icon has no picture (normal=%ld, selected=%ld)", 
                  (long)icon->normal_picture, (long)icon->selected_picture);
    }
    
    XFreePixmap(dpy, pixmap);
    return dest;
}

// Event handlers
bool iconinfo_handle_key_press(XKeyEvent *event) {
    if (!event) return false;
    
    Canvas *canvas = itn_canvas_find_by_window(event->window);
    if (!canvas) return false;
    
    IconInfoDialog *dialog = get_iconinfo_for_canvas(canvas);
    if (!dialog) return false;
    
    // Route keyboard events to the focused InputField
    bool handled = false;
    
    // Check which field has focus and route the event to it
    if (dialog->name_field && dialog->name_field->has_focus) {
        handled = inputfield_handle_key(dialog->name_field, event);
    } else if (dialog->comment_field && dialog->comment_field->has_focus) {
        // Special handling for Enter key in comment field
        KeySym keysym = XLookupKeysym(event, 0);
        if (keysym == XK_Return || keysym == XK_KP_Enter) {
            // Add comment to listview if not empty
            if (dialog->comment_field->text[0] != '\0' && dialog->comment_list) {
                listview_add_item(dialog->comment_list, dialog->comment_field->text, false, NULL);
                inputfield_set_text(dialog->comment_field, "");  // Clear the field
                handled = true;
            }
        } else {
            handled = inputfield_handle_key(dialog->comment_field, event);
        }
    } else if (dialog->path_field && dialog->path_field->has_focus) {
        handled = inputfield_handle_key(dialog->path_field, event);
    } else if (dialog->app_field && dialog->app_field->has_focus) {
        handled = inputfield_handle_key(dialog->app_field, event);
    }
    
    // Handle Tab key to switch between fields
    if (!handled) {
        KeySym keysym = XLookupKeysym(event, 0);
        if (keysym == XK_Tab) {
            // Find which field has focus and move to the next one
            if (dialog->name_field && dialog->name_field->has_focus) {
                dialog->name_field->has_focus = false;
                if (dialog->comment_field) {
                    dialog->comment_field->has_focus = true;
                    dialog->comment_field->cursor_pos = strlen(dialog->comment_field->text);
                }
            } else if (dialog->comment_field && dialog->comment_field->has_focus) {
                dialog->comment_field->has_focus = false;
                if (dialog->app_field && !dialog->app_field->disabled) {
                    dialog->app_field->has_focus = true;
                    dialog->app_field->cursor_pos = strlen(dialog->app_field->text);
                } else if (dialog->name_field) {
                    // Wrap around if app field is disabled
                    dialog->name_field->has_focus = true;
                    dialog->name_field->cursor_pos = strlen(dialog->name_field->text);
                }
            } else if (dialog->app_field && dialog->app_field->has_focus) {
                dialog->app_field->has_focus = false;
                if (dialog->name_field) {
                    dialog->name_field->has_focus = true;
                    dialog->name_field->cursor_pos = strlen(dialog->name_field->text);
                }
            } else {
                // No field has focus, give it to the first one
                if (dialog->name_field) {
                    dialog->name_field->has_focus = true;
                    dialog->name_field->cursor_pos = strlen(dialog->name_field->text);
                }
            }
            handled = true;
        } else if (keysym == XK_Escape) {
            // Close dialog on Escape
            close_icon_info_dialog(dialog);
            return true;
        }
    }
    
    if (handled) {
        redraw_canvas(canvas);
    }
    
    return handled;
}

bool iconinfo_handle_button_press(XButtonEvent *event) {
    if (!event) return false;
    
    Canvas *canvas = itn_canvas_find_by_window(event->window);
    if (!canvas) return false;
    
    IconInfoDialog *dialog = get_iconinfo_for_canvas(canvas);
    if (!dialog) return false;
    
    
    // Check for "Get Size" button if this is a directory
    if (dialog->get_size_button && dialog->is_directory && 
        !dialog->calculating_size && dialog->size_calc_pid <= 0) {
        // Use the button's own hit testing
        if (button_handle_press(dialog->get_size_button, event->x, event->y)) {
            dialog->get_size_pressed = true;
            redraw_canvas(canvas);
            return true;
        }
    }
    
    // Check OK button using toolkit hit testing
    if (dialog->ok_button && button_handle_press(dialog->ok_button, event->x, event->y)) {
        dialog->ok_pressed = true;
        redraw_canvas(canvas);
        return true;
    }
    
    // Check Cancel button using toolkit hit testing
    if (dialog->cancel_button && button_handle_press(dialog->cancel_button, event->x, event->y)) {
        dialog->cancel_pressed = true;
        redraw_canvas(canvas);
        return true;
    }
    
    // Handle input field clicks for focus and cursor positioning
    bool field_clicked = false;
    
    // Check name field
    if (dialog->name_field && inputfield_handle_click(dialog->name_field, event->x, event->y)) {
        // Remove focus from other fields
        if (dialog->comment_field) dialog->comment_field->has_focus = false;
        if (dialog->path_field) dialog->path_field->has_focus = false;
        if (dialog->app_field) dialog->app_field->has_focus = false;
        field_clicked = true;
    }
    // Check comment field
    else if (dialog->comment_field && inputfield_handle_click(dialog->comment_field, event->x, event->y)) {
        // Remove focus from other fields
        if (dialog->name_field) dialog->name_field->has_focus = false;
        if (dialog->path_field) dialog->path_field->has_focus = false;
        if (dialog->app_field) dialog->app_field->has_focus = false;
        field_clicked = true;
    }
    // Check comment listview for clicks
    else if (dialog->comment_list && listview_handle_click(dialog->comment_list, event->x, event->y, itn_core_get_display(), get_font())) {
        // Get selected item and put it in the comment field for editing
        int selected = dialog->comment_list->selected_index;
        if (selected >= 0 && selected < dialog->comment_list->item_count) {
            inputfield_set_text(dialog->comment_field, dialog->comment_list->items[selected].text);
            // Remove the item from the list since we're editing it
            for (int i = selected; i < dialog->comment_list->item_count - 1; i++) {
                dialog->comment_list->items[i] = dialog->comment_list->items[i + 1];
            }
            dialog->comment_list->item_count--;
            dialog->comment_list->selected_index = -1;
            // Give focus to comment field
            dialog->comment_field->has_focus = true;
            dialog->comment_field->cursor_pos = strlen(dialog->comment_field->text);
        }
        field_clicked = true;
    }
    // Check path field (readonly but can be selected for copying)
    else if (dialog->path_field && inputfield_handle_click(dialog->path_field, event->x, event->y)) {
        // Remove focus from other fields
        if (dialog->name_field) dialog->name_field->has_focus = false;
        if (dialog->comment_field) dialog->comment_field->has_focus = false;
        if (dialog->app_field) dialog->app_field->has_focus = false;
        field_clicked = true;
    }
    // Check app field
    else if (dialog->app_field && inputfield_handle_click(dialog->app_field, event->x, event->y)) {
        // Remove focus from other fields
        if (dialog->name_field) dialog->name_field->has_focus = false;
        if (dialog->comment_field) dialog->comment_field->has_focus = false;
        if (dialog->path_field) dialog->path_field->has_focus = false;
        field_clicked = true;
    }
    // Click outside all fields - remove focus from all
    else {
        if (dialog->name_field) dialog->name_field->has_focus = false;
        if (dialog->comment_field) dialog->comment_field->has_focus = false;
        if (dialog->path_field) dialog->path_field->has_focus = false;
        if (dialog->app_field) dialog->app_field->has_focus = false;
    }
    
    if (field_clicked) {
        redraw_canvas(canvas);
        return true;
    }
    
    return false;
}

bool iconinfo_handle_button_release(XButtonEvent *event) {
    if (!event) return false;
    
    Canvas *canvas = itn_canvas_find_by_window(event->window);
    if (!canvas) return false;
    
    IconInfoDialog *dialog = get_iconinfo_for_canvas(canvas);
    if (!dialog) return false;
    
    // Check for "Get Size" button release
    if (dialog->get_size_pressed && dialog->get_size_button) {
        // Use the button's own hit testing
        if (button_handle_release(dialog->get_size_button, event->x, event->y)) {
            // Start directory size calculation
            dialog->get_size_pressed = false;
            dialog->calculating_size = true;
            snprintf(dialog->size_text, sizeof(dialog->size_text), "Calculating...");
            
            // Start the calculation process
            dialog->size_calc_pid = calculate_directory_size(dialog->icon->path, &dialog->size_pipe_fd);
            if (dialog->size_calc_pid < 0) {
                snprintf(dialog->size_text, sizeof(dialog->size_text), "Error");
                dialog->calculating_size = false;
                log_error("[ERROR] Failed to start directory size calculation");
            }
            
            redraw_canvas(canvas);
            return true;
        }
        
        // Button was pressed but released outside - reset state
        dialog->get_size_pressed = false;
        if (dialog->get_size_button) {
            dialog->get_size_button->pressed = false;
        }
        redraw_canvas(canvas);
        return true;
    }
    
    bool handled = false;
    
    // Check OK button using toolkit hit testing
    if (dialog->ok_pressed && dialog->ok_button) {
        if (button_handle_release(dialog->ok_button, event->x, event->y)) {
            dialog->ok_pressed = false;
            save_file_changes(dialog);
            close_icon_info_dialog(dialog);
            return true;  // Return immediately to avoid use-after-free
        }
        // Button was pressed but released outside
        dialog->ok_pressed = false;
        dialog->ok_button->pressed = false;
        redraw_canvas(canvas);
        handled = true;
    }
    
    // Check Cancel button using toolkit hit testing
    if (dialog->cancel_pressed && dialog->cancel_button) {
        if (button_handle_release(dialog->cancel_button, event->x, event->y)) {
            dialog->cancel_pressed = false;
            close_icon_info_dialog(dialog);
            return true;  // Return immediately to avoid use-after-free
        }
        // Button was pressed but released outside
        dialog->cancel_pressed = false;
        dialog->cancel_button->pressed = false;
        redraw_canvas(canvas);
        handled = true;
    }
    
    return handled;
}

bool iconinfo_handle_motion(XMotionEvent *event) {
    // TODO: Implement motion handling if needed
    return false;
}

// Query functions
bool is_iconinfo_canvas(Canvas *canvas) {
    if (!canvas) return false;
    // Check if this dialog canvas belongs to an iconinfo dialog
    return get_iconinfo_for_canvas(canvas) != NULL;
}

IconInfoDialog* get_iconinfo_for_canvas(Canvas *canvas) {
    if (!canvas) return NULL;
    
    IconInfoDialog *dialog = g_iconinfo_dialogs;
    while (dialog) {
        if (dialog->canvas == canvas) {
            return dialog;
        }
        dialog = dialog->next;
    }
    return NULL;
}

// Cleanup functions
void close_icon_info_dialog(IconInfoDialog *dialog) {
    if (!dialog) return;
    
    
    // Remove from list
    IconInfoDialog **prev = &g_iconinfo_dialogs;
    while (*prev) {
        if (*prev == dialog) {
            *prev = dialog->next;
            break;
        }
        prev = &(*prev)->next;
    }
    
    // Free resources
    if (dialog->icon_2x != None) {
        XRenderFreePicture(itn_core_get_display(), dialog->icon_2x);
    }
    
    // Destroy input fields
    if (dialog->name_field) inputfield_destroy(dialog->name_field);
    if (dialog->comment_field) inputfield_destroy(dialog->comment_field);
    if (dialog->path_field) inputfield_destroy(dialog->path_field);
    if (dialog->app_field) inputfield_destroy(dialog->app_field);
    
    // Destroy buttons
    if (dialog->get_size_button) button_destroy(dialog->get_size_button);
    if (dialog->ok_button) button_destroy(dialog->ok_button);
    if (dialog->cancel_button) button_destroy(dialog->cancel_button);
    
    // Destroy canvas
    if (dialog->canvas) {
        itn_canvas_destroy(dialog->canvas);
    }
    
    free(dialog);
}

// Close dialog by canvas (called from intuition.c when window is closed)
void close_icon_info_dialog_by_canvas(Canvas *canvas) {
    if (!canvas) return;
    
    IconInfoDialog *dialog = get_iconinfo_for_canvas(canvas);
    if (dialog) {
        
        // Remove from list
        IconInfoDialog **prev = &g_iconinfo_dialogs;
        while (*prev) {
            if (*prev == dialog) {
                *prev = dialog->next;
                    break;
            }
            prev = &(*prev)->next;
        }
        
        // Free resources
        if (dialog->icon_2x != None) {
            XRenderFreePicture(itn_core_get_display(), dialog->icon_2x);
        }
        
        // Destroy input fields
        if (dialog->name_field) inputfield_destroy(dialog->name_field);
        if (dialog->comment_field) inputfield_destroy(dialog->comment_field);
        if (dialog->path_field) inputfield_destroy(dialog->path_field);
        if (dialog->app_field) inputfield_destroy(dialog->app_field);
        
        // Don't destroy canvas here - intuition.c will do it
        dialog->canvas = NULL;
        
        free(dialog);
    }
}

void cleanup_all_iconinfo_dialogs(void) {
    while (g_iconinfo_dialogs) {
        IconInfoDialog *next = g_iconinfo_dialogs->next;
        close_icon_info_dialog(g_iconinfo_dialogs);
        g_iconinfo_dialogs = next;
    }
}

// Process monitoring for directory size calculation
void iconinfo_check_size_calculations(void) {
    IconInfoDialog *dialog = g_iconinfo_dialogs;
    while (dialog) {
        if (dialog->calculating_size && dialog->size_calc_pid > 0) {
            // Check if calculation is complete
            off_t size = read_directory_size_result(dialog->size_pipe_fd);
            if (size >= 0) {
                // Calculation complete
                format_file_size(size, dialog->size_text, sizeof(dialog->size_text));
                dialog->calculating_size = false;

                // Reap the child process BEFORE clearing the PID
                int status;
                waitpid(dialog->size_calc_pid, &status, WNOHANG);

                // Now clear the tracking variables
                dialog->size_calc_pid = -1;
                dialog->size_pipe_fd = -1;

                // Redraw to show the result
                if (dialog->canvas) {
                    redraw_canvas(dialog->canvas);
                }
            }
        }
        dialog = dialog->next;
    }
}

// Render the icon info dialog content
void render_iconinfo_content(Canvas *canvas) {
    if (!canvas) return;
    
    IconInfoDialog *dialog = get_iconinfo_for_canvas(canvas);
    if (!dialog) return;
    
    Display *dpy = itn_core_get_display();
    if (!dpy) return;
    
    Picture dest = canvas->canvas_render;
    if (dest == None) return;
    
    // Clear content area to gray
    int content_x = BORDER_WIDTH_LEFT;
    int content_y = BORDER_HEIGHT_TOP;
    int content_w = canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT_CLIENT;
    int content_h = canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM;
    XRenderFillRectangle(dpy, PictOpSrc, dest, &GRAY, content_x, content_y, content_w, content_h);
    
    // Draw icon with sunken frame (reduced to half size)
    int icon_x = content_x + ICONINFO_MARGIN;
    int icon_y = content_y + ICONINFO_MARGIN;
    int icon_size = ICONINFO_ICON_SIZE;  // Changed from * 2 to just the base size (64)
    
    // Draw sunken frame (black top/left, white bottom/right)
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK,
                        icon_x - 1, icon_y - 1, icon_size + 2, 1);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK,
                        icon_x - 1, icon_y - 1, 1, icon_size + 2);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE,
                        icon_x - 1, icon_y + icon_size, icon_size + 2, 1);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE,
                        icon_x + icon_size, icon_y - 1, 1, icon_size + 2);
    
    // Center the icon in the smaller sunken rectangle
    // Most icons are smaller than 64x64, so they'll fit nicely centered
    int centered_x = icon_x + (icon_size - dialog->icon->width) / 2;
    int centered_y = icon_y + (icon_size - dialog->icon->height) / 2;
    
    // If icon is larger than the rectangle, we need to scale or clip
    // For now, we'll just center and let it overflow (could add scaling later)
    if (dialog->icon->width > icon_size || dialog->icon->height > icon_size) {
        // Icon is too big - center it anyway, it will be clipped by the rectangle visually
        centered_x = icon_x + (icon_size - dialog->icon->width) / 2;
        centered_y = icon_y + (icon_size - dialog->icon->height) / 2;
    }
    
    // Render the icon centered
    Picture src = dialog->icon->normal_picture;
    if (src != None) {
        XRenderComposite(dpy, PictOpOver, src, None, dest,
                        0, 0, 0, 0, centered_x, centered_y, 
                        dialog->icon->width, dialog->icon->height);
    }
    
    // Get XftDraw for text rendering
    XftDraw *xft = canvas->xft_draw;
    
    // Layout constants
    int x = ICONINFO_MARGIN + BORDER_WIDTH_LEFT;
    int y = ICONINFO_MARGIN + BORDER_HEIGHT_TOP;
    int field_width = content_w - (2 * ICONINFO_MARGIN);
    
    // Position for text fields (to the right of icon - now using smaller icon)
    int text_x = icon_x + icon_size + ICONINFO_SPACING * 2;
    int text_y = icon_y;
    
    // Draw "Filename:" label and input field
    if (xft) {
        XftColor color;
        XftColorAllocValue(dpy, canvas->visual, canvas->colormap,
                          &(XRenderColor){0, 0, 0, 0xffff}, &color);

        XftFont *font = get_font();
        if (font) {
            XftDrawStringUtf8(xft, &color, font, text_x, text_y + 15,
                             (XftChar8 *)"Filename:", 9);
        }
        XftColorFree(dpy, canvas->visual, canvas->colormap, &color);
    }
    
    // Draw input fields
    if (dialog->name_field) {
        dialog->name_field->x = text_x;
        dialog->name_field->y = text_y + 20;
        dialog->name_field->width = field_width - (text_x - x);
        XftFont *font = get_font();
        inputfield_draw(dialog->name_field, dest, dpy, xft, font);
    }
    
    // Move down for size info
    text_y += 60;
    
    // Draw size text or button
    if (xft) {
        XftColor color;
        XftColorAllocValue(dpy, canvas->visual, canvas->colormap,
                          &(XRenderColor){0, 0, 0, 0xffff}, &color);

        XftFont *font = get_font();
        if (font) {
            // Draw "Size: " label
            XftDrawStringUtf8(xft, &color, font, text_x, text_y,
                             (XftChar8 *)"Size: ", 6);

            // If it's a directory and not calculated yet, draw a button
            if (dialog->is_directory && !dialog->calculating_size && dialog->size_calc_pid <= 0
                && strcmp(dialog->size_text, "[Get Size]") == 0) {
                // Create/update the button struct if needed
                if (!dialog->get_size_button) {
                    XftFont *font = get_font();
                    dialog->get_size_button = button_create(
                        text_x + 50,
                        text_y - 15,
                        70,
                        20,
                        "Get Size",
                        font
                    );
                } else {
                    // Update position in case window was resized or layout changed
                    dialog->get_size_button->x = text_x + 50;
                    dialog->get_size_button->y = text_y - 15;
                    dialog->get_size_button->pressed = dialog->get_size_pressed;
                }

                // Draw the button using toolkit
                button_render(dialog->get_size_button, dest, dpy, xft);
            } else {
                // Draw the size text (file size, calculating, or calculated dir size)
                XftDrawStringUtf8(xft, &color, font, text_x + 50, text_y,
                                 (XftChar8 *)dialog->size_text, strlen(dialog->size_text));
            }
        }
        XftColorFree(dpy, canvas->visual, canvas->colormap, &color);
    }
    
    // Continue with other fields below the icon
    y = icon_y + icon_size + ICONINFO_SPACING * 2;
    
    // Comment field
    if (dialog->comment_field) {
        // Draw label
        if (xft) {
            XftColor color;
            XftColorAllocValue(dpy, canvas->visual, canvas->colormap,
                              &(XRenderColor){0, 0, 0, 0xffff}, &color);

            XftFont *font = get_font();
            if (font) {
                XftDrawStringUtf8(xft, &color, font, x, y + 15,
                                 (XftChar8 *)"Comment:", 8);
            }
            XftColorFree(dpy, canvas->visual, canvas->colormap, &color);
        }
        
        dialog->comment_field->x = x + ICONINFO_LABEL_WIDTH;
        dialog->comment_field->y = y;
        dialog->comment_field->width = field_width - ICONINFO_LABEL_WIDTH;
        XftFont *font = get_font();
        inputfield_draw(dialog->comment_field, dest, dpy, xft, font);
        y += 30;
        
        // Draw comment listview
        if (dialog->comment_list) {
            dialog->comment_list->x = x + ICONINFO_LABEL_WIDTH;
            dialog->comment_list->y = y;
            dialog->comment_list->width = field_width - ICONINFO_LABEL_WIDTH;  // Same width as comment field
            listview_draw(dialog->comment_list, dpy, dest, xft, font);
            y += 85;  // Move past the listview (80px height + 5px spacing)
        }
    }
    
    // Draw permissions, dates, etc.
    if (xft) {
        XftColor color;
        XftColorAllocValue(dpy, canvas->visual, canvas->colormap,
                          &(XRenderColor){0, 0, 0, 0xffff}, &color);

        XftFont *font = get_font();
        if (font) {
            // Permissions
            y += ICONINFO_SPACING;
            // Access (permissions)
            char perm_label[128];
            snprintf(perm_label, sizeof(perm_label), "Access   : %s", dialog->perms_text);
            XftDrawStringUtf8(xft, &color, font, x, y + 15,
                             (XftChar8 *)perm_label, strlen(perm_label));
            y += 25;

            // Owner
            char owner_label[128];
            snprintf(owner_label, sizeof(owner_label), "Owner    : %s", dialog->owner_text);
            XftDrawStringUtf8(xft, &color, font, x, y + 15,
                             (XftChar8 *)owner_label, strlen(owner_label));
            y += 25;

            // Group
            char group_label[128];
            snprintf(group_label, sizeof(group_label), "Group    : %s", dialog->group_text);
            XftDrawStringUtf8(xft, &color, font, x, y + 15,
                             (XftChar8 *)group_label, strlen(group_label));
            y += 25;

            // Created date
            char created_label[128];
            snprintf(created_label, sizeof(created_label), "Created  : %s", dialog->created_text);
            XftDrawStringUtf8(xft, &color, font, x, y + 15,
                             (XftChar8 *)created_label, strlen(created_label));
            y += 25;

            // Modified date
            char modified_label[128];
            snprintf(modified_label, sizeof(modified_label), "Modified : %s", dialog->modified_text);
            XftDrawStringUtf8(xft, &color, font, x, y + 15,
                             (XftChar8 *)modified_label, strlen(modified_label));
            y += 25;
        }
        XftColorFree(dpy, canvas->visual, canvas->colormap, &color);
    }
    
    // Filepath (as input field showing directory only)
    if (dialog->path_field) {
        if (xft) {
            XftColor color;
            XftColorAllocValue(dpy, canvas->visual, canvas->colormap,
                              &(XRenderColor){0, 0, 0, 0xffff}, &color);

            XftFont *font = get_font();
            if (font) {
                XftDrawStringUtf8(xft, &color, font, x, y + 15,
                                 (XftChar8 *)"Filepath", 8);
            }
            XftColorFree(dpy, canvas->visual, canvas->colormap, &color);
        }
        
        // Position field at same X as Filename field, Y aligned with label
        dialog->path_field->x = x + ICONINFO_LABEL_WIDTH;
        dialog->path_field->y = y;
        dialog->path_field->width = field_width - ICONINFO_LABEL_WIDTH;
        XftFont *font = get_font();
        inputfield_draw(dialog->path_field, dest, dpy, xft, font);
        y += 25;
    }
    
    // App field (Run with)
    if (dialog->app_field) {
        if (xft) {
            XftColor color;
            XftColorAllocValue(dpy, canvas->visual, canvas->colormap,
                              &(XRenderColor){0, 0, 0, 0xffff}, &color);

            XftFont *font = get_font();
            if (font) {
                XftDrawStringUtf8(xft, &color, font, x, y + 15,
                                 (XftChar8 *)"Run with", 8);
            }
            XftColorFree(dpy, canvas->visual, canvas->colormap, &color);
        }
        
        // Position field at same X as Filename field, Y aligned with label
        dialog->app_field->x = x + ICONINFO_LABEL_WIDTH;
        dialog->app_field->y = y;
        dialog->app_field->width = field_width - ICONINFO_LABEL_WIDTH;
        XftFont *font = get_font();
        inputfield_draw(dialog->app_field, dest, dpy, xft, font);
        y += 25;
    }
    
    // Draw OK and Cancel buttons at bottom
    int button_y = canvas->height - BORDER_HEIGHT_BOTTOM - ICONINFO_BUTTON_HEIGHT - ICONINFO_MARGIN;
    int ok_x = canvas->width / 2 - ICONINFO_BUTTON_WIDTH - 20;
    int cancel_x = canvas->width / 2 + 20;
    
    // Create/update OK button
    if (!dialog->ok_button) {
        XftFont *font = get_font();
        dialog->ok_button = button_create(ok_x, button_y,
                                         ICONINFO_BUTTON_WIDTH, ICONINFO_BUTTON_HEIGHT, "OK", font);
    } else {
        // Update position in case window was resized
        dialog->ok_button->x = ok_x;
        dialog->ok_button->y = button_y;
        dialog->ok_button->pressed = dialog->ok_pressed;
    }
    
    // Create/update Cancel button
    if (!dialog->cancel_button) {
        XftFont *font = get_font();
        dialog->cancel_button = button_create(cancel_x, button_y,
                                             ICONINFO_BUTTON_WIDTH, ICONINFO_BUTTON_HEIGHT, "Cancel", font);
    } else {
        // Update position in case window was resized
        dialog->cancel_button->x = cancel_x;
        dialog->cancel_button->y = button_y;
        dialog->cancel_button->pressed = dialog->cancel_pressed;
    }
    
    XftFont *font = get_font();
    if (font) {
        button_render(dialog->ok_button, dest, dpy, xft);
        button_render(dialog->cancel_button, dest, dpy, xft);
    }
}
