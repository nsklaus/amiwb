// Hobbler Configuration Header
#ifndef HOBBLER_CONFIG_H
#define HOBBLER_CONFIG_H

// Hobbler version
#define HOBBLER_VERSION "0.01"

// Buffer sizes for paths and filenames
#define PATH_SIZE 512           // Buffer size for URLs and file paths
#define NAME_SIZE 128           // Buffer size for file names

// Window dimensions
#define WINDOW_WIDTH 800        // Default window width
#define WINDOW_HEIGHT 600       // Default window height
#define TOOLBAR_HEIGHT 50       // Height of the toolbar

// Navigation button dimensions
#define NAV_BUTTON_WIDTH 60     // Width of back/forward buttons
#define NAV_BUTTON_HEIGHT 30    // Height of navigation buttons
#define HOME_BUTTON_WIDTH 50    // Width of home button
#define STOP_RELOAD_WIDTH 70    // Width of stop/reload button
#define GO_BUTTON_WIDTH 40      // Width of go button
#define BUTTON_PADDING 5        // Padding between buttons

// Colors
#define COLOR_TOOLBAR 0xBBBBBB  // Toolbar background color

// Default home page
#define DEFAULT_HOME_URL "file:///home/klaus/Documents/start.html"

// Log file path
#define HOBBLER_LOG_PATH "hobbler.log"

// Error logging function declaration
// Defined in hobbler.c
void log_error(const char *format, ...);

#endif // HOBBLER_CONFIG_H