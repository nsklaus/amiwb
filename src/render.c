/* Render implementation: Loads JPEG wallpapers, sets root background, redraws canvases (bg, frames, icons/clients), composites areas. Handles drawing. */

#include "render.h"
#include "config.h"
#include "workbench.h"
#include <jpeglib.h>  // JPEG lib for wallpaper.
#include <stdlib.h>
#include <setjmp.h>  // For JPEG error handling
#include <X11/Xatom.h>  // Atoms like _XROOTPMAP_ID.

// Load JPEG to pixmap.
static Pixmap load_jpeg(Display *dpy, const char *filename, int *width, int *height) {
    FILE *infile = fopen(filename, "rb");
    if (!infile) return None;
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, infile);
    jpeg_read_header(&cinfo, TRUE);
    jpeg_start_decompress(&cinfo);
    *width = cinfo.output_width;
    *height = cinfo.output_height;
    int depth = DefaultDepth(dpy, DefaultScreen(dpy));
    Pixmap pix = XCreatePixmap(dpy, DefaultRootWindow(dpy), *width, *height, depth);
    XImage *image = XCreateImage(dpy, DefaultVisual(dpy, DefaultScreen(dpy)), depth, ZPixmap, 0, malloc(*width * *height * 4), *width, *height, 32, 0);
    unsigned char *row = malloc(cinfo.output_width * cinfo.output_components);
    for (int y = 0; y < cinfo.output_height; y++) {
        jpeg_read_scanlines(&cinfo, &row, 1);
        for (int x = 0; x < cinfo.output_width; x++) {
            unsigned long pixel = (row[x*3] << 16) | (row[x*3+1] << 8) | row[x*3+2] | (0xFF << 24); // RGB to pixel with alpha.
            XPutPixel(image, x, y, pixel);
        }
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(infile);
    free(row);
    GC gc = XCreateGC(dpy, pix, 0, NULL);
    XPutImage(dpy, pix, gc, image, 0, 0, 0, 0, *width, *height);
    XFreeGC(dpy, gc);
    XDestroyImage(image);
    return pix;
}

// Set root wallpaper from JPEG.
void set_wallpaper(RenderContext *ctx, const char *path) {
    int w, h;
    Pixmap pix = load_jpeg(ctx->dpy, path, &w, &h);
    if (pix == None) return;
    Window root = DefaultRootWindow(ctx->dpy);
    if (ctx->bg_pixmap != None) XFreePixmap(ctx->dpy, ctx->bg_pixmap);
    ctx->bg_pixmap = pix;
    XSetWindowBackgroundPixmap(ctx->dpy, root, pix);  // Set root bg.
    XClearWindow(ctx->dpy, root);  // Clear to apply.
    Atom root_pmap = XInternAtom(ctx->dpy, "_XROOTPMAP_ID", False);
    XChangeProperty(ctx->dpy, root, root_pmap, XA_PIXMAP, 32, PropModeReplace, (unsigned char *)&pix, 1);  // Set property for persistence.
}

// Redraw canvas, handling dirty rect or full.
void redraw_canvas(RenderContext *ctx, Canvas *canvas, XRectangle *dirty_rect) {
    XRectangle full = {0, 0, canvas->width, canvas->height};
    XRectangle *dirty = dirty_rect ? dirty_rect : &full;
    XRenderSetPictureClipRectangles(ctx->dpy, canvas->back_pic, 0, 0, dirty, 1);  // Clip to dirty.
    if (canvas->titlebar_height == 0) { // Desktop: composite wallpaper.
        Atom root_pmap = XInternAtom(ctx->dpy, "_XROOTPMAP_ID", True);
        if (root_pmap != None) {
            Atom actual_type;
            int actual_format;
            unsigned long nitems, bytes_after;
            Pixmap *bg_pixmap;
            if (XGetWindowProperty(ctx->dpy, DefaultRootWindow(ctx->dpy), root_pmap, 0L, 1L, False, XA_PIXMAP, &actual_type, &actual_format, &nitems, &bytes_after, (unsigned char **)&bg_pixmap) == Success && nitems == 1) {
                Picture bg_pic = XRenderCreatePicture(ctx->dpy, *bg_pixmap, XRenderFindVisualFormat(ctx->dpy, DefaultVisual(ctx->dpy, DefaultScreen(ctx->dpy))), 0, NULL);
                XRenderPictureAttributes pa = {.repeat = True};
                XRenderChangePicture(ctx->dpy, bg_pic, CPRepeat, &pa);
                XRenderComposite(ctx->dpy, PictOpSrc, bg_pic, None, canvas->back_pic, dirty->x, canvas->y + dirty->y, 0, 0, dirty->x, dirty->y, dirty->width, dirty->height);
                XRenderFreePicture(ctx->dpy, bg_pic);
                XFree(bg_pixmap);
            } else {
                XRenderFillRectangle(ctx->dpy, PictOpSrc, canvas->back_pic, &canvas->bg_color, dirty->x, dirty->y, dirty->width, dirty->height);
            }
        } else {
            XRenderFillRectangle(ctx->dpy, PictOpSrc, canvas->back_pic, &canvas->bg_color, dirty->x, dirty->y, dirty->width, dirty->height);
        }
    } else {
        draw_frame(ctx, canvas, canvas->back_pic);  // Draw borders.
        XRenderFillRectangle(ctx->dpy, PictOpSrc, canvas->back_pic, &canvas->bg_color, BORDER_WIDTH, canvas->titlebar_height, canvas->width - BORDER_WIDTH * 2, canvas->height - canvas->titlebar_height - BORDER_WIDTH); // Fill content.
    }

    if (canvas->client_win && canvas->client_pic) {  // Composite client if present.
        XWindowAttributes wa;
        if (XGetWindowAttributes(ctx->dpy, canvas->client_win, &wa) == 0 || wa.map_state != IsViewable) {
            // Skip if not viewable.
        } else {
            XRenderComposite(ctx->dpy, PictOpOver, canvas->client_pic, None, canvas->back_pic, 0, 0, 0, 0, BORDER_WIDTH, canvas->titlebar_height, canvas->width - BORDER_WIDTH * 2, canvas->height - TITLEBAR_HEIGHT - BORDER_WIDTH);
        }
    } else if (!canvas->client_win) { // Composite icons.
        for (int i = 0; i < canvas->num_icons; i++) {
            FileIcon *icon = &canvas->icons[i];
            XRectangle icon_rect = {icon->x, icon->y, icon->width, icon->height};
            if (rect_intersect(&icon_rect, dirty) && icon->picture) {
                XRenderComposite(ctx->dpy, PictOpOver, icon->picture, None, canvas->back_pic, 0, 0, 0, 0, icon->x, icon->y, icon->width, icon->height);
            }
        }
    }
    XRenderSetPictureClipRectangles(ctx->dpy, canvas->back_pic, 0, 0, &full, 1);  // Reset clip.
    composite_rect(ctx, canvas, dirty->x, dirty->y, dirty->width, dirty->height);  // Composite to screen.
    XSync(ctx->dpy, False);
}

// Composite backing to window rect.
void composite_rect(RenderContext *ctx, Canvas *canvas, int x, int y, int w, int h) {
    XRenderComposite(ctx->dpy, PictOpSrc, canvas->back_pic, None, canvas->win_pic, x, y, 0, 0, x, y, w, h);
}