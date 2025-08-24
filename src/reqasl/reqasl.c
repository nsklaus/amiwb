#include "reqasl.h"
#include "../amiwb/config.h"
#include "../toolkit/button.h"
#include "../toolkit/inputfield.h"
#include "../toolkit/listview.h"
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
#include <unistd.h>
#include <time.h>
#include <pwd.h>
#include <stdarg.h>

// Initialize debug logging (create fresh log with timestamp)
static void reqasl_log_init(void) {
    const char *home = getenv("HOME");
    if (!home) return;
    
    char log_path[512];
    snprintf(log_path, sizeof(log_path), "%s/Sources/amiwb/reqasl.log", home);
    
    FILE *debug_file = fopen(log_path, "w");  // "w" to create fresh file
    if (debug_file) {
        time_t now = time(NULL);
        char *timestamp = ctime(&now);
        fprintf(debug_file, "=== ReqASL Debug Log ===\n");
        fprintf(debug_file, "Started: %s", timestamp);  // ctime includes newline
        fprintf(debug_file, "========================\n\n");
        fclose(debug_file);
    }
}

// Debug logging function
static void reqasl_log(const char *format, ...) {
    const char *home = getenv("HOME");
    if (!home) return;
    
    char log_path[512];
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

// Forward declarations
static void free_entries(ReqASL *req);
static void scan_directory(ReqASL *req, const char *path);
static void draw_window(ReqASL *req);
static void draw_list_view(ReqASL *req, Picture dest);
static void handle_list_click(ReqASL *req, int y);
static void handle_list_double_click(ReqASL *req, int y);
static int compare_entries(const void *a, const void *b);
static void listview_select_callback(int index, const char *text, void *user_data);
static void listview_double_click_callback(int index, const char *text, void *user_data);

ReqASL* reqasl_create(Display *display) {
    if (!display) return NULL;
    
    // Initialize debug logging with fresh file
    reqasl_log_init();
    reqasl_log("ReqASL starting...");
    
    ReqASL *req = calloc(1, sizeof(ReqASL));
    if (!req) return NULL;
    
    req->display = display;
    req->width = REQASL_WIDTH;
    req->height = REQASL_HEIGHT;
    
    // Initialize font - use SourceCodePro-Bold.otf with FontConfig pattern
    FcPattern *pattern = FcPatternCreate();
    if (pattern) {
        FcPatternAddString(pattern, FC_FILE, (const FcChar8 *)"/usr/local/share/amiwb/fonts/SourceCodePro-Bold.otf");
        FcPatternAddDouble(pattern, FC_SIZE, 12.0);
        FcPatternAddInteger(pattern, FC_WEIGHT, FC_WEIGHT_BOLD);
        FcPatternAddDouble(pattern, FC_DPI, 75);
        FcConfigSubstitute(NULL, pattern, FcMatchPattern);
        XftDefaultSubstitute(display, DefaultScreen(display), pattern);
        req->font = XftFontOpenPattern(display, pattern);
        // Note: pattern is now owned by the font, don't destroy it
    }
    
    if (!req->font) {
        fprintf(stderr, "[WARNING] Failed to load SourceCodePro-Bold.otf, falling back to monospace\n");
        req->font = XftFontOpen(display, DefaultScreen(display),
                               XFT_FAMILY, XftTypeString, "monospace",
                               XFT_SIZE, XftTypeDouble, 12.0,
                               NULL);
        if (!req->font) {
            fprintf(stderr, "[WARNING] Failed to load monospace, falling back to fixed\n");
            req->font = XftFontOpenName(display, DefaultScreen(display), "fixed");
        }
    }
    req->is_open = false;
    req->show_hidden = false;
    req->selected_index = -1;
    req->scroll_offset = 0;
    
    // Initialize button press states
    req->open_button_pressed = false;
    req->volumes_button_pressed = false;
    req->parent_button_pressed = false;
    req->cancel_button_pressed = false;
    
    // Calculate list area (at top, takes most space)
    req->list_y = MARGIN;
    req->list_height = req->height - MARGIN - (3 * INPUT_HEIGHT) - (4 * SPACING) - BUTTON_HEIGHT - MARGIN;
    req->visible_items = req->list_height / LISTVIEW_ITEM_HEIGHT;
    
    // Create ListView widget
    req->listview = listview_create(MARGIN, req->list_y, 
                                   req->width - MARGIN * 2, 
                                   req->list_height);
    if (!req->listview) {
        reqasl_log("ERROR: Failed to create listview");
        // Continue anyway - old rendering will be fallback
    } else {
        // Set callbacks for ListView
        listview_set_callbacks(req->listview, 
                             listview_select_callback,
                             listview_double_click_callback,
                             req);
    }
    
    // Create InputField widgets for text editing
    // Calculate positions (will be updated in draw_window for dynamic resizing)
    int input_y = req->list_y + req->list_height + SPACING;
    
    req->pattern_field = inputfield_create(MARGIN + LABEL_WIDTH, input_y, 
                                          req->width - MARGIN * 2 - LABEL_WIDTH, INPUT_HEIGHT);
    if (req->pattern_field) {
        inputfield_set_text(req->pattern_field, "");
    }
    
    req->drawer_field = inputfield_create(MARGIN + LABEL_WIDTH, input_y + INPUT_HEIGHT + SPACING,
                                         req->width - MARGIN * 2 - LABEL_WIDTH, INPUT_HEIGHT);
    if (req->drawer_field) {
        inputfield_set_text(req->drawer_field, "");
    }
    
    req->file_field = inputfield_create(MARGIN + LABEL_WIDTH, input_y + 2 * (INPUT_HEIGHT + SPACING),
                                       req->width - MARGIN * 2 - LABEL_WIDTH, INPUT_HEIGHT);
    if (req->file_field) {
        inputfield_set_text(req->file_field, "");
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
    
    // Sync InputFields with text buffers
    if (req->drawer_field) {
        inputfield_set_text(req->drawer_field, req->drawer_text);
        // Ensure the end of the path is visible
        inputfield_scroll_to_end(req->drawer_field);
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
    
    // Set window title (multiple methods for compatibility with different WMs)
    XStoreName(display, req->window, "reqasl");
    
    // Set WM_NAME property
    XTextProperty window_name;
    char *title = "reqasl";
    XStringListToTextProperty(&title, 1, &window_name);
    XSetWMName(display, req->window, &window_name);
    XFree(window_name.value);
    
    // Also set _NET_WM_NAME for modern window managers
    Atom net_wm_name = XInternAtom(display, "_NET_WM_NAME", False);
    Atom utf8_string = XInternAtom(display, "UTF8_STRING", False);
    XChangeProperty(display, req->window, net_wm_name, utf8_string, 8,
                    PropModeReplace, (unsigned char *)"reqasl", 6);
    
    // Set WM_CLASS for window identification
    XClassHint *class_hint = XAllocClassHint();
    if (class_hint) {
        class_hint->res_name = "reqasl";
        class_hint->res_class = "ReqASL";
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
    
    if (req->font) {
        XftFontClose(req->display, req->font);
    }
    
    if (req->window) {
        XDestroyWindow(req->display, req->window);
    }
    
    free_entries(req);
    free(req);
}

void reqasl_show(ReqASL *req, const char *initial_path) {
    if (!req) return;
    
    req->is_open = true;
    
    // Navigate to initial path if provided
    if (initial_path && initial_path[0]) {
        reqasl_navigate_to(req, initial_path);
    } else {
        // Use current path
        scan_directory(req, req->current_path);
    }
    
    // Show window
    XMapRaised(req->display, req->window);
    XFlush(req->display);
    
    // Initial draw
    draw_window(req);
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

void reqasl_navigate_to(ReqASL *req, const char *path) {
    if (!req || !path) return;
    
    reqasl_log("reqasl_navigate_to: Navigating to path: %s", path);
    
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        reqasl_log("reqasl_navigate_to: Path is valid directory");
        strncpy(req->current_path, path, sizeof(req->current_path) - 1);
        req->current_path[sizeof(req->current_path) - 1] = '\0';
        strncpy(req->drawer_text, req->current_path, sizeof(req->drawer_text) - 1);
        req->drawer_text[sizeof(req->drawer_text) - 1] = '\0';
        
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
        
        req->selected_index = -1;
        req->scroll_offset = 0;
        scan_directory(req, req->current_path);  // Use copied path, not original which may be freed
        reqasl_log("reqasl_navigate_to: After scan_directory, entry_count=%d", req->entry_count);
        if (req->listview) {
            reqasl_log("reqasl_navigate_to: ListView item_count=%d", req->listview->item_count);
        }
        draw_window(req);
    } else {
        reqasl_log("reqasl_navigate_to: Path is not valid or not a directory");
    }
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
    
    reqasl_log("scan_directory: Scanning path: %s", path);
    
    // Free existing entries
    free_entries(req);
    
    // Clear ListView
    if (req->listview) {
        reqasl_log("scan_directory: Clearing ListView");
        listview_clear(req->listview);
        reqasl_log("scan_directory: After clear, ListView item_count=%d", req->listview->item_count);
    }
    
    DIR *dir = opendir(path);
    if (!dir) {
        reqasl_log("scan_directory: Failed to open directory: %s", path);
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
        
        // Build full path
        char full_path[2048];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
        fe->path = strdup(full_path);
        
        // Get file info
        struct stat st;
        if (stat(full_path, &st) == 0) {
            fe->type = S_ISDIR(st.st_mode) ? TYPE_DRAWER : TYPE_FILE;
            fe->size = st.st_size;
            fe->modified = st.st_mtime;
        } else {
            fe->type = TYPE_FILE;
            fe->size = 0;
            fe->modified = 0;
        }
        
        req->entries[req->entry_count++] = fe;
    }
    
    closedir(dir);
    
    reqasl_log("scan_directory: Found %d entries total", req->entry_count);
    
    // Sort entries
    if (req->entry_count > 0) {
        qsort(req->entries, req->entry_count, sizeof(FileEntry*), compare_entries);
        
        // Add entries to ListView
        if (req->listview) {
            reqasl_log("scan_directory: Adding %d entries to ListView", req->entry_count);
            for (int i = 0; i < req->entry_count; i++) {
                FileEntry *fe = req->entries[i];
                reqasl_log("scan_directory: Adding item %d: %s (dir=%d)", i, fe->name, fe->type == TYPE_DRAWER);
                listview_add_item(req->listview, fe->name, 
                                fe->type == TYPE_DRAWER, fe);
            }
            reqasl_log("scan_directory: ListView now has %d items", req->listview->item_count);
            reqasl_log("scan_directory: ListView needs_redraw=%d", req->listview->needs_redraw);
        } else {
            reqasl_log("scan_directory: ERROR - ListView is NULL!");
        }
    } else {
        reqasl_log("scan_directory: No entries found in directory");
    }
}

static void draw_window(ReqASL *req) {
    if (!req || !req->window) return;
    
    // Debug output for window dimensions
    reqasl_log("Window dimensions: width=%d, height=%d", req->width, req->height);
    
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
    
    // Draw ListView widget if available, otherwise fallback to old rendering
    if (req->listview) {
        listview_draw(req->listview, req->display, dest, temp_xft_draw, req->font);
    } else {
        // Fallback to old manual rendering
        int list_x = MARGIN;
        int list_w = req->width - MARGIN * 2;
        
        // Inset border for list
        XRenderColor black = BLACK;
        XRenderColor white = WHITE;
        XRenderColor dark = {0x5555, 0x5555, 0x5555, 0xffff};  // Dark gray for border
        XRenderFillRectangle(req->display, PictOpSrc, dest, &dark, 
                            list_x, req->list_y, list_w, req->list_height);
        XRenderFillRectangle(req->display, PictOpSrc, dest, &black,
                            list_x+1, req->list_y+1, 1, req->list_height-2);
        XRenderFillRectangle(req->display, PictOpSrc, dest, &black,
                            list_x+1, req->list_y+1, list_w-2, 1);
        XRenderFillRectangle(req->display, PictOpSrc, dest, &white,
                            list_x+list_w-2, req->list_y+1, 1, req->list_height-2);
        XRenderFillRectangle(req->display, PictOpSrc, dest, &white,
                            list_x+1, req->list_y+req->list_height-2, list_w-2, 1);
        
        // Gray background for list content (Amiga style)
        XRenderFillRectangle(req->display, PictOpSrc, dest, &gray,
                            list_x+2, req->list_y+2, list_w-4, req->list_height-4);
        
        // Draw list items
        draw_list_view(req, dest);
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
        inputfield_draw(req->file_field, dest, req->display, temp_xft_draw, req->font);
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
        inputfield_draw(req->drawer_field, dest, req->display, temp_xft_draw, req->font);
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
        inputfield_draw(req->pattern_field, dest, req->display, temp_xft_draw, req->font);
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
        .label = "Open",
        .pressed = req->open_button_pressed
    };
    button_draw(&open_btn, dest, req->display, temp_xft_draw, req->font);
    
    Button volumes_btn = {
        .x = volumes_x, .y = button_y,
        .width = BUTTON_WIDTH, .height = BUTTON_HEIGHT,
        .label = "Volumes",
        .pressed = req->volumes_button_pressed
    };
    button_draw(&volumes_btn, dest, req->display, temp_xft_draw, req->font);
    
    Button parent_btn = {
        .x = parent_x, .y = button_y,
        .width = BUTTON_WIDTH, .height = BUTTON_HEIGHT,
        .label = "Parent",
        .pressed = req->parent_button_pressed
    };
    button_draw(&parent_btn, dest, req->display, temp_xft_draw, req->font);
    
    Button cancel_btn = {
        .x = cancel_x, .y = button_y,
        .width = BUTTON_WIDTH, .height = BUTTON_HEIGHT,
        .label = "Cancel",
        .pressed = req->cancel_button_pressed
    };
    button_draw(&cancel_btn, dest, req->display, temp_xft_draw, req->font);
    
    // Copy to window
    GC gc = XCreateGC(req->display, req->window, 0, NULL);
    XCopyArea(req->display, pixmap, req->window, gc, 
             0, 0, req->width, req->height, 0, 0);
    
    // Cleanup
    XFreeGC(req->display, gc);
    XftDrawDestroy(temp_xft_draw);
    XRenderFreePicture(req->display, dest);
    XFreePixmap(req->display, pixmap);
    XFlush(req->display);
}

static void draw_list_view(ReqASL *req, Picture dest) {
    if (!req || !req->entries) return;
    
    int list_x = MARGIN + 4;
    int list_y = req->list_y + 4;
    int list_w = req->width - MARGIN * 2 - 8;
    
    // Set up text color if we have font
    XftColor text_color, white_color;
    if (req->font && req->xft_draw) {
        XRenderColor black = BLACK;
        XRenderColor white = WHITE;
        XftColorAllocValue(req->display, DefaultVisual(req->display, DefaultScreen(req->display)),
                          DefaultColormap(req->display, DefaultScreen(req->display)),
                          &black, &text_color);
        XftColorAllocValue(req->display, DefaultVisual(req->display, DefaultScreen(req->display)),
                          DefaultColormap(req->display, DefaultScreen(req->display)),
                          &white, &white_color);
    }
    
    // Draw visible items
    for (int i = 0; i < req->visible_items && i + req->scroll_offset < req->entry_count; i++) {
        int idx = i + req->scroll_offset;
        FileEntry *entry = req->entries[idx];
        
        int item_y = list_y + i * LIST_ITEM_HEIGHT;
        
        // Highlight selected item
        if (idx == req->selected_index) {
            XRenderColor blue = BLUE;
            XRenderFillRectangle(req->display, PictOpSrc, dest, &blue,
                               list_x, item_y, list_w, LIST_ITEM_HEIGHT);
        }
        
        // Draw item name if we have font (no icon squares)
        if (req->font && req->xft_draw && entry->name) {
            int text_x = list_x + 4;  // Small left padding
            int text_y = item_y + (LIST_ITEM_HEIGHT + req->font->ascent - req->font->descent) / 2;
            
            // Choose text color based on type and selection:
            // - Selected items always use white
            // - Directories use white (unless selected)
            // - Files use black (unless selected)
            XftColor *color;
            if (idx == req->selected_index) {
                color = &white_color;  // White text on blue background
            } else if (entry->type == TYPE_DRAWER) {
                color = &white_color;  // White text for directories
            } else {
                color = &text_color;   // Black text for files
            }
            
            XftDrawStringUtf8(req->xft_draw, color, req->font,
                             text_x, text_y,
                             (FcChar8*)entry->name, strlen(entry->name));
        }
    }
    
    // Free colors if allocated
    if (req->font && req->xft_draw) {
        XftColorFree(req->display, DefaultVisual(req->display, DefaultScreen(req->display)),
                    DefaultColormap(req->display, DefaultScreen(req->display)), &text_color);
        XftColorFree(req->display, DefaultVisual(req->display, DefaultScreen(req->display)),
                    DefaultColormap(req->display, DefaultScreen(req->display)), &white_color);
    }
}

static void handle_list_click(ReqASL *req, int y) {
    if (!req) return;
    
    int relative_y = y - req->list_y - 4;
    if (relative_y < 0) return;
    
    int item_index = relative_y / LIST_ITEM_HEIGHT;
    int absolute_index = item_index + req->scroll_offset;
    
    if (absolute_index >= 0 && absolute_index < req->entry_count) {
        req->selected_index = absolute_index;
        
        // Update file field with selection
        FileEntry *entry = req->entries[absolute_index];
        if (entry->type == TYPE_FILE) {
            strcpy(req->file_text, entry->name);
        }
        
        draw_window(req);
    }
}

static void handle_list_double_click(ReqASL *req, int y) {
    if (!req) return;
    
    handle_list_click(req, y);
    
    if (req->selected_index >= 0 && req->selected_index < req->entry_count) {
        FileEntry *entry = req->entries[req->selected_index];
        
        if (entry->type == TYPE_DRAWER) {
            // Navigate into directory
            reqasl_navigate_to(req, entry->path);
        } else {
            // Open file
            if (req->on_open) {
                req->on_open(entry->path);
            }
            reqasl_hide(req);
        }
    }
}

bool reqasl_handle_event(ReqASL *req, XEvent *event) {
    if (!req || !event) return false;
    
    switch (event->type) {
        case Expose:
            if (event->xexpose.count == 0) {
                draw_window(req);
            }
            return true;
            
        case ButtonPress:
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
                    if (req->pattern_field) inputfield_set_focus(req->pattern_field, false);
                    if (req->drawer_field) inputfield_set_focus(req->drawer_field, false);
                    if (req->file_field) inputfield_set_focus(req->file_field, false);
                }
                
                // Handle ListView click if available
                if (req->listview) {
                    if (listview_handle_click(req->listview, x, y)) {
                        draw_window(req);
                        return true;
                    }
                } else {
                    // Fallback to old list handling
                    if (x >= MARGIN && x < req->width - MARGIN &&
                        y >= req->list_y && y < req->list_y + req->list_height) {
                        
                        // Check for double-click (simplified - would need timing)
                        static Time last_click = 0;
                        if (event->xbutton.time - last_click < 500) {
                            handle_list_double_click(req, y);
                        } else {
                            handle_list_click(req, y);
                        }
                        last_click = event->xbutton.time;
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
            break;
            
        case ButtonRelease:
            // Handle scrollbar release in ListView
            if (req->listview) {
                if (listview_handle_release(req->listview)) {
                    draw_window(req);
                    return true;
                }
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
                        
                        // Check if something is selected in the listview
                        if (req->listview && req->listview->selected_index >= 0 && 
                            req->listview->selected_index < req->entry_count) {
                            
                            FileEntry *entry = req->entries[req->listview->selected_index];
                            
                            if (entry->type == TYPE_DRAWER) {
                                // Directory selected - open it in workbench window via IPC
                                Atom amiwb_open_dir = XInternAtom(req->display, 
                                                                 "AMIWB_OPEN_DIRECTORY", False);
                                Window root = DefaultRootWindow(req->display);
                                
                                XChangeProperty(req->display, root, amiwb_open_dir,
                                              XA_STRING, 8, PropModeReplace,
                                              (unsigned char *)entry->path, 
                                              strlen(entry->path));
                                XFlush(req->display);
                            } else {
                                // File selected - open with xdg-open
                                pid_t pid = fork();
                                if (pid == 0) {
                                    // Child process
                                    execlp("xdg-open", "xdg-open", entry->path, (char *)NULL);
                                    perror("execlp failed for xdg-open");
                                    _exit(EXIT_FAILURE);
                                }
                            }
                            reqasl_hide(req);
                        } else {
                            // Nothing selected - open current directory in workbench window
                            Atom amiwb_open_dir = XInternAtom(req->display, 
                                                             "AMIWB_OPEN_DIRECTORY", False);
                            Window root = DefaultRootWindow(req->display);
                            
                            XChangeProperty(req->display, root, amiwb_open_dir,
                                          XA_STRING, 8, PropModeReplace,
                                          (unsigned char *)req->current_path, 
                                          strlen(req->current_path));
                            XFlush(req->display);
                            reqasl_hide(req);
                        }
                        return true;
                    }
                }
                
                // Check Volumes button
                if (req->volumes_button_pressed) {
                    req->volumes_button_pressed = false;
                    need_redraw = true;
                    
                    if (x >= volumes_x && x < volumes_x + BUTTON_WIDTH &&
                        y >= button_y && y < button_y + BUTTON_HEIGHT) {
                        // TODO: Implement volumes functionality
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
                reqasl_log("ConfigureRequest: width=%d, height=%d", 
                          event->xconfigurerequest.width, event->xconfigurerequest.height);
                
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
                
                // Handle Escape key - clear selection first, then quit
                if (keysym == XK_Escape) {
                    // If something is selected, clear the selection
                    if (req->listview && req->listview->selected_index >= 0) {
                        req->listview->selected_index = -1;
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
                
                // Handle Return key for opening files/directories
                if (keysym == XK_Return) {
                    if (req->listview && req->listview->selected_index >= 0 && 
                        req->listview->selected_index < req->entry_count) {
                        
                        FileEntry *entry = req->entries[req->listview->selected_index];
                        
                        if (entry->type == TYPE_DRAWER) {
                            // Navigate into directory (same as double-click)
                            reqasl_navigate_to(req, entry->path);
                        } else {
                            // Open file with xdg-open (like workbench.c does)
                            pid_t pid = fork();
                            if (pid == 0) {
                                // Child process
                                execlp("xdg-open", "xdg-open", entry->path, (char *)NULL);
                                perror("execlp failed for xdg-open");
                                _exit(EXIT_FAILURE);
                            } else if (pid > 0) {
                                // Parent - file opened, hide reqasl
                                reqasl_hide(req);
                            }
                        }
                        return true;
                    } else {
                        // No selection or invalid selection - open current directory in workbench window
                        Atom amiwb_open_dir = XInternAtom(req->display, 
                                                         "AMIWB_OPEN_DIRECTORY", False);
                        Window root = DefaultRootWindow(req->display);
                        
                        XChangeProperty(req->display, root, amiwb_open_dir,
                                      XA_STRING, 8, PropModeReplace,
                                      (unsigned char *)req->current_path, 
                                      strlen(req->current_path));
                        XFlush(req->display);
                        reqasl_hide(req);
                        return true;
                    }
                }
                
                // Handle Backspace key - go to parent directory
                if (keysym == XK_BackSpace) {
                    reqasl_navigate_parent(req);
                    return true;
                }
                
                // Pass keyboard events to input fields
                // Try each input field in order
                if (req->pattern_field && inputfield_handle_key(req->pattern_field, &event->xkey)) {
                    // Sync field text back to buffer
                    strncpy(req->pattern_text, inputfield_get_text(req->pattern_field), 
                           sizeof(req->pattern_text) - 1);
                    req->pattern_text[sizeof(req->pattern_text) - 1] = '\0';
                    draw_window(req);
                    return true;
                }
                
                if (req->drawer_field && inputfield_handle_key(req->drawer_field, &event->xkey)) {
                    // Sync field text back to buffer
                    strncpy(req->drawer_text, inputfield_get_text(req->drawer_field),
                           sizeof(req->drawer_text) - 1);
                    req->drawer_text[sizeof(req->drawer_text) - 1] = '\0';
                    draw_window(req);
                    return true;
                }
                
                if (req->file_field && inputfield_handle_key(req->file_field, &event->xkey)) {
                    // Sync field text back to buffer
                    strncpy(req->file_text, inputfield_get_text(req->file_field),
                           sizeof(req->file_text) - 1);
                    req->file_text[sizeof(req->file_text) - 1] = '\0';
                    draw_window(req);
                    return true;
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
                    reqasl_log("ConfigureNotify: Window resized to %dx%d", new_width, new_height);
                    
                    // Update internal dimensions
                    req->width = new_width;
                    req->height = new_height;
                    
                    // Recalculate list area height (grows with window)
                    req->list_height = req->height - MARGIN - (3 * INPUT_HEIGHT) - 
                                      (4 * SPACING) - BUTTON_HEIGHT - MARGIN;
                    req->visible_items = req->list_height / LIST_ITEM_HEIGHT;
                    
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
            break;
    }
    
    return false;
}

static void listview_select_callback(int index, const char *text, void *user_data) {
    ReqASL *req = (ReqASL *)user_data;
    if (!req || index < 0 || index >= req->entry_count) return;
    
    req->selected_index = index;
    FileEntry *entry = req->entries[index];
    
    // Update file field with selection if it's a file
    if (entry->type == TYPE_FILE) {
        strcpy(req->file_text, entry->name);
        // Also update the InputField widget
        if (req->file_field) {
            inputfield_set_text(req->file_field, entry->name);
        }
    }
    
    // Redraw to update input fields
    draw_window(req);
}

static void listview_double_click_callback(int index, const char *text, void *user_data) {
    ReqASL *req = (ReqASL *)user_data;
    if (!req || index < 0 || index >= req->entry_count) return;
    
    FileEntry *entry = req->entries[index];
    
    if (entry->type == TYPE_DRAWER) {
        // Navigate into directory
        reqasl_navigate_to(req, entry->path);
    } else {
        // Open file
        if (req->on_open) {
            req->on_open(entry->path);
        }
        reqasl_hide(req);
    }
}