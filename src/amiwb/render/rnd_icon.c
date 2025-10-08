// File: rnd_icon.c
// Icon rendering for workbench icons
// Draws icon graphics and labels using XRender compositing and Xft text

#include "rnd_internal.h"
#include "../intuition/itn_public.h"
#include "../font_manager.h"
#include <string.h>

// Render a single icon
void render_icon(FileIcon *icon, Canvas *canvas) {
    //printf("render_icon called\n");
    if (!icon || icon->display_window == None || !icon->current_picture) {
        // Only log the error once per icon to prevent log spam
        if (icon && !icon->render_error_logged) {
            log_error("[ERROR] render_icon: Invalid icon "
                    "(icon=%p, window=%p, picture=%p, filename=%s ) - will not log again",
                (void*)icon,
                icon->display_window ? (void*)icon->display_window : NULL,
                icon->current_picture ? (void*)icon->current_picture : NULL,
                icon->label ? icon->label : "(null)");
            icon->render_error_logged = true;
        }
        return;
    }

    RenderContext *ctx = get_render_context();
    if (!ctx) {
        log_error("[ERROR] render_icon: No render context");
        return;
    }
    if (!canvas) {
        log_error("[ERROR] in render.c, render_icon(), canvas is NULL");
        return;
    }
    int base_x = (canvas->type == WINDOW) ? BORDER_WIDTH_LEFT : 0;
    int base_y = (canvas->type == WINDOW) ? BORDER_HEIGHT_TOP : 0;
    int render_x = base_x + icon->x - canvas->scroll_x;
    int render_y = base_y + icon->y - canvas->scroll_y;
    // Use appropriate dimensions based on selection state
    int render_width = icon->selected ? icon->sel_width : icon->width;
    int render_height = icon->selected ? icon->sel_height : icon->height;
    XRenderComposite(ctx->dpy, PictOpOver, icon->current_picture, None, canvas->canvas_render,
                     0, 0, 0, 0, render_x, render_y, render_width, render_height);

    XftFont *font = font_manager_get();
    if (!font) {
        log_error("[ERROR] render_icon: Font not loaded");
        return;
    }
    if (!icon->label) {
        log_error("[ERROR] render_icon: No label for icon");
        return;
    }

    // Use cached XftDraw instead of creating a new one
    if (!canvas->xft_draw) {
        log_error("[ERROR] render_icon: No cached XftDraw for label '%s'", icon->label);
        return;
    }

    const char *display_label = icon->label;  // Use full label

    XftColor label_color;
    XRenderColor render_color = *((canvas->type == DESKTOP) ? &DESKFONTCOL : &WINFONTCOL);
    XftColorAllocValue(ctx->dpy, canvas->visual, canvas->colormap, &render_color, &label_color);
    XGlyphInfo extents;
    XftTextExtentsUtf8(ctx->dpy, font, (FcChar8 *)display_label, strlen(display_label), &extents);
    int text_x = render_x + (icon->width - extents.xOff) / 2;
    int text_y = render_y + icon->height + font->ascent + 2;
    XftDrawStringUtf8(canvas->xft_draw, &label_color, font, text_x, text_y,
                      (FcChar8 *)display_label, strlen(display_label));
    XftColorFree(ctx->dpy, canvas->visual, canvas->colormap, &label_color);
}
