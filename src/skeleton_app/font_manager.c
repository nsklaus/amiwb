/* font_manager.c - Font management implementation */

/*
 * What could be added later:
 * - Font metrics caching
 * - Text measurement helpers
 * - Font substitution for missing glyphs
 * - Memory usage tracking
 */

#include "font_manager.h"
#include "logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fontconfig/fontconfig.h>

static Display *dpy = NULL;
static XftFont *font = NULL;

/* AmiWB standard font paths - same as EditPad/ReqASL */
#define RESOURCE_DIR_USER ".config/amiwb"
#define RESOURCE_DIR_SYSTEM "/usr/local/share/amiwb/fonts"
#define SYSFONT "SourceCodePro-Bold.otf"

/* Get the font file path - checks user dir then system dir */
static char* get_font_path(void) {
    char *home = getenv("HOME");
    static char path[1024];

    /* Try user directory first */
    snprintf(path, sizeof(path), "%s/%s/fonts/%s", home, RESOURCE_DIR_USER, SYSFONT);
    if (access(path, F_OK) == 0) {
        return path;
    }

    /* Also try without fonts subdirectory */
    snprintf(path, sizeof(path), "%s/%s/%s", home, RESOURCE_DIR_USER, SYSFONT);
    if (access(path, F_OK) == 0) {
        return path;
    }

    /* Fall back to system directory */
    snprintf(path, sizeof(path), "%s/%s", RESOURCE_DIR_SYSTEM, SYSFONT);
    if (access(path, F_OK) == 0) {
        return path;
    }

    return NULL;
}

int font_init(Display *display) {
    dpy = display;

    /* Get font path - same method as EditPad/ReqASL */
    char *font_path = get_font_path();
    if (!font_path) {
        log_message("FATAL: Cannot find font file %s", SYSFONT);
        log_message("FATAL: Searched in:");
        log_message("  - %s/%s/fonts/", getenv("HOME"), RESOURCE_DIR_USER);
        log_message("  - %s/%s/", getenv("HOME"), RESOURCE_DIR_USER);
        log_message("  - %s/", RESOURCE_DIR_SYSTEM);
        return 0;
    }

    /* Create font pattern from file - EXACT same as EditPad/ReqASL */
    FcPattern *pattern = FcPatternCreate();
    FcPatternAddString(pattern, FC_FILE, (FcChar8*)font_path);
    FcPatternAddDouble(pattern, FC_SIZE, 12.0);  /* Standard AmiWB font size */

    /* Finalize the pattern */
    FcConfigSubstitute(NULL, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);

    /* Load the font */
    font = XftFontOpenPattern(dpy, pattern);
    if (!font) {
        log_message("FATAL: Failed to load font from %s", font_path);
        FcPatternDestroy(pattern);
        return 0;
    }

    log_message("Font loaded from: %s", font_path);
    return 1;
}

XftFont *font_get(void) {
    return font;  /* Caller must NOT free this */
}

void font_cleanup(void) {
    if (font && dpy) {
        XftFontClose(dpy, font);
        font = NULL;
    }
}