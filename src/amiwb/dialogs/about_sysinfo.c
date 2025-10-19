// File: about_sysinfo.c
// System information gathering for About dialog
// Refactored from dialog_about.c following The AmiWB Way principles

#include "about_sysinfo.h"
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

// GPU Vendor IDs (PCI standard)
#define PCI_VENDOR_AMD    0x1002
#define PCI_VENDOR_NVIDIA 0x10de
#define PCI_VENDOR_INTEL  0x8086
#define PCI_VENDOR_APPLE  0x106b

// ============================================================================
// Module State (Private - Cached System Information)
// ============================================================================

static SystemInfo g_cached_sysinfo;
static bool g_sysinfo_cached = false;

// ============================================================================
// Helper Functions (Static - Module Private)
// ============================================================================

// Read PCI vendor/device IDs from sysfs
// Returns: 0 on success, -1 on failure
static int read_pci_ids(int card_num, unsigned int *vendor_id, unsigned int *device_id) {
    char vendor_path[PATH_SIZE];
    char device_path[PATH_SIZE];

    snprintf(vendor_path, sizeof(vendor_path), "/sys/class/drm/card%d/device/vendor", card_num);
    snprintf(device_path, sizeof(device_path), "/sys/class/drm/card%d/device/device", card_num);

    FILE *vendor_file = fopen(vendor_path, "r");
    if (!vendor_file) return -1;

    if (fscanf(vendor_file, "0x%x", vendor_id) != 1) {
        fclose(vendor_file);
        return -1;
    }
    fclose(vendor_file);

    FILE *device_file = fopen(device_path, "r");
    if (!device_file) return -1;

    if (fscanf(device_file, "0x%x", device_id) != 1) {
        fclose(device_file);
        return -1;
    }
    fclose(device_file);

    return 0;
}

// Read PCI slot and driver name from uevent file
// Returns: 0 on success, -1 on failure
static int read_pci_info(int card_num, char *pci_slot, size_t slot_size,
                         char *driver_name, size_t driver_size) {
    char uevent_path[PATH_SIZE];
    snprintf(uevent_path, sizeof(uevent_path), "/sys/class/drm/card%d/device/uevent", card_num);

    FILE *uevent_file = fopen(uevent_path, "r");
    if (!uevent_file) {
        log_error("[WARNING] Failed to open uevent file: %s", uevent_path);
        return -1;
    }

    pci_slot[0] = '\0';
    driver_name[0] = '\0';

    char line[PATH_SIZE];
    while (fgets(line, sizeof(line), uevent_file)) {
        if (strncmp(line, "PCI_SLOT_NAME=", 14) == 0) {
            char *value = line + 14;
            size_t len = strlen(value);
            if (len > 0 && value[len - 1] == '\n') value[len - 1] = '\0';
            snprintf(pci_slot, slot_size, "%s", value);
        } else if (strncmp(line, "DRIVER=", 7) == 0) {
            char *value = line + 7;
            size_t len = strlen(value);
            if (len > 0 && value[len - 1] == '\n') value[len - 1] = '\0';
            snprintf(driver_name, driver_size, "%s", value);
        }
    }
    fclose(uevent_file);

    return (pci_slot[0] != '\0') ? 0 : -1;
}

// Read VRAM size from AMD-specific sysfs path
// Returns: 0 on success with vram_out populated, -1 on failure
static int read_vram_amd(int card_num, unsigned long long *vram_out) {
    char vram_path[PATH_SIZE];
    snprintf(vram_path, sizeof(vram_path), "/sys/class/drm/card%d/device/mem_info_vram_total", card_num);

    FILE *vram_file = fopen(vram_path, "r");
    if (!vram_file) return -1;

    if (fscanf(vram_file, "%llu", vram_out) != 1) {
        fclose(vram_file);
        return -1;
    }
    fclose(vram_file);

    return 0;
}

// Read VRAM size from PCI resource file (NVIDIA, Intel, others)
// Parses BAR1 (line 1) for prefetchable memory range
// Returns: 0 on success with vram_out populated, -1 on failure
static int read_vram_pci_resource(int card_num, unsigned long long *vram_out) {
    char resource_path[PATH_SIZE];
    snprintf(resource_path, sizeof(resource_path), "/sys/class/drm/card%d/device/resource", card_num);

    FILE *res_file = fopen(resource_path, "r");
    if (!res_file) return -1;

    char res_line[PATH_SIZE];
    int line_num = 0;

    while (fgets(res_line, sizeof(res_line), res_file) && line_num < 2) {
        if (line_num == 1) {  // BAR1 (second line, 0-indexed)
            unsigned long long start, end, flags;
            if (sscanf(res_line, "%llx %llx %llx", &start, &end, &flags) == 3) {
                // Check if prefetchable (bit 3 set in flags)
                // FIXED: Was 0x200 (bit 9), should be 0x8 (bit 3)
                if (flags & 0x8) {  // Prefetchable flag
                    *vram_out = end - start + 1;
                    fclose(res_file);
                    return 0;
                }
            }
        }
        line_num++;
    }
    fclose(res_file);

    return -1;
}

// Determine if GPU is integrated using multi-factor heuristic
// Uses VRAM size as primary indicator, with PCI topology as fallback
// iGPU: Typically <2GB VRAM (512MB-1GB common for APUs like Radeon 780M)
// dGPU: Typically ≥2GB VRAM (dedicated cards start at 2GB minimum)
// Fallback: Intel iGPUs are always on bus 00, device 02 or lower
static bool is_integrated_gpu(const char *pci_slot, unsigned long long vram_bytes) {
    if (!pci_slot) return false;

    // Primary heuristic: VRAM size
    // Modern iGPUs: 256MB-1GB (shared system RAM)
    // Modern dGPUs: 2GB+ (dedicated VRAM)
    // Threshold: 2GB handles edge cases (low-end dGPUs ≥2GB, high-end iGPUs ≤1GB)
    if (vram_bytes > 0) {
        unsigned long long two_gb = 2ULL * 1024ULL * 1024ULL * 1024ULL;
        return (vram_bytes < two_gb);  // <2GB = iGPU, ≥2GB = dGPU
    }

    // Fallback: PCI topology check (Intel iGPUs)
    // Intel integrated graphics: bus 00, device 00-02 (traditional location)
    // AMD APU iGPUs: can be on any bus (e.g., bus 65 on Ryzen laptops)
    if (strncmp(pci_slot, "0000:00:", 8) == 0) {
        int device = 0;
        if (sscanf(pci_slot + 8, "%d", &device) == 1) {
            return (device <= 2);
        }
    }

    // No VRAM info and not on bus 00 → assume dGPU
    return false;
}

// Forward declaration for PCI database parser
static int parse_pci_ids_database(unsigned int vendor_id, unsigned int device_id,
                                   char *name_out, size_t name_size);

// Get GPU model name from sysfs using vendor/device IDs
// Direct sysfs read - NO subprocess overhead (vs popen lspci)
// Returns: 0 on success, -1 on failure (falls back to driver name)
static int get_gpu_name_from_sysfs(int card_num, const char *pci_slot, const char *driver_name,
                                    char *name_out, size_t name_size) {
    if (!name_out || name_size == 0) {
        log_error("[ERROR] get_gpu_name_from_sysfs() called with invalid parameters");
        return -1;
    }

    unsigned int vendor_id = 0, device_id = 0;

    // Read vendor/device IDs from sysfs
    if (read_pci_ids(card_num, &vendor_id, &device_id) != 0) {
        log_error("[WARNING] Failed to read PCI IDs for card%d", card_num);
        return -1;
    }

    // Try reading model name from sysfs label (modern systems)
    char label_path[PATH_SIZE];
    snprintf(label_path, sizeof(label_path), "/sys/class/drm/card%d/device/label", card_num);

    FILE *label_file = fopen(label_path, "r");
    if (label_file) {
        char label[NAME_SIZE];
        if (fgets(label, sizeof(label), label_file)) {
            size_t len = strlen(label);
            if (len > 0 && label[len - 1] == '\n') label[len - 1] = '\0';
            snprintf(name_out, name_size, "%s", label);
            fclose(label_file);
            return 0;
        }
        fclose(label_file);
    }

    // Try PCI IDs database for full model name (e.g., "GeForce RTX 4050 Max-Q / Mobile")
    if (parse_pci_ids_database(vendor_id, device_id, name_out, name_size) == 0) {
        return 0;
    }

    // Fallback: Generate name from vendor ID + driver name
    const char *vendor_name = NULL;
    switch (vendor_id) {
        case PCI_VENDOR_AMD:    vendor_name = "AMD"; break;
        case PCI_VENDOR_NVIDIA: vendor_name = "NVIDIA"; break;
        case PCI_VENDOR_INTEL:  vendor_name = "Intel"; break;
        case PCI_VENDOR_APPLE:  vendor_name = "Apple"; break;
        default:                vendor_name = "Unknown"; break;
    }

    // Use driver name for more specific info if available
    if (driver_name && driver_name[0] != '\0') {
        if (strcmp(driver_name, "amdgpu") == 0) {
            snprintf(name_out, name_size, "AMD Radeon GPU");
        } else if (strcmp(driver_name, "nvidia") == 0) {
            snprintf(name_out, name_size, "NVIDIA GPU");
        } else if (strcmp(driver_name, "i915") == 0) {
            snprintf(name_out, name_size, "Intel GPU");
        } else if (strcmp(driver_name, "xe") == 0) {
            snprintf(name_out, name_size, "Intel Xe GPU");
        } else if (strcmp(driver_name, "asahi") == 0) {
            snprintf(name_out, name_size, "Apple GPU");
        } else {
            snprintf(name_out, name_size, "%s %s", vendor_name, driver_name);
        }
    } else {
        snprintf(name_out, name_size, "%s GPU", vendor_name);
    }

    return 0;
}

// Parse PCI IDs database to get GPU model name from vendor/device IDs
// Format: vendor lines start at column 0, device lines start with tab
// Example: "10de  NVIDIA Corporation" followed by "\t28e1  AD107M [GeForce RTX 4050 Max-Q / Mobile]"
// Returns: 0 on success, -1 on failure
static int parse_pci_ids_database(unsigned int vendor_id, unsigned int device_id,
                                   char *name_out, size_t name_size) {
    if (!name_out || name_size == 0) {
        return -1;
    }

    // Try standard PCI IDs database paths (Fedora/RHEL, Debian, etc.)
    const char *pci_ids_paths[] = {
        "/usr/share/hwdata/pci.ids",
        "/usr/share/misc/pci.ids",
        NULL
    };

    FILE *fp = NULL;
    for (int i = 0; pci_ids_paths[i] != NULL; i++) {
        fp = fopen(pci_ids_paths[i], "r");
        if (fp) break;
    }

    if (!fp) {
        return -1;  // No PCI database found
    }

    // Search for vendor line (format: "10de  NVIDIA Corporation")
    char vendor_line[16];
    snprintf(vendor_line, sizeof(vendor_line), "%04x  ", vendor_id);

    bool found_vendor = false;
    char line[512];

    while (fgets(line, sizeof(line), fp)) {
        if (!found_vendor) {
            // Look for vendor at start of line
            if (strncmp(line, vendor_line, 6) == 0) {
                found_vendor = true;
            }
        } else {
            // Inside vendor section, look for device (format: "\t28e1  Device Name")
            if (line[0] == '\t') {
                // Check if this is our device
                char device_line[16];
                snprintf(device_line, sizeof(device_line), "\t%04x  ", device_id);

                if (strncmp(line, device_line, 7) == 0) {
                    // Found device! Extract name starting at position 7
                    const char *device_name = line + 7;

                    // Remove trailing newline
                    size_t len = strlen(device_name);
                    while (len > 0 && (device_name[len - 1] == '\n' || device_name[len - 1] == '\r')) {
                        len--;
                    }

                    // Extract name from brackets if present (e.g., "AD107M [GeForce RTX 4050]" -> "GeForce RTX 4050")
                    const char *bracket_start = strchr(device_name, '[');
                    const char *bracket_end = strchr(device_name, ']');

                    if (bracket_start && bracket_end && bracket_end > bracket_start) {
                        // Copy name from inside brackets
                        size_t bracket_len = bracket_end - bracket_start - 1;
                        if (bracket_len > 0 && bracket_len < name_size) {
                            strncpy(name_out, bracket_start + 1, bracket_len);
                            name_out[bracket_len] = '\0';
                            fclose(fp);
                            return 0;
                        }
                    }

                    // No brackets or extraction failed - use entire name
                    if (len > 0 && len < name_size) {
                        strncpy(name_out, device_name, len);
                        name_out[len] = '\0';
                        fclose(fp);
                        return 0;
                    }
                }
            } else if (line[0] != '#' && line[0] != '\t') {
                // Hit next vendor section without finding our device
                break;
            }
        }
    }

    fclose(fp);
    return -1;  // Device not found in database
}

// Map Apple Silicon chip codes to marketing names
// Based on https://github.com/AsahiLinux/docs/wiki/Codenames
static const char *apple_chip_code_to_name(unsigned int code) {
    switch (code) {
        case 8103: return "Apple M1";
        case 6000: return "Apple M1 Pro";
        case 6001: return "Apple M1 Max";
        case 6002: return "Apple M1 Ultra";
        case 8112: return "Apple M2";
        case 6020: return "Apple M2 Pro";
        case 6021: return "Apple M2 Max";
        case 6022: return "Apple M2 Ultra";
        case 8122: return "Apple M3";
        case 6030: return "Apple M3 Pro";
        case 6031:
        case 6034: return "Apple M3 Max";
        case 8132: return "Apple M4";
        case 6040: return "Apple M4 Pro";
        case 6041: return "Apple M4 Max";
        default: return NULL;
    }
}

// Detect CPU name from device tree (ARM/Apple Silicon)
// Reads /proc/device-tree/compatible for vendor,model pairs
// Returns: 0 on success with name_out populated, -1 on failure
static int detect_cpu_from_devicetree(char *name_out, size_t name_size) {
    if (!name_out || name_size == 0) return -1;

    FILE *fp = fopen("/proc/device-tree/compatible", "r");
    if (!fp) return -1;

    // Device tree compatible format: "vendor,model\0vendor,model\0..."
    char buffer[512];
    size_t bytes_read = fread(buffer, 1, sizeof(buffer) - 1, fp);
    fclose(fp);

    if (bytes_read == 0) return -1;
    buffer[bytes_read] = '\0';

    // Parse backwards to find first valid vendor,model pair
    // Skip entries ending with "-platform" or "-soc" (not CPU identifiers)
    for (size_t i = 0; i < bytes_read; ) {
        size_t len = strlen(buffer + i);
        if (len == 0) break;

        char *comma = strchr(buffer + i, ',');
        if (comma) {
            *comma = '\0';  // Split vendor and model
            const char *vendor = buffer + i;
            const char *model = comma + 1;

            // Skip platform/soc entries
            if (strstr(model, "-platform") || strstr(model, "-soc")) {
                i += len + 1;
                continue;
            }

            // Apple Silicon detection
            if (strcmp(vendor, "apple") == 0 && model[0] == 't') {
                unsigned int chip_code = (unsigned int)strtoul(model + 1, NULL, 10);
                const char *chip_name = apple_chip_code_to_name(chip_code);
                if (chip_name) {
                    snprintf(name_out, name_size, "%s", chip_name);
                    return 0;
                }
                // Fallback: "Apple Silicon tXXXX"
                snprintf(name_out, name_size, "Apple Silicon %s", model);
                return 0;
            }

            // Other vendors: just use model name
            snprintf(name_out, name_size, "%s", model);
            return 0;
        }

        i += len + 1;  // Move to next entry
    }

    return -1;
}

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
// Uses direct sysfs reads for model names and VRAM detection
// Returns: 0 on success, -1 on failure
static int detect_gpus(char *igpu_out, size_t igpu_size, char *dgpu_out, size_t dgpu_size,
                       char *igpu_ram_out, size_t igpu_ram_size, char *dgpu_ram_out, size_t dgpu_ram_size) {
    if (!igpu_out || !dgpu_out || !igpu_ram_out || !dgpu_ram_out) {
        log_error("[ERROR] detect_gpus() called with NULL output buffers");
        return -1;
    }

    igpu_out[0] = '\0';
    dgpu_out[0] = '\0';
    igpu_ram_out[0] = '\0';
    dgpu_ram_out[0] = '\0';

    // Scan up to 8 card devices (card0-card7)
    for (int i = 0; i < 8; i++) {
        char pci_slot[32] = {0};
        char driver_name[NAME_SIZE] = {0};

        // Read PCI slot and driver name from uevent
        if (read_pci_info(i, pci_slot, sizeof(pci_slot), driver_name, sizeof(driver_name)) != 0) {
            continue;  // Card doesn't exist or couldn't read info
        }

        // Detect VRAM size (try AMD path first, then PCI resource fallback)
        unsigned long long vram_bytes = 0;
        if (read_vram_amd(i, &vram_bytes) != 0) {
            read_vram_pci_resource(i, &vram_bytes);  // Fallback for NVIDIA/Intel
        }

        // Classify GPU using VRAM size heuristic (<2GB = iGPU, ≥2GB = dGPU)
        bool is_igpu = is_integrated_gpu(pci_slot, vram_bytes);

        // Get GPU model name from sysfs (no subprocess overhead)
        char gpu_name[NAME_SIZE] = {0};
        if (get_gpu_name_from_sysfs(i, pci_slot, driver_name, gpu_name, sizeof(gpu_name)) != 0) {
            log_error("[WARNING] Failed to get GPU name for card%d, skipping", i);
            continue;
        }

        // Format VRAM size (MB or GB based on size)
        char vram_str[32] = {0};
        if (vram_bytes > 0) {
            unsigned long long one_gb = 1024ULL * 1024ULL * 1024ULL;
            if (vram_bytes >= one_gb) {
                snprintf(vram_str, sizeof(vram_str), "%llu GB", vram_bytes / one_gb);
            } else {
                snprintf(vram_str, sizeof(vram_str), "%llu MB", vram_bytes / (1024ULL * 1024ULL));
            }
        }

        // Store in appropriate output buffers
        if (is_igpu && igpu_out[0] == '\0') {
            snprintf(igpu_out, igpu_size, "%s", gpu_name);
            if (vram_str[0] != '\0') {
                snprintf(igpu_ram_out, igpu_ram_size, "%s", vram_str);
            }
        } else if (!is_igpu && dgpu_out[0] == '\0') {
            snprintf(dgpu_out, dgpu_size, "%s", gpu_name);
            if (vram_str[0] != '\0') {
                snprintf(dgpu_ram_out, dgpu_ram_size, "%s", vram_str);
            }
        }

        // Early exit: Both GPUs detected, stop scanning
        if (igpu_out[0] != '\0' && dgpu_out[0] != '\0') {
            break;
        }
    }

    return 0;
}

// ============================================================================
// Public API Implementation
// ============================================================================

// Gather all system information (caches result for subsequent calls)
// Returns: pointer to cached SystemInfo, or NULL on failure
SystemInfo *about_sysinfo_gather(void) {
    if (g_sysinfo_cached) {
        return &g_cached_sysinfo;  // Return cached data
    }

    SystemInfo *info = &g_cached_sysinfo;

    // Initialize all fields to defaults
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
        char line[PATH_SIZE];
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
    char igpu_from_cpu[NAME_SIZE] = {0};  // Temporary buffer for iGPU extracted from CPU string
    FILE *cpu_file = fopen("/proc/cpuinfo", "r");
    if (cpu_file) {
        char line[PATH_SIZE];
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

        // Fallback for ARM/Apple Silicon: Try device-tree if /proc/cpuinfo failed
        if (!found) {
            if (detect_cpu_from_devicetree(info->cpu_name, sizeof(info->cpu_name)) == 0) {
                found = true;
            }
        }
    }

    // 7-8. GPU detection via /sys/class/drm (standard Linux DRM subsystem)
    char detected_igpu[NAME_SIZE] = {0};
    char detected_dgpu[NAME_SIZE] = {0};
    char detected_igpu_ram[32] = {0};
    char detected_dgpu_ram[32] = {0};
    detect_gpus(detected_igpu, sizeof(detected_igpu), detected_dgpu, sizeof(detected_dgpu),
               detected_igpu_ram, sizeof(detected_igpu_ram), detected_dgpu_ram, sizeof(detected_dgpu_ram));

    // Prefer iGPU info from CPU string (more specific model info)
    // Fall back to DRM detection if CPU string didn't contain GPU info
    if (igpu_from_cpu[0] != '\0') {
        snprintf(info->igpu_name, sizeof(info->igpu_name), "%s", igpu_from_cpu);
    } else if (detected_igpu[0] != '\0') {
        snprintf(info->igpu_name, sizeof(info->igpu_name), "%s", detected_igpu);
    }
    // Otherwise keeps default "N/A" from initialization

    // Set iGPU RAM if detected
    if (detected_igpu_ram[0] != '\0') {
        snprintf(info->igpu_ram, sizeof(info->igpu_ram), "%s", detected_igpu_ram);
    }

    // dGPU always comes from DRM detection
    if (detected_dgpu[0] != '\0') {
        snprintf(info->dgpu_name, sizeof(info->dgpu_name), "%s", detected_dgpu);
    }
    // Otherwise keeps default "N/A" from initialization

    // Set dGPU RAM if detected
    if (detected_dgpu_ram[0] != '\0') {
        snprintf(info->dgpu_ram, sizeof(info->dgpu_ram), "%s", detected_dgpu_ram);
    }

    // 9. Xorg version (from display)
    Display *dpy = itn_core_get_display();
    if (dpy) {
        int vendor_release = VendorRelease(dpy);
        // XOrg version format: major.minor.patch encoded as integer
        int major = vendor_release / XORG_MAJOR_DIVISOR;
        int minor = (vendor_release / XORG_MINOR_DIVISOR) % 100;
        int patch = (vendor_release / XORG_PATCH_DIVISOR) % 100;
        snprintf(info->xorg_version, sizeof(info->xorg_version), "%d.%d.%d", major, minor, patch);
    }

    // 10. Input backend (assume libinput on modern systems)
    snprintf(info->input_backend, sizeof(info->input_backend), "libinput");

    // 11. Calculate optimal window width based on longest string
    Display *width_dpy = itn_core_get_display();
    XftFont *width_font = get_font();
    if (width_dpy && width_font) {
        int max_width = 0;
        char line[PATH_SIZE];
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
        info->optimal_width = 600;  // DIALOG_ABOUT_WIDTH fallback
    }

    // Mark as cached for subsequent calls
    g_sysinfo_cached = true;

    return info;
}

// Get cached system information (fast, no re-detection)
// Returns: pointer to cached SystemInfo, or NULL if not yet gathered
SystemInfo *about_sysinfo_get_cached(void) {
    if (!g_sysinfo_cached) {
        log_error("[WARNING] about_sysinfo_get_cached() called before about_sysinfo_gather()");
        return NULL;
    }
    return &g_cached_sysinfo;
}
