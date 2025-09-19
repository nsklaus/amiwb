/* font_manager.c - Unified font management for ReqASL
 * Based on AmiWB's font_manager but simplified for ReqASL
 */

#include "font_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xft/Xft.h>

// The ONE font instance for ReqASL
static XftFont *the_font = NULL;
static Display *font_display = NULL;

// ReqASL-specific font paths
#define RESOURCE_DIR_USER ".config/amiwb"
#define RESOURCE_DIR_SYSTEM "/usr/local/share/amiwb/fonts"
#define SYSFONT "SourceCodePro-Bold.otf"

// Get the font file path - checks user dir then system dir
static char* get_font_path(void) {
    char *home = getenv("HOME");
    static char path[1024];

    // Try user directory first
    snprintf(path, sizeof(path), "%s/%s/%s", home, RESOURCE_DIR_USER, SYSFONT);
    if (access(path, F_OK) == 0) {
        return path;
    }

    // Fall back to system directory
    snprintf(path, sizeof(path), "%s/%s", RESOURCE_DIR_SYSTEM, SYSFONT);
    if (access(path, F_OK) == 0) {
        return path;
    }

    return NULL;
}

bool reqasl_font_init(Display *dpy) {
    if (the_font) {
        // Already initialized
        return true;
    }

    if (!dpy) {
        fprintf(stderr, "[REQASL FONT] ERROR: Display is NULL\n");
        return false;
    }

    font_display = dpy;

    // Get font path
    char *font_path = get_font_path();
    if (!font_path) {
        fprintf(stderr, "[REQASL FONT] FATAL: Cannot find font file %s\n", SYSFONT);
        fprintf(stderr, "[REQASL FONT] Searched in:\n");
        fprintf(stderr, "  - %s/%s/\n", getenv("HOME"), RESOURCE_DIR_USER);
        fprintf(stderr, "  - %s/\n", RESOURCE_DIR_SYSTEM);
        return false;
    }

    // Create font pattern from file
    FcPattern *pattern = FcPatternCreate();
    FcPatternAddString(pattern, FC_FILE, (FcChar8*)font_path);
    FcPatternAddDouble(pattern, FC_SIZE, 12.0);  // Standard ReqASL font size

    // Finalize the pattern
    FcConfigSubstitute(NULL, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);

    // Load the font
    the_font = XftFontOpenPattern(dpy, pattern);
    if (!the_font) {
        fprintf(stderr, "[REQASL FONT] FATAL: Failed to load font from %s\n", font_path);
        FcPatternDestroy(pattern);
        return false;
    }

    // Success - no debug output per logging_system.md rules
    return true;
}

XftFont* reqasl_font_get(void) {
    if (!the_font) {
        fprintf(stderr, "[REQASL FONT] WARNING: Font requested but not initialized\n");
    }
    return the_font;
}

void reqasl_font_cleanup(void) {
    if (!the_font) {
        return;
    }

    // ReqASL doesn't hot-restart, always clean up properly
    if (font_display) {
        XftFontClose(font_display, the_font);
        // Cleanup successful - no debug output per logging_system.md
    }
    the_font = NULL;
    font_display = NULL;
}

int reqasl_font_text_width(const char *text) {
    if (!the_font || !text || !font_display) {
        return 0;
    }

    XGlyphInfo extents;
    XftTextExtentsUtf8(font_display, the_font, (FcChar8*)text, strlen(text), &extents);
    return extents.xOff;
}

int reqasl_font_get_ascent(void) {
    return the_font ? the_font->ascent : 0;
}

int reqasl_font_get_descent(void) {
    return the_font ? the_font->descent : 0;
}

int reqasl_font_get_height(void) {
    return the_font ? the_font->height : 0;
}