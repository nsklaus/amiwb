#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include "icons.h"
#include "intuition.h"
#include "menus.h"
#include "events.h"
#include "render.h"

#define DEFAULT_WALLPAPER "/home/klaus/Pictures/backgrounds/classicmac1.png"

// Global variables
FileIcon *desktop_icons = NULL;
int num_desktop_icons = 0;
unsigned long desktop_label_color = 0xFFFFFFFF;
char *wallpaper_path = NULL;
char *def_tool_path = NULL;
char *def_drawer_path = NULL;
char *desktop_font_name = NULL;
XFontStruct *desktop_font = NULL;

// Parse .amiwbrc configuration
static void parse_config(Display *dpy, int screen) {
    char config_path[512], *home = getenv("HOME");
    snprintf(config_path, sizeof(config_path), "%s/.amiwbrc", home);
    FILE *fp = fopen(config_path, "r");
    if (!fp) return;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char *key = strtok(line, "="), *value = strtok(NULL, "\n");
        if (key && value) {
            while (*key == ' ') key++;
            while (*value == ' ') value++;
            if (strcmp(key, "desktop_label_color") == 0) {
                desktop_label_color = (strcmp(value, "white") == 0) ? 0xFFFFFFFF : (strcmp(value, "black") == 0) ? 0xFF000000 : 0xFFFFFFFF;
            } else if (strcmp(key, "wallpaper_path") == 0) {
                wallpaper_path = strdup(value);
            } else if (strcmp(key, "def_tool_path") == 0) {
                def_tool_path = strdup(value);
            } else if (strcmp(key, "def_drawer_path") == 0) {
                def_drawer_path = strdup(value);
            } else if (strcmp(key, "desktop_font") == 0) {
                desktop_font_name = strdup(value);
                desktop_font = XLoadQueryFont(dpy, desktop_font_name);
                if (!desktop_font) desktop_font = XLoadQueryFont(dpy, "fixed");
            }
        }
    }
    fclose(fp);
}

// Set root background
static void set_root_background(Display *dpy, Window root, int screen) {
    if (wallpaper_path) {
        char command[512];
        snprintf(command, sizeof(command), "DISPLAY=:1 hsetroot -tile %s", wallpaper_path);
        if (system(command) == 0) {
            XClearWindow(dpy, root);
            return;
        }
    }
    Pixmap pixmap = XCreatePixmap(dpy, root, 32, 32, DefaultDepth(dpy, screen));
    GC gc = DefaultGC(dpy, screen);
    XSetForeground(dpy, gc, 0x333333);
    XFillRectangle(dpy, pixmap, gc, 0, 0, 32, 32);
    XSetForeground(dpy, gc, 0x666666);
    for (int x = 0; x < 32; x += 8) {
        for (int y = 0; y < 32; y += 8) {
            if ((x / 8 + y / 8) % 2 == 0) {
                XFillRectangle(dpy, pixmap, gc, x, y, 8, 8);
            }
        }
    }
    XSetWindowBackgroundPixmap(dpy, root, pixmap);
    XClearWindow(dpy, root);
    XFreePixmap(dpy, pixmap);
}

// Main entry point
int main(void) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        printf("[main] ERROR: Cannot open display\n");
        exit(1);
    }
    int screen = DefaultScreen(dpy);
    Window root = DefaultRootWindow(dpy);

    def_tool_path = strdup("/home/klaus/Sources/amiwb/icons/def_tool.info");
    def_drawer_path = strdup("/home/klaus/Sources/amiwb/icons/def_drawer.info");
    parse_config(dpy, screen);
    set_root_background(dpy, root, screen);
    intuition_init(dpy, root);
    menus_init(dpy, root, screen, desktop_font);
    scan_existing_windows(dpy);
    scan_icons(dpy, root, "/home/klaus/Desktop/", &desktop_icons, &num_desktop_icons, desktop_label_color, 1, desktop_font);
    restack_windows(dpy);  // Ensure icons at bottom, windows above
    event_loop(dpy);
    return 0;
}
