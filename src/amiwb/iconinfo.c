// File: iconinfo.c
// Icon Information dialog implementation
#include "iconinfo.h"
#include "render.h"
#include "config.h"
#include "workbench.h"
#include "../toolkit/button.h"
#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>
#include <X11/Xft/Xft.h>
#include <X11/keysym.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <pwd.h>
#include <grp.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

// Global dialog list
static IconInfoDialog *g_iconinfo_dialogs = NULL;

// External functions
extern XftFont *get_font(void);

// Forward declarations
static void load_file_info(IconInfoDialog *dialog);
static Picture create_2x_icon(FileIcon *icon);
static void format_file_size(off_t size, char *buffer, size_t bufsize);
static void format_permissions(mode_t mode, char *buffer, size_t bufsize);

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
    char title[256];
    snprintf(title, sizeof(title), "%s Information", icon->label);
    dialog->canvas->title_base = strdup(title);
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
    dialog->name_field = inputfield_create(field_x, y_pos, field_width, 20);
    if (dialog->name_field) {
        inputfield_set_text(dialog->name_field, icon->label);
    } else {
        log_error("[WARNING] Failed to create name field");
    }
    
    y_pos = BORDER_HEIGHT_TOP + dialog->icon_display_size + 40;  // Below icon
    
    // Comment field (editable) - same x alignment as name field
    dialog->comment_field = inputfield_create(field_x,  // Same x as name field and "Size:" 
                                            y_pos, 
                                            field_width,  // Same width as name field
                                            20);
    if (dialog->comment_field) {
        inputfield_set_text(dialog->comment_field, "comment");
    }
    
    y_pos = 365;  // Align with "Path:" label baseline (378 - ~13 for ascent)
    
    // Path field (read-only but scrollable) - same alignment as above fields
    dialog->path_field = inputfield_create(field_x,
                                          y_pos,
                                          field_width,
                                          20);
    if (dialog->path_field) {
        inputfield_set_disabled(dialog->path_field, true);
    }
    
    y_pos = 395;  // Align with "Opens with:" label baseline (408 - ~13 for ascent)
    
    // Opens with field (editable) - same alignment as above fields
    dialog->app_field = inputfield_create(field_x,
                                         y_pos,
                                         field_width,
                                         20);
    
    // Load file information
    load_file_info(dialog);
    
    // Add to dialog list
    dialog->next = g_iconinfo_dialogs;
    g_iconinfo_dialogs = dialog;
    
    // Show the dialog
    XMapRaised(get_display(), dialog->canvas->win);
    set_active_window(dialog->canvas);
    
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
            strcpy(dialog->size_text, "[Get Size]");
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
        
        // Format dates
        struct tm *tm;
        
        // Creation time (birth time) - not available on all filesystems
        // Using ctime (change time) as fallback
        tm = localtime(&st.st_ctime);
        strftime(dialog->created_text, sizeof(dialog->created_text), 
                "%Y-%m-%d %H:%M:%S", tm);
        
        // Modification time
        tm = localtime(&st.st_mtime);
        strftime(dialog->modified_text, sizeof(dialog->modified_text),
                "%Y-%m-%d %H:%M:%S", tm);
        
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
    
    // Get path (directory part)
    char *path_copy = strdup(dialog->icon->path);
    char *last_slash = strrchr(path_copy, '/');
    if (last_slash && last_slash != path_copy) {
        *last_slash = '\0';
        if (dialog->path_field) {
            inputfield_set_text(dialog->path_field, path_copy);
            inputfield_scroll_to_end(dialog->path_field);
        }
    }
    free(path_copy);
    
    // Try to read comment from extended attributes
    char comment[256] = {0};
    ssize_t len = getxattr(dialog->icon->path, "user.comment", comment, sizeof(comment) - 1);
    if (len > 0) {
        comment[len] = '\0';
        if (dialog->comment_field) {
            inputfield_set_text(dialog->comment_field, comment);
        }
    }
    
    // TODO: Get default application using xdg-mime
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
    
    Display *dpy = get_display();
    if (!dpy) {
        log_error("[ERROR] create_2x_icon: NULL display");
        return None;
    }
    
    int size = ICONINFO_ICON_SIZE * 2;
    
    // Create pixmap for 2x icon
    Pixmap pixmap = XCreatePixmap(dpy, DefaultRootWindow(dpy), 
                                 size, size, 
                                 DefaultDepth(dpy, DefaultScreen(dpy)));
    
    Picture dest = XRenderCreatePicture(dpy, pixmap,
                                       XRenderFindStandardFormat(dpy, PictStandardARGB32),
                                       0, NULL);
    
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
    // TODO: Implement keyboard handling (Tab, Enter, Escape)
    return false;
}

bool iconinfo_handle_button_press(XButtonEvent *event) {
    if (!event) return false;
    
    Canvas *canvas = find_canvas(event->window);
    if (!canvas) return false;
    
    IconInfoDialog *dialog = get_iconinfo_for_canvas(canvas);
    if (!dialog) return false;
    
    // Calculate button positions (same as in render)
    int button_y = canvas->height - BORDER_HEIGHT_BOTTOM - ICONINFO_MARGIN - ICONINFO_BUTTON_HEIGHT;
    int ok_x = canvas->width / 2 - ICONINFO_BUTTON_WIDTH - 20;
    int cancel_x = canvas->width / 2 + 20;
    
    // Check if click is on OK button
    if (event->x >= ok_x && event->x < ok_x + ICONINFO_BUTTON_WIDTH &&
        event->y >= button_y && event->y < button_y + ICONINFO_BUTTON_HEIGHT) {
        dialog->ok_pressed = true;
        redraw_canvas(canvas);
        return true;
    }
    
    // Check if click is on Cancel button
    if (event->x >= cancel_x && event->x < cancel_x + ICONINFO_BUTTON_WIDTH &&
        event->y >= button_y && event->y < button_y + ICONINFO_BUTTON_HEIGHT) {
        dialog->cancel_pressed = true;
        redraw_canvas(canvas);
        return true;
    }
    
    // TODO: Handle input field clicks
    
    return false;
}

bool iconinfo_handle_button_release(XButtonEvent *event) {
    if (!event) return false;
    
    Canvas *canvas = find_canvas(event->window);
    if (!canvas) return false;
    
    IconInfoDialog *dialog = get_iconinfo_for_canvas(canvas);
    if (!dialog) return false;
    
    // Calculate button positions (same as in render)
    int button_y = canvas->height - BORDER_HEIGHT_BOTTOM - ICONINFO_MARGIN - ICONINFO_BUTTON_HEIGHT;
    int ok_x = canvas->width / 2 - ICONINFO_BUTTON_WIDTH - 20;
    int cancel_x = canvas->width / 2 + 20;
    
    bool handled = false;
    
    // Check if release is on OK button while it was pressed
    if (dialog->ok_pressed && 
        event->x >= ok_x && event->x < ok_x + ICONINFO_BUTTON_WIDTH &&
        event->y >= button_y && event->y < button_y + ICONINFO_BUTTON_HEIGHT) {
        dialog->ok_pressed = false;
        // TODO: save_file_changes(dialog);
        close_icon_info_dialog(dialog);
        return true;  // Return immediately to avoid use-after-free
    }
    
    // Check if release is on Cancel button while it was pressed
    else if (dialog->cancel_pressed && 
             event->x >= cancel_x && event->x < cancel_x + ICONINFO_BUTTON_WIDTH &&
             event->y >= button_y && event->y < button_y + ICONINFO_BUTTON_HEIGHT) {
        dialog->cancel_pressed = false;
        close_icon_info_dialog(dialog);
        return true;  // Return immediately to avoid use-after-free
    }
    
    // Reset button states if we had any pressed
    if (dialog->ok_pressed || dialog->cancel_pressed) {
        dialog->ok_pressed = false;
        dialog->cancel_pressed = false;
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
    
    log_error("[DEBUG] Closing iconinfo dialog, canvas=%p", dialog->canvas);
    
    // Remove from list
    IconInfoDialog **prev = &g_iconinfo_dialogs;
    while (*prev) {
        if (*prev == dialog) {
            *prev = dialog->next;
            log_error("[DEBUG] Removed iconinfo dialog from list");
            break;
        }
        prev = &(*prev)->next;
    }
    
    // Free resources
    if (dialog->icon_2x != None) {
        XRenderFreePicture(get_display(), dialog->icon_2x);
    }
    
    // Destroy input fields
    if (dialog->name_field) inputfield_destroy(dialog->name_field);
    if (dialog->comment_field) inputfield_destroy(dialog->comment_field);
    if (dialog->path_field) inputfield_destroy(dialog->path_field);
    if (dialog->app_field) inputfield_destroy(dialog->app_field);
    
    // Destroy canvas
    if (dialog->canvas) {
        destroy_canvas(dialog->canvas);
    }
    
    free(dialog);
}

// Close dialog by canvas (called from intuition.c when window is closed)
void close_icon_info_dialog_by_canvas(Canvas *canvas) {
    if (!canvas) return;
    
    IconInfoDialog *dialog = get_iconinfo_for_canvas(canvas);
    if (dialog) {
        log_error("[DEBUG] Closing iconinfo via window close button, canvas=%p", canvas);
        
        // Remove from list
        IconInfoDialog **prev = &g_iconinfo_dialogs;
        while (*prev) {
            if (*prev == dialog) {
                *prev = dialog->next;
                log_error("[DEBUG] Removed iconinfo dialog from list");
                break;
            }
            prev = &(*prev)->next;
        }
        
        // Free resources
        if (dialog->icon_2x != None) {
            XRenderFreePicture(get_display(), dialog->icon_2x);
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
    // TODO: Implement directory size calculation monitoring
}

// Render the icon info dialog content
void render_iconinfo_content(Canvas *canvas) {
    if (!canvas) return;
    
    IconInfoDialog *dialog = get_iconinfo_for_canvas(canvas);
    if (!dialog) return;
    
    Display *dpy = get_display();
    if (!dpy) return;
    
    Picture dest = canvas->canvas_render;
    if (dest == None) return;
    
    // Clear content area to gray
    int content_x = BORDER_WIDTH_LEFT;
    int content_y = BORDER_HEIGHT_TOP;
    int content_w = canvas->width - BORDER_WIDTH_LEFT - get_right_border_width(canvas);
    int content_h = canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM;
    XRenderFillRectangle(dpy, PictOpSrc, dest, &GRAY, content_x, content_y, content_w, content_h);
    
    // Draw 2x icon with sunken frame
    int icon_x = content_x + ICONINFO_MARGIN;
    int icon_y = content_y + ICONINFO_MARGIN;
    int icon_size = ICONINFO_ICON_SIZE * 2;
    
    // Draw sunken frame (black top/left, white bottom/right)
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK,
                        icon_x - 1, icon_y - 1, icon_size + 2, 1);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK,
                        icon_x - 1, icon_y - 1, 1, icon_size + 2);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE,
                        icon_x - 1, icon_y + icon_size, icon_size + 2, 1);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE,
                        icon_x + icon_size, icon_y - 1, 1, icon_size + 2);
    
    // Just render the icon normally like workbench does
    Picture src = dialog->icon->normal_picture;
    if (src != None) {
        XRenderComposite(dpy, PictOpOver, src, None, dest,
                        0, 0, 0, 0, icon_x, icon_y, 
                        dialog->icon->width, dialog->icon->height);
    }
    
    // Get XftDraw for text rendering
    XftDraw *xft = canvas->xft_draw;
    
    // Layout constants
    int x = ICONINFO_MARGIN + BORDER_WIDTH_LEFT;
    int y = ICONINFO_MARGIN + BORDER_HEIGHT_TOP;
    int field_width = content_w - (2 * ICONINFO_MARGIN);
    
    // Position for text fields (to the right of icon)
    int text_x = icon_x + icon_size + ICONINFO_SPACING * 2;
    int text_y = icon_y;
    
    // Draw "Filename:" label and input field
    if (xft) {
        XftColor color;
        XftColorAllocValue(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                          DefaultColormap(dpy, DefaultScreen(dpy)),
                          &(XRenderColor){0, 0, 0, 0xffff}, &color);
        
        XftFont *font = get_font();
        if (font) {
            XftDrawStringUtf8(xft, &color, font, text_x, text_y + 15,
                             (XftChar8 *)"Filename:", 9);
        }
        XftColorFree(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                    DefaultColormap(dpy, DefaultScreen(dpy)), &color);
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
    
    // Draw size text
    if (xft) {
        XftColor color;
        XftColorAllocValue(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                          DefaultColormap(dpy, DefaultScreen(dpy)),
                          &(XRenderColor){0, 0, 0, 0xffff}, &color);
        
        XftFont *font = get_font();
        if (font) {
            char size_label[128];
            snprintf(size_label, sizeof(size_label), "Size: %s", dialog->size_text);
            XftDrawStringUtf8(xft, &color, font, text_x, text_y,
                             (XftChar8 *)size_label, strlen(size_label));
        }
        XftColorFree(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                    DefaultColormap(dpy, DefaultScreen(dpy)), &color);
    }
    
    // Continue with other fields below the icon
    y = icon_y + icon_size + ICONINFO_SPACING * 2;
    
    // Comment field
    if (dialog->comment_field) {
        // Draw label
        if (xft) {
            XftColor color;
            XftColorAllocValue(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                              DefaultColormap(dpy, DefaultScreen(dpy)),
                              &(XRenderColor){0, 0, 0, 0xffff}, &color);
            
            XftFont *font = get_font();
            if (font) {
                XftDrawStringUtf8(xft, &color, font, x, y + 15,
                                 (XftChar8 *)"Comment:", 8);
            }
            XftColorFree(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                        DefaultColormap(dpy, DefaultScreen(dpy)), &color);
        }
        
        dialog->comment_field->x = x + ICONINFO_LABEL_WIDTH;
        dialog->comment_field->y = y;
        dialog->comment_field->width = field_width - ICONINFO_LABEL_WIDTH;
        XftFont *font = get_font();
        inputfield_draw(dialog->comment_field, dest, dpy, xft, font);
        y += 30;
    }
    
    // Draw permissions, dates, etc.
    if (xft) {
        XftColor color;
        XftColorAllocValue(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                          DefaultColormap(dpy, DefaultScreen(dpy)),
                          &(XRenderColor){0, 0, 0, 0xffff}, &color);
        
        XftFont *font = get_font();
        if (font) {
            // Permissions
            y += ICONINFO_SPACING;
            char perm_label[128];
            snprintf(perm_label, sizeof(perm_label), "Permissions: %s", dialog->perms_text);
            XftDrawStringUtf8(xft, &color, font, x, y + 15,
                             (XftChar8 *)perm_label, strlen(perm_label));
            
            // Owner and Group on same line
            char owner_label[128];
            snprintf(owner_label, sizeof(owner_label), "    Owner: %s  Group: %s", 
                    dialog->owner_text, dialog->group_text);
            XftDrawStringUtf8(xft, &color, font, x, y + 35,
                             (XftChar8 *)owner_label, strlen(owner_label));
            y += 50;
            
            // Created date
            char created_label[128];
            snprintf(created_label, sizeof(created_label), "Created:  %s", dialog->created_text);
            XftDrawStringUtf8(xft, &color, font, x, y + 15,
                             (XftChar8 *)created_label, strlen(created_label));
            y += 25;
            
            // Modified date
            char modified_label[128];
            snprintf(modified_label, sizeof(modified_label), "Modified: %s", dialog->modified_text);
            XftDrawStringUtf8(xft, &color, font, x, y + 15,
                             (XftChar8 *)modified_label, strlen(modified_label));
            y += 35;
        }
        XftColorFree(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                    DefaultColormap(dpy, DefaultScreen(dpy)), &color);
    }
    
    // Path field
    if (dialog->path_field) {
        if (xft) {
            XftColor color;
            XftColorAllocValue(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                              DefaultColormap(dpy, DefaultScreen(dpy)),
                              &(XRenderColor){0, 0, 0, 0xffff}, &color);
            
            XftFont *font = get_font();
            if (font) {
                XftDrawStringUtf8(xft, &color, font, x, y + 15,
                                 (XftChar8 *)"Path:", 5);
            }
            XftColorFree(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                        DefaultColormap(dpy, DefaultScreen(dpy)), &color);
        }
        
        dialog->path_field->x = x + ICONINFO_LABEL_WIDTH;
        dialog->path_field->y = y;
        dialog->path_field->width = field_width - ICONINFO_LABEL_WIDTH;
        XftFont *font = get_font();
        inputfield_draw(dialog->path_field, dest, dpy, xft, font);
        y += 35;
    }
    
    // App field
    if (dialog->app_field) {
        if (xft) {
            XftColor color;
            XftColorAllocValue(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                              DefaultColormap(dpy, DefaultScreen(dpy)),
                              &(XRenderColor){0, 0, 0, 0xffff}, &color);
            
            XftFont *font = get_font();
            if (font) {
                XftDrawStringUtf8(xft, &color, font, x, y + 15,
                                 (XftChar8 *)"Opens with:", 11);
            }
            XftColorFree(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                        DefaultColormap(dpy, DefaultScreen(dpy)), &color);
        }
        
        dialog->app_field->x = x + ICONINFO_LABEL_WIDTH;
        dialog->app_field->y = y;
        dialog->app_field->width = field_width - ICONINFO_LABEL_WIDTH;
        XftFont *font = get_font();
        inputfield_draw(dialog->app_field, dest, dpy, xft, font);
        y += 40;
    }
    
    // Draw OK and Cancel buttons at bottom
    int button_y = canvas->height - BORDER_HEIGHT_BOTTOM - ICONINFO_BUTTON_HEIGHT - ICONINFO_MARGIN;
    int ok_x = canvas->width / 2 - ICONINFO_BUTTON_WIDTH - 20;
    int cancel_x = canvas->width / 2 + 20;
    
    // Create temporary button structures for drawing
    Button ok_btn = {
        .x = ok_x, .y = button_y,
        .width = ICONINFO_BUTTON_WIDTH, .height = ICONINFO_BUTTON_HEIGHT,
        .label = "OK",
        .pressed = dialog->ok_pressed
    };
    
    Button cancel_btn = {
        .x = cancel_x, .y = button_y,
        .width = ICONINFO_BUTTON_WIDTH, .height = ICONINFO_BUTTON_HEIGHT,
        .label = "Cancel",
        .pressed = dialog->cancel_pressed
    };
    
    XftFont *font = get_font();
    if (font) {
        button_draw(&ok_btn, dest, dpy, xft, font);
        button_draw(&cancel_btn, dest, dpy, xft, font);
    }
}
