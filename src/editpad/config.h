// EditPad Configuration Header
#ifndef EDITPAD_CONFIG_H
#define EDITPAD_CONFIG_H

// EditPad version
#define EDITPAD_VERSION "0.01"

// Buffer sizes for paths and filenames
#define PATH_SIZE 512           // Buffer size for file paths (typical paths are <300 chars)
#define NAME_SIZE 128           // Buffer size for file names (most names are <50 chars)
#define FULL_SIZE (PATH_SIZE + NAME_SIZE + 2)  // Buffer for path + "/" + filename + null

// Default log file path
#define EDITPAD_LOG_PATH "~/.config/amiwb/editpad.log"

// Default configuration directory
#define EDITPAD_CONFIG_DIR "~/.config/amiwb/editpad"

// Error logging function declaration
// Defined in editpad_main.c, used throughout EditPad
void log_error(const char *format, ...);

// Utility macros
#define max(a,b) ((a) > (b) ? (a) : (b))
#define min(a,b) ((a) < (b) ? (a) : (b))

#endif // EDITPAD_CONFIG_H