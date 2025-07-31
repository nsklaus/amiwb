#include "menus.h"
#include "events.h"
#include "intuition.h"
#include "workbench.h"
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <stdio.h> // For fprintf

bool running = true;

// Initialize event handling
void init_events(void) {
    fprintf(stderr, "Initializing events\n");
    // no event system initialization needed for now; 
    // modules handle their own masks and events
}

// Main event loop
void handle_events(void) {
    Display *dpy = get_display();
    if (!dpy) {
        fprintf(stderr, "No display in handle_events\n");
        return;
    }

    fprintf(stderr, "Entering event loop\n");
    XEvent event;
    int fd = ConnectionNumber(dpy);  // X connection file descriptor for select
    while (running) {
        // fprintf(stderr, "Waiting for event\n");
        XNextEvent(dpy, &event);
        // fprintf(stderr, "Received event type %d\n", event.type);
        switch (event.type) {
            case ButtonPress:
                handle_button_press(&event.xbutton);
                break;
            case ButtonRelease:
                handle_button_release(&event.xbutton);
                break;
            case KeyPress:
                handle_key_press(&event.xkey);
                break;
            case Expose:
                handle_expose(&event.xexpose);
                break;
            case MapRequest:
                handle_map_request(&event.xmaprequest);
                break;
            case ConfigureRequest:
                handle_configure_request(&event.xconfigurerequest);
                break;
            case MotionNotify:
                handle_motion_notify(&event.xmotion);
                break;
            case PropertyNotify:
                handle_property_notify(&event.xproperty);
                break;
            case ConfigureNotify:
                handle_configure_notify(&event.xconfigure);
                break;
            case DestroyNotify:
                handle_destroy_notify(&event.xdestroywindow);
                break;
            default:
                // fprintf(stderr, "Unhandled event type %d\n", event.type);
                break;
        }
    }
}

void quit_event_loop(void) {
    running = false;
}

// Dispatch mouse button press
void handle_button_press(XButtonEvent *event) {
    Canvas *canvas = find_canvas(event->window);
    if (!canvas) {
        fprintf(stderr, "No canvas for ButtonPress event on window %lu\n", event->window);
        return;
    }
    // fprintf(stderr, "ButtonPress on canvas type %d\n", canvas->type);


    if (canvas->type == MENU) {
        //menu_handle_button_press(event);
        if (canvas == get_menubar()) {
            menu_handle_menubar_press(event);
        } else {
            menu_handle_button_press(event);
        }


    } else if (canvas == get_menubar()) {
        menu_handle_menubar_press(event);
    } else if (canvas->type == DESKTOP) {
        workbench_handle_button_press(event);
        intuition_handle_button_press(event);
    } else if (canvas->type == WINDOW) {
        intuition_handle_button_press(event);
        workbench_handle_button_press(event);
        
    } /*else {
        workbench_handle_button_press(event);
    }*/
}

// Dispatch mouse button release
void handle_button_release(XButtonEvent *event) {
    Canvas *canvas = find_canvas(event->window);
    //fprintf(stderr, "ButtonRelease event\n");
    if (!canvas) return;
    if (canvas->type == WINDOW) {
        intuition_handle_button_release(event);
        workbench_handle_button_release(event);
    } else {
        workbench_handle_button_release(event);
    }
}

// Dispatch key press
void handle_key_press(XKeyEvent *event) {
    fprintf(stderr, "KeyPress event\n");
    KeySym keysym = XLookupKeysym(event, 0);
    if (keysym == XK_Escape) {
        running = false;
        return;
    }
    menu_handle_key_press(event);
}

// Dispatch window expose
void handle_expose(XExposeEvent *event) {
    Canvas *canvas = find_canvas(event->window);
    //fprintf(stderr, "Expose event on window %lu (canvas type %d)\n", event->window, canvas ? canvas->type : -1);
    intuition_handle_expose(event);
}

void handle_map_request(XMapRequestEvent *event) {
    Canvas *canvas = find_canvas(event->window);
    if (!canvas) {
        fprintf(stderr, "MapRequest: No canvas, handling as client window %lu\n", event->window);
        intuition_handle_map_request(event);
    }
}

void handle_configure_request(XConfigureRequestEvent *event) {
    Canvas *canvas = find_canvas_by_client(event->window); // Will implement in intuition
    if (canvas) {
        fprintf(stderr, "ConfigureRequest on canvas window %lu\n", canvas->win);
        intuition_handle_configure_request(event);
    } else {
        fprintf(stderr, "ConfigureRequest on non-canvas window %lu\n", event->window);
        XWindowChanges changes;
        changes.x = event->x;
        changes.y = event->y;
        changes.width = event->width;
        changes.height = event->height;
        XConfigureWindow(get_display(), event->window, event->value_mask, &changes);
    }
}

// Dispatch property notify
void handle_property_notify(XPropertyEvent *event) {
    //fprintf(stderr, "PropertyNotify event on window %lu\n", event->window);
    intuition_handle_property_notify(event);
}

// Dispatch mouse motion
void handle_motion_notify(XMotionEvent *event) {
    Canvas *canvas = find_canvas(event->window);
    if (!canvas) {
        fprintf(stderr, "No canvas for MotionNotify event on window %lu\n", event->window);
        return;
    }

    //fprintf(stderr, "MotionNotify on canvas type %d\n", canvas->type);
    if (canvas->type == MENU) {
        //menu_handle_motion_notify(event); 
        //menu_handle_menubar_motion(event);

        if (canvas == get_menubar()) {
            menu_handle_menubar_motion(event);
        } else {
            menu_handle_motion_notify(event);
        }
    } 
    else if (canvas == get_menubar()) {
        menu_handle_menubar_motion(event);
        printf("something\n");
    } 
    else {
        workbench_handle_motion_notify(event); // Call first for icon dragging
        if (canvas->type == WINDOW) {
            intuition_handle_motion_notify(event); // Then window dragging
        }
    }
}

void handle_configure_notify(XConfigureEvent *event) {
    Canvas *canvas = find_canvas(event->window);
    if (canvas && canvas->type == WINDOW) {
        intuition_handle_configure_notify(event);
    }
}

void handle_destroy_notify(XDestroyWindowEvent *event) {
    Canvas *canvas = find_canvas_by_client(event->window);
    if (canvas) {
        //fprintf(stderr, "DestroyNotify on canvas client %lu\n", event->window);
        intuition_handle_destroy_notify(event);
    }
}