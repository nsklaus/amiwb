#ifndef REQASL_H
#define REQASL_H

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>
#include <stdbool.h>

// Forward declaration
typedef struct ListView ListView;

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
    char current_path[1024];
    FileEntry **entries;
    int entry_count;
    int entry_capacity;
    
    // Selection state
    int selected_index;
    int scroll_offset;
    int visible_items;
    
    // Input fields
    char pattern_text[256];
    char drawer_text[1024];
    char file_text[256];
    
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

#endif