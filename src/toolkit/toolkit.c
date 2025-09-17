/* toolkit.c - Shared functionality implementation for AmiWB toolkit
 *
 * Implements common utilities including the logging callback system
 * that allows toolkit widgets to log to parent application log files.
 */

#include "toolkit.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

// Static storage for the logging callback
static toolkit_log_func log_callback = NULL;

// Set the logging callback function
void toolkit_set_log_callback(toolkit_log_func callback) {
    log_callback = callback;
}

// Internal logging function for toolkit widgets
void toolkit_log_error(const char *format, ...) {
    if (log_callback) {
        // Use the parent application's log function
        // We need to format the string first since we can't pass va_list directly
        char buffer[1024];
        va_list args;
        va_start(args, format);
        vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);

        // Call the app's log function with the formatted string
        log_callback("%s", buffer);
    } else {
        // Fallback to stderr if no callback is set (for development/debugging)
        va_list args;
        va_start(args, format);
        vfprintf(stderr, format, args);
        va_end(args);
        fprintf(stderr, "\n");
    }
}