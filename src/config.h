/* 
Configuration header: Defines constants like colors, thresholds, and types for the project. 
Used to centralize tunable parameters. TODO: figure some sort of file based settings storage.
a dot file .amiwbrc or many dedicated setting files like on amiga env:archives/ ...
*/

#ifndef CONFIG_H
#define CONFIG_H

#include <X11/extensions/Xrender.h>  // XRenderColor

// TODO: remove old colors (replaced by global colors below)
//#define BG_COLOR_DESKTOP (XRenderColor){0x5555, 0x5555, 0x5555, 0xFFFF} // Desktop background
//#define BG_COLOR_FOLDER (XRenderColor){0xAAAA, 0xAAAA, 0xAAAA, 0xFFFF} // Folder background 
#define BLUE_FRAME (XRenderColor){0x0000, 0x0000, 0xFFFF, 0xFFFF} // Active frame (blue).
#define GRAY_FRAME (XRenderColor){0x8888, 0x8888, 0x8888, 0xFFFF} // Inactive frame (gray).

// global colors
#define BLACK (XRenderColor){0x0000, 0x0000, 0x0000, 0xFFFF}
#define WHITE (XRenderColor){0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}
#define BLUE (XRenderColor){0x4858, 0x6F6F, 0xB0B0, 0xFFFF}
#define GRAY (XRenderColor){0xa0a0, 0xa2a2, 0xa0a0, 0xffff}

#define DOUBLE_CLICK_TIME 1000 	// Time threshold for double-click in ms.
#define CLICK_TOLERANCE 10 		// Pixel tolerance for double-click.
#define DRAG_THRESHOLD 10 		// Pixel threshold to start drag.
#define ICON_SPACING 80 		// Spacing between icons in grid (increased).
#define MAX_FILES 10000 		// Max icons per canvas.

// frame sizes
#define BORDER_HEIGHT_TOP 20 	// height of titlebar.
#define BORDER_WIDTH_LEFT 8 	// width of left border
#define BORDER_WIDTH_RIGHT 20  	// width of right border
#define BORDER_HEIGHT_BOTTOM 20 // height of bottom border

// frame buttons sizes
#define BUTTON_CLOSE_SIZE 30 
#define BUTTON_ICONIFY_SIZE 20
#define BUTTON_MAXIMIZE_SIZE 20
#define BUTTON_LOWER_SIZE 20
#define BUTTON_RESIZE_SIZE 20

#define GLOBAL_DEPTH 32 		// Set to 8, 16, 24, or 32; fallback to default if not available
#define TYPE_ICONIFIED 3 		// Type for iconified windows.
#define MAX_WINDOWS 100 		// Max open windows.
#define ICON_HEADER_SIZE 20 	// Size of icon file header.
#define TYPE_TOOL 0 			// Type for tool/file icon.
#define TYPE_DRAWER 1 			// Type for drawer/folder icon.
#define MENUBAR_HEIGHT 20 		// Height of menubar.
#define MENU_ITEM_HEIGHT 20 	// Height of menu item.

//extern char *def_tool_path;  // Default icons path
//extern char *def_drawer_path;
extern char *iconify_path;

#define max(a,b) ((a) > (b) ? (a) : (b))
#define min(a,b) ((a) < (b) ? (a) : (b))

#define DEBUG false  		// enable debug output
#define MIN_KNOB_SIZE 10  	// Minimum size for scrollbar knobs in pixels

#endif