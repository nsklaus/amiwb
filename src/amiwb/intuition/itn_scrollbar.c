// Scrollbar interaction handling
// All scrollbar-related logic: knobs, tracks, arrows, auto-repeat

#include "itn_scrollbar.h"
#include "itn_internal.h"
#include "../config.h"
#include <X11/Xlib.h>
#include <sys/time.h>
#include <math.h>

// External functions we need
extern void update_scroll_from_mouse_drag(Canvas *canvas, bool is_vertical,
                                         int initial_scroll, int drag_start_pos,
                                         int current_mouse_pos);

// Module-private state (no longer extern - encapsulated)
static Canvas *scrolling_canvas = NULL;
static Canvas *arrow_scroll_canvas = NULL;
static int arrow_scroll_direction = 0;
static bool arrow_scroll_vertical = false;
static struct timeval arrow_scroll_start_time;
static struct timeval arrow_scroll_last_time;
static bool scrolling_vertical = false;
static int initial_scroll = 0;
static int scroll_start_pos = 0;

// Scrollbar constants
#define SCROLL_STEP 20
#define TRACK_MARGIN 10
#define TRACK_RESERVED 54
// MIN_KNOB_SIZE is defined in config.h (10 pixels)

// ============================================================================
// Private Helper Functions
// ============================================================================

// Calculate the scrollbar track area (where the knob moves)
static void get_scrollbar_track_area(Canvas *canvas, bool is_vertical,
                                     int *x, int *y, int *width, int *height) {
    if (is_vertical) {
        *x = canvas->width - BORDER_WIDTH_RIGHT;
        *y = BORDER_HEIGHT_TOP + TRACK_MARGIN;
        *width = BORDER_WIDTH_RIGHT;
        *height = (canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM) - TRACK_RESERVED - TRACK_MARGIN;
    } else {
        *x = BORDER_WIDTH_LEFT + TRACK_MARGIN;
        *y = canvas->height - BORDER_HEIGHT_BOTTOM;
        *width = (canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT) - TRACK_RESERVED - TRACK_MARGIN;
        *height = BORDER_HEIGHT_BOTTOM;
    }
}

// Calculate scrollbar knob size based on content/visible ratio
static int calculate_scrollbar_knob_size(int track_length, int content_length) {
    float size_ratio = (float)track_length / (float)content_length;
    int knob_size = (int)(size_ratio * track_length);
    if (knob_size < MIN_KNOB_SIZE) knob_size = MIN_KNOB_SIZE;
    if (knob_size > track_length) knob_size = track_length;
    return knob_size;
}

// Calculate knob position from scroll amount
static int calculate_knob_position_from_scroll(int track_length, int knob_length,
                                               int scroll_amount, int max_scroll) {
    if (max_scroll <= 0) return 0;
    float position_ratio = (float)scroll_amount / (float)max_scroll;
    int available_space = track_length - knob_length;
    if (available_space <= 0) return 0;
    return (int)(position_ratio * available_space);
}

// Calculate scroll amount from mouse click position on track
static int calculate_scroll_from_mouse_click(int track_start, int track_length,
                                             int max_scroll, int click_position) {
    float click_ratio = (float)(click_position - track_start) / (float)track_length;
    int scroll_value = (int)(click_ratio * (float)max_scroll);
    return clamp_value_between(scroll_value, 0, max_scroll);
}

// Handle click on scrollbar track or knob
static bool handle_scrollbar_click(Canvas *canvas, XButtonEvent *event, bool is_vertical) {
    if (event->button != Button1) return false;

    int track_x, track_y, track_width, track_height;
    get_scrollbar_track_area(canvas, is_vertical, &track_x, &track_y, &track_width, &track_height);

    // Check if click is within track area
    bool click_in_track = (event->x >= track_x && event->x < track_x + track_width &&
                          event->y >= track_y && event->y < track_y + track_height);
    if (!click_in_track) return false;

    int track_length = is_vertical ? track_height : track_width;
    int content_length = is_vertical ? canvas->content_height : canvas->content_width;
    int current_scroll = is_vertical ? canvas->scroll_y : canvas->scroll_x;
    int max_scroll = is_vertical ? canvas->max_scroll_y : canvas->max_scroll_x;

    int knob_length = calculate_scrollbar_knob_size(track_length, content_length);
    int knob_position = (is_vertical ? track_y : track_x) +
                       calculate_knob_position_from_scroll(track_length, knob_length, current_scroll, max_scroll);

    int click_coordinate = is_vertical ? event->y : event->x;
    bool click_on_knob = (click_coordinate >= knob_position && click_coordinate < knob_position + knob_length);

    if (click_on_knob) {
        // Start dragging the knob
        int root_coordinate = is_vertical ? event->y_root : event->x_root;
        scrolling_canvas = canvas;
        scrolling_vertical = is_vertical;
        initial_scroll = current_scroll;
        scroll_start_pos = root_coordinate;
    } else {
        // Click on track - jump to that position
        int track_start = is_vertical ? track_y : track_x;
        int new_scroll = calculate_scroll_from_mouse_click(track_start, track_length, max_scroll, click_coordinate);
        if (is_vertical) canvas->scroll_y = new_scroll;
        else canvas->scroll_x = new_scroll;
        DAMAGE_CANVAS(canvas);
        SCHEDULE_FRAME();
    }
    return true;
}

// Handle arrow button auto-repeat while held
static bool handle_arrow_scroll_repeat(void) {
    if (!arrow_scroll_canvas || arrow_scroll_direction == 0) return false;

    if (arrow_scroll_vertical) {
        int current_scroll = arrow_scroll_canvas->scroll_y;
        int new_scroll = current_scroll + (arrow_scroll_direction * SCROLL_STEP);
        new_scroll = clamp_value_between(new_scroll, 0, arrow_scroll_canvas->max_scroll_y);

        if (new_scroll != current_scroll) {
            arrow_scroll_canvas->scroll_y = new_scroll;
            DAMAGE_CANVAS(arrow_scroll_canvas);
            SCHEDULE_FRAME();
            return true;
        }
    } else {
        int current_scroll = arrow_scroll_canvas->scroll_x;
        int new_scroll = current_scroll + (arrow_scroll_direction * SCROLL_STEP);
        new_scroll = clamp_value_between(new_scroll, 0, arrow_scroll_canvas->max_scroll_x);

        if (new_scroll != current_scroll) {
            arrow_scroll_canvas->scroll_x = new_scroll;
            DAMAGE_CANVAS(arrow_scroll_canvas);
            SCHEDULE_FRAME();
            return true;
        }
    }

    // If we can't scroll anymore, stop the repeat
    arrow_scroll_canvas = NULL;
    arrow_scroll_direction = 0;
    return false;
}

// ============================================================================
// Public API Implementation
// ============================================================================

// Handle button press on scrollbar (arrows, track, knob)
// Returns true if event was consumed, false if not a scrollbar interaction
bool itn_scrollbar_handle_button_press(Canvas *canvas, XButtonEvent *event) {
    // Only handle workbench windows (no client window) with scrollbars enabled
    if (event->button != Button1 || canvas->client_win != None || canvas->disable_scrollbars) {
        return false;
    }

    int x = event->x;
    int y = event->y;
    int w = canvas->width;
    int h = canvas->height;

    // Check vertical scroll arrows (on right border) FIRST
    if (x >= w - BORDER_WIDTH_RIGHT && x < w) {
        // Up arrow area
        if (y >= h - BORDER_HEIGHT_BOTTOM - 41 && y < h - BORDER_HEIGHT_BOTTOM - 21) {
            canvas->v_arrow_up_armed = true;
            arrow_scroll_canvas = canvas;
            arrow_scroll_direction = -1;
            arrow_scroll_vertical = true;
            gettimeofday(&arrow_scroll_start_time, NULL);
            arrow_scroll_last_time = arrow_scroll_start_time;
            // Perform initial scroll
            if (canvas->scroll_y > 0) {
                canvas->scroll_y = max(0, canvas->scroll_y - SCROLL_STEP);
            }
            DAMAGE_CANVAS(canvas);
            SCHEDULE_FRAME();
            return true;
        }
        // Down arrow area
        else if (y >= h - BORDER_HEIGHT_BOTTOM - 21 && y < h - BORDER_HEIGHT_BOTTOM) {
            canvas->v_arrow_down_armed = true;
            arrow_scroll_canvas = canvas;
            arrow_scroll_direction = 1;
            arrow_scroll_vertical = true;
            gettimeofday(&arrow_scroll_start_time, NULL);
            arrow_scroll_last_time = arrow_scroll_start_time;
            // Perform initial scroll
            if (canvas->scroll_y < canvas->max_scroll_y) {
                canvas->scroll_y = min(canvas->max_scroll_y, canvas->scroll_y + SCROLL_STEP);
            }
            DAMAGE_CANVAS(canvas);
            SCHEDULE_FRAME();
            return true;
        }
    }

    // Check horizontal scroll arrows (on bottom border)
    if (y >= h - BORDER_HEIGHT_BOTTOM && y < h) {
        // Left arrow area
        if (x >= w - BORDER_WIDTH_RIGHT - 41 && x < w - BORDER_WIDTH_RIGHT - 21) {
            canvas->h_arrow_left_armed = true;
            arrow_scroll_canvas = canvas;
            arrow_scroll_direction = -1;
            arrow_scroll_vertical = false;
            gettimeofday(&arrow_scroll_start_time, NULL);
            arrow_scroll_last_time = arrow_scroll_start_time;
            // Perform initial scroll
            if (canvas->scroll_x > 0) {
                canvas->scroll_x = max(0, canvas->scroll_x - SCROLL_STEP);
            }
            DAMAGE_CANVAS(canvas);
            SCHEDULE_FRAME();
            return true;
        }
        // Right arrow area
        else if (x >= w - BORDER_WIDTH_RIGHT - 21 && x < w - BORDER_WIDTH_RIGHT) {
            canvas->h_arrow_right_armed = true;
            arrow_scroll_canvas = canvas;
            arrow_scroll_direction = 1;
            arrow_scroll_vertical = false;
            gettimeofday(&arrow_scroll_start_time, NULL);
            arrow_scroll_last_time = arrow_scroll_start_time;
            // Perform initial scroll
            if (canvas->scroll_x < canvas->max_scroll_x) {
                canvas->scroll_x = min(canvas->max_scroll_x, canvas->scroll_x + SCROLL_STEP);
            }
            DAMAGE_CANVAS(canvas);
            SCHEDULE_FRAME();
            return true;
        }
    }

    // Now check scrollbar track/knob
    // Try vertical scrollbar first
    if (handle_scrollbar_click(canvas, event, true)) {
        return true;
    }
    // Then horizontal scrollbar
    if (handle_scrollbar_click(canvas, event, false)) {
        return true;
    }

    // Not a scrollbar interaction
    return false;
}

// Handle button release on scrollbar arrows
// Returns true if event was consumed, false otherwise
bool itn_scrollbar_handle_button_release(Canvas *canvas, XButtonEvent *event) {
    bool consumed = false;

    // Clear arrow armed states and perform final scroll if still on button
    if (canvas->v_arrow_up_armed) {
        canvas->v_arrow_up_armed = false;
        DAMAGE_CANVAS(canvas);
        // Check if still on button
        if (event->x >= canvas->width - BORDER_WIDTH_RIGHT && event->x < canvas->width &&
            event->y >= canvas->height - BORDER_HEIGHT_BOTTOM - 42 &&
            event->y < canvas->height - BORDER_HEIGHT_BOTTOM - 22) {
            if (canvas->scroll_y > 0) {
                canvas->scroll_y = max(0, canvas->scroll_y - SCROLL_STEP);
                DAMAGE_CANVAS(canvas);
            }
        }
        SCHEDULE_FRAME();
        consumed = true;
    }

    if (canvas->v_arrow_down_armed) {
        canvas->v_arrow_down_armed = false;
        DAMAGE_CANVAS(canvas);
        // Check if still on button
        if (event->x >= canvas->width - BORDER_WIDTH_RIGHT && event->x < canvas->width &&
            event->y >= canvas->height - BORDER_HEIGHT_BOTTOM - 21 &&
            event->y < canvas->height - BORDER_HEIGHT_BOTTOM) {
            if (canvas->scroll_y < canvas->max_scroll_y) {
                canvas->scroll_y = min(canvas->max_scroll_y, canvas->scroll_y + SCROLL_STEP);
                DAMAGE_CANVAS(canvas);
            }
        }
        SCHEDULE_FRAME();
        consumed = true;
    }

    if (canvas->h_arrow_left_armed) {
        canvas->h_arrow_left_armed = false;
        DAMAGE_CANVAS(canvas);
        // Check if still on button
        if (event->y >= canvas->height - BORDER_HEIGHT_BOTTOM && event->y < canvas->height &&
            event->x >= canvas->width - BORDER_WIDTH_RIGHT - 42 &&
            event->x < canvas->width - BORDER_WIDTH_RIGHT - 22) {
            if (canvas->scroll_x > 0) {
                canvas->scroll_x = max(0, canvas->scroll_x - SCROLL_STEP);
                DAMAGE_CANVAS(canvas);
            }
        }
        SCHEDULE_FRAME();
        consumed = true;
    }

    if (canvas->h_arrow_right_armed) {
        canvas->h_arrow_right_armed = false;
        DAMAGE_CANVAS(canvas);
        // Check if still on button
        if (event->y >= canvas->height - BORDER_HEIGHT_BOTTOM && event->y < canvas->height &&
            event->x >= canvas->width - BORDER_WIDTH_RIGHT - 22 &&
            event->x < canvas->width - BORDER_WIDTH_RIGHT) {
            if (canvas->scroll_x < canvas->max_scroll_x) {
                canvas->scroll_x = min(canvas->max_scroll_x, canvas->scroll_x + SCROLL_STEP);
                DAMAGE_CANVAS(canvas);
            }
        }
        SCHEDULE_FRAME();
        consumed = true;
    }

    // Clear knob dragging state
    if (scrolling_canvas) {
        scrolling_canvas = NULL;
        consumed = true;
    }

    // Clear arrow repeat state
    if (arrow_scroll_canvas) {
        arrow_scroll_canvas = NULL;
        arrow_scroll_direction = 0;
        consumed = true;
    }

    return consumed;
}

// Handle motion during scrollbar knob dragging
// Returns true if scrolling is active, false otherwise
bool itn_scrollbar_handle_motion(XMotionEvent *event) {
    if (scrolling_canvas) {
        // Pass the motion coordinate in the appropriate axis
        int current_mouse_pos = scrolling_vertical ? event->y_root : event->x_root;
        update_scroll_from_mouse_drag(scrolling_canvas, scrolling_vertical,
                                     initial_scroll, scroll_start_pos, current_mouse_pos);
        return true;
    }
    return false;
}

// Check if enough time has passed for arrow scroll repeat
// Called from main event loop
void itn_scrollbar_check_arrow_repeat(void) {
    if (!arrow_scroll_canvas || arrow_scroll_direction == 0) return;

    struct timeval now;
    gettimeofday(&now, NULL);

    // Calculate time since last scroll
    long elapsed_ms = (now.tv_sec - arrow_scroll_last_time.tv_sec) * 1000 +
                      (now.tv_usec - arrow_scroll_last_time.tv_usec) / 1000;

    // Initial delay: 400ms, then repeat every 50ms
    long delay_ms = 50;  // Fast repeat rate
    if (now.tv_sec == arrow_scroll_start_time.tv_sec &&
        (now.tv_usec - arrow_scroll_start_time.tv_usec) < 400000) {
        delay_ms = 400;  // Initial delay before auto-repeat starts
    }

    if (elapsed_ms >= delay_ms) {
        handle_arrow_scroll_repeat();
        arrow_scroll_last_time = now;
    }
}

// Query if scrollbar knob dragging is active
bool itn_scrollbar_is_scrolling_active(void) {
    return scrolling_canvas != NULL;
}
