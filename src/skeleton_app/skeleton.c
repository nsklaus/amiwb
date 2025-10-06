/* skeleton.c - Main application implementation */

/*
 * What could be added later:
 * - More complex widget layouts
 * - Configuration loading
 * - Window state persistence
 * - Resource management
 * - Error recovery
 */

#include "../toolkit/button/button.h"      /* Include toolkit first */
#include "../toolkit/inputfield/inputfield.h"   /* This pulls in main config.h */
#include "skeleton.h"
#include "config.h"                  /* Our config with guards */
#include "logging.h"
#include "menus.h"
#include "font_manager.h"
#include <stdlib.h>
#include <string.h>

SkeletonApp *skeleton_create(Display *display) {
    SkeletonApp *app = calloc(1, sizeof(SkeletonApp));
    if (!app) {
        log_message("ERROR: Failed to allocate app structure");
        return NULL;
    }

    app->display = display;
    app->width = WINDOW_WIDTH;
    app->height = WINDOW_HEIGHT;

    /* Allocate gray color for background - use AmiWB standard gray */
    XColor gray_color;
    Colormap colormap = DefaultColormap(display, DefaultScreen(display));
    XRenderColor render_gray = GRAY;
    gray_color.red = render_gray.red;
    gray_color.green = render_gray.green;
    gray_color.blue = render_gray.blue;
    gray_color.flags = DoRed | DoGreen | DoBlue;
    XAllocColor(display, colormap, &gray_color);

    /* Create main window with gray background */
    Window root = DefaultRootWindow(display);
    app->main_window = XCreateSimpleWindow(display, root,
                                          100, 100,  /* x, y */
                                          app->width, app->height,
                                          0,  /* border width */
                                          BlackPixel(display, 0),  /* border */
                                          gray_color.pixel);        /* background */

    /* Set window title */
    XStoreName(display, app->main_window, APP_NAME);

    /* Register for window close events */
    Atom wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, app->main_window, &wm_delete, 1);

    /* Select input events */
    XSelectInput(display, app->main_window,
                ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask);

    /* Register menus with AmiWB */
    menus_init(display, app->main_window);

    /* Create example toolkit widgets */
    XftFont *font = font_get();
    app->example_button = button_create(20, 20, 100, 30, "Click Me", font);
    if (app->example_button) {
        /* Set a callback - we'll define button_callback below */
        button_set_callback(app->example_button, NULL, app);
        log_message("Button widget created");
    }

    app->example_input = inputfield_create(20, 60, 200, 25, font);
    if (app->example_input) {
        inputfield_set_text(app->example_input, "Type here...");
        log_message("Input field widget created");
    }

    /* Show the window first to get proper dimensions */
    XMapWindow(display, app->main_window);
    XFlush(display);
    XSync(display, False);

    /* Create rendering resources after window is mapped */
    app->pixmap = XCreatePixmap(display, app->main_window,
                               app->width, app->height,
                               DefaultDepth(display, DefaultScreen(display)));

    /* Create XRender Picture */
    XRenderPictFormat *fmt = XRenderFindStandardFormat(display, PictStandardRGB24);
    app->picture = XRenderCreatePicture(display, app->pixmap, fmt, 0, NULL);

    /* Create XftDraw for text rendering */
    Visual *visual = DefaultVisual(display, DefaultScreen(display));
    Colormap cmap = DefaultColormap(display, DefaultScreen(display));
    app->xft_draw = XftDrawCreate(display, app->pixmap, visual, cmap);

    log_message("Application window created");
    return app;
}

void skeleton_draw(SkeletonApp *app) {
    if (!app || !app->pixmap) return;

    /* Clear the pixmap with gray background */
    XRenderColor render_gray = GRAY;
    XRenderFillRectangle(app->display, PictOpSrc, app->picture,
                         &render_gray, 0, 0, app->width, app->height);

    /* Draw widgets */
    if (app->example_button) {
        button_render(app->example_button, app->picture, app->display, app->xft_draw);
    }

    if (app->example_input) {
        inputfield_render(app->example_input, app->picture, app->display,
                         app->xft_draw);
    }

    /* Copy pixmap to window */
    XCopyArea(app->display, app->pixmap, app->main_window,
             DefaultGC(app->display, DefaultScreen(app->display)),
             0, 0, app->width, app->height, 0, 0);

    XFlush(app->display);
}

void skeleton_destroy(SkeletonApp *app) {
    if (!app) return;

    /* Destroy toolkit widgets */
    if (app->example_button) {
        button_destroy(app->example_button);
    }
    if (app->example_input) {
        inputfield_destroy(app->example_input);
    }

    /* Destroy rendering resources */
    if (app->xft_draw) {
        XftDrawDestroy(app->xft_draw);
    }
    if (app->picture) {
        XRenderFreePicture(app->display, app->picture);
    }
    if (app->pixmap) {
        XFreePixmap(app->display, app->pixmap);
    }

    if (app->main_window) {
        XDestroyWindow(app->display, app->main_window);
    }

    free(app);
    log_message("Application destroyed");
}