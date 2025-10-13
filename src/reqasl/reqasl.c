#include "reqasl.h"
#include "config.h"
#include "font_manager.h"
#include "../toolkit/button/button.h"
#include "../toolkit/inputfield/inputfield.h"
#include "../toolkit/listview/listview.h"
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xrender.h>
#include <X11/Xft/Xft.h>
#include <X11/keysym.h>
#include <fontconfig/fontconfig.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>
#include <pwd.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>

// ============================================================================
// Mouse Multiselection State (module-private)
// ============================================================================

static bool multiselect_pending = false;           // Button pressed, awaiting 10px threshold
static bool multiselect_active = false;            // Threshold crossed, drawing active
static int multiselect_start_x = 0;                // Initial click position
static int multiselect_start_y = 0;
static int multiselect_current_x = 0;              // Current mouse position
static int multiselect_current_y = 0;
static ReqASL *multiselect_target = NULL;          // ReqASL instance where selection is happening

// ============================================================================
// Forward Declarations
// ============================================================================

static void draw_window(ReqASL *req);

// Initialize log file with timestamp header (overwrites previous log)
static void reqasl_log_init(void) {
    const char *home = getenv("HOME");
    if (!home) return;
    
    char log_path[PATH_SIZE];
    snprintf(log_path, sizeof(log_path), "%s/Sources/amiwb/reqasl.log", home);
    
    FILE *lf = fopen(log_path, "w");  // "w" to overwrite each run
    if (lf) {
        // Header with timestamp - EXACTLY like AmiWB
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        char ts[NAME_SIZE];
        strftime(ts, sizeof(ts), "%a %d %b %Y - %H:%M", &tm);
        fprintf(lf, "ReqASL log file, started on: %s\n", ts);
        fprintf(lf, "----------------------------------------\n");
        fclose(lf);  // Close immediately - no fd inheritance
    }
}

// Error logging function - only logs actual errors
void log_error(const char *format, ...) {
    const char *home = getenv("HOME");
    if (!home) return;
    
    char log_path[PATH_SIZE];
    snprintf(log_path, sizeof(log_path), "%s/Sources/amiwb/reqasl.log", home);
    
    FILE *debug_file = fopen(log_path, "a");
    if (debug_file) {
        va_list args;
        va_start(args, format);
        vfprintf(debug_file, format, args);
        va_end(args);
        fprintf(debug_file, "\n");
        fflush(debug_file);
        fclose(debug_file);
    }
}

// Window dimensions
#define REQASL_WIDTH 381
#define REQASL_HEIGHT 405
#define REQASL_MIN_WIDTH 377  // Decreased by 4
#define REQASL_MIN_HEIGHT 405  // Decreased to not overlap decoration
#define BUTTON_WIDTH 80
#define BUTTON_HEIGHT 25
#define INPUT_HEIGHT 20
#define LIST_ITEM_HEIGHT 15
#define MARGIN 10
#define SPACING 5
#define LABEL_WIDTH 60

// Note: Colors are now defined in config.h (BLACK, WHITE, GRAY, BLUE)

// ============================================================================
// Mouse Multiselection Functions (module-private)
// ============================================================================

// Start multiselection - activates rectangle drawing
static void multiselect_start(ReqASL *req) {
    if (!req || multiselect_active) return;
    multiselect_active = true;
}

// Update file selection based on rectangle bounds (live updates)
static void multiselect_update_selection(ReqASL *req, int x1, int y1, int x2, int y2) {
    if (!req || !req->listview) return;

    ListView *lv = req->listview;

    // Normalize rectangle bounds
    int left = (x1 < x2) ? x1 : x2;
    int top = (y1 < y2) ? y1 : y2;
    int right = (x1 < x2) ? x2 : x1;
    int bottom = (y1 < y2) ? y2 : y1;

    // Clear all selections first
    listview_clear_selection(lv);

    // Check each visible item for intersection
    for (int i = 0; i < lv->item_count; i++) {
        // Calculate item bounds in screen coordinates
        int item_y = lv->y + 2 + (i - lv->scroll_offset) * LISTVIEW_ITEM_HEIGHT;

        // Skip items outside visible area
        if (i < lv->scroll_offset || i >= lv->scroll_offset + lv->visible_items) {
            continue;
        }

        int item_x = lv->x + 6;  // Text starts at x+6
        int item_w = lv->width - LISTVIEW_SCROLLBAR_WIDTH - 8;
        int item_h = LISTVIEW_ITEM_HEIGHT;

        // Rectangle intersection test
        bool intersects = !(item_x + item_w < left || item_x > right ||
                           item_y + item_h < top || item_y > bottom);

        if (intersects) {
            lv->selected[i] = true;
            lv->selection_count++;
        }
    }
}

// Update rectangle position as mouse moves
static void multiselect_update(ReqASL *req, int x, int y) {
    if (!req || !multiselect_active) return;

    // Save current mouse position
    multiselect_current_x = x;
    multiselect_current_y = y;

    // Update file selection based on current rectangle
    multiselect_update_selection(req, multiselect_start_x, multiselect_start_y, x, y);

    // Redraw window to show updated rectangle and selection
    draw_window(req);
}

// End multiselection and cleanup
static void multiselect_end(void) {
    if (!multiselect_active) return;

    ReqASL *target = multiselect_target;

    // Reset state flags
    multiselect_active = false;
    multiselect_pending = false;
    multiselect_target = NULL;

    // Redraw to remove rectangle
    if (target) {
        draw_window(target);
    }
}

// ============================================================================
// Selection Rectangle Drawing
// ============================================================================

// Draw selection rectangle using XRender (called from draw_window)
static void draw_selection_rect(ReqASL *req) {
    if (!req || !multiselect_active || req != multiselect_target) return;

    Display *dpy = req->display;
    Drawable d = req->window;
    if (!dpy || d == None) return;

    Visual *visual = DefaultVisual(dpy, DefaultScreen(dpy));

    // Normalize coordinates (ensure x1 < x2, y1 < y2)
    int x1 = multiselect_start_x;
    int y1 = multiselect_start_y;
    int x2 = multiselect_current_x;
    int y2 = multiselect_current_y;

    if (x1 > x2) { int tmp = x1; x1 = x2; x2 = tmp; }
    if (y1 > y2) { int tmp = y1; y1 = y2; y2 = tmp; }

    int w = x2 - x1;
    int h = y2 - y1;
    if (w < 2 || h < 2) return;  // Skip tiny rectangles

    // Get XRender format for alpha blending
    XRenderPictFormat *format = XRenderFindVisualFormat(dpy, visual);
    if (!format) return;

    Picture dest = XRenderCreatePicture(dpy, d, format, 0, NULL);
    if (dest == None) return;

    // Convert percentage to XRender alpha and premultiply RGB
    unsigned short fill_alpha = (SELECTION_RECT_ALPHA_FILL * 0xFFFF) / 100;
    unsigned short outline_alpha = (SELECTION_RECT_ALPHA_OUTLINE * 0xFFFF) / 100;

    float fill_alpha_norm = (float)SELECTION_RECT_ALPHA_FILL / 100.0f;
    float outline_alpha_norm = (float)SELECTION_RECT_ALPHA_OUTLINE / 100.0f;

    // Fill rectangle with transparent SELECT color
    XRenderColor fill_color = {
        (unsigned short)(SELECTION_RECT_FILL_COLOR.red * fill_alpha_norm),
        (unsigned short)(SELECTION_RECT_FILL_COLOR.green * fill_alpha_norm),
        (unsigned short)(SELECTION_RECT_FILL_COLOR.blue * fill_alpha_norm),
        fill_alpha
    };
    XRenderFillRectangle(dpy, PictOpOver, dest, &fill_color, x1, y1, w, h);

    // Draw outline with different opacity
    XRenderColor outline_color = {
        (unsigned short)(SELECTION_RECT_OUTLINE_COLOR.red * outline_alpha_norm),
        (unsigned short)(SELECTION_RECT_OUTLINE_COLOR.green * outline_alpha_norm),
        (unsigned short)(SELECTION_RECT_OUTLINE_COLOR.blue * outline_alpha_norm),
        outline_alpha
    };
    XRenderFillRectangle(dpy, PictOpOver, dest, &outline_color, x1, y1, w, 1);      // Top
    XRenderFillRectangle(dpy, PictOpOver, dest, &outline_color, x1, y2-1, w, 1);    // Bottom
    XRenderFillRectangle(dpy, PictOpOver, dest, &outline_color, x1, y1, 1, h);      // Left
    XRenderFillRectangle(dpy, PictOpOver, dest, &outline_color, x2-1, y1, 1, h);    // Right

    XRenderFreePicture(dpy, dest);
}

// Forward declarations
static void free_entries(ReqASL *req);
static void scan_directory(ReqASL *req, const char *path);
static void draw_window(ReqASL *req);
static int compare_entries(const void *a, const void *b);
static void listview_select_callback(int index, const char *text, void *user_data);
static void listview_double_click_callback(int index, const char *text, void *user_data);
static bool reqasl_execute_action(ReqASL *req);
static void reqasl_update_menu_data(ReqASL *req);

ReqASL* reqasl_create(Display *display) {
    if (!display) return NULL;
    
    // Initialize debug logging with fresh file
    reqasl_log_init();
    
    ReqASL *req = calloc(1, sizeof(ReqASL));
    if (!req) return NULL;
    
    req->display = display;
    req->width = REQASL_WIDTH;
    req->height = REQASL_HEIGHT;

    // Get font from font manager
    req->font = reqasl_font_get();
    if (!req->font) {
        log_error("[ERROR] Font not available from font manager");
        free(req);
        return NULL;
    }
    req->is_open = false;
    req->show_hidden = false;
    req->selected_index = -1;

    // Initialize button press states
    req->open_button_pressed = false;
    req->volumes_button_pressed = false;
    req->parent_button_pressed = false;
    req->cancel_button_pressed = false;

    // Calculate list area (at top, takes most space)
    req->list_y = MARGIN;
    req->list_height = req->height - MARGIN - (3 * INPUT_HEIGHT) - (4 * SPACING) - BUTTON_HEIGHT - MARGIN;
    
    // Create ListView widget
    req->listview = listview_create(MARGIN, req->list_y, 
                                   req->width - MARGIN * 2, 
                                   req->list_height);
    if (!req->listview) {
        log_error("[ERROR] Failed to create listview");
        // Continue anyway - old rendering will be fallback
    } else {
        // Set callbacks for ListView
        listview_set_callbacks(req->listview, 
                             listview_select_callback,
                             listview_double_click_callback,
                             req);
        // Default to open mode with multi-selection enabled
        req->multi_select_enabled = true;
        listview_set_multi_select(req->listview, true);
    }
    
    // Create InputField widgets for text editing
    // Calculate positions (will be updated in draw_window for dynamic resizing)
    int input_y = req->list_y + req->list_height + SPACING;
    
    req->pattern_field = inputfield_create(MARGIN + LABEL_WIDTH, input_y, 
                                          req->width - MARGIN * 2 - LABEL_WIDTH, INPUT_HEIGHT, req->font);
    if (req->pattern_field) {
        inputfield_set_text(req->pattern_field, "*");  // Default to show all files
        inputfield_set_disabled(req->pattern_field, false);  // Pattern field is now functional
    }
    
    req->drawer_field = inputfield_create(MARGIN + LABEL_WIDTH, input_y + INPUT_HEIGHT + SPACING,
                                         req->width - MARGIN * 2 - LABEL_WIDTH, INPUT_HEIGHT, req->font);
    if (req->drawer_field) {
        inputfield_set_text(req->drawer_field, "");
        inputfield_enable_path_completion(req->drawer_field, true);  // Enable path completion for drawer field
    }
    
    req->file_field = inputfield_create(MARGIN + LABEL_WIDTH, input_y + 2 * (INPUT_HEIGHT + SPACING),
                                       req->width - MARGIN * 2 - LABEL_WIDTH, INPUT_HEIGHT, req->font);
    if (req->file_field) {
        inputfield_set_text(req->file_field, "");
        inputfield_enable_path_completion(req->file_field, true);  // Enable path completion for file field
    }
    
    // Initialize entries array
    req->entries = NULL;
    req->entry_count = 0;
    req->entry_capacity = 0;
    
    // Get home directory as default
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir) {
        strncpy(req->current_path, pw->pw_dir, sizeof(req->current_path) - 1);
        req->current_path[sizeof(req->current_path) - 1] = '\0';
    } else {
        strncpy(req->current_path, "/", sizeof(req->current_path) - 1);
        req->current_path[sizeof(req->current_path) - 1] = '\0';
    }
    strncpy(req->drawer_text, req->current_path, sizeof(req->drawer_text) - 1);
    req->drawer_text[sizeof(req->drawer_text) - 1] = '\0';
    req->pattern_text[0] = '\0';
    req->file_text[0] = '\0';
    
    // Initialize window title to empty - will be set by reqasl_set_title()
    req->window_title[0] = '\0';
    
    // Sync InputFields with text buffers
    if (req->drawer_field) {
        inputfield_set_text(req->drawer_field, req->drawer_text);
        // Ensure the end of the path is visible
        inputfield_scroll_to_end(req->drawer_field);
    }
    
    // Set file field's base directory for completion
    if (req->file_field) {
        inputfield_set_completion_base_dir(req->file_field, req->drawer_text);
    }
    
    // Create window
    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);
    
    XSetWindowAttributes attrs;
    // Use gray background (approximate Amiga gray)
    attrs.background_pixel = 0xa0a0a2;  // GRAY from config.h
    attrs.border_pixel = BlackPixel(display, screen);
    attrs.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | 
                       ButtonReleaseMask | PointerMotionMask | StructureNotifyMask;
    
    req->window = XCreateWindow(display, root,
                               100, 100, req->width, req->height,
                               1, CopyFromParent, InputOutput, CopyFromParent,
                               CWBackPixel | CWBorderPixel | CWEventMask,
                               &attrs);
    
    // Don't set window title here - it will be set by reqasl_set_title() later
    // This ensures the correct title is set before the window is mapped
    
    // Set WM_CLASS for window identification (app identity, never changes)
    XClassHint *class_hint = XAllocClassHint();
    if (class_hint) {
        class_hint->res_name = "ReqASL";  // Application name
        class_hint->res_class = "ReqASL";  // Application class
        XSetClassHint(display, req->window, class_hint);
        XFree(class_hint);
    }
    
    // Set minimum and maximum window size hints to the same value
    // This effectively makes the window non-resizable
    XSizeHints *size_hints = XAllocSizeHints();
    if (size_hints) {
        size_hints->flags = PMinSize | PMaxSize | PBaseSize | PSize;
        size_hints->min_width = REQASL_MIN_WIDTH;
        size_hints->min_height = REQASL_MIN_HEIGHT;
        // Set max size to same as min - this makes window non-resizable
        size_hints->max_width = REQASL_MIN_WIDTH;
        size_hints->max_height = REQASL_MIN_HEIGHT;
        size_hints->base_width = REQASL_MIN_WIDTH;
        size_hints->base_height = REQASL_MIN_HEIGHT;
        size_hints->width = req->width;
        size_hints->height = req->height;
        // Use both old and new API for maximum compatibility
        XSetWMSizeHints(display, req->window, size_hints, XA_WM_NORMAL_HINTS);
        XSetWMNormalHints(display, req->window, size_hints);
        XFree(size_hints);
    }
    
    // Set WM protocols for window close
    Atom wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, req->window, &wm_delete, 1);
    
    // Register with AmiWB for menu substitution
    Atom app_type_atom = XInternAtom(display, "_AMIWB_APP_TYPE", False);

    // Set app type
    const char *app_type = "ReqASL";
    XChangeProperty(display, req->window, app_type_atom,
                   XA_STRING, 8, PropModeReplace,
                   (unsigned char*)app_type, strlen(app_type));
    
    // Set initial menu data using the update function to ensure consistency
    reqasl_update_menu_data(req);

    return req;
}

void reqasl_destroy(ReqASL *req) {
    if (!req) return;
    
    if (req->listview) {
        listview_destroy(req->listview);
    }
    
    // Destroy InputField widgets
    if (req->pattern_field) {
        inputfield_destroy(req->pattern_field);
    }
    if (req->drawer_field) {
        inputfield_destroy(req->drawer_field);
    }
    if (req->file_field) {
        inputfield_destroy(req->file_field);
    }
    
    if (req->xft_draw) {
        XftDrawDestroy(req->xft_draw);
    }
    
    // Font is managed by font_manager - don't close it!
    req->font = NULL;  // Just clear the reference
    
    if (req->window) {
        XDestroyWindow(req->display, req->window);
    }
    
    free_entries(req);
    free(req);
}

// Update menu data to show checkmark for Show Hidden toggle
static void reqasl_update_menu_data(ReqASL *req) {
    if (!req || !req->display || !req->window) return;

    // Build menu data string with "#" shortcut notation and [o]/[x] checkbox notation
    char menu_data[NAME_SIZE * 3];  // Menu string buffer (larger for shortcuts)

    // Use [o] for checked (on), [x] for unchecked (off) toggleable items
    const char *show_hidden_state = req->show_hidden ? "[o]" : "[x]";

    snprintf(menu_data, sizeof(menu_data),
            "File:Open #O,Quit #Q|Edit:New Drawer,Rename,Delete,Select Files #A,Select None #Z|View:By Names,By Date,%s Show Hidden #H|Locations:Add Place,Del Place",
            show_hidden_state);

    // Update the property
    Atom menu_data_atom = XInternAtom(req->display, "_AMIWB_MENU_DATA", False);
    XChangeProperty(req->display, req->window, menu_data_atom,
                   XA_STRING, 8, PropModeReplace,
                   (unsigned char*)menu_data, strlen(menu_data));
}

// Check if path is a standard XDG directory (protected from deletion)
static bool is_standard_xdg_dir(const char *path) {
    const char *home = getenv("HOME");
    if (!home || !path) return false;

    // Standard XDG directories that shouldn't be deleted
    const char *standard_dirs[] = {
        "Desktop", "Documents", "Downloads", "Music",
        "Pictures", "Videos", "Templates", "Public",
        NULL
    };

    // Check if path matches any standard directory
    for (int i = 0; standard_dirs[i]; i++) {
        char test_path[PATH_SIZE];
        snprintf(test_path, sizeof(test_path), "%s/%s", home, standard_dirs[i]);
        if (strcmp(path, test_path) == 0) {
            return true;  // This is a protected standard directory
        }
    }
    return false;
}

// Check if path already exists in user-dirs.dirs
static bool path_exists_in_user_dirs(const char *path) {
    const char *home = getenv("HOME");
    if (!home || !path) return false;

    char config_path[PATH_SIZE];
    snprintf(config_path, sizeof(config_path), "%s/.config/user-dirs.dirs", home);

    FILE *file = fopen(config_path, "r");
    if (!file) return false;

    char line[FULL_SIZE * 2];  // Line buffer for config file
    bool found = false;

    while (fgets(line, sizeof(line), file)) {
        // Skip comments
        if (line[0] == '#') continue;

        // Parse XDG_xxx_DIR="path" lines
        char *equals = strchr(line, '=');
        if (!equals) continue;

        char *value = equals + 1;
        // Remove quotes and newline
        if (*value == '"') value++;
        char *end = strchr(value, '"');
        if (end) *end = '\0';

        // Build full path from value
        char full_path[PATH_SIZE];
        if (strncmp(value, "$HOME/", 6) == 0) {
            snprintf(full_path, sizeof(full_path), "%s/%s", home, value + 6);
        } else if (strncmp(value, "$HOME", 5) == 0) {
            snprintf(full_path, sizeof(full_path), "%s%s", home, value + 5);
        } else if (value[0] == '/') {
            snprintf(full_path, sizeof(full_path), "%s", value);
        } else {
            snprintf(full_path, sizeof(full_path), "%s/%s", home, value);
        }

        // Check for exact match
        if (strcmp(full_path, path) == 0) {
            found = true;
            break;
        }
    }

    fclose(file);
    return found;
}

// Update menu item enable/disable states
static void reqasl_update_menu_states(ReqASL *req) {
    if (!req) return;
    
    // Build menu state string
    // Format: "menu_index,item_index,enabled;menu_index,item_index,enabled;..."
    char menu_states[PATH_SIZE];  // Menu state string
    char *p = menu_states;
    int remaining = sizeof(menu_states);
    
    // File menu (index 0)
    // Open (0): Enable if files are selected
    bool has_selection = false;
    if (req->listview) {
        // Check for multi-selection
        for (int i = 0; i < req->listview->item_count; i++) {
            if (req->listview->selected[i]) {
                has_selection = true;
                break;
            }
        }
        // If no multi-selection, check for single selection
        if (!has_selection && req->listview->selected_index >= 0) {
            has_selection = true;
        }
    }
    
    int written = snprintf(p, remaining, "0,0,%d;", has_selection ? 1 : 0);  // File > Open
    p += written;
    remaining -= written;

    written = snprintf(p, remaining, "0,1,1;");  // File > Quit (always enabled)
    p += written;
    remaining -= written;

    // Edit menu (index 1)
    // New Drawer (0): Disabled (not implemented yet)
    // Rename (1): Disabled for now (needs dialog)
    // Delete (2): Disabled for now (needs warning dialog first)
    // Select Files (3): Enabled when multi-select is on
    // Select None (4): Enabled when multi-select is on
    written = snprintf(p, remaining, "1,0,0;");  // Edit > New Drawer (disabled - not implemented)
    p += written;
    remaining -= written;

    written = snprintf(p, remaining, "1,1,0;");  // Edit > Rename (disabled)
    p += written;
    remaining -= written;

    written = snprintf(p, remaining, "1,2,0;");  // Edit > Delete (disabled)
    p += written;
    remaining -= written;

    // Select Files/None are enabled when multi-select is active
    written = snprintf(p, remaining, "1,3,%d;", req->multi_select_enabled ? 1 : 0);  // Edit > Select Files
    p += written;
    remaining -= written;

    written = snprintf(p, remaining, "1,4,%d;", req->multi_select_enabled ? 1 : 0);  // Edit > Select None
    p += written;
    remaining -= written;

    // View menu (index 2)
    // By Names (0), By Date (1): Both disabled for now (stubs)
    written = snprintf(p, remaining, "2,0,0;2,1,0;");  // View items disabled
    p += written;
    remaining -= written;

    // Locations menu (index 3)
    // Add Place (0): Enable if not already in user-dirs.dirs and not in Locations view
    bool can_add_place = !req->showing_locations && !path_exists_in_user_dirs(req->current_path);
    written = snprintf(p, remaining, "3,0,%d;", can_add_place ? 1 : 0);
    p += written;
    remaining -= written;

    // Del Place (1): Enable if in user-dirs.dirs but NOT a standard XDG directory
    bool can_del_place = !req->showing_locations &&
                         path_exists_in_user_dirs(req->current_path) &&
                         !is_standard_xdg_dir(req->current_path);
    written = snprintf(p, remaining, "3,1,%d", can_del_place ? 1 : 0);
    p += written;
    remaining -= written;
    
    // Set the menu states property
    Atom menu_states_atom = XInternAtom(req->display, "_AMIWB_MENU_STATES", False);
    XChangeProperty(req->display, req->window, menu_states_atom,
                   XA_STRING, 8, PropModeReplace,
                   (unsigned char*)menu_states, strlen(menu_states));
    XFlush(req->display);
}

void reqasl_show(ReqASL *req, const char *initial_path) {
    if (!req) return;
    
    req->is_open = true;
    
    // Determine which path to use:
    // Priority: 1) --path argument, 2) REQASL_LAST_PATH env, 3) HOME
    const char *path_to_use = NULL;
    
    if (initial_path && initial_path[0]) {
        // Command line argument takes priority
        path_to_use = initial_path;
    } else {
        // Check X11 property for last used path (shared across all processes)
        Window root = DefaultRootWindow(req->display);
        Atom prop = XInternAtom(req->display, "REQASL_LAST_PATH", False);
        Atom actual_type;
        int actual_format;
        unsigned long nitems, bytes_after;
        unsigned char *data = NULL;
        
        if (XGetWindowProperty(req->display, root, prop,
                              0, PATH_SIZE, False, XA_STRING,
                              &actual_type, &actual_format, &nitems, &bytes_after,
                              &data) == Success && data) {
            // Got the property - check if path still exists
            if (access((char *)data, F_OK) == 0) {
                path_to_use = (char *)data;
            }
            // Note: XFree(data) is called after we're done using it
        }
    }
    
    if (path_to_use) {
        reqasl_navigate_to(req, path_to_use);
        // Free the X11 property data if we allocated it
        if (path_to_use != initial_path) {
            XFree((unsigned char *)path_to_use);
        }
    } else {
        // Fall back to current path (HOME)
        scan_directory(req, req->current_path);
    }
    
    // Show window
    XMapRaised(req->display, req->window);
    XFlush(req->display);
    
    // Initial draw
    draw_window(req);

    // Set initial menu data and states
    reqasl_update_menu_data(req);    // Send menu structure with checkmarks
    reqasl_update_menu_states(req);  // Send enabled/disabled states
}

void reqasl_hide(ReqASL *req) {
    if (!req) return;
    
    req->is_open = false;
    XUnmapWindow(req->display, req->window);
    XFlush(req->display);
}

void reqasl_set_callbacks(ReqASL *req, 
                         void (*on_open)(const char*),
                         void (*on_cancel)(void),
                         void *user_data) {
    if (!req) return;
    req->on_open = on_open;
    req->on_cancel = on_cancel;
    req->user_data = user_data;
}

void reqasl_refresh(ReqASL *req) {
    if (!req) return;
    scan_directory(req, req->current_path);
    draw_window(req);
}

// Internal navigation function - can optionally skip env var update
static void navigate_internal(ReqASL *req, const char *path, bool update_env) {
    if (!req || !path) return;
    
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        // Path is valid
        strncpy(req->current_path, path, sizeof(req->current_path) - 1);
        req->current_path[sizeof(req->current_path) - 1] = '\0';
        strncpy(req->drawer_text, req->current_path, sizeof(req->drawer_text) - 1);
        req->drawer_text[sizeof(req->drawer_text) - 1] = '\0';
        
        // Update X11 property only if requested (replaces env var)
        if (update_env) {
            // Store as X11 property on root window for persistence across processes
            Window root = DefaultRootWindow(req->display);
            Atom prop = XInternAtom(req->display, "REQASL_LAST_PATH", False);
            XChangeProperty(req->display, root, prop,
                          XA_STRING, 8, PropModeReplace,
                          (unsigned char *)req->current_path, 
                          strlen(req->current_path));
            XFlush(req->display);
        }
        
        // Clear the File field when changing directories
        req->file_text[0] = '\0';
        if (req->file_field) {
            inputfield_set_text(req->file_field, "");
        }
        
        // Update drawer field and ensure end is visible
        if (req->drawer_field) {
            inputfield_set_text(req->drawer_field, req->drawer_text);
            inputfield_scroll_to_end(req->drawer_field);
        }
        
        // Update file field's base directory for completion
        if (req->file_field) {
            inputfield_set_completion_base_dir(req->file_field, req->drawer_text);
        }
        
        req->selected_index = -1;
        scan_directory(req, req->current_path);  // Use copied path, not original which may be freed
        // Scanned directory
        if (req->listview) {
            // Reset double-click tracking to prevent leaking clicks into new directory
            req->listview->last_click_time = 0;
            req->listview->last_click_index = -1;
            // Clear any multi-selection
            if (req->multi_select_enabled) {
                listview_clear_selection(req->listview);
            }
        }
        // Update menu states for Add/Del Place based on new directory
        reqasl_update_menu_states(req);
        draw_window(req);
    } else {
        // Path not valid
    }
}

// Public navigation function - always updates env var
void reqasl_navigate_to(ReqASL *req, const char *path) {
    navigate_internal(req, path, true);  // true = update env var
}

void reqasl_navigate_parent(ReqASL *req) {
    if (!req) return;
    
    char *last_slash = strrchr(req->current_path, '/');
    if (last_slash && last_slash != req->current_path) {
        *last_slash = '\0';
        reqasl_navigate_to(req, req->current_path);
    } else if (last_slash == req->current_path) {
        // Already at root
        reqasl_navigate_to(req, "/");
    }
}

static void free_entries(ReqASL *req) {
    if (!req->entries) return;
    
    for (int i = 0; i < req->entry_count; i++) {
        if (req->entries[i]) {
            free(req->entries[i]->name);
            free(req->entries[i]->path);
            free(req->entries[i]);
        }
    }
    free(req->entries);
    req->entries = NULL;
    req->entry_count = 0;
    req->entry_capacity = 0;
}

// Check if filename matches pattern (supports wildcards like *.txt, *.mp4)
static bool matches_pattern(const char *filename, const char *pattern) {
    if (!pattern || !*pattern || strcmp(pattern, "*") == 0) {
        return true;  // No pattern or "*" matches everything
    }
    
    // Handle multiple patterns separated by commas
    char pattern_copy[NAME_SIZE * 2];  // Pattern buffer
    strncpy(pattern_copy, pattern, sizeof(pattern_copy) - 1);
    pattern_copy[sizeof(pattern_copy) - 1] = '\0';
    
    char *token = strtok(pattern_copy, ",");
    while (token) {
        // Trim spaces
        while (*token == ' ') token++;
        char *end = token + strlen(token) - 1;
        while (end > token && *end == ' ') *end-- = '\0';
        
        // Simple wildcard matching
        if (*token == '*') {
            // Pattern like *.txt
            const char *ext = token + 1;
            if (*ext == '.') {
                // Check file extension
                const char *file_ext = strrchr(filename, '.');
                if (file_ext && strcasecmp(file_ext, ext) == 0) {
                    return true;
                }
            }
        } else {
            // Exact match
            if (strcasecmp(filename, token) == 0) {
                return true;
            }
        }
        
        token = strtok(NULL, ",");
    }
    
    return false;
}

static int compare_entries(const void *a, const void *b) {
    FileEntry *ea = *(FileEntry**)a;
    FileEntry *eb = *(FileEntry**)b;
    
    // Directories first
    if (ea->type != eb->type) {
        return (ea->type == TYPE_DRAWER) ? -1 : 1;
    }
    
    // Then alphabetical
    return strcasecmp(ea->name, eb->name);
}

static void scan_directory(ReqASL *req, const char *path) {
    if (!req || !path) return;
    
    // Scanning directory
    
    // Free existing entries
    free_entries(req);
    
    // Clear ListView
    if (req->listview) {
        // Clear ListView
        listview_clear(req->listview);
        // ListView cleared
    }
    
    DIR *dir = opendir(path);
    if (!dir) {
        // Failed to open directory - silent fail
        return;
    }
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        // Skip . and ..
        if (strcmp(entry->d_name, ".") == 0 || 
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        // Skip hidden files unless showing them
        if (!req->show_hidden && entry->d_name[0] == '.') {
            continue;
        }
        
        // Check if it's a directory first (directories always shown)
        char full_path[2048];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
        struct stat st;
        bool is_directory = false;
        if (stat(full_path, &st) == 0) {
            is_directory = S_ISDIR(st.st_mode);
        }
        
        // Apply pattern filter (but always show directories)
        if (!is_directory) {
            // Get pattern from input field if available, otherwise use stored pattern
            const char *current_pattern = NULL;
            if (req->pattern_field) {
                current_pattern = inputfield_get_text(req->pattern_field);
            }
            if (!current_pattern || !*current_pattern) {
                current_pattern = req->pattern_text;
            }
            
            if (current_pattern && *current_pattern && strcmp(current_pattern, "*") != 0) {
                if (!matches_pattern(entry->d_name, current_pattern)) {
                    continue;  // Skip files that don't match pattern
                }
            }
        }
        
        // Grow array if needed
        if (req->entry_count >= req->entry_capacity) {
            int new_capacity = req->entry_capacity ? req->entry_capacity * 2 : 16;
            FileEntry **new_entries = realloc(req->entries, 
                                             sizeof(FileEntry*) * new_capacity);
            if (!new_entries) continue;
            req->entries = new_entries;
            req->entry_capacity = new_capacity;
        }
        
        // Create entry
        FileEntry *fe = calloc(1, sizeof(FileEntry));
        if (!fe) continue;
        
        fe->name = strdup(entry->d_name);
        fe->path = strdup(full_path);
        
        // Use the stat info we already got
        fe->type = is_directory ? TYPE_DRAWER : TYPE_FILE;
        fe->size = st.st_size;
        fe->modified = st.st_mtime;
        
        req->entries[req->entry_count++] = fe;
    }
    
    closedir(dir);
    
    // Entries found
    
    // Sort entries
    if (req->entry_count > 0) {
        qsort(req->entries, req->entry_count, sizeof(FileEntry*), compare_entries);
        
        // Add entries to ListView
        if (req->listview) {
            // Adding entries
            for (int i = 0; i < req->entry_count; i++) {
                FileEntry *fe = req->entries[i];
                // Adding item
                listview_add_item(req->listview, fe->name, 
                                fe->type == TYPE_DRAWER, fe);
            }
            // ListView populated
            // Redraw state set
        } else {
            log_error("[ERROR] scan_directory: ListView is NULL!");
        }
    } else {
        // No entries found
    }
}

// Load user places from XDG user-dirs.dirs
static void load_user_places(ReqASL *req, FileEntry ***entries, int *count) {
    const char *home = getenv("HOME");
    if (!home) return;

    // Dynamic array to hold entries from user-dirs.dirs
    // Start small and grow as needed
    int capacity = 8;
    *entries = malloc(capacity * sizeof(FileEntry*));
    if (!*entries) return;
    *count = 0;

    // Read user-dirs.dirs if it exists
    char config_path[PATH_SIZE];
    snprintf(config_path, sizeof(config_path), "%s/.config/user-dirs.dirs", home);

    FILE *file = fopen(config_path, "r");
    if (!file) return;  // No file, no places

    char line[FULL_SIZE * 2];  // Line buffer for config file
    while (fgets(line, sizeof(line), file)) {
        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '\n') continue;

        // Parse XDG_xxx_DIR="path" lines
        char *equals = strchr(line, '=');
        if (!equals) continue;

        *equals = '\0';
        char *var_name = line;
        char *value = equals + 1;

        // Remove quotes and newline
        if (*value == '"') value++;
        char *end = strchr(value, '"');
        if (end) *end = '\0';

        // Extract label from variable name (XDG_DESKTOP_DIR -> Desktop)
        char label[NAME_SIZE];
        if (strncmp(var_name, "XDG_", 4) == 0) {
            char *name_start = var_name + 4;
            char *dir_suffix = strstr(name_start, "_DIR");
            if (dir_suffix) {
                *dir_suffix = '\0';
                // Convert from XDG format to display format
                // DESKTOP -> Desktop
                // AMIWB_SOURCES -> Amiwb Sources
                // MY_CUSTOM_DIR -> My Custom Dir
                snprintf(label, sizeof(label), "%s", name_start);

                // Process the label character by character
                bool start_of_word = true;
                int j = 0;
                for (int i = 0; name_start[i] && j < sizeof(label) - 1; i++) {
                    if (name_start[i] == '_') {
                        // Replace underscore with space
                        label[j++] = ' ';
                        start_of_word = true;
                    } else {
                        // Capitalize first letter of each word, lowercase the rest
                        if (start_of_word) {
                            label[j++] = toupper(name_start[i]);
                            start_of_word = false;
                        } else {
                            label[j++] = tolower(name_start[i]);
                        }
                    }
                }
                label[j] = '\0';
            } else {
                continue;  // Not a valid _DIR entry
            }
        } else {
            continue;  // Not an XDG entry
        }

        // Build full path
        char full_path[PATH_SIZE];
        if (strncmp(value, "$HOME/", 6) == 0) {
            snprintf(full_path, sizeof(full_path), "%s/%s", home, value + 6);
        } else if (strncmp(value, "$HOME", 5) == 0) {
            snprintf(full_path, sizeof(full_path), "%s%s", home, value + 5);
        } else if (value[0] == '/') {
            snprintf(full_path, sizeof(full_path), "%s", value);
        } else {
            snprintf(full_path, sizeof(full_path), "%s/%s", home, value);
        }

        // Check if directory exists
        struct stat st;
        if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            // Grow array if needed
            if (*count >= capacity) {
                capacity *= 2;
                FileEntry **new_entries = realloc(*entries, capacity * sizeof(FileEntry*));
                if (!new_entries) {
                    // Keep what we have so far
                    break;
                }
                *entries = new_entries;
            }

            // Add entry with formatted display (label + full path)
            FileEntry *entry = malloc(sizeof(FileEntry));
            if (entry) {
                // Replace home directory with ~ for display
                char display_path[PATH_SIZE];
                if (strncmp(full_path, home, strlen(home)) == 0) {
                    // Path starts with home directory - replace with ~
                    snprintf(display_path, sizeof(display_path), "~%s", full_path + strlen(home));
                } else {
                    // Not in home directory - show full path
                    snprintf(display_path, sizeof(display_path), "%s", full_path);
                }

                // Format: "Label          (~/path)" with consistent spacing
                char display_name[FULL_SIZE];
                snprintf(display_name, sizeof(display_name), "%-15s (%s)", label, display_path);

                entry->name = strdup(display_name);
                entry->path = strdup(full_path);  // Keep full path for navigation
                entry->type = TYPE_DRAWER;
                entry->size = 0;
                entry->modified = st.st_mtime;
                (*entries)[(*count)++] = entry;
            }
        }
    }

    fclose(file);

    // Sort entries: standard XDG dirs first, then custom entries
    // Both groups sorted alphabetically
    if (*count > 1) {
        // Simple bubble sort is fine for small lists
        for (int i = 0; i < *count - 1; i++) {
            for (int j = i + 1; j < *count; j++) {
                FileEntry *a = (*entries)[i];
                FileEntry *b = (*entries)[j];

                // Check if either is a standard XDG directory
                bool a_is_standard = false;
                bool b_is_standard = false;

                const char *standard_names[] = {
                    "Desktop ", "Documents ", "Downloads ", "Music ",
                    "Pictures ", "Videos ", "Templates ", "Public ", NULL
                };

                for (int k = 0; standard_names[k]; k++) {
                    if (strncmp(a->name, standard_names[k], strlen(standard_names[k])) == 0) {
                        a_is_standard = true;
                    }
                    if (strncmp(b->name, standard_names[k], strlen(standard_names[k])) == 0) {
                        b_is_standard = true;
                    }
                }

                // Swap if needed (standard dirs come first, then alphabetical within each group)
                bool should_swap = false;
                if (a_is_standard && !b_is_standard) {
                    should_swap = false;  // a is already in right place
                } else if (!a_is_standard && b_is_standard) {
                    should_swap = true;   // b should come before a
                } else {
                    // Both are same type, sort alphabetically
                    should_swap = (strcmp(a->name, b->name) > 0);
                }

                if (should_swap) {
                    (*entries)[i] = b;
                    (*entries)[j] = a;
                }
            }
        }
    }
}

// Build the Locations view with user places only
static void build_locations_view(ReqASL *req) {
    // Free existing entries
    free_entries(req);

    // Get user places
    FileEntry **user_places = NULL;
    int place_count = 0;
    load_user_places(req, &user_places, &place_count);

    // Just use the user places directly
    req->entries = user_places;
    req->entry_count = place_count;
    req->entry_capacity = place_count;
    req->showing_locations = true;

    // Reset selection
    req->selected_index = -1;
}

// Add current directory to user places in user-dirs.dirs
static void add_user_place(ReqASL *req, const char *path) {
    if (!path || !*path) return;

    const char *home = getenv("HOME");
    if (!home) return;

    // Get the label - use last component of path
    const char *last_slash = strrchr(path, '/');
    const char *label = (last_slash && *(last_slash + 1)) ? last_slash + 1 : path;

    // Don't add standard XDG directories - they're already there
    const char *standard_dirs[] = {
        "Desktop", "Documents", "Downloads", "Music", "Pictures", "Videos", NULL
    };
    for (int i = 0; standard_dirs[i]; i++) {
        if (strcmp(label, standard_dirs[i]) == 0) {
            // Standard directory - silently skip
            return;
        }
    }

    // Read existing user-dirs.dirs
    char config_path[PATH_SIZE];
    snprintf(config_path, sizeof(config_path), "%s/.config/user-dirs.dirs", home);

    // Check if this path is already in the file (use proper check)
    if (path_exists_in_user_dirs(path)) {
        // Path already exists - silently skip
        return;
    }

    // Append new entry
    FILE *file = fopen(config_path, "a");
    if (!file) {
        fprintf(stderr, "[ERROR] Cannot open %s for writing\n", config_path);
        return;
    }

    // Create XDG variable name from label (uppercase, replace spaces with underscore)
    char var_name[NAME_SIZE];
    snprintf(var_name, sizeof(var_name), "XDG_%s_DIR", label);
    for (char *p = var_name; *p; p++) {
        if (*p == ' ' || *p == '-') *p = '_';
        else *p = toupper(*p);
    }

    // Write the entry
    // If path starts with home, make it relative
    if (strncmp(path, home, strlen(home)) == 0) {
        const char *rel_path = path + strlen(home);
        if (*rel_path == '/') rel_path++;  // Skip the slash
        fprintf(file, "%s=\"$HOME/%s\"\n", var_name, rel_path);
    } else {
        fprintf(file, "%s=\"%s\"\n", var_name, path);
    }

    fclose(file);
    // Place added successfully

    // If we're in Locations view, refresh it
    if (req->showing_locations) {
        build_locations_view(req);
        if (req->listview) {
            listview_clear(req->listview);
            for (int i = 0; i < req->entry_count; i++) {
                FileEntry *fe = req->entries[i];
                listview_add_item(req->listview, fe->name,
                                fe->type == TYPE_DRAWER, fe);
            }
        }
        draw_window(req);
    }
}

// Remove current directory from user places
static void remove_user_place(ReqASL *req, const char *path) {
    if (!path || !*path) return;

    const char *home = getenv("HOME");
    if (!home) return;

    // Don't remove standard XDG directories
    if (is_standard_xdg_dir(path)) {
        fprintf(stderr, "[WARNING] Cannot remove standard XDG directory: %s\n", path);
        return;
    }

    char config_path[PATH_SIZE];
    snprintf(config_path, sizeof(config_path), "%s/.config/user-dirs.dirs", home);

    // Read existing file
    FILE *file = fopen(config_path, "r");
    if (!file) {
        fprintf(stderr, "[WARNING] Cannot open %s for reading\n", config_path);
        return;
    }

    // Create temp file (use FULL_SIZE to ensure room for .tmp suffix)
    char temp_path[FULL_SIZE];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", config_path);
    FILE *temp = fopen(temp_path, "w");
    if (!temp) {
        fclose(file);
        fprintf(stderr, "[ERROR] Cannot create temp file\n");
        return;
    }

    // Copy all lines except the one with our exact path
    char line[FULL_SIZE * 2];  // Line buffer for config file
    bool removed = false;
    while (fgets(line, sizeof(line), file)) {
        // Always keep comments
        if (line[0] == '#') {
            fputs(line, temp);
            continue;
        }

        // Parse the line to extract the path
        char line_copy[FULL_SIZE * 2];  // Copy for parsing
        snprintf(line_copy, sizeof(line_copy), "%s", line);

        char *equals = strchr(line_copy, '=');
        if (!equals) {
            fputs(line, temp);  // Not a valid entry, keep it
            continue;
        }

        char *value = equals + 1;
        // Remove quotes and newline
        if (*value == '"') value++;
        char *end = strchr(value, '"');
        if (end) *end = '\0';

        // Build full path from value
        char full_path[PATH_SIZE];
        if (strncmp(value, "$HOME/", 6) == 0) {
            snprintf(full_path, sizeof(full_path), "%s/%s", home, value + 6);
        } else if (strncmp(value, "$HOME", 5) == 0) {
            snprintf(full_path, sizeof(full_path), "%s%s", home, value + 5);
        } else if (value[0] == '/') {
            snprintf(full_path, sizeof(full_path), "%s", value);
        } else {
            snprintf(full_path, sizeof(full_path), "%s/%s", home, value);
        }

        // Check for exact match
        if (strcmp(full_path, path) == 0) {
            removed = true;  // Skip this line
            // Place removed successfully
        } else {
            fputs(line, temp);  // Keep this line
        }
    }

    fclose(file);
    fclose(temp);

    if (removed) {
        // Replace original with temp
        if (rename(temp_path, config_path) != 0) {
            fprintf(stderr, "[ERROR] Failed to update user-dirs.dirs\n");
            unlink(temp_path);
        }

        // If we're in Locations view, refresh it
        if (req->showing_locations) {
            build_locations_view(req);
            if (req->listview) {
                listview_clear(req->listview);
            for (int i = 0; i < req->entry_count; i++) {
                FileEntry *fe = req->entries[i];
                listview_add_item(req->listview, fe->name,
                                fe->type == TYPE_DRAWER, fe);
            }
            }
            draw_window(req);
        }
    } else {
        unlink(temp_path);
        // Path not found - nothing to remove
    }
}

static void draw_window(ReqASL *req) {
    if (!req || !req->window) return;
    
    // Debug output for window dimensions
    // Window dimensions set
    
    // Get window drawable
    Pixmap pixmap = XCreatePixmap(req->display, req->window, 
                                 req->width, req->height,
                                 DefaultDepth(req->display, DefaultScreen(req->display)));
    
    // Create render picture
    XRenderPictFormat *fmt = XRenderFindStandardFormat(req->display, PictStandardRGB24);
    Picture dest = XRenderCreatePicture(req->display, pixmap, fmt, 0, NULL);
    
    // Create temporary XftDraw for this pixmap
    XftDraw *temp_xft_draw = XftDrawCreate(req->display, pixmap,
                                           DefaultVisual(req->display, DefaultScreen(req->display)),
                                           DefaultColormap(req->display, DefaultScreen(req->display)));
    
    // Clear background
    XRenderColor gray = GRAY;
    XRenderFillRectangle(req->display, PictOpSrc, dest, &gray, 
                        0, 0, req->width, req->height);
    
    // Draw ListView widget
    if (req->listview) {
        listview_draw(req->listview, req->display, dest, temp_xft_draw, req->font);
    } else {
        // ListView creation failed - this should never happen
        log_error("[ERROR] ListView widget not available in draw_window()");
    }
    
    // Draw input field labels if we have font
    if (req->font && temp_xft_draw) {
        XftColor label_color;
        XRenderColor black = BLACK;
        XftColorAllocValue(req->display, DefaultVisual(req->display, DefaultScreen(req->display)),
                          DefaultColormap(req->display, DefaultScreen(req->display)),
                          &black, &label_color);
        
        // Calculate label positions (right-justified, moved 5px to right to align with listview)
        int label_x_right = MARGIN + LABEL_WIDTH;
        
        // Pattern label
        int pattern_y = req->height - MARGIN - BUTTON_HEIGHT - SPACING - (3 * INPUT_HEIGHT) - (2 * SPACING);
        XGlyphInfo extents;
        XftTextExtentsUtf8(req->display, req->font, (FcChar8*)"Pattern:", 8, &extents);
        XftDrawStringUtf8(temp_xft_draw, &label_color, req->font,
                         label_x_right - extents.width,
                         pattern_y + (INPUT_HEIGHT + req->font->ascent - req->font->descent) / 2,
                         (FcChar8*)"Pattern:", 8);
        
        // Drawer label
        int drawer_y = req->height - MARGIN - BUTTON_HEIGHT - SPACING - (2 * INPUT_HEIGHT) - SPACING;
        XftTextExtentsUtf8(req->display, req->font, (FcChar8*)"Drawer:", 7, &extents);
        XftDrawStringUtf8(temp_xft_draw, &label_color, req->font,
                         label_x_right - extents.width,
                         drawer_y + (INPUT_HEIGHT + req->font->ascent - req->font->descent) / 2,
                         (FcChar8*)"Drawer:", 7);
        
        // File label
        int file_y = req->height - MARGIN - BUTTON_HEIGHT - SPACING - INPUT_HEIGHT;
        XftTextExtentsUtf8(req->display, req->font, (FcChar8*)"File:", 5, &extents);
        XftDrawStringUtf8(temp_xft_draw, &label_color, req->font,
                         label_x_right - extents.width,
                         file_y + (INPUT_HEIGHT + req->font->ascent - req->font->descent) / 2,
                         (FcChar8*)"File:", 5);
        
        XftColorFree(req->display, DefaultVisual(req->display, DefaultScreen(req->display)),
                    DefaultColormap(req->display, DefaultScreen(req->display)), &label_color);
    }
    
    // Update positions and draw input fields anchored to bottom
    int input_y = req->height - MARGIN - BUTTON_HEIGHT - SPACING;
    
    // File field (bottom-most input)
    input_y -= INPUT_HEIGHT;
    if (req->file_field) {
        req->file_field->x = MARGIN + LABEL_WIDTH + 5;
        req->file_field->y = input_y;
        req->file_field->width = req->width - MARGIN * 2 - LABEL_WIDTH - 5;
        req->file_field->height = INPUT_HEIGHT;
        // Only sync text if it has changed (to preserve cursor position)
        if (strcmp(req->file_field->text, req->file_text) != 0) {
            inputfield_set_text(req->file_field, req->file_text);
        }
        inputfield_render(req->file_field, dest, req->display, temp_xft_draw);
    }
    
    // Drawer field (middle input)
    input_y -= (INPUT_HEIGHT + SPACING);
    if (req->drawer_field) {
        req->drawer_field->x = MARGIN + LABEL_WIDTH + 5;
        req->drawer_field->y = input_y;
        req->drawer_field->width = req->width - MARGIN * 2 - LABEL_WIDTH - 5;
        req->drawer_field->height = INPUT_HEIGHT;
        // Only sync text if it has changed (to preserve cursor position)
        if (strcmp(req->drawer_field->text, req->drawer_text) != 0) {
            inputfield_set_text(req->drawer_field, req->drawer_text);
        }
        inputfield_render(req->drawer_field, dest, req->display, temp_xft_draw);
    }
    
    // Pattern field (top-most input)
    input_y -= (INPUT_HEIGHT + SPACING);
    if (req->pattern_field) {
        req->pattern_field->x = MARGIN + LABEL_WIDTH + 5;
        req->pattern_field->y = input_y;
        req->pattern_field->width = req->width - MARGIN * 2 - LABEL_WIDTH - 5;
        req->pattern_field->height = INPUT_HEIGHT;
        // Only sync text if it has changed (to preserve cursor position)
        if (strcmp(req->pattern_field->text, req->pattern_text) != 0) {
            inputfield_set_text(req->pattern_field, req->pattern_text);
        }
        inputfield_render(req->pattern_field, dest, req->display, temp_xft_draw);
    }
    
    // Draw buttons with dynamic spacing (moved down by 2 pixels)
    int button_y = req->height - MARGIN - BUTTON_HEIGHT + 2;
    
    // Calculate button positions to spread evenly
    // Left button stays at left, right button stays at right
    // Middle buttons spread evenly in remaining space
    int open_x = MARGIN;
    int cancel_x = req->width - MARGIN - BUTTON_WIDTH;
    
    // Calculate spacing for middle buttons
    int middle_space = cancel_x - (open_x + BUTTON_WIDTH);
    int middle_buttons_width = 2 * BUTTON_WIDTH;  // Volumes and Parent
    int middle_spacing = (middle_space - middle_buttons_width) / 3;  // 3 gaps: after Open, between middle, before Cancel
    
    // Ensure minimum spacing
    if (middle_spacing < SPACING) middle_spacing = SPACING;
    
    int volumes_x = open_x + BUTTON_WIDTH + middle_spacing;
    int parent_x = volumes_x + BUTTON_WIDTH + middle_spacing;
    
    Button open_btn = {
        .x = open_x, .y = button_y,
        .width = BUTTON_WIDTH, .height = BUTTON_HEIGHT,
        .label = req->is_save_mode ? "Save" : "Open",
        .pressed = req->open_button_pressed,
        .font = req->font
    };
    button_render(&open_btn, dest, req->display, temp_xft_draw);
    
    Button volumes_btn = {
        .x = volumes_x, .y = button_y,
        .width = BUTTON_WIDTH, .height = BUTTON_HEIGHT,
        .label = "Locations",
        .pressed = req->volumes_button_pressed,
        .font = req->font
    };
    button_render(&volumes_btn, dest, req->display, temp_xft_draw);
    
    Button parent_btn = {
        .x = parent_x, .y = button_y,
        .width = BUTTON_WIDTH, .height = BUTTON_HEIGHT,
        .label = "Parent",
        .pressed = req->parent_button_pressed,
        .font = req->font
    };
    button_render(&parent_btn, dest, req->display, temp_xft_draw);
    
    Button cancel_btn = {
        .x = cancel_x, .y = button_y,
        .width = BUTTON_WIDTH, .height = BUTTON_HEIGHT,
        .label = "Cancel",
        .pressed = req->cancel_button_pressed,
        .font = req->font
    };
    button_render(&cancel_btn, dest, req->display, temp_xft_draw);
    
    // Copy to window
    GC gc = XCreateGC(req->display, req->window, 0, NULL);
    XCopyArea(req->display, pixmap, req->window, gc, 
             0, 0, req->width, req->height, 0, 0);
    
    // Cleanup
    XFreeGC(req->display, gc);
    XftDrawDestroy(temp_xft_draw);
    XRenderFreePicture(req->display, dest);
    XFreePixmap(req->display, pixmap);

    // Draw multiselection rectangle on top (after window is drawn)
    draw_selection_rect(req);

    XFlush(req->display);
}

// Helper function to execute the appropriate action based on current ReqASL state
// Returns true if action was taken and dialog should close/continue
static bool reqasl_execute_action(ReqASL *req) {
    if (!req) return false;

    // Function called - debug removed per logging_system.md

    // Check if we should skip File field due to multi-selection
    bool is_multi_selection = (req->listview && req->multi_select_enabled && req->listview->selection_count > 1);
    
    // Priority 1: Check File field ONLY if not multi-selection
    if (!is_multi_selection && req->file_field && strlen(req->file_field->text) > 0) {
        char full_path[FULL_SIZE];
        
        // Build full path
        if (req->file_field->text[0] == '/' || req->file_field->text[0] == '~') {
            // Absolute path provided
            strncpy(full_path, req->file_field->text, sizeof(full_path) - 1);
            full_path[sizeof(full_path) - 1] = '\0';
        } else {
            // Relative to current directory
            snprintf(full_path, sizeof(full_path), "%s/%s", 
                    req->current_path, req->file_field->text);
        }
        
        if (req->is_save_mode) {
            // Save mode - save to this file
            if (req->on_open) {
                req->on_open(full_path);
            } else {
                printf("%s\n", full_path);  // Standalone mode output
            }
            reqasl_hide(req);
            return true;
        } else {
            // Open mode - file must exist
            struct stat st;
            if (stat(full_path, &st) == 0 && S_ISREG(st.st_mode)) {
                // It's a regular file - open it
                if (req->on_open) {
                    req->on_open(full_path);
                } else {
                    pid_t pid = fork();
                    if (pid == 0) {
                        execlp("xdg-open", "xdg-open", full_path, (char *)NULL);
                        _exit(EXIT_FAILURE);
                    }
                }
                reqasl_hide(req);
                return true;
            }
            // File doesn't exist or is not a regular file - continue to next priority
        }
    }
    
    // Priority 2: Check multi-selection
    if (req->listview && req->multi_select_enabled && req->listview->selection_count > 0) {
        int *selected_indices = malloc(req->listview->capacity * sizeof(int));
        if (!selected_indices) return false;
        int count = listview_get_selected_items(req->listview, selected_indices, req->listview->capacity);


        if (count > 0) {
            // Check if all selected items are files
            bool all_files = true;
            for (int i = 0; i < count; i++) {
                if (req->entries[selected_indices[i]]->type == TYPE_DRAWER) {
                    all_files = false;
                    break;
                }
            }
            
            if (all_files) {
                if (req->on_open) {
                    // Callback mode - return each path
                    for (int i = 0; i < count; i++) {
                        req->on_open(req->entries[selected_indices[i]]->path);
                    }
                    reqasl_hide(req);
                    free(selected_indices);
                    return true;
                } else {
                    // Standalone mode - check if all files use same xdg-open app

                    // Optimized approach: Group files by extension, check one file per unique extension
                    typedef struct {
                        const char *ext;
                        int file_index;
                        char app[NAME_SIZE];
                    } ExtGroup;

                    ExtGroup groups[20];  // Support up to 20 different extensions
                    int group_count = 0;

                    // Group files by extension
                    for (int i = 0; i < count && group_count < 20; i++) {
                        const char *ext = strrchr(req->entries[selected_indices[i]]->name, '.');
                        if (!ext) ext = "";  // Files without extension

                        // Check if we already have this extension
                        bool found = false;
                        for (int g = 0; g < group_count; g++) {
                            if (strcasecmp(groups[g].ext, ext) == 0) {
                                found = true;
                                break;
                            }
                        }

                        if (!found) {
                            groups[group_count].ext = ext;
                            groups[group_count].file_index = i;
                            group_count++;
                        }
                    }

                    // Now check one file per unique extension
                    char first_app[NAME_SIZE] = {0};
                    char first_mime[NAME_SIZE] = {0};
                    int mismatch_group = -1;

                    for (int g = 0; g < group_count; g++) {
                        int idx = groups[g].file_index;

                        // Get MIME type for this file
                        char mime_cmd[PATH_SIZE];
                        snprintf(mime_cmd, sizeof(mime_cmd),
                                "xdg-mime query filetype '%s' 2>/dev/null",
                                req->entries[selected_indices[idx]]->path);

                        FILE *mime_pipe = popen(mime_cmd, "r");
                        if (!mime_pipe) {
                            log_error("[ERROR] Failed to execute xdg-mime for: %s",
                                     req->entries[selected_indices[idx]]->path);
                            return false;
                        }

                        char mime_type[NAME_SIZE] = {0};
                        if (!fgets(mime_type, sizeof(mime_type), mime_pipe)) {
                            pclose(mime_pipe);
                            log_error("[ERROR] No MIME type found for: %s",
                                     req->entries[selected_indices[idx]]->path);
                            return false;
                        }
                        pclose(mime_pipe);

                        // Remove newline
                        char *nl = strchr(mime_type, '\n');
                        if (nl) *nl = '\0';

                        // Get default app for this MIME type
                        char app_cmd[PATH_SIZE];
                        snprintf(app_cmd, sizeof(app_cmd),
                                "xdg-mime query default '%s' 2>/dev/null", mime_type);

                        FILE *app_pipe = popen(app_cmd, "r");
                        if (!app_pipe) {
                            log_error("[ERROR] Failed to query default app for MIME type: %s", mime_type);
                            return false;
                        }

                        if (!fgets(groups[g].app, sizeof(groups[g].app), app_pipe)) {
                            pclose(app_pipe);
                            log_error("[ERROR] No default app for MIME type: %s", mime_type);
                            log_error("  File: %s", req->entries[selected_indices[idx]]->path);
                            return false;
                        }
                        pclose(app_pipe);

                        // Remove newline
                        nl = strchr(groups[g].app, '\n');
                        if (nl) *nl = '\0';

                        // Check if all extensions use the same app
                        if (g == 0) {
                            snprintf(first_app, sizeof(first_app), "%s", groups[g].app);
                            snprintf(first_mime, sizeof(first_mime), "%s", mime_type);
                        } else if (strcmp(first_app, groups[g].app) != 0) {
                            mismatch_group = g;
                            snprintf(first_mime, sizeof(first_mime), "%s", mime_type);  // Save for error message
                            break;
                        }
                    }

                    // If apps don't match, report the mismatch
                    if (mismatch_group >= 0) {
                        int idx1 = groups[0].file_index;
                        int idx2 = groups[mismatch_group].file_index;

                        log_error("[ERROR] Multi-open failed: files use different xdg-open apps");
                        log_error("  File: %s (extension: %s) -> app: %s",
                                 req->entries[selected_indices[idx1]]->name,
                                 groups[0].ext[0] ? groups[0].ext : "none",
                                 groups[0].app);
                        log_error("  File: %s (extension: %s) -> app: %s",
                                 req->entries[selected_indices[idx2]]->name,
                                 groups[mismatch_group].ext[0] ? groups[mismatch_group].ext : "none",
                                 groups[mismatch_group].app);
                        return false;
                    }

                    // All files have same app - need to find the actual executable
                    // since xdg-open only accepts ONE file at a time

                    // Extract the actual executable from the .desktop file
                    char exec_cmd[FULL_SIZE];
                    snprintf(exec_cmd, sizeof(exec_cmd),
                            "grep '^Exec=' /usr/share/applications/%s 2>/dev/null | "
                            "head -1 | sed 's/^Exec=//' | sed 's/ %%[fFuU].*//'",
                            first_app);

                    FILE *exec_pipe = popen(exec_cmd, "r");
                    char exec_line[PATH_SIZE] = {0};

                    if (exec_pipe && fgets(exec_line, sizeof(exec_line), exec_pipe)) {
                        pclose(exec_pipe);

                        // Remove newline
                        char *nl = strchr(exec_line, '\n');
                        if (nl) *nl = '\0';

                        if (strlen(exec_line) > 0) {
                            // Found the executable - use it directly
                            pid_t pid = fork();
                            if (pid == -1) {
                                log_error("[ERROR] Failed to fork process: %s", strerror(errno));
                                return false;
                            }

                            if (pid == 0) {
                                // Build args array for the actual application
                                char *args[count + 2];

                                // Parse the executable (might have spaces in command)
                                char *exe = strtok(exec_line, " ");
                                if (!exe) exe = exec_line;

                                args[0] = exe;
                                for (int i = 0; i < count; i++) {
                                    args[i + 1] = req->entries[selected_indices[i]]->path;
                                }
                                args[count + 1] = NULL;

                                execvp(exe, args);

                                // If direct exec failed, try xdg-open with just first file
                                execlp("xdg-open", "xdg-open",
                                      req->entries[selected_indices[0]]->path, (char *)NULL);

                                fprintf(stderr, "[ERROR] Failed to execute %s: %s\n", exe, strerror(errno));
                                _exit(EXIT_FAILURE);
                            }

                            reqasl_hide(req);
                            return true;
                        }
                    }

                    if (exec_pipe) pclose(exec_pipe);

                    // Fallback: couldn't find the actual executable, use xdg-open for first file only
                    log_error("[WARNING] Could not extract executable from %s, opening only first file", first_app);

                    pid_t pid = fork();
                    if (pid == -1) {
                        log_error("[ERROR] Failed to fork process: %s", strerror(errno));
                        return false;
                    }

                    if (pid == 0) {
                        execlp("xdg-open", "xdg-open",
                              req->entries[selected_indices[0]]->path, (char *)NULL);

                        fprintf(stderr, "[ERROR] xdg-open failed: %s\n", strerror(errno));
                        _exit(EXIT_FAILURE);
                    }

                    // Parent process
                    reqasl_hide(req);
                    free(selected_indices);
                    return true;
                }
            } else if (count == 1 && req->entries[selected_indices[0]]->type == TYPE_DRAWER) {
                // Single directory - open in workbench
                FileEntry *entry = req->entries[selected_indices[0]];
                Atom amiwb_open_dir = XInternAtom(req->display, "AMIWB_OPEN_DIRECTORY", False);
                Window root = DefaultRootWindow(req->display);

                XChangeProperty(req->display, root, amiwb_open_dir,
                              XA_STRING, 8, PropModeReplace,
                              (unsigned char *)entry->path, strlen(entry->path));
                XFlush(req->display);
                reqasl_hide(req);
                free(selected_indices);
                return true;
            }
            // Mixed selection or multiple directories - do nothing
            free(selected_indices);
            return false;
        }
    }
    
    // Priority 3: Single selection in listview
    if (req->listview && req->selected_index >= 0 &&
        req->selected_index < req->entry_count) {
        FileEntry *entry = req->entries[req->selected_index];

if (entry->type == TYPE_DRAWER) {
            // Directory - open in workbench
            Atom amiwb_open_dir = XInternAtom(req->display, "AMIWB_OPEN_DIRECTORY", False);
            Window root = DefaultRootWindow(req->display);

            XChangeProperty(req->display, root, amiwb_open_dir,
                          XA_STRING, 8, PropModeReplace,
                          (unsigned char *)entry->path, strlen(entry->path));
            XFlush(req->display);
        } else {
            // File - open with xdg-open or callback
            if (req->on_open) {
                req->on_open(entry->path);
            } else {
                pid_t pid = fork();
                if (pid == 0) {
                    execlp("xdg-open", "xdg-open", entry->path, (char *)NULL);
                    _exit(EXIT_FAILURE);
                }
            }
        }
        reqasl_hide(req);
        return true;
    }
    
    // Priority 4: Nothing selected - open current directory
Atom amiwb_open_dir = XInternAtom(req->display, "AMIWB_OPEN_DIRECTORY", False);
    Window root = DefaultRootWindow(req->display);

    XChangeProperty(req->display, root, amiwb_open_dir,
                  XA_STRING, 8, PropModeReplace,
                  (unsigned char *)req->current_path, strlen(req->current_path));
    XFlush(req->display);
    reqasl_hide(req);
    return true;
}

bool reqasl_handle_event(ReqASL *req, XEvent *event) {
    if (!req || !event) return false;
    
    switch (event->type) {
        case Expose:
            if (event->xexpose.count == 0) {
                // Check if expose is for a dropdown window
                if (req->drawer_field && inputfield_is_completion_window(req->drawer_field, event->xexpose.window)) {
                    inputfield_redraw_completion(req->drawer_field, req->display);
                    return true;
                }
                if (req->file_field && inputfield_is_completion_window(req->file_field, event->xexpose.window)) {
                    inputfield_redraw_completion(req->file_field, req->display);
                    return true;
                }
                
                // Normal window expose
                draw_window(req);
            }
            return true;
            
        case ButtonPress:
            // Check if click is on a dropdown window
            if (req->drawer_field && inputfield_is_completion_window(req->drawer_field, event->xbutton.window)) {
                // Handle scroll wheel on dropdown
                if (event->xbutton.button == Button4 || event->xbutton.button == Button5) {
                    int direction = (event->xbutton.button == Button4) ? -1 : 1;
                    inputfield_handle_dropdown_scroll(req->drawer_field, direction, req->display);
                    return true;
                }
                // Click is on drawer field's dropdown
                if (inputfield_handle_completion_click(req->drawer_field,
                                                      event->xbutton.x, event->xbutton.y, req->display)) {
                    // Selection was made, close dropdown
                    inputfield_hide_completions(req->drawer_field, req->display);
                    // Field text already updated by inputfield_handle_completion_click
                    // Update req->drawer_text to match the new field value
                    snprintf(req->drawer_text, PATH_SIZE, "%s", inputfield_get_text(req->drawer_field));
                    draw_window(req);
                }
                return true;
            }
            if (req->file_field && inputfield_is_completion_window(req->file_field, event->xbutton.window)) {
                // Handle scroll wheel on dropdown
                if (event->xbutton.button == Button4 || event->xbutton.button == Button5) {
                    int direction = (event->xbutton.button == Button4) ? -1 : 1;
                    inputfield_handle_dropdown_scroll(req->file_field, direction, req->display);
                    return true;
                }
                // Click is on file field's dropdown
                if (inputfield_handle_completion_click(req->file_field,
                                                      event->xbutton.x, event->xbutton.y, req->display)) {
                    // Selection was made, close dropdown
                    inputfield_hide_completions(req->file_field, req->display);
                    // Field text already updated by inputfield_handle_completion_click
                    // Update req->file_text to match the new field value
                    snprintf(req->file_text, PATH_SIZE, "%s", inputfield_get_text(req->file_field));
                    draw_window(req);
                }
                return true;
            }
            
            // Check if any dropdown is open - click outside should close it
            if ((req->drawer_field && inputfield_has_dropdown_open(req->drawer_field)) ||
                (req->file_field && inputfield_has_dropdown_open(req->file_field))) {
                // Close any open dropdowns on click outside
                if (req->drawer_field) inputfield_hide_completions(req->drawer_field, req->display);
                if (req->file_field) inputfield_hide_completions(req->file_field, req->display);
                draw_window(req);
                // Don't return - let the click be processed normally
            }
            
            // Handle scroll wheel
            if (event->xbutton.button == Button4 || event->xbutton.button == Button5) {
                if (req->listview) {
                    int direction = (event->xbutton.button == Button4) ? -1 : 1;
                    if (listview_handle_scroll(req->listview, direction)) {
                        draw_window(req);
                        return true;
                    }
                }
            }
            
            // Handle right-click for Locations/current directory toggle
            if (event->xbutton.button == Button3) {
                // Check if click is in listview area
                if (req->listview &&
                    event->xbutton.x >= req->listview->x &&
                    event->xbutton.x < req->listview->x + req->listview->width &&
                    event->xbutton.y >= req->listview->y &&
                    event->xbutton.y < req->listview->y + req->listview->height) {

                    // Toggle between Locations view and current directory
                    if (req->showing_locations) {
                        // Return to previous directory
                        req->showing_locations = false;
                        snprintf(req->current_path, sizeof(req->current_path), "%s", req->previous_path);
                        snprintf(req->drawer_text, sizeof(req->drawer_text), "%s", req->previous_path);

                        // Update drawer field
                        if (req->drawer_field) {
                            inputfield_set_text(req->drawer_field, req->drawer_text);
                            inputfield_scroll_to_end(req->drawer_field);
                        }

                        // Refresh the normal directory view
                        scan_directory(req, req->current_path);
                    } else {
                        // Save current path and switch to Locations view
                        snprintf(req->previous_path, sizeof(req->previous_path), "%s", req->current_path);
                        build_locations_view(req);
                    }

                    // Update the ListView
                    if (req->listview) {
                        listview_clear(req->listview);
            for (int i = 0; i < req->entry_count; i++) {
                FileEntry *fe = req->entries[i];
                listview_add_item(req->listview, fe->name,
                                fe->type == TYPE_DRAWER, fe);
            }
                    }

                    // Update menu states after view change
                    reqasl_update_menu_states(req);
                    draw_window(req);
                    return true;
                }
            }
            
            if (event->xbutton.button == Button1) {
                int x = event->xbutton.x;
                int y = event->xbutton.y;
                
                // Handle input field clicks first
                bool field_clicked = false;
                
                if (req->pattern_field && inputfield_handle_click(req->pattern_field, x, y)) {
                    // Clear focus from other fields
                    if (req->drawer_field) inputfield_set_focus(req->drawer_field, false);
                    if (req->file_field) inputfield_set_focus(req->file_field, false);
                    field_clicked = true;
                    draw_window(req);
                    XFlush(req->display);  // Force immediate update
                    return true;
                }
                
                if (req->drawer_field && inputfield_handle_click(req->drawer_field, x, y)) {
                    // Clear focus from other fields
                    if (req->pattern_field) inputfield_set_focus(req->pattern_field, false);
                    if (req->file_field) inputfield_set_focus(req->file_field, false);
                    field_clicked = true;
                    draw_window(req);
                    XFlush(req->display);  // Force immediate update
                    return true;
                }
                
                if (req->file_field && inputfield_handle_click(req->file_field, x, y)) {
                    // Clear focus from other fields
                    if (req->pattern_field) inputfield_set_focus(req->pattern_field, false);
                    if (req->drawer_field) inputfield_set_focus(req->drawer_field, false);
                    field_clicked = true;
                    draw_window(req);
                    XFlush(req->display);  // Force immediate update
                    return true;
                }
                
                // If no field was clicked, clear all focus
                if (!field_clicked) {
                    bool had_focus = false;
                    if (req->pattern_field && req->pattern_field->has_focus) {
                        inputfield_set_focus(req->pattern_field, false);
                        had_focus = true;
                    }
                    if (req->drawer_field && req->drawer_field->has_focus) {
                        inputfield_set_focus(req->drawer_field, false);
                        had_focus = true;
                    }
                    if (req->file_field && req->file_field->has_focus) {
                        inputfield_set_focus(req->file_field, false);
                        had_focus = true;
                    }
                    // Redraw if we removed focus from any field
                    if (had_focus) {
                        draw_window(req);
                    }
                }
                
                // Handle ListView click if available
                if (req->listview) {
                    // Use the time-aware click handler for proper double-click detection
                    bool click_handled = listview_handle_click_with_time(req->listview, x, y,
                                                       event->xbutton.state,
                                                       event->xbutton.time,
                                                       req->display,
                                                       req->font);

                    if (click_handled) {
                        // Check if ListView cleared selection or has no selection (empty space click)
                        if (req->listview->selected_index == -1) {
                            // Empty space was clicked - start multiselection
                            multiselect_pending = true;
                            multiselect_start_x = x;
                            multiselect_start_y = y;
                            multiselect_target = req;
                        }
                        draw_window(req);
                        return true;
                    }
                }
                
                // Check if pressing a button and set pressed state
                int button_y = req->height - MARGIN - BUTTON_HEIGHT;
                int total_button_width = 4 * BUTTON_WIDTH;
                int available_space = req->width - 2 * MARGIN;
                int total_spacing = available_space - total_button_width;
                int middle_spacing = total_spacing / 3;
                
                int open_x = MARGIN;
                int volumes_x = open_x + BUTTON_WIDTH + middle_spacing;
                int parent_x = volumes_x + BUTTON_WIDTH + middle_spacing;
                int cancel_x = req->width - MARGIN - BUTTON_WIDTH;
                
                if (y >= button_y && y < button_y + BUTTON_HEIGHT) {
                    if (x >= open_x && x < open_x + BUTTON_WIDTH) {
req->open_button_pressed = true;
                        draw_window(req);
                        return true;
                    } else if (x >= volumes_x && x < volumes_x + BUTTON_WIDTH) {
                        req->volumes_button_pressed = true;
                        draw_window(req);
                        return true;
                    } else if (x >= parent_x && x < parent_x + BUTTON_WIDTH) {
                        req->parent_button_pressed = true;
                        draw_window(req);
                        return true;
                    } else if (x >= cancel_x && x < cancel_x + BUTTON_WIDTH) {
                        req->cancel_button_pressed = true;
                        draw_window(req);
                        return true;
                    }
                }
            }
            break;
            
        case MotionNotify:
            // Handle scrollbar dragging in ListView
            if (req->listview) {
                if (listview_handle_motion(req->listview, event->xmotion.x, event->xmotion.y)) {
                    draw_window(req);
                    return true;
                }
            }

            // Handle multiselection motion
            if (req == multiselect_target && (multiselect_pending || multiselect_active)) {
                int x = event->xmotion.x;
                int y = event->xmotion.y;

                // Check 10px threshold before activating
                if (multiselect_pending && !multiselect_active) {
                    int dx = x - multiselect_start_x;
                    int dy = y - multiselect_start_y;

                    // Require 10 pixel movement (prevents accidental activation)
                    if (dx*dx + dy*dy >= 10*10) {
                        // Threshold crossed - activate multiselection
                        multiselect_start(req);
                    }
                }

                // Update rectangle if active
                if (multiselect_active) {
                    multiselect_update(req, x, y);
                    return true;
                }
            }
            break;
            
        case ButtonRelease:
            // Handle scrollbar release in ListView
            if (req->listview) {
                if (listview_handle_release(req->listview)) {
                    draw_window(req);
                    return true;
                }
            }

            // Handle multiselection release
            if (req == multiselect_target && (multiselect_pending || multiselect_active)) {
                if (multiselect_active) {
                    // Active state - complete selection
                    multiselect_end();
                } else {
                    // Pending state - below threshold, just clear flags
                    multiselect_pending = false;
                    multiselect_target = NULL;
                }
                // Note: Don't return here - let button handling proceed
            }

            // Handle button clicks on release
            if (event->xbutton.button == Button1) {
                int x = event->xbutton.x;
                int y = event->xbutton.y;
                
                // Calculate button positions (same as in ButtonPress)
                int button_y = req->height - MARGIN - BUTTON_HEIGHT;
                int total_button_width = 4 * BUTTON_WIDTH;
                int available_space = req->width - 2 * MARGIN;
                int total_spacing = available_space - total_button_width;
                int middle_spacing = total_spacing / 3;
                
                int open_x = MARGIN;
                int volumes_x = open_x + BUTTON_WIDTH + middle_spacing;
                int parent_x = volumes_x + BUTTON_WIDTH + middle_spacing;
                int cancel_x = req->width - MARGIN - BUTTON_WIDTH;
                
                bool need_redraw = false;
                
                // Check Open button
                if (req->open_button_pressed) {
                    req->open_button_pressed = false;
                    need_redraw = true;

                    if (x >= open_x && x < open_x + BUTTON_WIDTH &&
                        y >= button_y && y < button_y + BUTTON_HEIGHT) {
// Use helper function to execute the appropriate action
                        reqasl_execute_action(req);
                    }
                }
                
                // Check Volumes button
                if (req->volumes_button_pressed) {
                    req->volumes_button_pressed = false;
                    need_redraw = true;
                    
                    if (x >= volumes_x && x < volumes_x + BUTTON_WIDTH &&
                        y >= button_y && y < button_y + BUTTON_HEIGHT) {
                        // Save current path and switch to Locations view
                        if (!req->showing_locations) {
                            snprintf(req->previous_path, sizeof(req->previous_path), "%s", req->current_path);
                            build_locations_view(req);

                            // Update the ListView with locations
                            if (req->listview) {
                                listview_clear(req->listview);
            for (int i = 0; i < req->entry_count; i++) {
                FileEntry *fe = req->entries[i];
                listview_add_item(req->listview, fe->name,
                                fe->type == TYPE_DRAWER, fe);
            }
                            }

                            // Update menu states after switching to Locations view
                            reqasl_update_menu_states(req);
                        }
                        draw_window(req);
                        return true;
                    }
                }
                
                // Check Parent button
                if (req->parent_button_pressed) {
                    req->parent_button_pressed = false;
                    need_redraw = true;
                    
                    if (x >= parent_x && x < parent_x + BUTTON_WIDTH &&
                        y >= button_y && y < button_y + BUTTON_HEIGHT) {
                        reqasl_navigate_parent(req);
                        return true;
                    }
                }
                
                // Check Cancel button
                if (req->cancel_button_pressed) {
                    req->cancel_button_pressed = false;
                    need_redraw = true;
                    
                    if (x >= cancel_x && x < cancel_x + BUTTON_WIDTH &&
                        y >= button_y && y < button_y + BUTTON_HEIGHT) {
                        if (req->on_cancel) {
                            req->on_cancel();
                        }
                        reqasl_hide(req);
                        return true;
                    }
                }
                
                // Redraw if any button was released (to clear pressed state)
                if (need_redraw) {
                    draw_window(req);
                }
            }
            break;
            
        case MapNotify:
            // When window is mapped, ensure size hints are properly set
            {
                XSizeHints *size_hints = XAllocSizeHints();
                if (size_hints) {
                    size_hints->flags = PMinSize | PMaxSize | PBaseSize | PSize;
                    size_hints->min_width = REQASL_MIN_WIDTH;
                    size_hints->min_height = REQASL_MIN_HEIGHT;
                    size_hints->max_width = 1920;
                    size_hints->max_height = 1080;
                    size_hints->base_width = REQASL_MIN_WIDTH;
                    size_hints->base_height = REQASL_MIN_HEIGHT;
                    size_hints->width = req->width;
                    size_hints->height = req->height;
                    XSetWMSizeHints(req->display, req->window, size_hints, XA_WM_NORMAL_HINTS);
                    XSetWMNormalHints(req->display, req->window, size_hints);
                    XFree(size_hints);
                }
            }
            return true;
            
        case ConfigureRequest:
            // Handle resize requests - enforce minimum size
            {
                // Configure request received
                
                XWindowChanges changes;
                changes.x = event->xconfigurerequest.x;
                changes.y = event->xconfigurerequest.y;
                changes.width = event->xconfigurerequest.width;
                changes.height = event->xconfigurerequest.height;
                changes.border_width = event->xconfigurerequest.border_width;
                changes.sibling = event->xconfigurerequest.above;
                changes.stack_mode = event->xconfigurerequest.detail;
                
                // Enforce minimum dimensions
                if (changes.width < REQASL_MIN_WIDTH) {
                    changes.width = REQASL_MIN_WIDTH;
                }
                if (changes.height < REQASL_MIN_HEIGHT) {
                    changes.height = REQASL_MIN_HEIGHT;
                }
                
                XConfigureWindow(req->display, req->window, 
                                event->xconfigurerequest.value_mask, &changes);
            }
            return true;
            
        case KeyPress:
            // Handle keyboard input
            {
                KeySym keysym = XLookupKeysym(&event->xkey, 0);
                unsigned int state = event->xkey.state;

                // Check for Super (Mod4) key combinations
                if (state & Mod4Mask) {
                    switch (keysym) {
                        case XK_a:  // Super+A - Select Files
                            if (req->listview && req->multi_select_enabled) {
                                // Clear previous selections first
                                listview_clear_selection(req->listview);
                                // Select only files, not directories
                                int file_count = 0;
                                for (int i = 0; i < req->entry_count && i < req->listview->item_count; i++) {
                                    if (req->entries[i]->type == TYPE_FILE) {
                                        req->listview->selected[i] = true;
                                        file_count++;
                                    }
                                }
                                req->listview->selection_count = file_count;

                                // Update file field display
                                if (file_count == 0) {
                                    req->file_text[0] = '\0';
                                    if (req->file_field) {
                                        inputfield_set_text(req->file_field, "");
                                    }
                                } else if (file_count == 1) {
                                    // Find the single selected file
                                    for (int i = 0; i < req->entry_count; i++) {
                                        if (req->listview->selected[i]) {
                                            strncpy(req->file_text, req->entries[i]->name, sizeof(req->file_text) - 1);
                                            if (req->file_field) {
                                                inputfield_set_text(req->file_field, req->file_text);
                                            }
                                            break;
                                        }
                                    }
                                } else {
                                    // Multiple files selected
                                    snprintf(req->file_text, sizeof(req->file_text), "%d files selected", file_count);
                                    if (req->file_field) {
                                        inputfield_set_text(req->file_field, req->file_text);
                                    }
                                }

                                reqasl_update_menu_states(req);
                                draw_window(req);
                            }
                            return true;

                        case XK_z:  // Super+Z - Select None
                            if (req->listview && req->multi_select_enabled) {
                                listview_clear_selection(req->listview);
                                req->file_text[0] = '\0';
                                if (req->file_field) {
                                    inputfield_set_text(req->file_field, "");
                                }
                                reqasl_update_menu_states(req);
                                draw_window(req);
                            }
                            return true;

                        case XK_h:  // Super+H - Toggle Show Hidden
                            req->show_hidden = !req->show_hidden;
                            reqasl_update_menu_data(req);  // This updates the checkmark
                            reqasl_update_menu_states(req);
                            scan_directory(req, req->current_path);
                            draw_window(req);
                            return true;
                    }
                }

                // Handle Escape key - clear selection first, then quit
                if (keysym == XK_Escape) {
                    // Check if any dropdown is open - if so, let it handle Escape
                    if (req->drawer_field && inputfield_has_dropdown_open(req->drawer_field)) {
                        inputfield_handle_key(req->drawer_field, &event->xkey);
                        draw_window(req);
                        return true;
                    }
                    if (req->file_field && inputfield_has_dropdown_open(req->file_field)) {
                        inputfield_handle_key(req->file_field, &event->xkey);
                        draw_window(req);
                        return true;
                    }
                    
                    // Check if there's any selection (multi or single)
                    bool has_selection = false;
                    if (req->listview) {
                        if (req->multi_select_enabled && req->listview->selection_count > 0) {
                            has_selection = true;
                        } else if (!req->multi_select_enabled && req->listview->selected_index >= 0) {
                            has_selection = true;
                        }
                    }
                    
                    if (has_selection) {
                        // Clear the selection
                        if (req->listview) {
                            if (req->multi_select_enabled) {
                                listview_clear_selection(req->listview);
                            } else {
                                req->listview->selected_index = -1;
                            }
                        }
                        // Clear file text field when deselecting
                        req->file_text[0] = '\0';
                        if (req->file_field) {
                            inputfield_set_text(req->file_field, "");
                        }
                        draw_window(req);
                        return true;
                    } else {
                        // Nothing selected - quit ReqASL
                        if (req->on_cancel) {
                            req->on_cancel();
                        }
                        reqasl_hide(req);
                        return true;
                    }
                }
                
                // Check if any dropdown is open - if so, let it handle arrow keys
                if (keysym == XK_Up || keysym == XK_Down) {
                    if (req->drawer_field && inputfield_has_dropdown_open(req->drawer_field)) {
                        inputfield_handle_key(req->drawer_field, &event->xkey);
                        draw_window(req);
                        return true;
                    }
                    if (req->file_field && inputfield_has_dropdown_open(req->file_field)) {
                        inputfield_handle_key(req->file_field, &event->xkey);
                        draw_window(req);
                        return true;
                    }
                }
                
                // Handle arrow keys for listview navigation
                if (keysym == XK_Up || keysym == XK_Down) {
                    if (req->listview && req->listview->item_count > 0) {
                        // Check if Shift is pressed - scroll view without changing selection
                        if (event->xkey.state & ShiftMask) {
                            // Shift+Arrow: scroll view only
                            if (keysym == XK_Up) {
                                if (req->listview->scroll_offset > 0) {
                                    req->listview->scroll_offset--;
                                    listview_update_scrollbar(req->listview);
                                    draw_window(req);
                                }
                            } else { // XK_Down
                                int max_scroll = req->listview->item_count - req->listview->visible_items;
                                if (max_scroll < 0) max_scroll = 0;
                                if (req->listview->scroll_offset < max_scroll) {
                                    req->listview->scroll_offset++;
                                    listview_update_scrollbar(req->listview);
                                    draw_window(req);
                                }
                            }
                        } else {
                            // Regular arrow keys: change selection
                            int new_index = req->listview->selected_index;
                            
                            if (keysym == XK_Up) {
                                new_index--;
                                if (new_index < 0) new_index = 0;
                            } else { // XK_Down
                                new_index++;
                                if (new_index >= req->listview->item_count) {
                                    new_index = req->listview->item_count - 1;
                                }
                            }
                            
                            listview_set_selected(req->listview, new_index);
                            listview_ensure_visible(req->listview, new_index);
                            draw_window(req);
                        }
                        return true;
                    }
                }
                
                // Pass keyboard events to input fields FIRST
                // This ensures input fields get priority for handling keys
		// bool handled_by_field = false;
                
                if (req->pattern_field && req->pattern_field->has_focus) {
                    if (inputfield_handle_key(req->pattern_field, &event->xkey)) {
                        // Sync field text back to buffer
                        strncpy(req->pattern_text, inputfield_get_text(req->pattern_field), 
                               sizeof(req->pattern_text) - 1);
                        req->pattern_text[sizeof(req->pattern_text) - 1] = '\0';
                        
                        // If Enter was pressed in pattern field, refresh the file list
                        // Note: InputField widget automatically removes focus on Enter
                        if (keysym == XK_Return || keysym == XK_KP_Enter) {
                            scan_directory(req, req->current_path);
                        }
                        
                        draw_window(req);
                        return true;
                    }
                }
                
                if (req->drawer_field && req->drawer_field->has_focus) {
                    if (inputfield_handle_key(req->drawer_field, &event->xkey)) {
                        // Sync field text back to buffer
                        strncpy(req->drawer_text, inputfield_get_text(req->drawer_field),
                               sizeof(req->drawer_text) - 1);
                        req->drawer_text[sizeof(req->drawer_text) - 1] = '\0';
                        
                        // Update file field's base directory for completion
                        if (req->file_field) {
                            inputfield_set_completion_base_dir(req->file_field, req->drawer_text);
                        }
                        
                        // If Enter was pressed in drawer field, navigate to path
                        // Note: InputField widget automatically removes focus on Enter
                        if (keysym == XK_Return || keysym == XK_KP_Enter) {
                            reqasl_navigate_to(req, req->drawer_text);
                        }
                        
                        draw_window(req);
                        return true;
                    }
                }
                
                if (req->file_field && req->file_field->has_focus) {
                    if (inputfield_handle_key(req->file_field, &event->xkey)) {
                        // Sync field text back to buffer
                        strncpy(req->file_text, inputfield_get_text(req->file_field),
                               sizeof(req->file_text) - 1);
                        req->file_text[sizeof(req->file_text) - 1] = '\0';
                        
                        // Note: InputField widget automatically removes focus on Enter
                        
                        draw_window(req);
                        return true;
                    }
                }
                
                // Check if any input field has focus
                bool any_field_focused = false;
                if (req->pattern_field && req->pattern_field->has_focus) any_field_focused = true;
                if (req->drawer_field && req->drawer_field->has_focus) any_field_focused = true;
                if (req->file_field && req->file_field->has_focus) any_field_focused = true;
                
                // Handle Return key for opening files/directories (only if not editing)
                if (keysym == XK_Return && !any_field_focused) {
                    // Use helper function to execute the appropriate action
                    return reqasl_execute_action(req);
                }
                
                // Handle Backspace key - go to parent directory (only if not editing)
                // Note: any_field_focused was already checked above for Return key
                if (keysym == XK_BackSpace && !any_field_focused) {
                    reqasl_navigate_parent(req);
                    return true;
                }
                
                // Try to handle keyboard input for fields that don't have focus yet
                // This allows Tab key or other navigation to give focus to fields
                if (!any_field_focused) {
                    // Try each field to see if it wants to handle this key
                    if (req->pattern_field && inputfield_handle_key(req->pattern_field, &event->xkey)) {
                        draw_window(req);
                        return true;
                    }
                    if (req->drawer_field && inputfield_handle_key(req->drawer_field, &event->xkey)) {
                        draw_window(req);
                        return true;
                    }
                    if (req->file_field && inputfield_handle_key(req->file_field, &event->xkey)) {
                        draw_window(req);
                        return true;
                    }
                }
            }
            break;
            
        case ConfigureNotify:
            // Handle window resize
            {
                int new_width = event->xconfigure.width;
                int new_height = event->xconfigure.height;
                
                // Check if size actually changed
                if (new_width != req->width || new_height != req->height) {
                    // Window resized
                    
                    // Update internal dimensions
                    req->width = new_width;
                    req->height = new_height;
                    
                    // Recalculate list area height (grows with window)
                    req->list_height = req->height - MARGIN - (3 * INPUT_HEIGHT) -
                                      (4 * SPACING) - BUTTON_HEIGHT - MARGIN;

                    // Update ListView dimensions
                    if (req->listview) {
                        req->listview->width = req->width - 2 * MARGIN;
                        req->listview->height = req->list_height;
                        listview_update_scrollbar(req->listview);
                    }
                    
                    // Update InputField dimensions and positions
                    int input_width = req->width - MARGIN * 2 - LABEL_WIDTH;
                    int input_y = req->list_y + req->list_height + SPACING;
                    
                    if (req->pattern_field) {
                        req->pattern_field->y = input_y;
                        inputfield_update_size(req->pattern_field, input_width);
                    }
                    
                    if (req->drawer_field) {
                        req->drawer_field->y = input_y + INPUT_HEIGHT + SPACING;
                        inputfield_update_size(req->drawer_field, input_width);
                        // Ensure drawer path stays visible at the right
                        inputfield_scroll_to_end(req->drawer_field);
                    }
                    
                    if (req->file_field) {
                        req->file_field->y = input_y + 2 * (INPUT_HEIGHT + SPACING);
                        inputfield_update_size(req->file_field, input_width);
                        // Ensure file name stays visible at the right if it's long
                        if (strlen(req->file_field->text) > 20) {
                            inputfield_scroll_to_end(req->file_field);
                        }
                    }
                    
                    // Redraw everything with new layout
                    draw_window(req);
                }
            }
            return true;
            
        case ClientMessage:
            // Handle window close
            if ((Atom)event->xclient.data.l[0] == 
                XInternAtom(req->display, "WM_DELETE_WINDOW", False)) {
                if (req->on_cancel) {
                    req->on_cancel();
                }
                reqasl_hide(req);
                return true;
            }
            // Handle menu selection from AmiWB
            else if (event->xclient.message_type == 
                     XInternAtom(req->display, "_AMIWB_MENU_SELECT", False)) {
                int menu_index = event->xclient.data.l[0];
                int item_index = event->xclient.data.l[1];
                
                // Handle File menu (index 0)
                if (menu_index == 0) {
                    switch (item_index) {
                        case 0:  // Open - same as OK button/double-click
                            reqasl_execute_action(req);
                            break;
                        case 1:  // Quit
                            req->is_open = false;
                            break;
                    }
                }
                // Handle Edit menu (index 1)
                else if (menu_index == 1) {
                    switch (item_index) {
                        case 0:  // New Drawer
                            // TODO: Create dialog to get drawer name
                            // TODO: Create directory in current path
                            // TODO: Refresh file list
                            break;
                        case 1:  // Rename
                            // TODO: Create rename dialog
                            // TODO: Rename selected file/directory
                            // TODO: Refresh file list
                            break;
                        case 2:  // Delete
                            // TODO: Create warning dialog
                            // TODO: Delete selected files/directories
                            // TODO: Refresh file list
                            break;
                        case 3:  // Select Files
                            if (req->listview && req->multi_select_enabled) {
                                // Clear previous selections first
                                listview_clear_selection(req->listview);

                                // Select only files, not directories
                                int file_count = 0;
                                for (int i = 0; i < req->entry_count && i < req->listview->item_count; i++) {
                                    if (req->entries[i]->type == TYPE_FILE) {
                                        req->listview->selected[i] = true;
                                        file_count++;
                                    }
                                }
                                req->listview->selection_count = file_count;
                                req->listview->needs_redraw = true;

                                // Update file field based on selection
                                if (file_count > 0) {
                                    char display_text[NAME_SIZE];
                                    if (file_count == 1) {
                                        // Find the single selected file
                                        for (int i = 0; i < req->entry_count; i++) {
                                            if (req->entries[i]->type == TYPE_FILE && req->listview->selected[i]) {
                                                snprintf(display_text, sizeof(display_text), "%s", req->entries[i]->name);
                                                break;
                                            }
                                        }
                                    } else {
                                        snprintf(display_text, sizeof(display_text), "%d files selected", file_count);
                                    }
                                    snprintf(req->file_text, PATH_SIZE, "%s", display_text);
                                    if (req->file_field) {
                                        inputfield_set_text(req->file_field, display_text);
                                    }
                                } else {
                                    // No files, clear the field
                                    req->file_text[0] = '\0';
                                    if (req->file_field) {
                                        inputfield_set_text(req->file_field, "");
                                    }
                                }

                                draw_window(req);
                            }
                            break;
                        case 4:  // Select None
                            if (req->listview && req->multi_select_enabled) {
                                // Clear all selections
                                listview_clear_selection(req->listview);
                                // Clear the file field
                                req->file_text[0] = '\0';
                                if (req->file_field) {
                                    inputfield_set_text(req->file_field, "");
                                }
                                draw_window(req);
                            }
                            break;
                    }
                }
                // Handle View menu (index 2)
                else if (menu_index == 2) {
                    switch (item_index) {
                        case 0:  // By Names
                            // TODO: Implement alphabetical sorting
                            break;
                        case 1:  // By Date
                            // TODO: Implement date sorting
                            break;
                        case 2:  // Show Hidden
                            // Toggle show_hidden state
                            req->show_hidden = !req->show_hidden;
                            // Update menu to show/hide checkmark
                            reqasl_update_menu_data(req);
                            // Refresh directory listing with new hidden state
                            reqasl_refresh(req);
                            break;
                    }
                }
                // Handle Locations menu (index 3)
                else if (menu_index == 3) {
                    switch (item_index) {
                        case 0:  // Add Place
                            add_user_place(req, req->current_path);
                            // Update menu states after adding place
                            reqasl_update_menu_states(req);
                            break;
                        case 1:  // Del Place
                            remove_user_place(req, req->current_path);
                            // Update menu states after removing place
                            reqasl_update_menu_states(req);
                            break;
                    }
                }
                return true;
            }
            break;
    }
    
    return false;
}

static void listview_select_callback(int index, const char *text, void *user_data) {
    ReqASL *req = (ReqASL *)user_data;
    if (!req) return;

    // Handle selection cleared (index -1)
    if (index < 0) {
        req->selected_index = -1;
        // Clear the file field
        req->file_text[0] = '\0';
        if (req->file_field) {
            inputfield_set_text(req->file_field, "");
        }
        return;
    }

    if (index >= req->entry_count) return;

    req->selected_index = index;
    
    
    // In multi-select mode, always build the list from all selected items
    if (req->listview && req->multi_select_enabled) {
        // If no items selected, clear the file field
        if (req->listview->selection_count == 0) {
            req->file_text[0] = '\0';
            if (req->file_field) {
                inputfield_set_text(req->file_field, "");
            }
        } else {
            // Count selected files and find the single file if only one
            int file_count = 0;
            int single_file_index = -1;
            for (int i = 0; i < req->entry_count && i < req->listview->capacity; i++) {
                if (req->listview->selected[i] && req->entries[i]->type == TYPE_FILE) {
                    if (file_count == 0) {
                        single_file_index = i;
                    }
                    file_count++;
                }
            }
            
            // Show filename for single selection, count for multi-selection
            char display_text[NAME_SIZE];
            if (file_count == 1 && single_file_index >= 0) {
                // Single file - show its name
                snprintf(display_text, sizeof(display_text), "%s", req->entries[single_file_index]->name);
            } else if (file_count > 1) {
                // Multiple files - show count
                snprintf(display_text, sizeof(display_text), "%d files selected", file_count);
            } else {
                // No files selected (shouldn't happen in this branch)
                display_text[0] = '\0';
            }
            
            // Update the file field
            snprintf(req->file_text, PATH_SIZE, "%s", display_text);
            if (req->file_field) {
                inputfield_set_text(req->file_field, display_text);
            }
        }
    } else {
        // Single selection - original behavior
        FileEntry *entry = req->entries[index];
        
        // Update file field with selection if it's a file
        if (entry->type == TYPE_FILE) {
            snprintf(req->file_text, PATH_SIZE, "%s", entry->name);
            // Also update the InputField widget
            if (req->file_field) {
                inputfield_set_text(req->file_field, entry->name);
            }
        }
    }
    
    // Redraw to update input fields
    draw_window(req);
    
    // Update menu states when selection changes
    reqasl_update_menu_states(req);
}

static void listview_double_click_callback(int index, const char *text, void *user_data) {
    ReqASL *req = (ReqASL *)user_data;
    if (!req || index < 0 || index >= req->entry_count) return;

    FileEntry *entry = req->entries[index];

    // Check if we're in Locations view
    if (req->showing_locations) {
        // Skip headers and blank lines (they have path == NULL or size == -1)
        if (!entry->path || entry->size == -1) {
            return;  // Headers and separators are not clickable
        }

        // Navigate to the location
        req->showing_locations = false;  // Exit Locations view
        reqasl_navigate_to(req, entry->path);
        return;
    }

    // Normal file/directory handling
    if (entry->type == TYPE_DRAWER) {
        // Navigate into directory
        reqasl_navigate_to(req, entry->path);
    } else {
        // Open file - check if we have a callback or should use xdg-open
        if (req->on_open) {
            // Callback mode - return path to caller
            req->on_open(entry->path);
            reqasl_hide(req);
        } else {
            // Standalone mode - open with xdg-open
            pid_t pid = fork();
            if (pid == 0) {
                execlp("xdg-open", "xdg-open", entry->path, (char *)NULL);
                _exit(EXIT_FAILURE);
            }
            reqasl_hide(req);
        }
    }
}

// Set pattern filter from simple extensions list (e.g. "avi,mp4,mkv")
// Converts to wildcard format (e.g. "*.avi,*.mp4,*.mkv")
void reqasl_set_pattern(ReqASL *req, const char *extensions) {
    if (!req || !extensions) return;
    
    char pattern_buffer[NAME_SIZE * 2] = {0};  // Pattern input buffer
    char ext_copy[NAME_SIZE * 2];  // Extension buffer
    strncpy(ext_copy, extensions, sizeof(ext_copy) - 1);
    
    // Parse comma-separated extensions
    char *token = strtok(ext_copy, ",");
    bool first = true;
    
    while (token) {
        // Trim leading/trailing spaces
        while (*token == ' ') token++;
        char *end = token + strlen(token) - 1;
        while (end > token && *end == ' ') *end-- = '\0';
        
        // Add to pattern buffer
        if (!first) {
            strncat(pattern_buffer, ",", sizeof(pattern_buffer) - strlen(pattern_buffer) - 1);
        }
        strncat(pattern_buffer, "*.", sizeof(pattern_buffer) - strlen(pattern_buffer) - 1);
        strncat(pattern_buffer, token, sizeof(pattern_buffer) - strlen(pattern_buffer) - 1);
        
        first = false;
        token = strtok(NULL, ",");
    }
    
    // Set the pattern in the input field
    if (req->pattern_field) {
        inputfield_set_text(req->pattern_field, pattern_buffer);
        inputfield_set_disabled(req->pattern_field, false);  // Enable the field
    }
    
    // Store in pattern_text as well
    strncpy(req->pattern_text, pattern_buffer, sizeof(req->pattern_text) - 1);
    
    // TODO: Apply filter to file list
    // This will be implemented when pattern filtering is added
}

// Set window title
void reqasl_set_title(ReqASL *req, const char *title) {
    if (!req || !title) return;
    
    strncpy(req->window_title, title, sizeof(req->window_title) - 1);
    req->window_title[sizeof(req->window_title) - 1] = '\0';
    
    // Update the window title using AmiWB's property system
    if (req->window && req->display) {
        // Use the _AMIWB_TITLE_CHANGE property that AmiWB monitors
        Atom amiwb_title_change = XInternAtom(req->display, "_AMIWB_TITLE_CHANGE", False);
        
        // Set the property on our window
        XChangeProperty(req->display, req->window,
                       amiwb_title_change,
                       XA_STRING, 8,
                       PropModeReplace,
                       (unsigned char *)req->window_title,
                       strlen(req->window_title));
        
        XFlush(req->display);
    }
}

// Set dialog mode (open or save)
void reqasl_set_mode(ReqASL *req, bool is_save_mode) {
    if (!req) return;
    
    req->is_save_mode = is_save_mode;
    
    // In save mode, disable multi-selection; in open mode, enable it
    if (is_save_mode) {
        req->multi_select_enabled = false;
        if (req->listview) {
            listview_set_multi_select(req->listview, false);
        }
    } else {
        // Open mode - enable multi-selection
        req->multi_select_enabled = true;
        if (req->listview) {
            listview_set_multi_select(req->listview, true);
        }
    }
}

