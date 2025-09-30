// File: amiwbrc.h
// Configuration file parser for AmiWB
// Reads ~/.config/amiwb/amiwbrc for user settings
#ifndef AMIWBRC_H
#define AMIWBRC_H

#include "config.h"  // For PATH_SIZE and NAME_SIZE

// Configuration structure - extensible for future settings
typedef struct {
    // Media key commands
    char brightness_up_cmd[512];
    char brightness_down_cmd[512];
    char volume_up_cmd[512];
    char volume_down_cmd[512];
    char volume_mute_cmd[512];
    
    // Background images and tiling
    char desktop_background[PATH_SIZE];
    int desktop_tiling;  // 0=no tile, 1=tile
    char window_background[PATH_SIZE];
    int window_tiling;   // 0=no tile, 1=tile

    // Rendering configuration
    int target_fps;      // Target framerate (default 120)
    int render_mode;     // 0=on-demand (default), 1=continuous

    // Future expansion space - add new settings here
} AmiwbConfig;

// Load configuration from ~/.config/amiwb/amiwbrc
// No defaults - missing config means empty strings/zeros
void load_config(void);

// Get pointer to global config (read-only)
const AmiwbConfig* get_config(void);

#endif // AMIWBRC_H