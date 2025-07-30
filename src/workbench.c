// File: workbench.c
#include "workbench.h"
#include "render.h"
#include "config.h"  // Added to include config.h for max/min macros
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>

#define INITIAL_ICON_CAPACITY 16

// Global state for workbench
static FileIcon **icon_array = NULL;        // Dynamic array of all icons
static int icon_count = 0;                  // Current number of icons
static int icon_array_size = 0;             // Allocated size of icon array
static FileIcon *dragged_icon = NULL;       // Moved from events.c
static int drag_start_x, drag_start_y;      // Moved from events.c


// =========================
// Manage dynamic icon array 
// =========================

// (add or remove icons)
FileIcon *manage_icons(bool add, FileIcon *icon_to_remove) {
    if (add) {
        if (icon_count >= icon_array_size) {
            icon_array_size = icon_array_size ? icon_array_size * 2 : INITIAL_ICON_CAPACITY;
            FileIcon **new_icons = realloc(icon_array, icon_array_size * sizeof(FileIcon *));
            if (!new_icons) return NULL;
            icon_array = new_icons;
        }
        FileIcon *new_icon = malloc(sizeof(FileIcon));
        if (!new_icon) return NULL;
        icon_array[icon_count++] = new_icon;
        return new_icon;
    } else if (icon_to_remove) {
        for (int i = 0; i < icon_count; i++) {
            if (icon_array[i] == icon_to_remove) {
                memmove(&icon_array[i], &icon_array[i + 1], (icon_count - i - 1) * sizeof(FileIcon *));
                icon_count--;
                break;
            }
        }
    }
    return NULL;
}

// ===============
// start workbench
// ===============

// Initialize icon array and setup desktop icons
void init_workbench(void) {
    icon_array = malloc(INITIAL_ICON_CAPACITY * sizeof(FileIcon *));
    if (!icon_array) return;
    icon_array_size = INITIAL_ICON_CAPACITY;
    icon_count = 0;

    // Create "System" root fs icon
    create_icon("/usr/local/share/amiwb/icons/harddisk.info", get_desktop_canvas(), 10, 40);
    FileIcon *system_icon = icon_array[icon_count - 1];
    system_icon->type = TYPE_DRAWER;
    system_icon->label = "System";
    free(system_icon->path);
    system_icon->path = strdup("/");

    // Create "Home" /home/$user fs icon
    create_icon("/usr/local/share/amiwb/icons/harddisk.info", get_desktop_canvas(), 10, 120);
    FileIcon *home_icon = icon_array[icon_count - 1];
    home_icon->type = TYPE_DRAWER;
    home_icon->label = "Home";
    free(home_icon->path);
    home_icon->path = strdup(getenv("HOME"));

    redraw_canvas(get_desktop_canvas());
}


// Get current icon count
int get_icon_count(void) {
    return icon_count;
}

// Get icon array
FileIcon **get_icon_array(void) {
    return icon_array;
}

// Remove all icons for a given canvas
void clear_canvas_icons(Canvas *canvas) {
    for (int i = icon_count - 1; i >= 0; i--) {
        if (icon_array[i]->display_window == canvas->win) {
            destroy_icon(icon_array[i]);
        }
    }
}

// Create new icon
void create_icon(const char *path, Canvas *canvas, int x, int y) {
    // Get new icon slot
    FileIcon *icon = manage_icons(true, NULL);

    // Initialize icon
    *icon = (FileIcon){0};
    icon->path = strdup(path);
    icon->label = strdup(strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
    struct stat st;
    if (stat(path, &st) == 0) {
        icon->type = S_ISDIR(st.st_mode) ? TYPE_DRAWER : TYPE_FILE;
    } else {
        icon->type = TYPE_FILE; // Fallback
        fprintf(stderr, "Failed to stat %s\n", path);
    }
    icon->x = x;
    icon->y = y;
    icon->display_window = canvas->win;
    icon->selected = false;
    icon->last_click_time = 0; // Initialize last click time

    // Load icon images (from icons.c)
    // Assumes render_context access
    create_icon_images(icon, get_render_context()); 
    icon->current_picture = icon->normal_picture;
}

// Destroy an icon
void destroy_icon(FileIcon *icon) {
    if (!icon) return;

    // Free icon resources (from icons.c)
    free_icon(icon);

    // Free strings
    free(icon->path);
    free(icon->label);

    // Remove from array
    manage_icons(false, icon);

    free(icon);
}

// Find icon by window and position
FileIcon *find_icon(Window win, int x, int y) {
    Canvas *canvas = find_canvas(win);
    if (!canvas) return NULL;

    // Adjust coordinates for workbench windows: transform window-relative (x,y) to content-relative, accounting for borders and scroll
    int base_x = (canvas->type == WINDOW && canvas->client_win == None) ? BORDER_WIDTH_LEFT : 0;
    int base_y = (canvas->type == WINDOW && canvas->client_win == None) ? BORDER_HEIGHT_TOP : 0;
    int adjusted_x = x - base_x + canvas->scroll_x;
    int adjusted_y = y - base_y + canvas->scroll_y;

    for (int i = 0; i < icon_count; i++) {
        if (icon_array[i]->display_window == win &&
            adjusted_x >= icon_array[i]->x && adjusted_x < icon_array[i]->x + icon_array[i]->width &&
            adjusted_y >= icon_array[i]->y && adjusted_y < icon_array[i]->y + icon_array[i]->height) {  // add ->height +20 to include label hitbox 
            return icon_array[i];
        }
    }
    return NULL;
}

// Clean up icon array
void cleanup_workbench(void) {
    for (int i = 0; i < icon_count; i++) {
        destroy_icon(icon_array[i]);
    }
    free(icon_array);
    icon_array = NULL;
    icon_count = 0;
    icon_array_size = 0;
}

// Move icon to new position (from workbench.h)
void move_icon(FileIcon *icon, int x, int y) {
    if (!icon) return;
    icon->x = max(0, x);  // Clamp to >=0 (relative to content)
    icon->y = max(0, y);  // Clamp to >=0
}

// Check if a string ends with a suffix
static bool ends_with(const char *s, const char *suffix) {
    size_t l = strlen(s), m = strlen(suffix);
    return l >= m && strcmp(s + l - m, suffix) == 0;
}

// Check if double-click
static bool is_double_click(Time current_time, Time last_time) {
    return current_time - last_time < 500;
}

// Select icon (handles single/multi-select using Ctrl key)
static void select_icon(FileIcon *icon, Canvas *canvas, unsigned int state) {
    // If Ctrl is not pressed, deselect all other icons
    if (!(state & ControlMask)) {
        FileIcon **icons = get_icon_array();
        int count = get_icon_count();
        for (int i = 0; i < count; i++) {
            if (icons[i] != icon && icons[i]->display_window == canvas->win && icons[i]->selected) {
                icons[i]->selected = false;
                icons[i]->current_picture = icons[i]->normal_picture;
            }
        }
    }
    // Toggle selection of the clicked icon
    icon->selected = !icon->selected;
    icon->current_picture = icon->selected ? icon->selected_picture : icon->normal_picture;
}

// Deselect all icons on canvas
static void deselect_all_icons(Canvas *canvas) {
    FileIcon **icons = get_icon_array();
    int count = get_icon_count();
    for (int i = 0; i < count; i++) {
        if (icons[i]->display_window == canvas->win && icons[i]->selected) {
            icons[i]->selected = false;
            icons[i]->current_picture = icons[i]->normal_picture;
        }
    }
}

// Start dragging icon
static void start_drag_icon(FileIcon *icon, int x, int y) {
    dragged_icon = icon;
    drag_start_x = x;
    drag_start_y = y;
}

// Continue dragging icon during motion
static void continue_drag_icon(XMotionEvent *event, Canvas *canvas) {
    if (!dragged_icon) return;
    int new_x = dragged_icon->x + (event->x - drag_start_x);
    int new_y = dragged_icon->y + (event->y - drag_start_y);
    move_icon(dragged_icon, new_x, new_y); // Reuse existing move_icon function
    drag_start_x = event->x;
    drag_start_y = event->y;
    compute_content_bounds(canvas);
    compute_max_scroll(canvas);
    redraw_canvas(canvas);
}

static void end_drag_icon(Canvas *canvas) {
    dragged_icon = NULL;
    if (canvas) {
        compute_content_bounds(canvas);
        compute_max_scroll(canvas);
        redraw_canvas(canvas);
    }
}

// Open directory (handles icon creation with matching logic)
static void open_directory(FileIcon *icon, Canvas *canvas) {
    Canvas *new_canvas = create_canvas(icon->path, 50, 50, 400, 300, WINDOW);
    if (new_canvas) {
        DIR *dir = opendir(icon->path);
        if (dir) {
            int x = 10, y = 10;  // Start relative to content area top-left
            int x_offset = 80;
            struct dirent *entry;
            while ((entry = readdir(dir))) {
                if (entry->d_name[0] == '.') continue;
                char full_path[1024];
                snprintf(full_path, sizeof(full_path), "%s/%s", icon->path, entry->d_name);
                if (ends_with(entry->d_name, ".info")) {
                    char base[256];
                    strncpy(base, entry->d_name, strlen(entry->d_name) - 5);
                    base[strlen(entry->d_name) - 5] = '\0';
                    char base_path[1024];
                    snprintf(base_path, sizeof(base_path), "%s/%s", icon->path, base);
                    struct stat st;
                    if (stat(base_path, &st) != 0) {
                        create_icon(full_path, new_canvas, x, y);
                        FileIcon *new_icon = icon_array[icon_count - 1];
                        new_icon->type = TYPE_FILE;
                        x += x_offset;
                        if (x + 64 > new_canvas->width) { x = 10; y += 80; }
                    }
                } else {
                    char info_name[256];
                    snprintf(info_name, sizeof(info_name), "%s.info", entry->d_name);
                    char info_path[1024];
                    snprintf(info_path, sizeof(info_path), "%s/%s", icon->path, info_name);
                    struct stat st_info;
                    char *icon_path = stat(info_path, &st_info) == 0 ? info_path : full_path;
                    create_icon(icon_path, new_canvas, x, y);
                    FileIcon *new_icon = icon_array[icon_count - 1];
                    free(new_icon->path);
                    new_icon->path = strdup(full_path);
                    struct stat st;
                    if (stat(full_path, &st) == 0) {
                        new_icon->type = S_ISDIR(st.st_mode) ? TYPE_DRAWER : TYPE_FILE;
                    }
                    free(new_icon->label);
                    new_icon->label = strdup(entry->d_name);
                    x += x_offset;
                    if (x + 64 > new_canvas->width) { x = 10; y += 80; }
                }
            }
            closedir(dir);
        } else {
            fprintf(stderr, "Failed to open directory %s\n", icon->path);
            create_icon("/usr/local/share/amiwb/icons/harddisk.info", new_canvas, 10, 25);
            FileIcon *new_icon = icon_array[icon_count - 1];
            new_icon->type = TYPE_DRAWER;
        }
        compute_content_bounds(new_canvas);  // Compute bounds after adding icons
        compute_max_scroll(new_canvas);      // Compute max scroll
        redraw_canvas(new_canvas);
    }
}

// Open/execute file (stub for future implementation)
static void open_file(FileIcon *icon) {
    // TODO: Launch tool or execute file
}

// Add new function to compute content bounds from icons
void compute_content_bounds(Canvas *canvas) {
    if (!canvas) return;
    int max_x = 0;
    int max_y = 0;
    for (int i = 0; i < icon_count; i++) {
        if (icon_array[i]->display_window == canvas->win) {
            max_x = max(max_x, icon_array[i]->x + icon_array[i]->width);
            max_y = max(max_y, icon_array[i]->y + icon_array[i]->height + 20);  // +20 for label height
        }
    }
    canvas->content_width = max_x + 10;   // +10 padding
    canvas->content_height = max_y + 10;  // +10 padding
}

// ========================
// workbench event handling
// ========================

// Dispatcher: workbench_handle_button_press
void workbench_handle_button_press(XButtonEvent *event) {
    Canvas *canvas = find_canvas(event->window);
    if (!canvas) return;

    FileIcon *icon = find_icon(event->window, event->x, event->y);
    if (icon) {
        select_icon(icon, canvas, event->state); // Pass event->state
        if (event->button == Button1) {
            start_drag_icon(icon, event->x, event->y);
        }
        if (is_double_click(event->time, icon->last_click_time)) {
            if (icon->type == TYPE_DRAWER || icon->type == TYPE_ICONIFIED) {
                open_directory(icon, canvas);
                icon->last_click_time = event->time;
            } else if (icon->type == TYPE_FILE) {
                open_file(icon);
                icon->last_click_time = event->time;
            }
        }
        icon->last_click_time = event->time;
    } else {
        deselect_all_icons(canvas);
    }
    redraw_canvas(canvas);
}

// Dispatcher: workbench_handle_motion_notify
void workbench_handle_motion_notify(XMotionEvent *event) {
    Canvas *canvas = find_canvas(event->window);
    if (!canvas) return;

    continue_drag_icon(event, canvas);
}

// Handle button release for icons
void workbench_handle_button_release(XButtonEvent *event) {
    Canvas *canvas = find_canvas(event->window);
    if (canvas) end_drag_icon(canvas);
}