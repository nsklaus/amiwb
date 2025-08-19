#ifndef LISTVIEW_H
#define LISTVIEW_H

#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>
#include <X11/Xft/Xft.h>
#include <stdbool.h>

#define LISTVIEW_MAX_ITEMS 1000
#define LISTVIEW_ITEM_HEIGHT 20
#define LISTVIEW_SCROLLBAR_WIDTH 20
#define LISTVIEW_ARROW_HEIGHT 17

typedef struct ListViewItem {
    char text[256];
    bool is_directory;  // For file browsers
    void *user_data;
} ListViewItem;

typedef struct ListView {
    // Position and dimensions
    int x, y;
    int width, height;
    
    // Items
    ListViewItem items[LISTVIEW_MAX_ITEMS];
    int item_count;
    
    // Selection and scrolling
    int selected_index;
    int scroll_offset;  // First visible item index
    int visible_items;  // Number of items that fit in view
    
    // Scrollbar state
    int scrollbar_knob_y;
    int scrollbar_knob_height;
    bool scrollbar_dragging;
    int scrollbar_drag_offset;
    
    // Callbacks
    void (*on_select)(int index, const char *text, void *user_data);
    void (*on_double_click)(int index, const char *text, void *user_data);
    void *callback_data;
    
    // Internal state
    bool needs_redraw;
} ListView;

// Creation and destruction
ListView* listview_create(int x, int y, int width, int height);
void listview_destroy(ListView *lv);

// Item management
void listview_clear(ListView *lv);
void listview_add_item(ListView *lv, const char *text, bool is_directory, void *user_data);
void listview_set_items(ListView *lv, ListViewItem *items, int count);

// Selection and scrolling
void listview_set_selected(ListView *lv, int index);
void listview_scroll_to(ListView *lv, int index);
void listview_ensure_visible(ListView *lv, int index);

// Event handling
bool listview_handle_click(ListView *lv, int x, int y);
bool listview_handle_motion(ListView *lv, int x, int y);
bool listview_handle_release(ListView *lv);
bool listview_handle_scroll(ListView *lv, int direction); // +1 down, -1 up

// Drawing
void listview_draw(ListView *lv, Display *dpy, Picture dest, XftDraw *xft_draw, XftFont *font);
void listview_update_scrollbar(ListView *lv);

// Callbacks
void listview_set_callbacks(ListView *lv, 
                           void (*on_select)(int, const char*, void*),
                           void (*on_double_click)(int, const char*, void*),
                           void *user_data);

#endif // LISTVIEW_H