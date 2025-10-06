/* toolkit_config.h - Configuration for AmiWB toolkit widgets
 *
 * This file contains only the configuration values needed by toolkit widgets.
 * It's separate from amiwb/config.h to keep the toolkit independent and reusable.
 */

#ifndef TOOLKIT_CONFIG_H
#define TOOLKIT_CONFIG_H

#include <X11/extensions/Xrender.h>

/* Buffer sizes */
#define NAME_SIZE 128           /* Buffer size for file names */
#define PATH_SIZE 512           /* Buffer size for file paths */

/* Global colors - Amiga style */
#define BLACK (XRenderColor){0x0000, 0x0000, 0x0000, 0xFFFF}
#define WHITE (XRenderColor){0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}
#define BLUE  (XRenderColor){0x4858, 0x6F6F, 0xB0B0, 0xFFFF}
#define GRAY  (XRenderColor){0xa0a0, 0xa2a2, 0xa0a0, 0xffff}

#endif /* TOOLKIT_CONFIG_H */
