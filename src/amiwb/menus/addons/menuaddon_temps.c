// Menu System - Temperature Monitoring Addon
// Displays CPU temperature (k10temp Tctl) in menubar (logo mode only)

#include "../menu_internal.h"
#include "../../font_manager.h"
#include "../../render/rnd_public.h"
#include "../../config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>

// ============================================================================
// Temperature State
// ============================================================================

static char cached_text[NAME_SIZE] = {0};  // Cache formatted text "Temps: 55 °C"
static int current_temp = 0;               // Current temperature in Celsius
static char temp_input_path[PATH_SIZE] = {0};  // Path to temp*_input file
static int reserved_width = 0;             // Reserved width for maximum text (prevents shifting)

// ============================================================================
// K10temp Tctl Sensor Discovery
// ============================================================================

// Find k10temp device with Tctl label and return temp*_input path
// Returns true if found, false otherwise
static bool find_k10temp_tctl_sensor(void) {
    DIR *dir = opendir("/sys/class/hwmon");
    if (!dir) {
        log_error("[WARNING] Cannot open /sys/class/hwmon for temperature monitoring");
        return false;
    }

    struct dirent *entry;
    bool found = false;

    // Search through all hwmon devices
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        // Check if this is a k10temp device
        char name_path[PATH_SIZE];
        snprintf(name_path, sizeof(name_path), "/sys/class/hwmon/%s/name", entry->d_name);

        FILE *name_file = fopen(name_path, "r");
        if (!name_file) continue;

        char device_name[NAME_SIZE];
        if (fgets(device_name, sizeof(device_name), name_file)) {
            // Remove newline
            size_t len = strlen(device_name);
            if (len > 0 && device_name[len-1] == '\n') {
                device_name[len-1] = '\0';
            }

            // Check if this is k10temp
            if (strcmp(device_name, "k10temp") == 0) {
                fclose(name_file);

                // Found k10temp device - now search for Tctl label
                char hwmon_dir[PATH_SIZE];
                snprintf(hwmon_dir, sizeof(hwmon_dir), "/sys/class/hwmon/%s", entry->d_name);

                DIR *hwmon = opendir(hwmon_dir);
                if (hwmon) {
                    struct dirent *temp_entry;
                    while ((temp_entry = readdir(hwmon)) != NULL) {
                        // Look for temp*_label files
                        if (strncmp(temp_entry->d_name, "temp", 4) == 0 &&
                            strstr(temp_entry->d_name, "_label")) {

                            // Check path length before constructing
                            size_t needed = strlen(hwmon_dir) + 1 + strlen(temp_entry->d_name) + 1;
                            if (needed > PATH_SIZE) continue;  // Skip this entry if path too long

                            // SAFE: Path length validated above - truncation impossible
                            // Pragma silences compiler limitation (can't connect runtime check to snprintf)
                            char label_path[PATH_SIZE];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
                            snprintf(label_path, sizeof(label_path), "%s/%s",
                                    hwmon_dir, temp_entry->d_name);
#pragma GCC diagnostic pop

                            FILE *label_file = fopen(label_path, "r");
                            if (label_file) {
                                char label[NAME_SIZE];
                                if (fgets(label, sizeof(label), label_file)) {
                                    // Remove newline
                                    size_t label_len = strlen(label);
                                    if (label_len > 0 && label[label_len-1] == '\n') {
                                        label[label_len-1] = '\0';
                                    }

                                    // Check if this is Tctl
                                    if (strcmp(label, "Tctl") == 0) {
                                        fclose(label_file);

                                        // Found Tctl! Build temp*_input path
                                        // Extract temp number from "temp1_label" -> "temp1_input"
                                        char temp_num[16];
                                        snprintf(temp_num, sizeof(temp_num), "%.*s",
                                                (int)(strstr(temp_entry->d_name, "_") - temp_entry->d_name),
                                                temp_entry->d_name);

                                        // Check path length before constructing
                                        size_t input_needed = strlen(hwmon_dir) + 1 + strlen(temp_num) + 6 + 1;  // "/" + num + "_input" + null
                                        if (input_needed <= sizeof(temp_input_path)) {
                                            // SAFE: Path length validated above - truncation impossible
                                            // Pragma silences compiler limitation (can't connect runtime check to snprintf)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
                                            snprintf(temp_input_path, sizeof(temp_input_path),
                                                    "%s/%s_input", hwmon_dir, temp_num);
#pragma GCC diagnostic pop
                                        } else {
                                            // Path too long - can't use this sensor
                                            closedir(hwmon);
                                            closedir(dir);
                                            return false;
                                        }

                                        found = true;
                                        closedir(hwmon);
                                        closedir(dir);
                                        return true;
                                    }
                                }
                                fclose(label_file);
                            }
                        }
                    }
                    closedir(hwmon);
                }

                // k10temp found but no Tctl label - keep searching other hwmon devices
            }
        } else {
            fclose(name_file);
        }
    }

    closedir(dir);
    return found;
}

// ============================================================================
// Temperature Reading
// ============================================================================

// Read temperature from k10temp Tctl sensor
static void update_temp_reading(void) {
    // Find sensor path on first call
    if (temp_input_path[0] == '\0') {
        if (!find_k10temp_tctl_sensor()) {
            current_temp = 0;
            return;
        }
    }

    // Read temperature value
    FILE *fp = fopen(temp_input_path, "r");
    if (!fp) {
        current_temp = 0;
        return;
    }

    int millidegrees = 0;
    if (fscanf(fp, "%d", &millidegrees) == 1) {
        // Convert millidegrees to Celsius
        current_temp = millidegrees / 1000;
    } else {
        current_temp = 0;
    }

    fclose(fp);
}

// ============================================================================
// Temperature Rendering
// ============================================================================

// Render temperature on menubar
static void temps_render(RenderContext *ctx, Canvas *menubar, int *x, int y) {
    if (!ctx || !menubar || !x) return;

    // Render cached text at current position
    menu_render_text(ctx, menubar, cached_text, *x, y);

    // Update x for next addon: use fixed maximum width + spacing (prevents shifting)
    *x += reserved_width + 40;
}

// ============================================================================
// Temperature Update
// ============================================================================

// Update callback - called periodically (every 1 second)
static void temps_update(void) {
    // Update temperature reading
    update_temp_reading();

    // Format display text
    if (current_temp > 0) {
        snprintf(cached_text, sizeof(cached_text), "Temps: %d °C", current_temp);
    } else {
        snprintf(cached_text, sizeof(cached_text), "Temps: N/A");
    }
}

// ============================================================================
// Temperature Cleanup
// ============================================================================

// Cleanup callback - called during shutdown
static void temps_cleanup(void) {
    // Nothing to cleanup
}

// ============================================================================
// Temperature Registration
// ============================================================================

// Initialize and register temperature addon
void menuaddon_temps_init(void) {
    // Allocate addon structure
    MenuAddon *temps_addon = calloc(1, sizeof(MenuAddon));
    if (!temps_addon) {
        log_error("[ERROR] Failed to allocate temps addon - continuing without temperature monitor");
        return;  // Gracefully fail - menubar works without temperature monitor
    }

    // Initial update to populate cached_text
    temps_update();

    // Calculate maximum text width (worst case: "Temps: 100 °C")
    RenderContext *ctx = get_render_context();
    if (ctx) {
        reserved_width = menu_measure_text(ctx, "Temps: 100 °C");
    } else {
        reserved_width = 130;  // Fallback if context not available yet
    }

    // Configure addon
    snprintf(temps_addon->name, sizeof(temps_addon->name), "temps");
    temps_addon->position = ADDON_POS_MIDDLE;   // Center of menubar
    temps_addon->width = reserved_width;        // Use calculated maximum width
    temps_addon->render = temps_render;
    temps_addon->update = temps_update;
    temps_addon->cleanup = temps_cleanup;
    temps_addon->enabled = false;               // Will be enabled by config loader
    temps_addon->config_order = -1;             // Will be set by config loader

    // Register with addon system
    menu_addon_register(temps_addon);
}
