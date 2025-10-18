// File: dialog_about.c
// About dialog implementation - displays AmiWB and system information

#include "dialog_internal.h"
#include "../render/rnd_public.h"
#include "../intuition/itn_internal.h"
#include "../../toolkit/toolkit_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>

// ============================================================================
// Constants
// ============================================================================

#define BYTES_PER_GB (1024UL * 1024UL * 1024UL)
#define XORG_MAJOR_DIVISOR 10000000
#define XORG_MINOR_DIVISOR 100000
#define XORG_PATCH_DIVISOR 1000
#define DIALOG_ABOUT_WIDTH 600
#define DIALOG_ABOUT_HEIGHT 400
#define ABOUT_BUTTON_Y 340

// ============================================================================
// System Information Structure
// ============================================================================

typedef struct {
    char amiwb_version[32];
    char toolkit_version[32];
    char os_name[64];
    char os_version[32];
    char kernel_name[65];      // utsname.sysname is char[65]
    char kernel_version[65];   // utsname.release is char[65]
    char total_ram[32];
    char cpu_name[128];
    char cpu_arch[65];         // utsname.machine is char[65]
    char igpu_name[128];
    char igpu_ram[32];
    char dgpu_name[128];
    char dgpu_ram[32];
    char xorg_version[64];
    char input_backend[64];
} SystemInfo;

// ============================================================================
// System Information Caching
// ============================================================================

// Cache system info at startup - most values never change during runtime
static SystemInfo g_cached_sysinfo;
static bool g_sysinfo_cached = false;

// ============================================================================
// System Information Gathering
// ============================================================================

// Gather all system information (called once at dialog creation)
static void gather_system_info(SystemInfo *info) {
    if (!info) return;

    // Initialize all fields to "Unknown"
    snprintf(info->amiwb_version, sizeof(info->amiwb_version), "Unknown");
    snprintf(info->toolkit_version, sizeof(info->toolkit_version), "Unknown");
    snprintf(info->os_name, sizeof(info->os_name), "Unknown");
    snprintf(info->os_version, sizeof(info->os_version), "Unknown");
    snprintf(info->kernel_name, sizeof(info->kernel_name), "Unknown");
    snprintf(info->kernel_version, sizeof(info->kernel_version), "Unknown");
    snprintf(info->total_ram, sizeof(info->total_ram), "Unknown");
    snprintf(info->cpu_name, sizeof(info->cpu_name), "Unknown");
    snprintf(info->cpu_arch, sizeof(info->cpu_arch), "Unknown");
    snprintf(info->igpu_name, sizeof(info->igpu_name), "N/A");
    snprintf(info->igpu_ram, sizeof(info->igpu_ram), "N/A");
    snprintf(info->dgpu_name, sizeof(info->dgpu_name), "N/A");
    snprintf(info->dgpu_ram, sizeof(info->dgpu_ram), "N/A");
    snprintf(info->xorg_version, sizeof(info->xorg_version), "Unknown");
    snprintf(info->input_backend, sizeof(info->input_backend), "Unknown");

    // 1. AmiWB version (from config.h)
    snprintf(info->amiwb_version, sizeof(info->amiwb_version), "%s", AMIWB_VERSION);

    // 2. Toolkit version (from toolkit_config.h)
    snprintf(info->toolkit_version, sizeof(info->toolkit_version), "%s", TOOLKIT_VERSION);

    // 3. OS name and version (from /etc/os-release)
    FILE *os_file = fopen("/etc/os-release", "r");
    if (os_file) {
        char line[256];
        while (fgets(line, sizeof(line), os_file)) {
            // Strip newline
            size_t len = strlen(line);
            if (len > 0 && line[len - 1] == '\n') {
                line[len - 1] = '\0';
            }

            // Parse NAME="..."
            if (strncmp(line, "NAME=", 5) == 0) {
                char *value = line + 5;
                // Strip quotes if present
                if (value[0] == '"') {
                    value++;
                    char *end_quote = strchr(value, '"');
                    if (end_quote) *end_quote = '\0';
                }
                snprintf(info->os_name, sizeof(info->os_name), "%s", value);
            }
            // Parse VERSION_ID="..."
            else if (strncmp(line, "VERSION_ID=", 11) == 0) {
                char *value = line + 11;
                if (value[0] == '"') {
                    value++;
                    char *end_quote = strchr(value, '"');
                    if (end_quote) *end_quote = '\0';
                }
                snprintf(info->os_version, sizeof(info->os_version), "%s", value);
            }
        }
        fclose(os_file);
    }

    // 4. Kernel name and version (from uname)
    struct utsname uts;
    if (uname(&uts) == 0) {
        snprintf(info->kernel_name, sizeof(info->kernel_name), "%s", uts.sysname);
        snprintf(info->kernel_version, sizeof(info->kernel_version), "%s", uts.release);
        snprintf(info->cpu_arch, sizeof(info->cpu_arch), "%s", uts.machine);
    }

    // 5. Total RAM (from sysinfo)
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        // Convert to GB (sysinfo returns bytes)
        unsigned long total_gb = si.totalram / BYTES_PER_GB;
        snprintf(info->total_ram, sizeof(info->total_ram), "%lu GB", total_gb);
    }

    // 6. CPU name (from /proc/cpuinfo)
    FILE *cpu_file = fopen("/proc/cpuinfo", "r");
    if (cpu_file) {
        char line[256];
        while (fgets(line, sizeof(line), cpu_file)) {
            // Look for "model name" line
            if (strncmp(line, "model name", 10) == 0) {
                char *colon = strchr(line, ':');
                if (colon) {
                    char *value = colon + 1;
                    // Skip leading whitespace
                    while (*value == ' ' || *value == '\t') value++;
                    // Strip newline
                    size_t len = strlen(value);
                    if (len > 0 && value[len - 1] == '\n') {
                        value[len - 1] = '\0';
                    }
                    snprintf(info->cpu_name, sizeof(info->cpu_name), "%s", value);
                    break;  // Only need first CPU
                }
            }
        }
        fclose(cpu_file);
    }

    // 7-8. GPU information (simplified - just mark as N/A for now)
    // Full GPU detection would require parsing lspci or /sys/class/drm
    // This is complex and varies by system - leave as N/A for initial implementation

    // 9. Xorg version (simplified - just mark version from display)
    Display *dpy = itn_core_get_display();
    if (dpy) {
        int vendor_release = VendorRelease(dpy);
        // XOrg version format: major.minor.patch encoded as integer
        int major = vendor_release / XORG_MAJOR_DIVISOR;
        int minor = (vendor_release / XORG_MINOR_DIVISOR) % 100;
        int patch = (vendor_release / XORG_PATCH_DIVISOR) % 100;
        snprintf(info->xorg_version, sizeof(info->xorg_version), "%d.%d.%d", major, minor, patch);
    }

    // 10. Input backend (simplified - assume libinput on modern systems)
    snprintf(info->input_backend, sizeof(info->input_backend), "libinput");
}

// ============================================================================
// About Dialog Creation
// ============================================================================

// Create and show about dialog (no callbacks needed - just closes on Accept)
void show_about_dialog(void) {
    // Gather system info on first call (cache for subsequent calls)
    if (!g_sysinfo_cached) {
        gather_system_info(&g_cached_sysinfo);
        g_sysinfo_cached = true;
    }

    // Create dialog using consistent lifecycle function
    Dialog *dialog = dialog_core_create(DIALOG_ABOUT, "About AmiWB", DIALOG_ABOUT_WIDTH, DIALOG_ABOUT_HEIGHT);
    if (!dialog) {
        log_error("[ERROR] Failed to create about dialog - feature unavailable");
        return;  // Graceful degradation
    }

    // Allocate SystemInfo copy for this dialog instance
    SystemInfo *sys_info = malloc(sizeof(SystemInfo));
    if (!sys_info) {
        log_error("[ERROR] Failed to allocate SystemInfo for about dialog");
        dialog_core_destroy(dialog);
        return;
    }

    // Copy cached info (fast - just memcpy)
    memcpy(sys_info, &g_cached_sysinfo, sizeof(SystemInfo));

    // Store in user_data for rendering
    dialog->user_data = sys_info;

    // Set minimum window size to prevent shrinking below content
    dialog->canvas->min_width = DIALOG_ABOUT_WIDTH;
    dialog->canvas->min_height = DIALOG_ABOUT_HEIGHT;
    dialog->canvas->resize_x_allowed = true;
    dialog->canvas->resize_y_allowed = true;

    // Create single centered "Accept" button
    int button_x = (DIALOG_ABOUT_WIDTH - BUTTON_WIDTH) / 2;  // Centered horizontally
    int button_y = ABOUT_BUTTON_Y;  // Near bottom
    dialog->ok_button = button_create(button_x, button_y, BUTTON_WIDTH, BUTTON_HEIGHT, "Accept", dialog->font);

    // No cancel button or input field needed
    dialog->cancel_button = NULL;
    dialog->input_field = NULL;

    // Set callbacks (on_ok just closes dialog, no on_cancel)
    dialog->on_ok = NULL;  // Will be handled by button release event
    dialog->on_cancel = NULL;

    // Register dialog in global list
    dialog_core_register(dialog);

    // Show the dialog and set it as active window
    XMapRaised(itn_core_get_display(), dialog->canvas->win);
    itn_focus_set_active(dialog->canvas);

    redraw_canvas(dialog->canvas);
}

// ============================================================================
// About Dialog Rendering
// ============================================================================

void dialog_about_render_content(Canvas *canvas, Dialog *dialog) {
    Display *dpy = itn_core_get_display();
    Picture dest = canvas->canvas_render;
    XftFont *font = get_font();
    if (!font) return;

    // Get system info from user_data
    SystemInfo *info = (SystemInfo *)dialog->user_data;
    if (!info) {
        log_error("[ERROR] No SystemInfo in about dialog");
        return;
    }

    // Setup text rendering
    XRenderColor text_color = BLACK;
    XftColor xft_text;
    XftColorAllocValue(dpy, canvas->visual, canvas->colormap, &text_color, &xft_text);

    // Starting position for text
    int text_x = BORDER_WIDTH_LEFT + 20;  // Left margin (reduced from 40)
    int text_y = BORDER_HEIGHT_TOP + 30;  // Top margin
    int line_height = 22;  // Spacing between lines

    // Build and render each line
    char line[256];

    // Line 1: Empty line for spacing
    text_y += line_height;

    // Line 2: Desktop version
    snprintf(line, sizeof(line), "  Desktop  : AmiWB %s", info->amiwb_version);
    XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_x, text_y,
                     (FcChar8*)line, strlen(line));
    text_y += line_height;

    // Line 3: Toolkit version
    snprintf(line, sizeof(line), "  Toolkit  : libamiwb %s", info->toolkit_version);
    XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_x, text_y,
                     (FcChar8*)line, strlen(line));
    text_y += line_height;

    // Line 4: Empty line
    text_y += line_height;

    // Line 5: Distro
    snprintf(line, sizeof(line), "  Distro   : %s %s", info->os_name, info->os_version);
    XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_x, text_y,
                     (FcChar8*)line, strlen(line));
    text_y += line_height;

    // Line 6: Kernel
    snprintf(line, sizeof(line), "  Kernel   : %s %s", info->kernel_name, info->kernel_version);
    XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_x, text_y,
                     (FcChar8*)line, strlen(line));
    text_y += line_height;

    // Line 7: Memory
    snprintf(line, sizeof(line), "  Memory   : %s", info->total_ram);
    XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_x, text_y,
                     (FcChar8*)line, strlen(line));
    text_y += line_height;

    // Line 8: CPU
    snprintf(line, sizeof(line), "  CPU      : %s %s", info->cpu_name, info->cpu_arch);
    XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_x, text_y,
                     (FcChar8*)line, strlen(line));
    text_y += line_height;

    // Line 9: iGPU
    snprintf(line, sizeof(line), "  iGPU     : %s %s", info->igpu_name, info->igpu_ram);
    XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_x, text_y,
                     (FcChar8*)line, strlen(line));
    text_y += line_height;

    // Line 10: dGPU
    snprintf(line, sizeof(line), "  dGPU     : %s %s", info->dgpu_name, info->dgpu_ram);
    XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_x, text_y,
                     (FcChar8*)line, strlen(line));
    text_y += line_height;

    // Line 11: Xorg
    snprintf(line, sizeof(line), "  Xorg     : X11 %s", info->xorg_version);
    XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_x, text_y,
                     (FcChar8*)line, strlen(line));
    text_y += line_height;

    // Line 12: Input backend
    snprintf(line, sizeof(line), "  Input    : %s", info->input_backend);
    XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_x, text_y,
                     (FcChar8*)line, strlen(line));

    // Render the Accept button
    if (dialog->ok_button) {
        button_render(dialog->ok_button, dest, dpy, canvas->xft_draw);
    }

    // Clean up
    XftColorFree(dpy, canvas->visual, canvas->colormap, &xft_text);
}
