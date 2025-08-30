#include "compositor.h"
#include "config.h"
#include <stdio.h>
#include <stdbool.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>
#include <X11/Xutil.h>
#include <stdlib.h>
#include <string.h>
#include "intuition.h"

typedef struct CompWin {
    Window win;
    Pixmap pm;
    Picture pict;
    Damage damage;
    int depth;
    int width;
    int height;
    int x;      // Cached position to avoid XTranslateCoordinates
    int y;      // Cached position to avoid XTranslateCoordinates
    struct CompWin *next;
} CompWin;

static bool g_active = false;
static Atom g_sel = None;
static Window g_owner = None;
static Window g_root = None;
static Window g_overlay = None;
static Picture g_overlay_pict = 0;
static Picture g_root_pict = 0;
static Picture g_wall_pict = 0;
// Screen-sized double buffer
static Pixmap  g_back_pm = 0;
static Picture g_back_pict = 0;
static CompWin *g_list = NULL;
static int g_damage_event_base = 0, g_damage_error_base = 0;
static int g_composite_event_base = 0, g_composite_error_base = 0;

// Motion event batching to reduce repaints
static bool g_pending_repaint = false;

// Helper function to safely free X11 resources with sync
static void safe_sync_and_free_picture(Display *dpy, Picture *pict) {
    if (pict && *pict) {
        XSync(dpy, False);
        XRenderFreePicture(dpy, *pict);
        *pict = 0;
    }
}

static void safe_sync_and_free_pixmap(Display *dpy, Pixmap *pm) {
    if (pm && *pm) {
        XSync(dpy, False);
        XFreePixmap(dpy, *pm);
        *pm = 0;
    }
}

// Forward declaration
static XRenderPictFormat *fmt_for_depth(Display *dpy, int depth);

// Helper function to create picture from pixmap with standard format
static Picture create_picture_from_pixmap(Display *dpy, Pixmap pm, int depth) {
    XRenderPictFormat *fmt = fmt_for_depth(dpy, depth);
    if (!fmt) return None;
    
    XRenderPictureAttributes pa;
    memset(&pa, 0, sizeof(pa));
    Picture pict = XRenderCreatePicture(dpy, pm, fmt, 0, &pa);
    
    // Include child windows when sampling this picture
    XRenderPictureAttributes change;
    memset(&change, 0, sizeof(change));
    change.subwindow_mode = IncludeInferiors;
    XRenderChangePicture(dpy, pict, CPSubwindowMode, &change);
    
    return pict;
}

// Helper function to get screen dimensions
static void get_screen_dimensions(Display *dpy, Window win, unsigned int *width, unsigned int *height) {
    XWindowAttributes wa;
    XGetWindowAttributes(dpy, win, &wa);
    *width = (unsigned int)wa.width;
    *height = (unsigned int)wa.height;
}

// Helper function to composite a source picture to destination
static void composite_picture_full_screen(Display *dpy, Picture src, Picture dest, unsigned int width, unsigned int height) {
    XRenderComposite(dpy, PictOpSrc, src, None, dest,
                     0, 0, 0, 0,
                     0, 0,
                     width, height);
}

static void free_win(Display *dpy, CompWin *cw) {
    if (!cw) return;
    safe_sync_and_free_picture(dpy, &cw->pict);
    safe_sync_and_free_pixmap(dpy, &cw->pm);
    if (cw->damage) {
        XDamageDestroy(dpy, cw->damage);
        cw->damage = 0;
    }
}

// Forward decl for internal debug dumper
static void dump_compositor_order(Display *dpy, const char *tag);

// Public debug entry point (declared in compositor.h)
void compositor_dump_order(const char *tag) {
    Display *dpy = get_display();
    if (!dpy) return;
    dump_compositor_order(dpy, tag);
}

// Debug: dump compositor list order (bottom->top) and XQueryTree order (top->bottom)
static void dump_compositor_order(Display *dpy, const char *tag) {
    // Stack order update - debug output removed
    // Compositor paint order: iterate g_list (built bottom->top)
    for (CompWin *it = g_list; it; it = it->next) {
        // Window in stack - debug output removed
    }
    // X server stacking: children are bottom->top
    Window root_ret, parent_ret, *children = NULL; unsigned int n = 0;
    if (XQueryTree(dpy, g_root, &root_ret, &parent_ret, &children, &n)) {
        // X11 stack dump - debug output removed
        if (children) XFree(children);
    }
}

// Functions preserved for future compositor enhancements
#if 0
// Reorder g_list to match current stacking from XQueryTree without recreating pixmaps
static void reorder_win_list(Display *dpy) {
    if (!g_list) return;
    Window root, parent, *children = NULL;
    unsigned int n = 0;
    if (!XQueryTree(dpy, g_root, &root, &parent, &children, &n)) return;
    CompWin *new_head = NULL; CompWin **tail = &new_head;
    for (unsigned int i = 0; i < n; ++i) {
        Window w = children[i];
        if (w == g_overlay || w == g_owner) continue;
        // Find existing node
        for (CompWin *it = g_list; it; it = it->next) {
            if (it->win == w) {
                // Append it to new list
                it->next = NULL; // sever old linkage to avoid corrupt chains
                *tail = it; tail = &it->next;
                break;
            }
        }
    }
    if (children) XFree(children);
    if (tail) *tail = NULL;
    if (new_head) g_list = new_head;
}
#endif

static void repaint(Display *dpy);
static void build_win_list(Display *dpy);
static void dump_compositor_order(Display *dpy, const char *tag);

#if 0
static void enforce_x_layering(Display *dpy) {
    // No-op: compositor must not alter stacking. Intuition owns stacking.
    (void)dpy;
}
#endif

void compositor_repaint(Display *dpy) {
    if (!g_active) return;
    repaint(dpy);
}

void compositor_flush_pending(Display *dpy) {
    if (!g_active) return;
    if (g_pending_repaint) {
        repaint(dpy);
        g_pending_repaint = false;
    }
}

void compositor_sync_stacking(Display *dpy) {
    if (!g_active) return;
    // Do not restack here; only reflect current X server order and repaint
    (void)dpy; // dpy kept for symmetry; no restacking performed
    build_win_list(dpy);
    repaint(dpy);
}

static XRenderPictFormat *fmt_for_depth(Display *dpy, int depth) {
    if (depth == 32) return XRenderFindStandardFormat(dpy, PictStandardARGB32);
    return XRenderFindStandardFormat(dpy, PictStandardRGB24);
}

static void clear_list(Display *dpy) {
    CompWin *it = g_list;
    while (it) {
        CompWin *n = it->next;
        free_win(dpy, it);
        free(it);
        it = n;
    }
    g_list = NULL;
}

static void build_win_list(Display *dpy) {
    clear_list(dpy);
    Window root, parent, *children = NULL;
    unsigned int n = 0;
    if (!XQueryTree(dpy, g_root, &root, &parent, &children, &n)) return;
    // Children are bottom-to-top; composite in that order, so append each to tail
    CompWin **tail = &g_list;
    unsigned int added = 0;
    for (unsigned int i = 0; i < n; ++i) {
        Window w = children[i];
        if (w == g_overlay || w == g_owner) continue; // skip overlay and our hidden owner
        XWindowAttributes wa;
        if (!XGetWindowAttributes(dpy, w, &wa)) continue;
        if (wa.map_state != IsViewable) continue;
        // Redirect named pixmap
        // XCompositeNameWindowPixmap: Gets the offscreen pixmap for a window
        // When a window is redirected, X renders it to this invisible pixmap
        // We can then read this pixmap to composite the window however we want
        Pixmap pm = XCompositeNameWindowPixmap(dpy, w);
        if (!pm) continue;
        // Pick format by depth
        XWindowAttributes dwa; XGetWindowAttributes(dpy, w, &dwa);
        int depth = dwa.depth;
        Picture pict = create_picture_from_pixmap(dpy, pm, depth);
        if (!pict) { XFreePixmap(dpy, pm); continue; }
        // XDamageCreate: Start monitoring this window for changes
        // XDamageReportNonEmpty means "notify us whenever damage happens"
        // When the window redraws, we get an event so we know to recomposite
        Damage dmg = XDamageCreate(dpy, w, XDamageReportNonEmpty);

        // Get window position once during creation
        int x = 0, y = 0;
        Window dummy;
        XTranslateCoordinates(dpy, w, g_root, 0, 0, &x, &y, &dummy);
        
        CompWin *cw = (CompWin*)calloc(1, sizeof(CompWin));
        cw->win = w; cw->pm = pm; cw->pict = pict; cw->damage = dmg; cw->depth = depth;
        cw->width = wa.width; cw->height = wa.height;
        cw->x = x; cw->y = y;  // Cache position
        cw->next = NULL; *tail = cw; tail = &cw->next; // append to preserve order
        added++;
    }
    if (children) XFree(children);
    // debug spam removed
}

static void repaint(Display *dpy) {
    if (!g_overlay_pict) return;
    // Ensure back buffer exists and matches screen size
    unsigned int sw, sh;
    get_screen_dimensions(dpy, g_overlay, &sw, &sh);
    if (!g_back_pm) {
        g_back_pm = XCreatePixmap(dpy, g_root, sw, sh, 32);
        XRenderPictFormat *bf = XRenderFindStandardFormat(dpy, PictStandardARGB32);
        if (bf) g_back_pict = XRenderCreatePicture(dpy, g_back_pm, bf, 0, NULL);
    }
    // If size mismatch, recreate back buffer
    if (g_back_pm) {
        XWindowAttributes rwa; XGetWindowAttributes(dpy, g_root, &rwa);
        // naive check: if any dimension changed vs overlay attrs, recreate
        // (Pixmap has no direct query API here; use overlay size as truth)
        static unsigned int last_w = 0, last_h = 0;
        if (last_w != sw || last_h != sh) {
            if (g_back_pict) { XRenderFreePicture(dpy, g_back_pict); g_back_pict = 0; }
            if (g_back_pm) { XFreePixmap(dpy, g_back_pm); g_back_pm = 0; }
            g_back_pm = XCreatePixmap(dpy, g_root, sw, sh, 32);
            XRenderPictFormat *bf = XRenderFindStandardFormat(dpy, PictStandardARGB32);
            if (bf) g_back_pict = XRenderCreatePicture(dpy, g_back_pm, bf, 0, NULL);
            last_w = sw; last_h = sh;
        }
    }
    if (!g_back_pict) return;
    // Clear back buffer to transparent
    XRenderColor clear = {0,0,0,0};
    XRenderFillRectangle(dpy, PictOpSrc, g_back_pict, &clear, 0, 0, sw, sh);

    // Try to use configured wallpaper pixmap from render context
    if (!g_wall_pict) {
        RenderContext *ctx = get_render_context();
        if (ctx && ctx->desk_img != None) {
            // Create a Picture from the wallpaper pixmap using root visual format
            XWindowAttributes rwa; XGetWindowAttributes(dpy, g_root, &rwa);
            XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy, rwa.visual);
            if (!fmt) fmt = XRenderFindStandardFormat(dpy, PictStandardRGB24);
            if (fmt) {
                g_wall_pict = XRenderCreatePicture(dpy, ctx->desk_img, fmt, 0, NULL);
            }
        }
    }
    if (g_wall_pict) {
        unsigned int rw, rh;
        get_screen_dimensions(dpy, g_root, &rw, &rh);
        composite_picture_full_screen(dpy, g_wall_pict, g_back_pict, rw, rh);
    } else if (g_root_pict) {
        // Fallback: copy whatever the root currently has
        unsigned int rw, rh;
        get_screen_dimensions(dpy, g_root, &rw, &rh);
        composite_picture_full_screen(dpy, g_root_pict, g_back_pict, rw, rh);
    }

    // No compositor order dump here; prints only around press/release

    // Composite in logical layers respecting X stacking for WINDOWs:
    // 1) Desktop canvases
    // 2) Normal windows (and unknown) in X stacking order
    // 3) Menus/menubar canvases
    unsigned int count = 0;
    // Pass 1: desktop - use cached position/size
    for (CompWin *it = g_list; it; it = it->next) {
        Canvas *c = find_canvas(it->win);
        if (!(c && c->type == DESKTOP)) continue;
        // Use cached values - no X11 calls!
        unsigned int ww = (unsigned int)it->width, wh = (unsigned int)it->height;
        int op = (it->depth == 32) ? PictOpOver : PictOpSrc;
        XRenderComposite(dpy, op, it->pict, None, g_back_pict, 0,0,0,0, it->x, it->y, ww,wh);
        count++;
    }
    // Pass 2: normal windows or unknowns - use cached position/size  
    for (CompWin *it = g_list; it; it = it->next) {
        Canvas *c = find_canvas(it->win);
        if (c && (c->type == DESKTOP || c->type == MENU)) continue;
        // Use cached values - no X11 calls!
        unsigned int ww = (unsigned int)it->width, wh = (unsigned int)it->height;
        int op = (it->depth == 32) ? PictOpOver : PictOpSrc;
        XRenderComposite(dpy, op, it->pict, None, g_back_pict, 0,0,0,0, it->x, it->y, ww,wh);
        count++;
    }
    // Pass 3: menus/menubar - use cached position/size
    for (CompWin *it = g_list; it; it = it->next) {
        Canvas *c = find_canvas(it->win);
        if (!(c && c->type == MENU)) continue;
        // Use cached values - no X11 calls!
        unsigned int ww = (unsigned int)it->width, wh = (unsigned int)it->height;
        int op = (it->depth == 32) ? PictOpOver : PictOpSrc;
        XRenderComposite(dpy, op, it->pict, None, g_back_pict, 0,0,0,0, it->x, it->y, ww,wh);
        count++;
    }
    // Single blit from back buffer to overlay
    composite_picture_full_screen(dpy, g_back_pict, g_overlay_pict, sw, sh);
    // fprintf(stderr, "[comp] repaint windows=%u\n", count);
    XFlush(dpy);
}

bool init_compositor(Display *dpy) {
    if (!dpy) return false;
    int screen = DefaultScreen(dpy);
    g_root = RootWindow(dpy, screen);

    // Ensure required extensions exist
    // XComposite: Allows windows to be rendered offscreen before display
    // This extension is essential for compositing window managers
    // Without it, we can't do transparency, shadows, or smooth effects
    if (!XCompositeQueryExtension(dpy, &g_composite_event_base, &g_composite_error_base)) {
        log_error("[ERROR] Compositor: XComposite extension missing");
        return false;
    }
    int major=0, minor=0;
    XCompositeQueryVersion(dpy, &major, &minor);
    // XDamage: Notifies us when window contents change
    // Instead of constantly redrawing everything, we only update damaged areas
    // This makes compositing efficient - we redraw only what changed
    if (!XDamageQueryExtension(dpy, &g_damage_event_base, &g_damage_error_base)) {
        log_error("[ERROR] Compositor: XDamage extension missing");
        return false;
    }

    // Try to acquire selection _NET_WM_CM_S{screen}, but do not abort if busy
    char selname[32];
    snprintf(selname, sizeof(selname), "_NET_WM_CM_S%d", screen);
    g_sel = XInternAtom(dpy, selname, False);
    Window current_owner = XGetSelectionOwner(dpy, g_sel);
    if (current_owner != None) {
        // Could not acquire selection - not critical
    } else {
        XSetWindowAttributes swa; memset(&swa, 0, sizeof(swa));
        swa.override_redirect = True;
        g_owner = XCreateWindow(dpy, g_root, -1, -1, 1, 1, 0, CopyFromParent, InputOutput,
                                CopyFromParent, CWOverrideRedirect, &swa);
        XSetSelectionOwner(dpy, g_sel, g_owner, CurrentTime);
    }

    // Redirect all subwindows to offscreen named pixmaps and listen for topology changes
    // XCompositeRedirectSubwindows: The magic that enables compositing!
    // CompositeRedirectManual means WE control how windows are drawn
    // Windows render to invisible pixmaps, then we composite them together
    XCompositeRedirectSubwindows(dpy, g_root, CompositeRedirectManual);
    XSelectInput(dpy, g_root, SubstructureNotifyMask | StructureNotifyMask | PropertyChangeMask);

    // Get overlay window and create a Picture for it (ARGB)
    // XCompositeGetOverlayWindow: Gets a special transparent window
    // This overlay sits above all other windows but is click-through
    // We draw our composited result here, creating the final desktop image
    g_overlay = XCompositeGetOverlayWindow(dpy, g_root);
    if (g_overlay == None) {
        log_error("[ERROR] Compositor: overlay window not available");
        shutdown_compositor(dpy);
        return false;
    }
    // Make overlay input-transparent so events pass through to underlying windows
    // XFixes extension provides region operations for complex shapes
    // We create an empty region (no pixels) and set it as the input shape
    // Result: overlay is visible but mouse clicks go through to windows below
    XserverRegion empty = XFixesCreateRegion(dpy, NULL, 0);
    XFixesSetWindowShapeRegion(dpy, g_overlay, ShapeInput, 0, 0, empty);
    XFixesDestroyRegion(dpy, empty);
    XWindowAttributes owa; XGetWindowAttributes(dpy, g_overlay, &owa);
    XRenderPictFormat *ofmt = XRenderFindVisualFormat(dpy, owa.visual);
    if (!ofmt) ofmt = XRenderFindStandardFormat(dpy, PictStandardARGB32);
    if (!ofmt) {
        log_error("[ERROR] Compositor: no overlay pict format");
        shutdown_compositor(dpy);
        return false;
    }
    XRenderPictureAttributes opa; memset(&opa, 0, sizeof(opa));
    g_overlay_pict = XRenderCreatePicture(dpy, g_overlay, ofmt, 0, &opa);
    if (!g_overlay_pict) {
        log_error("[ERROR] Compositor: failed to create overlay picture");
        shutdown_compositor(dpy);
        return false;
    }

    // Create a root picture to sample background if any
    XWindowAttributes rwa; XGetWindowAttributes(dpy, g_root, &rwa);
    XRenderPictFormat *rfmt = XRenderFindVisualFormat(dpy, rwa.visual);
    if (!rfmt) rfmt = XRenderFindStandardFormat(dpy, PictStandardRGB24);
    if (rfmt) {
        XRenderPictureAttributes rpa; memset(&rpa, 0, sizeof(rpa));
        g_root_pict = XRenderCreatePicture(dpy, g_root, rfmt, 0, &rpa);
    }

    // Build initial window list and paint once
    build_win_list(dpy);
    g_active = true;
    compositor_repaint(dpy);
    // Compositor active - no need to log success
    return true;
}

void shutdown_compositor(Display *dpy) {
    if (!g_active) return;
    clear_list(dpy);
    if (g_wall_pict) { XRenderFreePicture(dpy, g_wall_pict); g_wall_pict = 0; }
    if (g_root_pict) { XRenderFreePicture(dpy, g_root_pict); g_root_pict = 0; }
    if (g_back_pict) { XRenderFreePicture(dpy, g_back_pict); g_back_pict = 0; }
    if (g_back_pm) { XFreePixmap(dpy, g_back_pm); g_back_pm = 0; }
    if (g_overlay_pict) { XRenderFreePicture(dpy, g_overlay_pict); g_overlay_pict = 0; }
    if (g_overlay) { XCompositeReleaseOverlayWindow(dpy, g_root); g_overlay = None; }
    if (g_owner) {
        XWindowAttributes wa;
        if (XGetWindowAttributes(dpy, g_owner, &wa)) {
            XDestroyWindow(dpy, g_owner);
        }
        g_owner = None;
    }
    g_sel = None; g_root = None;
    g_active = false;
}

void compositor_handle_event(Display *dpy, XEvent *ev) {
    if (!g_active || !ev) return;
    if (!g_overlay_pict) return; // not redirecting, nothing to do
    int type = ev->type;
    if (type == MapNotify || type == UnmapNotify || type == DestroyNotify || type == CreateNotify || type == ReparentNotify) {
        build_win_list(dpy);
        repaint(dpy);
        return;
    }
    if (type == ConfigureNotify) {
        XConfigureEvent *cev = &ev->xconfigure;
        bool size_changed = false;
        bool position_changed = false;
        // Find window
        for (CompWin *it = g_list; it; it = it->next) {
            if (it->win == cev->window) {
                // Get absolute position relative to root
                int x = 0, y = 0;
                Window dummy;
                XTranslateCoordinates(dpy, it->win, g_root, 0, 0, &x, &y, &dummy);
                
                // Update cached position
                if (it->x != x || it->y != y) {
                    position_changed = true;
                    it->x = x;
                    it->y = y;
                }
                // If size changed, recreate named pixmap and picture
                if (it->width != cev->width || it->height != cev->height) {
                    size_changed = true;
                    if (it->pict) { XRenderFreePicture(dpy, it->pict); it->pict = 0; }
                    if (it->pm) { XFreePixmap(dpy, it->pm); it->pm = 0; }
                    it->pm = XCompositeNameWindowPixmap(dpy, it->win);
                    if (it->pm) {
                        XRenderPictFormat *fmt = fmt_for_depth(dpy, it->depth);
                        if (fmt) {
                            it->pict = XRenderCreatePicture(dpy, it->pm, fmt, 0, NULL);
                            XRenderPictureAttributes change; memset(&change, 0, sizeof(change));
                            change.subwindow_mode = IncludeInferiors;
                            XRenderChangePicture(dpy, it->pict, CPSubwindowMode, &change);
                        }
                    }
                    // Update cached size
                    it->width = cev->width; it->height = cev->height;
                }
                break;
            }
        }
        // CRITICAL: Only repaint immediately if size changed
        // For position changes, mark pending to batch them
        if (size_changed) {
            repaint(dpy);
            g_pending_repaint = false; // Clear pending since we just repainted
        } else if (position_changed) {
            // Mark that we need a repaint, but don't do it yet
            // This batches multiple rapid position changes
            g_pending_repaint = true;
        }
        return;
    }
    if (type == g_damage_event_base + XDamageNotify) {
        XDamageNotifyEvent *de = (XDamageNotifyEvent*)ev;
        for (CompWin *it = g_list; it; it = it->next) {
            if (it->damage == de->damage && it->damage != 0) {
                // Only validate window exists before damage subtract
                XWindowAttributes attrs;
                if (XGetWindowAttributes(dpy, it->win, &attrs) == True) {
                    XDamageSubtract(dpy, it->damage, None, None);
                }
                break;
            }
        }
        repaint(dpy);
    }
}
