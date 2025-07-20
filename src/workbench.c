/* Workbench implementation: Finds hit icons, scans directories, adds icons, checks intersections, refreshes/aligns icons. Core for file system UI. */

#include "workbench.h"
#include "icons.h"
#include "render.h"
#include <dirent.h>  // Directory reading.
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Find icon at mouse position (reverse for topmost).
FileIcon *find_hit_icon(int mx, int my, Canvas *canvas) {
    for (int i = canvas->num_icons - 1; i >= 0; i--) {
        FileIcon *icon = &canvas->icons[i];
        if (mx >= icon->x && mx < icon->x + icon->width && my >= icon->y && my < icon->y + icon->height) return icon;
    }
    return NULL;
}

// Scan directory for files/dirs, add icons.
void scan_icons(RenderContext *ctx, const char *path, FileIcon **icons, int *num_icons, Canvas *canvas) {
    DIR *dir = opendir(path);
    if (!dir) return;
    struct dirent *entry;
    char *files[MAX_FILES];
    int file_types[MAX_FILES], file_count = 0;
    while ((entry = readdir(dir)) && file_count < MAX_FILES) {
        if (entry->d_name[0] == '.') continue;  // Skip hidden.
        if (entry->d_type == DT_REG || entry->d_type == DT_DIR) {
            files[file_count] = strdup(entry->d_name);
            file_types[file_count] = (entry->d_type == DT_DIR) ? TYPE_DRAWER : TYPE_TOOL;
            file_count++;
        }
    }
    closedir(dir);
    for (int i = 0; i < file_count; i++) {
        add_icon(ctx, path, files[i], file_types[i], icons, num_icons, NULL, canvas);
        free(files[i]);
    }
    align_icons(canvas);
}

// Add single icon to canvas.
void add_icon(RenderContext *ctx, const char *dir_path, const char *name, int type, FileIcon **icons, int *num_icons, const char *custom_icon_path, Canvas *canvas) {
    if (*num_icons >= MAX_FILES) return;
    const char *dir_path_safe = dir_path ? dir_path : "";
    *icons = realloc(*icons, sizeof(FileIcon) * (*num_icons + 1));  // Grow array.
    FileIcon *icon = &(*icons)[*num_icons];
    memset(icon, 0, sizeof(FileIcon));
    icon->filename = strdup(name);
    size_t path_len = strlen(dir_path_safe) + strlen(name) + 2;
    icon->path = malloc(path_len);
    snprintf(icon->path, path_len, "%s%s%s", dir_path_safe, name, (type == TYPE_DRAWER ? "/" : ""));
    icon->label = strdup(name);
    int len = strlen(icon->label);
    icon->display_label = malloc(len > 10 ? 13 : len + 1);
    if (len > 10) {
        snprintf(icon->display_label, 13, "%.10s..", icon->label);  // Truncate label.
    } else {
        strcpy(icon->display_label, icon->label);
    }
    icon->type = type;
    const char *icon_path = custom_icon_path ? custom_icon_path : (type == TYPE_DRAWER ? def_drawer_path : def_tool_path);
    icon->icon_path = strdup(icon_path);
    if (load_icon(ctx->dpy, icon_path, &icon->icon)) {  // Load failed, clean up.
        free(icon->filename);
        free(icon->path);
        free(icon->label);
        free(icon->display_label);
        free(icon->icon_path);
        return;
    }
    XGlyphInfo extents;
    XftTextExtentsUtf8(ctx->dpy, ctx->font, (FcChar8 *)icon->display_label, strlen(icon->display_label), &extents);
    int text_width = extents.xOff;
    int text_height = ctx->font->height;
    icon->width = (icon->icon.width > text_width ? icon->icon.width : text_width) + 8;
    icon->height = icon->icon.height + text_height + 4;
    icon->pixmap = XCreatePixmap(ctx->dpy, canvas->win, icon->width, icon->height, 32);
    XRenderPictureAttributes pa = { .repeat = True };
    icon->picture = XRenderCreatePicture(ctx->dpy, icon->pixmap, ctx->fmt, CPRepeat, &pa);
    XRenderColor clear = {0, 0, 0, 0};
    XRenderFillRectangle(ctx->dpy, PictOpSrc, icon->picture, &clear, 0, 0, icon->width, icon->height);
    GC gc = XCreateGC(ctx->dpy, icon->pixmap, 0, NULL);
    int img_x = (icon->width - icon->icon.width) / 2;
    XPutImage(ctx->dpy, icon->pixmap, gc, icon->icon.image, 0, 0, img_x, 0, icon->icon.width, icon->icon.height);
    XSync(ctx->dpy, False);
    XFreeGC(ctx->dpy, gc);
    XDestroyImage(icon->icon.image);
    icon->icon.image = NULL;
    XftDraw *draw = XftDrawCreate(ctx->dpy, icon->pixmap, ctx->visual, ctx->cmap);
    int text_x = (icon->width - text_width) / 2;
    XftDrawStringUtf8(draw, &ctx->label_color, ctx->font, text_x, icon->icon.height + text_height + 2, (FcChar8 *)icon->display_label, strlen(icon->display_label));
    XftDrawDestroy(draw);
    XSync(ctx->dpy, False);
    (*num_icons)++;
}

// Check rectangle intersection.
bool rect_intersect(XRectangle *a, XRectangle *b) {
    return !(a->x + a->width <= b->x || b->x + b->width <= a->x ||
             a->y + a->height <= b->y || b->y + b->height <= a->y);
}

// Refresh icons by rescan.
void refresh_icons(RenderContext *ctx, Canvas *canvas) {
    for (int i = 0; i < canvas->num_icons; i++) free_icon(ctx->dpy, &canvas->icons[i]);
    free(canvas->icons);
    canvas->icons = NULL;
    canvas->num_icons = 0;
    scan_icons(ctx, canvas->path, &canvas->icons, &canvas->num_icons, canvas);
    redraw_canvas(ctx, canvas, NULL);
}

// Align icons in grid layout.
void align_icons(Canvas *canvas) {
    int border = (canvas->titlebar_height > 0) ? BORDER_HEIGHT_BOTTOM : 0;
    int y_offset = (canvas->titlebar_height > 0) ? 0 : MENUBAR_HEIGHT;
    int avail_h = canvas->height - canvas->titlebar_height - border * 2 - y_offset;
    int max_rows = avail_h / ICON_SPACING;
    if (max_rows < 1) max_rows = 1;
    int num_cols = (canvas->num_icons + max_rows - 1) / max_rows;
    for (int i = 0; i < canvas->num_icons; i++) {
        FileIcon *icon = &canvas->icons[i];
        int row = i % max_rows;
        int col = i / max_rows;
        icon->x = border + col * ICON_SPACING;
        icon->y = canvas->titlebar_height + border + y_offset + row * ICON_SPACING;
    }
}