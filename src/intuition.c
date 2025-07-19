/* Intuition implementation: Provides functions to create, activate, close, iconify canvases, and draw frames. Handles window logic. */

#include "intuition.h"
#include "render.h"
#include "workbench.h"
#include <X11/cursorfont.h>  // Cursors.
#include <stdio.h>  // snprintf.
#include <stdlib.h>  // free.
#include <string.h>  // strdup.

// Create canvas window with attributes.
Window create_canvas_window(RenderContext *ctx, Window parent, int x, int y, int w, int h, XSetWindowAttributes *attrs) {
    attrs->colormap = ctx->cmap;  // Set colormap.
    attrs->border_pixel = 0;  // No border.
    attrs->background_pixel = 0;  // Transparent bg.
    attrs->override_redirect = True;  // Override WM for positioning.
    attrs->event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | StructureNotifyMask | KeyPressMask;  // Events.
    Window win = XCreateWindow(ctx->dpy, parent, x, y, w, h, 0, 32, InputOutput, ctx->visual, CWColormap | CWBorderPixel | CWBackPixel | CWOverrideRedirect | CWEventMask, attrs);
    Cursor cursor = XCreateFontCursor(ctx->dpy, XC_left_ptr);
    XDefineCursor(ctx->dpy, win, cursor);
    return win;
}

// Activate canvas, deactivate others.
void activate_canvas(RenderContext *ctx, Canvas *canvas, Canvas *folders, int num_folders) {
    for (int j = 0; j < num_folders; j++) {
        if (&folders[j] == canvas) continue;
        if (folders[j].win == None) continue;
        if (folders[j].active) {
            folders[j].active = false;
            redraw_canvas(ctx, &folders[j], NULL);  // Redraw inactive.
        }
    }
    if (!canvas->active) {
        canvas->active = true;
        redraw_canvas(ctx, canvas, NULL);  // Redraw active.
    }
    ctx->active_canvas = canvas;
    if (canvas->titlebar_height > 0) XRaiseWindow(ctx->dpy, canvas->win);  // Raise if titled.
    if (canvas->client_win) {
        XSetInputFocus(ctx->dpy, canvas->client_win, RevertToParent, CurrentTime);  // Focus client.
    } else {
        XSetInputFocus(ctx->dpy, canvas->win, RevertToParent, CurrentTime);  // Focus canvas.
    }
    XSync(ctx->dpy, False);
}

// Close canvas, free resources.
void close_canvas(RenderContext *ctx, Canvas *canvas, Canvas *folders, int *num_folders) {
    XWindowAttributes wa;
    if (canvas->client_win) {
        if (XGetWindowAttributes(ctx->dpy, canvas->client_win, &wa) && wa.map_state != IsUnmapped) {  // If mapped.
            Atom wm_protocols = XInternAtom(ctx->dpy, "WM_PROTOCOLS", False);
            Atom wm_delete = XInternAtom(ctx->dpy, "WM_DELETE_WINDOW", False);
            XEvent msg;
            msg.type = ClientMessage;
            msg.xclient.window = canvas->client_win;
            msg.xclient.message_type = wm_protocols;
            msg.xclient.format = 32;
            msg.xclient.data.l[0] = wm_delete;
            msg.xclient.data.l[1] = CurrentTime;
            XSendEvent(ctx->dpy, canvas->client_win, False, NoEventMask, &msg);  // Send close request.
            XRemoveFromSaveSet(ctx->dpy, canvas->client_win);
            return;  // Wait for DestroyNotify.
        } else {
            XRemoveFromSaveSet(ctx->dpy, canvas->client_win);
            canvas->client_win = 0;
            canvas->client_pic = 0;
        }
    } else {  // Folder close.
        for (int i = 0; i < canvas->num_icons; i++) free_icon(ctx->dpy, &canvas->icons[i]);
        if (canvas->icons) free(canvas->icons);
        if (canvas->path) free(canvas->path);
    }
    if (canvas->back_pic) XRenderFreePicture(ctx->dpy, canvas->back_pic);
    if (canvas->win_pic) XRenderFreePicture(ctx->dpy, canvas->win_pic);
    if (canvas->backing) XFreePixmap(ctx->dpy, canvas->backing);
    if (canvas->win) XDestroyWindow(ctx->dpy, canvas->win);
    if (canvas->title) free(canvas->title);
    canvas->win = None;
    canvas->client_win = 0;
    canvas->back_pic = 0;
    canvas->win_pic = 0;
    canvas->backing = 0;
    canvas->icons = NULL;
    canvas->num_icons = 0;
    canvas->path = NULL;
    canvas->title = NULL;
    if (ctx->active_canvas == canvas) ctx->active_canvas = NULL;
}

// Iconify canvas to desktop icon.
void iconify_canvas(RenderContext *ctx, Canvas *canvas, Canvas *desktop) {
    if (canvas->client_win) {
        Atom wm_state = XInternAtom(ctx->dpy, "WM_STATE", False);
        long data[2] = {3, None};  // IconicState.
        XChangeProperty(ctx->dpy, canvas->client_win, wm_state, wm_state, 32, PropModeReplace, (unsigned char*)data, 2);
    } else {
        for (int i = 0; i < canvas->num_icons; i++) {
            if (canvas->icons[i].picture) XRenderFreePicture(ctx->dpy, canvas->icons[i].picture);
            canvas->icons[i].picture = 0;
            if (canvas->icons[i].pixmap) XFreePixmap(ctx->dpy, canvas->icons[i].pixmap);
            canvas->icons[i].pixmap = 0;
        }
    }
    XSync(ctx->dpy, False);
    XUnmapWindow(ctx->dpy, canvas->win);  // Unmap frame.
    XSync(ctx->dpy, False);
    canvas->client_pic = 0;
    if (canvas->back_pic) XRenderFreePicture(ctx->dpy, canvas->back_pic);
    if (canvas->win_pic) XRenderFreePicture(ctx->dpy, canvas->win_pic);
    if (canvas->backing) XFreePixmap(ctx->dpy, canvas->backing);
    canvas->backing = 0;
    canvas->back_pic = 0;
    canvas->win_pic = 0;
    canvas->active = false;
    char icon_name[256];
    snprintf(icon_name, sizeof(icon_name), "%s", canvas->title ? canvas->title : "Window");  // Icon label.
    add_icon(ctx, canvas->path ? canvas->path : "", icon_name, TYPE_TOOL, &desktop->icons, &desktop->num_icons, iconify_path, desktop);  // Add to desktop.
    XSync(ctx->dpy, False);
    desktop->icons[desktop->num_icons - 1].type = TYPE_ICONIFIED;
    desktop->icons[desktop->num_icons - 1].iconified_canvas = canvas;
    align_icons(desktop);
    redraw_canvas(ctx, desktop, NULL);
    XSync(ctx->dpy, False);
}

// Draw frame borders and title.
void draw_frame(RenderContext *ctx, Canvas *canvas, Picture pic) {
    XRenderColor frame_color = canvas->active ? BLUE_FRAME : GRAY_FRAME;  // Active/inactive color.
    XRenderFillRectangle(ctx->dpy, PictOpSrc, pic, &frame_color, 0, 0, canvas->width, canvas->titlebar_height);  // Top.
    XRenderFillRectangle(ctx->dpy, PictOpSrc, pic, &frame_color, 0, canvas->titlebar_height, BORDER_WIDTH, canvas->height - canvas->titlebar_height - BORDER_WIDTH);  // Left.
    XRenderFillRectangle(ctx->dpy, PictOpSrc, pic, &frame_color, canvas->width - BORDER_WIDTH, canvas->titlebar_height, BORDER_WIDTH, canvas->height - canvas->titlebar_height - BORDER_WIDTH);  // Right.
    XRenderFillRectangle(ctx->dpy, PictOpSrc, pic, &frame_color, 0, canvas->height - BORDER_WIDTH, canvas->width, BORDER_WIDTH);  // Bottom.
    if (canvas->title && canvas->titlebar_height > 0) {
        XftDraw *draw = XftDrawCreate(ctx->dpy, canvas->backing, ctx->visual, ctx->cmap);
        XftColor text_col;
        text_col.color = (XRenderColor){0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}; // White
        int text_y = (canvas->titlebar_height + ctx->font->ascent - ctx->font->descent) / 2 + ctx->font->descent;
        XftDrawStringUtf8(draw, &text_col, ctx->font, 5, text_y, (FcChar8 *)canvas->title, strlen(canvas->title));  // Draw title text.
        XftDrawDestroy(draw);
    }
}