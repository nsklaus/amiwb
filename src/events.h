// File: events.h
#ifndef EVENTS_H
#define EVENTS_H

#include <X11/Xlib.h>
#include "intuition.h"
#include "workbench.h"
#include "menus.h"

// Function prototypes
void init_events(void);									// Initialize event handling
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
void handle_destroy_notify(XDestroyWindowEvent *event); // Dispatch destroy notify
void quit_event_loop(void);
extern bool running;
#endif