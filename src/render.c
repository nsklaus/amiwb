// File: render.c
#define _POSIX_C_SOURCE 200809L
#include "render.h"
#include "intuition.h"
#include "workbench.h"
#include "config.h"
#include "menus.h"
#include "dialogs.h"
#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>
#include <X11/Xft/Xft.h>
#include <fontconfig/fontconfig.h>
#include <unistd.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "resize.h"

// Global font and UI colors. Centralize text style so all drawing
// uses the same metrics and palette. Font may be NULL early.
static XftFont *font = NULL;
static XftColor text_color_black;
static XftColor  text_color_white;
#define RESOURCE_DIR_SYSTEM "/usr/local/share/amiwb"  
#define RESOURCE_DIR_USER ".config/amiwb"  

// Resolve a resource path, preferring per-user config then system dir.
static char *get_resource_path(const char *rel_path) {
    char *home = getenv("HOME");
    char user_path[1024];
    snprintf(user_path, sizeof(user_path), "%s/%s/%s", home, RESOURCE_DIR_USER, rel_path);
    if (access(user_path, F_OK) == 0) {
        return strdup(user_path);
    }
    char sys_path[1024];
    snprintf(sys_path, sizeof(sys_path), "%s/%s", RESOURCE_DIR_SYSTEM, rel_path);
    return strdup(sys_path);
}

// Draw up and down arrow controls for vertical scrollbar
static void draw_vertical_scrollbar_arrows(Display *dpy, Picture dest, Canvas *canvas) {
    int window_width = canvas->width;
    int window_height = canvas->height;
    // Right border arrow separators
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT + 1, window_height - BORDER_HEIGHT_BOTTOM - 1, BORDER_WIDTH_RIGHT, 1); 
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - BORDER_WIDTH_RIGHT + 1, window_height - BORDER_HEIGHT_BOTTOM - 20, BORDER_WIDTH_RIGHT - 2, 1); 
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT + 1, window_height - BORDER_HEIGHT_BOTTOM - 21, BORDER_WIDTH_RIGHT - 2, 1); 
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - BORDER_WIDTH_RIGHT + 1, window_height - BORDER_HEIGHT_BOTTOM - 40, BORDER_WIDTH_RIGHT - 2, 1); 
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT + 1, window_height - BORDER_HEIGHT_BOTTOM - 41, BORDER_WIDTH_RIGHT - 2, 1);
    
    // Down arrow button (bottom)
    if (canvas->v_arrow_down_armed) {
        // Sunken effect - black on top/left, white on bottom/right
        XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT, window_height - BORDER_HEIGHT_BOTTOM - 20, 1, 19);  // Left edge
        XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT, window_height - BORDER_HEIGHT_BOTTOM - 21, 20, 1);  // Top edge
        XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - 1, window_height - BORDER_HEIGHT_BOTTOM - 20, 1, 19);  // Right edge
        XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - BORDER_WIDTH_RIGHT, window_height - BORDER_HEIGHT_BOTTOM - 1, 20, 1);  // Bottom edge
    }

    // Down arrow shape
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 10, window_height - BORDER_HEIGHT_BOTTOM - 10, 2, 4);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 12, window_height - BORDER_HEIGHT_BOTTOM - 12, 2, 4);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 14, window_height - BORDER_HEIGHT_BOTTOM - 14, 2, 4);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 8, window_height - BORDER_HEIGHT_BOTTOM - 12, 2, 4);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 6, window_height - BORDER_HEIGHT_BOTTOM - 14, 2, 4);
    
    // Up arrow button (top)
    if (canvas->v_arrow_up_armed) {
        // Sunken effect - black on top/left, white on bottom/right
        XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT, window_height - BORDER_HEIGHT_BOTTOM - 40, 1, 19);  // Left edge
        XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT, window_height - BORDER_HEIGHT_BOTTOM - 41, 20, 1);  // Top edge
        XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - 1, window_height - BORDER_HEIGHT_BOTTOM - 40, 1, 19);  // Right edge
        XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - BORDER_WIDTH_RIGHT, window_height - BORDER_HEIGHT_BOTTOM - 21, 20, 1);  // Bottom edge
    }

    // Up arrow shape
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 10, window_height - BORDER_HEIGHT_BOTTOM - 35, 2, 4);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 12, window_height - BORDER_HEIGHT_BOTTOM - 33, 2, 4);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 14, window_height - BORDER_HEIGHT_BOTTOM - 31, 2, 4);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 8, window_height - BORDER_HEIGHT_BOTTOM - 33, 2, 4);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 6, window_height - BORDER_HEIGHT_BOTTOM - 31, 2, 4);
}

// Draw left and right arrow controls for horizontal scrollbar
static void draw_horizontal_scrollbar_arrows(Display *dpy, Picture dest, Canvas *canvas) {
    int window_width = canvas->width;
    int window_height = canvas->height;
    // Bottom border arrow separators
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - BORDER_WIDTH_RIGHT - 21, window_height - BORDER_HEIGHT_BOTTOM, 1, BORDER_HEIGHT_BOTTOM - 1); 
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT - 22, window_height - BORDER_HEIGHT_BOTTOM + 1, 1, BORDER_HEIGHT_BOTTOM - 1);  
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - BORDER_WIDTH_RIGHT - 41, window_height - BORDER_HEIGHT_BOTTOM, 1, BORDER_HEIGHT_BOTTOM - 1); 
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT - 42, window_height - BORDER_HEIGHT_BOTTOM + 1, 1, BORDER_HEIGHT_BOTTOM - 1);  
    
    // Right arrow button
    if (canvas->h_arrow_right_armed) {
        // Sunken effect - black on top/left, white on bottom/right
        XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT - 22, window_height - BORDER_HEIGHT_BOTTOM, 1, 20);  // Left edge
        XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT - 22, window_height - BORDER_HEIGHT_BOTTOM, 22, 1);  // Top edge
        XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - BORDER_WIDTH_RIGHT, window_height - BORDER_HEIGHT_BOTTOM, 1, 20);  // Right edge
        XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - BORDER_WIDTH_RIGHT - 22, window_height - 1, 22, 1);  // Bottom edge
    }

    // Right arrow shape
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT - 8, window_height - BORDER_HEIGHT_BOTTOM + 10, 4, 2);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT - 10, window_height - BORDER_HEIGHT_BOTTOM + 8, 4, 2);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT - 12, window_height - BORDER_HEIGHT_BOTTOM + 6, 4, 2);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT - 10, window_height - BORDER_HEIGHT_BOTTOM + 12, 4, 2);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT - 12, window_height - BORDER_HEIGHT_BOTTOM + 14, 4, 2);
    
    // Left arrow button
    if (canvas->h_arrow_left_armed) {
        // Sunken effect - black on top/left, white on bottom/right
        XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT - 42, window_height - BORDER_HEIGHT_BOTTOM, 1, 20);  // Left edge
        XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT - 42, window_height - BORDER_HEIGHT_BOTTOM, 20, 1);  // Top edge
        XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - BORDER_WIDTH_RIGHT - 22, window_height - BORDER_HEIGHT_BOTTOM, 1, 20);  // Right edge
        XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - BORDER_WIDTH_RIGHT - 42, window_height - 1, 20, 1);  // Bottom edge
    }

    // Left arrow shape
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 40 - 16, window_height - BORDER_HEIGHT_BOTTOM + 10, 4, 2);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 40 - 14, window_height - BORDER_HEIGHT_BOTTOM + 8, 4, 2);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 40 - 12, window_height - BORDER_HEIGHT_BOTTOM + 6, 4, 2);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 40 - 14, window_height - BORDER_HEIGHT_BOTTOM + 12, 4, 2);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 40 - 12, window_height - BORDER_HEIGHT_BOTTOM + 14, 4, 2);
}

// Draw the resize handle/grip in the bottom-right corner of window frame
static void draw_resize_button(Display *dpy, Picture dest, Canvas *canvas) {
    int window_width = canvas->width;
    int window_height = canvas->height;
    
    // Apply sunken 3D effect when resize button is armed
    if (canvas->resize_armed) {
        // Draw sunken borders - swap colors for pressed look
        // Left edge (dark when pressed)
        XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT, window_height - BORDER_HEIGHT_BOTTOM, 1, BORDER_HEIGHT_BOTTOM); 
        // Top edge (dark when pressed)
        XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT, window_height - BORDER_HEIGHT_BOTTOM, BORDER_WIDTH_RIGHT, 1);
        // Right edge (light when pressed) 
        XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - 1, window_height - BORDER_HEIGHT_BOTTOM, 1, BORDER_HEIGHT_BOTTOM);
        // Bottom edge (light when pressed)
        XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - BORDER_WIDTH_RIGHT, window_height - 1, BORDER_WIDTH_RIGHT, 1);
    } else {
        // Border edges of resize button (normal state)
        XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - BORDER_WIDTH_RIGHT, window_height - BORDER_HEIGHT_BOTTOM, 1, BORDER_HEIGHT_BOTTOM - 1); 
        XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT - 1, window_height - BORDER_HEIGHT_BOTTOM + 1, 1, BORDER_HEIGHT_BOTTOM - 1); 
    }
    
    // Main grip lines - black outlines
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT + 5, window_height - 5, 11, 1);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 5, window_height - 15, 1, 10);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - BORDER_WIDTH_RIGHT + 5, window_height - 7, 1, 3);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 7, window_height - 15, 2, 1);
    
    // Diagonal black grip pattern 
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 8, window_height - 14, 1, 1);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 9, window_height - 13, 1, 1);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 10, window_height - 12, 1, 1);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 11, window_height - 11, 1, 1);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 12, window_height - 10, 1, 1);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 13, window_height - 9, 1, 1);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK, window_width - 14, window_height - 8, 1, 1);
    
    // White highlight for 3D effect
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - 7, window_height - 14, 2, 9);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - 8, window_height - 13, 1, 8);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - 9, window_height - 12, 1, 7);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - 10, window_height - 11, 1, 6);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - 11, window_height - 10, 1, 5);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - 12, window_height - 9, 1, 4);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - 13, window_height - 8, 1, 3);
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE, window_width - 14, window_height - 7, 1, 2);
}

// ---- Wallpaper helpers ----
// Load image with Imlib2 into a full-screen Pixmap; tile if requested.
static Pixmap load_wallpaper_to_pixmap(Display *dpy, int screen_num, const char *path, bool tile) {
    if (!path || strlen(path) == 0) return None;
    Imlib_Image img = imlib_load_image(path);
    if (!img) {
        fprintf(stderr, "Failed to load wallpaper: %s\n", path);
        return None;
    }
    imlib_context_set_image(img);
    int img_width = imlib_image_get_width();
    int img_height = imlib_image_get_height();

    int screen_width = DisplayWidth(dpy, screen_num);
    int screen_height = DisplayHeight(dpy, screen_num);

    Pixmap pixmap = XCreatePixmap(dpy, RootWindow(dpy, screen_num), screen_width, screen_height, DefaultDepth(dpy, screen_num));

    imlib_context_set_drawable(pixmap);
    if (!tile) {
        imlib_render_image_on_drawable_at_size(0, 0, screen_width, screen_height);
    } else {
        for (int y = 0; y < screen_height; y += img_height) {
            for (int x = 0; x < screen_width; x += img_width) {
                imlib_render_image_on_drawable(x, y);
            }
        }
    }

    imlib_free_image();
    return pixmap;
}

// Public API: (re)load wallpapers into RenderContext so background
// draws fast without re-scaling images each frame.
void render_load_wallpapers(void) {
    RenderContext *ctx = get_render_context();
    if (!ctx) return;
    Display *dpy = ctx->dpy;
    int scr = DefaultScreen(dpy);

    // Free previous pixmaps and cached Pictures if any
    if (ctx->desk_img != None) { 
        XFreePixmap(dpy, ctx->desk_img); 
        ctx->desk_img = None; 
    }
    if (ctx->desk_picture != None) {
        XRenderFreePicture(dpy, ctx->desk_picture);
        ctx->desk_picture = None;
    }
    if (ctx->wind_img != None) { 
        XFreePixmap(dpy, ctx->wind_img); 
        ctx->wind_img = None; 
    }
    if (ctx->wind_picture != None) {
        XRenderFreePicture(dpy, ctx->wind_picture);
        ctx->wind_picture = None;
    }

    if (strlen(DESKPICT) > 0) {
        ctx->desk_img = load_wallpaper_to_pixmap(dpy, scr, DESKPICT, DESKTILE);
        // Create cached Picture for desktop wallpaper
        if (ctx->desk_img != None) {
            Visual *visual = DefaultVisual(dpy, DefaultScreen(dpy));
            XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy, visual);
            if (fmt) {
                ctx->desk_picture = XRenderCreatePicture(dpy, ctx->desk_img, fmt, 0, NULL);
            }
        }
    }
    if (strlen(WINDPICT) > 0) {
        ctx->wind_img = load_wallpaper_to_pixmap(dpy, scr, WINDPICT, WINDTILE);
        // Create cached Picture for window wallpaper
        if (ctx->wind_img != None) {
            Visual *visual = DefaultVisual(dpy, DefaultScreen(dpy));
            XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy, visual);
            if (fmt) {
                ctx->wind_picture = XRenderCreatePicture(dpy, ctx->wind_img, fmt, 0, NULL);
            }
        }
    }
}

// Initialize rendering resources. Requires RenderContext from
// init_intuition(). If font is not ready yet, callers should guard
// text drawing (redraw_canvas() already does).
void init_render(void) {
    RenderContext *ctx = get_render_context();
    if (!ctx) { 
        printf("Failed to get render_context (call init_intuition first)\n");
        return; 
    }

    // Initialize FontConfig
    if (!FcInit()) {
        fprintf(stderr, "Failed to initialize FontConfig\n");
        return;
    }

    // Load font with DPI

    char *font_path = get_resource_path(SYSFONT);

    FcPattern *pattern = FcPatternCreate();
    FcPatternAddString(pattern, FC_FILE, (const FcChar8 *)font_path);
    FcPatternAddDouble(pattern, FC_SIZE, 12.0);
    FcPatternAddInteger(pattern, FC_WEIGHT, 200); // bold please
    FcPatternAddDouble(pattern, FC_DPI, 75);
    FcConfigSubstitute(NULL, pattern, FcMatchPattern);
    XftDefaultSubstitute(ctx->dpy, DefaultScreen(ctx->dpy), pattern);
    font = XftFontOpenPattern(ctx->dpy, pattern);
    if (!font) {
        fprintf(stderr, "Failed to load font %s\n", font_path);
        FcPatternDestroy(pattern);
        free(font_path);
        return;
    }
    free(font_path);

    // Now that we have a render context and font, load wallpapers and refresh desktop
    render_load_wallpapers();
    Canvas *desk = get_desktop_canvas();
    if (desk) redraw_canvas(desk);

    // Initialize colors
    text_color_black.color = BLACK;
    text_color_white.color = WHITE;
}

int get_text_width(const char *text) {
    if (!font || !text) return 0;
    RenderContext *ctx = get_render_context();
    if (!ctx) return 0;
    XGlyphInfo extents;
    XftTextExtentsUtf8(ctx->dpy, font, (FcChar8 *)text, strlen(text), &extents);
    return extents.xOff;
}

// Provide access to the loaded UI font
XftFont *get_font(void) {
    return font;
}

// Clean up rendering resources
void cleanup_render(void) {
    RenderContext *ctx = get_render_context();
    if (!ctx) return;

    if (font) {
        XftFontClose(ctx->dpy, font);
        font = NULL;
    }
    if (text_color_black.pixel) {
        XftColorFree(ctx->dpy, DefaultVisual(ctx->dpy, DefaultScreen(ctx->dpy)), 
                     DefaultColormap(ctx->dpy, DefaultScreen(ctx->dpy)), &text_color_black);
    }
    if (text_color_white.pixel) {
        XftColorFree(ctx->dpy, DefaultVisual(ctx->dpy, DefaultScreen(ctx->dpy)), 
                     DefaultColormap(ctx->dpy, DefaultScreen(ctx->dpy)), &text_color_white);
    }
    FcFini();
    printf("Called cleanup_render() \n");
}

// Render a single icon
void render_icon(FileIcon *icon, Canvas *canvas) {
    //printf("render_icon called\n");
    if (!icon || icon->display_window == None || !icon->current_picture) {
        printf("[ERROR] render_icon: Invalid icon "
                "(icon=%p, canvas=%p, picture=%p, filename=%s )\n", 
            (void*)icon, 
            (void*)icon->display_window, 
            (void*)icon->current_picture,
            (icon && icon->label) ? icon->label : "(null)");
        return;
    }

    RenderContext *ctx = get_render_context();
    if (!ctx) {
        fprintf(stderr, "render_icon: No render context\n");
        return;
    }
    if (!canvas) { printf("in render.c, render_icon(), canvas failled  \n"); }
    int base_x = (canvas->type == WINDOW) ? BORDER_WIDTH_LEFT : 0;
    int base_y = (canvas->type == WINDOW) ? BORDER_HEIGHT_TOP : 0;
    int render_x = base_x + icon->x - canvas->scroll_x;
    int render_y = base_y + icon->y - canvas->scroll_y;
    // Use appropriate dimensions based on selection state
    int render_width = icon->selected ? icon->sel_width : icon->width;
    int render_height = icon->selected ? icon->sel_height : icon->height;
    XRenderComposite(ctx->dpy, PictOpOver, icon->current_picture, None, canvas->canvas_render,
                     0, 0, 0, 0, render_x, render_y, render_width, render_height);

    if (!font) {
        fprintf(stderr, "render_icon: Font not loaded\n");
        return;
    }
    if (!icon->label) {
        fprintf(stderr, "render_icon: No label for icon\n");
        return;
    }

    XftDraw *draw = XftDrawCreate(ctx->dpy, canvas->canvas_buffer,
        canvas->visual ? canvas->visual : 
        DefaultVisual(ctx->dpy, DefaultScreen(ctx->dpy)), canvas->colormap);

    if (!draw) {
        fprintf(stderr, "render_icon: Failed to create XftDraw for label '%s'\n", icon->label);
        return;
    }

    const char *display_label = icon->label;  // Use full label

    XftColor label_color;
    label_color.color = *((canvas->type == DESKTOP) ? &DESKFONTCOL : &WINFONTCOL);
    XGlyphInfo extents;
    XftTextExtentsUtf8(ctx->dpy, font, (FcChar8 *)display_label, strlen(display_label), &extents);
    int text_x = render_x + (icon->width - extents.xOff) / 2;
    int text_y = render_y + icon->height + font->ascent + 2;
    XftDrawStringUtf8(draw, &label_color, font, text_x, text_y,
                      (FcChar8 *)display_label, strlen(display_label));
    XftDrawDestroy(draw);
}

// Draw checkerboard pattern in a rectangle area
static void draw_checkerboard(Display *dpy, Picture dest, int x, int y, int w, int h, XRenderColor color1, XRenderColor color2) {
    const int square_size = 2;  // 2x2 pixel squares for checkerboard
    for (int i = 0; i < h; i += square_size) {
        for (int j = 0; j < w; j += square_size) {
            XRenderColor *color = ((i / square_size + j / square_size) % 2 == 0) ? &color1 : &color2;
            XRenderFillRectangle(dpy, PictOpSrc, dest, color, x + j, y + i, square_size, square_size);
        }
    }
}

// Redraw the entire canvas and its icons. Skips work if surfaces or
// context are missing, which can occur during early init or teardown.
void redraw_canvas(Canvas *canvas) {
    if (!canvas || canvas->width <= 0 || canvas->height <= 0 || 
        canvas->canvas_render == None || canvas->window_render == None) {
        printf("[REDRAW] Early return: canvas=%p, width=%d, height=%d, canvas_render=%lu, window_render=%lu\n",
               (void*)canvas, canvas ? canvas->width : -1, canvas ? canvas->height : -1, 
               canvas ? canvas->canvas_render : 0, canvas ? canvas->window_render : 0);
        return;
    }
    
    // PERFORMANCE OPTIMIZATION: During interactive resize, only redraw the canvas being resized
    // BUT: Always allow icon rendering - just skip buffer updates
    Canvas *resizing = resize_get_canvas();
    if (resizing && canvas != resizing) {
        return; // Skip redrawing non-resizing windows during resize
    }
    
    RenderContext *ctx = get_render_context();
    if (!ctx) return;
    
    // Skip validation - canvas lifecycle is properly managed
    // This eliminates a synchronous X11 call on every redraw

    // If canvas is WINDOW type with a client window, 
    // dest is window_render; otherwise, canvas_render.
    bool is_client_frame = (canvas->type == WINDOW && 
        canvas->client_win != None);
    bool has_bg_image = false;
    Picture dest = is_client_frame ? canvas->window_render : canvas->canvas_render;

    // set background pictures or fill background for non-client canvas
    // (desktop or empty windows and not clients, not menus)
    if (!is_client_frame) {
        // During interactive resize, use buffer dimensions to avoid rendering outside buffer bounds
        int render_width = canvas->resizing_interactive ? canvas->buffer_width : canvas->width;
        int render_height = canvas->resizing_interactive ? canvas->buffer_height : canvas->height;
        

        // Apply wallpaper background for desktop and icon view windows
        // Use cached Pictures instead of creating/destroying every frame
        Picture wallpaper_picture = None;
        if (canvas->type == DESKTOP && ctx->desk_picture != None) {
            wallpaper_picture = ctx->desk_picture;
        } else if (canvas->type == WINDOW && canvas->view_mode == VIEW_ICONS && ctx->wind_picture != None) {
            wallpaper_picture = ctx->wind_picture;
        }
        
        if (wallpaper_picture != None) {
            Display *dpy = ctx->dpy;
            XRenderComposite(dpy, PictOpSrc, wallpaper_picture, None, canvas->canvas_render, 
                0, 0, 0, 0, 0, 0, render_width, render_height);
            // No XRenderFreePicture - we're using cached Pictures now!
            has_bg_image = true;
        }

        if (!has_bg_image) {
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &canvas->bg_color, 
                0, 0, render_width, render_height);
        }

/*    } else {
        // Fill only content area for workbench windows
        int visible_w = canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT;
        int visible_h = canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM;
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &canvas->bg_color,
            BORDER_WIDTH_LEFT, BORDER_HEIGHT_TOP, visible_w, visible_h);
    } 
    if (!is_client_frame) {*/
        // =============
        // Render icons (now includes during interactive resize for better UX)
        // =============
        if ((canvas->type == DESKTOP || canvas->type == WINDOW) && !canvas->scanning){
            FileIcon **icon_array = get_icon_array();
            int icon_count = get_icon_count();

            // Compute visible content bounds (viewport) for clipping
            int view_left = canvas->scroll_x;
            int view_top = canvas->scroll_y;
            int view_right = view_left + (canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT);
            int view_bottom = view_top + (canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM);

            if (canvas->type == WINDOW && canvas->view_mode == VIEW_NAMES) {
                // Names list: draw rows with selection highlight and label text only
                XftFont *font = get_font();
                if (font) {
                    XftDraw *draw = XftDrawCreate(ctx->dpy, canvas->canvas_buffer,
                        canvas->visual ? canvas->visual : DefaultVisual(ctx->dpy, DefaultScreen(ctx->dpy)), canvas->colormap);
                    if (draw) {
                        int row_h = font->ascent + font->descent + 6;
                        for (int i = 0; i < icon_count; i++) {
                            FileIcon *icon = icon_array[i];
                            if (!icon || icon->display_window != canvas->win) continue;
                            int rx = icon->x; int ry = icon->y;
                            int render_x = BORDER_WIDTH_LEFT + rx - canvas->scroll_x;
                            int render_y = BORDER_HEIGHT_TOP + ry - canvas->scroll_y;
                            // Clip by viewport
                            if (render_y > BORDER_HEIGHT_TOP + (view_bottom - view_top)) continue;
                            if (render_y + row_h < BORDER_HEIGHT_TOP) continue;
                            // Compute text width for this row
                            const char *label = icon->label ? icon->label : "";
                            XGlyphInfo ext;
                            XftTextExtentsUtf8(ctx->dpy, font, (FcChar8 *)label, (int)strlen(label), &ext);
                            int padding = 10; // small horizontal padding
                            int sel_w = ext.xOff + padding;
                            int max_row_w = (canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT);
                            if (sel_w > max_row_w) sel_w = max_row_w; // do not exceed frame
                            // Background fill: always draw base gray for the viewport row band
                            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &canvas->bg_color,
                                BORDER_WIDTH_LEFT, render_y, max_row_w, row_h);
                            // Selection overlay should move with horizontal scroll
                            if (icon->selected) {
                                int sel_x = BORDER_WIDTH_LEFT - canvas->scroll_x;
                                // Clip selection overlay to viewport bounds
                                int clip_x = max(BORDER_WIDTH_LEFT, sel_x);
                                int clip_w = min(BORDER_WIDTH_LEFT + max_row_w, sel_x + sel_w) - clip_x;
                                if (clip_w > 0) {
                                    XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLUE,
                                        clip_x, render_y, clip_w, row_h);
                                }
                            }
                            // Text color: directories are white, files are black; selection stays white
                            bool is_dir = (icon->type == TYPE_DRAWER);
                            XRenderColor fg = icon->selected ? WHITE : (is_dir ? WHITE : WINFONTCOL);
                            XftColor xftfg; XftColorAllocValue(ctx->dpy, canvas->visual, canvas->colormap, &fg, &xftfg);
                            int baseline = render_y + font->ascent + 3;
                            int text_x = BORDER_WIDTH_LEFT + 6 - canvas->scroll_x;
                            XftDrawStringUtf8(draw, &xftfg, font, text_x, baseline,
                                              (FcChar8*)label, (int)strlen(label));
                            XftColorFree(ctx->dpy, canvas->visual, canvas->colormap, &xftfg);
                        }
                        XftDrawDestroy(draw);
                    }
                }
            } else {
                for (int i = 0; i < icon_count; i++) {
                    FileIcon *icon = icon_array[i];
                    if (icon->display_window == canvas->win) {
                        // Check if icon and label height intersects viewport
                        int icon_right = icon->x + icon->width;
                        // +20 for label
                        int icon_bottom = icon->y + icon->height + 20;  
                        if (icon_right < view_left || icon->x > view_right ||
                            icon_bottom < view_top || icon->y > view_bottom) {
                            continue; // Skip off-screen icon to optimize rendering
                        }
                        render_icon(icon, canvas);
                    }
                }
            }
        }
    }
    // ===========
    // render menu
    // ===========
    if (canvas->type == MENU) {
        // Check if this is a completion dropdown
        extern bool is_completion_dropdown(Canvas *canvas);
        if (is_completion_dropdown(canvas)) {
            extern void render_completion_dropdown(Canvas *canvas);
            render_completion_dropdown(canvas);
            // Composite the rendered dropdown to the window
            XRenderComposite(ctx->dpy, PictOpSrc, canvas->canvas_render, None, 
                           canvas->window_render, 0, 0, 0, 0, 0, 0, 
                           canvas->width, canvas->height);
            // XFlush removed - let X11 batch operations for better performance
            return;
        }
        
        Menu *menu = get_menu_by_canvas(canvas);
        if (!menu) return;

    XftDraw *draw = XftDrawCreate(ctx->dpy, canvas->canvas_buffer, canvas->visual, canvas->colormap);
    if (!draw) return;

    bool is_menubar = (canvas == get_menubar());
    int selected = menu->selected_item;
    int padding = 20;  // Consistent for all items
    
    // For dropdown menus, fill entire background with white first
    if (!is_menubar) {
        XRenderFillRectangle(ctx->dpy, PictOpSrc, canvas->canvas_render, &canvas->bg_color, 
                           0, 0, canvas->width, canvas->height);
    }

    int x = 10;  // Aligned start
    int y_base = font->ascent + (MENU_ITEM_HEIGHT - font->height) / 2 - 1;  // Raised by 1 pixel

    for (int i = 0; i < menu->item_count; i++) {
        const char *label = menu->items[i];
        if (!label) continue;

        XGlyphInfo extents;
        XftTextExtentsUtf8(ctx->dpy, font, (FcChar8 *)label, strlen(label), &extents);
        int item_width = extents.xOff + padding;

        XRenderColor bg_color;
        XRenderColor fg_color;
        if (is_menubar) {
            // Horizontal: highlight only if selected and has submenus (skips logo)
            bg_color = (i == selected && menu->submenus) ? BLACK : canvas->bg_color;
            fg_color = (i == selected && menu->submenus) ? WHITE : BLACK;
            XRenderFillRectangle(ctx->dpy, PictOpSrc, canvas->canvas_render, &bg_color, x, 0, item_width, MENU_ITEM_HEIGHT);
            XRenderFillRectangle(ctx->dpy, PictOpSrc, canvas->canvas_render, &BLACK, 0, MENU_ITEM_HEIGHT - 1, canvas->width, 1);
            XftColor item_fg;
            XftColorAllocValue(ctx->dpy, canvas->visual, canvas->colormap, &fg_color, &item_fg);
            XftDrawStringUtf8(draw, &item_fg, font, x + 10, y_base, (FcChar8 *)label, strlen(label));
            XftColorFree(ctx->dpy, canvas->visual, canvas->colormap, &item_fg);
            x += item_width;
        } else {
            // Vertical (submenu): highlight if selected, regardless of submenus
            // Check if item is disabled
            bool is_disabled = (menu->enabled && !menu->enabled[i]);
            
            // Use gray color for disabled items
            XRenderColor GRAY_DISABLED = {0x8080, 0x8080, 0x8080, 0xffff};  // Medium gray for disabled text
            fg_color = is_disabled ? GRAY_DISABLED : ((i == selected) ? WHITE : BLACK);
            
            int item_y = i * MENU_ITEM_HEIGHT + 4;  // Start 4 pixels down from top
            // Always fill item area with white first
            XRenderFillRectangle(ctx->dpy, PictOpSrc, canvas->canvas_render, &canvas->bg_color, 0, item_y, canvas->width, MENU_ITEM_HEIGHT);
            
            // For selected items, draw black highlight inset from left and right edges
            if (i == selected && !is_disabled) {
                // Draw the black highlight box with 4px white border on sides for visibility
                // This creates visible white vertical lines on both sides of the highlight
                XRenderFillRectangle(ctx->dpy, PictOpSrc, canvas->canvas_render, &BLACK, 4, item_y + 1, canvas->width - 8, MENU_ITEM_HEIGHT - 2);
            }
            
            XftColor item_fg;
            XftColorAllocValue(ctx->dpy, canvas->visual, canvas->colormap, &fg_color, &item_fg);
            XftDrawStringUtf8(draw, &item_fg, font, 10, item_y + y_base, (FcChar8 *)label, strlen(label));
            
            // Draw shortcut if present (e.g., "∷ R" for Rename) - also in gray if disabled
            if (menu->shortcuts && menu->shortcuts[i]) {
                char shortcut_text[32];
                // No space for shortcuts with modifiers (^Q), but keep space for single chars (E)
                if (menu->shortcuts[i][0] == '^') {
                    snprintf(shortcut_text, sizeof(shortcut_text), "%s%s", SHORTCUT_SYMBOL, menu->shortcuts[i]);
                } else {
                    snprintf(shortcut_text, sizeof(shortcut_text), "%s %s", SHORTCUT_SYMBOL, menu->shortcuts[i]);
                }
                
                XGlyphInfo shortcut_extents;
                XftTextExtentsUtf8(ctx->dpy, font, (FcChar8 *)shortcut_text, strlen(shortcut_text), &shortcut_extents);
                
                // Right-align shortcut with 1 char padding from right edge
                int shortcut_x = canvas->width - shortcut_extents.xOff - 10;  // 10 pixels padding from right
                XftDrawStringUtf8(draw, &item_fg, font, shortcut_x, item_y + y_base, (FcChar8 *)shortcut_text, strlen(shortcut_text));
            }
            
            XftColorFree(ctx->dpy, canvas->visual, canvas->colormap, &item_fg);
        }
    }
    
    // Draw black borders around dropdown menus (after all items are drawn)
    if (!is_menubar) {
        XRenderFillRectangle(ctx->dpy, PictOpSrc, canvas->canvas_render, &BLACK, 0, canvas->height - 1, canvas->width, 1);  // Bottom
        XRenderFillRectangle(ctx->dpy, PictOpSrc, canvas->canvas_render, &BLACK, 0, 0, canvas->width, 1);  // Top
        XRenderFillRectangle(ctx->dpy, PictOpSrc, canvas->canvas_render, &BLACK, 0, 0, 1, canvas->height);  // Left
        XRenderFillRectangle(ctx->dpy, PictOpSrc, canvas->canvas_render, &BLACK, canvas->width - 1, 0, 1, canvas->height);  // Right
    }
    
    XftDrawDestroy(draw);

        // ============
        // menu button and date/time
        // ============
        if (!get_show_menus_state()){
            
#if MENU_SHOW_DATE
            // Display date and time on the right side
            if (font) {
                time_t now;
                time(&now);
                struct tm *tm_info = localtime(&now);
                char datetime_buf[64];
                strftime(datetime_buf, sizeof(datetime_buf), MENUBAR_DATE_FORMAT, tm_info);
                
                // Create Xft draw context for date/time
                XftDraw *dt_draw = XftDrawCreate(ctx->dpy, canvas->canvas_buffer, canvas->visual, canvas->colormap);
                if (dt_draw) {
                    XftColor dt_color;
                    XRenderColor black_color = BLACK;
                    XftColorAllocValue(ctx->dpy, canvas->visual, canvas->colormap, &black_color, &dt_color);
                    
                    // Calculate text width to position it properly
                    XGlyphInfo extents;
                    XftTextExtentsUtf8(ctx->dpy, font, (FcChar8 *)datetime_buf, strlen(datetime_buf), &extents);
                    
                    // Position: end 4 chars (30 pixels) + 4 chars space before menu button
                    int text_x = canvas->width - 30 - 30 - extents.xOff;
                    int text_y = font->ascent + (MENU_ITEM_HEIGHT - font->height) / 2 - 1;  // Raised by 1 pixel
                    
                    XftDrawStringUtf8(dt_draw, &dt_color, font, text_x, text_y, 
                                      (FcChar8 *)datetime_buf, strlen(datetime_buf));
                    
                    XftColorFree(ctx->dpy, canvas->visual, canvas->colormap, &dt_color);
                    XftDrawDestroy(dt_draw);
                }
            }
#endif
 
            // menu right side, lower button 
            XRenderFillRectangle(ctx->dpy, PictOpSrc, canvas->canvas_render, &GRAY, canvas->width -28, 0 , 26, 19);
            XRenderFillRectangle(ctx->dpy, PictOpSrc, canvas->canvas_render, &WHITE, canvas->width -28, 0 , 26, 1);
            XRenderFillRectangle(ctx->dpy, PictOpSrc, canvas->canvas_render, &BLACK, canvas->width -2, 0 , 1, 20);
            XRenderFillRectangle(ctx->dpy, PictOpSrc, canvas->canvas_render, &BLACK, canvas->width -30, 0 , 1, 20);

            XRenderFillRectangle(ctx->dpy, PictOpSrc, canvas->canvas_render, &BLACK, canvas->width -25, 4 , 15, 8); 
            XRenderFillRectangle(ctx->dpy, PictOpSrc, canvas->canvas_render, &GRAY,  canvas->width -24, 5 , 13, 6); 
            XRenderFillRectangle(ctx->dpy, PictOpSrc, canvas->canvas_render, &BLACK, canvas->width -20, 7 , 15, 8); 
            XRenderFillRectangle(ctx->dpy, PictOpSrc, canvas->canvas_render, &WHITE, canvas->width -19, 8 , 13, 6); 
        
         }
    }
    
    // ===========
    // render dialog
    // ===========
    if (canvas->type == DIALOG) {
        render_dialog_content(canvas);
    }

    // Draw frame for WINDOW and DIALOG types (skip when fullscreen)
    if ((canvas->type == WINDOW || canvas->type == DIALOG) && !canvas->fullscreen) {
        XRenderColor frame_color = canvas->active ? BLUE : GRAY;

        // top border   
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &frame_color, 0, 0, canvas->width, BORDER_HEIGHT_TOP); 
        
        // Bottom black line of titlebar
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, 0, 19 , canvas->width, 1); 

        // left border 
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &frame_color, 0, BORDER_HEIGHT_TOP, BORDER_WIDTH_LEFT, canvas->height - BORDER_HEIGHT_TOP);  
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, 0, 1, 1, canvas->height - 1);  // Start at y=1 to avoid corner
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, BORDER_WIDTH_LEFT -1, 20, 1, canvas->height); 

        // right border
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &frame_color, canvas->width - BORDER_WIDTH_RIGHT, BORDER_HEIGHT_TOP, BORDER_WIDTH_RIGHT, canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM);  
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, canvas->width - BORDER_WIDTH_RIGHT, 20, 1, canvas->height); 
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -1, 0, 1, canvas->height); 

        // bottom border
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &frame_color, 1, canvas->height - BORDER_HEIGHT_BOTTOM, canvas->width -2 , BORDER_HEIGHT_BOTTOM);
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, BORDER_WIDTH_LEFT, canvas->height - BORDER_HEIGHT_BOTTOM, canvas->width -9 , 1);  
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, 0, canvas->height - 1, canvas->width, 1); 

        // top border, close button 
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, 29, 1 , 1, BORDER_HEIGHT_TOP-1); 
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, 30, 1 , 1, BORDER_HEIGHT_TOP-2);
        
        // Draw close button with its portion of the white line
        if (canvas->close_armed) {
            // Sunken effect - black top line instead of white
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, 0, 0, 30, 1);   // Top black line
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, 0, 1, 1, 18);   // Left edge
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, 29, 1, 1, 18);  // Right edge
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, 1, 18, 28, 1);  // Bottom edge
        } else {
            // Normal state - draw white line for this button area
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, 0, 0, 30, 1);   // Top white line
        }
        
        // Draw button interior (white square)
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, 11, 6 , 8, 8); 
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, 12, 7 , 6, 6);
        
        // Title area white line (between close button and right-side buttons)
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, 30, 0, canvas->width - 91 - 30, 1);

        // top border, lower button 
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -31, 1 , 1, BORDER_HEIGHT_TOP-1); 
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, canvas->width -30, 1 , 1, BORDER_HEIGHT_TOP-2);
        
        if (canvas->lower_armed) {
            // Sunken effect - black top line instead of white
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -31, 0, 31, 1);   // Top black line
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -31, 1, 1, 18);   // Left edge
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, canvas->width -2, 1, 1, 18);    // Right edge
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, canvas->width -30, 18, 28, 1);  // Bottom edge
        } else {
            // Normal state - draw white line for this button area
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, canvas->width -31, 0, 31, 1);   // Top white line
        }
        
        // Draw button graphics
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -25, 4 , 15, 8); 
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &GRAY,  canvas->width -24, 5 , 13, 6); 
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -20, 7 , 15, 8); 
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, canvas->width -19, 8 , 13, 6); 
        
        // top border, maximize button 
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -61, 1 , 1, BORDER_HEIGHT_TOP-1); 
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, canvas->width -60, 1 , 1, BORDER_HEIGHT_TOP-2);
        
        if (canvas->maximize_armed) {
            // Sunken effect - black top line instead of white
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -61, 0, 30, 1);   // Top black line
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -61, 1, 1, 18);   // Left edge
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, canvas->width -32, 1, 1, 18);   // Right edge
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, canvas->width -60, 18, 28, 1);  // Bottom edge
        } else {
            // Normal state - draw white line for this button area
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, canvas->width -61, 0, 30, 1);   // Top white line
        }
        
        // Draw button graphics
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -53, 4 , 16, 11); 
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &frame_color, canvas->width -52, 5 , 14, 9);
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -52, 5 , 8, 6); 
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, canvas->width -51, 5 , 5, 5); 
        
        // top border, iconify button
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -91, 1 , 1, BORDER_HEIGHT_TOP-1); 
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, canvas->width -90, 1 , 1, BORDER_HEIGHT_TOP-2);
        
        if (canvas->iconify_armed) {
            // Sunken effect - black top line instead of white
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -91, 0, 30, 1);   // Top black line
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -91, 1, 1, 18);   // Left edge
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, canvas->width -62, 1, 1, 18);   // Right edge
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, canvas->width -90, 18, 28, 1);  // Bottom edge
        } else {
            // Normal state - draw white line for this button area
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, canvas->width -91, 0, 30, 1);   // Top white line
        }
        
        // Draw button graphics
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -83, 4 , 16, 11); 
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest,&frame_color,canvas->width-82, 5 , 14, 9); 
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -82, 10, 6, 5 );
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, canvas->width -82, 11, 5, 3 );
        
        // Draw scrollbar arrows for workbench windows only (skip for dialogs)
        if (canvas->type == WINDOW && canvas->client_win == None && !canvas->disable_scrollbars) {
            draw_vertical_scrollbar_arrows(ctx->dpy, dest, canvas);
        }

        // Draw resize button/handle in bottom-right corner
        draw_resize_button(ctx->dpy, dest, canvas);

        // Draw horizontal scrollbar arrows for workbench windows only (skip for dialogs)
        if (canvas->type == WINDOW && canvas->client_win == None && !canvas->disable_scrollbars) {
            draw_horizontal_scrollbar_arrows(ctx->dpy, dest, canvas);
        }

        // ==================
        // draw windows title (skip when fullscreen)
        // ==================
        if (canvas->title ) {

            XftColor text_col;
            if (canvas->active) {
                text_col.color = WHITE; 
            } else {
                text_col.color = BLACK; 
            }
            // case for workbench windows
            if (font) {
                if (canvas->client_win == None) {
                    XftDraw *draw = XftDrawCreate(ctx->dpy, canvas->canvas_buffer, canvas->visual, canvas->colormap);
                    int text_y = (BORDER_HEIGHT_TOP + font->ascent - font->descent) / 2 + font->descent;
                    XftDrawStringUtf8(draw, &text_col, font, 50, text_y-4, (FcChar8 *)canvas->title, strlen(canvas->title));  // Draw title text.
                    XftDrawDestroy(draw);
                }
                // case for client windows
                if (canvas->client_win != None){
                    XftDraw *draw = XftDrawCreate(ctx->dpy, canvas->win, canvas->visual, canvas->colormap);
                    int text_y = (BORDER_HEIGHT_TOP + font->ascent - font->descent) / 2 + font->descent;
                    XftDrawStringUtf8(draw, &text_col, font, 50, text_y-4, (FcChar8 *)canvas->title, strlen(canvas->title));  // Draw title text.
                    XftDrawDestroy(draw);
                }
            }
        }
        // =============
        // slider knob 
        // =============
        if (canvas->type == WINDOW && canvas->client_win == None) {
            XRenderColor color1, color2; 
            XRenderColor knob_color = canvas->active ? BLUE : GRAY;
            if (canvas->active) {
                color1 = BLUE;   // Active: blue and black checkerboard
                color2 = BLACK;
            } else {
                color1 = BLACK;  // Inactive: black and gray checkerboard
                color2 = GRAY;
            }

            // Vertical scrollbar track 
            //if (canvas->max_scroll_y > 0) {
            int sb_x = canvas->width - BORDER_WIDTH_RIGHT + 4;  // Inside border
            int sb_y = BORDER_HEIGHT_TOP + 10;  // Start at y=10
            int sb_w = BORDER_WIDTH_RIGHT - 8;  // Inside border width (
            int sb_h = (canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM) - 54 - 10;   //  full track height
            draw_checkerboard(ctx->dpy, dest, sb_x, sb_y, sb_w, sb_h, color1, color2);          // Draw checkerboard track

            // Knob on top (use sb_h for calculation)
            float ratio = (float)sb_h / (canvas->content_height > 0 ? canvas->content_height : sb_h);  // Avoid division by zero
            int knob_h = (canvas->max_scroll_y > 0) ? max(MIN_KNOB_SIZE, (int)(ratio * sb_h)) : sb_h;  // Full height if no scroll
            float pos_ratio = (canvas->max_scroll_y > 0) ? (float)canvas->scroll_y / canvas->max_scroll_y : 0.0f;
            int knob_y = sb_y + (int)(pos_ratio * (sb_h - knob_h));

            // vertical knob decoration
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &knob_color, sb_x, knob_y, sb_w, knob_h);   // active color fill
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, sb_x-1, knob_y-1, 1, knob_h+2);     // white vertical outline 
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, sb_x, knob_y-1, sb_w, 1);           // white horizontal outline
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, sb_x+sb_w, knob_y-1, 1, knob_h+2);  // black vertical outline 
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, sb_x, knob_y+knob_h, sb_w, 1);      // black horizontal outline

            // Horizontal scrollbar track - use actual window dimensions like vertical scrollbar
            int hb_x = BORDER_WIDTH_LEFT + 10;  // TRACK_MARGIN = 10
            int hb_y = canvas->height - BORDER_HEIGHT_BOTTOM + 4;  // Inside border
            int hb_w = (canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT) - 54 - 10;  // TRACK_RESERVED=54, TRACK_MARGIN=10
            int hb_h = BORDER_HEIGHT_BOTTOM - 8;  // Inside border height
            draw_checkerboard(ctx->dpy, dest, hb_x, hb_y+1, hb_w, hb_h, color1, color2);    // Draw checkerboard track

            // Knob on top (calculate ratio exactly like vertical scrollbar)
            float h_ratio = (float)hb_w / (canvas->content_width > 0 ? canvas->content_width : hb_w);  // Avoid division by zero
            int knob_w = (canvas->max_scroll_x > 0) ? max(MIN_KNOB_SIZE, (int)(h_ratio * hb_w)) : hb_w;  // Full width if no scroll
            pos_ratio = (canvas->max_scroll_x > 0) ? (float)canvas->scroll_x / canvas->max_scroll_x : 0.0f;
            int knob_x = hb_x + (int)(pos_ratio * (hb_w - knob_w));

            //horizontal knob decoration
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &knob_color, knob_x, hb_y, knob_w, hb_h);       // active color fill
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, knob_x-1, hb_y, 1, hb_h);               // white vertical outline 
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, knob_x-1, hb_y , knob_w, 1);            // white horizintal outline 
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, knob_x+knob_w-1, hb_y, 1, hb_h+1);      // black vertical outline 
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, knob_x, canvas->height-4, knob_w, 1); // black horizintal outline 
        }
    }



    // Composite buffer to window for non-client frames
    if (!is_client_frame) {
        // During interactive resize, use buffer dimensions to avoid copying outside buffer bounds
        int copy_width = canvas->resizing_interactive ? canvas->buffer_width : canvas->width;
        int copy_height = canvas->resizing_interactive ? canvas->buffer_height : canvas->height;
        
        // XRenderComposite: Hardware-accelerated image copying/blending
        // Parameters: display, operation, source, mask, destination,
        //            src_x, src_y, mask_x, mask_y, dest_x, dest_y, width, height
        // PictOpSrc means "replace destination with source" (no blending)
        // None for mask means no masking/transparency effects
        // This copies the offscreen buffer to the visible window
        XRenderComposite(ctx->dpy, PictOpSrc, canvas->canvas_render, None, canvas->window_render, 0, 0, 0, 0, 0, 0, copy_width, copy_height);
    }
    // XFlush removed - let X11 batch operations for better performance
}

// Destroy pixmap and XRender Pictures attached to a canvas
void render_destroy_canvas_surfaces(Canvas *canvas) {
    if (!canvas) return;
    RenderContext *ctx = get_render_context();
    if (!ctx) return;
    
    // Ensure all pending operations complete before cleanup
    XSync(ctx->dpy, False);
    
    if (canvas->canvas_render != None) {
        XRenderFreePicture(ctx->dpy, canvas->canvas_render);
        canvas->canvas_render = None;
    }
    if (canvas->window_render != None) {
        XRenderFreePicture(ctx->dpy, canvas->window_render);
        canvas->window_render = None;
    }
    if (canvas->canvas_buffer != None) {
        XFreePixmap(ctx->dpy, canvas->canvas_buffer);
        canvas->canvas_buffer = None;
    }
    
    // Final sync to ensure cleanup is complete
    XSync(ctx->dpy, False);
}

// Recreate pixmap and XRender Pictures based on current canvas size/visual
void render_recreate_canvas_surfaces(Canvas *canvas) {
    if (!canvas) return;
    RenderContext *ctx = get_render_context();
    if (!ctx) return;

    if (canvas->width <= 0 || canvas->height <= 0) return;


    // Free existing resources first
    render_destroy_canvas_surfaces(canvas);

    // Use buffer dimensions if they're larger (for resize), otherwise use canvas size
    int buffer_width = (canvas->buffer_width > canvas->width) ? canvas->buffer_width : canvas->width;
    int buffer_height = (canvas->buffer_height > canvas->height) ? canvas->buffer_height : canvas->height;
    
    // Update buffer dimensions (preserve larger dimensions during resize)
    canvas->buffer_width = buffer_width;
    canvas->buffer_height = buffer_height;

    // Create offscreen pixmap using buffer dimensions
    // XCreatePixmap: Creates an offscreen drawable (like a canvas in memory)
    // We draw everything to this invisible pixmap first, then copy to window
    // This prevents flickering - technique called "double buffering"
    canvas->canvas_buffer = XCreatePixmap(ctx->dpy, canvas->win,
        buffer_width, buffer_height, canvas->depth);
    if (!canvas->canvas_buffer) return;

    // Picture format for the offscreen buffer
    XRenderPictFormat *fmt = XRenderFindVisualFormat(ctx->dpy, canvas->visual);
    if (!fmt) { render_destroy_canvas_surfaces(canvas); return; }

    // Create XRender Picture for the offscreen buffer
    // This wraps our pixmap with rendering capabilities (compositing, transforms)
    // The 0 and NULL mean "no special attributes" - we use defaults
    canvas->canvas_render = XRenderCreatePicture(ctx->dpy, canvas->canvas_buffer, fmt, 0, NULL);
    if (!canvas->canvas_render) { render_destroy_canvas_surfaces(canvas); return; }

    // For the on-screen window picture, desktop uses root visual
    Visual *win_visual = (canvas->type == DESKTOP)
        ? DefaultVisual(ctx->dpy, DefaultScreen(ctx->dpy))
        : canvas->visual;
    XRenderPictFormat *wfmt = XRenderFindVisualFormat(ctx->dpy, win_visual);
    if (!wfmt) { render_destroy_canvas_surfaces(canvas); return; }

    // Create XRender Picture for the actual window
    // This is our destination for compositing - what the user sees
    // Desktop windows use root visual, others use their own visual
    canvas->window_render = XRenderCreatePicture(ctx->dpy, canvas->win, wfmt, 0, NULL);
    if (!canvas->window_render) { render_destroy_canvas_surfaces(canvas); return; }
}