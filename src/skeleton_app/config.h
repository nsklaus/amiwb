/* config.h - Configuration constants for skeleton app */

/*
 * What could be added later:
 * - More color variations
 * - Debug/feature flags
 * - Default config file paths
 * - UI element sizes
 * - Keyboard shortcut definitions
 */

#ifndef SKELETON_CONFIG_H
#define SKELETON_CONFIG_H

#include <X11/extensions/Xrender.h>  /* For XRenderColor */

/* App identification */
#define APP_NAME    "Skeleton"
#define APP_VERSION "0.1"

/* Window dimensions */
#define WINDOW_WIDTH  800
#define WINDOW_HEIGHT 600

/* Buffer sizes - match AmiWB conventions */
#define PATH_SIZE 512
#define NAME_SIZE 128

/* AmiWB standard colors - keep consistent with main WM */
/* Use guards to prevent redefinition if toolkit includes main config.h */
#ifndef BLACK
#define BLACK (XRenderColor){0x0000, 0x0000, 0x0000, 0xFFFF}
#endif
#ifndef WHITE
#define WHITE (XRenderColor){0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}
#endif
#ifndef BLUE
#define BLUE  (XRenderColor){0x4858, 0x6F6F, 0xB0B0, 0xFFFF}
#endif
#ifndef GRAY
#define GRAY  (XRenderColor){0xa0a0, 0xa2a2, 0xa0a0, 0xFFFF}
#endif

#endif /* SKELETON_CONFIG_H */