// File: evt_property.c
// Property and configuration event handling for AmiWB event system
// Routes property changes, configure requests, and configure notify events

#include "evt_internal.h"
#include "../workbench/wb_public.h"
#include "../menus/menu_public.h"
#include "../render.h"  // For redraw_canvas()
#include "../config.h"  // For PATH_SIZE
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdlib.h>  // For free(), strdup()
#include <string.h>  // For strdup()

// ============================================================================
// Configuration Event Dispatchers (Public API)
// ============================================================================

// Client wants to move/resize - this is THE ONLY way clients should resize
void handle_configure_request(XConfigureRequestEvent *event) {
    Canvas *canvas = itn_canvas_find_by_client(event->window);
    if (canvas) {
        // Managed window - handle the request properly
        intuition_handle_configure_request(event);
    } else {
        // Unmanaged window - just pass it through
        XWindowChanges changes;
        changes.x = event->x;
        changes.y = event->y;
        changes.width = event->width;
        changes.height = event->height;
        changes.border_width = event->border_width;
        changes.sibling = event->above;
        changes.stack_mode = event->detail;
        XConfigureWindow(itn_core_get_display(), event->window, event->value_mask, &changes);
    }
}

// Handle ConfigureNotify events - ONLY for our own frame windows
void handle_configure_notify(XConfigureEvent *event) {
    // Only handle ConfigureNotify for OUR frame windows
    Canvas *canvas = itn_canvas_find_by_window(event->window);
    if (canvas && (canvas->type == WINDOW || canvas->type == DIALOG)) {
        intuition_handle_configure_notify(event);
    }
    // IGNORE all ConfigureNotify from client windows - this is the standard!
    // Clients must use ConfigureRequest to resize, not resize themselves
}

// ============================================================================
// Property Event Dispatcher (Public API)
// ============================================================================

// Dispatch property notify - WM hints, protocols, and netwm properties changes
// Also handles dynamic title updates via _AMIWB_TITLE_CHANGE property
void handle_property_notify(XPropertyEvent *event) {
    if (!event) return;

    Display *dpy = itn_core_get_display();

    // First check for AMIWB_OPEN_DIRECTORY property on root window (from ReqASL)
    if (event->window == DefaultRootWindow(dpy)) {
        Atom amiwb_open_dir = XInternAtom(dpy, "AMIWB_OPEN_DIRECTORY", False);

        if (event->atom == amiwb_open_dir && event->state == PropertyNewValue) {
            // Read the directory path from the property
            Atom actual_type;
            int actual_format;
            unsigned long nitems, bytes_after;
            unsigned char *prop_data = NULL;

            if (XGetWindowProperty(dpy, event->window, amiwb_open_dir,
                                 0, PATH_SIZE, True, // Delete property after reading
                                 XA_STRING, &actual_type, &actual_format,
                                 &nitems, &bytes_after, &prop_data) == Success) {

                if (prop_data && nitems > 0) {
                    // Open the directory in a new workbench window
                    workbench_open_directory((char *)prop_data);
                    XFree(prop_data);
                }
            }
            return;
        }
    }

    // Now check for properties on client windows
    Canvas *canvas = itn_canvas_find_by_client(event->window);
    if (!canvas) {
        // Try to find by main window too
        canvas = itn_canvas_find_by_window(event->window);
        if (!canvas) {
            return;
        }
    }

    // Check if this is our custom title change property
    Atom amiwb_title_change = XInternAtom(dpy, "_AMIWB_TITLE_CHANGE", False);
    Atom amiwb_menu_states = XInternAtom(dpy, "_AMIWB_MENU_STATES", False);

    // Handle menu state changes
    if (event->atom == amiwb_menu_states) {
        // Call menu handler to update menu states
        handle_menu_state_change(event->window);
        return;
    }

    if (event->atom == amiwb_title_change) {
        if (event->state == PropertyNewValue) {
            // Get the new title from the property
            Atom actual_type;
            int actual_format;
            unsigned long nitems, bytes_after;
            unsigned char *data = NULL;

            if (XGetWindowProperty(dpy, event->window, amiwb_title_change,
                                  0, 256, False, AnyPropertyType,
                                  &actual_type, &actual_format, &nitems, &bytes_after,
                                  &data) == Success && data) {

                // Update the canvas title_change field
                if (canvas->title_change) {
                    free(canvas->title_change);
                    canvas->title_change = NULL;
                }

                // Only set if we got valid data
                if (nitems > 0) {
                    canvas->title_change = strdup((char *)data);
                    if (!canvas->title_change) {
                        log_error("[ERROR] Failed to allocate memory for title_change");
                    }
                }
                XFree(data);

                // Trigger a redraw of the canvas to show the new title
                redraw_canvas(canvas);
            }
        } else if (event->state == PropertyDelete) {
            // Property was deleted - revert to base title
            if (canvas->title_change) {
                free(canvas->title_change);
                canvas->title_change = NULL;
                redraw_canvas(canvas);
            }
        }
    }
}
