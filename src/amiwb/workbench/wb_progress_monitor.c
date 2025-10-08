// File: wb_progress_monitor.c
// Progress monitoring for async file operations
// Tracks background child processes and optionally shows progress UI

#include "wb_internal.h"
#include "../render_public.h"
#include "../intuition/itn_internal.h"
#include "../font_manager.h"
#include "../../toolkit/progressbar/progressbar.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

// Button dimensions (same as dialogs)
#define BUTTON_WIDTH 80
#define BUTTON_HEIGHT 25

// ============================================================================
// Module-Private State
// ============================================================================

// Global list of active progress monitors
static ProgressMonitor *g_progress_monitors = NULL;

// ============================================================================
// Progress Monitor List Management
// ============================================================================

// Add monitor to global list (internal)
static void add_monitor_to_list(ProgressMonitor *monitor) {
    if (!monitor) return;
    monitor->next = g_progress_monitors;
    g_progress_monitors = monitor;
}

// Remove monitor from global list (internal)
static void remove_monitor_from_list(ProgressMonitor *monitor) {
    if (!monitor) return;

    if (g_progress_monitors == monitor) {
        g_progress_monitors = monitor->next;
    } else {
        for (ProgressMonitor *m = g_progress_monitors; m; m = m->next) {
            if (m->next == monitor) {
                m->next = monitor->next;
                break;
            }
        }
    }
}

// ============================================================================
// Progress Monitor Lookup and Query
// ============================================================================

// Check if canvas is a progress monitor window
bool wb_progress_monitor_is_canvas(Canvas *canvas) {
    if (!canvas) return false;
    for (ProgressMonitor *monitor = g_progress_monitors; monitor; monitor = monitor->next) {
        if (monitor->canvas == canvas) return true;
    }
    return false;
}

// Get monitor for canvas
ProgressMonitor* wb_progress_monitor_get_for_canvas(Canvas *canvas) {
    if (!canvas) return NULL;
    for (ProgressMonitor *monitor = g_progress_monitors; monitor; monitor = monitor->next) {
        if (monitor->canvas == canvas) return monitor;
    }
    return NULL;
}

// Get all monitors (for polling in event loop)
ProgressMonitor* wb_progress_monitor_get_all(void) {
    return g_progress_monitors;
}

// ============================================================================
// Progress Window Creation (internal helper)
// ============================================================================

// Create progress window canvas (internal - called after threshold)
static Canvas* create_progress_window(ProgressOperation op, const char *title) {
    // Determine proper title based on operation
    const char *window_title = title;
    if (!window_title) {
        switch (op) {
            case PROGRESS_COPY: window_title = "Copying Files"; break;
            case PROGRESS_MOVE: window_title = "Moving Files"; break;
            case PROGRESS_DELETE: window_title = "Deleting Files"; break;
            case PROGRESS_EXTRACT: window_title = "Extracting Archive"; break;
            default: window_title = "Progress"; break;
        }
    }

    // Center on screen
    Display *dpy = itn_core_get_display();
    int screen = DefaultScreen(dpy);
    int screen_width = DisplayWidth(dpy, screen);
    int screen_height = DisplayHeight(dpy, screen);
    int x = (screen_width - 400) / 2;
    int y = (screen_height - 164) / 2;

    Canvas *canvas = create_canvas(NULL, x, y, 400, 164, DIALOG);
    if (!canvas) {
        log_error("[ERROR] create_progress_window: failed to create canvas");
        return NULL;
    }

    // Set title for AmiWB window rendering
    canvas->title_base = strdup(window_title);

    // DO NOT make it modal - user should be able to continue working
    // Show, raise and make it the active window
    XMapRaised(dpy, canvas->win);
    itn_focus_set_active(canvas);
    XSync(dpy, False);

    return canvas;
}

// ============================================================================
// Progress Monitor Lifecycle
// ============================================================================

// Create progress monitor with UI (shows window immediately)
ProgressMonitor* wb_progress_monitor_create(ProgressOperation op, const char *title) {
    ProgressMonitor *monitor = calloc(1, sizeof(ProgressMonitor));
    if (!monitor) return NULL;

    monitor->operation = op;
    monitor->percent = 0.0f;
    monitor->current_file[0] = '\0';
    monitor->pipe_fd = -1;
    monitor->child_pid = 0;
    monitor->abort_requested = false;
    monitor->on_abort = NULL;

    // Create canvas window (400x164)
    monitor->canvas = create_canvas(NULL, 200, 150, 400, 164, DIALOG);
    if (!monitor->canvas) {
        log_error("[ERROR] wb_progress_monitor_create: failed to create canvas");
        free(monitor);
        return NULL;
    }

    // Set title based on operation
    const char *op_title = title ? title :
        (op == PROGRESS_MOVE ? "Moving Files..." :
         op == PROGRESS_COPY ? "Copying Files..." :
         op == PROGRESS_DELETE ? "Deleting Files..." :
         "Processing...");

    monitor->canvas->title_base = strdup(op_title);
    monitor->canvas->title_change = NULL;
    monitor->canvas->bg_color = GRAY;
    monitor->canvas->disable_scrollbars = true;

    // Add to monitor list
    add_monitor_to_list(monitor);

    // Show the window
    XMapRaised(itn_core_get_display(), monitor->canvas->win);
    itn_focus_set_active(monitor->canvas);
    XSync(itn_core_get_display(), False);

    redraw_canvas(monitor->canvas);
    XFlush(itn_core_get_display());

    return monitor;
}

// Create background progress monitor (no UI initially, for child process tracking)
ProgressMonitor* wb_progress_monitor_create_background(ProgressOperation op, const char *filename,
                                                       int pipe_fd, pid_t child_pid) {
    ProgressMonitor *monitor = calloc(1, sizeof(ProgressMonitor));
    if (!monitor) {
        log_error("[ERROR] calloc failed for background ProgressMonitor");
        return NULL;
    }

    monitor->operation = op;
    monitor->pipe_fd = pipe_fd;
    monitor->child_pid = child_pid;
    monitor->start_time = time(NULL);
    monitor->canvas = NULL;  // No UI initially - created after threshold
    monitor->percent = -1.0f;
    strncpy(monitor->current_file, filename, PATH_SIZE - 1);
    monitor->current_file[PATH_SIZE - 1] = '\0';

    // Add to monitor list for polling
    add_monitor_to_list(monitor);

    return monitor;
}

// Update progress monitor state
void wb_progress_monitor_update(ProgressMonitor *monitor, const char *file, float percent) {
    if (!monitor) return;

    if (file) {
        strncpy(monitor->current_file, file, PATH_SIZE - 1);
        monitor->current_file[PATH_SIZE - 1] = '\0';
    }

    if (percent >= 0.0f && percent <= 100.0f) {
        monitor->percent = percent;
    }

    if (monitor->canvas) {
        redraw_canvas(monitor->canvas);
        XFlush(itn_core_get_display());
    }
}

// Close progress monitor
void wb_progress_monitor_close(ProgressMonitor *monitor) {
    if (!monitor) return;

    // Remove from list
    remove_monitor_from_list(monitor);

    // Clean up
    if (monitor->canvas) {
        itn_canvas_destroy(monitor->canvas);
    }
    if (monitor->progress_bar) {
        progressbar_destroy(monitor->progress_bar);
    }
    free(monitor);
}

// Close progress monitor by canvas (called from intuition when window X is clicked)
void wb_progress_monitor_close_by_canvas(Canvas *canvas) {
    if (!canvas) return;

    ProgressMonitor *monitor = wb_progress_monitor_get_for_canvas(canvas);
    if (monitor) {
        // If there's a child process running, abort it
        if (monitor->child_pid > 0) {
            // Set abort flag so child knows to clean up
            monitor->abort_requested = true;

            // Send SIGTERM to child process to trigger clean abort
            kill(monitor->child_pid, SIGTERM);

            // Don't remove from list or free here - let normal cleanup happen
            // when workbench_check_progress_monitors() detects child exit
            return;
        }

        // No child process - just clean up immediately
        remove_monitor_from_list(monitor);

        // Call abort callback if it exists
        if (monitor->on_abort) {
            monitor->on_abort();
        }

        // Don't destroy canvas here - intuition.c will do it
        monitor->canvas = NULL;

        free(monitor);
    }
}

// ============================================================================
// Progress Monitor Rendering
// ============================================================================

// Render progress monitor content (EXACT COPY of original render_progress_dialog_content)
void wb_progress_monitor_render(Canvas *canvas) {
    ProgressMonitor *dialog = wb_progress_monitor_get_for_canvas(canvas);
    if (!dialog) {
        return;
    }

    Display *dpy = itn_core_get_display();
    Picture dest = canvas->canvas_render;

    if (dest == None) {
        static bool dest_none_debug_done = false;
        if (!dest_none_debug_done) {
            log_error("[ERROR] wb_progress_monitor_render: canvas_render is None!");
            dest_none_debug_done = true;
        }
        return;
    }

    XftFont *font = font_manager_get();
    if (!font) {
        static bool no_font_debug_done = false;
        if (!no_font_debug_done) {
            log_error("[ERROR] wb_progress_monitor_render: no font!");
            no_font_debug_done = true;
        }
        return;
    }

    // Clear content area to dialog gray
    int content_x = BORDER_WIDTH_LEFT;
    int content_y = BORDER_HEIGHT_TOP;
    int content_w = canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT_CLIENT;
    int content_h = canvas->height - BORDER_HEIGHT_TOP - BORDER_HEIGHT_BOTTOM;

    XRenderFillRectangle(dpy, PictOpSrc, dest, &GRAY, content_x, content_y, content_w, content_h);

    // Use cached XftDraw
    if (!canvas->xft_draw) {
        static bool no_xft_debug_done = false;
        if (!no_xft_debug_done) {
            log_error("[ERROR] wb_progress_monitor_render: canvas->xft_draw is NULL!");
            no_xft_debug_done = true;
        }
        return;
    }

    XRenderColor text_color = BLACK;
    XftColor xft_text;
    XftColorAllocValue(dpy, canvas->visual, canvas->colormap, &text_color, &xft_text);

    // Line 1: Current file (at top, with operation prefix)
    int text_y = content_y + 20;
    const char *op_prefix = dialog->operation == PROGRESS_MOVE ? "Moving: " :
                           dialog->operation == PROGRESS_COPY ? "Copying: " :
                           dialog->operation == PROGRESS_DELETE ? "Deleting: " :
                           dialog->operation == PROGRESS_EXTRACT ? "Extracting: " :
                           "";

    char display_text[PATH_SIZE + 20];
    snprintf(display_text, sizeof(display_text), "%s%s", op_prefix, dialog->current_file);

    // Truncate with "..." if too long
    XGlyphInfo text_ext;
    XftTextExtentsUtf8(dpy, font, (FcChar8*)display_text, strlen(display_text), &text_ext);
    int max_width = content_w - 40;  // Leave margin

    if (text_ext.xOff > max_width) {
        // Truncate and add ...
        int len = strlen(display_text);
        while (len > 3) {
            display_text[len - 1] = '\0';
            display_text[len - 2] = '.';
            display_text[len - 3] = '.';
            display_text[len - 4] = '.';
            len -= 4;
            XftTextExtentsUtf8(dpy, font, (FcChar8*)display_text, strlen(display_text), &text_ext);
            if (text_ext.xOff <= max_width) break;
        }
    }

    XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, content_x + 20, text_y,
                     (FcChar8*)display_text, strlen(display_text));

    // Line 2: Bytes and file count (below filename)
    int info_y = text_y + font->height + 2;
    char info_text[256];

    // Check if totals are known yet (child process may still be counting)
    if (dialog->bytes_total == -1 || dialog->files_total == -1) {
        // Still counting files and bytes in child process
        snprintf(info_text, sizeof(info_text), "Calculating size...");
    } else {
        // Format bytes in human-readable form
        if (dialog->bytes_total < 1024 * 1024) {
            // Show in KB for small files
            double bytes_kb = dialog->bytes_done / 1024.0;
            double total_kb = dialog->bytes_total / 1024.0;
            snprintf(info_text, sizeof(info_text), "%.1f KB / %.1f KB  (%d/%d files)",
                    bytes_kb, total_kb, dialog->files_done, dialog->files_total);
        } else if (dialog->bytes_total < 1024 * 1024 * 1024) {
            // Show in MB
            double bytes_mb = dialog->bytes_done / (1024.0 * 1024.0);
            double total_mb = dialog->bytes_total / (1024.0 * 1024.0);
            snprintf(info_text, sizeof(info_text), "%.1f MB / %.1f MB  (%d/%d files)",
                    bytes_mb, total_mb, dialog->files_done, dialog->files_total);
        } else {
            // Show in GB for large files
            double bytes_gb = dialog->bytes_done / (1024.0 * 1024.0 * 1024.0);
            double total_gb = dialog->bytes_total / (1024.0 * 1024.0 * 1024.0);
            snprintf(info_text, sizeof(info_text), "%.2f GB / %.2f GB  (%d/%d files)",
                    bytes_gb, total_gb, dialog->files_done, dialog->files_total);
        }
    }

    XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, content_x + 20, info_y,
                     (FcChar8*)info_text, strlen(info_text));

    // Progress bar position - 10px closer to info text
    int bar_x = content_x + 20;
    int bar_y = info_y + font->height - 8;
    int bar_width = content_w - 40;  // Resizes with window
    int bar_height = (font->height * 2) - 8;

    // Create progress bar widget lazily if it doesn't exist
    if (!dialog->progress_bar) {
        dialog->progress_bar = progressbar_create(bar_x, bar_y, bar_width, bar_height, font);
        if (dialog->progress_bar) {
            progressbar_set_show_percentage(dialog->progress_bar, true);
        }
    }

    // Update and render progress bar using toolkit widget
    if (dialog->progress_bar) {
        progressbar_set_percent(dialog->progress_bar, dialog->percent);
        progressbar_render(dialog->progress_bar, dest, dpy, canvas->xft_draw);
    }

    // Abort button - centered horizontally, 10px below bar
    int button_x = content_x + (content_w - BUTTON_WIDTH) / 2;
    int button_y = bar_y + bar_height + 10;

    // Draw button with 3D effect
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE,
                        button_x, button_y, 1, BUTTON_HEIGHT);  // Left
    XRenderFillRectangle(dpy, PictOpSrc, dest, &WHITE,
                        button_x, button_y, BUTTON_WIDTH, 1);  // Top
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK,
                        button_x + BUTTON_WIDTH - 1, button_y, 1, BUTTON_HEIGHT);  // Right
    XRenderFillRectangle(dpy, PictOpSrc, dest, &BLACK,
                        button_x, button_y + BUTTON_HEIGHT - 1, BUTTON_WIDTH, 1);  // Bottom
    XRenderFillRectangle(dpy, PictOpSrc, dest, &GRAY,
                        button_x + 1, button_y + 1, BUTTON_WIDTH - 2, BUTTON_HEIGHT - 2);  // Fill

    // Draw "Abort" text
    XGlyphInfo abort_ext;
    XftTextExtentsUtf8(dpy, font, (FcChar8*)"Abort", 5, &abort_ext);
    int abort_text_x = button_x + (BUTTON_WIDTH - abort_ext.xOff) / 2;
    int abort_text_y = button_y + (BUTTON_HEIGHT + font->ascent) / 2 - 2;
    XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, abort_text_x, abort_text_y,
                     (FcChar8*)"Abort", 5);

    XftColorFree(dpy, canvas->visual, canvas->colormap, &xft_text);
}

// ============================================================================
// Progress Window Creation (Deferred - internal helper for wb_progress.c)
// ============================================================================

// Create progress window for existing monitor after threshold (internal use by wb_progress.c)
Canvas* wb_progress_monitor_create_window(ProgressMonitor *monitor, const char *title) {
    if (!monitor || monitor->canvas) return monitor ? monitor->canvas : NULL;

    monitor->canvas = create_progress_window(monitor->operation, title);
    return monitor->canvas;
}
