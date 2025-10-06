/* events.c - Event handling implementation */

/*
 * What could be added later:
 * - Keyboard shortcut handling
 * - Mouse wheel support
 * - Drag and drop
 * - Window state tracking
 * - Event timing/profiling
 */

#include "../toolkit/button/button.h"      /* Include toolkit first */
#include "../toolkit/inputfield/inputfield.h"   /* This pulls in main config.h */
#include "events.h"
#include "menus.h"
#include "logging.h"
#include "skeleton.h"
#include <stdbool.h>

int events_dispatch(SkeletonApp *app, XEvent *event) {
    if (!app) return 0;

    switch (event->type) {
        case Expose:
            if (event->xexpose.count == 0) {
                /* Redraw all widgets */
                skeleton_draw(app);
                log_message("Window exposed - redraw");
            }
            break;

        case ButtonPress:
            /* Handle widget clicks */
            bool needs_redraw = false;
            if (app->example_button) {
                if (button_handle_press(app->example_button,
                                       event->xbutton.x, event->xbutton.y)) {
                    needs_redraw = true;
                }
            }
            if (app->example_input) {
                if (inputfield_handle_click(app->example_input,
                                           event->xbutton.x, event->xbutton.y)) {
                    needs_redraw = true;
                }
            }
            if (needs_redraw) {
                skeleton_draw(app);
            }
            break;

        case ButtonRelease:
            /* Handle button release for proper button behavior */
            if (app->example_button) {
                if (button_handle_release(app->example_button,
                                         event->xbutton.x, event->xbutton.y)) {
                    log_message("Button clicked!");
                    skeleton_draw(app);  /* Redraw to show button unpressed */
                }
            }
            break;

        case KeyPress:
            /* Route to input field if it has focus */
            if (app->example_input) {
                inputfield_handle_key(app->example_input, &event->xkey);
                /* Redraw after keyboard input to show changes */
                skeleton_draw(app);
            }
            break;

        case ClientMessage:
            /* Check for window close */
            if ((Atom)event->xclient.data.l[0] ==
                XInternAtom(app->display, "WM_DELETE_WINDOW", False)) {
                log_message("Window close requested");
                return 0;  /* Quit */
            }
            /* Check for menu selection from AmiWB */
            if (event->xclient.message_type ==
                XInternAtom(app->display, "_AMIWB_MENU_SELECT", False)) {
                int menu_id = event->xclient.data.l[0];
                int item_id = event->xclient.data.l[1];
                menus_handle_selection(menu_id, item_id);

                /* Check if quit was selected */
                if (menu_id == 0 && item_id == 3) return 0;
            }
            break;
    }

    return 1;  /* Continue running */
}