/* menus.c - Menu system implementation */

/*
 * What could be added later:
 * - Menu state updates (enable/disable items)
 * - Submenu support
 * - Recent files menu
 * - Dynamic menu generation
 * - Menu callbacks table
 */

#include "menus.h"
#include "logging.h"
#include <X11/Xatom.h>
#include <string.h>

void menus_init(Display *display, Window window) {
    /* Register app type with AmiWB */
    Atom app_type_atom = XInternAtom(display, "_AMIWB_APP_TYPE", False);
    const char *app_type = "Skeleton";
    XChangeProperty(display, window, app_type_atom,
                   XA_STRING, 8, PropModeReplace,
                   (unsigned char*)app_type, strlen(app_type));

    /* Define menu structure for AmiWB */
    Atom menu_data_atom = XInternAtom(display, "_AMIWB_MENU_DATA", False);
    const char *menu_data = "File:New,Open,Save,Quit|Edit:Cut,Copy,Paste|Help:About";
    XChangeProperty(display, window, menu_data_atom,
                   XA_STRING, 8, PropModeReplace,
                   (unsigned char*)menu_data, strlen(menu_data));

    log_message("Menus registered with AmiWB");
}

void menus_handle_selection(int menu_id, int item_id) {
    /* Simple example of menu handling */
    if (menu_id == 0) {  /* File menu */
        switch (item_id) {
            case 0: log_message("Menu: File->New"); break;
            case 1: log_message("Menu: File->Open"); break;
            case 2: log_message("Menu: File->Save"); break;
            case 3: log_message("Menu: File->Quit");
                   /* Main loop will handle quit */ break;
        }
    }
    /* Add more menu handlers as needed */
}