// File: rnd_canvas.c
// Main canvas rendering orchestrator
// Draws complete canvas contents: backgrounds, icons, menus, dialogs, frames, scrollbars
#define _POSIX_C_SOURCE 200809L
#include "rnd_internal.h"  // For widget drawing functions and rnd_public.h
#include "../intuition/itn_public.h"  // For is_restarting()
#include "../workbench/wb_public.h"
#include "../workbench/wb_internal.h"
#include "../config.h"
#include "../amiwbrc.h"  // For config access
#include "../menus/menu_public.h"
#include "../menus/menu_internal.h"  // For menu_addon_render_all
#include "../dialogs/dialog_public.h"
#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>
#include <X11/Xft/Xft.h>
#include <unistd.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../font_manager.h"

// Lifecycle and font management moved to render/rnd_core.c

// ============================================================================
// Static Rendering Helpers (Private to this module)
// ============================================================================

// Render canvas background (wallpaper or solid fill)
// Returns true if wallpaper was applied, false if solid fill was used
static bool render_background(Canvas *canvas, RenderContext *ctx, Picture dest) {
    // During interactive resize, use buffer dimensions
    int render_width = canvas->resizing_interactive ?
                      canvas->buffer_width : canvas->width;
    int render_height = canvas->resizing_interactive ?
                       canvas->buffer_height : canvas->height;

    // Select wallpaper based on canvas type
    Picture wallpaper_picture = None;
    if (canvas->type == DESKTOP && ctx->desk_picture != None) {
        wallpaper_picture = ctx->desk_picture;
    } else if (canvas->type == WINDOW && canvas->view_mode == VIEW_ICONS &&
               ctx->wind_picture != None) {
        wallpaper_picture = ctx->wind_picture;
    }

    // Apply wallpaper if available
    if (wallpaper_picture != None) {
        XRenderComposite(ctx->dpy, PictOpSrc, wallpaper_picture, None,
                        canvas->canvas_render, 0, 0, 0, 0, 0, 0,
                        render_width, render_height);
        return true;
    }

    // Fallback to solid fill
    XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &canvas->bg_color,
                        0, 0, render_width, render_height);
    return false;
}

// Composite offscreen buffer to visible window
static void composite_to_window(Canvas *canvas, RenderContext *ctx) {
    // During interactive resize, use buffer dimensions
    int copy_width = canvas->resizing_interactive ?
                    canvas->buffer_width : canvas->width;
    int copy_height = canvas->resizing_interactive ?
                     canvas->buffer_height : canvas->height;

    // Hardware-accelerated copy from offscreen buffer to window
    XRenderComposite(ctx->dpy, PictOpSrc, canvas->canvas_render, None,
                    canvas->window_render, 0, 0, 0, 0, 0, 0,
                    copy_width, copy_height);
}

// Render a single icon row in list view
static void render_list_view_row(Canvas *canvas, RenderContext *ctx, Picture dest,
                                 FileIcon *icon, XftFont *font, int render_y, int row_h,
                                 int max_row_w, XftColor *white_col, XftColor *normal_col) {
    // Compute text width
    const char *label = icon->label ? icon->label : "";
    XGlyphInfo ext;
    XftTextExtentsUtf8(ctx->dpy, font, (FcChar8 *)label, strlen(label), &ext);
    int sel_w = min(ext.xOff + 10, max_row_w);

    // Background fill
    XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &canvas->bg_color,
                        BORDER_WIDTH_LEFT, render_y, max_row_w, row_h);

    // Selection highlight
    if (icon->selected) {
        int sel_x = BORDER_WIDTH_LEFT - canvas->scroll_x;
        int clip_x = max(BORDER_WIDTH_LEFT, sel_x);
        int clip_w = min(BORDER_WIDTH_LEFT + max_row_w, sel_x + sel_w) - clip_x;
        if (clip_w > 0) {
            XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLUE,
                                clip_x, render_y, clip_w, row_h);
        }
    }

    // Text rendering - use cached colors
    bool is_dir = (icon->type == TYPE_DRAWER);
    XftColor *color = (icon->selected || is_dir) ? white_col : normal_col;
    int baseline = render_y + font->ascent + 3;
    int text_x = BORDER_WIDTH_LEFT + 6 - canvas->scroll_x;
    XftDrawStringUtf8(canvas->xft_draw, color, font, text_x, baseline,
                     (FcChar8*)label, strlen(label));
}

// Render icons in list view (VIEW_NAMES)
static void render_icons_list_view(Canvas *canvas, RenderContext *ctx,
                                   Picture dest, FileIcon **icon_array,
                                   int icon_count, int view_bottom) {
    XftFont *font = get_font();
    if (!font || !canvas->xft_draw) return;

    // Cache XftColors outside the loop (performance optimization)
    XftColor white_col, normal_col;
    XftColorAllocValue(ctx->dpy, canvas->visual, canvas->colormap, &WHITE, &white_col);
    XftColorAllocValue(ctx->dpy, canvas->visual, canvas->colormap, &WINFONTCOL, &normal_col);

    int row_h = font->ascent + font->descent + 6;
    int max_row_w = canvas->width - BORDER_WIDTH_LEFT -
                   (canvas->client_win == None ? BORDER_WIDTH_RIGHT :
                    BORDER_WIDTH_RIGHT_CLIENT);

    for (int i = 0; i < icon_count; i++) {
        FileIcon *icon = icon_array[i];
        if (!icon || icon->display_window != canvas->win) continue;

        int render_y = BORDER_HEIGHT_TOP + icon->y - canvas->scroll_y;

        // Viewport clipping
        if (render_y > BORDER_HEIGHT_TOP + (view_bottom - canvas->scroll_y))
            continue;
        if (render_y + row_h < BORDER_HEIGHT_TOP) continue;

        render_list_view_row(canvas, ctx, dest, icon, font, render_y, row_h, max_row_w,
                            &white_col, &normal_col);
    }

    // Free cached colors once after loop
    XftColorFree(ctx->dpy, canvas->visual, canvas->colormap, &white_col);
    XftColorFree(ctx->dpy, canvas->visual, canvas->colormap, &normal_col);
}

// Render icons in grid view (VIEW_ICONS)
static void render_icons_grid_view(Canvas *canvas, FileIcon **icon_array,
                                   int icon_count, int view_left,
                                   int view_right, int view_top,
                                   int view_bottom) {
    XftFont *font = get_font();

    for (int i = 0; i < icon_count; i++) {
        FileIcon *icon = icon_array[i];
        if (icon->display_window != canvas->win) continue;

        // Calculate label width
        int label_width = 0;
        if (icon->label && font) {
            XGlyphInfo extents;
            XftTextExtentsUtf8(get_render_context()->dpy, font,
                              (FcChar8 *)icon->label, strlen(icon->label),
                              &extents);
            label_width = extents.xOff;
        }

        // Icon bounding box includes label
        int icon_left = icon->x;
        int icon_right = icon->x + max(icon->width, label_width);
        int icon_top = icon->y;
        int icon_bottom = icon->y + icon->height +
                         (font ? font->ascent + 4 : 20);

        // Viewport clipping - skip off-screen icons
        if (icon_right < view_left || icon_left > view_right ||
            icon_bottom < view_top || icon_top > view_bottom) {
            continue;
        }

        render_icon(icon, canvas);
    }
}

// Render menu content (dropdown or menubar)
static void render_menu_content(Canvas *canvas, RenderContext *ctx) {
    Menu *menu = get_menu_by_canvas(canvas);
    if (!menu) return;

    // Safety check - font might be NULL during shutdown/restart
    XftFont *font = font_manager_get();
    if (!font) return;

    // Defensive check - validate menu data before rendering
    if (!menu->items || menu->item_count == 0) {
        log_error("[WARNING] Menu render: NULL or empty items! items=%p, count=%d",
                 menu->items, menu->item_count);
        return;
    }

    // Use cached XftDraw instead of creating a new one
    if (!canvas->xft_draw) {
        log_error("[WARNING] No cached XftDraw for menu rendering");
        return;
    }

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
        if (!label) {
            log_error("[WARNING] Menu render: NULL item at index %d/%d", i, menu->item_count);
            continue;
        }

        // Check if this menu item should show a checkmark
        bool has_checkmark = (menu->checkmarks && menu->checkmarks[i]);

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
            XftDrawStringUtf8(canvas->xft_draw, &item_fg, font, x + 10, y_base, (FcChar8 *)label, strlen(label));
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
            
            // Check if we need to add " [WB]" suffix for window list menu
            char display_label[256];
            if (menu->window_refs && menu->window_refs[i]) {
                Canvas *win = menu->window_refs[i];
                if (win->client_win == None) {  // Workbench window
                    // Max 20 chars total: need room for " [WB]" (5 chars)
                    if (strlen(label) > 15) {
                        // Smart truncation: try to break at word boundary
                        int cut_pos = 13;  // Default position (13 + 2 for ".." + 5 for " [WB]" = 20)
                        for (int j = 13; j >= 9; j--) {
                            if (label[j] == ' ' || label[j] == '_' || label[j] == '-') {
                                cut_pos = j;
                                break;
                            }
                        }
                        snprintf(display_label, sizeof(display_label), "%.*s.. [WB]", cut_pos, label);
                        // cut_pos chars + ".." (2) + " [WB]" (5) = exactly 20 total
                    } else {
                        snprintf(display_label, sizeof(display_label), "%s [WB]", label);
                    }
                } else {  // Client window
                    // Full 20 chars for clients (no suffix)
                    if (strlen(label) > 20) {
                        // Smart truncation for clients too
                        int cut_pos = 18;  // Default position (18 + 2 for ".." = 20)
                        for (int j = 18; j >= 14; j--) {
                            if (label[j] == ' ' || label[j] == '_' || label[j] == '-') {
                                cut_pos = j;
                                break;
                            }
                        }
                        snprintf(display_label, sizeof(display_label), "%.*s..", cut_pos, label);
                        // cut_pos chars + ".." (2) = exactly 20 total
                    } else {
                        snprintf(display_label, sizeof(display_label), "%s", label);
                    }
                }
                XftDrawStringUtf8(canvas->xft_draw, &item_fg, font, 10, item_y + y_base, (FcChar8 *)display_label, strlen(display_label));
            } else {
                // Regular menu item, no modification
                XftDrawStringUtf8(canvas->xft_draw, &item_fg, font, 10, item_y + y_base, (FcChar8 *)label, strlen(label));
            }

            // Draw checkmark after label if this item has one
            if (has_checkmark) {
                XGlyphInfo label_extents;
                XftTextExtentsUtf8(ctx->dpy, font, (FcChar8 *)label, strlen(label), &label_extents);

                // Position checkmark 1 character space after the label
                int checkmark_x = 10 + label_extents.xOff + 10;  // 10px gap after label
                XftDrawStringUtf8(canvas->xft_draw, &item_fg, font, checkmark_x, item_y + y_base,
                                (FcChar8 *)CHECKMARK, strlen(CHECKMARK));
            }

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
                XftDrawStringUtf8(canvas->xft_draw, &item_fg, font, shortcut_x, item_y + y_base, (FcChar8 *)shortcut_text, strlen(shortcut_text));
            }
            
            // Draw ">>" indicator for items that have submenus
            if (menu->submenus && menu->submenus[i]) {
                const char *submenu_indicator = ">>";
                XGlyphInfo indicator_extents;
                XftTextExtentsUtf8(ctx->dpy, font, (FcChar8 *)submenu_indicator, strlen(submenu_indicator), &indicator_extents);
                
                // Right-align indicator with same padding as shortcuts
                int indicator_x = canvas->width - indicator_extents.xOff - 10;  // 10 pixels padding from right
                XftDrawStringUtf8(canvas->xft_draw, &item_fg, font, indicator_x, item_y + y_base, (FcChar8 *)submenu_indicator, strlen(submenu_indicator));
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
}

// Render menubar addons and menu button (logo mode only)
static void render_menubar_addons(Canvas *canvas, RenderContext *ctx) {
    // Render all menu addons (clock, CPU, RAM, etc.)
    int addon_x = 10;  // Start position for addons (left-to-right)
    menu_addon_render_all(ctx, canvas, &addon_x, 0);

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


// Render window title in titlebar
static void render_window_title(Canvas *canvas, RenderContext *ctx, Picture dest) {
    // Properly allocate XftColor for title text
    XftColor text_col;
    XRenderColor render_color = canvas->active ? WHITE : BLACK;
    XftColorAllocValue(ctx->dpy, canvas->visual, canvas->colormap, &render_color, &text_col);

    // Get unified font for title drawing
    XftFont *title_font = get_font();
    if (title_font) {
        // Determine which title to display: title_change if set, otherwise title_base
        const char *display_title = canvas->title_change ? canvas->title_change : canvas->title_base;
        if (!display_title) display_title = "Untitled";  // Fallback if both are NULL

        int text_y = (BORDER_HEIGHT_TOP + title_font->ascent - title_font->descent) / 2 + title_font->descent;

        if (canvas->client_win == None) {
            // Workbench windows: draw to buffer using cached XftDraw
            if (canvas->xft_draw) {
                XftDrawStringUtf8(canvas->xft_draw, &text_col, title_font, 50, text_y-4,
                                (FcChar8 *)display_title, strlen(display_title));
            }
        } else {
            // Client windows: draw directly to window, not buffer
            // Create cached XftDraw for direct window drawing if needed
            if (!canvas->xft_draw) {
                canvas->xft_draw = XftDrawCreate(ctx->dpy, canvas->win, canvas->visual, canvas->colormap);
            }
            if (canvas->xft_draw) {
                XftDrawStringUtf8(canvas->xft_draw, &text_col, title_font, 50, text_y-4,
                                (FcChar8 *)display_title, strlen(display_title));
            }
        }
        XftColorFree(ctx->dpy, canvas->visual, canvas->colormap, &text_col);
    }
}

// Render vertical and horizontal scrollbar knobs
static void render_scrollbar_knobs(Canvas *canvas, RenderContext *ctx, Picture dest) {
    XRenderColor knob_color = canvas->active ? BLUE : GRAY;
    XRenderColor color1 = canvas->active ? BLUE : BLACK;
    XRenderColor color2 = canvas->active ? BLACK : GRAY;

    // Vertical scrollbar
    int sb_x = canvas->width - BORDER_WIDTH_RIGHT + 4;
    int sb_y = BORDER_HEIGHT_TOP + 10;
    int sb_w = BORDER_WIDTH_RIGHT - 8;
    int sb_h = (canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM) - 54 - 10;
    draw_checkerboard(ctx->dpy, dest, sb_x, sb_y, sb_w, sb_h, color1, color2);

    float ratio = (float)sb_h / (canvas->content_height > 0 ? canvas->content_height : sb_h);
    int knob_h = (canvas->max_scroll_y > 0) ? max(MIN_KNOB_SIZE, (int)(ratio * sb_h)) : sb_h;
    float pos_ratio = (canvas->max_scroll_y > 0) ? (float)canvas->scroll_y / canvas->max_scroll_y : 0.0f;
    int knob_y = sb_y + (int)(pos_ratio * (sb_h - knob_h));

    XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &knob_color, sb_x, knob_y, sb_w, knob_h);
    XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, sb_x-1, knob_y-1, 1, knob_h+2);
    XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, sb_x, knob_y-1, sb_w, 1);
    XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, sb_x+sb_w, knob_y-1, 1, knob_h+2);
    XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, sb_x, knob_y+knob_h, sb_w, 1);

    // Horizontal scrollbar
    int hb_x = BORDER_WIDTH_LEFT + 10;
    int hb_y = canvas->height - BORDER_HEIGHT_BOTTOM + 4;
    int hb_w = (canvas->width - BORDER_WIDTH_LEFT - (canvas->client_win == None ? BORDER_WIDTH_RIGHT : BORDER_WIDTH_RIGHT_CLIENT)) - 54 - 10;
    int hb_h = BORDER_HEIGHT_BOTTOM - 8;
    draw_checkerboard(ctx->dpy, dest, hb_x, hb_y+1, hb_w, hb_h, color1, color2);

    float h_ratio = (float)hb_w / (canvas->content_width > 0 ? canvas->content_width : hb_w);
    int knob_w = (canvas->max_scroll_x > 0) ? max(MIN_KNOB_SIZE, (int)(h_ratio * hb_w)) : hb_w;
    float h_pos_ratio = (canvas->max_scroll_x > 0) ? (float)canvas->scroll_x / canvas->max_scroll_x : 0.0f;
    int knob_x = hb_x + (int)(h_pos_ratio * (hb_w - knob_w));

    XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &knob_color, knob_x, hb_y, knob_w, hb_h);
    XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, knob_x-1, hb_y, 1, hb_h);
    XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, knob_x-1, hb_y , knob_w, 1);
    XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, knob_x+knob_w-1, hb_y, 1, hb_h+1);
    XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &BLACK, knob_x, canvas->height-4, knob_w, 1);
}


// Dispatch rendering based on canvas type
static void render_canvas_content(Canvas *canvas, RenderContext *ctx, Picture dest,
                                  bool is_client_frame) {
    // Render icons for desktop and window canvases
    if (!is_client_frame && !canvas->scanning &&
        (canvas->type == DESKTOP || canvas->type == WINDOW)) {
        FileIcon **icon_array = wb_icons_array_get();
        int icon_count = wb_icons_array_count();

        // Compute visible viewport bounds
        int view_left = canvas->scroll_x;
        int view_top = canvas->scroll_y;
        int view_right = view_left + (canvas->width - BORDER_WIDTH_LEFT -
            (canvas->client_win == None ? BORDER_WIDTH_RIGHT : BORDER_WIDTH_RIGHT_CLIENT));
        int view_bottom = view_top + (canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM);

        // Dispatch to list or grid view
        if (canvas->type == WINDOW && canvas->view_mode == VIEW_NAMES) {
            render_icons_list_view(canvas, ctx, dest, icon_array, icon_count, view_bottom);
        } else {
            render_icons_grid_view(canvas, icon_array, icon_count,
                                  view_left, view_right, view_top, view_bottom);
        }
    }

    // Render menu content
    if (canvas->type == MENU) {
        render_menu_content(canvas, ctx);
        if (canvas == get_menubar() && !get_show_menus_state()) {
            render_menubar_addons(canvas, ctx);
        }
        return;
    }

    // Render dialog content - dispatch to specialized dialog renderers
    if (canvas->type == DIALOG) {
        if (wb_progress_monitor_is_canvas(canvas)) {
            wb_progress_monitor_render(canvas);
        } else if (is_iconinfo_canvas(canvas)) {
            render_iconinfo_content(canvas);
        } else {
            render_dialog_content(canvas);
        }
        return;
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
        // Dialogs and client windows use 8px border, workbench windows use 20px
        int right_border_width = (canvas->type == DIALOG || canvas->client_win != None) ? BORDER_WIDTH_RIGHT_CLIENT : BORDER_WIDTH_RIGHT;
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &frame_color, canvas->width - right_border_width, BORDER_HEIGHT_TOP, right_border_width, canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM);
        XRenderFillRectangle(ctx->dpy, PictOpSrc, dest, &WHITE, canvas->width - right_border_width, 20, 1, canvas->height);
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

        // Draw window title
        if (canvas->title_base || canvas->title_change) {
            render_window_title(canvas, ctx, dest);
        }

        // Draw scrollbar knobs for workbench windows
        if (canvas->type == WINDOW && canvas->client_win == None) {
            render_scrollbar_knobs(canvas, ctx, dest);
        }
    }
}
void redraw_canvas(Canvas *canvas) {
    if (!canvas || canvas->width <= 0 || canvas->height <= 0 ||
        canvas->canvas_render == None || canvas->window_render == None) {
        log_error("[REDRAW] Early return: canvas=%p, width=%d, height=%d, canvas_render=%lu, window_render=%lu\n",
               (void*)canvas, canvas ? canvas->width : -1, canvas ? canvas->height : -1,
               canvas ? canvas->canvas_render : 0, canvas ? canvas->window_render : 0);
        return;
    }


    // PERFORMANCE OPTIMIZATION: During interactive resize, only redraw the canvas being resized
    // BUT: Always allow icon rendering - just skip buffer updates
    Canvas *resizing = itn_resize_get_target();
    if (resizing && canvas != resizing) {
        return; // Skip redrawing non-resizing windows during resize
    }

    RenderContext *ctx = get_render_context();
    if (!ctx) return;
    
    // Skip validation - canvas lifecycle is properly managed
    // This eliminates a synchronous X11 call on every redraw

    // If canvas is WINDOW type with a client window,
    // dest is window_render; otherwise, canvas_render.
    // Note: DIALOG windows use canvas_render, not window_render
    bool is_client_frame = (canvas->type == WINDOW && canvas->client_win != None);
    Picture dest = is_client_frame ? canvas->window_render : canvas->canvas_render;


    // Render background for non-client canvas (desktop or empty windows)
    if (!is_client_frame) {
        render_background(canvas, ctx, dest);

    }
    
    // Render canvas content (icons, menus, dialogs, decorations)
    render_canvas_content(canvas, ctx, dest, is_client_frame);

    // Composite buffer to window for non-client frames
    if (!is_client_frame) {
        composite_to_window(canvas, ctx);
    }
}
