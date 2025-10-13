// ReqASL Configuration Header
// Constants copied from amiwb/config.h for consistency
#ifndef REQASL_CONFIG_H
#define REQASL_CONFIG_H

// ReqASL version
#define REQASL_VERSION "0.01"

// Buffer sizes - must match amiwb for consistency
#define PATH_SIZE 512           // Buffer size for file paths (typical paths are <300 chars)
#define NAME_SIZE 128           // Buffer size for file names (most names are <50 chars)
#define FULL_SIZE (PATH_SIZE + NAME_SIZE + 2)  // Buffer for path + "/" + filename + null

// Checkmark symbol (Unicode 2713: ✓)
#define CHECKMARK "\xe2\x9c\x93"  // UTF-8 encoding of ✓ (U+2713)

// X11/Xrender for color definitions
#include <X11/extensions/Xrender.h>

// Base color definitions (copied from amiwb/config.h for consistency)
#define BLACK (XRenderColor){0x0000, 0x0000, 0x0000, 0xFFFF}
#define WHITE (XRenderColor){0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}
#define BLUE (XRenderColor){0x4858, 0x6F6F, 0xB0B0, 0xFFFF}
#define GRAY (XRenderColor){0xa0a0, 0xa2a2, 0xa0a0, 0xffff}
#define SELECT (XRenderColor){0x9999, 0xcccc, 0xffff, 0xFFFF}

// Selection rectangle appearance (multiselection)
#define SELECTION_RECT_FILL_COLOR SELECT         // Fill color
#define SELECTION_RECT_OUTLINE_COLOR BLACK       // Outline color
#define SELECTION_RECT_ALPHA_FILL 30             // Fill opacity (0-100, lower = more transparent)
#define SELECTION_RECT_ALPHA_OUTLINE 70          // Outline opacity (0-100, lower = more transparent)

// For logging - ReqASL uses fprintf(stderr, ...) not log_error()
// since log_error is in amiwb's main.c

#endif // REQASL_CONFIG_H