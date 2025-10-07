// Damage tracking & frame scheduling
// This module handles damage accumulation and frame timing

#include "../config.h"
#include "itn_internal.h"
#include "../render.h"  // For redraw_canvas
#include "../amiwbrc.h"  // For config access
#include <X11/Xlib.h>
#include <time.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

// Frame scheduling state (module-private)
static int g_frame_timer_fd = -1;
static bool g_frame_scheduled = false;
static int g_target_fps = 120;  // Default 120Hz
bool g_continuous_mode = false;  // Default on-demand rendering

// Damage accumulation state
static bool damage_pending = false;
static XRectangle damage_bounds = {0, 0, 0, 0};
static time_t last_frame_time = 0;

// Performance metrics (migrated from old compositor)
static struct {
    // Frame timing
    struct timespec last_frame_time;
    struct timespec current_frame_start;
    struct timespec metrics_start_time;
    uint64_t frame_count;
    double total_frame_time_ms;
    double worst_frame_time_ms;

    // Render statistics
    uint64_t full_repaints;
    uint64_t damage_events;
    uint64_t frames_skipped;
    uint64_t composite_calls;

    // Window statistics
    int window_count;
    int visible_windows;

    // Repaint triggers
    uint64_t repaints_damage;
    uint64_t repaints_configure;
    uint64_t repaints_map;

    // Damage tracking
    uint64_t pixels_actually_drawn;

    time_t start_time;
} metrics = {0};

// External references (temporary during migration)

void itn_render_accumulate_damage(int x, int y, int width, int height) {
    if (width <= 0 || height <= 0) return;

    if (!damage_pending) {
        // First damage - initialize bounds
        damage_bounds.x = x;
        damage_bounds.y = y;
        damage_bounds.width = width;
        damage_bounds.height = height;
        damage_pending = true;
    } else {
        // Expand bounds to include new damage
        int right = max(damage_bounds.x + damage_bounds.width, x + width);
        int bottom = max(damage_bounds.y + damage_bounds.height, y + height);
        damage_bounds.x = min(damage_bounds.x, x);
        damage_bounds.y = min(damage_bounds.y, y);
        damage_bounds.width = right - damage_bounds.x;
        damage_bounds.height = bottom - damage_bounds.y;
    }
}

void itn_render_accumulate_canvas_damage(Canvas *canvas) {
    if (!canvas) return;
    itn_render_accumulate_damage(canvas->x, canvas->y, canvas->width, canvas->height);
}

void itn_render_schedule_frame(void) {
    // Don't schedule if we're already scheduled or no timer
    if (g_frame_scheduled) {
        // log_error("[FRAME] Schedule blocked - already scheduled");
        return;
    }
    if (g_frame_timer_fd < 0) return;
    if (g_target_fps <= 0) return;  // Safety check

    // In on-demand mode, don't schedule if we don't have damage yet
    // In continuous mode, always schedule frames
    if (!g_continuous_mode && !damage_pending) {
        // log_error("[FRAME] Schedule blocked - no damage pending");
        return;
    }

    // Calculate time since last frame
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    // Calculate minimum frame interval in nanoseconds (e.g., 8.33ms for 120Hz)
    long frame_interval_ns = 1000000000L / g_target_fps;

    // Calculate time elapsed since last frame
    long elapsed_ns = (now.tv_sec - metrics.last_frame_time.tv_sec) * 1000000000L +
                     (now.tv_nsec - metrics.last_frame_time.tv_nsec);

    // Calculate delay to next frame
    long delay_ns;
    if (g_continuous_mode) {
        // In continuous mode, ALWAYS use full frame interval to ensure
        // X11 input events get processed between frames
        // Never use minimal delay - that starves the event loop
        delay_ns = frame_interval_ns;
    } else if (elapsed_ns < frame_interval_ns) {
        // On-demand mode: wait for remainder of frame interval
        delay_ns = frame_interval_ns - elapsed_ns;
    } else {
        // On-demand mode: render immediately with minimal delay
        // Use 100 microseconds (0.1ms) for near-immediate response
        delay_ns = 100000;  // 0.1ms - prevents CPU spinning while being responsive
    }

    // Set timer for next frame
    struct itimerspec its = {
        .it_value.tv_sec = delay_ns / 1000000000L,
        .it_value.tv_nsec = delay_ns % 1000000000L,
        .it_interval = {0, 0}  // One-shot timer
    };

    if (timerfd_settime(g_frame_timer_fd, 0, &its, NULL) == 0) {
        g_frame_scheduled = true;
    } else {
        log_error("[RENDER] ERROR: timerfd_settime failed: %s", strerror(errno));
    }
}

void itn_render_process_frame(void) {
    // In on-demand mode, skip if no damage
    // In continuous mode, always render
    if (!g_continuous_mode && !damage_pending) {
        // No damage to render, just return
        // Don't clear g_frame_scheduled - let timer handle it
        return;
    }

    // Start frame timing
    struct timespec frame_start;
    clock_gettime(CLOCK_MONOTONIC, &frame_start);

    metrics.frame_count++;
    metrics.full_repaints++;  // Currently always full repaint

    // If compositor is active, use it
    if (itn_composite_is_active()) {
        itn_composite_render_all();
    } else {
        // Fallback: render damaged canvases using legacy path
        itn_render_damaged_canvases();
    }

    // End frame timing
    struct timespec frame_end;
    clock_gettime(CLOCK_MONOTONIC, &frame_end);

    // Calculate frame time
    double frame_time_ms = (frame_end.tv_sec - frame_start.tv_sec) * 1000.0 +
                          (frame_end.tv_nsec - frame_start.tv_nsec) / 1000000.0;

    metrics.total_frame_time_ms += frame_time_ms;
    if (frame_time_ms > metrics.worst_frame_time_ms) {
        metrics.worst_frame_time_ms = frame_time_ms;
    }
    metrics.last_frame_time = frame_end;

    // Clear damage for next frame
    damage_pending = false;
    damage_bounds = (XRectangle){0, 0, 0, 0};

    // DON'T clear g_frame_scheduled here! It will be cleared when timer expires
    // This prevents immediate re-scheduling

    // Update frame time
    last_frame_time = time(NULL);

    // In continuous mode, schedule next frame AFTER processing current one
    // This allows X11 events to be handled between frames
    if (g_continuous_mode) {
        itn_render_schedule_frame();
    }
}

// Render canvases that have damage (fallback for non-compositor mode)
void itn_render_damaged_canvases(void) {
    // Find canvases that intersect with damage bounds
    int count = itn_manager_get_count();
    for (int i = 0; i < count; i++) {
        Canvas *canvas = itn_manager_get_canvas(i);
        if (!canvas) continue;

        // Check if canvas intersects with damage bounds
        if (canvas->x < damage_bounds.x + damage_bounds.width &&
            canvas->x + canvas->width > damage_bounds.x &&
            canvas->y < damage_bounds.y + damage_bounds.height &&
            canvas->y + canvas->height > damage_bounds.y) {

            // Use legacy render path from render.c
            redraw_canvas(canvas);
        }
    }
}

// Check if we need to render a frame
bool itn_render_needs_frame(void) {
    return damage_pending;
}

// Get frame scheduling file descriptor for select()
int itn_render_get_timer_fd(void) {
    return g_frame_timer_fd;
}

// Read timer to clear it after select()
void itn_render_consume_timer(void) {
    if (g_frame_timer_fd >= 0) {
        uint64_t expirations;
        read(g_frame_timer_fd, &expirations, sizeof(expirations));
        // Clear the scheduled flag - now new frames can be scheduled
        g_frame_scheduled = false;

        // DON'T re-schedule here in continuous mode!
        // This creates a tight loop that starves X11 event processing.
        // Only schedule if we have pending damage in on-demand mode.
        if (!g_continuous_mode && damage_pending) {
            itn_render_schedule_frame();
        }
    }
}

// Set target frame rate
void itn_render_set_target_fps(int fps) {
    if (fps > 0 && fps <= 240) {  // Reasonable limits
        g_target_fps = fps;
    }
}

// Record damage event for metrics
void itn_render_record_damage_event(void) {
    metrics.damage_events++;
}

// Get performance metrics
void itn_render_get_metrics(uint64_t *frames, uint64_t *damage, uint64_t *skipped) {
    if (frames) *frames = metrics.frame_count;
    if (damage) *damage = metrics.damage_events;
    if (skipped) *skipped = metrics.frames_skipped;
}

bool itn_render_init_frame_scheduler(void) {
    // Create timer for frame scheduling
    g_frame_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (g_frame_timer_fd < 0) {
        log_error("[ERROR] Failed to create frame timer");
        return false;
    }

    // Read configuration
    const AmiwbConfig *config = get_config();

    // Apply target FPS from config (default to 120 if not set or invalid)
    if (config->target_fps > 0 && config->target_fps <= 240) {
        g_target_fps = config->target_fps;
        log_error("[RENDER] Target FPS set to %d from config", g_target_fps);
    } else {
        g_target_fps = 120;  // Default to 120Hz
        log_error("[RENDER] Target FPS defaulting to %d", g_target_fps);
    }

    // Apply render mode from config (0=on-demand, 1=continuous)
    g_continuous_mode = (config->render_mode == 1);
    log_error("[RENDER] Render mode: %s", g_continuous_mode ? "CONTINUOUS" : "ON-DEMAND");

    // Initialize metrics timestamps
    clock_gettime(CLOCK_MONOTONIC, &metrics.last_frame_time);
    clock_gettime(CLOCK_MONOTONIC, &metrics.metrics_start_time);

    // In continuous mode, kick off the first frame
    if (g_continuous_mode) {
        itn_render_schedule_frame();
    }

    return true;
}

void itn_render_cleanup_frame_scheduler(void) {
    if (g_frame_timer_fd >= 0) {
        close(g_frame_timer_fd);
        g_frame_timer_fd = -1;
    }
    g_frame_scheduled = false;
}

// Set the frame timer file descriptor (called by events.c during transition)
void itn_render_set_timer_fd(int fd) {
    g_frame_timer_fd = fd;
}

// Check if a frame needs to be scheduled (for events.c)
bool itn_render_needs_frame_scheduled(void) {
    // Check if we have pending damage that needs rendering
    // Don't check g_frame_scheduled here - that's for preventing double-scheduling
    return damage_pending && !g_frame_scheduled;
}

// Get the next frame time for scheduling (for events.c)
struct timespec itn_render_get_next_frame_time(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    // Frame interval in nanoseconds (e.g., 1000000000 / 120 = 8333333 for 120Hz)
    const long frame_interval_ns = 1000000000L / g_target_fps;

    struct timespec next_frame = now;
    next_frame.tv_nsec += frame_interval_ns;
    if (next_frame.tv_nsec >= 1000000000L) {
        next_frame.tv_sec++;
        next_frame.tv_nsec -= 1000000000L;
    }

    return next_frame;
}

// Helper: Calculate time difference in milliseconds
static double time_diff_ms(struct timespec *start, struct timespec *end) {
    return (end->tv_sec - start->tv_sec) * 1000.0 +
           (end->tv_nsec - start->tv_nsec) / 1000000.0;
}

// Update metrics from compositor (called by itn_composite)
void itn_render_update_metrics(int composite_calls, uint64_t pixels, int visible) {
    metrics.composite_calls += composite_calls;
    metrics.pixels_actually_drawn += pixels;
    metrics.visible_windows = visible;
}

// Log performance metrics (migrated from old compositor)
void itn_render_log_metrics(void) {
    if (metrics.frame_count == 0) {
        log_error("[METRICS] No frames rendered yet");
        return;
    }

    // Calculate actual elapsed time
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed_seconds = (now.tv_sec - metrics.metrics_start_time.tv_sec) +
                            (now.tv_nsec - metrics.metrics_start_time.tv_nsec) / 1000000000.0;

    double avg_frame = metrics.total_frame_time_ms / metrics.frame_count;
    // Calculate ACTUAL RPS based on frames rendered over elapsed time
    double actual_rps = (elapsed_seconds > 0) ? metrics.frame_count / elapsed_seconds : 0;
    // Calculate theoretical max RPS based on frame render time
    double max_rps = (avg_frame > 0) ? 1000.0 / avg_frame : 0;

    log_error("[METRICS] ===== Performance Snapshot =====");
    log_error("[METRICS] Compositor: %s", itn_composite_is_active() ? "ACTIVE" : "INACTIVE");

    log_error("[METRICS] Frame Statistics:");
    log_error("[METRICS]   Frames rendered: %lu", metrics.frame_count);
    log_error("[METRICS]   Actual FPS: %.1f (frames/second)", actual_rps);
    log_error("[METRICS]   Max possible FPS: %.1f (based on render time)", max_rps);
    log_error("[METRICS]   Avg frame time: %.2f ms", avg_frame);
    log_error("[METRICS]   Worst frame time: %.2f ms", metrics.worst_frame_time_ms);

    log_error("[METRICS] Render Statistics:");
    log_error("[METRICS]   Full repaints: %lu", metrics.full_repaints);
    log_error("[METRICS]   Damage events: %lu", metrics.damage_events);
    if (metrics.frame_count > 0) {
        log_error("[METRICS]   Damage events per frame: %.1f",
                  (double)metrics.damage_events / metrics.frame_count);
    }

    log_error("[METRICS] GPU Operations:");
    log_error("[METRICS]   XRenderComposite calls: %lu", metrics.composite_calls);
    if (metrics.frame_count > 0) {
        log_error("[METRICS]   Composites per frame: %.1f",
                  (double)metrics.composite_calls / metrics.frame_count);
    }

    log_error("[METRICS] Window Statistics:");
    log_error("[METRICS]   Windows tracked: %d", itn_manager_get_count());
    log_error("[METRICS]   Visible windows: %d", metrics.visible_windows);

    // Repaint reason breakdown
    if (metrics.repaints_damage + metrics.repaints_configure + metrics.repaints_map > 0) {
        log_error("[METRICS] Repaint Triggers:");
        if (metrics.frame_count > 0) {
            log_error("[METRICS]   Damage events: %lu (%.1f%%)", metrics.repaints_damage,
                      (100.0 * metrics.repaints_damage) / metrics.frame_count);
            log_error("[METRICS]   Configure events: %lu (%.1f%%)", metrics.repaints_configure,
                      (100.0 * metrics.repaints_configure) / metrics.frame_count);
            log_error("[METRICS]   Map/Unmap events: %lu (%.1f%%)", metrics.repaints_map,
                      (100.0 * metrics.repaints_map) / metrics.frame_count);
        }
    }

    // Pixel efficiency tracking
    if (metrics.pixels_actually_drawn > 0) {
        log_error("[METRICS] Pixel Efficiency:");
        double megapixels_drawn = metrics.pixels_actually_drawn / 1000000.0;
        log_error("[METRICS]   Total megapixels drawn: %.1f", megapixels_drawn);
        if (metrics.frame_count > 0) {
            log_error("[METRICS]   Megapixels per frame: %.2f",
                      megapixels_drawn / metrics.frame_count);
        }
    }

    // CPU usage calculation (reuse 'now' from above)
    // now already has current time from earlier calculation

    // Use metrics_start_time if available, otherwise fall back to start_time
    struct timespec start_time_spec;
    if (metrics.metrics_start_time.tv_sec > 0) {
        start_time_spec = metrics.metrics_start_time;
    } else if (metrics.start_time > 0) {
        start_time_spec.tv_sec = metrics.start_time;
        start_time_spec.tv_nsec = 0;
    } else {
        // Initialize if not set
        clock_gettime(CLOCK_MONOTONIC, &metrics.metrics_start_time);
        start_time_spec = metrics.metrics_start_time;
    }

    double total_elapsed_ms = time_diff_ms(&start_time_spec, &now);
    if (total_elapsed_ms > 0 && metrics.total_frame_time_ms > 0) {
        double cpu_percent = (metrics.total_frame_time_ms / total_elapsed_ms) * 100.0;
        log_error("[METRICS] CPU Usage:");
        log_error("[METRICS]   Compositor CPU: %.1f%% (%.1fms work in %.1fms elapsed)",
                  cpu_percent, metrics.total_frame_time_ms, total_elapsed_ms);
    }

    log_error("[METRICS] =============================");

    // Reset metrics for next interval
    metrics = (typeof(metrics)){0};
    metrics.start_time = time(NULL);
    clock_gettime(CLOCK_MONOTONIC, &metrics.metrics_start_time);
    metrics.last_frame_time = metrics.metrics_start_time;
}