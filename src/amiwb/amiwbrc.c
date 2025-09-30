// File: amiwbrc.c
// Configuration file parser for AmiWB
// Simple, brutal: no defaults, no magic
#include "amiwbrc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// Global config - zero-initialized
static AmiwbConfig g_config = {0};

// Trim leading and trailing whitespace
static char* trim(char* str) {
    if (!str) return str;
    
    // Trim leading
    while (*str && isspace(*str)) str++;
    
    // Empty string
    if (!*str) return str;
    
    // Trim trailing
    char *end = str + strlen(str) - 1;
    while (end > str && isspace(*end)) {
        *end = '\0';
        end--;
    }
    
    return str;
}

// Set a string field in config
static void set_string(char *dest, const char *value, size_t max_size) {
    strncpy(dest, value, max_size - 1);
    dest[max_size - 1] = '\0';
}

// Parse a single line and update config
static void parse_line(char *line) {
    // Skip comments and empty lines
    char *trimmed = trim(line);
    if (!trimmed[0] || trimmed[0] == '#') return;
    
    // Find the = separator
    char *eq = strchr(trimmed, '=');
    if (!eq) return;  // No = found, skip line
    
    // Split into key and value
    *eq = '\0';
    char *key = trim(trimmed);
    char *value = trim(eq + 1);
    
    // Skip if key or value is empty
    if (!key[0] || !value[0]) return;
    
    // Match key and set corresponding config field
    // Media keys
    if (strcmp(key, "brightness_up_cmd") == 0) {
        set_string(g_config.brightness_up_cmd, value, sizeof(g_config.brightness_up_cmd));
    }
    else if (strcmp(key, "brightness_down_cmd") == 0) {
        set_string(g_config.brightness_down_cmd, value, sizeof(g_config.brightness_down_cmd));
    }
    else if (strcmp(key, "volume_up_cmd") == 0) {
        set_string(g_config.volume_up_cmd, value, sizeof(g_config.volume_up_cmd));
    }
    else if (strcmp(key, "volume_down_cmd") == 0) {
        set_string(g_config.volume_down_cmd, value, sizeof(g_config.volume_down_cmd));
    }
    else if (strcmp(key, "volume_mute_cmd") == 0) {
        set_string(g_config.volume_mute_cmd, value, sizeof(g_config.volume_mute_cmd));
    }
    // Backgrounds
    else if (strcmp(key, "desktop_background") == 0) {
        set_string(g_config.desktop_background, value, sizeof(g_config.desktop_background));
    }
    else if (strcmp(key, "desktop_tiling") == 0) {
        g_config.desktop_tiling = atoi(value);
    }
    else if (strcmp(key, "window_background") == 0) {
        set_string(g_config.window_background, value, sizeof(g_config.window_background));
    }
    else if (strcmp(key, "window_tiling") == 0) {
        g_config.window_tiling = atoi(value);
    }
    // Rendering configuration
    else if (strcmp(key, "target_fps") == 0) {
        g_config.target_fps = atoi(value);
    }
    else if (strcmp(key, "render_mode") == 0) {
        g_config.render_mode = atoi(value);
    }
    // Unknown key - silently ignore
}

// Load configuration from file
void load_config(void) {
    // Clear config to all zeros
    memset(&g_config, 0, sizeof(g_config));
    
    // Build config file path
    const char *home = getenv("HOME");
    if (!home) return;  // No home, no config
    
    char path[PATH_SIZE];
    snprintf(path, sizeof(path), "%s/.config/amiwb/amiwbrc", home);
    
    // Try to open config file
    FILE *f = fopen(path, "r");
    if (!f) return;  // No file, config stays empty
    
    // Read and parse each line
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        // Remove newline if present
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
        }
        
        parse_line(line);
    }
    
    fclose(f);
}

// Get pointer to config (read-only)
const AmiwbConfig* get_config(void) {
    return &g_config;
}