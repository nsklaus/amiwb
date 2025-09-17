/*
 * Hobbler - Browser for AmiWB using WebKit2GTK
 * Based on GuruBrowser approach - WebKit for rendering, X11 for custom UI
 */

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xrender.h>
#include <X11/Xft/Xft.h>
#include <fontconfig/fontconfig.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <time.h>

// Toolkit includes
#include "../toolkit/toolkit.h"
#include "../toolkit/button.h"
#include "../toolkit/inputfield.h"

// Buffer sizes
#define PATH_SIZE 512
#define NAME_SIZE 128

// Window dimensions
#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 600
#define TOOLBAR_HEIGHT 50

// Button dimensions
#define NAV_BUTTON_WIDTH 60
#define NAV_BUTTON_HEIGHT 30
#define HOME_BUTTON_WIDTH 50
#define STOP_RELOAD_WIDTH 70
#define GO_BUTTON_WIDTH 40
#define BUTTON_PADDING 5

// Colors
#define COLOR_TOOLBAR 0xBBBBBB

typedef struct {
    // GTK/WebKit components
    GtkWidget *window;
    GtkWidget *vbox;
    WebKitWebView *webview;

    // X11 components for custom toolbar
    Display *x_display;
    Window toolbar_window;
    Pixmap toolbar_pixmap;
    XftFont *font;

    // Toolkit widgets
    Button *back_btn;
    Button *forward_btn;
    Button *stop_reload_btn;
    Button *home_btn;
    Button *go_btn;
    InputField *url_field;

    // State
    bool is_loading;
    char home_url[PATH_SIZE];
} HobblerApp;

// Global app instance
static HobblerApp *app = NULL;

// Forward declarations
void log_error(const char *format, ...);
static void create_toolbar_widgets(void);
static void redraw_toolbar(void);
static gboolean handle_x11_events(gpointer data);
static void on_back_clicked(void *data);
static void on_forward_clicked(void *data);
static void on_stop_reload_clicked(void *data);
static void on_home_clicked(void *data);
static void on_go_clicked(void *data);
static void on_url_enter(const char *text, void *data);

// Logging function - follows standard AmiWB pattern
void log_error(const char *format, ...) {
    const char *log_path = "/home/klaus/Sources/amiwb/hobbler.log";
    FILE *log = fopen(log_path, "a");
    if (!log) return;

    // Add timestamp like other AmiWB apps
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    fprintf(log, "[%02d:%02d:%02d] ",
            tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);

    va_list args;
    va_start(args, format);
    vfprintf(log, format, args);
    va_end(args);
    fprintf(log, "\n");
    fclose(log);
}

// Initialize log file
static void init_log(void) {
    const char *log_path = "/home/klaus/Sources/amiwb/hobbler.log";
    FILE *log = fopen(log_path, "w");
    if (!log) return;

    time_t now = time(NULL);
    fprintf(log, "Hobbler log file, started on: %s", ctime(&now));
    fprintf(log, "----------------------------------------\n");
    fclose(log);

    // DO NOT redirect stderr - follow standard logging pattern
    // Each error logged via log_error() using direct file I/O
}

// Button callbacks
static void on_back_clicked(void *data) {
    if (app && app->webview) {
        webkit_web_view_go_back(app->webview);
    }
}

static void on_forward_clicked(void *data) {
    if (app && app->webview) {
        webkit_web_view_go_forward(app->webview);
    }
}

static void on_stop_reload_clicked(void *data) {
    if (app && app->webview) {
        if (app->is_loading) {
            webkit_web_view_stop_loading(app->webview);
        } else {
            webkit_web_view_reload(app->webview);
        }
    }
}

static void on_home_clicked(void *data) {
    if (app && app->webview) {
        webkit_web_view_load_uri(app->webview, app->home_url);
        inputfield_set_text(app->url_field, app->home_url);
    }
}

static void on_go_clicked(void *data) {
    if (app && app->url_field && app->webview) {
        const char *url = inputfield_get_text(app->url_field);
        if (url && url[0]) {
            // Add https:// if no protocol specified
            if (!strstr(url, "://")) {
                char full_url[PATH_SIZE];
                snprintf(full_url, sizeof(full_url), "https://%s", url);
                webkit_web_view_load_uri(app->webview, full_url);
            } else {
                webkit_web_view_load_uri(app->webview, url);
            }
        }
    }
}

static void on_url_enter(const char *text, void *data) {
    on_go_clicked(data);
}

// WebKit callbacks
static void on_load_changed(WebKitWebView *webview, WebKitLoadEvent event, gpointer data) {
    HobblerApp *app = (HobblerApp *)data;

    switch (event) {
        case WEBKIT_LOAD_STARTED:
        case WEBKIT_LOAD_REDIRECTED:
            app->is_loading = true;
            if (app->stop_reload_btn) {
                free(app->stop_reload_btn->label);
                app->stop_reload_btn->label = strdup("Stop");
            }
            break;

        case WEBKIT_LOAD_COMMITTED:
            // Update URL bar with current URL
            if (app->url_field) {
                const char *uri = webkit_web_view_get_uri(webview);
                if (uri) {
                    inputfield_set_text(app->url_field, uri);
                }
            }
            break;

        case WEBKIT_LOAD_FINISHED:
            app->is_loading = false;
            if (app->stop_reload_btn) {
                free(app->stop_reload_btn->label);
                app->stop_reload_btn->label = strdup("Reload");
            }
            break;
    }

    redraw_toolbar();
}

// Update toolbar layout based on window width
static void update_toolbar_layout(int window_width) {
    int x = BUTTON_PADDING * 3;  // Triple padding at start

    // Update button positions with consistent triple spacing
    app->back_btn->x = x;
    x += NAV_BUTTON_WIDTH + (BUTTON_PADDING * 3);

    app->forward_btn->x = x;
    x += NAV_BUTTON_WIDTH + (BUTTON_PADDING * 3);

    app->stop_reload_btn->x = x;
    x += STOP_RELOAD_WIDTH + (BUTTON_PADDING * 3);

    app->home_btn->x = x;
    x += HOME_BUTTON_WIDTH + (BUTTON_PADDING * 3);  // Triple spacing before input field

    // Calculate URL field width to fill remaining space
    // Account for triple padding at the end too
    int url_width = window_width - x - GO_BUTTON_WIDTH - (BUTTON_PADDING * 6);  // 3 after field + 3 at end
    if (url_width < 100) url_width = 100;  // Minimum width

    app->url_field->x = x;
    app->url_field->width = url_width;
    x += url_width + (BUTTON_PADDING * 3);

    // Go button at the end
    app->go_btn->x = x;
}

// Create toolbar widgets
static void create_toolbar_widgets(void) {
    int x = BUTTON_PADDING;
    int y = (TOOLBAR_HEIGHT - NAV_BUTTON_HEIGHT) / 2;

    // Create buttons
    app->back_btn = button_create(x, y, NAV_BUTTON_WIDTH, NAV_BUTTON_HEIGHT, "Back", app->font);
    button_set_callback(app->back_btn, on_back_clicked, app);

    app->forward_btn = button_create(x, y, NAV_BUTTON_WIDTH, NAV_BUTTON_HEIGHT, "Forward", app->font);
    button_set_callback(app->forward_btn, on_forward_clicked, app);

    app->stop_reload_btn = button_create(x, y, STOP_RELOAD_WIDTH, NAV_BUTTON_HEIGHT, "Reload", app->font);
    button_set_callback(app->stop_reload_btn, on_stop_reload_clicked, app);

    app->home_btn = button_create(x, y, HOME_BUTTON_WIDTH, NAV_BUTTON_HEIGHT, "Home", app->font);
    button_set_callback(app->home_btn, on_home_clicked, app);

    // URL field
    app->url_field = inputfield_create(x, y, 100, NAV_BUTTON_HEIGHT, app->font);  // Initial width
    inputfield_set_text(app->url_field, app->home_url);
    inputfield_set_callbacks(app->url_field, on_url_enter, NULL, app);

    // Go button
    app->go_btn = button_create(x, y, GO_BUTTON_WIDTH, NAV_BUTTON_HEIGHT, "Go", app->font);
    button_set_callback(app->go_btn, on_go_clicked, app);

    // Set initial layout
    update_toolbar_layout(WINDOW_WIDTH);
}

// Create X11 toolbar overlay
static void create_x11_toolbar(void) {
    if (!app || !app->x_display) return;

    // Get GTK window's X11 window ID
    GdkWindow *gdk_window = gtk_widget_get_window(app->window);
    Window parent_window = gdk_x11_window_get_xid(gdk_window);

    // Create X11 window for toolbar
    XSetWindowAttributes attrs;
    attrs.background_pixel = COLOR_TOOLBAR;
    attrs.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                       KeyPressMask | StructureNotifyMask | PointerMotionMask;

    app->toolbar_window = XCreateWindow(
        app->x_display,
        parent_window,
        0, 0, WINDOW_WIDTH, TOOLBAR_HEIGHT,
        0,
        CopyFromParent,
        InputOutput,
        CopyFromParent,
        CWBackPixel | CWEventMask,
        &attrs
    );

    // Create pixmap for double buffering
    app->toolbar_pixmap = XCreatePixmap(
        app->x_display,
        app->toolbar_window,
        WINDOW_WIDTH, TOOLBAR_HEIGHT,
        DefaultDepth(app->x_display, DefaultScreen(app->x_display))
    );

    // Map the toolbar window
    XMapWindow(app->x_display, app->toolbar_window);
    XFlush(app->x_display);
}

// Redraw toolbar
static void redraw_toolbar(void) {
    if (!app || !app->toolbar_window) return;

    Display *dpy = app->x_display;

    // Get current window dimensions
    Window root;
    int x, y;
    unsigned int width, height, border, depth;
    XGetGeometry(dpy, app->toolbar_window, &root, &x, &y, &width, &height, &border, &depth);

    // Update layout if width changed
    update_toolbar_layout(width);

    // Recreate pixmap at new size if needed
    if (app->toolbar_pixmap) {
        XFreePixmap(dpy, app->toolbar_pixmap);
    }
    app->toolbar_pixmap = XCreatePixmap(dpy, app->toolbar_window, width, TOOLBAR_HEIGHT,
                                        DefaultDepth(dpy, DefaultScreen(dpy)));

    // Clear background
    XSetForeground(dpy, DefaultGC(dpy, DefaultScreen(dpy)), COLOR_TOOLBAR);
    XFillRectangle(dpy, app->toolbar_pixmap, DefaultGC(dpy, DefaultScreen(dpy)),
                   0, 0, width, TOOLBAR_HEIGHT);

    // Create XftDraw for the pixmap
    XftDraw *xft_draw = XftDrawCreate(dpy, app->toolbar_pixmap,
                                      DefaultVisual(dpy, DefaultScreen(dpy)),
                                      DefaultColormap(dpy, DefaultScreen(dpy)));

    // Create Picture for rendering
    Picture pic = XRenderCreatePicture(dpy, app->toolbar_pixmap,
                                       XRenderFindVisualFormat(dpy, DefaultVisual(dpy, DefaultScreen(dpy))),
                                       0, NULL);

    // Draw buttons
    button_render(app->back_btn, pic, dpy, xft_draw);
    button_render(app->forward_btn, pic, dpy, xft_draw);
    button_render(app->stop_reload_btn, pic, dpy, xft_draw);
    button_render(app->home_btn, pic, dpy, xft_draw);
    button_render(app->go_btn, pic, dpy, xft_draw);

    // Draw input field
    inputfield_draw(app->url_field, pic, dpy, xft_draw, app->font);

    XRenderFreePicture(dpy, pic);

    // Copy pixmap to window
    XCopyArea(dpy, app->toolbar_pixmap, app->toolbar_window,
              DefaultGC(dpy, DefaultScreen(dpy)),
              0, 0, width, TOOLBAR_HEIGHT, 0, 0);

    XftDrawDestroy(xft_draw);
    XFlush(dpy);
}

// Handle X11 events for toolbar
static gboolean handle_x11_events(gpointer data) {
    if (!app || !app->x_display) return TRUE;

    XEvent event;
    while (XPending(app->x_display)) {
        XNextEvent(app->x_display, &event);

        if (event.xany.window == app->toolbar_window) {
            switch (event.type) {
                case Expose:
                    redraw_toolbar();
                    break;

                case ButtonPress: {
                    bool need_redraw = false;

                    // ALWAYS handle input field click to manage focus properly
                    if (inputfield_handle_click(app->url_field, event.xbutton.x, event.xbutton.y)) {
                        need_redraw = true;
                    }

                    // Check buttons
                    if (button_handle_press(app->back_btn, event.xbutton.x, event.xbutton.y) ||
                        button_handle_press(app->forward_btn, event.xbutton.x, event.xbutton.y) ||
                        button_handle_press(app->stop_reload_btn, event.xbutton.x, event.xbutton.y) ||
                        button_handle_press(app->home_btn, event.xbutton.x, event.xbutton.y) ||
                        button_handle_press(app->go_btn, event.xbutton.x, event.xbutton.y)) {
                        need_redraw = true;
                    }

                    if (need_redraw) redraw_toolbar();
                    break;
                }

                case ButtonRelease: {
                    bool need_redraw = false;

                    // Call release for all buttons to clear pressed state
                    need_redraw |= button_handle_release(app->back_btn, event.xbutton.x, event.xbutton.y);
                    need_redraw |= button_handle_release(app->forward_btn, event.xbutton.x, event.xbutton.y);
                    need_redraw |= button_handle_release(app->stop_reload_btn, event.xbutton.x, event.xbutton.y);
                    need_redraw |= button_handle_release(app->home_btn, event.xbutton.x, event.xbutton.y);
                    need_redraw |= button_handle_release(app->go_btn, event.xbutton.x, event.xbutton.y);

                    if (need_redraw) redraw_toolbar();
                    break;
                }

                case KeyPress:
                    if (inputfield_handle_key(app->url_field, &event.xkey)) {
                        redraw_toolbar();
                    }
                    break;
            }
        }
    }

    return TRUE;
}

// Window resize callback
static gboolean on_window_resize(GtkWidget *widget, GdkEventConfigure *event, gpointer data) {
    // Resize toolbar to match window width
    if (app && app->toolbar_window) {
        XResizeWindow(app->x_display, app->toolbar_window, event->width, TOOLBAR_HEIGHT);
        redraw_toolbar();
    }
    return FALSE;
}

// Window destroy callback
static void on_window_destroy(GtkWidget *widget, gpointer data) {
    if (app) {
        // Clean up toolkit widgets
        if (app->back_btn) button_destroy(app->back_btn);
        if (app->forward_btn) button_destroy(app->forward_btn);
        if (app->stop_reload_btn) button_destroy(app->stop_reload_btn);
        if (app->home_btn) button_destroy(app->home_btn);
        if (app->go_btn) button_destroy(app->go_btn);
        if (app->url_field) inputfield_destroy(app->url_field);

        // Clean up X11 resources
        if (app->toolbar_pixmap) {
            XFreePixmap(app->x_display, app->toolbar_pixmap);
        }
        if (app->toolbar_window) {
            XDestroyWindow(app->x_display, app->toolbar_window);
        }
        if (app->font) {
            XftFontClose(app->x_display, app->font);
        }

        free(app);
        app = NULL;
    }
    gtk_main_quit();
}

int main(int argc, char *argv[]) {
    // Initialize logging
    init_log();

    // Register toolkit logging callback so toolkit widgets log to hobbler.log
    toolkit_set_log_callback(log_error);

    // Initialize GTK
    gtk_init(&argc, &argv);

    // Allocate app structure
    app = calloc(1, sizeof(HobblerApp));
    if (!app) {
        log_error("[ERROR] Failed to allocate app structure");
        return 1;
    }

    // Set default home URL to local start page
    snprintf(app->home_url, PATH_SIZE, "file:///home/klaus/Documents/start.html");

    // Parse command line
    if (argc > 1) {
        snprintf(app->home_url, PATH_SIZE, "%s", argv[1]);
    }

    // Get X11 display
    app->x_display = XOpenDisplay(NULL);
    if (!app->x_display) {
        log_error("[ERROR] Cannot open X display");
        free(app);
        return 1;
    }

    // Load font - same as all AmiWB apps
    FcPattern *pattern = FcPatternCreate();
    if (pattern) {
        const char *font_path = "/usr/local/share/amiwb/fonts/SourceCodePro-Bold.otf";
        FcPatternAddString(pattern, FC_FILE, (const FcChar8 *)font_path);
        FcPatternAddDouble(pattern, FC_SIZE, 12.0);
        FcPatternAddDouble(pattern, FC_DPI, 75);
        FcConfigSubstitute(NULL, pattern, FcMatchPattern);
        XftDefaultSubstitute(app->x_display, DefaultScreen(app->x_display), pattern);
        app->font = XftFontOpenPattern(app->x_display, pattern);
    }

    if (!app->font) {
        log_error("[WARNING] Failed to load SourceCodePro-Bold.otf, falling back to monospace");
        app->font = XftFontOpen(app->x_display, DefaultScreen(app->x_display),
                               XFT_FAMILY, XftTypeString, "monospace",
                               XFT_SIZE, XftTypeDouble, 12.0,
                               NULL);
    }

    // Create GTK window
    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app->window), "Hobbler Browser");
    gtk_window_set_default_size(GTK_WINDOW(app->window), WINDOW_WIDTH, WINDOW_HEIGHT);
    g_signal_connect(app->window, "destroy", G_CALLBACK(on_window_destroy), NULL);
    g_signal_connect(app->window, "configure-event", G_CALLBACK(on_window_resize), NULL);

    // Create vertical box
    app->vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(app->window), app->vbox);

    // Add spacer for toolbar (WebKit will be below it)
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(spacer, -1, TOOLBAR_HEIGHT);
    gtk_box_pack_start(GTK_BOX(app->vbox), spacer, FALSE, FALSE, 0);

    // Create WebKit view
    app->webview = WEBKIT_WEB_VIEW(webkit_web_view_new());
    gtk_box_pack_start(GTK_BOX(app->vbox), GTK_WIDGET(app->webview), TRUE, TRUE, 0);

    // Connect WebKit signals
    g_signal_connect(app->webview, "load-changed", G_CALLBACK(on_load_changed), app);

    // Show all GTK widgets
    gtk_widget_show_all(app->window);

    // Tag window for AmiWB menu persistence
    GdkWindow *gdk_window = gtk_widget_get_window(app->window);
    Window xid = gdk_x11_window_get_xid(gdk_window);

    Atom app_type_atom = XInternAtom(app->x_display, "_AMIWB_APP_TYPE", False);
    XChangeProperty(app->x_display, xid, app_type_atom, XA_STRING, 8,
                   PropModeReplace, (unsigned char *)"HOBBLER", 7);

    // Register browser menus
    Atom menu_data = XInternAtom(app->x_display, "_AMIWB_MENU_DATA", False);
    const char *menus =
        "File:New Tab,New Window,Quit|"
        "Navigate:Back,Forward,Reload,Stop,Home|"
        "View:Zoom In,Zoom Out,Actual Size,Full Screen|"
        "Bookmarks:Add Bookmark,Manage Bookmarks|"
        "Tools:Developer Tools,View Source,Settings";
    XChangeProperty(app->x_display, xid, menu_data, XA_STRING, 8,
                   PropModeReplace, (unsigned char *)menus, strlen(menus));

    // Create toolbar widgets
    create_toolbar_widgets();

    // Create X11 toolbar overlay
    create_x11_toolbar();
    redraw_toolbar();

    // Set up X11 event handling
    g_timeout_add(10, handle_x11_events, NULL);

    // Load initial URL
    webkit_web_view_load_uri(app->webview, app->home_url);

    // Run GTK main loop
    gtk_main();

    return 0;
}