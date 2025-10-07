// Compositing operations using XRender extension
// This module handles the actual rendering of windows with transparency
// FOLLOWS THE PLAN: Canvas IS the compositing structure, no separate CompWin

#include "../config.h"
#include "itn_internal.h"
#include "../render_public.h"
#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

// Compositing state
static Window overlay_window = None;
static Picture overlay_pict = None;
static Picture back_buffer = None;
static Pixmap back_pixmap = None;
static Picture root_pict = None;
static Picture wallpaper_pict = None;
static XRenderPictFormat *format_32 = NULL;
static XRenderPictFormat *format_24 = NULL;

// External references (none needed - all in itn_public.h via itn_internal.h)

// Lightweight structure for override-redirect windows (popup menus, tooltips)
typedef struct OverrideWin {
    Window win;
    Pixmap pixmap;
    Picture picture;
    Damage damage;
    int x, y, width, height;
    int depth;
    bool needs_repaint;
    struct OverrideWin *next;
} OverrideWin;

// Override-redirect window tracking
static OverrideWin *override_list = NULL;
static int override_count = 0;

// Temporary error handler to suppress errors during compositor setup
// Tooltips/popups can be destroyed microseconds after mapping, causing:
// BadWindow, BadDrawable, BadDamage, BadMatch, RenderBadPicture
static int ignore_compositor_setup_errors(Display *dpy, XErrorEvent *error) {
    // Silently ignore these expected errors during override window compositing
    if (error->error_code == BadWindow ||   // code 3
        error->error_code == BadDrawable || // code 9
        error->error_code == BadMatch ||    // code 8
        error->error_code == 152 ||         // BadDamage
        error->error_code == 143) {         // RenderBadPicture
        return 0;  // Suppress error
    }
    // Call the default error handler for unexpected errors
    extern int x_error_handler(Display *dpy, XErrorEvent *error);
    return x_error_handler(dpy, error);
}

// Helper: Create XRender picture from pixmap
static Picture create_picture_from_pixmap(Display *dpy, Pixmap pixmap, int depth) {
    if (!pixmap) return None;

    XRenderPictFormat *format = XRenderFindStandardFormat(dpy,
        depth == 32 ? PictStandardARGB32 : PictStandardRGB24);

    if (!format) {
        format = XRenderFindVisualFormat(dpy, DefaultVisual(dpy, DefaultScreen(dpy)));
    }

    if (format) {
        XRenderPictureAttributes pa = {0};
        pa.subwindow_mode = IncludeInferiors;
        return XRenderCreatePicture(dpy, pixmap, format, CPSubwindowMode, &pa);
    }

    return None;
}

// Helper: Free picture safely
static void safe_free_picture(Display *dpy, Picture *pict) {
    if (*pict) {
        XRenderFreePicture(dpy, *pict);
        *pict = None;
    }
}

// Helper: Free pixmap safely
static void safe_free_pixmap(Display *dpy, Pixmap *pm) {
    if (*pm) {
        XFreePixmap(dpy, *pm);
        *pm = None;
    }
}

// Get overlay window (for external modules to check)
Window itn_composite_get_overlay_window(void) {
    return overlay_window;
}

// Initialize compositor overlay window
bool itn_composite_init_overlay(void) {
    Display *dpy = itn_core_get_display();
    // Don't check g_compositor_active - we're called DURING init!
    if (!dpy) {
        log_error("[COMPOSITE] No display available");
        return false;
    }

    Window root_win = RootWindow(dpy, itn_core_get_screen());

    // Get the composite overlay window
    overlay_window = XCompositeGetOverlayWindow(dpy, root_win);
    if (!overlay_window) {
        log_error("[COMPOSITE] Failed to get overlay window from XCompositeGetOverlayWindow");
        return false;
    }

    // Make overlay window transparent to input using Shape extension
    // Pass 0 rectangles to create empty input region (input passes through)
    XShapeCombineRectangles(dpy, overlay_window, ShapeInput, 0, 0, NULL, 0, ShapeSet, 0);

    // Get standard formats
    format_32 = XRenderFindStandardFormat(dpy, PictStandardARGB32);
    format_24 = XRenderFindStandardFormat(dpy, PictStandardRGB24);

    // Create overlay picture
    Visual *vis = DefaultVisual(dpy, itn_core_get_screen());
    XRenderPictFormat *format = XRenderFindVisualFormat(dpy, vis);
    if (!format) {
        log_error("[COMPOSITE] Failed to find visual format for overlay (visual=%p)", vis);
        XCompositeReleaseOverlayWindow(dpy, root_win);
        overlay_window = None;
        return false;
    }

    XRenderPictureAttributes pa = {0};
    pa.subwindow_mode = IncludeInferiors;
    overlay_pict = XRenderCreatePicture(dpy, overlay_window, format, CPSubwindowMode, &pa);
    if (!overlay_pict) {
        log_error("[COMPOSITE] Failed to create overlay picture");
        XCompositeReleaseOverlayWindow(dpy, root_win);
        overlay_window = None;
        return false;
    }

    // Create back buffer for double buffering
    if (!itn_composite_create_back_buffer()) {
        log_error("[COMPOSITE] Failed to create back buffer");
        safe_free_picture(dpy, &overlay_pict);
        XCompositeReleaseOverlayWindow(dpy, root_win);
        overlay_window = None;
        return false;
    }

    return true;
}

// Cleanup compositor overlay
void itn_composite_cleanup_overlay(void) {
    Display *dpy = itn_core_get_display();
    if (!dpy) return;

    // Clean up compositing data from all canvases
    int count = itn_manager_get_count();
    for (int i = 0; i < count; i++) {
        Canvas *c = itn_manager_get_canvas(i);
        if (!c) continue;

        // Free damage object
        if (c->comp_damage) {
            XDamageDestroy(dpy, c->comp_damage);
            c->comp_damage = None;
        }

        // Free picture AND pixmap (XComposite does NOT auto-free pixmaps!)
        safe_free_picture(dpy, &c->comp_picture);
        safe_free_pixmap(dpy, &c->comp_pixmap);
    }

    // Free global resources
    safe_free_picture(dpy, &overlay_pict);
    safe_free_picture(dpy, &back_buffer);
    safe_free_picture(dpy, &root_pict);
    safe_free_picture(dpy, &wallpaper_pict);
    safe_free_pixmap(dpy, &back_pixmap);

    // Release overlay window
    if (overlay_window) {
        XCompositeReleaseOverlayWindow(dpy, RootWindow(dpy, itn_core_get_screen()));
        overlay_window = None;
    }
}

// Create/recreate back buffer for double buffering
bool itn_composite_create_back_buffer(void) {
    Display *dpy = itn_core_get_display();
    // Don't check g_compositor_active here - we might be called during init!
    if (!dpy) return false;

    // Free old buffers
    safe_free_picture(dpy, &back_buffer);
    safe_free_pixmap(dpy, &back_pixmap);

    // Get actual screen dimensions if width/height are 0
    int actual_width = itn_core_get_screen_width();
    int actual_height = itn_core_get_screen_height();
    int actual_depth = itn_core_get_screen_depth();
    int actual_screen = itn_core_get_screen();
    Window actual_root = itn_core_get_root();

    if (actual_width == 0 || actual_height == 0) {
        actual_screen = DefaultScreen(dpy);
        actual_width = DisplayWidth(dpy, actual_screen);
        actual_height = DisplayHeight(dpy, actual_screen);
        actual_root = RootWindow(dpy, actual_screen);
        actual_depth = DefaultDepth(dpy, actual_screen);
        if (actual_depth < 24) actual_depth = 32; // Force 32-bit for compositing
        log_error("[COMPOSITE] Width/height were 0, fetched: w=%d h=%d d=%d",
                  actual_width, actual_height, actual_depth);
    }

    // Create new back buffer pixmap
    back_pixmap = XCreatePixmap(dpy, actual_root, actual_width, actual_height, actual_depth);
    if (!back_pixmap) {
        log_error("[COMPOSITE] Failed to create back buffer pixmap (w=%d, h=%d, d=%d)",
                  actual_width, actual_height, actual_depth);
        return false;
    }

    // Create picture from pixmap
    back_buffer = create_picture_from_pixmap(dpy, back_pixmap, actual_depth);
    if (!back_buffer) {
        log_error("[COMPOSITE] Failed to create back buffer picture");
        safe_free_pixmap(dpy, &back_pixmap);
        return false;
    }

    return true;
}

// Setup compositing for a canvas (called when canvas is created/mapped)
void itn_composite_setup_canvas(Canvas *canvas) {
    if (!canvas || !itn_composite_is_active()) return;

    Display *dpy = itn_core_get_display();
    if (!dpy) return;


    // Get the composite pixmap for this window
    canvas->comp_pixmap = XCompositeNameWindowPixmap(dpy, canvas->win);
    if (!canvas->comp_pixmap) {
        log_error("[COMPOSITE] Failed to get named pixmap for window 0x%lx", canvas->win);
        return;
    }

    // Create XRender picture from the pixmap
    int win_depth = canvas->depth ? canvas->depth : itn_core_get_screen_depth();
    canvas->comp_picture = create_picture_from_pixmap(dpy, canvas->comp_pixmap, win_depth);

    // Create damage tracking for this window
    if (!canvas->comp_damage) {
        // Use RawRectangles - most reliable for immediate damage processing
        // GPU-accelerated clients (Kitty, etc.) will generate many events at high FPS
        // This is expected behavior, not a bug - we need every frame notification
        canvas->comp_damage = XDamageCreate(dpy, canvas->win, XDamageReportRawRectangles);
    }

    // Mark as needing repaint
    canvas->comp_needs_repaint = true;
}

// Update canvas pixmap (called after resize or when pixmap becomes invalid)
void itn_composite_update_canvas_pixmap(Canvas *canvas) {
    if (!canvas || !itn_composite_is_active()) return;

    Display *dpy = itn_core_get_display();
    if (!dpy || !canvas->win) return;

    // Free old picture AND pixmap to prevent memory leak
    // XCompositeNameWindowPixmap creates a NEW pixmap each time - must free old one
    safe_free_picture(dpy, &canvas->comp_picture);
    safe_free_pixmap(dpy, &canvas->comp_pixmap);

    // Get new composite pixmap (allocates new GPU memory)
    canvas->comp_pixmap = XCompositeNameWindowPixmap(dpy, canvas->win);
    if (canvas->comp_pixmap) {
        int win_depth = canvas->depth ? canvas->depth : itn_core_get_screen_depth();
        canvas->comp_picture = create_picture_from_pixmap(dpy, canvas->comp_pixmap, win_depth);
    }

    // Mark for repaint - the compositor will call redraw_canvas if needed
    canvas->comp_needs_repaint = true;
    DAMAGE_CANVAS(canvas);
    SCHEDULE_FRAME();
}

// Add an override-redirect window to tracking
void itn_composite_add_override(Window win, XWindowAttributes *attrs) {
    if (!itn_composite_is_active()) return;

    Display *dpy = itn_core_get_display();
    if (!dpy) return;

    // CRITICAL: Never add our own overlay window!
    if (win == overlay_window) {
        log_error("[COMPOSITE] WARNING: Attempted to add overlay window 0x%lx as override!", win);
        return;
    }

    // Check if already tracked
    OverrideWin *ow = override_list;
    while (ow) {
        if (ow->win == win) return;  // Already tracked
        ow = ow->next;
    }

    // Create new entry
    ow = calloc(1, sizeof(OverrideWin));
    if (!ow) return;

    ow->win = win;
    ow->x = attrs->x;
    ow->y = attrs->y;
    ow->width = attrs->width;
    ow->height = attrs->height;
    ow->depth = attrs->depth;

    // CRITICAL: Tooltips can be destroyed microseconds after mapping
    // Install temporary error handler to suppress expected errors
    XErrorHandler old_handler = XSetErrorHandler(ignore_compositor_setup_errors);

    // Verify window still exists before compositing
    XWindowAttributes verify_attrs;
    if (!XGetWindowAttributes(dpy, win, &verify_attrs)) {
        // Window already destroyed - don't composite it
        XSetErrorHandler(old_handler);
        free(ow);
        return;
    }

    // Get composite pixmap (can fail if window destroyed between checks)
    ow->pixmap = XCompositeNameWindowPixmap(dpy, win);

    // Create XRender picture
    if (ow->pixmap && ow->pixmap != None) {
        XRenderPictFormat *format = XRenderFindVisualFormat(dpy, attrs->visual);
        if (format) {
            XRenderPictureAttributes pa = {0};
            pa.subwindow_mode = IncludeInferiors;
            ow->picture = XRenderCreatePicture(dpy, ow->pixmap, format,
                                               CPSubwindowMode, &pa);
        }
    }

    // Create damage tracking for continuous updates
    if (ow->picture && ow->picture != None) {
        // Use RawRectangles for reliable damage notification (same as canvas windows)
        ow->damage = XDamageCreate(dpy, win, XDamageReportRawRectangles);
    }

    // Force error processing before restoring handler
    XSync(dpy, False);
    XSetErrorHandler(old_handler);

    // Validate all resources created successfully
    if (!ow->pixmap || !ow->picture || !ow->damage) {
        // Cleanup partial resources
        if (ow->damage) XDamageDestroy(dpy, ow->damage);
        if (ow->picture) XRenderFreePicture(dpy, ow->picture);
        // Don't free pixmap - XComposite owns it
        free(ow);
        return;
    }

    // All resources created successfully - add to list
    ow->next = override_list;
    override_list = ow;
    override_count++;
}

// Remove an override-redirect window
bool itn_composite_remove_override(Window win) {
    if (!itn_composite_is_active()) return false;

    Display *dpy = itn_core_get_display();
    if (!dpy) return false;

    OverrideWin **prev = &override_list;
    OverrideWin *ow = override_list;

    while (ow) {
        if (ow->win == win) {
            // Found it - remove from list
            *prev = ow->next;

            // Clean up resources - CRITICAL: flush pending damage events first
            // to prevent BadDamage/RenderBadPicture errors from tooltips/popups
            if (ow->damage) {
                // Clear all accumulated damage and flush the event queue
                XDamageSubtract(dpy, ow->damage, None, None);
                XSync(dpy, False);  // Wait for all pending events to be processed
                XDamageDestroy(dpy, ow->damage);
            }
            if (ow->picture) XRenderFreePicture(dpy, ow->picture);
            // Don't free pixmap - XComposite owns it

            free(ow);
            override_count--;
            return true;
        }
        prev = &ow->next;
        ow = ow->next;
    }

    return false;
}

// Render all windows to back buffer
void itn_composite_render_all(void) {
    Display *dpy = itn_core_get_display();
    if (!dpy || !itn_composite_is_active() || !back_buffer) {
        log_error("[COMPOSITE] render_all failed: dpy=%p, active=%d, back_buffer=%p",
                  dpy, itn_composite_is_active(), back_buffer);
        return;
    }

    // Get actual dimensions
    int actual_width = itn_core_get_screen_width() > 0 ? itn_core_get_screen_width() : DisplayWidth(dpy, DefaultScreen(dpy));
    int actual_height = itn_core_get_screen_height() > 0 ? itn_core_get_screen_height() : DisplayHeight(dpy, DefaultScreen(dpy));

    // log_error("[COMPOSITE] Rendering all windows (w=%d, h=%d)", actual_width, actual_height);

    // Clear back buffer
    XRenderColor black = {0, 0, 0, 0xffff};
    XRenderFillRectangle(dpy, PictOpSrc, back_buffer, &black, 0, 0, actual_width, actual_height);

    // TODO: Render wallpaper if available
    if (wallpaper_pict) {
        XRenderComposite(dpy, PictOpSrc, wallpaper_pict, None, back_buffer,
                        0, 0, 0, 0, 0, 0, actual_width, actual_height);
    }

    // Render each canvas from bottom to top using X11 stacking order
    Canvas *visible[256];
    int visible_count = 0;

    // Query X11 for actual stacking order (bottom-to-top)
    Window root_return, parent_return;
    Window *children = NULL;
    unsigned int nchildren = 0;

    if (XQueryTree(dpy, itn_core_get_root(), &root_return, &parent_return, &children, &nchildren)) {
        // Children are returned in bottom-to-top stacking order
        for (unsigned int i = 0; i < nchildren && visible_count < 256; i++) {
            Window w = children[i];

            // Skip overlay window if it exists
            if (w == overlay_window) continue;

            // Find canvas for this window
            Canvas *c = itn_canvas_find_by_window(w);
            if (c && c->win) {
                // Skip canvases that are closing (client_win cleared but frame not yet destroyed)
                // This happens after close button is clicked but before DestroyNotify
                if (c->close_request_sent && !c->client_win) {
                    continue;
                }

                // Safe attribute query handles window destruction race
                XWindowAttributes attrs;
                if (safe_get_window_attributes(dpy, c->win, &attrs)) {
                    if (attrs.map_state == IsViewable) {
                        // Check compositor visibility flag (used for hiding menubar during fullscreen)
                        // Only skip if explicitly set to false (menubar during fullscreen)
                        if (c->type == MENU && !c->comp_visible) {
                            continue;  // Skip hidden menubar
                        }
                        visible[visible_count++] = c;
                    }
                }
            }
            // Override-redirect windows are now tracked via MapNotify/UnmapNotify events
            // No need to poll for them here - saves expensive XGetWindowAttributes calls
        }
        if (children) XFree(children);
    }

    // Compositor is working correctly - no need to log canvas count
    // (Was: debug logging during development)

    // Render in stacking order (bottom to top)
    for (int i = 0; i < visible_count; i++) {
        Canvas *c = visible[i];

        // If this canvas needs repainting, ensure its window content is current
        // This handles button state changes and other visual updates
        if (c->comp_needs_repaint) {
            redraw_canvas(c);
        }

        // Ensure canvas has compositing data - create if missing
        if (!c->comp_pixmap) {
            // Window is mapped but compositor pixmap is missing (race between XMapWindow and MapEvent)
            // Try to create it now instead of waiting for MapEvent handler
            c->comp_pixmap = XCompositeNameWindowPixmap(dpy, c->win);
            if (!c->comp_pixmap) {
                // Still can't create - window might not be mapped yet, skip for now
                continue;
            }
            // Force repaint since we just created the pixmap (it's empty)
            c->comp_needs_repaint = true;
        }

        if (!c->comp_picture && c->comp_pixmap) {
            int win_depth = c->depth ? c->depth : itn_core_get_screen_depth();
            c->comp_picture = create_picture_from_pixmap(dpy, c->comp_pixmap, win_depth);
        }

        // Ensure damage tracking exists (needed for future updates)
        if (!c->comp_damage) {
            c->comp_damage = XDamageCreate(dpy, c->win, XDamageReportRawRectangles);
        }

        if (c->comp_picture) {
            // Composite the window
            // TODO: Handle transparency/opacity
            // log_error("[COMPOSITE] Compositing canvas %d at %d,%d size %dx%d",
            //           i, c->x, c->y, c->width, c->height);
            XRenderComposite(dpy, PictOpOver, c->comp_picture, None, back_buffer,
                            0, 0, 0, 0, c->x, c->y, c->width, c->height);

            // Update metrics - properly access the itn_render metrics
            extern void itn_render_update_metrics(int composite_calls, uint64_t pixels, int visible);
            itn_render_update_metrics(1, (uint64_t)(c->width * c->height), visible_count);
        } else {
            log_error("[COMPOSITE] Canvas %d has no comp_picture", i);
        }
    }

    // Pass 4: Render override-redirect windows (popup menus, tooltips) - TOPMOST
    // Note: GTK menus are children of root, NOT children of the application window!
    if (override_count > 0) {
        OverrideWin *ow = override_list;
        while (ow) {
            if (ow->picture) {
                // Check if window is still valid and mapped
                // Safe attribute query handles window destruction race
                XWindowAttributes attrs;
                if (safe_get_window_attributes(dpy, ow->win, &attrs) &&
                    attrs.map_state == IsViewable) {

                    // Update position if it moved
                    ow->x = attrs.x;
                    ow->y = attrs.y;
                    ow->width = attrs.width;
                    ow->height = attrs.height;

                    // Composite with transparency support
                    int op = (ow->depth == 32) ? PictOpOver : PictOpSrc;
                    XRenderComposite(dpy, op, ow->picture, None, back_buffer,
                                    0, 0, 0, 0, ow->x, ow->y,
                                    ow->width, ow->height);

                    // Update metrics
                    extern void itn_render_update_metrics(int composite_calls, uint64_t pixels, int visible);
                    itn_render_update_metrics(1, (uint64_t)(ow->width * ow->height),
                                             visible_count + override_count);
                }
            }
            ow = ow->next;
        }
    }

    // Swap buffers to display
    itn_composite_swap_buffers();
}

// Swap back buffer to front (display on overlay or root)
void itn_composite_swap_buffers(void) {
    Display *dpy = itn_core_get_display();
    if (!dpy || !itn_composite_is_active() || !back_buffer) {
        log_error("[COMPOSITE] swap_buffers failed: dpy=%p, active=%d, back_buffer=%p",
                  dpy, itn_composite_is_active(), back_buffer);
        return;
    }

    // log_error("[COMPOSITE] Swapping buffers to display");

    // Determine output target
    Picture output_target = overlay_pict;

    // If no overlay, we need to composite directly to root window
    if (!output_target) {
        // Create a picture for the root window if we don't have one
        static Picture root_pict = None;
        if (!root_pict) {
            XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy, DefaultVisual(dpy, itn_core_get_screen()));
            if (fmt) {
                XRenderPictureAttributes pa = {0};
                pa.subwindow_mode = IncludeInferiors;
                root_pict = XRenderCreatePicture(dpy, itn_core_get_root(), fmt, CPSubwindowMode, &pa);
            }
        }
        output_target = root_pict;
    }

    if (!output_target) {
        log_error("[COMPOSITE] No output target available for compositing!");
        return;
    }

    // Get actual dimensions
    int actual_width = itn_core_get_screen_width() > 0 ? itn_core_get_screen_width() : DisplayWidth(dpy, DefaultScreen(dpy));
    int actual_height = itn_core_get_screen_height() > 0 ? itn_core_get_screen_height() : DisplayHeight(dpy, DefaultScreen(dpy));

    // Copy back buffer to output target (overlay or root)
    XRenderComposite(dpy, PictOpSrc, back_buffer, None, output_target,
                    0, 0, 0, 0, 0, 0, actual_width, actual_height);

    XSync(dpy, False);
}

// Render a single canvas (for partial updates)
void itn_composite_render_canvas(Canvas *canvas) {
    if (!canvas || !itn_composite_is_active()) return;

    Display *dpy = itn_core_get_display();
    if (!dpy || !canvas->comp_picture || !back_buffer) return;

    // Composite this canvas to back buffer at its position
    XRenderComposite(dpy, PictOpOver, canvas->comp_picture, None, back_buffer,
                    0, 0, 0, 0, canvas->x, canvas->y, canvas->width, canvas->height);

    canvas->comp_needs_repaint = false;
}

// Get canvas XRender picture (create if needed)
Picture itn_composite_get_canvas_picture(Canvas *canvas) {
    if (!canvas || !itn_composite_is_active()) return None;

    // Create picture if needed
    if (!canvas->comp_picture && canvas->comp_pixmap) {
        Display *dpy = itn_core_get_display();
        if (dpy) {
            int win_depth = canvas->depth ? canvas->depth : itn_core_get_screen_depth();
            canvas->comp_picture = create_picture_from_pixmap(dpy, canvas->comp_pixmap, win_depth);
        }
    }

    return canvas->comp_picture;
}

// Process damage event for a window
void itn_composite_process_damage(XDamageNotifyEvent *ev) {
    if (!itn_composite_is_active()) return;

    Display *dpy = itn_core_get_display();
    if (!dpy) return;

    // Find the canvas for this damage event
    Canvas *damaged = NULL;
    int count = itn_manager_get_count();
    for (int i = 0; i < count; i++) {
        Canvas *c = itn_manager_get_canvas(i);
        if (c && c->comp_damage == ev->damage) {
            damaged = c;
            break;
        }
    }

    if (damaged) {
        // Record damage event for metrics
        extern void itn_render_record_damage_event(void);
        itn_render_record_damage_event();

        // Update damage bounds
        damaged->comp_damage_bounds.x = ev->area.x;
        damaged->comp_damage_bounds.y = ev->area.y;
        damaged->comp_damage_bounds.width = ev->area.width;
        damaged->comp_damage_bounds.height = ev->area.height;
        damaged->comp_needs_repaint = true;

        // Clear the damage (required by XDamage protocol)
        XDamageSubtract(dpy, ev->damage, None, None);

        // Accumulate damage to set damage_pending flag, then schedule frame
        DAMAGE_CANVAS(damaged);
        SCHEDULE_FRAME();
        return;
    }

    // Check if it's an override-redirect window's damage
    OverrideWin *ow = override_list;
    while (ow) {
        if (ow->damage == ev->damage) {
            // Defensive check: verify damage object is still valid
            // (protection against race condition if cleanup happened)
            if (!ow || ow->damage == None) {
                return;  // Already destroyed, ignore stale event
            }

            // Record damage event for metrics
            extern void itn_render_record_damage_event(void);
            itn_render_record_damage_event();

            // Mark as needing repaint
            ow->needs_repaint = true;

            // Clear the damage (required by XDamage protocol)
            XDamageSubtract(dpy, ev->damage, None, None);

            // Accumulate damage to set damage_pending flag, then schedule frame
            DAMAGE_REGION(ow->x, ow->y, ow->width, ow->height);
            SCHEDULE_FRAME();
            return;
        }
        ow = ow->next;
    }
}

// Handle expose events
void itn_composite_handle_expose(XExposeEvent *ev) {
    if (!itn_composite_is_active()) return;

    // Find canvas for this window
    Canvas *canvas = NULL;
    int count = itn_manager_get_count();
    for (int i = 0; i < count; i++) {
        Canvas *c = itn_manager_get_canvas(i);
        if (c && c->win == ev->window) {
            canvas = c;
            break;
        }
    }

    if (canvas) {
        canvas->comp_needs_repaint = true;
        DAMAGE_RECT(ev->x, ev->y, ev->width, ev->height);
        SCHEDULE_FRAME();
    }
}

// Check if any canvas needs compositing
bool itn_composite_needs_frame(void) {
    if (!itn_composite_is_active()) return false;

    int count = itn_manager_get_count();
    for (int i = 0; i < count; i++) {
        Canvas *c = itn_manager_get_canvas(i);
        if (c && c->comp_needs_repaint) {
            return true;
        }
    }

    return false;
}

// Reorder windows based on stacking
void itn_composite_reorder_windows(void) {
    // TODO: Implement proper stacking order tracking
    // For now, just mark all as needing repaint
    int count = itn_manager_get_count();
    for (int i = 0; i < count; i++) {
        Canvas *c = itn_manager_get_canvas(i);
        if (c) {
            c->comp_needs_repaint = true;
        }
    }

    SCHEDULE_FRAME();
}