// File: wb_layout.c
// View Modes and Icon Layout - positioning, sorting, view mode management

#define _POSIX_C_SOURCE 200809L
#include "wb_internal.h"
#include "../config.h"
#include "../render.h"
#include "../render_public.h"
#include "../intuition/itn_internal.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// Forward declaration (now public - called from wb_drag.c)
void wb_layout_apply_view(Canvas *canvas);

// Global state
static bool spatial_mode = true;  // New window per directory
static bool global_show_hidden = false;
static ViewMode global_view_mode = VIEW_ICONS;  // Default to icons mode

// ============================================================================
// Icon Sorting Comparators
// ============================================================================

// Label comparison (case-insensitive)
static int label_cmp(const void *a, const void *b) {
    const FileIcon *ia = *(FileIcon* const*)a;
    const FileIcon *ib = *(FileIcon* const*)b;
    const char *la = (ia && ia->label) ? ia->label : "";
    const char *lb = (ib && ib->label) ? ib->label : "";
    return strcasecmp(la, lb);
}

// Directories first, then files; both groups A..Z by label
static int dir_first_cmp(const void *A, const void *B) {
    const FileIcon *a = *(FileIcon* const*)A;
    const FileIcon *b = *(FileIcon* const*)B;
    if ((a->type == TYPE_DRAWER) != (b->type == TYPE_DRAWER)) {
        return (a->type == TYPE_DRAWER) ? -1 : 1;
    }
    return label_cmp(A, B);
}

// Icon cleanup comparison (for desktop ordering)
static int icon_cmp(const void *a, const void *b) {
    FileIcon *ia = *(FileIcon **)a;
    FileIcon *ib = *(FileIcon **)b;
    
    // System always first
    if (strcmp(ia->label, "System") == 0) return -1;
    if (strcmp(ib->label, "System") == 0) return 1;
    
    // Home always second
    if (strcmp(ia->label, "Home") == 0) return -1;
    if (strcmp(ib->label, "Home") == 0) return 1;
    
    // Device drives after System/Home
    if (ia->type == TYPE_DEVICE && ib->type != TYPE_DEVICE) return -1;
    if (ia->type != TYPE_DEVICE && ib->type == TYPE_DEVICE) return 1;
    
    // Drawers before files
    if (ia->type == TYPE_DRAWER && ib->type != TYPE_DRAWER) return -1;
    if (ia->type != TYPE_DRAWER && ib->type == TYPE_DRAWER) return 1;
    
    // Alphabetical
    return strcmp(ia->label, ib->label);
}

// ============================================================================
// Spatial Mode Getters/Setters
// ============================================================================

bool get_spatial_mode(void) {
    return spatial_mode;
}

void set_spatial_mode(bool mode) {
    spatial_mode = mode;
}

bool get_global_show_hidden_state(void) {
    return global_show_hidden;
}

void set_global_show_hidden_state(bool show) {
    global_show_hidden = show;
}

ViewMode get_global_view_mode(void) {
    return global_view_mode;
}

void set_global_view_mode(ViewMode mode) {
    global_view_mode = mode;
}

// ============================================================================
// Content Bounds Calculation
// ============================================================================

void wb_layout_compute_bounds(Canvas *canvas) {
    if (!canvas) return;
    
    FileIcon **icon_array = wb_icons_array_get();
    int icon_count = wb_icons_array_count();
    
    // For Names view, calculate based on text width
    if (canvas->type == WINDOW && canvas->view_mode == VIEW_NAMES) {
        int max_text_w = 0;
        int max_y = 0;
        for (int i = 0; i < icon_count; i++) {
            if (icon_array[i]->display_window == canvas->win) {
                int lw = get_text_width(icon_array[i]->label ? icon_array[i]->label : "");
                if (lw > max_text_w) max_text_w = lw;
                max_y = max(max_y, icon_array[i]->y + 24);
            }
        }
        int padding = 16;
        int visible_w = canvas->width - BORDER_WIDTH_LEFT - 
                       (canvas->client_win == None ? BORDER_WIDTH_RIGHT : BORDER_WIDTH_RIGHT_CLIENT);
        canvas->content_width = max(visible_w, max_text_w + padding);
        canvas->content_height = max_y + 10;
    } else {
        // Icons view: use icon bounds INCLUDING label width
        int max_x = 0, max_y = 0;
        for (int i = 0; i < icon_count; i++) {
            if (icon_array[i]->display_window == canvas->win) {
                FileIcon *icon = icon_array[i];
                int icon_right = icon->x + icon->width;
                int icon_bottom = icon->y + icon->height + 20;  // +20 for label
                
                if (icon_right > max_x) max_x = icon_right;
                if (icon_bottom > max_y) max_y = icon_bottom;
            }
        }
        
        int visible_w = canvas->width - BORDER_WIDTH_LEFT -
                       (canvas->client_win == None ? BORDER_WIDTH_RIGHT : BORDER_WIDTH_RIGHT_CLIENT);
        int visible_h = canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM;
        
        canvas->content_width = max(visible_w, max_x + 20);
        canvas->content_height = max(visible_h, max_y + 20);
    }
}

// ============================================================================
// Icon Cleanup (Auto-arrange)
// ============================================================================

void icon_cleanup(Canvas *canvas) {
    if (!canvas) return;
    
    int count = 0;
    FileIcon **list = wb_icons_for_canvas(canvas, &count);
    if (!list || count == 0) {
        refresh_canvas(canvas);
        return;
    }
    
    qsort(list, count, sizeof(FileIcon *), icon_cmp);
    
    int cell_h = ICON_SPACING;
    int visible_h = (canvas->type == WINDOW) ? 
                   (canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM) : 
                   canvas->height;
    int start_x = (canvas->type == DESKTOP) ? 20 : 10;
    int start_y = (canvas->type == DESKTOP) ? 40 : 10;
    
    if (canvas->type == DESKTOP) {
        // Desktop: vertical column layout
        int step_x = 110;
        int step_y = 80;
        int first_slot_y = 200;  // Below Home icon
        
        int x = start_x;
        int y = first_slot_y;
        
        for (int i2 = 0; i2 < count; ++i2) {
            FileIcon *ic = list[i2];
            
            // Fixed positions for System and Home
            if (strcmp(ic->label, "System") == 0) {
                int column_center_offset = (step_x - ic->width) / 2;
                if (column_center_offset < 0) column_center_offset = 0;
                ic->x = 20 + column_center_offset;
                ic->y = 40;
            } else if (strcmp(ic->label, "Home") == 0) {
                int column_center_offset = (step_x - ic->width) / 2;
                if (column_center_offset < 0) column_center_offset = 0;
                ic->x = 20 + column_center_offset;
                ic->y = 120;
            } else {
                // Column layout for all other icons
                int column_center_offset = (step_x - ic->width) / 2;
                if (column_center_offset < 0) column_center_offset = 0;
                
                ic->x = x + column_center_offset;
                ic->y = y;
                
                y += step_y;
                
                // Move to next column if needed
                if (y + 64 > canvas->height) {
                    x += step_x;
                    y = first_slot_y;
                }
            }
        }
        free(list);
    } else {
        // Window: grid layout
        int num_rows = max(1, (visible_h - start_y) / cell_h);
        int num_columns = (count + num_rows - 1) / num_rows;
        
        int *col_widths = malloc(num_columns * sizeof(int));
        if (!col_widths) {
            log_error("[ERROR] malloc failed for col_widths");
            free(list);
            return;
        }
        
        // Calculate column widths
        int min_cell_w = 80;
        int max_allowed_w = get_text_width("WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW");
        int padding = 20;
        
        for (int col = 0; col < num_columns; col++) {
            int max_w_in_col = 0;
            for (int row = 0; row < num_rows; row++) {
                int i2 = col * num_rows + row;
                if (i2 >= count) break;
                int label_w = get_text_width(list[i2]->label);
                if (label_w > max_w_in_col) max_w_in_col = label_w;
            }
            col_widths[col] = max(min_cell_w, min(max_w_in_col + padding, max_allowed_w + padding));
        }
        
        // Position icons
        int current_x = start_x;
        for (int col = 0; col < num_columns; col++) {
            int col_w = col_widths[col];
            for (int row = 0; row < num_rows; row++) {
                int i2 = col * num_rows + row;
                if (i2 >= count) break;
                int cell_y = start_y + row * cell_h;
                list[i2]->x = current_x + (col_w - list[i2]->width) / 2;
                list[i2]->y = cell_y + (cell_h - list[i2]->height - 20);
            }
            current_x += col_w;
        }
        
        free(col_widths);
        free(list);
    }
    
    wb_layout_apply_view(canvas);
    compute_max_scroll(canvas);
    redraw_canvas(canvas);
}

// ============================================================================
// View Mode Layout
// ============================================================================

// Apply view layout (exported for wb_drag.c)
void wb_layout_apply_view(Canvas *canvas) {
    if (!canvas) return;
    
    // Desktop remains icon grid
    if (canvas->type != WINDOW) {
        wb_layout_compute_bounds(canvas);
        return;
    }
    
    if (canvas->view_mode == VIEW_NAMES) {
        // List view: single column sorted
        int count = 0;
        FileIcon **list = wb_icons_for_canvas(canvas, &count);
        if (!list || count == 0) {
            wb_layout_compute_bounds(canvas);
            return;
        }
        
        qsort(list, count, sizeof(FileIcon*), dir_first_cmp);
        
        int x = 12, y = 10, row_h = 24;
        for (int i = 0; i < count; ++i) {
            FileIcon *ic = list[i];
            ic->x = x;
            ic->y = y;
            y += row_h;
        }
        
        free(list);
        
        int padding = 16;
        int visible_w = canvas->width - BORDER_WIDTH_LEFT -
                       (canvas->client_win == None ? BORDER_WIDTH_RIGHT : BORDER_WIDTH_RIGHT_CLIENT);
        
        int max_text_w = 0;
        FileIcon **icon_array = wb_icons_array_get();
        int icon_count = wb_icons_array_count();
        for (int i = 0; i < icon_count; i++) {
            if (icon_array[i]->display_window == canvas->win) {
                int lw = get_text_width(icon_array[i]->label ? icon_array[i]->label : "");
                if (lw > max_text_w) max_text_w = lw;
            }
        }
        
        canvas->content_width = max(visible_w, max_text_w + padding);
        canvas->content_height = y + 10;
    } else {
        // Icon grid mode: keep positions, recompute bounds
        wb_layout_compute_bounds(canvas);
    }
}

void set_canvas_view_mode(Canvas *canvas, ViewMode m) {
    if (!canvas) return;
    if (canvas->view_mode == m) return;

    canvas->view_mode = m;
    canvas->scroll_x = 0;
    canvas->scroll_y = 0;

    // Update global view mode so new windows use the same mode
    set_global_view_mode(m);

    // Always cleanup icons when switching modes to ensure proper positioning
    icon_cleanup(canvas);

    wb_layout_apply_view(canvas);
    redraw_canvas(canvas);
}

// ============================================================================
// Find Free Slot
// ============================================================================

void wb_layout_find_free_slot(Canvas *canvas, int *out_x, int *out_y) {
    if (!canvas || !out_x || !out_y) return;
    
    int step_x = 110;
    int step_y = 80;
    
    // Find rightmost/bottommost icon
    FileIcon **icons = wb_icons_array_get();
    int count = wb_icons_array_count();
    int last_x = -1;
    int last_y = -1;
    
    for (int i = 0; i < count; i++) {
        if (icons[i] && icons[i]->display_window == canvas->win) {
            if (icons[i]->x > last_x || (icons[i]->x == last_x && icons[i]->y > last_y)) {
                last_x = icons[i]->x;
                last_y = icons[i]->y;
            }
        }
    }
    
    // Place new icon after last
    if (last_x >= 0) {
        *out_x = last_x;
        *out_y = last_y + step_y;
        
        // Wrap to next column if too far down
        if (*out_y > canvas->height - 100) {
            *out_x = last_x + step_x;
            *out_y = (canvas->type == DESKTOP) ? 200 : 10;
        }
    } else {
        // No icons found, use default position
        *out_x = (canvas->type == DESKTOP) ? 20 : 10;
        *out_y = (canvas->type == DESKTOP) ? 200 : 10;
    }
}
