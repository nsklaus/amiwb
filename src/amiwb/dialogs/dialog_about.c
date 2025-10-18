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
#define DIALOG_ABOUT_HEIGHT 390
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
    int optimal_width;         // Calculated minimum width based on longest string
} SystemInfo;

// ============================================================================
// System Information Caching
// ============================================================================

// Cache system info at startup - most values never change during runtime
static SystemInfo g_cached_sysinfo;
static bool g_sysinfo_cached = false;

// ============================================================================
// Helper Functions
// ============================================================================

// Strip GPU info from CPU name (e.g., "AMD Ryzen 7 8845HS w/ Radeon 780M Graphics" -> "AMD Ryzen 7 8845HS")
// Extracted GPU info is stored in igpu_out parameter if non-NULL
static void strip_gpu_from_cpu_name(char *cpu_name, char *igpu_out, size_t igpu_size) {
    if (!cpu_name) return;

    // Look for common GPU markers in CPU strings
    char *gpu_marker = strstr(cpu_name, " w/ ");
    if (!gpu_marker) gpu_marker = strstr(cpu_name, " with ");

    if (gpu_marker) {
        // Extract GPU part if output buffer provided
        if (igpu_out && igpu_size > 0) {
            const char *gpu_start = gpu_marker + (gpu_marker[2] == '/' ? 4 : 6);  // Skip " w/ " or " with "
            snprintf(igpu_out, igpu_size, "%s", gpu_start);
        }
        // Truncate CPU name at GPU marker (modifies in-place)
        *gpu_marker = '\0';
    }
}

// Detect GPUs via /sys/class/drm (standard Linux DRM subsystem)
// Uses VRAM size heuristic: <1GB = iGPU, >=1GB = dGPU
// Returns: 0 on success, -1 on failure
static int detect_gpus(char *igpu_out, size_t igpu_size, char *dgpu_out, size_t dgpu_size) {
    if (!igpu_out || !dgpu_out) return -1;

    igpu_out[0] = '\0';
    dgpu_out[0] = '\0';

    // Scan up to 8 card devices (card0-card7)
    for (int i = 0; i < 8; i++) {
        char uevent_path[256];
        char vram_path[256];
        snprintf(uevent_path, sizeof(uevent_path), "/sys/class/drm/card%d/device/uevent", i);
        snprintf(vram_path, sizeof(vram_path), "/sys/class/drm/card%d/device/mem_info_vram_total", i);

        // Check if this card exists
        FILE *uevent_file = fopen(uevent_path, "r");
        if (!uevent_file) continue;

        // Parse uevent to get driver name
        char driver_name[64] = {0};
        char line[256];
        while (fgets(line, sizeof(line), uevent_file)) {
            if (strncmp(line, "DRIVER=", 7) == 0) {
                char *value = line + 7;
                size_t len = strlen(value);
                if (len > 0 && value[len - 1] == '\n') value[len - 1] = '\0';
                snprintf(driver_name, sizeof(driver_name), "%s", value);
                break;
            }
        }
        fclose(uevent_file);

        if (driver_name[0] == '\0') continue;

        // Read VRAM size to distinguish iGPU (<1GB) from dGPU (>=1GB)
        long long vram_bytes = 0;
        FILE *vram_file = fopen(vram_path, "r");
        if (vram_file) {
            if (fscanf(vram_file, "%lld", &vram_bytes) != 1) {
                vram_bytes = 0;
            }
            fclose(vram_file);
        }

        // VRAM heuristic: <1GB = iGPU, >=1GB = dGPU
        long long one_gb = 1024LL * 1024LL * 1024LL;
        bool is_igpu = (vram_bytes > 0 && vram_bytes < one_gb);

        // Format GPU string based on driver
        char gpu_str[128] = {0};
        if (strcmp(driver_name, "amdgpu") == 0) {
            snprintf(gpu_str, sizeof(gpu_str), "AMD Radeon %s", is_igpu ? "iGPU" : "GPU");
        } else if (strcmp(driver_name, "nvidia") == 0) {
            snprintf(gpu_str, sizeof(gpu_str), "NVIDIA GPU");
        } else if (strcmp(driver_name, "i915") == 0 || strcmp(driver_name, "xe") == 0) {
            snprintf(gpu_str, sizeof(gpu_str), "Intel %s", is_igpu ? "iGPU" : "GPU");
        } else {
            snprintf(gpu_str, sizeof(gpu_str), "%s GPU", driver_name);
        }

        // Store in appropriate output buffer
        if (is_igpu && igpu_out[0] == '\0') {
            snprintf(igpu_out, igpu_size, "%s", gpu_str);
        } else if (!is_igpu && dgpu_out[0] == '\0') {
            snprintf(dgpu_out, dgpu_size, "%s", gpu_str);
        }
    }

    return 0;
}

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

    // 6. CPU name (from /proc/cpuinfo - multi-architecture support)
    char igpu_from_cpu[128] = {0};  // Temporary buffer for iGPU extracted from CPU string
    FILE *cpu_file = fopen("/proc/cpuinfo", "r");
    if (cpu_file) {
        char line[256];
        bool found = false;

        while (fgets(line, sizeof(line), cpu_file)) {
            char *colon = strchr(line, ':');
            if (!colon) continue;

            // x86/x86_64: "model name : ..."
            // ARM: "Model : ..." or "Hardware : ..."
            // RISC-V: "uarch : ..."
            if (!found && (strncmp(line, "model name", 10) == 0 ||
                           strncmp(line, "Model", 5) == 0 ||
                           strncmp(line, "Hardware", 8) == 0 ||
                           strncmp(line, "uarch", 5) == 0)) {
                char *value = colon + 1;
                // Skip leading whitespace
                while (*value == ' ' || *value == '\t') value++;
                // Strip newline
                size_t len = strlen(value);
                if (len > 0 && value[len - 1] == '\n') {
                    value[len - 1] = '\0';
                }
                snprintf(info->cpu_name, sizeof(info->cpu_name), "%s", value);
                found = true;
                break;  // Only need first CPU
            }
        }
        fclose(cpu_file);

        // Strip GPU info from CPU name if present (e.g., "w/ Radeon 780M Graphics")
        if (found) {
            strip_gpu_from_cpu_name(info->cpu_name, igpu_from_cpu, sizeof(igpu_from_cpu));
        }
    }

    // 7-8. GPU detection via /sys/class/drm (standard Linux DRM subsystem)
    char detected_igpu[128] = {0};
    char detected_dgpu[128] = {0};
    detect_gpus(detected_igpu, sizeof(detected_igpu), detected_dgpu, sizeof(detected_dgpu));

    // Prefer iGPU info from CPU string (more specific model info)
    // Fall back to DRM detection if CPU string didn't contain GPU info
    if (igpu_from_cpu[0] != '\0') {
        snprintf(info->igpu_name, sizeof(info->igpu_name), "%s", igpu_from_cpu);
    } else if (detected_igpu[0] != '\0') {
        snprintf(info->igpu_name, sizeof(info->igpu_name), "%s", detected_igpu);
    }
    // Otherwise keeps default "N/A" from initialization

    // dGPU always comes from DRM detection
    if (detected_dgpu[0] != '\0') {
        snprintf(info->dgpu_name, sizeof(info->dgpu_name), "%s", detected_dgpu);
    }
    // Otherwise keeps default "N/A" from initialization

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

    // 11. Calculate optimal window width based on longest string
    // This ensures the dialog adapts to actual content rather than using fixed width
    Display *width_dpy = itn_core_get_display();
    XftFont *width_font = get_font();
    if (width_dpy && width_font) {
        int max_width = 0;
        char line[256];
        XGlyphInfo extents;

        // Build all lines exactly as they appear in rendering and measure each
        snprintf(line, sizeof(line), "  Desktop : AmiWB %s", info->amiwb_version);
        XftTextExtentsUtf8(width_dpy, width_font, (FcChar8*)line, strlen(line), &extents);
        if (extents.xOff > max_width) max_width = extents.xOff;

        snprintf(line, sizeof(line), "  Toolkit : libamiwb %s", info->toolkit_version);
        XftTextExtentsUtf8(width_dpy, width_font, (FcChar8*)line, strlen(line), &extents);
        if (extents.xOff > max_width) max_width = extents.xOff;

        snprintf(line, sizeof(line), "  Distro  : %s %s", info->os_name, info->os_version);
        XftTextExtentsUtf8(width_dpy, width_font, (FcChar8*)line, strlen(line), &extents);
        if (extents.xOff > max_width) max_width = extents.xOff;

        snprintf(line, sizeof(line), "  Kernel  : %s %s", info->kernel_name, info->kernel_version);
        XftTextExtentsUtf8(width_dpy, width_font, (FcChar8*)line, strlen(line), &extents);
        if (extents.xOff > max_width) max_width = extents.xOff;

        snprintf(line, sizeof(line), "  Memory  : %s", info->total_ram);
        XftTextExtentsUtf8(width_dpy, width_font, (FcChar8*)line, strlen(line), &extents);
        if (extents.xOff > max_width) max_width = extents.xOff;

        snprintf(line, sizeof(line), "  CPU     : %s %s", info->cpu_name, info->cpu_arch);
        XftTextExtentsUtf8(width_dpy, width_font, (FcChar8*)line, strlen(line), &extents);
        if (extents.xOff > max_width) max_width = extents.xOff;

        snprintf(line, sizeof(line), "  iGPU    : %s %s", info->igpu_name, info->igpu_ram);
        XftTextExtentsUtf8(width_dpy, width_font, (FcChar8*)line, strlen(line), &extents);
        if (extents.xOff > max_width) max_width = extents.xOff;

        snprintf(line, sizeof(line), "  dGPU    : %s %s", info->dgpu_name, info->dgpu_ram);
        XftTextExtentsUtf8(width_dpy, width_font, (FcChar8*)line, strlen(line), &extents);
        if (extents.xOff > max_width) max_width = extents.xOff;

        snprintf(line, sizeof(line), "  Xorg    : X11 %s", info->xorg_version);
        XftTextExtentsUtf8(width_dpy, width_font, (FcChar8*)line, strlen(line), &extents);
        if (extents.xOff > max_width) max_width = extents.xOff;

        snprintf(line, sizeof(line), "  Input   : %s", info->input_backend);
        XftTextExtentsUtf8(width_dpy, width_font, (FcChar8*)line, strlen(line), &extents);
        if (extents.xOff > max_width) max_width = extents.xOff;

        // Calculate optimal width: max_text_width + left_padding + right_padding + borders
        // Left padding is 20px (from text_x), use same for right padding
        int left_padding = 20;
        int right_padding = 20;
        int calculated_width = max_width + left_padding + right_padding +
                              BORDER_WIDTH_LEFT + BORDER_WIDTH_RIGHT_CLIENT;

        // Add extra 20px breathing room
        calculated_width += 20;

        // Ensure minimum width for button (Accept button width is 90px, centered needs ~130px minimum)
        int min_width_for_button = 300;
        if (calculated_width < min_width_for_button) {
            calculated_width = min_width_for_button;
        }

        info->optimal_width = calculated_width;
    } else {
        // Fallback to fixed width if font/display not available
        info->optimal_width = DIALOG_ABOUT_WIDTH;
    }
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

    // Use optimal width calculated from system info
    int initial_width = g_cached_sysinfo.optimal_width;

    // Create dialog using consistent lifecycle function
    Dialog *dialog = dialog_core_create(DIALOG_ABOUT, "About AmiWB", initial_width, DIALOG_ABOUT_HEIGHT);
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

    // Use optimal width calculated from actual content
    int dialog_width = sys_info->optimal_width;

    // Set size constraints and disable vertical resize (horizontal only)
    dialog->canvas->min_width = dialog_width;
    dialog->canvas->min_height = DIALOG_ABOUT_HEIGHT;
    dialog->canvas->max_height = DIALOG_ABOUT_HEIGHT;  // Force fixed height (min == max)
    dialog->canvas->resize_x_allowed = true;
    dialog->canvas->resize_y_allowed = false;  // Disable vertical resize

    // Create single centered "Accept" button
    int button_x = (dialog_width - BUTTON_WIDTH) / 2;  // Centered horizontally
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

    // Draw checkerboard border decoration (same style as delete dialog)
    int border_thickness = 10;
    int content_left = BORDER_WIDTH_LEFT;
    int content_top = BORDER_HEIGHT_TOP;
    int content_width = canvas->width - BORDER_WIDTH_LEFT - BORDER_WIDTH_RIGHT_CLIENT;

    // Calculate button area for bottom border
    int button_area_y = ABOUT_BUTTON_Y - 2;  // Start 2px above button
    int button_area_height = (ABOUT_BUTTON_Y + BUTTON_HEIGHT + 4) - button_area_y;

    // Top border
    dialog_base_draw_checkerboard(dest, content_left, content_top,
                            content_width, border_thickness);

    // Left border (up to button area)
    dialog_base_draw_checkerboard(dest, content_left,
                            content_top + border_thickness,
                            border_thickness,
                            button_area_y - (content_top + border_thickness));

    // Right border (up to button area)
    dialog_base_draw_checkerboard(dest,
                            content_left + content_width - border_thickness,
                            content_top + border_thickness,
                            border_thickness,
                            button_area_y - (content_top + border_thickness));

    // Bottom area encompassing the button
    dialog_base_draw_checkerboard(dest, content_left, button_area_y,
                            content_width, button_area_height);

    // Add 3D inset border (recessed look)
    int inner_left = content_left + border_thickness;
    int inner_top = content_top + border_thickness;
    int inner_width = content_width - (2 * border_thickness);
    int inner_height = button_area_y - inner_top;

    XRenderColor black = {0x0000, 0x0000, 0x0000, 0xffff};
    XRenderColor white = {0xffff, 0xffff, 0xffff, 0xffff};

    // Left/top shadow
    XRenderFillRectangle(dpy, PictOpOver, dest, &black,
                       inner_left, inner_top, 1, inner_height);
    XRenderFillRectangle(dpy, PictOpOver, dest, &black,
                       inner_left, inner_top, inner_width, 1);

    // Right/bottom highlight
    XRenderFillRectangle(dpy, PictOpOver, dest, &white,
                       inner_left + inner_width - 2, inner_top, 2, inner_height);
    XRenderFillRectangle(dpy, PictOpOver, dest, &white,
                       inner_left, inner_top + inner_height - 2, inner_width, 2);

    // Starting position for text
    int text_x = BORDER_WIDTH_LEFT + 20;  // Left margin (reduced from 40)
    int text_y = BORDER_HEIGHT_TOP + 30;  // Top margin
    int line_height = 22;  // Spacing between lines

    // Build and render each line
    char line[256];

    // Line 1: Empty line for spacing
    text_y += line_height;

    // Line 2: Desktop version
    snprintf(line, sizeof(line), "  Desktop : AmiWB %s", info->amiwb_version);
    XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_x, text_y,
                     (FcChar8*)line, strlen(line));
    text_y += line_height;

    // Line 3: Toolkit version
    snprintf(line, sizeof(line), "  Toolkit : libamiwb %s", info->toolkit_version);
    XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_x, text_y,
                     (FcChar8*)line, strlen(line));
    text_y += line_height;

    // Line 4: Empty line
    text_y += line_height;

    // Line 5: Distro
    snprintf(line, sizeof(line), "  Distro  : %s %s", info->os_name, info->os_version);
    XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_x, text_y,
                     (FcChar8*)line, strlen(line));
    text_y += line_height;

    // Line 6: Kernel
    snprintf(line, sizeof(line), "  Kernel  : %s %s", info->kernel_name, info->kernel_version);
    XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_x, text_y,
                     (FcChar8*)line, strlen(line));
    text_y += line_height;

    // Line 7: Memory
    snprintf(line, sizeof(line), "  Memory  : %s", info->total_ram);
    XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_x, text_y,
                     (FcChar8*)line, strlen(line));
    text_y += line_height;

    // Line 8: CPU
    snprintf(line, sizeof(line), "  CPU     : %s %s", info->cpu_name, info->cpu_arch);
    XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_x, text_y,
                     (FcChar8*)line, strlen(line));
    text_y += line_height;

    // Line 9: iGPU
    snprintf(line, sizeof(line), "  iGPU    : %s %s", info->igpu_name, info->igpu_ram);
    XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_x, text_y,
                     (FcChar8*)line, strlen(line));
    text_y += line_height;

    // Line 10: dGPU
    snprintf(line, sizeof(line), "  dGPU    : %s %s", info->dgpu_name, info->dgpu_ram);
    XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_x, text_y,
                     (FcChar8*)line, strlen(line));
    text_y += line_height;

    // Line 11: Xorg
    snprintf(line, sizeof(line), "  Xorg    : X11 %s", info->xorg_version);
    XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_x, text_y,
                     (FcChar8*)line, strlen(line));
    text_y += line_height;

    // Line 12: Input backend
    snprintf(line, sizeof(line), "  Input   : %s", info->input_backend);
    XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_x, text_y,
                     (FcChar8*)line, strlen(line));

    // Render the Accept button
    if (dialog->ok_button) {
        button_render(dialog->ok_button, dest, dpy, canvas->xft_draw);
    }

    // Clean up
    XftColorFree(dpy, canvas->visual, canvas->colormap, &xft_text);
}
