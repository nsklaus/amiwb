// File: rnd_public.h
// Public API for AmiWB render system
// Used by external subsystems (main, intuition, workbench, menus, dialogs)

#ifndef RND_PUBLIC_H
#define RND_PUBLIC_H

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>
#include "../intuition/itn_public.h"
#include "../icons.h"

// ============================================================================
// Lifecycle Management
// ============================================================================

void init_render(void);               // Initialize render system, fonts, wallpapers
void cleanup_render(void);            // Free render resources safely

// ============================================================================
// Canvas Rendering
// ============================================================================

void redraw_canvas(Canvas *canvas);   // Redraw full canvas contents

// ============================================================================
// Icon Rendering
// ============================================================================

void render_icon(FileIcon *icon, Canvas *canvas); // Draw one icon

// ============================================================================
// Text Utilities
// ============================================================================

int get_text_width(const char *text); // Width in pixels of a UTF-8 string
XftFont *get_font(void);              // Access the global UI font

// ============================================================================
// Canvas Surface Management
// ============================================================================

void render_recreate_canvas_surfaces(Canvas *canvas); // Recreate pixmap and Pictures
void render_destroy_canvas_surfaces(Canvas *canvas);  // Destroy pixmap and Pictures

// ============================================================================
// Wallpaper Management
// ============================================================================

void render_load_wallpapers(void);    // Load or reload wallpapers into cache

#endif // RND_PUBLIC_H
