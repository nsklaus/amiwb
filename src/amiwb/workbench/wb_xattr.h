// File: wb_xattr.h
// Extended Attributes Preservation Utility

#ifndef WB_XATTR_H
#define WB_XATTR_H

// Copy all extended attributes from src to dst
void wb_xattr_copy_all(const char *src_path, const char *dst_path);

#endif
