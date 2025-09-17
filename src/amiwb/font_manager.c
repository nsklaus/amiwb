/* font_manager.c - Unified font management for AmiWB
 *
 * Philosophy: One font to rule them all.
 * Load once, use everywhere, fail cleanly.
 */

#include "font_manager.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xft/Xft.h>

// The ONE font instance for the entire application
static XftFont *the_font = NULL;
static Display *font_display = NULL;

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

bool font_manager_init(Display *dpy) {
    if (the_font) {
        // Already initialized
        return true;
    }

    if (!dpy) {
        fprintf(stderr, "[FONT] ERROR: Display is NULL\n");
        return false;
    }

    font_display = dpy;

    // Get font path
    char *font_path = get_font_path();
    if (!font_path) {
        fprintf(stderr, "[FONT] FATAL: Cannot find font file %s\n", SYSFONT);
        fprintf(stderr, "[FONT] Searched in:\n");
        fprintf(stderr, "  - %s/%s/\n", getenv("HOME"), RESOURCE_DIR_USER);
        fprintf(stderr, "  - %s/\n", RESOURCE_DIR_SYSTEM);
        return false;
    }

    // Create font pattern from file
    FcPattern *pattern = FcPatternCreate();
    FcPatternAddString(pattern, FC_FILE, (FcChar8*)font_path);
    FcPatternAddDouble(pattern, FC_SIZE, 12.0);  // Standard AmiWB font size
    // No need to set weight - SourceCodePro-Bold.otf is already bold

    // Finalize the pattern
    FcConfigSubstitute(NULL, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);

    // Load the font
    the_font = XftFontOpenPattern(dpy, pattern);
    if (!the_font) {
        fprintf(stderr, "[FONT] FATAL: Failed to load font from %s\n", font_path);
        FcPatternDestroy(pattern);
        return false;
    }

    printf("[FONT] Successfully loaded: %s (size 12)\n", font_path);
    printf("[FONT] Metrics: ascent=%d, descent=%d, height=%d\n",
           the_font->ascent, the_font->descent, the_font->height);

    return true;
}

XftFont* font_manager_get(void) {
    if (!the_font) {
        fprintf(stderr, "[FONT] WARNING: Font requested but not initialized\n");
    }
    return the_font;
}

void font_manager_cleanup(bool is_restarting) {
    if (!the_font) {
        return;
    }

    if (is_restarting) {
        // During hot-restart, DO NOT free the font
        // XCloseDisplay will handle cleanup to avoid crash
        printf("[FONT] Hot-restart: Skipping font cleanup (XCloseDisplay will handle it)\n");
        the_font = NULL;
        font_display = NULL;
    } else {
        // Normal shutdown - clean up properly
        if (font_display) {
            XftFontClose(font_display, the_font);
            printf("[FONT] Font resources freed\n");
        }
        the_font = NULL;
        font_display = NULL;
    }
}

int font_manager_text_width(const char *text) {
    if (!the_font || !text || !font_display) {
        return 0;
    }

    XGlyphInfo extents;
    XftTextExtentsUtf8(font_display, the_font, (FcChar8*)text, strlen(text), &extents);
    return extents.xOff;
}

int font_manager_get_ascent(void) {
    return the_font ? the_font->ascent : 0;
}

int font_manager_get_descent(void) {
    return the_font ? the_font->descent : 0;
}

int font_manager_get_height(void) {
    return the_font ? the_font->height : 0;
}