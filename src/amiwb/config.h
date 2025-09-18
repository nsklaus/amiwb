#ifndef CONFIG_H
#define CONFIG_H

// AmiWB version
#define AMIWB_VERSION "0.01"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>  // XRenderColor



// global colors
#define BLACK (XRenderColor){0x0000, 0x0000, 0x0000, 0xFFFF}
#define WHITE (XRenderColor){0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}
#define BLUE (XRenderColor){0x4858, 0x6F6F, 0xB0B0, 0xFFFF}
#define GRAY (XRenderColor){0xa0a0, 0xa2a2, 0xa0a0, 0xffff}

// font colors for desktop and windows icons
#define DESKFONTCOL WHITE
#define WINFONTCOL BLACK

#define DOUBLE_CLICK_TIME 1000  // Time threshold for double-click in ms.
#define CLICK_TOLERANCE 10      // Pixel tolerance for double-click.
#define DRAG_THRESHOLD 10       // Pixel threshold to start drag.
#define ICON_SPACING 70         // Spacing between icons in grid (increased).
#define MAX_FILES 10000         // Max icons per canvas.

// Buffer sizes for paths and filenames
// These are reasonable sizes that cover 99.9% of real-world usage
// while being memory-efficient for a lightweight desktop
#define PATH_SIZE 512           // Buffer size for file paths (typical paths are <300 chars)
#define NAME_SIZE 128           // Buffer size for file names (most names are <50 chars)
#define FULL_SIZE (PATH_SIZE + NAME_SIZE + 2)  // Buffer for path + "/" + filename + null

// frame sizes
// NOTE: Border sizes differ by window type:
// - Workbench windows (file manager): 20px all borders (for scrollbar/resize gadget)  
// - Client windows & dialogs: 8px left/right, 20px top/bottom
#define BORDER_HEIGHT_TOP 20    // height of titlebar (all windows)
#define BORDER_WIDTH_LEFT 8     // width of left border (all windows)
#define BORDER_WIDTH_RIGHT 20   // width of right border (workbench windows only)
#define BORDER_WIDTH_RIGHT_CLIENT 8  // width of right border (client windows)
#define BORDER_HEIGHT_BOTTOM 20 // height of bottom border (all windows)

// frame buttons sizes
#define BUTTON_CLOSE_SIZE 30 
#define BUTTON_ICONIFY_SIZE 30
#define BUTTON_MAXIMIZE_SIZE 30
#define BUTTON_LOWER_SIZE 30
#define BUTTON_RESIZE_SIZE 20

#define GLOBAL_DEPTH 24         // Set to 8, 16, 24, or 32
// TYPE_ICONIFIED is defined as enum in icons.h
#define MAX_WINDOWS 100         // Max open windows.
#define ICON_HEADER_SIZE 20     // Size of icon file header.
#define ICON_RENDER_DEPTH 32    // Icons need 32-bit for alpha channel
// TYPE_TOOL and TYPE_DRAWER are defined as enum in icons.h
#define MENUBAR_HEIGHT 20       // Height of menubar.
#define MENU_ITEM_HEIGHT 20     // Height of menu item.
#define MENU_SHOW_DATE 1        // Show date/time on menubar (0=off, 1=on)

// "%I:%M %p" = 02:00pm
// "%a %e %b %H:%M" = Sun 17 Aug 02:00
// #define MENUBAR_DATE_FORMAT "%I:%M %p"  // Date/time format for menubar (see strftime)
#define MENUBAR_DATE_FORMAT "%a.%e %b %H:%M"  // Date/time format for menubar (see strftime)

// Global shortcut symbol (Unicode 2237: ∷)
#define SHORTCUT_SYMBOL "\xe2\x88\xb7"  // UTF-8 encoding of ∷ (U+2237)

// Checkmark symbol (Unicode 2713: ✓)
#define CHECKMARK "\xe2\x9c\x93"  // UTF-8 encoding of ✓ (U+2713)

extern char *iconify_path;


#define RESOURCE_DIR_SYSTEM "/usr/local/share/amiwb"  
#define RESOURCE_DIR_USER ".config/amiwb"  

//#define SYSFONT "fonts/SourceCodePro-Semibold.otf"
//#define SYSFONT "fonts/SourceCodePro-Regular.otf"
#define SYSFONT "fonts/SourceCodePro-Bold.otf"

#define max(a,b) ((a) > (b) ? (a) : (b))
#define min(a,b) ((a) < (b) ? (a) : (b))

#define DEBUG false         // enable debug output
#define MIN_KNOB_SIZE 10    // Minimum size for scrollbar knobs in pixels

// Wallpaper settings now in ~/.config/amiwb/amiwbrc
// Removed: DESKPICT, DESKTILE, WINDPICT, WINDTILE

// Logging configuration
// When LOGGING_ENABLED is 1, AmiWB redirects stdout/stderr to LOG_FILE_PATH,
// truncating the file at startup and printing a timestamp header.
// If LOG_CAP_ENABLED is 1, the event loop enforces a size cap (LOG_CAP_BYTES)
// by truncating the file when it grows beyond the cap.
#define LOGGING_ENABLED 1
#define LOG_FILE_PATH "$HOME/Sources/amiwb/amiwb.log" //"$HOME/.config/amiwb/amiwb.log"
#define LOG_CAP_ENABLED 0
#define LOG_CAP_BYTES (5 * 1024 * 1024)

// ReqASL file dialog hook - replaces native file choosers with ReqASL
// This will intercept GTK3, Qt, and other toolkit file dialogs
#define REQASL_HOOK_PATH "/usr/local/lib/reqasl_hook.so"

// Error logging function - defined once in main.c, used everywhere
void log_error(const char *format, ...);

#endif