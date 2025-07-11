#include <stdio.h>
#include <stdlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include "intuition.h"
#include "icons.h"
#include "events.h"
#include "menus.h"

#define MENUBAR_HEIGHT 20
#define MAX_FRAMES 100

// Global variables
Display *g_dpy;
Window g_root;
Window active_frame = 0;
XContext frame_context, client_context, resize_context, close_context, iconify_context, lower_context, iconified_frame_context;
FrameInfo frames[MAX_FRAMES];
int num_frames = 0;

// Error handler
static int error_handler(Display *d, XErrorEvent *e) {
    if (e->error_code == BadWindow || e->error_code == BadMatch || e->error_code == BadValue) {
        return 0;  // Suppress BadWindow, BadMatch, and BadValue errors
    }
    char error_text[256];
    XGetErrorText(d, e->error_code, error_text, sizeof(error_text));
    printf("[intuition] ERROR: X11 error: %s (code=%d, request=%d, resource=0x%lx)\n",
           error_text, e->error_code, e->request_code, e->resourceid);
    return 0;
}

// Initialize window manager
void intuition_init(Display *dpy, Window root) {
    g_dpy = dpy;
    g_root = root;
    XSetErrorHandler(error_handler);
    XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask | ButtonPressMask);
    XSync(dpy, False);
    frame_context = XUniqueContext();
    client_context = XUniqueContext();
    resize_context = XUniqueContext();
    close_context = XUniqueContext();
    iconify_context = XUniqueContext();
    lower_context = XUniqueContext();
    iconified_frame_context = XUniqueContext();
}

// Handle map request
void intuition_handle_map_request(Display *dpy, XMapRequestEvent *e) {
    XWindowAttributes attr;
    if (!XGetWindowAttributes(dpy, e->window, &attr)) return;

    int border = 2, button_size = 15, title_height = 20, bottom_height = 20;
    int frame_x = attr.x, frame_y = attr.y < MENUBAR_HEIGHT ? MENUBAR_HEIGHT : attr.y;
    int frame_w = attr.width + border * 2, frame_h = attr.height + title_height + bottom_height;

    Window frame = XCreateSimpleWindow(dpy, g_root, frame_x, frame_y, frame_w, frame_h, border,
                                       BlackPixel(dpy, DefaultScreen(dpy)), 0xFF9F9F9F);
    Window resize_button = XCreateSimpleWindow(dpy, frame, frame_w - button_size - border, frame_h - bottom_height - border,
                                              button_size, button_size, 0, BlackPixel(dpy, DefaultScreen(dpy)), 0xFF00FF00);
    Window close_button = XCreateSimpleWindow(dpy, frame, border, 2, button_size, button_size, 0,
                                             BlackPixel(dpy, DefaultScreen(dpy)), 0xFFFF0000);
    Window iconify_button = XCreateSimpleWindow(dpy, frame, frame_w - button_size - border - button_size - 2, 2,
                                                button_size, button_size, 0, BlackPixel(dpy, DefaultScreen(dpy)), 0xFFFFFF00);
    Window lower_button = XCreateSimpleWindow(dpy, frame, frame_w - button_size - border, 2,
                                             button_size, button_size, 0, BlackPixel(dpy, DefaultScreen(dpy)), 0xFF9F9F9F);

    if (num_frames < MAX_FRAMES) {
        FrameInfo *f = &frames[num_frames++];
        f->frame = (WindowInfo){frame, frame_x, frame_y, frame_w, frame_h};
        f->client = (WindowInfo){e->window, border, title_height, attr.width, attr.height};
        f->resize_button = (WindowInfo){resize_button, frame_w - button_size - border, frame_h - bottom_height - border, button_size, button_size};
        f->close_button = (WindowInfo){close_button, border, 2, button_size, button_size};
        f->iconify_button = (WindowInfo){iconify_button, frame_w - button_size - border - button_size - 2, 2, button_size, button_size};
        f->lower_button = (WindowInfo){lower_button, frame_w - button_size - border, 2, button_size, button_size};

        XSelectInput(dpy, resize_button, ButtonPressMask | ExposureMask);
        XSelectInput(dpy, close_button, ButtonPressMask | ExposureMask);
        XSelectInput(dpy, iconify_button, ButtonPressMask | ExposureMask);
        XSelectInput(dpy, lower_button, ButtonPressMask | ExposureMask);
        XSelectInput(dpy, frame, ButtonPressMask | ButtonReleaseMask | PointerMotionMask);
        XSelectInput(dpy, e->window, StructureNotifyMask);

        XSaveContext(dpy, resize_button, resize_context, (XPointer)frame);
        XSaveContext(dpy, close_button, close_context, (XPointer)frame);
        XSaveContext(dpy, iconify_button, iconify_context, (XPointer)frame);
        XSaveContext(dpy, lower_button, lower_context, (XPointer)frame);
        XSaveContext(dpy, e->window, frame_context, (XPointer)frame);
        XSaveContext(dpy, frame, client_context, (XPointer)e->window);

        XAddToSaveSet(dpy, e->window);
        XReparentWindow(dpy, e->window, frame, border, title_height);
        XMapWindow(dpy, frame);
        XMapWindow(dpy, e->window);
        XMapWindow(dpy, resize_button);
        XMapWindow(dpy, close_button);
        XMapWindow(dpy, iconify_button);
        XMapWindow(dpy, lower_button);
        restack_windows(dpy);  // Ensure new frame above icons
        set_active_frame(dpy, frame);
    } else {
        XDestroyWindow(dpy, frame);
        XDestroyWindow(dpy, resize_button);
        XDestroyWindow(dpy, close_button);
        XDestroyWindow(dpy, iconify_button);
        XDestroyWindow(dpy, lower_button);
    }
}

// Handle configure request
void intuition_handle_configure_request(Display *dpy, XConfigureRequestEvent *e) {
    XWindowAttributes attr;
    if (!XGetWindowAttributes(dpy, e->window, &attr)) return;
    XWindowChanges changes = {
        .x = e->x,
        .y = e->y < MENUBAR_HEIGHT ? MENUBAR_HEIGHT : e->y,
        .width = e->width < 50 ? 50 : e->width,
        .height = e->height < 50 ? 50 : e->height,
        .border_width = e->border_width,
        .sibling = e->above,
        .stack_mode = e->detail
    };
    XConfigureWindow(dpy, e->window, e->value_mask, &changes);
    restack_windows(dpy);  // Ensure order after configuration
}

// Scan existing windows
void scan_existing_windows(Display *dpy) {
    Window root_ret, parent_ret, *children;
    unsigned int n;
    if (XQueryTree(dpy, g_root, &root_ret, &parent_ret, &children, &n)) {
        for (unsigned int i = 0; i < n; i++) {
            XWindowAttributes attr;
            if (XGetWindowAttributes(dpy, children[i], &attr) && !attr.override_redirect && attr.map_state == IsViewable) {
                XEvent fake = { .xmaprequest = { .type = MapRequest, .display = dpy, .parent = g_root, .window = children[i] } };
                intuition_handle_map_request(dpy, &fake.xmaprequest);
            }
        }
        XFree(children);
    }
}

// Set active frame
void set_active_frame(Display *dpy, Window frame) {
    if (frame == 0) return;
    if (active_frame != 0 && active_frame != frame) {
        XSetWindowBackground(dpy, active_frame, 0xFF9F9F9F);
        XClearWindow(dpy, active_frame);
    }
    active_frame = frame;
    XSetWindowBackground(dpy, frame, 0xFF4870AE);
    XClearWindow(dpy, frame);
    // Restack: menubar/menu, active frame, other frames, icons
    Window *stack = malloc((num_desktop_icons + num_frames + 2) * sizeof(Window));
    int stack_count = 0;
    if (get_menubar_window()) stack[stack_count++] = get_menubar_window();
    if (get_menu_window()) stack[stack_count++] = get_menu_window();
    stack[stack_count++] = frame;  // Active frame at top of managed windows
    for (int i = 0; i < num_frames; i++) {
        if (frames[i].frame.window && frames[i].frame.window != frame) {
            stack[stack_count++] = frames[i].frame.window;
        }
    }
    for (int i = 0; i < num_desktop_icons; i++) {
        if (desktop_icons[i].window) stack[stack_count++] = desktop_icons[i].window;
    }
    printf("[intuition] Active frame 0x%lx, stack: count=%d\n", frame, stack_count);
    XRestackWindows(dpy, stack, stack_count);
    free(stack);
    XSync(dpy, False);
}

// Get top managed window
Window get_top_managed_window(Display *dpy) {
    Window root_ret, parent_ret, *children;
    unsigned int n;
    if (XQueryTree(dpy, g_root, &root_ret, &parent_ret, &children, &n)) {
        for (int i = n - 1; i >= 0; i--) {
            XPointer client_data;
            if (XFindContext(dpy, children[i], client_context, &client_data) == 0 && children[i] != active_frame) {
                Window frame = children[i];
                XFree(children);
                return frame;
            }
        }
        XFree(children);
    }
    return 0;
}
