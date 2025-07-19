/* Events header: Declares the main event loop function and slot finder. Central for processing X events. */

#ifndef EVENTS_H
#define EVENTS_H

#include "intuition.h"
#include "menus.h"

// Central event loop function.
void handle_events(RenderContext *ctx, Canvas *desktop, Canvas *windows, int *num_windows, Window root, MenuBar *menubar, int randr_event_base); // Central event loop to process all X events
int find_free_slot(Canvas *windows, int num_windows, int max_windows);

#endif