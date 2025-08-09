// File: main.c
#include "intuition.h"
#include "menus.h"
#include "workbench.h"
#include "events.h"
#include "compositor.h"
#include "render.h"
#include <X11/Xlib.h>
#include <stdio.h> // For fprintf

// Initialize window manager components
int main(int argc, char *argv[]) {
    fprintf(stderr, "Starting amiwb\n");
    
    // Initialize intuition
    fprintf(stderr, "Calling init_intuition\n");
    init_intuition();

    // Initialize rendering
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
    // Initialize compositor
    fprintf(stderr, "Calling init_compositor\n");
    if (!init_compositor(get_display())) {
        fprintf(stderr, "Compositor: could not acquire selection, continuing without\n");
    }
    
    // Start event loop
    fprintf(stderr, "Starting event loop\n");
    handle_events();
    
    // Clean up
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