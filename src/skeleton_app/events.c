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

        case ConfigureNotify:
            /* Handle window resize - update widget geometry to match new window size
             *
             * This is the FLICKER-FREE RESIZE PATTERN for AmiWB native apps:
             * 1. ConfigureNotify fires when window size changes
             * 2. Update ALL widget geometries to match new window dimensions
             * 3. Recreate rendering resources (pixmap, picture, xft_draw) at new size
             * 4. DON'T draw here - wait for Expose event
             * 5. X11 automatically sends Expose after ConfigureNotify
             * 6. Single synchronized draw on Expose = smooth, flicker-free
             *
             * WHY this prevents flickering:
             * - background_pixmap = None prevents X11 auto-clear on Expose
             * - Widget geometry updated BEFORE drawing (no lag = no white gaps)
             * - Single draw (Expose) instead of double-draw (ConfigureNotify + Expose)
             */
            {
                int new_width = event->xconfigure.width;
                int new_height = event->xconfigure.height;

                /* Check if size actually changed (avoid unnecessary updates) */
                if (new_width != app->width || new_height != app->height) {
                    log_message("Window resized: %dx%d -> %dx%d",
                               app->width, app->height, new_width, new_height);

                    /* Update internal dimensions */
                    app->width = new_width;
                    app->height = new_height;

                    /* CRITICAL: Update ALL widget geometries to match new window size
                     * This prevents "white flashing" during resize (widgets lagging behind window growth)
                     */

                    /* Resize input field to match window width */
                    if (app->example_input) {
                        int new_input_width = new_width - 40;  /* 20px margin on each side */
                        if (new_input_width < 100) new_input_width = 100;  /* Minimum width */
                        inputfield_update_size(app->example_input, new_input_width);
                    }

                    /* Button stays at fixed position (20, 20)
                     * But you could also reposition it dynamically, for example:
                     * - Move to bottom-right corner:
                     *   app->example_button->x = new_width - 120;  // 100 width + 20 margin
                     *   app->example_button->y = new_height - 50;  // 30 height + 20 margin
                     * - Keep current position (no update needed)
                     */

                    /* IMPORTANT: No need to recreate Picture or XftDraw on resize!
                     * They are attached to the WINDOW, not to a pixmap buffer
                     * Window handle stays the same - only dimensions change
                     * Widget geometries are already updated above
                     *
                     * Standard X11 resize pattern: DON'T draw here - wait for Expose
                     * X11 will automatically send Expose events after ConfigureNotify
                     * Drawing here + on Expose = double-drawing = flickering!
                     * Let Expose handle the single synchronized redraw with updated geometry
                     */
                }
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