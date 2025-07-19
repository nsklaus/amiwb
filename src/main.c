/* Main entry point: Initializes display, context, menubar, desktop, manages windows, sets wallpaper, enters event loop, and cleans up. Starts the WM. */

#include "events.h"
#include "icons.h"
#include "intuition.h"
#include "render.h"
#include "workbench.h"
#include "menus.h"
#include <X11/Xlib.h>   
#include <X11/Xutil.h>  
#include <X11/extensions/Xrender.h>  
#include <X11/extensions/Xrandr.h>  
#include <X11/Xft/Xft.h>            
#include <X11/cursorfont.h>   
#include <stdio.h>      
#include <stdlib.h>     
#include <pwd.h>        // for getpwuid to get user information like home directory.
#include <unistd.h>     // for getuid to get user ID.
#include <sys/stat.h>   // for stat and mkdir to check/create directories.
#include <string.h>     // ftring functions like strdup, strcpy.

#define RESOURCE_DIR_SYSTEM "/usr/local/share/amiwb"  // system-wide resource dir for assets, icons, patterns..
#define RESOURCE_DIR_USER ".config/amiwb"  // user config dir.

// Function to get full path to a resource, preferring user dir, then system dir.
// Uses access() to check if file exists (F_OK flag).
static char *get_resource_path(const char *rel_path) {
    char *home = getenv("HOME");  // Get user's home directory from environment.
    char user_path[1024];  // Buffer for user path (fixed size to avoid overflow).
    snprintf(user_path, sizeof(user_path), "%s/%s/%s", home, RESOURCE_DIR_USER, rel_path);  // Build user path.
    if (access(user_path, F_OK) == 0) {  // If user path exists, return a copy.
        return strdup(user_path);
    }
    char sys_path[1024];  // Buffer for system path.
    snprintf(sys_path, sizeof(sys_path), "%s/%s", RESOURCE_DIR_SYSTEM, rel_path);  // Build system path.
    return strdup(sys_path);  // Return system path copy if user path not found.
}

// Custom X error handler to catch and print errors without crashing.
// Ignores specific non-fatal errors from extensions like Xrender.
static int x_error_handler(Display *dpy, XErrorEvent *error) {

    /* 
    // harmless errors usualy get ignored, but i want to see it all for now
    // so this block gets commented out for now
   
    if (error->request_code == 139) {  // Check for specific Xrender-related errors.
        if (error->error_code == 9 && error->minor_code == 4) return 0;  // Ignore BadPicture for XRenderFreePicture.
        if (error->error_code == 143 && (error->minor_code == 5 || error->minor_code == 7 || error->minor_code == 8)) return 0;  // Ignore other Xrender ops.
    }
    */

    char buf[256];  // Buffer for error text.
    XGetErrorText(dpy, error->error_code, buf, sizeof(buf));  // Get human-readable error message.
    fprintf(stderr, "X Error: %s, request_code: %d, minor_code: %d, resourceid: %lu\n", buf, error->request_code, error->minor_code, error->resourceid);  // Print to stderr.
    return 0;  // Return 0 to continue execution.
}

int main() {
    Display *dpy = XOpenDisplay(NULL);  // Open connection to X server; NULL uses default DISPLAY env var.
    if (!dpy) return 1;  // Exit if connection fails.
    XSetErrorHandler(x_error_handler);  // Set custom error handler for X errors.
    int screen = DefaultScreen(dpy);  // Get default screen number.
    Window root = RootWindow(dpy, screen);  // Get root window (desktop background).
    Cursor root_cursor = XCreateFontCursor(dpy, XC_left_ptr);  // Create standard left arrow cursor.
    XDefineCursor(dpy, root, root_cursor);  // Set cursor for root window.
    
    XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask | StructureNotifyMask | PropertyChangeMask);  // Select events for WM: redirects for managing windows, notifies for changes.
    XVisualInfo vinfo;  // Structure to hold visual info.
    XMatchVisualInfo(dpy, screen, 32, TrueColor, &vinfo);  // Find 32-bit TrueColor visual for alpha support.
    if (!XMatchVisualInfo(dpy, screen, 32, TrueColor, &vinfo)) {
        fprintf(stderr, "No 32-bit TrueColor visual available\n");
        XCloseDisplay(dpy);
        return 1;
    }
    Visual *visual = vinfo.visual;  // Extract visual.
    Colormap cmap = XCreateColormap(dpy, root, visual, AllocNone);  // Create colormap for the visual.
    char *font_path = get_resource_path("fonts/SourceCodePro-Regular.otf");  // Get dynamic font path.
    XftFont *font = XftFontOpen(dpy, screen, XFT_FILE, FcTypeString, font_path, XFT_SIZE, FcTypeDouble, 14.0, NULL);  // Open font using dynamic path; Xft for modern font rendering.
    free(font_path);  // Free allocated path.
    if (!font) {  // Check if font loaded.
        fprintf(stderr, "Font not found\n");  // Error message.
        return 1;  // Exit.
    }
    XftColor white;  // Xft color structure.
    XftColorAllocName(dpy, visual, cmap, "white", &white);  // Allocate white color for text.
    XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy, visual);  // Find picture format for rendering.
if (!fmt) {
fprintf(stderr, "No render format for visual\n");
XCloseDisplay(dpy);
return 1;
}
    int randr_event_base, randr_error_base;  // Bases for RandR extension events/errors.
    if (!XRRQueryExtension(dpy, &randr_event_base, &randr_error_base)) {  // Check if RandR is available.
        fprintf(stderr, "RandR extension missing\n");  // Error if missing.
    }
    XRRSelectInput(dpy, root, RRScreenChangeNotifyMask);  // Select screen change events.

    RenderContext render_ctx = {  // Initialize rendering context structure.
        .dpy = dpy,
        .visual = visual,
        .fmt = fmt,
        .font = font,
        .label_color = white,
        .cmap = cmap,
        .bg_pixmap = None,
        .active_canvas = NULL
    };  // Struct holds display, visual, etc., for rendering.

    // Check and create user config dir, copy assets if needed.
    char *home = getenv("HOME");  // Get HOME env.
    char home_config[1024];  // Buffer for config path.
    snprintf(home_config, sizeof(home_config), "%s/%s", home, RESOURCE_DIR_USER);  // Build path.
    struct stat st;  // Stat structure to check dir.
    if (stat(home_config, &st) != 0) {  // If dir doesn't exist.
        mkdir(home_config, 0755);  // Create dir with permissions.
        char cmd[1024];  // Buffer for shell command.
        snprintf(cmd, sizeof(cmd), "cp -r %s/* %s", RESOURCE_DIR_SYSTEM, home_config);  // Build copy command.
        system(cmd);  // Execute shell command to copy assets.
    }
    // Create empty prefs file if not exist (amiwbrc).
    char prefs_path[1024];
    snprintf(prefs_path, sizeof(prefs_path), "%s/amiwbrc", home_config);
    if (access(prefs_path, F_OK) != 0) {
        FILE *fp = fopen(prefs_path, "w");  // Create empty file.
        if (fp) fclose(fp);
    }
    // Set dynamic icon paths.
    def_tool_path = get_resource_path("icons/def_tool.info");
    def_drawer_path = get_resource_path("icons/def_drawer.info");
    iconify_path = get_resource_path("icons/filer.info");

    MenuBar menubar;  // Menubar structure.
    create_menubar(&render_ctx, root, &menubar);  // Create menubar window and init.

    Canvas desktop = {0};  // Initialize desktop canvas struct (zeroed).
    desktop.x = 0;  // Set position.
    desktop.y = MENUBAR_HEIGHT;
    desktop.width = DisplayWidth(dpy, screen);  // Get screen width.
    desktop.height = DisplayHeight(dpy, screen) - MENUBAR_HEIGHT;  // Screen height minus menubar.
    desktop.bg_color = BG_COLOR_DESKTOP;  // Set background color from config.
    desktop.active = false;  // Not active initially.
    desktop.titlebar_height = 0;  // No titlebar for desktop.
    desktop.path = NULL;  // No path.
    desktop.client_win = 0;  // No client window.
    desktop.client_visual = NULL;
    desktop.title = NULL;
    XSetWindowAttributes attrs = {0};  // Window attributes struct.
    attrs.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | StructureNotifyMask | KeyPressMask;  // Select events for exposure, mouse, keys.
    desktop.win = create_canvas_window(&render_ctx, root, desktop.x, desktop.y, desktop.width, desktop.height, &attrs);  // Create desktop window.
    XMapWindow(dpy, desktop.win);  // Map (show) the window.

    desktop.backing = XCreatePixmap(dpy, desktop.win, desktop.width, desktop.height, 32);  // Create backing pixmap for double buffering.
    desktop.back_pic = XRenderCreatePicture(dpy, desktop.backing, fmt, 0, NULL);  // Create render picture from pixmap.
    desktop.win_pic = XRenderCreatePicture(dpy, desktop.win, fmt, 0, NULL);  // Picture for window.
    desktop.icons = NULL;  // No icons yet.
    desktop.num_icons = 0;
    char *home_path = malloc(strlen(home) + 2);  // Alloc for home path with /.
    strcpy(home_path, home);
    strcat(home_path, "/");
    char *harddisk_path = get_resource_path("icons/harddisk.info");  // Get dynamic path.
    add_icon(&render_ctx, "", "harddisk", TYPE_DRAWER, &desktop.icons, &desktop.num_icons, harddisk_path, &desktop);  // Add home icon as "harddisk".
    free(harddisk_path);  // Free.
    free(desktop.icons[0].path);  // Free old path.
    desktop.icons[0].path = home_path;  // Set to user's home.
    align_icons(&desktop);  // Align icons in grid.
    // don't do the initial draw, rely on Expose events to do it for us.
    // it's too early to redraw now.
    // redraw_canvas(&render_ctx, &desktop, NULL);  // Redraw desktop.
    printf("Initial desktop size: %d x %d\n", desktop.width, desktop.height);  // Debug print.
    Canvas windows[MAX_WINDOWS];  // Array for managed windows.
    int num_windows = 0;  // Count of windows.

    // Manage existing windows on startup (for reparenting existing apps).
    Window dummy_root, dummy_parent;
    Window *children;
    unsigned int nchildren;
    if (XQueryTree(dpy, root, &dummy_root, &dummy_parent, &children, &nchildren)) {  // Query child windows of root.
        for (unsigned int i = 0; i < nchildren; i++) {  // Loop through children.
            XWindowAttributes wa;  // Window attributes.
            XGetWindowAttributes(dpy, children[i], &wa);  // Get attrs.
            if (wa.override_redirect || wa.map_state != IsViewable) continue;  // Skip override-redirect or unmapped.
            int slot = find_free_slot(windows, num_windows, MAX_WINDOWS);  // Find free slot in array.
            if (slot == -1) continue;  // No slot.
            Canvas *new_canvas = &windows[slot];  // New canvas.
            if (slot == num_windows) num_windows++;  // Increment count.
            new_canvas->path = NULL;
            new_canvas->icons = NULL;
            new_canvas->num_icons = 0;
            new_canvas->bg_color = BG_COLOR_FOLDER;
            new_canvas->active = true;
            new_canvas->titlebar_height = TITLEBAR_HEIGHT;
            new_canvas->client_win = children[i];  // Set client window.
            new_canvas->x = wa.x;
            new_canvas->y = wa.y + MENUBAR_HEIGHT;
            new_canvas->width = wa.width + BORDER_WIDTH * 2;
            new_canvas->height = wa.height + TITLEBAR_HEIGHT + BORDER_WIDTH;
            XSetWindowAttributes attrs = {0};
            attrs.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | StructureNotifyMask | KeyPressMask;
            new_canvas->win = create_canvas_window(&render_ctx, root, new_canvas->x, new_canvas->y, new_canvas->width, new_canvas->height, &attrs);  // Create frame window.
            new_canvas->backing = XCreatePixmap(dpy, new_canvas->win, new_canvas->width, new_canvas->height, 32);
            new_canvas->back_pic = XRenderCreatePicture(dpy, new_canvas->backing, fmt, 0, NULL);
            new_canvas->win_pic = XRenderCreatePicture(dpy, new_canvas->win, fmt, 0, NULL);
            new_canvas->client_pic = XRenderCreatePicture(dpy, new_canvas->client_win, XRenderFindVisualFormat(dpy, wa.visual), 0, NULL);  // Picture for client.
            new_canvas->client_visual = wa.visual;
            XSelectInput(dpy, children[i], StructureNotifyMask | PropertyChangeMask);  // Select events on client.
            XAddToSaveSet(dpy, children[i]);  // Add to save set to reparent on WM restart.
            XReparentWindow(dpy, children[i], new_canvas->win, BORDER_WIDTH, TITLEBAR_HEIGHT);  // Reparent client into frame.
            
            Atom net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);  // EWMH atom for window title.

            Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
            unsigned char *prop_data = NULL;
            int format;
            unsigned long type, bytes_after, nitems;
            if (XGetWindowProperty(dpy, children[i], net_wm_name, 0, 1024, False, utf8, &type, &format, &nitems, &bytes_after, &prop_data) == Success && prop_data) {  // Get _NET_WM_NAME property.
                new_canvas->title = strdup((char *)prop_data);  // Set title.
                XFree(prop_data);
            } else {
                XTextProperty tp;
                if (XGetWMName(dpy, children[i], &tp) && tp.value) {  // Fallback to WM_NAME.
                    new_canvas->title = strdup((char *)tp.value);
                    XFree(tp.value);
                } else {
                    new_canvas->title = strdup("Window");  // Default title.
                }
            }
            XMapWindow(dpy, new_canvas->win);  // Map frame.

            // Set WM_STATE for existing windows to NormalState.
            Atom wm_state = XInternAtom(dpy, "WM_STATE", False);
            long data[2] = {1, None}; // 1 = NormalState
            XChangeProperty(dpy, children[i], wm_state, wm_state, 32, PropModeReplace, (unsigned char*)data, 2);

            redraw_canvas(&render_ctx, new_canvas, NULL);  // Redraw.
            activate_canvas(&render_ctx, new_canvas, windows, num_windows);  // Activate.
        }
        XFree(children);  // Free query list.
    }

    char *wallpaper_path = get_resource_path("patterns/stonepat.jpg");  // Get dynamic wallpaper path.
    set_wallpaper(&render_ctx, wallpaper_path);  // Set initial wallpaper.
    free(wallpaper_path);  // Free allocated path.

    handle_events(&render_ctx, &desktop, windows, &num_windows, root, &menubar, randr_event_base);  // Enter main event loop.

    // Cleanup resources.
    XftFontClose(dpy, font);  // Close font.
    XftColorFree(dpy, visual, cmap, &white);  // Free color.
    XFreeColormap(dpy, cmap);  // Free colormap.
    free(def_tool_path);
    free(def_drawer_path);
    free(iconify_path);
    // Close desktop.
    close_canvas(&render_ctx, &desktop, NULL, NULL);
    // Close all windows.
    for (int i = 0; i < num_windows; i++) {
        if (windows[i].win != None) {
            close_canvas(&render_ctx, &windows[i], NULL, NULL);
        }
    }
    // Close menubar.
    if (menubar.win != None) {
        XRenderFreePicture(dpy, menubar.back_pic);
        XRenderFreePicture(dpy, menubar.win_pic);
        XFreePixmap(dpy, menubar.backing);
        XDestroyWindow(dpy, menubar.win);
    }
    if (render_ctx.bg_pixmap != None) {
        XFreePixmap(dpy, render_ctx.bg_pixmap);
    }
    XCloseDisplay(dpy);  // Close display connection.

    return 0;
}