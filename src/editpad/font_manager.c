/* font_manager.c - Unified font management for EditPad
 * Based on AmiWB's font_manager but simplified for EditPad
 */

#include "font_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xft/Xft.h>

// The ONE font instance for EditPad
static XftFont *the_font = NULL;
static Display *font_display = NULL;

// EditPad-specific font paths
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

bool editpad_font_init(Display *dpy) {
    if (the_font) {
        // Already initialized
        return true;
    }

    if (!dpy) {
        fprintf(stderr, "[EDITPAD FONT] ERROR: Display is NULL\n");
        return false;
    }

    font_display = dpy;

    // Get font path
    char *font_path = get_font_path();
    if (!font_path) {
        fprintf(stderr, "[EDITPAD FONT] FATAL: Cannot find font file %s\n", SYSFONT);
        fprintf(stderr, "[EDITPAD FONT] Searched in:\n");
        fprintf(stderr, "  - %s/%s/\n", getenv("HOME"), RESOURCE_DIR_USER);
        fprintf(stderr, "  - %s/\n", RESOURCE_DIR_SYSTEM);
        return false;
    }

    // Create font pattern from file
    FcPattern *pattern = FcPatternCreate();
    FcPatternAddString(pattern, FC_FILE, (FcChar8*)font_path);
    FcPatternAddDouble(pattern, FC_SIZE, 12.0);  // Standard EditPad font size

    // Finalize the pattern
    FcConfigSubstitute(NULL, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);

    // Load the font
    the_font = XftFontOpenPattern(dpy, pattern);
    if (!the_font) {
        fprintf(stderr, "[EDITPAD FONT] FATAL: Failed to load font from %s\n", font_path);
        FcPatternDestroy(pattern);
        return false;
    }

    printf("[EDITPAD FONT] Successfully loaded: %s (size 12)\n", font_path);
    printf("[EDITPAD FONT] Metrics: ascent=%d, descent=%d, height=%d\n",
           the_font->ascent, the_font->descent, the_font->height);

    return true;
}

XftFont* editpad_font_get(void) {
    if (!the_font) {
        fprintf(stderr, "[EDITPAD FONT] WARNING: Font requested but not initialized\n");
    }
    return the_font;
}

void editpad_font_cleanup(void) {
    if (!the_font) {
        return;
    }

    // EditPad doesn't hot-restart, always clean up properly
    if (font_display) {
        XftFontClose(font_display, the_font);
        printf("[EDITPAD FONT] Font resources freed\n");
    }
    the_font = NULL;
    font_display = NULL;
}

int editpad_font_text_width(const char *text) {
    if (!the_font || !text || !font_display) {
        return 0;
    }

    XGlyphInfo extents;
    XftTextExtentsUtf8(font_display, the_font, (FcChar8*)text, strlen(text), &extents);
    return extents.xOff;
}

int editpad_font_get_ascent(void) {
    return the_font ? the_font->ascent : 0;
}

int editpad_font_get_descent(void) {
    return the_font ? the_font->descent : 0;
}

int editpad_font_get_height(void) {
    return the_font ? the_font->height : 0;
}