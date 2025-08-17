#ifndef CONFIG_H
#define CONFIG_H

#include <X11/extensions/Xrender.h>  // XRenderColor


// global colors
#define BLACK (XRenderColor){0x0000, 0x0000, 0x0000, 0xFFFF}
#define WHITE (XRenderColor){0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}
#define BLUE (XRenderColor){0x4858, 0x6F6F, 0xB0B0, 0xFFFF}
#define GRAY (XRenderColor){0xa0a0, 0xa2a2, 0xa0a0, 0xffff}

// font colors for desktop and windows icons
#define DESKFONTCOL WHITE
#define WINFONTCOL BLACK

#define DOUBLE_CLICK_TIME 1000 	// Time threshold for double-click in ms.
#define CLICK_TOLERANCE 10 		// Pixel tolerance for double-click.
#define DRAG_THRESHOLD 10 		// Pixel threshold to start drag.
#define ICON_SPACING 70 		// Spacing between icons in grid (increased).
#define MAX_FILES 10000 		// Max icons per canvas.

// frame sizes
#define BORDER_HEIGHT_TOP 20 	// height of titlebar.
#define BORDER_WIDTH_LEFT 8 	// width of left border
#define BORDER_WIDTH_RIGHT 20  	// width of right border
#define BORDER_HEIGHT_BOTTOM 20 // height of bottom border

// frame buttons sizes
#define BUTTON_CLOSE_SIZE 30 
#define BUTTON_ICONIFY_SIZE 30
#define BUTTON_MAXIMIZE_SIZE 30
#define BUTTON_LOWER_SIZE 30
#define BUTTON_RESIZE_SIZE 20

#define GLOBAL_DEPTH 24 		// Set to 8, 16, 24, or 32
#define TYPE_ICONIFIED 3 		// Type for iconified windows.
#define MAX_WINDOWS 100 		// Max open windows.
#define ICON_HEADER_SIZE 20 	// Size of icon file header.
#define TYPE_TOOL 0 			// Type for tool/file icon.
#define TYPE_DRAWER 1 			// Type for drawer/folder icon.
#define MENUBAR_HEIGHT 20 		// Height of menubar.
#define MENU_ITEM_HEIGHT 20 	// Height of menu item.
#define MENU_SHOW_DATE 1        // Show date/time on menubar (0=off, 1=on)

// "%I:%M %p" = 02:00pm
// "%a %e %b %H:%M" = Sun 17 Aug 02:00
#define MENUBAR_DATE_FORMAT "%I:%M %p"  // Date/time format for menubar (see strftime)

// Global shortcut symbol (Unicode 2237: ∷)
#define SHORTCUT_SYMBOL "\xe2\x88\xb7"  // UTF-8 encoding of ∷ (U+2237)

extern char *iconify_path;

//#define SYSFONT "fonts/SourceCodePro-Semibold.otf"
//#define SYSFONT "fonts/SourceCodePro-Regular.otf"
#define SYSFONT "fonts/SourceCodePro-Bold.otf"

#define max(a,b) ((a) > (b) ? (a) : (b))
#define min(a,b) ((a) < (b) ? (a) : (b))

#define DEBUG false  		// enable debug output
#define MIN_KNOB_SIZE 10  	// Minimum size for scrollbar knobs in pixels

// desktop and windows backgrounds, with tile option 0/1
#define DESKPICT "/usr/local/share/amiwb/patterns/atom_art.png"
#define DESKTILE 0
#define WINDPICT "/usr/local/share/amiwb/patterns/pattern8.png"
#define WINDTILE 1

// Logging configuration
// When LOGGING_ENABLED is 1, AmiWB redirects stdout/stderr to LOG_FILE_PATH,
// truncating the file at startup and printing a timestamp header.
// If LOG_CAP_ENABLED is 1, the event loop enforces a size cap (LOG_CAP_BYTES)
// by truncating the file when it grows beyond the cap.
#define LOGGING_ENABLED 1
#define LOG_FILE_PATH "$HOME/Sources/amiwb/amiwb.log" //"$HOME/.config/amiwb/amiwb.log"
#define LOG_CAP_ENABLED 0
#define LOG_CAP_BYTES (5 * 1024 * 1024)
#endif