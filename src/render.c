// File: render.c
#define _POSIX_C_SOURCE 200809L
#include "render.h"
#include "intuition.h"
#include "workbench.h"
#include "config.h"
#include "menus.h"
#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>
#include <X11/Xft/Xft.h>
#include <fontconfig/fontconfig.h>
#include <unistd.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>

// Static Xft resources
static XftFont *font = NULL;
static XftColor text_color_black;
static XftColor  text_color_white;
#define RESOURCE_DIR_SYSTEM "/usr/local/share/amiwb"  
#define RESOURCE_DIR_USER ".config/amiwb"  

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

// Initialize rendering resources
void init_render(void) {
    RenderContext *ctx = get_render_context();
    if (!ctx) { 
        printf("Failled to get render_context\n");
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
        printf("render_icon: Invalid icon (icon=%p, canvas=%p, picture=%p)\n", 
            (void*)icon, 
            (void*)icon->display_window, 
            (void*)icon->current_picture);
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
    XRenderComposite(ctx->dpy, PictOpOver, icon->current_picture, None, canvas->canvas_render,
                     0, 0, 0, 0, render_x, render_y, icon->width, icon->height);

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

// Redraw entire canvas and its icons
void redraw_canvas(Canvas *canvas) {
    //if (!canvas) return;
    if (!canvas || canvas->width <= 0 || canvas->height <= 0 || 
        canvas->canvas_render == None || canvas->window_render == None) {
        return;
    }
    if (!canvas || canvas->canvas_render == None || 
        canvas->window_render == None) {
        /* skip rendering if resources are invalid */
        return;
    }
    RenderContext *ctx = get_render_context();
    if (!ctx) return;

    // If canvas is WINDOW type with a client window, 
    // dest is window_render; otherwise, canvas_render.
    bool is_client_frame = (canvas->type == WINDOW && 
        canvas->client_win != None);
    bool has_bg_image = false;
    Picture dest = is_client_frame ? canvas->window_render : canvas->canvas_render;

    // set background pictures or fill background for non-client canvas
    // (desktop or empty windows and not clients, not menus)
    if (!is_client_frame) {
        if (canvas->type == DESKTOP && ctx->desk_img != None) {

            Display *dpy = ctx->dpy;
            Visual *visual = DefaultVisual(dpy, DefaultScreen(dpy));
            // Create Picture from bg_pixmap
            XRenderPictFormat *fmt = XRenderFindVisualFormat(ctx->dpy, visual);
            Picture bg_picture = XRenderCreatePicture(dpy, ctx->desk_img, fmt, 0, NULL);
            // Composite (blend) onto canvas; 
            // use PictOpSrc for direct copy or adjust for transparency
            XRenderComposite(dpy, PictOpSrc, bg_picture, None, canvas->canvas_render, 
                0, 0, 0, 0, 0, 0, canvas->width, canvas->height);
            XRenderFreePicture(dpy, bg_picture);
            has_bg_image = true;
        } /*else {
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &canvas->bg_color, 
                0, 0, canvas->width, canvas->height);
        }*/

        // For WINDOWS: only show wallpaper in icon view; names view stays gray
        if (canvas->type == WINDOW && canvas->view_mode == VIEW_ICONS && ctx->wind_img != None) {
            Display *dpy = ctx->dpy;
            Visual *visual = DefaultVisual(dpy, DefaultScreen(dpy));
            XRenderPictFormat *fmt = XRenderFindVisualFormat(ctx->dpy, visual);
            Picture bg_picture = XRenderCreatePicture(dpy, ctx->wind_img, fmt, 0, NULL);
            XRenderComposite(dpy, PictOpSrc, bg_picture, None, canvas->canvas_render,
                             0, 0, 0, 0, 0, 0, canvas->width, canvas->height);
            XRenderFreePicture(dpy, bg_picture);
            has_bg_image = true;
        }

        if (!has_bg_image) {
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &canvas->bg_color, 
                0, 0, canvas->width, canvas->height);
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
        // Render icons
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
        Menu *menu = get_menu_by_canvas(canvas);
        if (!menu) return;

    XftDraw *draw = XftDrawCreate(ctx->dpy, canvas->canvas_buffer, canvas->visual, canvas->colormap);
    if (!draw) return;

    bool is_menubar = (canvas == get_menubar());
    int selected = menu->selected_item;
    int padding = 20;  // Consistent for all items

    int x = 10;  // Aligned start
    int y_base = font->ascent + (MENU_ITEM_HEIGHT - font->height) / 2;

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
            bg_color = (i == selected && menu->submenus) ? (XRenderColor){0x0000, 0x0000, 0x0000, 0xFFFF} : canvas->bg_color;
            fg_color = (i == selected && menu->submenus) ? (XRenderColor){0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF} : (XRenderColor){0x0000, 0x0000, 0x0000, 0xFFFF};
            XRenderFillRectangle(ctx->dpy, PictOpSrc, canvas->canvas_render, &bg_color, x, 0, item_width, MENU_ITEM_HEIGHT);
            XftColor item_fg;
            XftColorAllocValue(ctx->dpy, canvas->visual, canvas->colormap, &fg_color, &item_fg);
            XftDrawStringUtf8(draw, &item_fg, font, x + 10, y_base, (FcChar8 *)label, strlen(label));
            XftColorFree(ctx->dpy, canvas->visual, canvas->colormap, &item_fg);
            x += item_width;
        } else {
            // Vertical (submenu): highlight if selected, regardless of submenus
            bg_color = (i == selected) ? (XRenderColor){0x0000, 0x0000, 0x0000, 0xFFFF} : canvas->bg_color;
            fg_color = (i == selected) ? (XRenderColor){0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF} : (XRenderColor){0x0000, 0x0000, 0x0000, 0xFFFF};
            int item_y = i * MENU_ITEM_HEIGHT;
            XRenderFillRectangle(ctx->dpy, PictOpSrc, canvas->canvas_render, &bg_color, 0, item_y, canvas->width, MENU_ITEM_HEIGHT);
            XftColor item_fg;
            XftColorAllocValue(ctx->dpy, canvas->visual, canvas->colormap, &fg_color, &item_fg);
            XftDrawStringUtf8(draw, &item_fg, font, 10, item_y + y_base, (FcChar8 *)label, strlen(label));
            XftColorFree(ctx->dpy, canvas->visual, canvas->colormap, &item_fg);
        }
    }
    XftDrawDestroy(draw);

        // ============
        // menu button
        // ============
        if (!get_show_menus_state()){
 
            // menu right side, lower button 
            XRenderFillRectangle(ctx->dpy, PictOpSrc, canvas->canvas_render, &GRAY, canvas->width -28, 0 , 26, 20);
            XRenderFillRectangle(ctx->dpy, PictOpSrc, canvas->canvas_render, &WHITE, canvas->width -28, 0 , 26, 1);
            XRenderFillRectangle(ctx->dpy, PictOpSrc, canvas->canvas_render, &BLACK, canvas->width -2, 0 , 1, 20);
            XRenderFillRectangle(ctx->dpy, PictOpSrc, canvas->canvas_render, &BLACK, canvas->width -30, 0 , 1, 20);

            XRenderFillRectangle(ctx->dpy, PictOpSrc, canvas->canvas_render, &BLACK, canvas->width -25, 4 , 15, 8); 
            XRenderFillRectangle(ctx->dpy, PictOpSrc, canvas->canvas_render, &GRAY,  canvas->width -24, 5 , 13, 6); 
            XRenderFillRectangle(ctx->dpy, PictOpSrc, canvas->canvas_render, &BLACK, canvas->width -20, 7 , 15, 8); 
            XRenderFillRectangle(ctx->dpy, PictOpSrc, canvas->canvas_render, &WHITE, canvas->width -19, 8 , 13, 6); 
        
         }
    }

    // Draw frame for WINDOW types
    if (canvas->type == WINDOW ) {
        XRenderColor frame_color = canvas->active ? BLUE : GRAY;

        // top border   
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &frame_color, 0, 0, canvas->width, BORDER_HEIGHT_TOP); 
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, 0, 0, canvas->width, 1); 
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, 0, 19 , canvas->width, 1); 

        // left border 
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &frame_color, 0, BORDER_HEIGHT_TOP, BORDER_WIDTH_LEFT, canvas->height - BORDER_HEIGHT_TOP);  
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, 0, 0, 1, canvas->height); 
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
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, 11, 6 , 8, 8); 
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, 12, 7 , 6, 6); 

        // top border, lower button 
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -31, 1 , 1, BORDER_HEIGHT_TOP-1); 
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, canvas->width -30, 1 , 1, BORDER_HEIGHT_TOP-2); 
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -25, 4 , 15, 8); 
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &GRAY,  canvas->width -24, 5 , 13, 6); 
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -20, 7 , 15, 8); 
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, canvas->width -19, 8 , 13, 6); 
        
        // top border, maximize button 
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -61, 1 , 1, BORDER_HEIGHT_TOP-1); 
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, canvas->width -60, 1 , 1, BORDER_HEIGHT_TOP-2); 
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -53, 4 , 16, 11); 
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &frame_color, canvas->width -52, 5 , 14, 9);
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -52, 5 , 8, 6); 
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, canvas->width -51, 5 , 5, 5); 
        
        // top border, iconify button
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -91, 1 , 1, BORDER_HEIGHT_TOP-1); 
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, canvas->width -90, 1 , 1, BORDER_HEIGHT_TOP-2); 
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -83, 4 , 16, 11); 
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest,&frame_color,canvas->width-82, 5 , 14, 9); 
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -82, 10, 6, 5 );
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, canvas->width -82, 11, 5, 3 );
        
        // don't draw scroll bar arrows on clients frame
        if (canvas->type == WINDOW && canvas->client_win == None) {
            // right border, arrows separators
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width - BORDER_WIDTH_RIGHT +1, canvas->height - BORDER_HEIGHT_BOTTOM - 1, BORDER_WIDTH_RIGHT, 1); 
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, canvas->width - BORDER_WIDTH_RIGHT +1, canvas->height - BORDER_HEIGHT_BOTTOM - 20, BORDER_WIDTH_RIGHT-2, 1); 
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width - BORDER_WIDTH_RIGHT +1, canvas->height - BORDER_HEIGHT_BOTTOM - 21, BORDER_WIDTH_RIGHT-2, 1); 
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, canvas->width - BORDER_WIDTH_RIGHT +1, canvas->height - BORDER_HEIGHT_BOTTOM - 40, BORDER_WIDTH_RIGHT-2, 1); 
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width - BORDER_WIDTH_RIGHT +1, canvas->height - BORDER_HEIGHT_BOTTOM - 41, BORDER_WIDTH_RIGHT-2, 1);

            // down arrow
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -10, canvas->height - BORDER_HEIGHT_BOTTOM-10, 2,4);
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -12, canvas->height - BORDER_HEIGHT_BOTTOM-12, 2,4);
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -14, canvas->height - BORDER_HEIGHT_BOTTOM-14, 2,4);
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -8, canvas->height - BORDER_HEIGHT_BOTTOM-12, 2,4);
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -6, canvas->height - BORDER_HEIGHT_BOTTOM-14, 2,4);

            // up arrow
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -10, canvas->height - BORDER_HEIGHT_BOTTOM-35, 2,4);
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -12, canvas->height - BORDER_HEIGHT_BOTTOM-33, 2,4);
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -14, canvas->height - BORDER_HEIGHT_BOTTOM-31, 2,4);
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -8, canvas->height - BORDER_HEIGHT_BOTTOM-33, 2,4);
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -6, canvas->height - BORDER_HEIGHT_BOTTOM-31, 2,4);
        }

        // resize button
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, canvas->width - BORDER_WIDTH_RIGHT, canvas->height - BORDER_HEIGHT_BOTTOM, 1, BORDER_HEIGHT_BOTTOM -1); 
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width - BORDER_WIDTH_RIGHT -1, canvas->height - BORDER_HEIGHT_BOTTOM +1, 1, BORDER_HEIGHT_BOTTOM -1); 
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width - BORDER_WIDTH_RIGHT + 5, canvas->height - 5, 11, 1);
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -5, canvas->height - 15, 1, 10);
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width - BORDER_WIDTH_RIGHT + 5, canvas->height - 7, 1, 3);
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -7, canvas->height - 15, 2, 1);
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -8, canvas->height - 14, 1, 1);
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -9, canvas->height - 13, 1, 1);
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -10, canvas->height - 12, 1, 1);
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -11, canvas->height - 11, 1, 1);
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -12, canvas->height - 10, 1, 1);
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -13, canvas->height - 9, 1, 1);
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width -14, canvas->height - 8, 1, 1);
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, canvas->width -7, canvas->height - 14, 2, 9);
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, canvas->width -8, canvas->height - 13, 1, 8);
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, canvas->width -9, canvas->height - 12, 1, 7);
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, canvas->width -10, canvas->height - 11, 1, 6);
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, canvas->width -11, canvas->height - 10, 1, 5);
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, canvas->width -12, canvas->height - 9, 1, 4);
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, canvas->width -13, canvas->height - 8, 1, 3);
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, canvas->width -14, canvas->height - 7, 1, 2);

        // don't draw scroll bar arrows on clients frame
        if (canvas->type == WINDOW && canvas->client_win == None) {
            // bottom border arrows separators
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, canvas->width - BORDER_WIDTH_RIGHT -21, canvas->height - BORDER_HEIGHT_BOTTOM, 1, BORDER_HEIGHT_BOTTOM -1); 
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width - BORDER_WIDTH_RIGHT -22, canvas->height - BORDER_HEIGHT_BOTTOM +1, 1, BORDER_HEIGHT_BOTTOM -1);  
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, canvas->width - BORDER_WIDTH_RIGHT -41, canvas->height - BORDER_HEIGHT_BOTTOM, 1, BORDER_HEIGHT_BOTTOM -1); 
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width - BORDER_WIDTH_RIGHT -42, canvas->height - BORDER_HEIGHT_BOTTOM +1, 1, BORDER_HEIGHT_BOTTOM -1);  

            // right arrow
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width - BORDER_WIDTH_RIGHT-8, canvas->height - BORDER_HEIGHT_BOTTOM+10, 4,2);
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width - BORDER_WIDTH_RIGHT-10, canvas->height - BORDER_HEIGHT_BOTTOM+8, 4,2);
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width - BORDER_WIDTH_RIGHT-12, canvas->height - BORDER_HEIGHT_BOTTOM+6, 4,2);
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width - BORDER_WIDTH_RIGHT-10, canvas->height - BORDER_HEIGHT_BOTTOM+12, 4,2);
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width - BORDER_WIDTH_RIGHT-12, canvas->height - BORDER_HEIGHT_BOTTOM+14, 4,2);

            // left arrow
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width - 40-16, canvas->height - BORDER_HEIGHT_BOTTOM+10, 4,2);
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width - 40-14, canvas->height - BORDER_HEIGHT_BOTTOM+8, 4,2);
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width - 40-12, canvas->height - BORDER_HEIGHT_BOTTOM+6, 4,2);
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width - 40-14, canvas->height - BORDER_HEIGHT_BOTTOM+12, 4,2);
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, canvas->width - 40-12, canvas->height - BORDER_HEIGHT_BOTTOM+14, 4,2);

        }

        // ==================
        // draw windows title
        // ==================
        if (canvas->title ) {

            XftColor text_col;
            if (canvas->active) {
                text_col.color = WHITE; 
            } else {
                text_col.color = BLACK; 
            }
            // case for workbench windows
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

            // Horizontal scrollbar track
            int hb_x = BORDER_WIDTH_LEFT + 10;  // Start at x=10
            int hb_y = canvas->height - BORDER_HEIGHT_BOTTOM + 4;  // Inside border
            int hb_w = (canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT) - 54 - 10;  // stop scroll track before arrows button
            int hb_h = BORDER_HEIGHT_BOTTOM - 8;  // Inside border height
            draw_checkerboard(ctx->dpy, dest, hb_x, hb_y+1, hb_w, hb_h, color1, color2);    // Draw checkerboard track

            // Knob on top (use adjusted hb_w for calculation)
            ratio = (float)hb_w / (canvas->content_width > 0 ? canvas->content_width : hb_w);          // Avoid division by zero
            int knob_w = (canvas->max_scroll_x > 0) ? max(MIN_KNOB_SIZE, (int)(ratio * hb_w)) : hb_w;  // Full width if no scroll
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
        XRenderComposite(ctx->dpy, PictOpSrc, canvas->canvas_render, None, canvas->window_render, 0, 0, 0, 0, 0, 0, canvas->width, canvas->height);
    }
    XFlush(ctx->dpy);
}

// Destroy pixmap and XRender Pictures attached to a canvas
void render_destroy_canvas_surfaces(Canvas *canvas) {
    if (!canvas) return;
    RenderContext *ctx = get_render_context();
    if (!ctx) return;
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
}

// Recreate pixmap and XRender Pictures based on current canvas size/visual
void render_recreate_canvas_surfaces(Canvas *canvas) {
    if (!canvas) return;
    RenderContext *ctx = get_render_context();
    if (!ctx) return;

    // Free existing resources first
    render_destroy_canvas_surfaces(canvas);

    if (canvas->width <= 0 || canvas->height <= 0) return;

    // Create offscreen pixmap matching canvas depth
    canvas->canvas_buffer = XCreatePixmap(ctx->dpy, canvas->win,
        canvas->width, canvas->height, canvas->depth);
    if (!canvas->canvas_buffer) return;

    // Picture format for the offscreen buffer
    XRenderPictFormat *fmt = XRenderFindVisualFormat(ctx->dpy, canvas->visual);
    if (!fmt) { render_destroy_canvas_surfaces(canvas); return; }

    canvas->canvas_render = XRenderCreatePicture(ctx->dpy, canvas->canvas_buffer, fmt, 0, NULL);
    if (!canvas->canvas_render) { render_destroy_canvas_surfaces(canvas); return; }

    // For the on-screen window picture, desktop uses root visual
    Visual *win_visual = (canvas->type == DESKTOP)
        ? DefaultVisual(ctx->dpy, DefaultScreen(ctx->dpy))
        : canvas->visual;
    XRenderPictFormat *wfmt = XRenderFindVisualFormat(ctx->dpy, win_visual);
    if (!wfmt) { render_destroy_canvas_surfaces(canvas); return; }

    canvas->window_render = XRenderCreatePicture(ctx->dpy, canvas->win, wfmt, 0, NULL);
    if (!canvas->window_render) { render_destroy_canvas_surfaces(canvas); return; }
}