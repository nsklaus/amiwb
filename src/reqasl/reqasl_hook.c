/*
 * ReqASL Universal File Dialog Hook
 * Intercepts file dialogs from GTK2/3/4, Qt, and other toolkits
 * Replaces them with ReqASL file requester
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/wait.h>
#include <time.h>

// ============================================================================
// Logging System
// ============================================================================

static void log_error(const char *format, ...) {
    const char *log_path = "/home/klaus/Sources/amiwb/reqasl_hook.log";

    FILE *log = fopen(log_path, "a");
    if (!log) return;

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    fprintf(log, "[%02d:%02d:%02d] ",
            tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);

    va_list args;
    va_start(args, format);
    vfprintf(log, format, args);
    va_end(args);
    fprintf(log, "\n");

    fclose(log);
}

// ============================================================================
// GTK Constants (GTK2/GTK3 Compatible)
// ============================================================================

// File chooser actions
#define GTK_FILE_CHOOSER_ACTION_OPEN           0
#define GTK_FILE_CHOOSER_ACTION_SAVE           1
#define GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER  2
#define GTK_FILE_CHOOSER_ACTION_CREATE_FOLDER  3

// Dialog response codes
#define GTK_RESPONSE_CANCEL -6
#define GTK_RESPONSE_ACCEPT -3
#define GTK_RESPONSE_OK     -5

// ============================================================================
// Helper Macros
// ============================================================================

// Load original GTK function pointer using dlsym (eliminates duplication)
// Usage: LOAD_ORIGINAL(gtk_dialog_run);
#define LOAD_ORIGINAL(func_name) \
    do { \
        if (!original_##func_name) { \
            original_##func_name = dlsym(RTLD_NEXT, #func_name); \
        } \
    } while(0)

// ============================================================================
// Dialog State
// ============================================================================

// Store dialog state
typedef struct {
    void *dialog;
    int action;
    char *title;
    char *filename;       // Selected file from ReqASL
    char *initial_folder; // Initial directory for dialog
    char *initial_name;   // Initial filename for Save As
    int response;
    int needs_reqasl;     // Flag: get_files() should launch ReqASL
    int created_by_hook;  // Flag: 1=we created dialog, 0=app created it
} DialogState;

static DialogState current_dialog = {0};

// Function pointers to original GTK functions
static void* (*original_gtk_file_chooser_dialog_new)(const char *title, 
                                                      void *parent,
                                                      int action,
                                                      const char *first_button_text,
                                                      ...) = NULL;

static int (*original_gtk_dialog_run)(void *dialog) = NULL;
static char* (*original_gtk_file_chooser_get_filename)(void *chooser) = NULL;
static void* (*original_gtk_file_chooser_get_filenames)(void *chooser) = NULL;
static char* (*original_gtk_file_chooser_get_uri)(void *chooser) = NULL;
static void* (*original_gtk_file_chooser_get_uris)(void *chooser) = NULL;
static void* (*original_gtk_file_chooser_get_file)(void *chooser) = NULL;
static void* (*original_gtk_file_chooser_get_files)(void *chooser) = NULL;
static void (*original_gtk_file_chooser_set_current_folder)(void *chooser, const char *folder) = NULL;
static void (*original_gtk_file_chooser_set_current_name)(void *chooser, const char *name) = NULL;
static void (*original_gtk_file_chooser_set_action)(void *chooser, int action) = NULL;
static int (*original_gtk_file_chooser_get_action)(void *chooser) = NULL;
static void (*original_gtk_widget_destroy)(void *widget) = NULL;
static void (*original_gtk_widget_show)(void *widget) = NULL;
static void (*original_gtk_widget_show_all)(void *widget) = NULL;
static void (*original_gtk_window_present)(void *window) = NULL;
static void (*original_gtk_widget_map)(void *widget) = NULL;

// GIO (GFile) functions
static void* (*original_g_file_new_for_path)(const char *path) = NULL;

// GLib (GSList) functions
static void* (*original_g_slist_prepend)(void *list, void *data) = NULL;

// GLib (signal) functions
static unsigned long (*original_g_signal_connect_data)(void *instance, const char *signal, void *callback, void *data, void *destroy_data, int connect_flags) = NULL;

// Captured callback for "response" signal
typedef void (*ResponseCallback)(void *dialog, int response_id, void *user_data);
static ResponseCallback captured_callback = NULL;
static void *captured_user_data = NULL;

// GTK3 Native Dialog API (GTK 3.20+)
static void* (*original_gtk_file_chooser_native_new)(const char *title,
                                                       void *parent,
                                                       int action,
                                                       const char *accept_label,
                                                       const char *cancel_label) = NULL;
static int (*original_gtk_native_dialog_run)(void *dialog) = NULL;
static void (*original_gtk_native_dialog_show)(void *dialog) = NULL;

// Forward declarations
static char* launch_reqasl(int action, const char *title, const char *initial_folder, const char *initial_name);
static int is_leafpad(void);

// Launch ReqASL and get result
static char* launch_reqasl(int action, const char *title, const char *initial_folder, const char *initial_name) {
    (void)initial_name; // TODO: Pass to ReqASL when --filename support is added

    // Build command
    char command[1024];
    const char *mode;

    switch (action) {
        case GTK_FILE_CHOOSER_ACTION_OPEN:
            mode = "open";
            break;
        case GTK_FILE_CHOOSER_ACTION_SAVE:
            mode = "save";
            break;
        case GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER:
            mode = "directory";
            break;
        default:
            mode = "open";
    }

    // Determine initial path (prefer explicit folder, fall back to home)
    const char *path = initial_folder;
    if (!path || strlen(path) == 0) {
        path = getenv("HOME");
        if (!path) {
            path = "/home";
        }
    }

    // Build title if not provided
    const char *window_title = title;
    if (!window_title || strlen(window_title) == 0) {
        switch (action) {
            case GTK_FILE_CHOOSER_ACTION_OPEN:
                window_title = "Open File";
                break;
            case GTK_FILE_CHOOSER_ACTION_SAVE:
                window_title = "Save File";
                break;
            case GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER:
                window_title = "Select Folder";
                break;
            default:
                window_title = "File Selection";
        }
    }

    // Full path to reqasl executable
    // IMPORTANT: Clear LD_PRELOAD so reqasl doesn't load this hook
    snprintf(command, sizeof(command), "LD_PRELOAD='' /usr/local/bin/reqasl --mode %s --path '%s' --title '%s'",
             mode, path, window_title);

    // TODO: Add --filename support to ReqASL for initial_name
    // For now, initial_name is stored but not passed (ReqASL doesn't support it yet)
    
    // Execute and read result
    FILE *fp = popen(command, "r");
    if (!fp) {
        log_error("[ERROR] Failed to launch ReqASL");
        return NULL;
    }
    
    // Read the selected file path
    static char result[512];
    memset(result, 0, sizeof(result));
    
    if (fgets(result, sizeof(result), fp) != NULL) {
        // Remove trailing newline
        size_t len = strlen(result);
        if (len > 0 && result[len-1] == '\n') {
            result[len-1] = '\0';
        }
        
        // Check for cancellation
        if (strncmp(result, "CANCEL", 6) == 0 || strlen(result) == 0) {
            pclose(fp);
            return NULL;
        }

        pclose(fp);
        return strdup(result);
    }

    pclose(fp);
    return NULL;
}

// Check if current process is leafpad
static int is_leafpad(void) {
    static int cached_result = -1;  // -1=not checked, 0=no, 1=yes

    if (cached_result != -1) {
        return cached_result;
    }

    // Read process name from /proc/self/exe
    char exe_path[256];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);

    if (len != -1) {
        exe_path[len] = '\0';

        // Check if path contains "leafpad"
        if (strstr(exe_path, "leafpad") != NULL) {
            cached_result = 1;
            return 1;
        }
    }

    cached_result = 0;
    return 0;
}

// Hook: gtk_file_chooser_dialog_new
void* gtk_file_chooser_dialog_new(const char *title,
                                  void *parent,
                                  int action,
                                  const char *first_button_text,
                                  ...) {
    (void)first_button_text; // We build our own button list

    // Store dialog info
    current_dialog.action = action;
    if (current_dialog.title) {
        free(current_dialog.title);
    }
    current_dialog.title = title ? strdup(title) : NULL;
    
    // Clear previous filename
    if (current_dialog.filename) {
        free(current_dialog.filename);
        current_dialog.filename = NULL;
    }
    
    // Load original function
    LOAD_ORIGINAL(gtk_file_chooser_dialog_new);
    if (!original_gtk_file_chooser_dialog_new) {
        log_error("[ERROR] Could not find original gtk_file_chooser_dialog_new");
        return NULL;
    }
    
    // Create the dialog properly with all varargs
    // This is important - the dialog needs proper buttons to work
    va_list args;
    va_start(args, first_button_text);
    
    // We need to pass through ALL the arguments properly
    // For GTK file chooser, typical args are:
    // "gtk-cancel", GTK_RESPONSE_CANCEL, "gtk-open", GTK_RESPONSE_ACCEPT, NULL
    void *dialog = NULL;

    if (action == GTK_FILE_CHOOSER_ACTION_OPEN) {
        // leafpad expects OK (-5), others expect ACCEPT (-3)
        int open_response = is_leafpad() ? GTK_RESPONSE_OK : GTK_RESPONSE_ACCEPT;
        dialog = original_gtk_file_chooser_dialog_new(title, parent, action,
                                                      "_Cancel", GTK_RESPONSE_CANCEL,
                                                      "_Open", open_response,
                                                      NULL);
    } else if (action == GTK_FILE_CHOOSER_ACTION_SAVE) {
        dialog = original_gtk_file_chooser_dialog_new(title, parent, action,
                                                      "_Cancel", GTK_RESPONSE_CANCEL,
                                                      "_Save", GTK_RESPONSE_ACCEPT,
                                                      NULL);
    } else {
        dialog = original_gtk_file_chooser_dialog_new(title, parent, action,
                                                      "_Cancel", GTK_RESPONSE_CANCEL,
                                                      "_OK", GTK_RESPONSE_ACCEPT,
                                                      NULL);
    }
    
    va_end(args);

    current_dialog.dialog = dialog;
    current_dialog.created_by_hook = 1;  // We created this dialog
    return dialog;
}

// Hook: gtk_dialog_run
int gtk_dialog_run(void *dialog) {

    // Check if this is our file chooser dialog
    if (dialog == current_dialog.dialog) {

        // Launch ReqASL with stored initial folder and name
        char *selected_file = launch_reqasl(current_dialog.action, current_dialog.title,
                                            current_dialog.initial_folder, current_dialog.initial_name);

        if (selected_file) {
            // Store result - app will call get_filename() to retrieve it
            current_dialog.filename = selected_file;

            // Return response code that matches button mapping
            // leafpad expects OK (-5) for OPEN, others expect ACCEPT (-3)
            if (current_dialog.action == GTK_FILE_CHOOSER_ACTION_OPEN && is_leafpad()) {
                current_dialog.response = GTK_RESPONSE_OK;
            } else {
                current_dialog.response = GTK_RESPONSE_ACCEPT;
            }

            log_error("[DEBUG] gtk_dialog_run: dialog=%p, stored filename='%s', returning response=%d",
                     dialog, selected_file, current_dialog.response);
            return current_dialog.response;
        } else {
            // User cancelled
            current_dialog.filename = NULL;
            current_dialog.response = GTK_RESPONSE_CANCEL;
            log_error("[DEBUG] gtk_dialog_run: dialog=%p, user cancelled, returning CANCEL", dialog);
            return GTK_RESPONSE_CANCEL;
        }
    }

    // Not our dialog, call original
    LOAD_ORIGINAL(gtk_dialog_run);

    if (original_gtk_dialog_run) {
        return original_gtk_dialog_run(dialog);
    }
    
    return GTK_RESPONSE_CANCEL;
}

// Hook: gtk_file_chooser_get_filename
char* gtk_file_chooser_get_filename(void *chooser) {

    log_error("[DEBUG] gtk_file_chooser_get_filename: chooser=%p, dialog=%p, filename='%s', response=%d",
             chooser, current_dialog.dialog, current_dialog.filename ? current_dialog.filename : "(null)",
             current_dialog.response);

    // If we have a valid filename from ReqASL, return it
    // NOTE: Not checking pointer equality because GObject interface casting
    // means the same object has different pointers for different interfaces
    if (current_dialog.filename &&
        (current_dialog.response == GTK_RESPONSE_OK ||
         current_dialog.response == GTK_RESPONSE_ACCEPT)) {
        log_error("[DEBUG] gtk_file_chooser_get_filename: RETURNING our filename='%s'", current_dialog.filename);
        return strdup(current_dialog.filename);
    }

    // Otherwise call the original
    LOAD_ORIGINAL(gtk_file_chooser_get_filename);

    if (original_gtk_file_chooser_get_filename) {
        log_error("[DEBUG] gtk_file_chooser_get_filename: calling original GTK function");
        return original_gtk_file_chooser_get_filename(chooser);
    }

    log_error("[DEBUG] gtk_file_chooser_get_filename: returning NULL");
    return NULL;
}

// Hook: gtk_file_chooser_get_filenames (returns GSList* of strings)
void* gtk_file_chooser_get_filenames(void *chooser) {

    log_error("[DEBUG] gtk_file_chooser_get_filenames: chooser=%p, dialog=%p, filename='%s', response=%d",
             chooser, current_dialog.dialog, current_dialog.filename ? current_dialog.filename : "(null)",
             current_dialog.response);

    // If we have a valid filename from ReqASL, return it as list
    // NOTE: Not checking pointer equality due to GObject interface casting
    if (current_dialog.filename &&
        (current_dialog.response == GTK_RESPONSE_OK ||
         current_dialog.response == GTK_RESPONSE_ACCEPT)) {

        // Load GLib list function
        LOAD_ORIGINAL(g_slist_prepend);

        if (original_g_slist_prepend) {
            // Create a GSList with one element (our filename)
            void *list = NULL;
            list = original_g_slist_prepend(list, strdup(current_dialog.filename));
            log_error("[DEBUG] gtk_file_chooser_get_filenames: RETURNING list with our filename");
            return list;
        } else {
            log_error("[ERROR] Could not find g_slist_prepend");
            return NULL;
        }
    }

    // Otherwise call the original
    LOAD_ORIGINAL(gtk_file_chooser_get_filenames);

    if (original_gtk_file_chooser_get_filenames) {
        log_error("[DEBUG] gtk_file_chooser_get_filenames: calling original GTK function");
        return original_gtk_file_chooser_get_filenames(chooser);
    }

    log_error("[DEBUG] gtk_file_chooser_get_filenames: returning NULL");
    return NULL;
}

// Hook: gtk_file_chooser_set_current_folder
void gtk_file_chooser_set_current_folder(void *chooser, const char *folder) {

    // Store folder in current_dialog so launch_reqasl() can use it
    if (current_dialog.initial_folder) {
        free(current_dialog.initial_folder);
    }
    current_dialog.initial_folder = folder ? strdup(folder) : NULL;

    // Still call original for compatibility (GTK dialog might need it)
    LOAD_ORIGINAL(gtk_file_chooser_set_current_folder);

    if (original_gtk_file_chooser_set_current_folder) {
        original_gtk_file_chooser_set_current_folder(chooser, folder);
    }
}

// Hook: gtk_file_chooser_set_current_name
void gtk_file_chooser_set_current_name(void *chooser, const char *name) {

    // Store name in current_dialog so launch_reqasl() can use it
    if (current_dialog.initial_name) {
        free(current_dialog.initial_name);
    }
    current_dialog.initial_name = name ? strdup(name) : NULL;

    // Still call original for compatibility (GTK dialog might need it)
    LOAD_ORIGINAL(gtk_file_chooser_set_current_name);

    if (original_gtk_file_chooser_set_current_name) {
        original_gtk_file_chooser_set_current_name(chooser, name);
    }
}

// Library constructor - runs when loaded
__attribute__((constructor))
static void init(void) {
    // Pre-load gtk_file_chooser_get_filename to ensure it's hooked properly
    LOAD_ORIGINAL(gtk_file_chooser_get_filename);
}

// Hook: gtk_file_chooser_get_uri
char* gtk_file_chooser_get_uri(void *chooser) {

    // CRITICAL: Only return our URI if this is OUR hooked dialog
    if (chooser == current_dialog.dialog &&
        current_dialog.filename &&
        (current_dialog.response == GTK_RESPONSE_OK ||
         current_dialog.response == GTK_RESPONSE_ACCEPT)) {
        // Convert filename to URI format
        char uri[1024];
        snprintf(uri, sizeof(uri), "file://%s", current_dialog.filename);
        return strdup(uri);
    }

    LOAD_ORIGINAL(gtk_file_chooser_get_uri);

    if (original_gtk_file_chooser_get_uri) {
        return original_gtk_file_chooser_get_uri(chooser);
    }

    return NULL;
}

// Hook: gtk_file_chooser_get_uris (returns GSList* of URI strings)
void* gtk_file_chooser_get_uris(void *chooser) {

    // If we have a valid filename from ReqASL, return it as URI list
    // NOTE: Not checking pointer equality due to GObject interface casting
    if (current_dialog.filename &&
        (current_dialog.response == GTK_RESPONSE_OK ||
         current_dialog.response == GTK_RESPONSE_ACCEPT)) {

        // Load GLib list function
        LOAD_ORIGINAL(g_slist_prepend);

        if (original_g_slist_prepend) {
            // Convert filename to URI
            char uri[1024];
            snprintf(uri, sizeof(uri), "file://%s", current_dialog.filename);

            // Create a GSList with one URI element
            void *list = NULL;
            list = original_g_slist_prepend(list, strdup(uri));
            return list;
        } else {
            log_error("[ERROR] Could not find g_slist_prepend");
            return NULL;
        }
    }

    // Otherwise call the original
    LOAD_ORIGINAL(gtk_file_chooser_get_uris);

    if (original_gtk_file_chooser_get_uris) {
        return original_gtk_file_chooser_get_uris(chooser);
    }

    return NULL;
}

// Hook: gtk_file_chooser_get_file (GTK3 modern API - returns GFile*)
void* gtk_file_chooser_get_file(void *chooser) {

    // If we have a valid filename from ReqASL, create GFile from it
    // NOTE: Not checking pointer equality due to GObject interface casting
    if (current_dialog.filename &&
        (current_dialog.response == GTK_RESPONSE_OK ||
         current_dialog.response == GTK_RESPONSE_ACCEPT)) {

        // Load GIO function to create GFile from path
        LOAD_ORIGINAL(g_file_new_for_path);

        if (original_g_file_new_for_path) {
            // Create and return GFile object (caller must g_object_unref() it)
            return original_g_file_new_for_path(current_dialog.filename);
        } else {
            log_error("[ERROR] Could not find g_file_new_for_path");
            return NULL;
        }
    }

    // Otherwise call the original
    LOAD_ORIGINAL(gtk_file_chooser_get_file);

    if (original_gtk_file_chooser_get_file) {
        return original_gtk_file_chooser_get_file(chooser);
    }

    return NULL;
}

// Hook: gtk_file_chooser_get_files (GTK3 modern API - returns GSList* of GFile*)
void* gtk_file_chooser_get_files(void *chooser) {

    // Check if this is for our hooked dialog (async apps call this via captured callback)
    if (chooser == current_dialog.dialog || current_dialog.needs_reqasl) {

        // Launch ReqASL to get user's actual file selection
        char *selected_file = launch_reqasl(
            current_dialog.action,
            current_dialog.title,
            current_dialog.initial_folder,
            current_dialog.initial_name
        );

        if (selected_file) {
            // User selected a file
            current_dialog.filename = selected_file;
            current_dialog.response = GTK_RESPONSE_OK;

            // Create GFile for selected file
            LOAD_ORIGINAL(g_file_new_for_path);
            if (original_g_file_new_for_path) {
                void *gfile = original_g_file_new_for_path(selected_file);

                if (gfile) {
                    // Return as GSList
                    LOAD_ORIGINAL(g_slist_prepend);
                    if (original_g_slist_prepend) {
                        void *list = NULL;
                        list = original_g_slist_prepend(list, gfile);
                        return list;
                    } else {
                        log_error("[ERROR] Could not find g_slist_prepend");
                    }
                } else {
                    log_error("[ERROR] g_file_new_for_path returned NULL");
                }
            } else {
                log_error("[ERROR] Could not find g_file_new_for_path");
            }
        }

        // User cancelled or error - return NULL
        return NULL;
    }

    // Not our dialog, call original
    LOAD_ORIGINAL(gtk_file_chooser_get_files);
    if (original_gtk_file_chooser_get_files) {
        return original_gtk_file_chooser_get_files(chooser);
    }

    return NULL;
}

// Hook: gtk_widget_destroy
void gtk_widget_destroy(void *widget) {

    LOAD_ORIGINAL(gtk_widget_destroy);

    if (original_gtk_widget_destroy) {
        original_gtk_widget_destroy(widget);
    }
}

// Hook: gtk_widget_show (for apps that show dialog without gtk_dialog_run)
void gtk_widget_show(void *widget) {

    // Check if this is our file chooser dialog being shown
    if (widget == current_dialog.dialog) {

        // Set flag for get_files() to launch ReqASL
        current_dialog.needs_reqasl = 1;

        // DON'T call original gtk_widget_show - dialog never appears!

        // Call xed's callback directly with GTK_RESPONSE_OK
        if (captured_callback) {
            int response = (current_dialog.action == GTK_FILE_CHOOSER_ACTION_OPEN)
                           ? GTK_RESPONSE_OK
                           : GTK_RESPONSE_ACCEPT;

            captured_callback(widget, response, captured_user_data);

        } else {
            log_error("[ERROR] No callback captured - cannot trigger file loading!");
        }

        return;
    }

    // Not our dialog, call original
    LOAD_ORIGINAL(gtk_widget_show);

    if (original_gtk_widget_show) {
        original_gtk_widget_show(widget);
    }
}

// Hook: gtk_widget_show_all (recursively shows widget and all children)
void gtk_widget_show_all(void *widget) {

    // Check if this is our file chooser dialog being shown
    if (widget == current_dialog.dialog) {
        // Block this too - don't show the dialog
        return;
    }

    // Not our dialog, call original
    LOAD_ORIGINAL(gtk_widget_show_all);

    if (original_gtk_widget_show_all) {
        original_gtk_widget_show_all(widget);
    }
}

// Hook: gtk_window_present (brings window to front and shows it)
void gtk_window_present(void *window) {

    // Check if this is our file chooser dialog
    if (window == current_dialog.dialog) {
        // Don't call original - dialog should not be presented
        // Signal was already emitted in gtk_widget_show()
        return;
    }

    // Not our dialog, call original
    LOAD_ORIGINAL(gtk_window_present);

    if (original_gtk_window_present) {
        original_gtk_window_present(window);
    }
}

// Hook: gtk_widget_map (maps widget to X11 window)
void gtk_widget_map(void *widget) {

    // Check if this is our file chooser dialog
    if (widget == current_dialog.dialog) {
        // Block this - don't map the dialog
        return;
    }

    // Not our dialog, call original
    LOAD_ORIGINAL(gtk_widget_map);

    if (original_gtk_widget_map) {
        original_gtk_widget_map(widget);
    }
}

// ============================================================================
// GLib Signal Hook (capture xed's response callback)
// ============================================================================

// Hook: g_signal_connect_data (captures callback when xed connects "response" signal)
unsigned long g_signal_connect_data(void *instance, const char *signal, void *callback,
                                     void *data, void *destroy_data, int connect_flags) {
    // Check if this is the "response" signal on our dialog
    if (instance == current_dialog.dialog && signal && strcmp(signal, "response") == 0) {
        captured_callback = (ResponseCallback)callback;
        captured_user_data = data;
    }

    // Call original to actually connect the signal
    LOAD_ORIGINAL(g_signal_connect_data);
    if (original_g_signal_connect_data) {
        return original_g_signal_connect_data(instance, signal, callback, data, destroy_data, connect_flags);
    }

    return 0;
}

// ============================================================================
// File Chooser Action Hook (for apps using g_object_new + setter functions)
// ============================================================================

// Hook: gtk_file_chooser_set_action
// This catches file choosers created via g_object_new() when they set action
void gtk_file_chooser_set_action(void *chooser, int action) {

    // Track this as our dialog if we haven't seen it yet
    if (current_dialog.dialog != chooser) {
        current_dialog.dialog = chooser;
        current_dialog.action = action;
        current_dialog.created_by_hook = 0;  // App created this dialog via g_object_new

        // Clear previous state
        if (current_dialog.filename) {
            free(current_dialog.filename);
            current_dialog.filename = NULL;
        }
        if (current_dialog.title) {
            free(current_dialog.title);
            current_dialog.title = NULL;
        }
    } else {
        // Update action if dialog already tracked
        current_dialog.action = action;
    }

    // Call original to actually set the action
    LOAD_ORIGINAL(gtk_file_chooser_set_action);
    if (original_gtk_file_chooser_set_action) {
        original_gtk_file_chooser_set_action(chooser, action);
    }
}

// Hook: gtk_file_chooser_get_action
int gtk_file_chooser_get_action(void *chooser) {

    // Call original
    LOAD_ORIGINAL(gtk_file_chooser_get_action);
    if (original_gtk_file_chooser_get_action) {
        return original_gtk_file_chooser_get_action(chooser);
    }

    return GTK_FILE_CHOOSER_ACTION_OPEN; // Default fallback
}

// ============================================================================
// GTK3 Native Dialog API Hooks (GTK 3.20+)
// ============================================================================

// Hook: gtk_file_chooser_native_new
void* gtk_file_chooser_native_new(const char *title,
                                   void *parent,
                                   int action,
                                   const char *accept_label,
                                   const char *cancel_label) {

    // Store dialog info (same as Classic Dialog API)
    current_dialog.action = action;
    if (current_dialog.title) {
        free(current_dialog.title);
    }
    current_dialog.title = title ? strdup(title) : NULL;

    // Clear previous filename
    if (current_dialog.filename) {
        free(current_dialog.filename);
        current_dialog.filename = NULL;
    }

    // Load original function
    LOAD_ORIGINAL(gtk_file_chooser_native_new);
    if (!original_gtk_file_chooser_native_new) {
        log_error("[ERROR] Could not find original gtk_file_chooser_native_new");
        return NULL;
    }

    // Create the native dialog
    void *dialog = original_gtk_file_chooser_native_new(title, parent, action,
                                                         accept_label, cancel_label);

    current_dialog.dialog = dialog;
    return dialog;
}

// Hook: gtk_native_dialog_run
int gtk_native_dialog_run(void *dialog) {

    // Check if this is our file chooser native dialog
    if (dialog == current_dialog.dialog) {

        // Launch ReqASL with stored initial folder and name
        char *selected_file = launch_reqasl(current_dialog.action, current_dialog.title,
                                            current_dialog.initial_folder, current_dialog.initial_name);

        if (selected_file) {
            // Store result - app will call get_filename() to retrieve it
            current_dialog.filename = selected_file;

            // Return response code that matches button mapping
            // leafpad expects OK (-5) for OPEN, others expect ACCEPT (-3)
            if (current_dialog.action == GTK_FILE_CHOOSER_ACTION_OPEN && is_leafpad()) {
                current_dialog.response = GTK_RESPONSE_OK;
            } else {
                current_dialog.response = GTK_RESPONSE_ACCEPT;
            }

            return current_dialog.response;
        } else {
            // User cancelled
            current_dialog.filename = NULL;
            current_dialog.response = GTK_RESPONSE_CANCEL;
            return GTK_RESPONSE_CANCEL;
        }
    }

    // Not our dialog, call original
    LOAD_ORIGINAL(gtk_native_dialog_run);

    if (original_gtk_native_dialog_run) {
        return original_gtk_native_dialog_run(dialog);
    }

    return GTK_RESPONSE_CANCEL;
}

// Hook: gtk_native_dialog_show (async API)
void gtk_native_dialog_show(void *dialog) {

    // Check if this is our file chooser native dialog
    if (dialog == current_dialog.dialog) {

        // Launch ReqASL synchronously (blocks until user responds)
        // Even though the app expects async behavior, we block here and then fire the callback
        char *selected_file = launch_reqasl(current_dialog.action, current_dialog.title,
                                            current_dialog.initial_folder, current_dialog.initial_name);

        if (selected_file) {
            // Store result - app will call get_filename() to retrieve it
            current_dialog.filename = selected_file;

            // Return response code that matches button mapping
            // leafpad expects OK (-5) for OPEN, others expect ACCEPT (-3)
            if (current_dialog.action == GTK_FILE_CHOOSER_ACTION_OPEN && is_leafpad()) {
                current_dialog.response = GTK_RESPONSE_OK;
            } else {
                current_dialog.response = GTK_RESPONSE_ACCEPT;
            }
        } else {
            // User cancelled
            current_dialog.filename = NULL;
            current_dialog.response = GTK_RESPONSE_CANCEL;
        }

        // Return immediately (async behavior)
        // NOTE: We don't call original_gtk_native_dialog_show because we handled everything
        return;
    }

    // Not our dialog, call original
    LOAD_ORIGINAL(gtk_native_dialog_show);

    if (original_gtk_native_dialog_show) {
        original_gtk_native_dialog_show(dialog);
    }
}

// Library destructor - cleanup
// REMOVED: Destructor was causing segfaults in forked child processes
// Child processes inherit parent's memory but shouldn't free it
// The OS will clean up memory when process exits anyway