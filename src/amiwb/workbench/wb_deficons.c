// File: wb_deficons.c
// Default Icons System - automatically loads and matches def_*.info files

#define _POSIX_C_SOURCE 200809L
#include "wb_internal.h"
#include "../config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

// ============================================================================
// Deficons (default icons) support
// Provide fallback .info icons when files lack sidecar .info next to them.
// Directories use def_dir; unknown filetypes use def_foo so everything gets
// a consistent icon even without custom sidecars.
// ============================================================================

static const char *deficons_dir = "/usr/local/share/amiwb/icons/def_icons";

// Dynamic def_icons system - automatically scans directory for def_*.info files
typedef struct DefIconEntry {
    char *extension;   // File extension (without dot): "txt", "jpg", etc.
    char *icon_path;   // Full path to the .info file
} DefIconEntry;

static DefIconEntry *def_icons_array = NULL;
static int def_icons_count = 0;
static int def_icons_capacity = 0;

// Special cases that don't follow the pattern
static char *def_dir_info  = NULL;   // for directories (def_dir.info)
static char *def_foo_info  = NULL;   // generic fallback (def_foo.info)

// ============================================================================
// Internal Helper
// ============================================================================

// String suffix matching helper
static bool ends_with(const char *s, const char *suffix) {
    size_t l = strlen(s), m = strlen(suffix);
    return l >= m && strcmp(s + l - m, suffix) == 0;
}

// ============================================================================
// Deficon Management
// ============================================================================

// Add or update a def_icon in the dynamic array (silent - no logging)
static void add_or_update_deficon_entry(const char *extension, const char *full_path, bool is_user) {
    if (!extension || !full_path) return;

    // Check if this extension already exists (user icons override system icons)
    for (int i = 0; i < def_icons_count; i++) {
        if (strcasecmp(def_icons_array[i].extension, extension) == 0) {
            // Found existing entry - update it silently
            free(def_icons_array[i].icon_path);
            def_icons_array[i].icon_path = strdup(full_path);
            if (!def_icons_array[i].icon_path) {
                log_error("[ERROR] strdup failed for deficon path update");
                return;
            }
            return;
        }
    }

    // Not found - add new entry
    // Grow array if needed
    if (def_icons_count >= def_icons_capacity) {
        int new_capacity = def_icons_capacity == 0 ? 16 : def_icons_capacity * 2;
        DefIconEntry *new_array = realloc(def_icons_array, new_capacity * sizeof(DefIconEntry));
        if (!new_array) {
            log_error("[ERROR] Failed to allocate memory for def_icons array");
            return;
        }
        def_icons_array = new_array;
        def_icons_capacity = new_capacity;
    }

    // Add the new entry silently
    def_icons_array[def_icons_count].extension = strdup(extension);
    def_icons_array[def_icons_count].icon_path = strdup(full_path);
    if (!def_icons_array[def_icons_count].extension || !def_icons_array[def_icons_count].icon_path) {
        log_error("[ERROR] strdup failed for deficon entry");
        if (def_icons_array[def_icons_count].extension) free(def_icons_array[def_icons_count].extension);
        if (def_icons_array[def_icons_count].icon_path) free(def_icons_array[def_icons_count].icon_path);
        return;
    }
    def_icons_count++;
}

// Scan a directory for def_*.info files and load them
static void scan_deficons_directory(const char *dir_path, bool is_user) {
    DIR *dir = opendir(dir_path);
    if (!dir) {
        // Only warn for system directory, user directory is optional
        if (!is_user) {
            log_error("[WARNING] Cannot open deficons directory: %s", dir_path);
        }
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir))) {
        // Check if filename matches pattern: def_*.info
        if (strncmp(entry->d_name, "def_", 4) != 0) continue;
        if (!ends_with(entry->d_name, ".info")) continue;

        // Build full path
        char full_path[PATH_SIZE];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

        // Verify it's a regular file
        struct stat st;
        if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        // Extract the extension part from def_XXX.info
        // Skip "def_" (4 chars), take until ".info" (strlen - 4 - 5)
        size_t name_len = strlen(entry->d_name);
        if (name_len <= 9) continue; // Too short to be valid def_X.info

        size_t ext_len = name_len - 4 - 5; // Remove "def_" and ".info"
        char extension[NAME_SIZE];
        strncpy(extension, entry->d_name + 4, ext_len);
        extension[ext_len] = '\0';

        // Handle special cases (silently)
        if (strcmp(extension, "dir") == 0) {
            if (def_dir_info) free(def_dir_info);
            def_dir_info = strdup(full_path);
            if (!def_dir_info) {
                log_error("[ERROR] strdup failed for def_dir.info");
            }
        } else if (strcmp(extension, "foo") == 0) {
            if (def_foo_info) free(def_foo_info);
            def_foo_info = strdup(full_path);
            if (!def_foo_info) {
                log_error("[ERROR] strdup failed for def_foo.info");
            }
        } else {
            // Regular extension - add or update in array
            add_or_update_deficon_entry(extension, full_path, is_user);
        }
    }

    closedir(dir);
}

// ============================================================================
// Public API
// ============================================================================

// Load all def_*.info files from system and user directories
void wb_deficons_load(void) {
    // First load system def_icons (silently)
    scan_deficons_directory(deficons_dir, false);

    // Then load user def_icons (these override system ones, also silently)
    const char *home = getenv("HOME");
    if (home) {
        char user_deficons_dir[PATH_SIZE];
        snprintf(user_deficons_dir, sizeof(user_deficons_dir),
                "%s/.config/amiwb/icons/def_icons", home);
        scan_deficons_directory(user_deficons_dir, true);
    }

    // Now log the final active icons (only what will actually be used)
    // Log special icons
    if (def_dir_info) {
        log_error("[ICON] def_dir.info -> %s", def_dir_info);
    }
    if (def_foo_info) {
        log_error("[ICON] def_foo.info -> %s", def_foo_info);
    }

    // Log regular extension icons
    for (int i = 0; i < def_icons_count; i++) {
        log_error("[ICON] def_%s.info -> %s",
                 def_icons_array[i].extension, def_icons_array[i].icon_path);
    }
}

// Get deficon path for a file (returns NULL if no match)
const char *wb_deficons_get_for_file(const char *name, bool is_dir) {
    if (!name) return NULL;
    if (is_dir) return def_dir_info; // default drawer icon if present

    const char *dot = strrchr(name, '.');
    if (!dot || !dot[1]) {
        // No extension - return generic fallback if available
        return def_foo_info;
    }
    const char *ext = dot + 1;

    // Search the dynamic array for matching extension
    for (int i = 0; i < def_icons_count; i++) {
        if (strcasecmp(ext, def_icons_array[i].extension) == 0) {
            return def_icons_array[i].icon_path;
        }

        // Special handling for common multi-extension mappings
        // jpg/jpeg -> jpg
        if (strcasecmp(ext, "jpeg") == 0 && strcasecmp(def_icons_array[i].extension, "jpg") == 0) {
            return def_icons_array[i].icon_path;
        }
        // htm/html -> html
        if (strcasecmp(ext, "htm") == 0 && strcasecmp(def_icons_array[i].extension, "html") == 0) {
            return def_icons_array[i].icon_path;
        }
    }

    // Unknown or unmapped extension -> generic tool icon if available
    return def_foo_info;
}
