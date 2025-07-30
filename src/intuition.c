// File: intuition.c
#include "intuition.h"
#include "render.h"
#include "menus.h"
#include "config.h"
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <X11/cursorfont.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define INITIAL_CANVAS_CAPACITY 8

// Global state for intuition
static Display *display = NULL;
static RenderContext *render_context = NULL;
static Canvas **canvas_array = NULL;
static int canvas_count = 0;
static int canvas_array_size = 0;
static Bool fullscreen_active = False;
static Canvas *active_window = NULL;
static Cursor root_cursor;
static int width = 0;
static int height = 0;
static Window screen = 0;
static Window root = 0;
static int depth = 0;
static Visual *visual = NULL;
static Canvas *dragging_canvas = NULL;
static int drag_start_x = 0, drag_start_y = 0;
static int window_start_x = 0, window_start_y = 0;

// resizing
static Canvas *resizing_canvas = NULL;
static int resize_start_x = 0, resize_start_y = 0;
static int window_start_width = 0, window_start_height = 0;

// scroling
static Canvas *scrolling_canvas = NULL;  // Canvas being scrolled via scrollbar
static bool scrolling_vertical = true;   // True for vertical scrollbar, false for horizontal
static int initial_scroll = 0;           // Initial scroll position at drag start
static int scroll_start_pos = 0;         // Initial mouse position (x or y root) at drag start

Display *get_display(void) {
    return display;
}

RenderContext *get_render_context(void) {
    return render_context;
}

Canvas *manage_canvases(bool add, Canvas *canvas_to_remove) {
    if (add) {
        if (canvas_count >= canvas_array_size) {
            canvas_array_size = canvas_array_size ? canvas_array_size * 2 : INITIAL_CANVAS_CAPACITY;
            Canvas **new_canvases = realloc(canvas_array, canvas_array_size * sizeof(Canvas *));
            if (!new_canvases) return NULL;
            canvas_array = new_canvases;
        }
        Canvas *new_canvas = malloc(sizeof(Canvas));
        if (!new_canvas) return NULL;
        canvas_array[canvas_count++] = new_canvas;
        return new_canvas;
    } else if (canvas_to_remove) {
        for (int i = 0; i < canvas_count; i++) {
            if (canvas_array[i] == canvas_to_remove) {
                Window win_id = canvas_array[i]->win;
                memmove(&canvas_array[i], &canvas_array[i + 1], (canvas_count - i - 1) * sizeof(Canvas *));
                canvas_count--;
                //fprintf(stderr, "manage_canvases: Removed canvas %lu at index %d\n", win_id, i);
                break;
            }
        }
    }
    return NULL;
}

XVisualInfo select_visual(CanvasType type) {
    XVisualInfo vinfo;
    if (type == DESKTOP || MENU) {
        vinfo.visual = DefaultVisual(display, screen);
        vinfo.depth = DefaultDepth(display, screen);
    } else {
        if (!XMatchVisualInfo(display, screen, GLOBAL_DEPTH, TrueColor, &vinfo)) {
            vinfo.visual = DefaultVisual(display, screen);
            vinfo.depth = DefaultDepth(display, screen);
        }
    }
    XMatchVisualInfo(display, screen, vinfo.depth, TrueColor, &vinfo);
    return vinfo;
}

Canvas *init_intuition(void) {
    display = XOpenDisplay(NULL);
    screen = DefaultScreen(display);
    width   = DisplayWidth(display, DefaultScreen(display)); 
    height  = DisplayHeight(display, DefaultScreen(display));
    root = RootWindow(display, DefaultScreen(display));
    depth = 32;

    root_cursor = XCreateFontCursor(display, XC_left_ptr);
    XDefineCursor(display, root, root_cursor);

    // select event masks on the root window
    XSelectInput(display, root, 
                SubstructureRedirectMask | 
                SubstructureNotifyMask | 
                PropertyChangeMask | 
                StructureNotifyMask | 
                ButtonPressMask | 
                ButtonReleaseMask | 
                PointerMotionMask | 
                KeyPressMask);

    // synchronize to ensure the selection is applied
    XSync(display, False);

    render_context = malloc(sizeof(RenderContext));
    render_context->dpy = display;

    // Select preferred visual for non-desktop canvases
    XVisualInfo vinfo;
    XMatchVisualInfo(display, screen, depth, TrueColor, &vinfo);
    render_context->fmt = XRenderFindVisualFormat(display, vinfo.visual);
    render_context->bg_pixmap = None;

    fprintf(stderr, "Creating desktop canvas\n");
    Canvas *desktop = create_canvas(getenv("HOME"), 0, 20, width, height, DESKTOP);




    // Scan and manage existing windows
    //XGrabServer(display);  // Prevent changes during reparenting
    Window root_return, parent_return;
    Window *children;
    unsigned int nchildren;
    if (XQueryTree(display, root, &root_return, &parent_return, &children, &nchildren)) {
        for (unsigned int i = 0; i < nchildren; i++) {
            Window child = children[i];

            // Skip if it's one of our canvases or clients
            bool is_own = false;
            for (int j = 0; j < canvas_count; j++) {
                if (canvas_array[j]->win == child || canvas_array[j]->client_win == child) {
                    is_own = true;
                    break;
                } else {canvas_array[j]->client_win = None;}
            }
            if (is_own) continue;

            // Get attributes and filter
            XWindowAttributes attrs;
            if (XGetWindowAttributes(display, child, &attrs)) {
                if (attrs.override_redirect || attrs.map_state != IsViewable) {
                    continue;  // Skip override-redirect or unmapped
                }

                // Create frame with preserved geometry
                int frame_x = attrs.x;
                int frame_y = attrs.y;
                int frame_width = attrs.width + BORDER_WIDTH_LEFT + BORDER_WIDTH_RIGHT;
                int frame_height = attrs.height + BORDER_HEIGHT_TOP + BORDER_HEIGHT_BOTTOM;
                Canvas *frame = create_canvas(NULL, frame_x, frame_y, frame_width, frame_height, WINDOW);
                if (!frame) {
                    fprintf(stderr, "Failed to create frame for existing client %lu\n", child);
                    continue;
                }

                // Reparent and configure
                XReparentWindow(display, child, frame->win, BORDER_WIDTH_LEFT, BORDER_HEIGHT_TOP);
                XSelectInput(display, child, StructureNotifyMask | PropertyChangeMask);  // Enable DestroyNotify and property events
                XResizeWindow(display, child, attrs.width, attrs.height);  // Retain client size
                frame->client_win = child;
                XAddToSaveSet(display, child);  // Ensure client persistence on WM exit
                XRaiseWindow(display, frame->win);
                redraw_canvas(frame);
                fprintf(stderr, "Reparented existing client %lu to frame %lu\n", child, frame->win);
            }
        }
        if (children) XFree(children);
    }
    //XUngrabServer(display);
    XSync(display, False);
    redraw_canvas(desktop);
    return desktop;
}

Canvas *get_desktop_canvas(void) {
    return canvas_count > 0 ? canvas_array[0] : NULL;
}

Canvas *find_canvas(Window win) {
    for (int i = 0; i < canvas_count; i++) {
        if (canvas_array[i]->win == win) 
            return canvas_array[i];
    }
    return NULL;
}

void set_active_window(Canvas *canvas) {
    if (canvas && canvas->type != WINDOW) return; // Only handle WINDOW type
    if (active_window == canvas) return;

    // Deactivate all other windows
    for (int i = 0; i < canvas_count; i++) {
        if (canvas_array[i]->type == WINDOW && canvas_array[i] != canvas) {
            canvas_array[i]->active = false;
            redraw_canvas(canvas_array[i]); // Redraw to show gray frame
            //fprintf(stderr, "Deactivated window %lu\n", canvas_array[i]->win);
        }
    }

    Canvas *prev_active = active_window;
    active_window = canvas;

    // Deselect all icons
    FileIcon **icon_array = get_icon_array();
    int icon_count = get_icon_count();
    for (int i = 0; i < icon_count; i++) {
        if (icon_array[i]->selected) {
            icon_array[i]->selected = false;
            icon_array[i]->current_picture = icon_array[i]->normal_picture;
        }
    }

    // Redraw previous active window
    if (prev_active) {
        redraw_canvas(prev_active);
    }

    // Activate and raise new window
    if (canvas) {
        canvas->active = true;
        XRaiseWindow(get_display(), canvas->win);
        redraw_canvas(canvas);
        XSync(get_display(), False);
        //fprintf(stderr, "set_active_window: Activated and raised window %lu\n", canvas->win);
    }
}

Canvas *get_active_window(void) {
    return active_window;
}

// compute max scroll and clamp current scroll
void compute_max_scroll(Canvas *canvas) {
    if (!canvas) return;
    int visible_w = canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT;
    int visible_h = canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM;
    canvas->max_scroll_x = max(0, canvas->content_width - visible_w);
    canvas->max_scroll_y = max(0, canvas->content_height - visible_h);
    //printf("max_scroll_x=%d, max_scroll_y=%d \n",canvas->max_scroll_x, canvas->max_scroll_y);
    canvas->scroll_x = min(canvas->scroll_x, canvas->max_scroll_x);
    canvas->scroll_y = min(canvas->scroll_y, canvas->max_scroll_y);
}

Canvas *create_canvas(const char *path, int x, int y, int width, int height, CanvasType type) {
    RenderContext *ctx = get_render_context();
    if (!ctx) {
        fprintf(stderr, "No render context\n");
        return NULL;
    }

    Canvas *canvas = manage_canvases(true, NULL);
    if (!canvas) {
        fprintf(stderr, "Failed to allocate canvas\n");
        return NULL;
    }
    *canvas = (Canvas){0};

    canvas->type = type;
    canvas->path = path ? strdup(path) : NULL;
    canvas->title = path ? strdup(strrchr(path, '/') ? strrchr(path, '/') + 1 : path) : NULL;
    canvas->x = x;
    canvas->y = y;
    canvas->width = width;
    canvas->height = height;
    canvas->bg_color = GRAY; //(XRenderColor){0xCCCC, 0xCCCC, 0xCCCC, 0xFFFF};
    canvas->active = false; // Initialize as inactive

    // Select visual
    XVisualInfo vinfo = select_visual(type);
    canvas->visual = vinfo.visual;
    canvas->depth = vinfo.depth;

    XSetWindowAttributes attrs = {0};
    attrs.colormap = XCreateColormap(ctx->dpy, root, canvas->visual, AllocNone);
    attrs.border_pixel = 0;
    unsigned long valuemask = CWColormap | CWBorderPixel;
    canvas->win = type == DESKTOP ? DefaultRootWindow(ctx->dpy) :
                  XCreateWindow(display, root, x, y, width, height, 1, vinfo.depth, InputOutput, canvas->visual, valuemask, &attrs);

    if (!canvas->win) {
        fprintf(stderr, "Failed to create window for canvas type %d\n", type);
        destroy_canvas(canvas);
        return NULL;
    }
    //canvas->colormap = (type == DESKTOP) ? DefaultColormap(ctx->dpy, DefaultScreen(ctx->dpy)) : attrs.colormap;
    canvas->colormap = attrs.colormap;

    long event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | KeyPressMask;
    if (type == DESKTOP) event_mask |= SubstructureRedirectMask | SubstructureNotifyMask;
    if (type == WINDOW)  event_mask |= StructureNotifyMask;

    XSelectInput(ctx->dpy, canvas->win, event_mask);
    //fprintf(stderr, "Set event mask 0x%lx for canvas type %d, window %lu\n", event_mask, type, canvas->win);

    canvas->canvas_buffer = XCreatePixmap(ctx->dpy, canvas->win, width, height, vinfo.depth);
    if (!canvas->canvas_buffer) {
        fprintf(stderr, "Failed to create pixmap for canvas type %d\n", type);
        destroy_canvas(canvas);
        return NULL;
    }

    XRenderPictFormat *fmt = XRenderFindVisualFormat(ctx->dpy, canvas->visual);
    if (!fmt) {
        fprintf(stderr, "Failed to find XRender format for canvas type %d\n", type);
        destroy_canvas(canvas);
        return NULL;
    }

    canvas->canvas_render = XRenderCreatePicture(ctx->dpy, canvas->canvas_buffer, fmt, 0, NULL);
    if (!canvas->canvas_render) {
        fprintf(stderr, "Failed to create canvas_render for canvas type %d\n", type);
        destroy_canvas(canvas);
        return NULL;
    }
    canvas->window_render = XRenderCreatePicture(ctx->dpy, canvas->win, fmt, 0, NULL);
    if (!canvas->window_render) {
        fprintf(stderr, "Failed to create window_render for canvas type %d\n", type);
        destroy_canvas(canvas);
        return NULL;
    }
    // initialization for scroll fields
    if (type == WINDOW) {
        canvas->scroll_x = 0;
        canvas->scroll_y = 0;
        canvas->content_width = canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT;
        canvas->content_height = canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM;
        compute_max_scroll(canvas);  // Initialize max scroll
    }
    if (type != DESKTOP) {
        // Set background to None for WINDOW to avoid obscuring client
        if (type == WINDOW) {
            attrs.background_pixmap = None;
            valuemask |= CWBackPixmap;
            XChangeWindowAttributes(ctx->dpy, canvas->win, valuemask, &attrs);
            canvas->active = true; // Set active for new WINDOW
        }
        XMapRaised(ctx->dpy, canvas->win); // Raise the window
        if (type == WINDOW) {
            set_active_window(canvas); // Activate and deactivate others
            redraw_canvas(canvas);
        }
        
        XSync(ctx->dpy, False);
        //fprintf(stderr, "Mapped and raised window %lu for canvas type %d, active=%d\n", canvas->win, type, canvas->active);
    }

    return canvas;
}

void intuition_handle_expose(XExposeEvent *event) {
    Canvas *canvas = find_canvas(event->window);
    if (canvas && !fullscreen_active) {
        redraw_canvas(canvas);
    }
}

void intuition_handle_property_notify(XPropertyEvent *event) {
    Display *dpy = get_display();
    if (!dpy || event->atom != XInternAtom(dpy, "WM_STATE", False)) return;

    Canvas *canvas = find_canvas(event->window);
    if (canvas && canvas->type == WINDOW) {
        Atom wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
        Atom fullscreen = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
        Atom type;
        int format;
        unsigned long nitems, bytes_after;
        unsigned char *prop;
        if (XGetWindowProperty(dpy, event->window, wm_state, 0, 1024, False, AnyPropertyType,
                               &type, &format, &nitems, &bytes_after, &prop) == Success && prop) {
            fullscreen_active = False;
            for (unsigned long i = 0; i < nitems; i++) {
                if (((Atom *)prop)[i] == fullscreen) {
                    fullscreen_active = True;
                    break;
                }
            }
            XFree(prop);
        }
        Canvas *menubar_canvas = get_menubar();
        if (menubar_canvas) {
            if (fullscreen_active) {
                XUnmapWindow(dpy, menubar_canvas->win);
            } else {
                XMapWindow(dpy, menubar_canvas->win);
                redraw_canvas(menubar_canvas);
            }
        }
    }

    Canvas *menubar_canvas = get_menubar();
    if (menubar_canvas) {
        menubar_canvas->width = DisplayWidth(dpy, DefaultScreen(dpy));
        XResizeWindow(dpy, menubar_canvas->win, menubar_canvas->width, MENU_ITEM_HEIGHT);
        redraw_canvas(menubar_canvas);
    }
}

// intuition_handle_button_press
void intuition_handle_button_press(XButtonEvent *event) {
    Canvas *canvas = find_canvas(event->window);
    if (!canvas) return;

    Display *dpy = get_display();
    if (!dpy) return;

    if (canvas->type == DESKTOP) {
        if (event->button == Button3) {  // Toggle menubar on RMB click
            toggle_menubar_state();
        }
        redraw_canvas(canvas);
        return;
    }

    if (canvas->type != WINDOW) return;

    set_active_window(canvas);

    // Close button: top-left corner (0, 0, 25, 20)
    if (event->y < BORDER_HEIGHT_TOP && event->x < BUTTON_CLOSE_SIZE) {
        destroy_canvas(canvas);
        return;
    } 

    // Titlebar drag: avoid close button (y < 20, x >= 20)
    else if (event->y < BORDER_HEIGHT_TOP && event->x >= BUTTON_CLOSE_SIZE) {
        dragging_canvas = canvas;
        drag_start_x = event->x_root;
        drag_start_y = event->y_root;
        window_start_x = canvas->x;
        window_start_y = canvas->y;
        return;
    }
    // Resize button: bottom-right corner
    else if (event->x >= canvas->width - BORDER_WIDTH_RIGHT && event->y >= canvas->height - BORDER_HEIGHT_BOTTOM) {
        resizing_canvas = canvas;
        resize_start_x = event->x_root;
        resize_start_y = event->y_root;
        window_start_width = canvas->width;
        window_start_height = canvas->height;
        //fprintf(stderr, "Resize start: window %lu at root (%d, %d)\n", canvas->win, event->x_root, event->y_root);
        return;
    }

    // don't manage scrollbars on clients
    if (canvas->type == WINDOW && canvas->client_win == None) {
        // Vertical scrollbar 
        int sb_x = canvas->width - BORDER_WIDTH_RIGHT;
        int sb_y = BORDER_HEIGHT_TOP + 10;
        int sb_w = BORDER_WIDTH_RIGHT;
        int sb_h = (canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM) - 54 - 10;
        if (event->x >= sb_x && event->x < sb_x + sb_w && event->y >= sb_y && event->y < sb_y + sb_h) {
            // Calculate knob position and size
            float ratio = (float)sb_h / canvas->content_height;
            int knob_h = max(MIN_KNOB_SIZE, (int)(ratio * sb_h));
            float pos_ratio = (float)canvas->scroll_y / canvas->max_scroll_y;
            int knob_y = sb_y + (int)(pos_ratio * (sb_h - knob_h));

            // Check if click is on knob (start drag)
            if (event->y >= knob_y && event->y < knob_y + knob_h) {
                scrolling_canvas = canvas;
                scrolling_vertical = true;
                initial_scroll = canvas->scroll_y;
                scroll_start_pos = event->y_root;
                return;  // Prevent other actions
            } else {
                // Click on track: jump knob toward click position
                float click_ratio = (float)(event->y - sb_y) / sb_h;
                int new_scroll = (int)(click_ratio * canvas->max_scroll_y);
                canvas->scroll_y = max(0, min(new_scroll, canvas->max_scroll_y));
                redraw_canvas(canvas);
                return;
            }
        }
        
        // handle vertical arrows clicks
        int arrow_size = 20;
        if (event->x >= sb_x && event->x < sb_x + sb_w) {
            if (event->y >= (canvas->height - BORDER_HEIGHT_BOTTOM - (2 * arrow_size)) && event->y < (canvas->height - BORDER_HEIGHT_BOTTOM - arrow_size)) {
                // Up arrow clicked: scroll up by fixed amount
                canvas->scroll_y = max(0, canvas->scroll_y - 20);
                redraw_canvas(canvas);
                return;
            } else if (event->y >= (canvas->height - BORDER_HEIGHT_BOTTOM - arrow_size) && event->y < (canvas->height - BORDER_HEIGHT_BOTTOM)) {
                // Down arrow clicked: scroll down by fixed amount
                canvas->scroll_y = min(canvas->max_scroll_y, canvas->scroll_y + 20);
                redraw_canvas(canvas);
                return;
            }
        }

        // Horizontal scrollbar 
        int hb_x = BORDER_WIDTH_LEFT + 10;
        int hb_y = canvas->height - BORDER_HEIGHT_BOTTOM;
        int hb_w = (canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT) - 54 - 10;
        int hb_h = BORDER_HEIGHT_BOTTOM;
        if (event->x >= hb_x && event->x < hb_x + hb_w && event->y >= hb_y && event->y < hb_y + hb_h) {
            // Calculate knob position and size
            float ratio = (float)hb_w / canvas->content_width;
            int knob_w = max(MIN_KNOB_SIZE, (int)(ratio * hb_w));
            float pos_ratio = (float)canvas->scroll_x / canvas->max_scroll_x;
            int knob_x = hb_x + (int)(pos_ratio * (hb_w - knob_w));

            // Check if click is on knob (start drag)
            if (event->x >= knob_x && event->x < knob_x + knob_w) {
                scrolling_canvas = canvas;
                scrolling_vertical = false;
                initial_scroll = canvas->scroll_x;
                scroll_start_pos = event->x_root;
                return;  // Prevent other actions
            } else {
                // Click on track: jump knob toward click position
                float click_ratio = (float)(event->x - hb_x) / hb_w;
                int new_scroll = (int)(click_ratio * canvas->max_scroll_x);
                canvas->scroll_x = max(0, min(new_scroll, canvas->max_scroll_x));
                redraw_canvas(canvas);
                return;
            }
        }

        // handle horizontal arrows clicks
        //int arrow_size = 20;
        if (event->y >= hb_y && event->y < hb_y + hb_h) {
            if (event->x >= (canvas->width - BORDER_WIDTH_RIGHT - (2 * arrow_size)) && event->x < (canvas->width - BORDER_WIDTH_RIGHT - arrow_size)) {
                // Left arrow clicked: scroll left
                canvas->scroll_x = max(0, canvas->scroll_x - 20);
                redraw_canvas(canvas);
                return;
            } else if (event->x >= (canvas->width - BORDER_WIDTH_RIGHT - arrow_size) && event->x < (canvas->width - BORDER_WIDTH_RIGHT)) {
                // Right arrow clicked: scroll right
                canvas->scroll_x = min(canvas->max_scroll_x, canvas->scroll_x + 20);
                redraw_canvas(canvas);
                return;
            }
        }
    }
}

void intuition_handle_motion_notify(XMotionEvent *event) {
    Display *dpy = get_display();
    if (!dpy) return;
    
    if (dragging_canvas) {
        // Calculate delta using root coordinates
        int delta_x = event->x_root - drag_start_x;
        int delta_y = event->y_root - drag_start_y;

        // Update window position
        window_start_x += delta_x;
        window_start_y += delta_y;
        XMoveWindow(dpy, dragging_canvas->win, window_start_x, window_start_y);
        dragging_canvas->x = window_start_x;
        dragging_canvas->y = window_start_y;
        redraw_canvas(dragging_canvas);
        XSync(dpy, False);

        // Update drag start for next event
        drag_start_x = event->x_root;
        drag_start_y = event->y_root;
        return;
    }
    if (resizing_canvas) {
        int delta_x = event->x_root - resize_start_x;
        int delta_y = event->y_root - resize_start_y;
        int new_width = window_start_width + delta_x;
        int new_height = window_start_height + delta_y;
        if (new_width < 150) new_width = 150;       // Minimum width
        if (new_height < 150) new_height = 150;     // Minimum height
        XResizeWindow(dpy, resizing_canvas->win, new_width, new_height);
        XSync(dpy, False);
        return;
    }
    // -
    if (scrolling_canvas) {
        int delta, scale, new_scroll;
        if (scrolling_vertical) {
            delta = event->y_root - scroll_start_pos;
            int sb_h = scrolling_canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM;
            float ratio = (float)sb_h / scrolling_canvas->content_height;
            int knob_h = max(MIN_KNOB_SIZE, (int)(ratio * sb_h));
            scale = (sb_h - knob_h > 0) ? (scrolling_canvas->max_scroll_y / (sb_h - knob_h)) : 0;
            new_scroll = initial_scroll + delta * scale;
            new_scroll = max(0, min(new_scroll, scrolling_canvas->max_scroll_y));
            scrolling_canvas->scroll_y = new_scroll;
        } else {
            delta = event->x_root - scroll_start_pos;
            int sb_w = scrolling_canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT;
            float ratio = (float)sb_w / scrolling_canvas->content_width;
            int knob_w = max(MIN_KNOB_SIZE, (int)(ratio * sb_w));
            scale = (sb_w - knob_w > 0) ? (scrolling_canvas->max_scroll_x / (sb_w - knob_w)) : 0;
            new_scroll = initial_scroll + delta * scale;
            new_scroll = max(0, min(new_scroll, scrolling_canvas->max_scroll_x));
            scrolling_canvas->scroll_x = new_scroll;
        }
        redraw_canvas(scrolling_canvas);
        XSync(get_display(), False);
        return;  // Prevent other motion handling
    }
}

void intuition_handle_destroy_notify(XDestroyWindowEvent *event) {
    Canvas *canvas = find_canvas_by_client(event->window);  // Find canvas by client window
    if (!canvas) return;                                    // Early return if no canvas found

    Display *dpy = get_display();       // Get display connection
    if (dpy && canvas->win != None) {   // Unmap frame if valid
        XUnmapWindow(dpy, canvas->win);
        XSync(dpy, False);              // Sync changes
    }

    fprintf(stderr, "DestroyNotify on client %lu\n", event->window); 

    // Perform cleanup for client canvas (mirrors own canvas cleanup)
    if (canvas->window_render != None) XRenderFreePicture(dpy, canvas->window_render);  // Free window render
    if (canvas->canvas_render != None) XRenderFreePicture(dpy, canvas->canvas_render);  // Free canvas render
    if (canvas->canvas_buffer != None) XFreePixmap(dpy, canvas->canvas_buffer);         // Free pixmap
    if (canvas->colormap != None && canvas->type != DESKTOP) XFreeColormap(dpy, canvas->colormap);  // Free colormap

    if (canvas->win != None && canvas->type != DESKTOP) XDestroyWindow(dpy, canvas->win);           // Destroy frame window

    free(canvas->path);     // Free path
    free(canvas->title);    // Free title

    if (active_window == canvas) active_window = NULL;  // Reset active

    manage_canvases(false, canvas);  // Remove from array

    free(canvas);  // Free struct

    Canvas *desktop = get_desktop_canvas(); // Get desktop
    if (desktop) redraw_canvas(desktop);    // Redraw desktop
}

void intuition_handle_button_release(XButtonEvent *event) {
    // cancel drag
    if (dragging_canvas) {
        dragging_canvas = NULL;
        XUngrabPointer(get_display(), CurrentTime);
        //fprintf(stderr, "Stopped dragging canvas %lu\n", event->window);
    }
    // cancel resize
    if (resizing_canvas) {
        resizing_canvas = NULL;
        //fprintf(stderr, "Stopped resizing canvas %lu\n", event->window);
    }
    // cancel scrolling
    if (scrolling_canvas) {
        scrolling_canvas = NULL;
    }
}

Canvas *find_canvas_by_client(Window client_win) {
    for (int i = 0; i < canvas_count; i++) {
        if (canvas_array[i]->client_win == client_win) return canvas_array[i];
    }
    return NULL;
}

void intuition_handle_map_request(XMapRequestEvent *event) {
    // mapping client, with properties
    Display *dpy = get_display();
    Canvas *frame = create_canvas(NULL, 100, 100, 400, 300, WINDOW);
    if (!frame) {
        fprintf(stderr, "Failed to create frame for client %lu\n", event->window);
        XMapWindow(dpy, event->window);
        return;
    }
    XReparentWindow(dpy, event->window, frame->win, BORDER_WIDTH_LEFT, BORDER_HEIGHT_TOP);
    XSelectInput(dpy, event->window, StructureNotifyMask | PropertyChangeMask);  // Enable DestroyNotify and property events
    XResizeWindow(dpy, event->window, 
                  (frame->width -2) - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT, 
                  (frame->height -2) - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM);
    frame->client_win = event->window;
    XMapWindow(dpy, event->window);
    XRaiseWindow(dpy, frame->win); // Ensure frame is raised
    redraw_canvas(frame);
    XSync(dpy, False);
    //fprintf(stderr, "Mapped client %lu to frame %lu, raised frame\n", event->window, frame->win);
}

void intuition_handle_configure_request(XConfigureRequestEvent *event) {
    Display *dpy = get_display();
    Canvas *canvas = find_canvas_by_client(event->window);
    if (!canvas) return;
    XWindowChanges changes;
    changes.x = event->x;
    changes.y = event->y;
    changes.width = event->width + BORDER_WIDTH_LEFT + BORDER_WIDTH_RIGHT;
    changes.height = event->height + BORDER_HEIGHT_TOP + BORDER_HEIGHT_BOTTOM;
    XConfigureWindow(dpy, canvas->win, event->value_mask, &changes);
    changes.width = event->width;
    changes.height = event->height;
    changes.x = BORDER_WIDTH_LEFT;
    changes.y = BORDER_HEIGHT_TOP;
    XConfigureWindow(dpy, event->window, event->value_mask, &changes);
    canvas->width = changes.width + BORDER_WIDTH_LEFT + BORDER_WIDTH_RIGHT;
    canvas->height = changes.height + BORDER_HEIGHT_TOP + BORDER_HEIGHT_BOTTOM;
    redraw_canvas(canvas);
    fprintf(stderr, "Configured frame %lu and client %lu\n", canvas->win, event->window);
}

void intuition_handle_configure_notify(XConfigureEvent *event) {
    Canvas *canvas = find_canvas(event->window);
    if (!canvas || (canvas->width == event->width && canvas->height == event->height)) return;

    Display *dpy = get_display();
    if (canvas->canvas_buffer != None) XFreePixmap(dpy, canvas->canvas_buffer);
    if (canvas->canvas_render != None) XRenderFreePicture(dpy, canvas->canvas_render);
    if (canvas->window_render != None) XRenderFreePicture(dpy, canvas->window_render);

    canvas->width = event->width;
    canvas->height = event->height;

    canvas->canvas_buffer = XCreatePixmap(dpy, canvas->win, canvas->width, canvas->height, canvas->depth);
    XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy, canvas->visual);
    canvas->canvas_render = XRenderCreatePicture(dpy, canvas->canvas_buffer, fmt, 0, NULL);
    canvas->window_render = XRenderCreatePicture(dpy, canvas->win, fmt, 0, NULL);

    if (canvas->client_win != None) {
        // client resize dimensions need -2 to stay well inside canvas decorations
        XResizeWindow(dpy, canvas->client_win,
                      (canvas->width -2) - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT,
                      (canvas->height - 2) - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM);
    }

    // don't scroll canvas on client windows
    if (canvas->type == WINDOW && canvas->client_win == None) {
        compute_max_scroll(canvas);  // Recompute max scroll for workbench windows on resize
    }
    redraw_canvas(canvas);
    XSync(dpy, False);
    //fprintf(stderr, "Resized canvas %lu to %dx%d\n", canvas->win, canvas->width, canvas->height);
}


void destroy_canvas(Canvas *canvas) {
    if (!canvas) return;  // Early return if canvas is null
    //printf("destroying canvas %lu\n", canvas->win);  // Log the canvas being destroyed
    clear_canvas_icons(canvas);  // Clear any icons associated with the canvas

    if (canvas->type == DESKTOP) return;  // Skip destruction for desktop canvas

    if (canvas->client_win != None) {  // Handle canvases with reparented client windows
        Display *dpy = get_display();  // Get the display connection
        Atom wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);    // Atom for protocols
        Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);   // Atom for delete window
        Atom *protocols;    // Pointer for protocol list
        int num;            // Number of protocols
        bool supports_delete = false;  // Flag for WM_DELETE_WINDOW support

        // Query if client supports WM_PROTOCOLS
        if (XGetWMProtocols(dpy, canvas->client_win, &protocols, &num)) {
            for (int i = 0; i < num; i++) {  // Check each protocol
                if (protocols[i] == wm_delete) {
                    supports_delete = true;  // Set flag if supported
                    break;
                }
            }
            XFree(protocols);  // Free the protocol list
        }

        XGrabServer(dpy);           // Grab server to ensure atomic operations
        if (supports_delete) {      // Send graceful close if supported
            XEvent ev = { .type = ClientMessage };  // Create ClientMessage event
            ev.xclient.window = canvas->client_win; // Set target window
            ev.xclient.message_type = wm_protocols; // Set message type
            ev.xclient.format = 32;                 // Set data format
            ev.xclient.data.l[0] = wm_delete;       // Set delete protocol
            ev.xclient.data.l[1] = CurrentTime;     // Set timestamp
            XSendEvent(dpy, canvas->client_win, False, NoEventMask, &ev);  // Send event
        } else {  // Fallback to forceful kill
            XKillClient(dpy, canvas->client_win);
        }
        XUnmapWindow(dpy, canvas->win); // Unmap the canvas frame
        XUngrabServer(dpy);             // Release server grab
        XSync(dpy, False);              // Sync to apply changes
        return;                         // Defer full cleanup to DestroyNotify handler
    } else {                            // Handle own canvases without clients: immediate cleanup
        Display *dpy = get_display();   // Get display
        if (canvas->window_render != None) XRenderFreePicture(dpy, canvas->window_render);  // Free window render picture
        if (canvas->canvas_render != None) XRenderFreePicture(dpy, canvas->canvas_render);  // Free canvas render picture
        if (canvas->canvas_buffer != None) XFreePixmap(dpy, canvas->canvas_buffer);         // Free pixmap buffer
        if (canvas->colormap != None) XFreeColormap(dpy, canvas->colormap);                 // Free colormap (skip desktop check as type != DESKTOP)

        if (canvas->win != None) XDestroyWindow(dpy, canvas->win);  // Destroy the window

        free(canvas->path);     // Free path string
        free(canvas->title);    // Free title string

        if (active_window == canvas) active_window = NULL;  // Reset active window if matching

        manage_canvases(false, canvas);  // Remove from canvas array

        free(canvas);  // Free the canvas struct

        Canvas *desktop = get_desktop_canvas(); // Get desktop canvas
        if (desktop) redraw_canvas(desktop);    // Redraw desktop if exists
    }
}

void cleanup_intuition(void) {
    if (!render_context) return;
    for (int i = 0; i < canvas_count; i++) {
        destroy_canvas(canvas_array[i]);
    }
    free(canvas_array);
    canvas_array = NULL;
    canvas_count = 0;
    canvas_array_size = 0;

    if (root_cursor) XFreeCursor(render_context->dpy, root_cursor);
    if (render_context->bg_pixmap != None) 
        XFreePixmap(render_context->dpy, render_context->bg_pixmap);
    XCloseDisplay(render_context->dpy);
    free(render_context);
    render_context = NULL;
    display = NULL;
}