#include "reqasl.h"
#include "../toolkit/button.h"
#include "../toolkit/inputfield.h"
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

// Window dimensions
#define REQASL_WIDTH 381
#define REQASL_HEIGHT 405
#define REQASL_MIN_WIDTH 381
#define REQASL_MIN_HEIGHT 405
#define BUTTON_WIDTH 80
#define BUTTON_HEIGHT 25
#define INPUT_HEIGHT 20
#define LIST_ITEM_HEIGHT 15
#define MARGIN 10
#define SPACING 5
#define LABEL_WIDTH 60

// Colors
static XRenderColor GRAY  = {0x9999, 0x9999, 0x9999, 0xffff};
static XRenderColor WHITE = {0xffff, 0xffff, 0xffff, 0xffff};
static XRenderColor BLACK = {0x0000, 0x0000, 0x0000, 0xffff};
static XRenderColor BLUE  = {0x5555, 0x8888, 0xffff, 0xffff};
static XRenderColor DARK  = {0x5555, 0x5555, 0x5555, 0xffff};

// Forward declarations
static void free_entries(ReqASL *req);
static void scan_directory(ReqASL *req, const char *path);
static void draw_window(ReqASL *req);
static void draw_list_view(ReqASL *req, Picture dest);
static void handle_list_click(ReqASL *req, int y);
static void handle_list_double_click(ReqASL *req, int y);
static int compare_entries(const void *a, const void *b);

ReqASL* reqasl_create(Display *display) {
    if (!display) return NULL;
    
    ReqASL *req = calloc(1, sizeof(ReqASL));
    if (!req) return NULL;
    
    req->display = display;
    req->width = REQASL_WIDTH;
    req->height = REQASL_HEIGHT;
    
    // Initialize font - use SourceCodePro-Bold.otf with FontConfig pattern
    FcPattern *pattern = FcPatternCreate();
    if (pattern) {
        FcPatternAddString(pattern, FC_FILE, (const FcChar8 *)"fonts/SourceCodePro-Bold.otf");
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
    
    // Calculate list area (at top, takes most space)
    req->list_y = MARGIN;
    req->list_height = req->height - MARGIN - (3 * INPUT_HEIGHT) - (4 * SPACING) - BUTTON_HEIGHT - MARGIN;
    req->visible_items = req->list_height / LIST_ITEM_HEIGHT;
    
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
    
    // Create window
    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);
    
    XSetWindowAttributes attrs;
    // Use gray background (approximate Amiga gray)
    attrs.background_pixel = 0x999999;
    attrs.border_pixel = BlackPixel(display, screen);
    attrs.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | 
                       ButtonReleaseMask | StructureNotifyMask;
    
    req->window = XCreateWindow(display, root,
                               100, 100, req->width, req->height,
                               1, CopyFromParent, InputOutput, CopyFromParent,
                               CWBackPixel | CWBorderPixel | CWEventMask,
                               &attrs);
    
    // Set window title
    XStoreName(display, req->window, "Select File");
    
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
    
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        strncpy(req->current_path, path, sizeof(req->current_path) - 1);
        req->current_path[sizeof(req->current_path) - 1] = '\0';
        strncpy(req->drawer_text, req->current_path, sizeof(req->drawer_text) - 1);
        req->drawer_text[sizeof(req->drawer_text) - 1] = '\0';
        req->selected_index = -1;
        req->scroll_offset = 0;
        scan_directory(req, path);
        draw_window(req);
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
    
    // Free existing entries
    free_entries(req);
    
    DIR *dir = opendir(path);
    if (!dir) return;
    
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
    
    // Sort entries
    if (req->entry_count > 0) {
        qsort(req->entries, req->entry_count, sizeof(FileEntry*), compare_entries);
    }
}

static void draw_window(ReqASL *req) {
    if (!req || !req->window) return;
    
    // Debug output for window dimensions to file
    FILE *debug_file = fopen("reqasl_dimensions.log", "a");
    if (debug_file) {
        fprintf(debug_file, "width=%d, height=%d\n", req->width, req->height);
        fflush(debug_file);
        fclose(debug_file);
    }
    
    // Get window drawable
    Pixmap pixmap = XCreatePixmap(req->display, req->window, 
                                 req->width, req->height,
                                 DefaultDepth(req->display, DefaultScreen(req->display)));
    
    // Create render picture
    XRenderPictFormat *fmt = XRenderFindStandardFormat(req->display, PictStandardRGB24);
    Picture dest = XRenderCreatePicture(req->display, pixmap, fmt, 0, NULL);
    
    // Create or update XftDraw for text rendering
    if (req->xft_draw) {
        XftDrawDestroy(req->xft_draw);
    }
    req->xft_draw = XftDrawCreate(req->display, pixmap,
                                  DefaultVisual(req->display, DefaultScreen(req->display)),
                                  DefaultColormap(req->display, DefaultScreen(req->display)));
    
    // Clear background
    XRenderFillRectangle(req->display, PictOpSrc, dest, &GRAY, 
                        0, 0, req->width, req->height);
    
    // Draw list view area with inset border
    int list_x = MARGIN;
    int list_w = req->width - MARGIN * 2;
    
    // Inset border for list
    XRenderFillRectangle(req->display, PictOpSrc, dest, &DARK, 
                        list_x, req->list_y, list_w, req->list_height);
    XRenderFillRectangle(req->display, PictOpSrc, dest, &BLACK,
                        list_x+1, req->list_y+1, 1, req->list_height-2);
    XRenderFillRectangle(req->display, PictOpSrc, dest, &BLACK,
                        list_x+1, req->list_y+1, list_w-2, 1);
    XRenderFillRectangle(req->display, PictOpSrc, dest, &WHITE,
                        list_x+list_w-2, req->list_y+1, 1, req->list_height-2);
    XRenderFillRectangle(req->display, PictOpSrc, dest, &WHITE,
                        list_x+1, req->list_y+req->list_height-2, list_w-2, 1);
    
    // Gray background for list content (Amiga style)
    XRenderFillRectangle(req->display, PictOpSrc, dest, &GRAY,
                        list_x+2, req->list_y+2, list_w-4, req->list_height-4);
    
    // Draw list items
    draw_list_view(req, dest);
    
    // Draw input field labels if we have font
    if (req->font && req->xft_draw) {
        XftColor label_color;
        XRenderColor black = {0x0000, 0x0000, 0x0000, 0xffff};
        XftColorAllocValue(req->display, DefaultVisual(req->display, DefaultScreen(req->display)),
                          DefaultColormap(req->display, DefaultScreen(req->display)),
                          &black, &label_color);
        
        // Calculate label positions (right-justified)
        int label_x_right = MARGIN + LABEL_WIDTH - 5;
        
        // Pattern label
        int pattern_y = req->height - MARGIN - BUTTON_HEIGHT - SPACING - (3 * INPUT_HEIGHT) - (2 * SPACING);
        XGlyphInfo extents;
        XftTextExtentsUtf8(req->display, req->font, (FcChar8*)"Pattern:", 8, &extents);
        XftDrawStringUtf8(req->xft_draw, &label_color, req->font,
                         label_x_right - extents.width,
                         pattern_y + (INPUT_HEIGHT + req->font->ascent - req->font->descent) / 2,
                         (FcChar8*)"Pattern:", 8);
        
        // Drawer label
        int drawer_y = req->height - MARGIN - BUTTON_HEIGHT - SPACING - (2 * INPUT_HEIGHT) - SPACING;
        XftTextExtentsUtf8(req->display, req->font, (FcChar8*)"Drawer:", 7, &extents);
        XftDrawStringUtf8(req->xft_draw, &label_color, req->font,
                         label_x_right - extents.width,
                         drawer_y + (INPUT_HEIGHT + req->font->ascent - req->font->descent) / 2,
                         (FcChar8*)"Drawer:", 7);
        
        // File label
        int file_y = req->height - MARGIN - BUTTON_HEIGHT - SPACING - INPUT_HEIGHT;
        XftTextExtentsUtf8(req->display, req->font, (FcChar8*)"File:", 5, &extents);
        XftDrawStringUtf8(req->xft_draw, &label_color, req->font,
                         label_x_right - extents.width,
                         file_y + (INPUT_HEIGHT + req->font->ascent - req->font->descent) / 2,
                         (FcChar8*)"File:", 5);
        
        XftColorFree(req->display, DefaultVisual(req->display, DefaultScreen(req->display)),
                    DefaultColormap(req->display, DefaultScreen(req->display)), &label_color);
    }
    
    // Draw input fields anchored to bottom
    int input_y = req->height - MARGIN - BUTTON_HEIGHT - SPACING;
    
    // File field (bottom-most input)
    input_y -= INPUT_HEIGHT;
    InputField file_field = {
        .x = MARGIN + LABEL_WIDTH, .y = input_y,
        .width = req->width - MARGIN * 2 - LABEL_WIDTH, .height = INPUT_HEIGHT
    };
    strncpy(file_field.text, req->file_text, INPUTFIELD_MAX_LENGTH);
    file_field.text[INPUTFIELD_MAX_LENGTH] = '\0';
    inputfield_draw(&file_field, dest, req->display, req->xft_draw, req->font);
    
    // Drawer field (middle input)
    input_y -= (INPUT_HEIGHT + SPACING);
    InputField drawer_field = {
        .x = MARGIN + LABEL_WIDTH, .y = input_y,
        .width = req->width - MARGIN * 2 - LABEL_WIDTH, .height = INPUT_HEIGHT
    };
    strncpy(drawer_field.text, req->drawer_text, INPUTFIELD_MAX_LENGTH);
    drawer_field.text[INPUTFIELD_MAX_LENGTH] = '\0';
    inputfield_draw(&drawer_field, dest, req->display, req->xft_draw, req->font);
    
    // Pattern field (top-most input)
    input_y -= (INPUT_HEIGHT + SPACING);
    InputField pattern_field = {
        .x = MARGIN + LABEL_WIDTH, .y = input_y,
        .width = req->width - MARGIN * 2 - LABEL_WIDTH, .height = INPUT_HEIGHT
    };
    strncpy(pattern_field.text, req->pattern_text, INPUTFIELD_MAX_LENGTH);
    pattern_field.text[INPUTFIELD_MAX_LENGTH] = '\0';
    inputfield_draw(&pattern_field, dest, req->display, req->xft_draw, req->font);
    
    // Draw buttons with dynamic spacing
    int button_y = req->height - MARGIN - BUTTON_HEIGHT;
    int total_button_width = 4 * BUTTON_WIDTH;
    int available_width = req->width - MARGIN * 2;
    int spacing = (available_width - total_button_width) / 3; // Space between buttons
    
    // Ensure minimum spacing
    if (spacing < SPACING) spacing = SPACING;
    
    Button open_btn = {
        .x = MARGIN, .y = button_y,
        .width = BUTTON_WIDTH, .height = BUTTON_HEIGHT,
        .label = "Open"
    };
    button_draw(&open_btn, dest, req->display, req->xft_draw, req->font);
    
    Button volumes_btn = {
        .x = MARGIN + BUTTON_WIDTH + spacing, .y = button_y,
        .width = BUTTON_WIDTH, .height = BUTTON_HEIGHT,
        .label = "Volumes"
    };
    button_draw(&volumes_btn, dest, req->display, req->xft_draw, req->font);
    
    Button parent_btn = {
        .x = MARGIN + 2 * (BUTTON_WIDTH + spacing), .y = button_y,
        .width = BUTTON_WIDTH, .height = BUTTON_HEIGHT,
        .label = "Parent"
    };
    button_draw(&parent_btn, dest, req->display, req->xft_draw, req->font);
    
    Button cancel_btn = {
        .x = req->width - MARGIN - BUTTON_WIDTH, .y = button_y,
        .width = BUTTON_WIDTH, .height = BUTTON_HEIGHT,
        .label = "Cancel"
    };
    button_draw(&cancel_btn, dest, req->display, req->xft_draw, req->font);
    
    // Copy to window
    GC gc = XCreateGC(req->display, req->window, 0, NULL);
    XCopyArea(req->display, pixmap, req->window, gc, 
             0, 0, req->width, req->height, 0, 0);
    
    // Cleanup
    XFreeGC(req->display, gc);
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
        XRenderColor black = {0x0000, 0x0000, 0x0000, 0xffff};
        XRenderColor white = {0xffff, 0xffff, 0xffff, 0xffff};
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
            XRenderFillRectangle(req->display, PictOpSrc, dest, &BLUE,
                               list_x, item_y, list_w, LIST_ITEM_HEIGHT);
        }
        
        // Draw folder/file icon placeholder
        if (entry->type == TYPE_DRAWER) {
            // Draw folder indicator (small rectangle)
            XRenderColor folder_color = {0x8888, 0x6666, 0x3333, 0xffff};
            XRenderFillRectangle(req->display, PictOpSrc, dest, &folder_color,
                               list_x + 2, item_y + 2, 10, 10);
        } else {
            // Draw file indicator (smaller rectangle)
            XRenderFillRectangle(req->display, PictOpSrc, dest, &DARK,
                               list_x + 3, item_y + 3, 8, 8);
        }
        
        // Draw item name if we have font
        if (req->font && req->xft_draw && entry->name) {
            int text_x = list_x + 16;  // After icon
            int text_y = item_y + (LIST_ITEM_HEIGHT + req->font->ascent - req->font->descent) / 2;
            
            // Use white text on blue background for selected items
            XftColor *color = (idx == req->selected_index) ? &white_color : &text_color;
            
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
            if (event->xbutton.button == Button1) {
                int x = event->xbutton.x;
                int y = event->xbutton.y;
                
                // Check if click is in list area
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
                
                // Check buttons with dynamic spacing
                int button_y = req->height - MARGIN - BUTTON_HEIGHT;
                int total_button_width = 4 * BUTTON_WIDTH;
                int available_width = req->width - MARGIN * 2;
                int spacing = (available_width - total_button_width) / 3;
                if (spacing < SPACING) spacing = SPACING;
                
                // Open button
                if (x >= MARGIN && x < MARGIN + BUTTON_WIDTH &&
                    y >= button_y && y < button_y + BUTTON_HEIGHT) {
                    if (req->on_open) {
                        char full_path[2048];
                        if (req->file_text[0]) {
                            snprintf(full_path, sizeof(full_path), "%s/%s",
                                   req->current_path, req->file_text);
                        } else {
                            strcpy(full_path, req->current_path);
                        }
                        req->on_open(full_path);
                    }
                    reqasl_hide(req);
                    return true;
                }
                
                // Volumes button
                int volumes_x = MARGIN + BUTTON_WIDTH + spacing;
                if (x >= volumes_x && x < volumes_x + BUTTON_WIDTH &&
                    y >= button_y && y < button_y + BUTTON_HEIGHT) {
                    // TODO: Implement volumes functionality
                    return true;
                }
                
                // Parent button
                int parent_x = MARGIN + 2 * (BUTTON_WIDTH + spacing);
                if (x >= parent_x && x < parent_x + BUTTON_WIDTH &&
                    y >= button_y && y < button_y + BUTTON_HEIGHT) {
                    reqasl_navigate_parent(req);
                    return true;
                }
                
                // Cancel button
                if (x >= req->width - MARGIN - BUTTON_WIDTH &&
                    x < req->width - MARGIN &&
                    y >= button_y && y < button_y + BUTTON_HEIGHT) {
                    if (req->on_cancel) {
                        req->on_cancel();
                    }
                    reqasl_hide(req);
                    return true;
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
                FILE *debug_file = fopen("reqasl_dimensions.log", "a");
                if (debug_file) {
                    fprintf(debug_file, "ConfigureRequest: width=%d, height=%d\n", 
                            event->xconfigurerequest.width, event->xconfigurerequest.height);
                    fflush(debug_file);
                    fclose(debug_file);
                }
                
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
            
        case ConfigureNotify:
            // Handle window configuration change (position, size)
            // Since we've set min and max to same value, size shouldn't change
            {
                int event_width = event->xconfigure.width;
                int event_height = event->xconfigure.height;
                
                // If somehow the size changed (non-compliant WM), force it back
                if (event_width != REQASL_MIN_WIDTH || event_height != REQASL_MIN_HEIGHT) {
                    // Log unexpected resize
                    FILE *debug_file = fopen("reqasl_dimensions.log", "a");
                    if (debug_file) {
                        fprintf(debug_file, "ConfigureNotify: Unexpected size change to width=%d, height=%d. Forcing back to %dx%d\n", 
                                event_width, event_height, REQASL_MIN_WIDTH, REQASL_MIN_HEIGHT);
                        fclose(debug_file);
                    }
                    
                    // Force the window to fixed size
                    XResizeWindow(req->display, req->window, REQASL_MIN_WIDTH, REQASL_MIN_HEIGHT);
                    XSync(req->display, False);
                    
                    // Update internal state to fixed dimensions
                    req->width = REQASL_MIN_WIDTH;
                    req->height = REQASL_MIN_HEIGHT;
                    
                    // Recalculate list area (though it shouldn't change)
                    req->list_height = req->height - MARGIN - (3 * INPUT_HEIGHT) - 
                                      (4 * SPACING) - BUTTON_HEIGHT - MARGIN;
                    req->visible_items = req->list_height / LIST_ITEM_HEIGHT;
                    
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