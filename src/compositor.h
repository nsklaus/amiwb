#pragma once
#include <stdbool.h>
#include <X11/Xlib.h>

// Initialize a minimal compositor selection owner so ARGB windows are supported
// Returns true if selection was acquired.
bool init_compositor(Display *dpy);

// Release selection and any resources
void shutdown_compositor(Display *dpy);

// Feed X events to compositor so it can rebuild and repaint when topology/damage changes
void compositor_handle_event(Display *dpy, XEvent *ev);

// Force a repaint of the composed scene
void compositor_repaint(Display *dpy);

// Re-read stacking from XQueryTree and repaint (use after XRaiseWindow/XLowerWindow)
void compositor_sync_stacking(Display *dpy);

// Debug: dump compositor stacking/paint order with an optional tag
void compositor_dump_order(const char *tag);
