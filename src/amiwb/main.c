// File: main.c
#include "intuition/itn_internal.h"
#include "menus/menu_public.h"
#include "dialogs.h"
#include "iconinfo.h"
#include "workbench/wb_public.h"
#include "events.h"
// #include "compositor.h"  // Now using itn modules
#include "render.h"
#include "config.h"
#include "amiwbrc.h"  // For config loading
#include <X11/Xlib.h>
#include <stdio.h> // For fprintf
#include <stdlib.h> // getenv
#include <unistd.h> // dup2
#include <fcntl.h>  // open, O_WRONLY
#include <time.h>   // time, strftime
#include <string.h>
#include <stdarg.h> // For va_list

// Global variables for restart functionality
static char **g_argv = NULL;
static int g_argc = 0;
static Window g_selection_window = None;
static Display *g_selection_display = NULL;

// Error logging function - only logs actual errors
void log_error(const char *format, ...) {
    char log_path[PATH_SIZE];
    const char *cfg = LOG_FILE_PATH;
    
    // Expand leading $HOME in the configured path
    if (cfg && strncmp(cfg, "$HOME/", 6) == 0) {
        const char *home = getenv("HOME");
        if (!home) return;  // Silent fail - no logs if no home
        snprintf(log_path, sizeof(log_path), "%s/%s", home, cfg + 6);
    } else {
        snprintf(log_path, sizeof(log_path), "%s", cfg ? cfg : "amiwb.log");
    }
    
    // Open, write, close immediately - no fd inheritance
    FILE *log = fopen(log_path, "a");
    if (!log) return;  // Silent fail - don't break on log errors
    
    // Add timestamp
    time_t now;
    time(&now);
    struct tm *tm_info = localtime(&now);
    fprintf(log, "[%02d:%02d:%02d] ", tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
    
    va_list args;
    va_start(args, format);
    vfprintf(log, format, args);
    va_end(args);
    fprintf(log, "\n");
    
    fclose(log);
}

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
    extern void itn_core_shutdown_compositor(void);
    itn_core_shutdown_compositor();
    cleanup_render();     // Clean up Xft fonts BEFORE closing display
    cleanup_intuition();  // This closes the X display
    
    // Clean up file descriptors before exec to prevent inheritance
    // Reset stdout/stderr to clean state
    int devtty = open("/dev/tty", O_WRONLY);
    if (devtty != -1) {
        dup2(devtty, STDOUT_FILENO);
        dup2(devtty, STDERR_FILENO);
        close(devtty);
    } else {
        // If no tty available, redirect to /dev/null
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull != -1) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
    }
    
    // Close all other file descriptors except stdin/stdout/stderr
    for (int fd = 3; fd < 256; fd++) {
        close(fd);
    }
    
    // Replace ourselves with a new instance
    execvp(g_argv[0], g_argv);
    
    // If exec fails, exit with error
    log_error("[ERROR] Failed to restart AmiWB: execvp failed");
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
    // Initialize log file with timestamp header (truncate on each run)
    #if LOGGING_ENABLED
    {
        char path_buf[PATH_SIZE];
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
            // Header with timestamp - only thing that goes in log during normal operation
            time_t now = time(NULL);
            struct tm tm; localtime_r(&now, &tm);
            char ts[128];
            strftime(ts, sizeof(ts), "%a %d %b %Y - %H:%M", &tm);
            fprintf(lf, "AmiWB log file, started on: %s\n", ts);
            fprintf(lf, "----------------------------------------\n");
            fclose(lf);  // Close immediately - no fd inheritance

            // NOTE: No stderr redirection - child processes need clean stderr
            // Toolkit errors go to terminal, not log file
            // TODO: Implement callback system for toolkit logging
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
    grab_global_shortcuts(itn_core_get_display(), DefaultRootWindow(itn_core_get_display()));

    // Rendering second: needs the RenderContext built by intuition
    init_render();

    // Initialize menus
    init_menus();
    
    // Initialize dialogs
    init_dialogs();
    
    // Initialize icon info dialogs
    init_iconinfo();
    
    // Initialize workbench
    init_workbench();

    // Initialize XDND support
    extern void xdnd_init(Display *dpy);
    xdnd_init(itn_core_get_display());

    // Initialize disk drives detection
    extern void diskdrives_init(void);
    diskdrives_init();
    
    // Initialize events
    init_events();

    // Initialize g_display for intuition modules (temporary during migration)
    extern Display *g_display;
    g_display = itn_core_get_display();

    // Start compositor - MANDATORY, no fallback!
    extern bool itn_core_init_compositor(void);
    if (!itn_core_init_compositor()) {
        fprintf(stderr, "FATAL: Compositor initialization failed.\n");
        fprintf(stderr, "Hardware acceleration is MANDATORY - no fallback, no compromise.\n");
        fprintf(stderr, "Check amiwb.log for details.\n");
        cleanup_workbench();
        cleanup_menus();
        cleanup_intuition();
        return EXIT_FAILURE;
    }
    
    // Start event loop
    handle_events();
    
    // Clean up
    // Reverse init order to avoid dangling X resources during teardown.
    // Enter shutdown mode to suppress benign X errors
    begin_shutdown();
    // Stop compositor before closing the Display in cleanup_intuition()
    extern void itn_core_shutdown_compositor(void);
    itn_core_shutdown_compositor();
    // Then tear down UI modules
    cleanup_menus();
    cleanup_dialogs();
    cleanup_iconinfo();
    extern void diskdrives_cleanup(void);
    diskdrives_cleanup();
    cleanup_workbench();
    extern void xdnd_shutdown(Display *dpy);
    xdnd_shutdown(itn_core_get_display());
    // Clean up render resources BEFORE closing Display (Xft fonts need display)
    cleanup_render();
    // Finally close Display connection
    cleanup_intuition();
    
    // Clean up instance selection
    if (g_selection_display && g_selection_window != None) {
        XDestroyWindow(g_selection_display, g_selection_window);
        XCloseDisplay(g_selection_display);
    }
    
    return 0;
}