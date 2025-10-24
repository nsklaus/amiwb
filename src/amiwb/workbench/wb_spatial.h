// File: wb_spatial.h
// Spatial Window Geometry Management
// Saves and loads window position/size using extended attributes
//
// NOTE: Spatial mode performs best on local filesystems (ext4, xfs, btrfs).
// Network filesystems (NFS, CIFS) may exhibit 10-500ms latency during window
// operations (open/drag/resize/close). This is inherent to network filesystem
// architecture and cannot be avoided without async implementation.

#ifndef WB_SPATIAL_H
#define WB_SPATIAL_H

#include <stdbool.h>

// Load window geometry from xattr, fallback to cascade algorithm if not found
// Returns true if geometry was loaded from xattr, false if cascade was used
bool wb_spatial_load_geometry(const char *dir_path, int *x, int *y, int *width, int *height);

// Save window geometry to xattr (called on drag end, resize end, window close)
void wb_spatial_save_geometry(const char *dir_path, int x, int y, int width, int height);

#endif // WB_SPATIAL_H
