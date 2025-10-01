// File: wb_xattr.c
// Extended Attributes Preservation Utility

#include "wb_xattr.h"
#include <sys/xattr.h>
#include <stdlib.h>
#include <string.h>

// Copy all extended attributes from source to destination
void wb_xattr_copy_all(const char *src_path, const char *dst_path) {
    if (!src_path || !dst_path) return;

    // Get list of attribute names
    ssize_t buflen = listxattr(src_path, NULL, 0);
    if (buflen <= 0) return;

    char *buf = malloc(buflen);
    if (!buf) return;

    buflen = listxattr(src_path, buf, buflen);
    if (buflen <= 0) {
        free(buf);
        return;
    }

    // Copy each attribute
    char *p = buf;
    while (p < buf + buflen) {
        ssize_t vallen = getxattr(src_path, p, NULL, 0);
        if (vallen > 0) {
            char *val = malloc(vallen);
            if (val) {
                vallen = getxattr(src_path, p, val, vallen);
                if (vallen > 0) {
                    setxattr(dst_path, p, val, vallen, 0);
                }
                free(val);
            }
        }
        p += strlen(p) + 1;
    }

    free(buf);
}
