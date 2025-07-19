/* Configuration header: Defines constants like colors, thresholds, and types for the project. Used to centralize tunable parameters. */

#ifndef CONFIG_H
#define CONFIG_H

#include <X11/extensions/Xrender.h>  // XRenderColor

#define BG_COLOR_DESKTOP (XRenderColor){0x5555, 0x5555, 0x5555, 0xFFFF} // Desktop background color (gray).
#define BG_COLOR_FOLDER (XRenderColor){0xAAAA, 0xAAAA, 0xAAAA, 0xFFFF} // Folder background color (lighter gray).
#define BLUE_FRAME (XRenderColor){0x0000, 0x0000, 0xFFFF, 0xFFFF} // Active frame color (blue).
#define GRAY_FRAME (XRenderColor){0x8888, 0x8888, 0x8888, 0xFFFF} // Inactive frame color (gray).
#define DOUBLE_CLICK_TIME 1000 // Time threshold for double-click in ms.
#define CLICK_TOLERANCE 10 // Pixel tolerance for double-click.
#define DRAG_THRESHOLD 10 // Pixel threshold to start drag.
#define ICON_SPACING 80 // Spacing between icons in grid (increased).
#define MAX_FILES 10000 // Max icons per canvas.
#define TITLEBAR_HEIGHT 20 // Height of titlebar.
#define BORDER_WIDTH 8 // Width of border.
#define TYPE_ICONIFIED 3 // Type for iconified windows.
#define MAX_WINDOWS 100 // Max open windows.
#define ICON_HEADER_SIZE 20 // Size of icon file header.
#define TYPE_TOOL 0 // Type for tool/file icon.
#define TYPE_DRAWER 1 // Type for drawer/folder icon.
#define MENUBAR_HEIGHT 20 // Height of menubar.
#define MENU_ITEM_HEIGHT 20 // Height of menu item.

extern char *def_tool_path;  // Default icons path
extern char *def_drawer_path;
extern char *iconify_path;

#endif