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

// Forward declarations
static void load_file_info(IconInfoDialog *dialog);
static void save_file_changes(IconInfoDialog *dialog);
static void draw_scaled_icon(IconInfoDialog *dialog, Picture dest, Display *dpy);
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
    
    log_error("[INFO] Opening Icon Information for: %s", icon->label);
    
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
        log_error("[INFO] Created name field at %d,%d size %dx20", field_x, y_pos, field_width);
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
    log_error("[INFO] Creating 2x icon, size: %d", size);
    
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
    
    // Set up 2x scaling transform
    XTransform transform = {
        {{XDoubleToFixed(2.0), XDoubleToFixed(0.0), XDoubleToFixed(0.0)},
         {XDoubleToFixed(0.0), XDoubleToFixed(2.0), XDoubleToFixed(0.0)},
         {XDoubleToFixed(0.0), XDoubleToFixed(0.0), XDoubleToFixed(1.0)}}
    };
    
    // Apply transform and composite icon
    Picture src = icon->selected ? icon->selected_picture : icon->normal_picture;
    if (src != None) {
        log_error("[INFO] Using %s picture for 2x icon", icon->selected ? "selected" : "normal");
        XRenderSetPictureTransform(dpy, src, &transform);
        XRenderComposite(dpy, PictOpOver, src, None, dest,
                        0, 0, 0, 0, 0, 0, size, size);
        
        // Reset transform
        XTransform identity = {
            {{XDoubleToFixed(1.0), XDoubleToFixed(0.0), XDoubleToFixed(0.0)},
             {XDoubleToFixed(0.0), XDoubleToFixed(1.0), XDoubleToFixed(0.0)},
             {XDoubleToFixed(0.0), XDoubleToFixed(0.0), XDoubleToFixed(1.0)}}
        };
        XRenderSetPictureTransform(dpy, src, &identity);
    } else {
        log_error("[WARNING] Icon has no picture (normal=%ld, selected=%ld)", 
                  (long)icon->normal_picture, (long)icon->selected_picture);
    }
    
    XFreePixmap(dpy, pixmap);
    return dest;
}

// Render icon info dialog content
void render_iconinfo_content(Canvas *canvas) {
    if (!canvas) {
        log_error("[ERROR] render_iconinfo_content called with NULL canvas");
        return;
    }
    
    IconInfoDialog *dialog = get_iconinfo_for_canvas(canvas);
    if (!dialog) {
        log_error("[WARNING] render_iconinfo_content: no dialog found for canvas");
        return;
    }
    
    log_error("[INFO] Rendering icon info dialog for: %s", dialog->icon ? dialog->icon->label : "unknown");
    
    Display *dpy = get_display();
    if (!dpy) {
        log_error("[ERROR] render_iconinfo_content: NULL display");
        return;
    }
    
    // Use the canvas's render target
    Picture dest = canvas->canvas_render;
    
    // Clear only the content area inside the borders to dialog gray
    int content_x = BORDER_WIDTH_LEFT;
    int content_y = BORDER_HEIGHT_TOP;
    int content_w = canvas->width - BORDER_WIDTH_LEFT - get_right_border_width(canvas);
    int content_h = canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM;
    XRenderColor gray = GRAY;
    XRenderFillRectangle(dpy, PictOpSrc, dest, &gray, content_x, content_y, content_w, content_h);
    
    // Create XftDraw for text rendering on the canvas buffer
    XftDraw *xft_draw = XftDrawCreate(dpy, canvas->canvas_buffer,
                                     DefaultVisual(dpy, DefaultScreen(dpy)),
                                     DefaultColormap(dpy, DefaultScreen(dpy)));
    if (!xft_draw) {
        log_error("[ERROR] Failed to create XftDraw in render_iconinfo_content");
        return;
    }
    
    // Get font from render context
    XftFont *font = get_font();
    if (!font) {
        log_error("[ERROR] No font available in render_iconinfo_content");
        XftDrawDestroy(xft_draw);
        return;
    }
    
    // Draw 2x icon with sunken frame (adjust for borders)
    int icon_x = content_x + ICONINFO_MARGIN;
    int icon_y = content_y + ICONINFO_MARGIN;
    
    // Draw sunken frame (black top/left, white bottom/right)
    XRenderColor black = BLACK;
    XRenderColor white = WHITE;
    
    // Top and left borders (black)
    XRenderFillRectangle(dpy, PictOpSrc, dest, &black,
                        icon_x - 1, icon_y - 1, 
                        dialog->icon_display_size + 2, 1);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &black,
                        icon_x - 1, icon_y - 1,
                        1, dialog->icon_display_size + 2);
    
    // Bottom and right borders (white)
    XRenderFillRectangle(dpy, PictOpSrc, dest, &white,
                        icon_x - 1, icon_y + dialog->icon_display_size,
                        dialog->icon_display_size + 2, 1);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &white,
                        icon_x + dialog->icon_display_size, icon_y - 1,
                        1, dialog->icon_display_size + 2);
    
    // Draw the 2x icon
    if (dialog->icon_2x != None) {
        log_error("[INFO] Drawing 2x icon at %d,%d size %d", icon_x, icon_y, dialog->icon_display_size);
        XRenderComposite(dpy, PictOpOver, dialog->icon_2x, None, dest,
                        0, 0, 0, 0, icon_x, icon_y,
                        dialog->icon_display_size, dialog->icon_display_size);
    } else {
        log_error("[WARNING] No 2x icon available for rendering");
    }
    
    // Draw labels
    XftColor text_color;
    XRenderColor text_render = BLACK;
    XftColorAllocValue(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                      DefaultColormap(dpy, DefaultScreen(dpy)),
                      &text_render, &text_color);
    
    int label_x = content_x + ICONINFO_MARGIN;
    int y = content_y + ICONINFO_MARGIN;
    
    // Filename label
    XftDrawStringUtf8(xft_draw, &text_color, font,
                     label_x + dialog->icon_display_size + 20, y + font->ascent,
                     (FcChar8*)"Filename:", 9);
    
    // Size label
    y += 30;
    XftDrawStringUtf8(xft_draw, &text_color, font,
                     label_x + dialog->icon_display_size + 20, y + font->ascent,
                     (FcChar8*)"Size:", 5);
    
    // Size value or button
    XftDrawStringUtf8(xft_draw, &text_color, font,
                     label_x + dialog->icon_display_size + 100, y + font->ascent,
                     (FcChar8*)dialog->size_text, strlen(dialog->size_text));
    
    // Move below icon for more fields
    y = icon_y + dialog->icon_display_size + 20;
    
    // Comment label
    XftDrawStringUtf8(xft_draw, &text_color, font,
                     label_x, y + font->ascent,
                     (FcChar8*)"Comment:", 8);
    
    y += 30;
    
    // Permissions label
    XftDrawStringUtf8(xft_draw, &text_color, font,
                     label_x, y + font->ascent,
                     (FcChar8*)"Permissions:", 12);
    
    // Draw permission checkboxes mockup - stacked in 2 columns of 3 each
    y += 25;  // Move to next line after "Permissions:"
    int perm_x = label_x;  // Same x as "Permissions:" label
    
    // First column (Owner permissions)
    XftDrawStringUtf8(xft_draw, &text_color, font,
                     perm_x, y + font->ascent,
                     (FcChar8*)"[X] Owner Read", 14);
    y += 20;
    XftDrawStringUtf8(xft_draw, &text_color, font,
                     perm_x, y + font->ascent,
                     (FcChar8*)"[X] Owner Write", 15);
    y += 20;
    XftDrawStringUtf8(xft_draw, &text_color, font,
                     perm_x, y + font->ascent,
                     (FcChar8*)"[ ] Owner Execute", 17);
    
    // Second column (Group/Other permissions) - same Y positions as first column
    y -= 40;  // Go back to start of permissions
    int perm_x2 = perm_x + 150;  // Second column offset
    XftDrawStringUtf8(xft_draw, &text_color, font,
                     perm_x2, y + font->ascent,
                     (FcChar8*)"[ ] Group Read", 14);
    y += 20;
    XftDrawStringUtf8(xft_draw, &text_color, font,
                     perm_x2, y + font->ascent,
                     (FcChar8*)"[ ] Group Write", 15);
    y += 20;
    XftDrawStringUtf8(xft_draw, &text_color, font,
                     perm_x2, y + font->ascent,
                     (FcChar8*)"[ ] Other All", 13);
    
    y += 60;
    
    // Created label and value
    XftDrawStringUtf8(xft_draw, &text_color, font,
                     label_x, y + font->ascent,
                     (FcChar8*)"Created:", 8);
    XftDrawStringUtf8(xft_draw, &text_color, font,
                     label_x + ICONINFO_LABEL_WIDTH, y + font->ascent,
                     (FcChar8*)dialog->created_text, strlen(dialog->created_text));
    
    y += 25;
    
    // Modified label and value
    XftDrawStringUtf8(xft_draw, &text_color, font,
                     label_x, y + font->ascent,
                     (FcChar8*)"Modified:", 9);
    XftDrawStringUtf8(xft_draw, &text_color, font,
                     label_x + ICONINFO_LABEL_WIDTH, y + font->ascent,
                     (FcChar8*)dialog->modified_text, strlen(dialog->modified_text));
    
    y += 25;
    
    // Path label
    XftDrawStringUtf8(xft_draw, &text_color, font,
                     label_x, y + font->ascent,
                     (FcChar8*)"Path:", 5);
    
    y += 30;
    
    // Opens with label
    XftDrawStringUtf8(xft_draw, &text_color, font,
                     label_x, y + font->ascent,
                     (FcChar8*)"Opens with:", 11);
    
    // Draw input fields
    if (dialog->name_field) {
        inputfield_draw(dialog->name_field, dest, dpy, xft_draw, font);
    }
    if (dialog->comment_field) {
        inputfield_draw(dialog->comment_field, dest, dpy, xft_draw, font);
    }
    if (dialog->path_field) {
        inputfield_draw(dialog->path_field, dest, dpy, xft_draw, font);
    }
    if (dialog->app_field) {
        inputfield_draw(dialog->app_field, dest, dpy, xft_draw, font);
    }
    
    // Draw buttons (adjusted for window borders)
    int button_y = canvas->height - BORDER_HEIGHT_BOTTOM - ICONINFO_MARGIN - ICONINFO_BUTTON_HEIGHT;
    int ok_x = canvas->width / 2 - ICONINFO_BUTTON_WIDTH - 20;
    int cancel_x = canvas->width / 2 + 20;
    
    Button ok_btn = {
        .x = ok_x, .y = button_y,
        .width = ICONINFO_BUTTON_WIDTH, .height = ICONINFO_BUTTON_HEIGHT,
        .label = "OK",
        .pressed = dialog->ok_pressed
    };
    button_draw(&ok_btn, dest, dpy, xft_draw, font);
    
    Button cancel_btn = {
        .x = cancel_x, .y = button_y,
        .width = ICONINFO_BUTTON_WIDTH, .height = ICONINFO_BUTTON_HEIGHT,
        .label = "Cancel",
        .pressed = dialog->cancel_pressed
    };
    button_draw(&cancel_btn, dest, dpy, xft_draw, font);
    
    // Cleanup
    XftColorFree(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                DefaultColormap(dpy, DefaultScreen(dpy)), &text_color);
    XftDrawDestroy(xft_draw);
}

// Event handlers
bool iconinfo_handle_key_press(XKeyEvent *event) {
    // TODO: Implement keyboard handling
    return false;
}

bool iconinfo_handle_button_press(XButtonEvent *event) {
    // TODO: Implement button press handling
    return false;
}

bool iconinfo_handle_button_release(XButtonEvent *event) {
    // TODO: Implement button release handling
    return false;
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
