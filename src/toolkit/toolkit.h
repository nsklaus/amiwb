/* toolkit.h - Shared functionality for AmiWB toolkit widgets
 *
 * Provides common utilities used by all toolkit components,
 * including logging callbacks for integration with parent applications.
 */

#ifndef TOOLKIT_H
#define TOOLKIT_H

#include <stdarg.h>

// Function pointer type for logging callbacks
typedef void (*toolkit_log_func)(const char *format, ...);

// Set the logging callback function
// Parent applications should call this at startup to register their log_error function
void toolkit_set_log_callback(toolkit_log_func callback);

// Internal logging function for toolkit widgets
// Widgets should use this instead of fprintf(stderr, ...)
void toolkit_log_error(const char *format, ...);

#endif /* TOOLKIT_H */