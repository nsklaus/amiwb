// File: main.c
#include "intuition.h"
#include "menus.h"
#include "dialogs.h"
#include "workbench.h"
#include "events.h"
#include "compositor.h"
#include "render.h"
#include "config.h"
#include "amiwbrc.h"  // For config loading
#include <X11/Xlib.h>
#include <stdio.h> // For fprintf
#include <stdlib.h> // getenv
#include <unistd.h> // dup2
#include <time.h>   // time, strftime
#include <string.h>

// Global variables for restart functionality
static char **g_argv = NULL;
static int g_argc = 0;
static Window g_selection_window = None;
static Display *g_selection_display = NULL;

// Function to restart the window manager
void restart_amiwb(void) {
    // Mark that we're restarting, not shutting down
    begin_restart();
    
    // Clean up instance selection first
    if (g_selection_display && g_selection_window != None) {
        XDestroyWindow(g_selection_display, g_selection_window);
        XCloseDisplay(g_selection_display);
    }
    
    // Perform minimal cleanup - we're about to exec
    begin_shutdown();
    shutdown_compositor(get_display());
    cleanup_intuition();  // This closes the X display
    
    // Replace ourselves with a new instance
    execvp(g_argv[0], g_argv);
    
    // If exec fails, exit with error
    fprintf(stderr, "Failed to restart AmiWB: execvp failed\n");
    exit(1);
}

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
    // Store arguments for potential restart
    g_argc = argc;
    g_argv = argv;
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
    
    // Check if another instance is already running by trying to own a selection
    g_selection_display = XOpenDisplay(NULL);
    if (g_selection_display) {
        Atom wm_selection = XInternAtom(g_selection_display, "_AMIWB_WM_S0", False);
        Window owner = XGetSelectionOwner(g_selection_display, wm_selection);
        
        if (owner != None) {
            // Another instance is running
            fprintf(stderr, "AmiWB is already running. Only one instance allowed.\n");
            XCloseDisplay(g_selection_display);
            return 1;
        }
        
        // Try to claim ownership
        g_selection_window = XCreateSimpleWindow(g_selection_display, 
                                                DefaultRootWindow(g_selection_display), 
                                                0, 0, 1, 1, 0, 0, 0);
        XSetSelectionOwner(g_selection_display, wm_selection, g_selection_window, CurrentTime);
        XSync(g_selection_display, False);
        
        // Verify we got it
        if (XGetSelectionOwner(g_selection_display, wm_selection) != g_selection_window) {
            fprintf(stderr, "Failed to acquire WM selection. Another WM may be running.\n");
            XDestroyWindow(g_selection_display, g_selection_window);
            XCloseDisplay(g_selection_display);
            g_selection_window = None;
            g_selection_display = NULL;
            return 1;
        }
        
        // Keep the display and window for the lifetime of the program
    }
    
    // Load configuration from ~/.config/amiwb/amiwbrc
    // Do this early so all init functions can use config values
    load_config();
    
    // Intuition first: sets up X Display and RenderContext
    init_intuition();
    
    // Grab global shortcuts so applications can't intercept them
    grab_global_shortcuts(get_display(), DefaultRootWindow(get_display()));

    // Rendering second: needs the RenderContext built by intuition
    init_render();

    // Initialize menus
    init_menus();
    
    // Initialize dialogs
    init_dialogs();
    
    // Initialize workbench
    init_workbench();
    
    // Initialize events
    init_events();
    // Start compositor after events are ready. If it fails, continue.
    if (!init_compositor(get_display())) {
        fprintf(stderr, "Compositor: could not acquire selection, continuing without\n");
    }
    
    // Start event loop
    handle_events();
    
    // Clean up
    // Reverse init order to avoid dangling X resources during teardown.
    // Enter shutdown mode to suppress benign X errors
    begin_shutdown();
    // Stop compositor before closing the Display in cleanup_intuition()
    shutdown_compositor(get_display());
    // Then tear down UI modules
    cleanup_menus();
    cleanup_dialogs();
    cleanup_workbench();
    // Finally close Display and render resources
    cleanup_intuition();
    cleanup_render();
    
    // Clean up instance selection
    if (g_selection_display && g_selection_window != None) {
        XDestroyWindow(g_selection_display, g_selection_window);
        XCloseDisplay(g_selection_display);
    }
    
    return 0;
}