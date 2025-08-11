// File: main.c
#include "intuition.h"
#include "menus.h"
#include "workbench.h"
#include "events.h"
#include "compositor.h"
#include "render.h"
#include "config.h"
#include <X11/Xlib.h>
#include <stdio.h> // For fprintf
#include <stdlib.h> // getenv
#include <unistd.h> // dup2
#include <time.h>   // time, strftime
#include <string.h>

// Entry point
// Initializes subsystems in the order they depend on each other and
// shuts them down in reverse. No behavior changes.
// Order matters:
// 1) init_intuition() creates Display and RenderContext
// 2) init_render() uses RenderContext (fonts, wallpapers)
// 3) init_menus(), init_workbench() create canvases that render
// 4) init_events() hooks dispatcher, then compositor starts
// Initialize window manager components
int main(int argc, char *argv[]) {
    // Configurable logging: redirect stdout/stderr to LOG_FILE_PATH when enabled.
    // Truncate on each run and print a timestamp header. Line-buffered writes.
    #if LOGGING_ENABLED
    {
        char path_buf[1024];
        const char *cfg = LOG_FILE_PATH;
        // Expand leading $HOME in the configured path for fopen()
        if (cfg && strncmp(cfg, "$HOME/", 6) == 0) {
            const char *home = getenv("HOME");
            if (home) {
                snprintf(path_buf, sizeof(path_buf), "%s/%s", home, cfg + 6);
            } else {
                snprintf(path_buf, sizeof(path_buf), "%s", cfg); // fallback
            }
        } else {
            snprintf(path_buf, sizeof(path_buf), "%s", cfg ? cfg : "amiwb.log");
        }
        FILE *lf = fopen(path_buf, "w"); // overwrite each run
        if (lf) {
            setvbuf(lf, NULL, _IOLBF, 0); // line-buffered file (harmless)
            dup2(fileno(lf), fileno(stdout));
            dup2(fileno(lf), fileno(stderr));
            // Ensure stdout/stderr are line-buffered so tail sees output promptly
            setvbuf(stdout, NULL, _IOLBF, 0);
            setvbuf(stderr, NULL, _IOLBF, 0);
            // Header with timestamp
            time_t now = time(NULL);
            struct tm tm; localtime_r(&now, &tm);
            char ts[128];
            strftime(ts, sizeof(ts), "%a %d %b %Y - %H:%M", &tm);
            fprintf(stderr, "AmiWB log file, started on: %s\n", ts);
            fprintf(stderr, "----------------------------------------\n");
            fflush(stderr);
        }
    }
    #endif
    fprintf(stderr, "Starting amiwb\n");
    
    // Intuition first: sets up X Display and RenderContext
    fprintf(stderr, "Calling init_intuition\n");
    init_intuition();

    // Rendering second: needs the RenderContext built by intuition
    fprintf(stderr, "Calling init_render\n");
    init_render();

    // Initialize menus
    fprintf(stderr, "Calling init_menus\n");
    init_menus();
    
    // Initialize workbench
    fprintf(stderr, "Calling init_workbench\n");
    init_workbench();
    
    // Initialize events
    fprintf(stderr, "Calling init_events\n");
    init_events();
    // Start compositor after events are ready. If it fails, continue.
    fprintf(stderr, "Calling init_compositor\n");
    if (!init_compositor(get_display())) {
        fprintf(stderr, "Compositor: could not acquire selection, continuing without\n");
    }
    
    // Start event loop
    fprintf(stderr, "Starting event loop\n");
    handle_events();
    
    // Clean up
    // Reverse init order to avoid dangling X resources during teardown.
    fprintf(stderr, "Cleaning up\n");
    // Enter shutdown mode to suppress benign X errors
    begin_shutdown();
    // Stop compositor before closing the Display in cleanup_intuition()
    shutdown_compositor(get_display());
    // Then tear down UI modules
    cleanup_menus();
    cleanup_workbench();
    // Finally close Display and render resources
    cleanup_intuition();
    cleanup_render();
    
    fprintf(stderr, "Exiting amiwb\n");
    return 0;
}