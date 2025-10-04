// File: wb_canvas.c
// Canvas Operations - directory refresh and canvas clearing

#define _POSIX_C_SOURCE 200809L
#include "wb_internal.h"
#include "wb_public.h"
#include "../config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <X11/Xlib.h>

// External dependencies
extern void redraw_canvas(Canvas *canvas);
extern void refresh_canvas(Canvas *canvas);

// Helper to check if string ends with suffix
static bool ends_with(const char *s, const char *suffix) {
    size_t l = strlen(s), m = strlen(suffix);
    return l >= m && strcmp(s + l - m, suffix) == 0;
}

// ============================================================================
// Canvas Refresh from Directory
// ============================================================================

void refresh_canvas_from_directory(Canvas *canvas, const char *dirpath) {
    if (!canvas) return;
    
    // Resolve directory path
    char pathbuf[PATH_SIZE];
    const char *dir = dirpath;
    if (canvas->type == DESKTOP || !dir) {
        const char *home = getenv("HOME");
        snprintf(pathbuf, sizeof(pathbuf), "%s/Desktop", home ? home : ".");
        dir = pathbuf;
    }
    
    // Clear existing icons
    clear_canvas_icons(canvas);
    
    // Draw background immediately
    redraw_canvas(canvas);
    XSync(itn_core_get_display(), False);
    
    // Suppress icon rendering during scan
    canvas->scanning = true;
    
    // Prime desktop icons (System, Home) are handled by diskdrives.c
    
    DIR *dirp = opendir(dir);
    if (dirp) {
        struct dirent *entry;
        while ((entry = readdir(dirp))) {
            // Skip . and ..
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            
            // Skip hidden unless enabled
            if (entry->d_name[0] == '.' && !canvas->show_hidden) {
                continue;
            }
            
            // Build full path
            char full_path[PATH_SIZE];
            int ret = snprintf(full_path, sizeof(full_path), "%s/%s", dir, entry->d_name);
            if (ret >= PATH_SIZE) {
                log_error("[ERROR] Path too long, skipping: %s/%s", dir, entry->d_name);
                continue;
            }

            // Check for sidecar .info file
            char info_path[FULL_SIZE];
            snprintf(info_path, sizeof(info_path), "%s.info", full_path);
            
            struct stat st;
            bool has_sidecar = (stat(info_path, &st) == 0);
            
            // Skip orphan .info files
            if (ends_with(entry->d_name, ".info")) {
                char base_path[PATH_SIZE];
                strncpy(base_path, full_path, sizeof(base_path) - 1);
                base_path[strlen(base_path) - 5] = '\0';  // Remove .info
                
                if (stat(base_path, &st) != 0) {
                    // Orphan .info - create at 0,0
                    // Use full_path (the .info file itself), not info_path (which has .info appended again)
                    create_icon_with_metadata(full_path, canvas, 0, 0,
                                              full_path, entry->d_name, TYPE_FILE);
                }
                continue;
            }
            
            // Determine file type
            int type = TYPE_FILE;
            if (stat(full_path, &st) == 0) {
                type = S_ISDIR(st.st_mode) ? TYPE_DRAWER : TYPE_FILE;
            }
            
            // Use sidecar if available, otherwise deficon
            const char *icon_path = has_sidecar ? info_path : 
                                    wb_deficons_get_for_file(entry->d_name, type == TYPE_DRAWER);
            
            if (icon_path) {
                create_icon_with_metadata(icon_path, canvas, 0, 0,
                                         full_path, entry->d_name, type);
            }
        }
        closedir(dirp);
    }
    
    // Re-enable icon rendering
    canvas->scanning = false;
    
    // Layout and refresh
    icon_cleanup(canvas);
}

// ============================================================================
// Canvas Icon Clearing
// ============================================================================

void clear_canvas_icons(Canvas *canvas) {
    FileIcon **icon_array = wb_icons_array_get();
    int icon_count = wb_icons_array_count();
    
    for (int i = icon_count - 1; i >= 0; i--) {
        if (icon_array[i]->display_window == canvas->win) {
            // Keep iconified and device icons on desktop
            if (icon_array[i]->type == TYPE_ICONIFIED || 
                icon_array[i]->type == TYPE_DEVICE) {
                continue;
            }
            destroy_icon(icon_array[i]);
        }
    }
}

// refresh_canvas is provided by render.c (extern declaration above)
