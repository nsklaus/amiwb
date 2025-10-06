#include "listview.h"
#include "../toolkit.h"
#include "../toolkit_config.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Calculate scrollbar dimensions
void listview_update_scrollbar(ListView *lv) {
    if (!lv) return;
    
    // Always calculate visible items - needed for drawing
    // Use full height since drawing already handles the y+2 offset
    int content_height = lv->height; 
    lv->visible_items = content_height / LISTVIEW_ITEM_HEIGHT;
    
    // Calculate scrollbar track height (track is separate from arrows)
    int track_height = lv->height - (LISTVIEW_ARROW_HEIGHT * 2) - 2;  // -2 for track borders only
    
    if (lv->item_count == 0 || lv->item_count <= lv->visible_items) {
        // No scrolling needed - knob fills entire track
        lv->scrollbar_knob_height = track_height - 2;  // Account for track borders
        lv->scrollbar_knob_y = 1;  // Start after top border
        return;
    }
    
    // Calculate knob size proportional to visible vs total
    lv->scrollbar_knob_height = (lv->visible_items * track_height) / lv->item_count;
    if (lv->scrollbar_knob_height < 20) lv->scrollbar_knob_height = 20; // Minimum knob size
    
    // Calculate knob position
    int scrollable_items = lv->item_count - lv->visible_items;
    if (scrollable_items > 0) {
        int available_space = track_height - lv->scrollbar_knob_height - 2; // Account for borders
        lv->scrollbar_knob_y = 1 + (lv->scroll_offset * available_space) / scrollable_items;
    } else {
        lv->scrollbar_knob_y = 1;
    }
}

ListView* listview_create(int x, int y, int width, int height) {
    ListView *lv = malloc(sizeof(ListView));
    if (!lv) {
        toolkit_log_error("[ERROR] ListView: Failed to allocate memory (size=%zu)", sizeof(ListView));
        return NULL;
    }
    
    lv->x = x;
    lv->y = y;
    lv->width = width;
    lv->height = height;

    // Initialize dynamic arrays
    lv->capacity = LISTVIEW_INITIAL_CAPACITY;
    lv->items = calloc(lv->capacity, sizeof(ListViewItem));
    lv->selected = calloc(lv->capacity, sizeof(bool));
    if (!lv->items || !lv->selected) {
        free(lv->items);
        free(lv->selected);
        free(lv);
        return NULL;
    }

    lv->item_count = 0;
    lv->selected_index = -1;
    lv->scroll_offset = 0;
    lv->visible_items = 0;

    // Multi-selection support
    lv->multi_select_enabled = false;
    lv->selection_count = 0;
    
    lv->scrollbar_knob_y = 0;
    lv->scrollbar_knob_height = 0;
    lv->scrollbar_dragging = false;
    lv->scrollbar_drag_offset = 0;
    
    lv->on_select = NULL;
    lv->on_double_click = NULL;
    lv->callback_data = NULL;

    // Rendering context (will be cached from xft_draw on first draw)
    lv->visual = NULL;
    lv->colormap = None;

    // Double-click detection
    lv->last_click_time = 0;
    lv->last_click_index = -1;
    
    lv->needs_redraw = true;
    
    listview_update_scrollbar(lv);
    
    return lv;
}

void listview_destroy(ListView *lv) {
    if (lv) {
        free(lv->items);
        free(lv->selected);
        free(lv);
    }
}

void listview_clear(ListView *lv) {
    if (!lv) return;

    // Shrink array if it's too large (but keep a reasonable minimum)
    if (lv->capacity > 64 && lv->item_count < lv->capacity / 4) {
        int new_capacity = lv->capacity / 2;
        if (new_capacity < LISTVIEW_INITIAL_CAPACITY) {
            new_capacity = LISTVIEW_INITIAL_CAPACITY;
        }

        ListViewItem *new_items = realloc(lv->items, new_capacity * sizeof(ListViewItem));
        bool *new_selected = realloc(lv->selected, new_capacity * sizeof(bool));

        if (new_items && new_selected) {
            lv->items = new_items;
            lv->selected = new_selected;
            lv->capacity = new_capacity;
        }
        // If realloc fails, keep the old arrays
    }

    lv->item_count = 0;
    lv->selected_index = -1;
    lv->scroll_offset = 0;
    lv->selection_count = 0;
    memset(lv->selected, 0, lv->capacity * sizeof(bool));
    lv->needs_redraw = true;
    listview_update_scrollbar(lv);
}

void listview_add_item(ListView *lv, const char *text, bool is_directory, void *user_data) {
    if (!lv || !text) return;

    // Grow array if needed
    if (lv->item_count >= lv->capacity) {
        int new_capacity = lv->capacity * 2;
        ListViewItem *new_items = realloc(lv->items, new_capacity * sizeof(ListViewItem));
        bool *new_selected = realloc(lv->selected, new_capacity * sizeof(bool));

        if (!new_items || !new_selected) {
            // Allocation failed - keep original arrays
            if (new_items && new_items != lv->items) free(new_items);
            if (new_selected && new_selected != lv->selected) free(new_selected);
            return;
        }

        lv->items = new_items;
        lv->selected = new_selected;

        // Clear the new selection flags
        memset(&lv->selected[lv->capacity], 0, (new_capacity - lv->capacity) * sizeof(bool));
        lv->capacity = new_capacity;
    }

    strncpy(lv->items[lv->item_count].text, text, 255);
    lv->items[lv->item_count].text[255] = '\0';
    lv->items[lv->item_count].is_directory = is_directory;
    lv->items[lv->item_count].user_data = user_data;
    lv->item_count++;
    lv->needs_redraw = true;
    listview_update_scrollbar(lv);
}

void listview_set_items(ListView *lv, ListViewItem *items, int count) {
    if (!lv || !items) return;

    // Ensure we have enough capacity
    if (count > lv->capacity) {
        int new_capacity = lv->capacity;
        while (new_capacity < count) {
            new_capacity *= 2;
        }

        ListViewItem *new_items = realloc(lv->items, new_capacity * sizeof(ListViewItem));
        bool *new_selected = realloc(lv->selected, new_capacity * sizeof(bool));

        if (!new_items || !new_selected) {
            // Allocation failed - add as many as we can
            if (new_items && new_items != lv->items) free(new_items);
            if (new_selected && new_selected != lv->selected) free(new_selected);
            count = lv->capacity;
        } else {
            lv->items = new_items;
            lv->selected = new_selected;
            memset(&lv->selected[lv->capacity], 0, (new_capacity - lv->capacity) * sizeof(bool));
            lv->capacity = new_capacity;
        }
    }

    lv->item_count = 0;
    for (int i = 0; i < count; i++) {
        lv->items[i] = items[i];
        lv->item_count++;
    }
    lv->needs_redraw = true;
    listview_update_scrollbar(lv);
}

void listview_set_selected(ListView *lv, int index) {
    if (!lv) return;
    if (index >= -1 && index < lv->item_count) {
        lv->selected_index = index;
        lv->needs_redraw = true;
        if (index >= 0) {
            listview_ensure_visible(lv, index);
        }
    }
}

void listview_scroll_to(ListView *lv, int index) {
    if (!lv || index < 0 || index >= lv->item_count) return;
    
    lv->scroll_offset = index;
    if (lv->scroll_offset > lv->item_count - lv->visible_items) {
        lv->scroll_offset = lv->item_count - lv->visible_items;
    }
    if (lv->scroll_offset < 0) lv->scroll_offset = 0;
    
    lv->needs_redraw = true;
    listview_update_scrollbar(lv);
}

void listview_ensure_visible(ListView *lv, int index) {
    if (!lv || index < 0 || index >= lv->item_count) return;
    
    if (index < lv->scroll_offset) {
        listview_scroll_to(lv, index);
    } else if (index >= lv->scroll_offset + lv->visible_items) {
        listview_scroll_to(lv, index - lv->visible_items + 1);
    }
}

// Multi-selection support functions
void listview_set_multi_select(ListView *lv, bool enabled) {
    if (!lv) return;
    lv->multi_select_enabled = enabled;
    if (!enabled) {
        // Clear multi-selection when disabled
        listview_clear_selection(lv);
    }
}

void listview_toggle_selection(ListView *lv, int index) {
    if (!lv || index < 0 || index >= lv->item_count) return;
    
    if (lv->selected[index]) {
        lv->selected[index] = false;
        lv->selection_count--;
    } else {
        lv->selected[index] = true;
        lv->selection_count++;
    }
    lv->needs_redraw = true;
}

void listview_clear_selection(ListView *lv) {
    if (!lv) return;
    memset(lv->selected, 0, lv->capacity * sizeof(bool));
    lv->selection_count = 0;
    lv->needs_redraw = true;
}

int listview_get_selected_items(ListView *lv, int *indices, int max_items) {
    if (!lv || !indices || max_items <= 0) return 0;
    
    int count = 0;
    for (int i = 0; i < lv->item_count && count < max_items; i++) {
        if (lv->selected[i]) {
            indices[count++] = i;
        }
    }
    return count;
}

bool listview_handle_click(ListView *lv, int x, int y, Display *dpy, XftFont *font) {
    // Call the new function with no modifiers and no time for backwards compatibility
    return listview_handle_click_with_time(lv, x, y, 0, 0, dpy, font);
}

bool listview_handle_click_with_modifiers(ListView *lv, int x, int y, unsigned int state, Display *dpy, XftFont *font) {
    // Call the new function with no time for backwards compatibility
    return listview_handle_click_with_time(lv, x, y, state, 0, dpy, font);
}

bool listview_handle_click_with_time(ListView *lv, int x, int y, unsigned int state, unsigned long time, Display *dpy, XftFont *font) {
    if (!lv || !dpy || !font) return false;
    
    // Check if click is within listview bounds
    if (x < lv->x || x >= lv->x + lv->width ||
        y < lv->y || y >= lv->y + lv->height) {
        return false;
    }
    
    // Check if click is on scrollbar
    int scrollbar_x = lv->x + lv->width - LISTVIEW_SCROLLBAR_WIDTH - 2;
    if (x >= scrollbar_x) {
        int rel_y = y - lv->y;
        
        // Check up arrow
        if (rel_y >= lv->height - 2 - LISTVIEW_ARROW_HEIGHT * 2 && 
            rel_y < lv->height - 2 - LISTVIEW_ARROW_HEIGHT) {
            listview_handle_scroll(lv, -1);
            return true;
        }
        
        // Check down arrow
        if (rel_y >= lv->height - 2 - LISTVIEW_ARROW_HEIGHT) {
            listview_handle_scroll(lv, 1);
            return true;
        }
        
        // Check knob
        if (rel_y >= lv->scrollbar_knob_y && 
            rel_y < lv->scrollbar_knob_y + lv->scrollbar_knob_height) {
            lv->scrollbar_dragging = true;
            lv->scrollbar_drag_offset = rel_y - lv->scrollbar_knob_y;
            return true;
        }
        
        // Click on track - page up/down
        if (rel_y < lv->scrollbar_knob_y) {
            // Page up
            lv->scroll_offset -= lv->visible_items;
            if (lv->scroll_offset < 0) lv->scroll_offset = 0;
        } else {
            // Page down
            lv->scroll_offset += lv->visible_items;
            int max_scroll = lv->item_count - lv->visible_items;
            if (lv->scroll_offset > max_scroll) lv->scroll_offset = max_scroll;
        }
        lv->needs_redraw = true;
        listview_update_scrollbar(lv);
        return true;
    }
    
    // Click on item area
    int item_y = y - lv->y - 2; // Account for border
    int item_index = lv->scroll_offset + (item_y / LISTVIEW_ITEM_HEIGHT);
    
    // Check if click is on a valid item or blank area
    if (item_index >= lv->item_count) {
        // Clicked on blank area below items - clear selection
        if (lv->multi_select_enabled) {
            listview_clear_selection(lv);
        } else {
            lv->selected_index = -1;
        }
        lv->needs_redraw = true;
        // Notify callback that selection was cleared
        if (lv->on_select) {
            lv->on_select(-1, NULL, lv->callback_data);
        }
        return true;
    }
    
    if (item_index >= 0 && item_index < lv->item_count) {
        // Check if click is on the actual text, not blank space
        int text_start_x = lv->x + 6;  // Text starts at x+6 (same as in draw function)
        int content_end_x = lv->x + lv->width - LISTVIEW_SCROLLBAR_WIDTH - 2;
        
        // Check if click is in the margins (not on text area)
        if (x < text_start_x || x >= content_end_x) {
            // Clicked on margin - clear selection
            if (lv->multi_select_enabled) {
                listview_clear_selection(lv);
            } else {
                lv->selected_index = -1;
            }
            lv->needs_redraw = true;
            // Notify callback that selection was cleared
            if (lv->on_select) {
                lv->on_select(-1, NULL, lv->callback_data);
            }
            return true;
        }
        
        // Measure the actual text width to check if click is on text
        const char *text = lv->items[item_index].text;
        int text_len = strlen(text);
        XGlyphInfo text_extents;
        XftTextExtentsUtf8(dpy, font, (FcChar8*)text, text_len, &text_extents);
        
        // Check if click is past the end of the actual text
        int text_end_x = text_start_x + text_extents.width;
        if (x > text_end_x) {
            // Clicked past the end of text - clear selection
            if (lv->multi_select_enabled) {
                listview_clear_selection(lv);
            } else {
                lv->selected_index = -1;
            }
            lv->needs_redraw = true;
            // Notify callback that selection was cleared
            if (lv->on_select) {
                lv->on_select(-1, NULL, lv->callback_data);
            }
            return true;
        }
        
        // Handle multi-selection with modifiers
        if (lv->multi_select_enabled) {
            // Check for Ctrl (ControlMask = 0x04)
            if (state & 0x04) {  // ControlMask
                // Ctrl+click: toggle individual selection
                listview_toggle_selection(lv, item_index);
                lv->selected_index = item_index;  // Keep track of last clicked
                // Call the select callback for multi-selection changes
                if (lv->on_select) {
                    lv->on_select(item_index, lv->items[item_index].text, lv->callback_data);
                }
            }
            // Check for Shift (ShiftMask = 0x01)
            else if (state & 0x01) {  // ShiftMask
                // Shift+click: select range from last selected to current
                if (lv->selected_index >= 0) {
                    // Clear previous selections
                    listview_clear_selection(lv);
                    // Select range
                    int start = (lv->selected_index < item_index) ? lv->selected_index : item_index;
                    int end = (lv->selected_index < item_index) ? item_index : lv->selected_index;
                    for (int i = start; i <= end; i++) {
                        lv->selected[i] = true;
                        lv->selection_count++;
                    }
                    lv->needs_redraw = true;
                } else {
                    // No previous selection, just select this one
                    listview_clear_selection(lv);
                    lv->selected[item_index] = true;
                    lv->selection_count = 1;
                    lv->selected_index = item_index;
                }
                // Call the select callback for range selection
                if (lv->on_select) {
                    lv->on_select(item_index, lv->items[item_index].text, lv->callback_data);
                }
            }
            else {
                // Regular click: clear others and select only this one
                // Check for double-click with proper timing
                bool is_double_click = false;
                if (time > 0) {  // Only check if we have time info
                    if (item_index == lv->last_click_index && 
                        (time - lv->last_click_time) < 500) {  // 500ms double-click threshold
                        is_double_click = true;
                    }
                    lv->last_click_time = time;
                    lv->last_click_index = item_index;
                } else {
                    // Fallback for no time info - simple check
                    is_double_click = (item_index == lv->selected_index);
                }
                
                if (is_double_click && lv->on_double_click) {
                    lv->on_double_click(item_index, lv->items[item_index].text, lv->callback_data);
                    // Reset double-click tracking after successful double-click
                    lv->last_click_index = -1;
                } else {
                    listview_clear_selection(lv);
                    lv->selected[item_index] = true;
                    lv->selection_count = 1;
                    lv->selected_index = item_index;
                    if (lv->on_select) {
                        lv->on_select(item_index, lv->items[item_index].text, lv->callback_data);
                    }
                }
            }
        } else {
            // Single selection mode (original behavior)
            // Check for double-click with proper timing
            bool is_double_click = false;
            if (time > 0) {  // Only check if we have time info
                if (item_index == lv->last_click_index && 
                    (time - lv->last_click_time) < 500) {  // 500ms double-click threshold
                    is_double_click = true;
                }
                lv->last_click_time = time;
                lv->last_click_index = item_index;
            } else {
                // Fallback for no time info - simple check
                is_double_click = (item_index == lv->selected_index);
            }
            
            if (is_double_click && lv->on_double_click) {
                lv->on_double_click(item_index, lv->items[item_index].text, lv->callback_data);
                // Reset double-click tracking after successful double-click
                lv->last_click_index = -1;
            } else {
                lv->selected_index = item_index;
                if (lv->on_select) {
                    lv->on_select(item_index, lv->items[item_index].text, lv->callback_data);
                }
            }
        }
        lv->needs_redraw = true;
        return true;
    }
    
    return false;
}

bool listview_handle_motion(ListView *lv, int x, int y) {
    if (!lv || !lv->scrollbar_dragging) return false;
    
    int rel_y = y - lv->y - lv->scrollbar_drag_offset;
    int track_height = lv->height - 4 - (LISTVIEW_ARROW_HEIGHT * 2);
    int available_space = track_height - lv->scrollbar_knob_height;
    
    if (available_space > 0) {
        // Calculate new scroll position
        int scrollable_items = lv->item_count - lv->visible_items;
        lv->scroll_offset = (rel_y - 2) * scrollable_items / available_space;
        
        if (lv->scroll_offset < 0) lv->scroll_offset = 0;
        if (lv->scroll_offset > scrollable_items) lv->scroll_offset = scrollable_items;
        
        lv->needs_redraw = true;
        listview_update_scrollbar(lv);
    }
    
    return true;
}

bool listview_handle_release(ListView *lv) {
    if (!lv) return false;
    bool was_dragging = lv->scrollbar_dragging;
    lv->scrollbar_dragging = false;
    return was_dragging;
}

bool listview_handle_scroll(ListView *lv, int direction) {
    if (!lv) return false;
    
    lv->scroll_offset += direction;
    
    if (lv->scroll_offset < 0) lv->scroll_offset = 0;
    int max_scroll = lv->item_count - lv->visible_items;
    if (max_scroll < 0) max_scroll = 0;
    if (lv->scroll_offset > max_scroll) lv->scroll_offset = max_scroll;
    
    lv->needs_redraw = true;
    listview_update_scrollbar(lv);
    return true;
}

void listview_draw(ListView *lv, Display *dpy, Picture dest, XftDraw *xft_draw, XftFont *font) {
    if (!lv || !dpy) return;
    
    int x = lv->x;
    int y = lv->y;
    int w = lv->width;
    int h = lv->height;
    
    // Get colors from config.h
    XRenderColor white = WHITE;
    XRenderColor black = BLACK;
    XRenderColor gray = GRAY;
    XRenderColor blue = BLUE;
    
    // Draw borders: white (left/top), black (right/bottom)
    // Don't draw right border into scrollbar area
    int list_content_width = w - LISTVIEW_SCROLLBAR_WIDTH - 2;
    XRenderFillRectangle(dpy, PictOpSrc, dest, &white, x, y, 1, h);      // Left
    XRenderFillRectangle(dpy, PictOpSrc, dest, &white, x, y, list_content_width + 1, 1);      // Top (only list area)
    XRenderFillRectangle(dpy, PictOpSrc, dest, &black, x + list_content_width, y, 1, h);  // Right (at edge of list content)
    XRenderFillRectangle(dpy, PictOpSrc, dest, &black, x, y+h-1, list_content_width + 1, 1);  // Bottom (only list area)
    
    // Fill list content background (only the area where items are drawn, not scrollbar)
    XRenderFillRectangle(dpy, PictOpSrc, dest, &gray, x+1, y+1, list_content_width - 1, h-2);
    
    // Calculate content area for item drawing (excluding scrollbar)
    int content_width = list_content_width - 2;
    
    // Draw items
    if (font && xft_draw) {
        for (int i = 0; i < lv->visible_items && i + lv->scroll_offset < lv->item_count; i++) {
            int item_index = i + lv->scroll_offset;
            int item_y = y + 2 + i * LISTVIEW_ITEM_HEIGHT;
            
            // Draw selection background
            bool is_selected = false;
            if (lv->multi_select_enabled && lv->selected[item_index]) {
                is_selected = true;
            } else if (!lv->multi_select_enabled && item_index == lv->selected_index) {
                is_selected = true;
            }
            
            if (is_selected) {
                XRenderFillRectangle(dpy, PictOpSrc, dest, &blue, 
                                   x+2, item_y, content_width, LISTVIEW_ITEM_HEIGHT);
            }
            
            // Draw text
            XftColor text_color;
            XRenderColor color;
            if (lv->items[item_index].is_directory) {
                color = white;  // White for directories
            } else {
                color = black;  // Black for files
            }

            // Cache visual and colormap from xft_draw on first use
            if (!lv->visual) {
                lv->visual = XftDrawVisual(xft_draw);
                lv->colormap = XftDrawColormap(xft_draw);
            }

            XftColorAllocValue(dpy, lv->visual, lv->colormap, &color, &text_color);
            
            int text_x = x + 6;  // Start at x+2 from content area (which is at x+1) + additional 3 for padding
            int text_y = item_y + (LISTVIEW_ITEM_HEIGHT + font->ascent - font->descent) / 2;
            
            // Calculate maximum text width to prevent drawing into scrollbar
            int max_text_width = list_content_width - 8;  // Account for padding
            
            // Measure the text to see if we need to truncate
            const char *text = lv->items[item_index].text;
            int text_len = strlen(text);
            XGlyphInfo text_extents;
            XftTextExtentsUtf8(dpy, font, (FcChar8*)text, text_len, &text_extents);
            
            // If text fits, draw it all; otherwise truncate cleanly
            if (text_extents.width <= max_text_width) {
                XftDrawStringUtf8(xft_draw, &text_color, font,
                                text_x, text_y,
                                (FcChar8*)text, text_len);
            } else {
                // Find how many characters fit without going past boundary
                int fit_len = text_len;
                while (fit_len > 0) {
                    XftTextExtentsUtf8(dpy, font, (FcChar8*)text, fit_len, &text_extents);
                    if (text_extents.width <= max_text_width) break;
                    fit_len--;
                }
                
                // Draw only the characters that fit
                if (fit_len > 0) {
                    XftDrawStringUtf8(xft_draw, &text_color, font,
                                    text_x, text_y,
                                    (FcChar8*)text, fit_len);
                }
            }
            
            // Free color using same visual/colormap
            XftColorFree(dpy, lv->visual, lv->colormap, &text_color);
        }
    }
    
    // Draw scrollbar area (3 separate elements: track, up arrow, down arrow)
    int sb_x = x + list_content_width + 1;  // Start right after list content border
    int sb_w = LISTVIEW_SCROLLBAR_WIDTH;
    
    // Calculate positions for the three SEPARATE elements
    int track_y = y;
    int track_h = h - (LISTVIEW_ARROW_HEIGHT * 2);  // Track height minus space for arrows
    int arrow_up_y = y + track_h;  // Arrow up AFTER track ends
    int arrow_down_y = y + track_h + LISTVIEW_ARROW_HEIGHT;  // Arrow down AFTER arrow up
    
    // Always draw scrollbar area
    if (true) {
        
        // 1. Draw scrollbar track (between arrows)
        // Gray background
        XRenderFillRectangle(dpy, PictOpSrc, dest, &gray, sb_x, track_y, sb_w, track_h);
        
        // Track borders
        XRenderFillRectangle(dpy, PictOpSrc, dest, &white, sb_x, track_y, 1, track_h);      // Left white
        XRenderFillRectangle(dpy, PictOpSrc, dest, &white, sb_x, track_y, sb_w, 1);      // Top white
        XRenderFillRectangle(dpy, PictOpSrc, dest, &black, sb_x + sb_w - 1, track_y, 1, track_h); // Right black
        XRenderFillRectangle(dpy, PictOpSrc, dest, &black, sb_x, track_y + track_h - 1, sb_w, 1); // Bottom black
        
        // Scrollbar knob (black, 14px wide centered in 20px track)
        // Always draw knob - fills track when no scrolling needed
        int knob_width = 14;
        int knob_x_offset = (sb_w - knob_width) / 2;
        if (lv->scrollbar_knob_height > 0) {
            XRenderFillRectangle(dpy, PictOpSrc, dest, &black,
                               sb_x + knob_x_offset, lv->scrollbar_knob_y + track_y,
                               knob_width, lv->scrollbar_knob_height);
        }
        
        // 2. Draw up arrow button
        // Fill gray background
        XRenderFillRectangle(dpy, PictOpSrc, dest, &gray,
                           sb_x, arrow_up_y, sb_w, LISTVIEW_ARROW_HEIGHT);
        // Draw borders: white (left/top), black (right/bottom)
        XRenderFillRectangle(dpy, PictOpSrc, dest, &white, sb_x, arrow_up_y, 1, LISTVIEW_ARROW_HEIGHT);     // Left white
        XRenderFillRectangle(dpy, PictOpSrc, dest, &white, sb_x, arrow_up_y, sb_w, 1);                     // Top white
        XRenderFillRectangle(dpy, PictOpSrc, dest, &black, sb_x + sb_w - 1, arrow_up_y, 1, LISTVIEW_ARROW_HEIGHT); // Right black
        XRenderFillRectangle(dpy, PictOpSrc, dest, &black, sb_x, arrow_up_y + LISTVIEW_ARROW_HEIGHT - 1, sb_w, 1); // Bottom black
        // Draw up arrow (black triangle) - vertically centered
        int arrow_height = 5;
        int arrow_offset = (LISTVIEW_ARROW_HEIGHT - arrow_height) / 2;
        for (int i = 0; i < 5; i++) {
            XRenderFillRectangle(dpy, PictOpSrc, dest, &black,
                               sb_x + sb_w/2 - i, arrow_up_y + arrow_offset + i, i*2+1, 1);
        }
        
        // 3. Draw down arrow button
        // Fill gray background
        XRenderFillRectangle(dpy, PictOpSrc, dest, &gray,
                           sb_x, arrow_down_y, sb_w, LISTVIEW_ARROW_HEIGHT);
        // Draw borders: white (left/top), black (right/bottom)
        XRenderFillRectangle(dpy, PictOpSrc, dest, &white, sb_x, arrow_down_y, 1, LISTVIEW_ARROW_HEIGHT);     // Left white
        XRenderFillRectangle(dpy, PictOpSrc, dest, &white, sb_x, arrow_down_y, sb_w, 1);                     // Top white
        XRenderFillRectangle(dpy, PictOpSrc, dest, &black, sb_x + sb_w - 1, arrow_down_y, 1, LISTVIEW_ARROW_HEIGHT); // Right black
        XRenderFillRectangle(dpy, PictOpSrc, dest, &black, sb_x, arrow_down_y + LISTVIEW_ARROW_HEIGHT - 1, sb_w, 1); // Bottom black
        // Draw down arrow (black triangle) - vertically centered
        for (int i = 0; i < 5; i++) {
            XRenderFillRectangle(dpy, PictOpSrc, dest, &black,
                               sb_x + sb_w/2 - i, arrow_down_y + arrow_offset + (4-i), i*2+1, 1);
        }
    }
    
    lv->needs_redraw = false;
}

void listview_set_callbacks(ListView *lv,
                           void (*on_select)(int, const char*, void*),
                           void (*on_double_click)(int, const char*, void*),
                           void *user_data) {
    if (!lv) return;
    lv->on_select = on_select;
    lv->on_double_click = on_double_click;
    lv->callback_data = user_data;
}