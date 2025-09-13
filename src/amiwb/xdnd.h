#ifndef AMIWB_XDND_H
#define AMIWB_XDND_H

#include <X11/Xlib.h>
#include <stdbool.h>

#define XDND_VERSION 5
#define XDND_THREE 3  // Older version for compatibility

// XDND context - holds all protocol atoms and state
typedef struct {
    // Protocol atoms
    Atom XdndAware;
    Atom XdndSelection;
    Atom XdndProxy;
    Atom XdndTypeList;

    // Message atoms
    Atom XdndEnter;
    Atom XdndPosition;
    Atom XdndStatus;
    Atom XdndLeave;
    Atom XdndDrop;
    Atom XdndFinished;

    // Action atoms
    Atom XdndActionCopy;
    Atom XdndActionMove;
    Atom XdndActionLink;
    Atom XdndActionAsk;
    Atom XdndActionPrivate;

    // Data type atoms
    Atom text_uri_list;
    Atom text_plain;
    Atom UTF8_STRING;

    // State tracking for source (when we're dragging)
    Window source_window;      // Our window that started the drag
    Window current_target;      // Current XDND-aware target under cursor
    Window last_target;         // Previous target (for leave messages)
    bool target_accepts;        // Whether current target accepts our data
    Time drag_timestamp;        // Timestamp of drag start
    Atom requested_action;      // Action requested by target

    // State tracking for target (when receiving drops)
    Window drop_source;         // Window dropping onto us
    Atom *offered_types;        // Types offered by source
    int offered_count;          // Number of offered types
    bool will_accept;           // Whether we'll accept the drop
    int pending_x, pending_y;   // Position of pending drop

    // Performance optimization - cache of XDND-aware windows
    Window *aware_cache;        // Array of known XDND windows
    int cache_size;            // Current cache size
    int cache_capacity;        // Allocated capacity
    Time cache_timestamp;      // Last cache clear time
} XdndContext;

// Global XDND context
extern XdndContext xdnd_ctx;

// Initialize XDND support
void xdnd_init(Display *dpy);
void xdnd_shutdown(Display *dpy);

// Mark a window as XDND-aware
void xdnd_make_aware(Display *dpy, Window win, int version);

// Source operations (when dragging from AmiWB)
Window xdnd_find_target(Display *dpy, int root_x, int root_y);
bool xdnd_is_aware(Display *dpy, Window win);
void xdnd_send_enter(Display *dpy, Window source, Window target);
void xdnd_send_position(Display *dpy, Window source, Window target,
                       int root_x, int root_y, Time timestamp, Atom action);
void xdnd_send_leave(Display *dpy, Window source, Window target);
void xdnd_send_drop(Display *dpy, Window source, Window target, Time timestamp);
void xdnd_send_finished(Display *dpy, Window source, Window target);

// Target operations (when receiving drops into AmiWB)
void xdnd_handle_enter(Display *dpy, XClientMessageEvent *event);
void xdnd_handle_position(Display *dpy, XClientMessageEvent *event);
void xdnd_handle_leave(Display *dpy, XClientMessageEvent *event);
void xdnd_handle_drop(Display *dpy, XClientMessageEvent *event);
void xdnd_send_status(Display *dpy, Window source, Window target,
                     bool will_accept, int x, int y, int w, int h, Atom action);

// Selection handling for data transfer
void xdnd_request_selection(Display *dpy, Window requestor, Atom target, Time timestamp);
void xdnd_handle_selection_request(Display *dpy, XSelectionRequestEvent *event);
void xdnd_handle_selection_notify(Display *dpy, XSelectionEvent *event);

// URI list utilities
char* xdnd_create_uri_list(const char **paths, int count);
char** xdnd_parse_uri_list(const char *data, int *count);
void xdnd_free_uri_list(char **uris, int count);

// Cache management for performance
void xdnd_cache_add(Window win);
bool xdnd_cache_check(Window win);
void xdnd_cache_clear(void);

#endif // AMIWB_XDND_H