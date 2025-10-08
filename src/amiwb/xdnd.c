#include "xdnd.h"
#include "icons.h"  // For FileIcon type
#include "config.h" // For PATH_SIZE, NAME_SIZE constants
#include "workbench/wb_public.h"
#include "workbench/wb_internal.h"
#include "intuition/itn_public.h" // For itn_canvas_find_by_window to exclude our own windows
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <X11/Xatom.h>

// Global XDND context
XdndContext xdnd_ctx = {0};

// Cache expiry time (5 seconds)
#define CACHE_EXPIRY_MS 5000

// Maximum cache size
#define MAX_CACHE_SIZE 100

// Initialize XDND support
void xdnd_init(Display *dpy) {
    // Register protocol atoms
    xdnd_ctx.XdndAware = XInternAtom(dpy, "XdndAware", False);
    xdnd_ctx.XdndSelection = XInternAtom(dpy, "XdndSelection", False);
    xdnd_ctx.XdndProxy = XInternAtom(dpy, "XdndProxy", False);
    xdnd_ctx.XdndTypeList = XInternAtom(dpy, "XdndTypeList", False);

    // Message atoms
    xdnd_ctx.XdndEnter = XInternAtom(dpy, "XdndEnter", False);
    xdnd_ctx.XdndPosition = XInternAtom(dpy, "XdndPosition", False);
    xdnd_ctx.XdndStatus = XInternAtom(dpy, "XdndStatus", False);
    xdnd_ctx.XdndLeave = XInternAtom(dpy, "XdndLeave", False);
    xdnd_ctx.XdndDrop = XInternAtom(dpy, "XdndDrop", False);
    xdnd_ctx.XdndFinished = XInternAtom(dpy, "XdndFinished", False);

    // Action atoms
    xdnd_ctx.XdndActionCopy = XInternAtom(dpy, "XdndActionCopy", False);
    xdnd_ctx.XdndActionMove = XInternAtom(dpy, "XdndActionMove", False);
    xdnd_ctx.XdndActionLink = XInternAtom(dpy, "XdndActionLink", False);
    xdnd_ctx.XdndActionAsk = XInternAtom(dpy, "XdndActionAsk", False);
    xdnd_ctx.XdndActionPrivate = XInternAtom(dpy, "XdndActionPrivate", False);

    // Data type atoms
    xdnd_ctx.text_uri_list = XInternAtom(dpy, "text/uri-list", False);
    xdnd_ctx.text_plain = XInternAtom(dpy, "text/plain", False);
    xdnd_ctx.UTF8_STRING = XInternAtom(dpy, "UTF8_STRING", False);

    // Initialize cache
    xdnd_ctx.cache_capacity = 16;
    xdnd_ctx.aware_cache = malloc(sizeof(Window) * xdnd_ctx.cache_capacity);
    xdnd_ctx.cache_size = 0;
    xdnd_ctx.cache_timestamp = CurrentTime;

    // Clear state
    xdnd_ctx.current_target = None;
    xdnd_ctx.last_target = None;
    xdnd_ctx.drop_source = None;
    xdnd_ctx.offered_types = NULL;
    xdnd_ctx.offered_count = 0;

    // Silent initialization - no logging in normal operation
}

// Shutdown and cleanup
void xdnd_shutdown(Display *dpy) {
    if (xdnd_ctx.aware_cache) {
        free(xdnd_ctx.aware_cache);
        xdnd_ctx.aware_cache = NULL;
    }
    if (xdnd_ctx.offered_types) {
        free(xdnd_ctx.offered_types);
        xdnd_ctx.offered_types = NULL;
    }
    xdnd_ctx.cache_size = 0;
    xdnd_ctx.cache_capacity = 0;
}

// Mark a window as XDND-aware
void xdnd_make_aware(Display *dpy, Window win, int version) {
    Atom actual;
    int format;
    unsigned long count, remaining;
    unsigned char *data = NULL;

    // Check if already aware
    if (XGetWindowProperty(dpy, win, xdnd_ctx.XdndAware,
                          0, 1, False, XA_ATOM,
                          &actual, &format, &count, &remaining, &data) == Success) {
        if (data) {
            XFree(data);
            return;  // Already aware
        }
    }

    // Set XdndAware property
    long ver = version;
    XChangeProperty(dpy, win, xdnd_ctx.XdndAware, XA_ATOM, 32,
                   PropModeReplace, (unsigned char*)&ver, 1);

    // Silent operation - window marked as XDND-aware
}

// Check if a window is XDND-aware (with caching)
bool xdnd_is_aware(Display *dpy, Window win) {
    // Check cache first
    if (xdnd_cache_check(win)) {
        return true;
    }

    Atom actual;
    int format;
    unsigned long count, remaining;
    unsigned char *data = NULL;

    if (XGetWindowProperty(dpy, win, xdnd_ctx.XdndAware,
                          0, 1, False, XA_ATOM,
                          &actual, &format, &count, &remaining, &data) == Success) {
        if (data) {
            long version = *(long*)data;
            XFree(data);

            // Add to cache if aware
            if (version >= XDND_THREE) {
                xdnd_cache_add(win);
                return true;
            }
        }
    }
    return false;
}

// Find XDND-aware window at given coordinates
Window xdnd_find_target(Display *dpy, int root_x, int root_y) {
    extern bool is_window_valid(Display *dpy, Window win);

    Window root = DefaultRootWindow(dpy);
    Window parent = root;
    Window child = None;
    int win_x = root_x;
    int win_y = root_y;

    // Walk down the window tree to find deepest window at position
    while (child != None || parent == root) {
        if (!safe_translate_coordinates(dpy, root, parent, root_x, root_y,
                                   &win_x, &win_y, &child)) {
            break;
        }

        if (child == None) {
            break;
        }

        // Validate child window before using it (prevent BadMatch on destroyed windows)
        if (!is_window_valid(dpy, child)) {
            break;
        }

        // Check if this window is XDND-aware
        if (xdnd_is_aware(dpy, child)) {
            // Check for proxy window
            Atom actual;
            int format;
            unsigned long count, remaining;
            unsigned char *data = NULL;

            if (XGetWindowProperty(dpy, child, xdnd_ctx.XdndProxy,
                                  0, 1, False, XA_WINDOW,
                                  &actual, &format, &count, &remaining, &data) == Success) {
                if (data && format == 32 && count == 1) {
                    Window proxy = *(Window*)data;
                    XFree(data);

                    // Verify proxy is valid before using it
                    if (!is_window_valid(dpy, proxy)) {
                        break;
                    }

                    // Verify proxy points back to the original window
                    if (XGetWindowProperty(dpy, proxy, xdnd_ctx.XdndProxy,
                                         0, 1, False, XA_WINDOW,
                                         &actual, &format, &count, &remaining, &data) == Success) {
                        if (data && format == 32 && count == 1 && *(Window*)data == proxy) {
                            XFree(data);
                            return proxy;
                        }
                        if (data) XFree(data);
                    }
                }
            }
            return child;
        }

        parent = child;
    }

    // Check the last parent if no child was XDND-aware
    if (parent != root && is_window_valid(dpy, parent) && xdnd_is_aware(dpy, parent)) {
        return parent;
    }
    return None;
}

// Send XdndEnter message
void xdnd_send_enter(Display *dpy, Window source, Window target) {
    if (target == None || target == xdnd_ctx.current_target) {
        return;  // Already in this target
    }

    XEvent evt;
    memset(&evt, 0, sizeof(evt));

    evt.type = ClientMessage;
    evt.xclient.window = target;
    evt.xclient.message_type = xdnd_ctx.XdndEnter;
    evt.xclient.format = 32;
    evt.xclient.data.l[0] = source;
    evt.xclient.data.l[1] = (XDND_VERSION << 24) | 0;  // Version 5, no more than 3 types
    evt.xclient.data.l[2] = xdnd_ctx.text_uri_list;
    evt.xclient.data.l[3] = xdnd_ctx.text_plain;
    evt.xclient.data.l[4] = 0;

    XSendEvent(dpy, target, False, NoEventMask, &evt);
    XFlush(dpy);

    xdnd_ctx.current_target = target;
    xdnd_ctx.target_accepts = false;  // Reset until we get status

    // XdndEnter sent
}

// Send XdndPosition message
void xdnd_send_position(Display *dpy, Window source, Window target,
                        int root_x, int root_y, Time timestamp, Atom action) {
    if (target == None) return;

    XEvent evt;
    memset(&evt, 0, sizeof(evt));

    evt.type = ClientMessage;
    evt.xclient.window = target;
    evt.xclient.message_type = xdnd_ctx.XdndPosition;
    evt.xclient.format = 32;
    evt.xclient.data.l[0] = source;
    evt.xclient.data.l[1] = 0;  // Reserved
    evt.xclient.data.l[2] = (root_x << 16) | (root_y & 0xFFFF);
    evt.xclient.data.l[3] = timestamp;
    evt.xclient.data.l[4] = action ? action : xdnd_ctx.XdndActionCopy;

    XSendEvent(dpy, target, False, NoEventMask, &evt);
    XFlush(dpy);
}

// Send XdndLeave message
void xdnd_send_leave(Display *dpy, Window source, Window target) {
    if (target == None) return;

    XEvent evt;
    memset(&evt, 0, sizeof(evt));

    evt.type = ClientMessage;
    evt.xclient.window = target;
    evt.xclient.message_type = xdnd_ctx.XdndLeave;
    evt.xclient.format = 32;
    evt.xclient.data.l[0] = source;
    evt.xclient.data.l[1] = 0;
    evt.xclient.data.l[2] = 0;
    evt.xclient.data.l[3] = 0;
    evt.xclient.data.l[4] = 0;

    XSendEvent(dpy, target, False, NoEventMask, &evt);
    XFlush(dpy);

    if (target == xdnd_ctx.current_target) {
        xdnd_ctx.current_target = None;
        xdnd_ctx.target_accepts = false;
    }

    // XdndLeave sent
}

// Send XdndDrop message
void xdnd_send_drop(Display *dpy, Window source, Window target, Time timestamp) {
    if (target == None) return;

    XEvent evt;
    memset(&evt, 0, sizeof(evt));

    evt.type = ClientMessage;
    evt.xclient.window = target;
    evt.xclient.message_type = xdnd_ctx.XdndDrop;
    evt.xclient.format = 32;
    evt.xclient.data.l[0] = source;
    evt.xclient.data.l[1] = 0;  // Reserved
    evt.xclient.data.l[2] = timestamp;
    evt.xclient.data.l[3] = 0;
    evt.xclient.data.l[4] = 0;

    XSendEvent(dpy, target, False, NoEventMask, &evt);
    XFlush(dpy);

    // XdndDrop sent
}

// Send XdndFinished message
void xdnd_send_finished(Display *dpy, Window source, Window target) {
    if (target == None) return;

    XEvent evt;
    memset(&evt, 0, sizeof(evt));

    evt.type = ClientMessage;
    evt.xclient.window = target;
    evt.xclient.message_type = xdnd_ctx.XdndFinished;
    evt.xclient.format = 32;
    evt.xclient.data.l[0] = source;
    evt.xclient.data.l[1] = 1;  // Drop accepted and successful
    evt.xclient.data.l[2] = xdnd_ctx.XdndActionCopy;  // Action performed
    evt.xclient.data.l[3] = 0;
    evt.xclient.data.l[4] = 0;

    XSendEvent(dpy, target, False, NoEventMask, &evt);
    XFlush(dpy);

    // XdndFinished sent
}

// Send XdndStatus message (when we're the target)
void xdnd_send_status(Display *dpy, Window source, Window target,
                     bool will_accept, int x, int y, int w, int h, Atom action) {
    XEvent evt;
    memset(&evt, 0, sizeof(evt));

    evt.type = ClientMessage;
    evt.xclient.window = source;
    evt.xclient.message_type = xdnd_ctx.XdndStatus;
    evt.xclient.format = 32;
    evt.xclient.data.l[0] = target;
    evt.xclient.data.l[1] = will_accept ? 1 : 0;  // Bit 0: accept, Bit 1: want position updates
    evt.xclient.data.l[2] = (x << 16) | (y & 0xFFFF);  // Rectangle where no more position updates needed
    evt.xclient.data.l[3] = (w << 16) | (h & 0xFFFF);
    evt.xclient.data.l[4] = will_accept ? action : None;

    XSendEvent(dpy, source, False, NoEventMask, &evt);
    XFlush(dpy);
}

// Create URI list from file paths
char* xdnd_create_uri_list(const char **paths, int count) {
    if (!paths || count <= 0) return NULL;

    // Calculate total size needed
    size_t total_size = 0;
    for (int i = 0; i < count; i++) {
        if (!paths[i]) continue;

        // "file://" + PATH_SIZE (max path) + "\r\n" + null terminator
        // Use PATH_SIZE to be safe for expanded paths from realpath()
        total_size += 7 + PATH_SIZE + 2 + 1;
    }

    if (total_size == 0) return NULL;

    char *uri_list = malloc(total_size);
    if (!uri_list) return NULL;

    char *ptr = uri_list;
    for (int i = 0; i < count; i++) {
        if (!paths[i]) continue;

        // Convert to absolute path if needed
        char abs_path[PATH_SIZE];
        if (paths[i][0] != '/') {
            if (!realpath(paths[i], abs_path)) {
                strncpy(abs_path, paths[i], PATH_SIZE - 1);  // Fall back to original
                abs_path[PATH_SIZE - 1] = '\0';
            }
        } else {
            strncpy(abs_path, paths[i], PATH_SIZE - 1);
            abs_path[PATH_SIZE - 1] = '\0';
        }

        // Build URI
        ptr += sprintf(ptr, "file://%s\r\n", abs_path);
    }

    return uri_list;
}

// Parse URI list into array of paths
char** xdnd_parse_uri_list(const char *data, int *count) {
    if (!data || !count) return NULL;

    *count = 0;

    // Count URIs (lines)
    const char *p = data;
    while (*p) {
        if (*p == '\n') (*count)++;
        p++;
    }
    if (p > data && p[-1] != '\n') (*count)++;  // Last line without newline

    if (*count == 0) return NULL;

    char **paths = malloc(sizeof(char*) * (*count));
    if (!paths) {
        *count = 0;
        return NULL;
    }

    // Parse each URI
    int index = 0;
    p = data;
    while (*p && index < *count) {
        // Skip whitespace
        while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        if (!*p) break;

        // Find end of line
        const char *line_start = p;
        while (*p && *p != '\r' && *p != '\n') p++;

        size_t line_len = p - line_start;
        if (line_len == 0) continue;

        // Check for file:// prefix
        if (line_len > 7 && strncmp(line_start, "file://", 7) == 0) {
            // Extract path (skip file://)
            size_t path_len = line_len - 7;
            paths[index] = malloc(path_len + 1);
            if (paths[index]) {
                memcpy(paths[index], line_start + 7, path_len);
                paths[index][path_len] = '\0';

                // TODO: URL decode special characters (%20 -> space, etc.)

                index++;
            }
        } else {
            // Not a file URI, skip
            continue;
        }
    }

    *count = index;
    return paths;
}

// Free URI list
void xdnd_free_uri_list(char **uris, int count) {
    if (!uris) return;
    for (int i = 0; i < count; i++) {
        if (uris[i]) free(uris[i]);
    }
    free(uris);
}

// Cache management
void xdnd_cache_add(Window win) {
    // Check if already in cache
    for (int i = 0; i < xdnd_ctx.cache_size; i++) {
        if (xdnd_ctx.aware_cache[i] == win) return;
    }

    // Grow cache if needed
    if (xdnd_ctx.cache_size >= xdnd_ctx.cache_capacity) {
        if (xdnd_ctx.cache_capacity >= MAX_CACHE_SIZE) {
            // Cache full, clear old entries
            xdnd_cache_clear();
        } else {
            // Double capacity
            int new_capacity = xdnd_ctx.cache_capacity * 2;
            if (new_capacity > MAX_CACHE_SIZE) new_capacity = MAX_CACHE_SIZE;

            Window *new_cache = realloc(xdnd_ctx.aware_cache, sizeof(Window) * new_capacity);
            if (new_cache) {
                xdnd_ctx.aware_cache = new_cache;
                xdnd_ctx.cache_capacity = new_capacity;
            }
        }
    }

    // Add to cache
    if (xdnd_ctx.cache_size < xdnd_ctx.cache_capacity) {
        xdnd_ctx.aware_cache[xdnd_ctx.cache_size++] = win;
    }
}

bool xdnd_cache_check(Window win) {
    for (int i = 0; i < xdnd_ctx.cache_size; i++) {
        if (xdnd_ctx.aware_cache[i] == win) return true;
    }
    return false;
}

void xdnd_cache_clear(void) {
    xdnd_ctx.cache_size = 0;
    xdnd_ctx.cache_timestamp = CurrentTime;
}

// Handle incoming XdndEnter
void xdnd_handle_enter(Display *dpy, XClientMessageEvent *event) {
    Window source = event->data.l[0];
    int version = (event->data.l[1] >> 24) & 0xFF;
    bool has_more_types = event->data.l[1] & 1;

    // Received XdndEnter
    (void)version;  // Version could be used for compatibility checks if needed

    // Store source window
    xdnd_ctx.drop_source = source;

    // Clear old type list
    if (xdnd_ctx.offered_types) {
        free(xdnd_ctx.offered_types);
        xdnd_ctx.offered_types = NULL;
        xdnd_ctx.offered_count = 0;
    }

    if (has_more_types) {
        // Read type list from property
        Atom actual;
        int format;
        unsigned long count, remaining;
        unsigned char *data = NULL;

        if (XGetWindowProperty(dpy, source, xdnd_ctx.XdndTypeList,
                              0, 1000, False, XA_ATOM,
                              &actual, &format, &count, &remaining, &data) == Success) {
            if (data && format == 32) {
                xdnd_ctx.offered_types = malloc(sizeof(Atom) * count);
                if (xdnd_ctx.offered_types) {
                    memcpy(xdnd_ctx.offered_types, data, sizeof(Atom) * count);
                    xdnd_ctx.offered_count = count;
                }
            }
            if (data) XFree(data);
        }
    } else {
        // Types are in the message
        xdnd_ctx.offered_types = malloc(sizeof(Atom) * 3);
        if (xdnd_ctx.offered_types) {
            int count = 0;
            for (int i = 2; i < 5; i++) {
                if (event->data.l[i] != None) {
                    xdnd_ctx.offered_types[count++] = event->data.l[i];
                }
            }
            xdnd_ctx.offered_count = count;
        }
    }

    // Check if we support any of the offered types
    xdnd_ctx.will_accept = false;
    for (int i = 0; i < xdnd_ctx.offered_count; i++) {
        if (xdnd_ctx.offered_types[i] == xdnd_ctx.text_uri_list ||
            xdnd_ctx.offered_types[i] == xdnd_ctx.text_plain) {
            xdnd_ctx.will_accept = true;
            break;
        }
    }
}

// Handle incoming XdndPosition
void xdnd_handle_position(Display *dpy, XClientMessageEvent *event) {
    Window source = event->data.l[0];
    int x = (event->data.l[2] >> 16) & 0xFFFF;
    int y = event->data.l[2] & 0xFFFF;
    // Time timestamp = event->data.l[3];  // Currently unused
    // Atom action = event->data.l[4];      // Currently unused

    // Store position for drop
    xdnd_ctx.pending_x = x;
    xdnd_ctx.pending_y = y;

    // Send status response
    // TODO: Check if position is over a valid drop zone
    xdnd_send_status(dpy, source, event->window,
                    xdnd_ctx.will_accept, x, y, 100, 100,
                    xdnd_ctx.XdndActionCopy);
}

// Handle incoming XdndLeave
void xdnd_handle_leave(Display *dpy, XClientMessageEvent *event) {
    // Received XdndLeave

    // Clear drop state
    xdnd_ctx.drop_source = None;
    if (xdnd_ctx.offered_types) {
        free(xdnd_ctx.offered_types);
        xdnd_ctx.offered_types = NULL;
        xdnd_ctx.offered_count = 0;
    }
    xdnd_ctx.will_accept = false;
}

// Handle incoming XdndDrop
void xdnd_handle_drop(Display *dpy, XClientMessageEvent *event) {
    Window source = event->data.l[0];
    Time timestamp = event->data.l[2];

    // Received XdndDrop

    if (!xdnd_ctx.will_accept) {
        // Send finished with failure
        xdnd_send_finished(dpy, event->window, source);
        return;
    }

    // Request the selection data
    Atom target_type = None;
    for (int i = 0; i < xdnd_ctx.offered_count; i++) {
        if (xdnd_ctx.offered_types[i] == xdnd_ctx.text_uri_list) {
            target_type = xdnd_ctx.text_uri_list;
            break;
        } else if (xdnd_ctx.offered_types[i] == xdnd_ctx.text_plain) {
            target_type = xdnd_ctx.text_plain;
        }
    }

    if (target_type != None) {
        xdnd_request_selection(dpy, event->window, target_type, timestamp);
    } else {
        xdnd_send_finished(dpy, event->window, source);
    }
}

// Request selection data
void xdnd_request_selection(Display *dpy, Window requestor, Atom target, Time timestamp) {
    Atom prop = XInternAtom(dpy, "XDND_DATA", False);
    XConvertSelection(dpy, xdnd_ctx.XdndSelection, target, prop, requestor, timestamp);
}


// Handle selection request (when we're the source)
void xdnd_handle_selection_request(Display *dpy, XSelectionRequestEvent *event) {
    XSelectionEvent response;
    response.type = SelectionNotify;
    response.requestor = event->requestor;
    response.selection = event->selection;
    response.target = event->target;
    response.time = event->time;
    response.property = None;

    // Check if this is for our XDND selection
    if (event->selection == xdnd_ctx.XdndSelection) {
        // Get file path from current drag operation
        FileIcon *icon = wb_drag_get_dragged_icon();
        if (icon && icon->path) {
            char *uri_list = xdnd_create_uri_list((const char**)&icon->path, 1);

            if (uri_list) {
                if (event->target == xdnd_ctx.text_uri_list) {
                    XChangeProperty(dpy, event->requestor, event->property,
                                   xdnd_ctx.text_uri_list, 8, PropModeReplace,
                                   (unsigned char*)uri_list, strlen(uri_list));
                    response.property = event->property;
                    // URI sent
                } else if (event->target == xdnd_ctx.text_plain) {
                    XChangeProperty(dpy, event->requestor, event->property,
                                   xdnd_ctx.text_plain, 8, PropModeReplace,
                                   (unsigned char*)icon->path, strlen(icon->path));
                    response.property = event->property;
                    // Plain path sent
                }
                free(uri_list);
            }
        }
    }

    XSendEvent(dpy, event->requestor, False, 0, (XEvent*)&response);
    XFlush(dpy);

    // Clean up drag state after successful selection transfer
    // Some targets don't send XdndFinished, so clean up here as well
    if (response.property != None) {
        workbench_cleanup_drag_state();
    }
}

// Handle selection notify (when we're the target receiving data)
void xdnd_handle_selection_notify(Display *dpy, XSelectionEvent *event) {
    if (event->property == None) {
        // Selection conversion failed - silent
        return;
    }

    // Read the property
    Atom actual;
    int format;
    unsigned long count, remaining;
    unsigned char *data = NULL;

    if (XGetWindowProperty(dpy, event->requestor, event->property,
                          0, LONG_MAX, True, AnyPropertyType,
                          &actual, &format, &count, &remaining, &data) == Success) {
        if (data) {
            if (actual == xdnd_ctx.text_uri_list) {
                // Parse URI list
                int uri_count;
                char **uris = xdnd_parse_uri_list((char*)data, &uri_count);

                if (uris) {
                    // Received files - TODO: Create file icons for dropped files
                    for (int i = 0; i < uri_count; i++) {
                        // Process each URI: uris[i]
                    }
                    xdnd_free_uri_list(uris, uri_count);
                }
            } else if (actual == xdnd_ctx.text_plain) {
                // Received plain text
            }

            XFree(data);
        }
    }

    // Send finished message
    if (xdnd_ctx.drop_source != None) {
        xdnd_send_finished(dpy, event->requestor, xdnd_ctx.drop_source);
        xdnd_ctx.drop_source = None;
    }
}