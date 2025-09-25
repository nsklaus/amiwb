/* logging.h - Logging system for skeleton app */

/*
 * What could be added later:
 * - Log levels (ERROR, WARNING, INFO, DEBUG)
 * - Log rotation
 * - Conditional compilation for debug logs
 * - Log to different outputs (file, stderr, syslog)
 * - Colored output for terminals
 */

#ifndef SKELETON_LOGGING_H
#define SKELETON_LOGGING_H

/* Initialize logging system */
void log_init(void);

/* Main logging function - works like printf */
void log_message(const char *format, ...);

#endif /* SKELETON_LOGGING_H */