/* Workbench header: Declares functions for icon handling, scanning, adding, refreshing, aligning. For desktop/folder operations. */

#ifndef WORKBENCH_H
#define WORKBENCH_H

#include "intuition.h"

// Function prototypes for workbench ops.
FileIcon *find_hit_icon(int mx, int my, Canvas *canvas); // Find icon at position
void scan_icons(RenderContext *ctx, const char *path, FileIcon **icons, int *num_icons, Canvas *canvas); // Scan directory for icons
void add_icon(RenderContext *ctx, const char *dir_path, const char *name, int type, FileIcon **icons, int *num_icons, const char *custom_icon_path, Canvas *canvas); // Add single icon
bool rect_intersect(XRectangle *a, XRectangle *b); // Check rect intersection
void refresh_icons(RenderContext *ctx, Canvas *canvas); // Refresh canvas icons
void align_icons(Canvas *canvas); // Align icons in grid

#endif