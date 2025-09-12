#ifndef REQASL_H
#define REQASL_H

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>
#include <stdbool.h>
#include "../amiwb/config.h"  // For PATH_SIZE and NAME_SIZE

// Forward declarations
typedef struct ListView ListView;
typedef struct InputField InputField;

// File types (matching workbench)
typedef enum {
    TYPE_FILE = 0,
    TYPE_DRAWER = 1
} FileType;

// File entry structure (reusing pattern from workbench)
typedef struct FileEntry {
    char *name;
    char *path;
    FileType type;
    long size;
    time_t modified;
} FileEntry;

// ReqASL dialog structure
typedef struct ReqASL {
    Window window;
    Display *display;
    XftFont *font;
    XftDraw *xft_draw;
    
    // Current directory and files
    char current_path[PATH_SIZE];
    FileEntry **entries;
    int entry_count;
    int entry_capacity;
    
    // Selection state
    int selected_index;
    int scroll_offset;
    int visible_items;
    
    // Input fields
    char pattern_text[NAME_SIZE];     // Pattern filter
    char drawer_text[PATH_SIZE];      // Current directory path
    char file_text[PATH_SIZE];        // Selected filename(s) - needs PATH_SIZE for multi-selection
    
    // InputField widgets for text editing
    InputField *pattern_field;
    InputField *drawer_field;
    InputField *file_field;
    
    // Dialog state
    bool is_open;
    bool show_hidden;
    
    // Callbacks
    void (*on_open)(const char *path);
    void (*on_cancel)(void);
    void *user_data;
    
    // Window dimensions
    int width, height;
    int list_y, list_height;
    
    // ListView widget
    ListView *listview;
    
    // Button press states
    bool open_button_pressed;
    bool volumes_button_pressed;
    bool parent_button_pressed;
    bool cancel_button_pressed;
    
    // Window title
    char window_title[NAME_SIZE];
    
    // Dialog mode
    bool is_save_mode;  // true for save, false for open
    
    // Multi-selection support
    bool multi_select_enabled;
    
} ReqASL;

// Public API
ReqASL* reqasl_create(Display *display);
void reqasl_destroy(ReqASL *req);
void reqasl_show(ReqASL *req, const char *initial_path);
void reqasl_hide(ReqASL *req);
bool reqasl_handle_event(ReqASL *req, XEvent *event);
void reqasl_set_callbacks(ReqASL *req, 
                         void (*on_open)(const char*),
                         void (*on_cancel)(void),
                         void *user_data);
void reqasl_refresh(ReqASL *req);
void reqasl_navigate_to(ReqASL *req, const char *path);
void reqasl_navigate_parent(ReqASL *req);
void reqasl_set_pattern(ReqASL *req, const char *extensions);
void reqasl_set_title(ReqASL *req, const char *title);
void reqasl_set_mode(ReqASL *req, bool is_save_mode);

// Error logging function - defined in reqasl.c
void log_error(const char *format, ...);

#endif