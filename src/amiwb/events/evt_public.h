// File: evt_public.h
// Public API for AmiWB event system
// This header is included by external modules (main.c, etc.)

#ifndef EVT_PUBLIC_H
#define EVT_PUBLIC_H

#include <X11/Xlib.h>
#include <stdbool.h>

// ============================================================================
// Event System Lifecycle
// ============================================================================

void init_events(void);           // Initialize event handling
void handle_events(void);          // Main event loop
void quit_event_loop(void);        // Stop event loop
void restart_amiwb(void);          // Restart window manager

// Event loop state (encapsulated - use getters/setters)
bool evt_core_is_running(void);    // Check if event loop is running
void evt_core_stop(void);          // Stop the event loop

// ============================================================================
// Keyboard Events
// ============================================================================

void grab_global_shortcuts(Display *display, Window root);  // Grab global keys
void handle_key_press(XKeyEvent *event);                    // Dispatch key press

// ============================================================================
// Mouse Events
// ============================================================================

void handle_button_press(XButtonEvent *event);     // Dispatch button press
void handle_button_release(XButtonEvent *event);   // Dispatch button release
void handle_motion_notify(XMotionEvent *event);    // Dispatch mouse motion

// Clear press target when window is destroyed (called from intuition)
void clear_press_target_if_matches(Window win);

// ============================================================================
// Window Events
// ============================================================================

void handle_expose(XExposeEvent *event);                       // Dispatch expose
void handle_map_request(XMapRequestEvent *event);              // Dispatch map request
void handle_unmap_notify(XUnmapEvent *event);                  // Dispatch unmap notify
void handle_destroy_notify(XDestroyWindowEvent *event);        // Dispatch destroy notify

// ============================================================================
// Property Events
// ============================================================================

void handle_configure_request(XConfigureRequestEvent *event);  // Dispatch configure request
void handle_configure_notify(XConfigureEvent *event);          // Dispatch configure notify
void handle_property_notify(XPropertyEvent *event);            // Dispatch property notify

#endif // EVT_PUBLIC_H
