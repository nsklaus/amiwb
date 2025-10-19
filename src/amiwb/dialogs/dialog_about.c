// File: dialog_about.c
// About dialog implementation - displays AmiWB and system information
// UI rendering only - hardware detection delegated to about_sysinfo module

#include "dialog_internal.h"
#include "about_sysinfo.h"
#include "../render/rnd_public.h"
#include "../intuition/itn_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Constants
// ============================================================================

#define DIALOG_ABOUT_HEIGHT 390
#define ABOUT_BUTTON_Y 340

// ============================================================================
// About Dialog Creation
// ============================================================================

// Create and show about dialog (no callbacks needed - just closes on Accept)
void show_about_dialog(void) {
    // Gather system info on first call (cached by about_sysinfo module)
    SystemInfo *sys_info = about_sysinfo_gather();
    if (!sys_info) {
        log_error("[ERROR] Failed to gather system information");
        return;
    }

    // Use optimal width calculated from system info
    int initial_width = sys_info->optimal_width;

    // Create dialog using consistent lifecycle function
    Dialog *dialog = dialog_core_create(DIALOG_ABOUT, "About AmiWB", initial_width, DIALOG_ABOUT_HEIGHT);
    if (!dialog) {
        log_error("[ERROR] Failed to create about dialog - feature unavailable");
        return;  // Graceful degradation
    }

    // Store system info pointer in user_data (borrowed reference - owned by about_sysinfo module)
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

    // Get system info from user_data (borrowed reference from about_sysinfo module)
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
    int text_x = BORDER_WIDTH_LEFT + 20;  // Left margin
    int text_y = BORDER_HEIGHT_TOP + 30;  // Top margin
    int line_height = 22;  // Spacing between lines

    // Build and render each line
    char line[PATH_SIZE];

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

    // Line 9: iGPU (show model name only, no RAM)
    snprintf(line, sizeof(line), "  iGPU    : %s", info->igpu_name);
    XftDrawStringUtf8(canvas->xft_draw, &xft_text, font, text_x, text_y,
                     (FcChar8*)line, strlen(line));
    text_y += line_height;

    // Line 10: dGPU (show model name only, no RAM)
    snprintf(line, sizeof(line), "  dGPU    : %s", info->dgpu_name);
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
