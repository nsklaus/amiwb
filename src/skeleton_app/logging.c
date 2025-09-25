/* logging.c - Logging implementation */

/*
 * What could be added later:
 * - Timestamp formatting options
 * - Log file size limits
 * - Multiple log targets
 * - Thread-safe logging
 * - Performance metrics logging
 */

#include "logging.h"
#include "config.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

static FILE *log_file = NULL;

void log_init(void) {
    /* Simple log file in current directory for now */
    log_file = fopen("skeleton.log", "w");
    if (log_file) {
        time_t now = time(NULL);
        fprintf(log_file, "=== %s Log Started: %s", APP_NAME, ctime(&now));
    }
}

void log_message(const char *format, ...) {
    if (!log_file) return;

    va_list args;
    va_start(args, format);
    vfprintf(log_file, format, args);
    va_end(args);
    fprintf(log_file, "\n");
    fflush(log_file);  /* Ensure it's written */
}