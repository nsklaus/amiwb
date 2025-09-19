// ReqASL Configuration Header
// Constants copied from amiwb/config.h for consistency
#ifndef REQASL_CONFIG_H
#define REQASL_CONFIG_H

// ReqASL version
#define REQASL_VERSION "0.01"

// Buffer sizes - must match amiwb for consistency
#define PATH_SIZE 512           // Buffer size for file paths (typical paths are <300 chars)
#define NAME_SIZE 128           // Buffer size for file names (most names are <50 chars)
#define FULL_SIZE (PATH_SIZE + NAME_SIZE + 2)  // Buffer for path + "/" + filename + null

// Checkmark symbol (Unicode 2713: ✓)
#define CHECKMARK "\xe2\x9c\x93"  // UTF-8 encoding of ✓ (U+2713)

// Note: Colors are defined differently in reqasl.c as hex values
// ReqASL doesn't use XRenderColor like amiwb does

// For logging - ReqASL uses fprintf(stderr, ...) not log_error()
// since log_error is in amiwb's main.c

#endif // REQASL_CONFIG_H