// File: events.h
#ifndef EVENTS_H
#define EVENTS_H

#include <X11/Xlib.h>
#include "intuition/itn_public.h"
#include "workbench/wb_public.h"
#include "menus/menu_public.h"

// Function prototypes
void init_events(void);									// Initialize event handling
void grab_global_shortcuts(Display *display, Window root); // Grab global keys
void handle_events(void); 								// Main event loop
void handle_button_press(XButtonEvent *event);			// Dispatch button press
void handle_button_release(XButtonEvent *event);		// Dispatch button release
void handle_key_press(XKeyEvent *event);				// Dispatch key press

void handle_expose(XExposeEvent *event);				// Dispatch expose
void handle_map_request(XMapRequestEvent *event);		// Dispatch map request
void handle_configure_request(XConfigureRequestEvent *event); // Dispatch configure request

void handle_motion_notify(XMotionEvent *event); 		// Dispatch mouse motion
void handle_property_notify(XPropertyEvent *event); 	// Dispatch property notify
void handle_configure_notify(XConfigureEvent *event); 	// Dispatch configure notify
void handle_unmap_notify(XUnmapEvent *event);          // Dispatch unmap notify
void handle_destroy_notify(XDestroyWindowEvent *event); // Dispatch destroy notify
void clear_press_target_if_matches(Window win);        // Clear press target if it matches
void quit_event_loop(void);
void restart_amiwb(void);                              // Restart window manager
extern bool running;
#endif