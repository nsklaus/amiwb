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
static void (*original_g_signal_emit_by_name)(void *instance, const char *detailed_signal, ...) = NULL;

// GObject type checking functions
static unsigned long (*original_g_type_check_instance_is_a)(void *instance, unsigned long iface_type) = NULL;
static unsigned long (*original_g_type_from_name)(const char *name) = NULL;

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
static int is_file_chooser(void *widget);

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

// ============================================================================
// App Detection System
// ============================================================================

// Detected application types
typedef enum {
    APP_UNKNOWN = 0,
    APP_LEAFPAD,
    APP_XED,
    APP_GIMP,
    APP_GEANY,
    APP_XARCHIVER,
    APP_TRANSMISSION,  // GTK4 future
} AppType;

// Detect which application is currently running
// Caches result for performance (process identity doesn't change)
static AppType detect_app(void) {
    static AppType cached_app = APP_UNKNOWN;
    static int detected = 0;

    if (detected) {
        return cached_app;
    }

    // Read process name from /proc/self/exe
    char exe_path[256];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);

    if (len == -1) {
        detected = 1;
        return APP_UNKNOWN;
    }

    exe_path[len] = '\0';

    // Check each known app
    // Order: Most specific first to avoid false matches
    if (strstr(exe_path, "transmission-gtk") || strstr(exe_path, "transmission")) {
        cached_app = APP_TRANSMISSION;
    } else if (strstr(exe_path, "xarchiver")) {
        cached_app = APP_XARCHIVER;
    } else if (strstr(exe_path, "leafpad")) {
        cached_app = APP_LEAFPAD;
    } else if (strstr(exe_path, "xed")) {
        cached_app = APP_XED;
    } else if (strstr(exe_path, "gimp")) {
        cached_app = APP_GIMP;
    } else if (strstr(exe_path, "geany")) {
        cached_app = APP_GEANY;
    } else {
        cached_app = APP_UNKNOWN;
    }

    detected = 1;
    return cached_app;
}

// Legacy compatibility wrapper (for any external references)
static int is_leafpad(void) {
    return (detect_app() == APP_LEAFPAD);
}

// ============================================================================
// Per-App Response Code Logic
// ============================================================================

// Get appropriate GTK response code for current app and action
// Centralizes all response code decisions to prevent scattered quirks
static int get_response_code_for_app(int action) {
    AppType app = detect_app();

    switch (app) {
        case APP_LEAFPAD:
        case APP_XED:
        case APP_GIMP:
            // These apps check `if (response_id == GTK_RESPONSE_OK)` for OPEN
            // Verified in source code (see docs/markdown/hook_plan.md)
            if (action == GTK_FILE_CHOOSER_ACTION_OPEN) {
                return GTK_RESPONSE_OK;  // -5
            } else {
                return GTK_RESPONSE_ACCEPT;  // -3
            }

        case APP_TRANSMISSION:
            // GTK4 app - implementation pending
            // Isolated path to prevent breaking GTK2/3 apps
            // TODO: Verify response code expectations when implementing GTK4
            return GTK_RESPONSE_ACCEPT;  // Placeholder

        case APP_GEANY:
        case APP_XARCHIVER:
            // These apps use ACCEPT for all actions (standard GTK behavior)
            return GTK_RESPONSE_ACCEPT;  // -3

        case APP_UNKNOWN:
        default:
            // Generic fallback - use majority pattern (OK for OPEN, ACCEPT for SAVE)
            // Most GTK apps seem to expect OK for OPEN despite documentation
            if (action == GTK_FILE_CHOOSER_ACTION_OPEN) {
                return GTK_RESPONSE_OK;
            } else {
                return GTK_RESPONSE_ACCEPT;
            }
    }
}

// Check if widget is a GtkFileChooser
static int is_file_chooser(void *widget) {
    if (!widget) {
        return 0;
    }

    // Load GObject type checking function
    LOAD_ORIGINAL(g_type_check_instance_is_a);
    LOAD_ORIGINAL(g_type_from_name);

    if (!original_g_type_check_instance_is_a || !original_g_type_from_name) {
        log_error("[ERROR] Could not load GObject type checking functions");
        return 0;
    }

    // Get GtkFileChooser interface type
    unsigned long file_chooser_type = original_g_type_from_name("GtkFileChooser");
    if (file_chooser_type == 0) {
        log_error("[ERROR] Could not get GtkFileChooser type");
        return 0;
    }

    // Check if widget implements GtkFileChooser interface
    int result = original_g_type_check_instance_is_a(widget, file_chooser_type);
    log_error("[DEBUG] is_file_chooser: widget=%p, implements GtkFileChooser=%d", widget, result);
    return result;
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

        // Clear previous state (important for reused dialogs like xarchiver)
        if (current_dialog.filename) {
            free(current_dialog.filename);
            current_dialog.filename = NULL;
        }
        current_dialog.response = 0;
        current_dialog.needs_reqasl = 0;

        // Launch ReqASL with stored initial folder and name
        char *selected_file = launch_reqasl(current_dialog.action, current_dialog.title,
                                            current_dialog.initial_folder, current_dialog.initial_name);

        if (selected_file) {
            // Store result - app will call get_filename() to retrieve it
            current_dialog.filename = selected_file;
            current_dialog.response = get_response_code_for_app(current_dialog.action);

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

    log_error("[DEBUG] gtk_file_chooser_get_uri: chooser=%p, dialog=%p, filename='%s', response=%d",
              chooser, current_dialog.dialog, current_dialog.filename ? current_dialog.filename : "(null)",
              current_dialog.response);

    // If we have a valid filename from ReqASL, return it as URI
    // NOTE: Not checking pointer equality because GObject interface casting
    // means the same object has different pointers for different interfaces
    if (current_dialog.filename &&
        (current_dialog.response == GTK_RESPONSE_OK ||
         current_dialog.response == GTK_RESPONSE_ACCEPT)) {
        // Convert filename to URI format
        char uri[1024];
        snprintf(uri, sizeof(uri), "file://%s", current_dialog.filename);
        log_error("[DEBUG] gtk_file_chooser_get_uri: RETURNING URI='%s'", uri);
        return strdup(uri);
    }

    LOAD_ORIGINAL(gtk_file_chooser_get_uri);

    if (original_gtk_file_chooser_get_uri) {
        log_error("[DEBUG] gtk_file_chooser_get_uri: calling original GTK function");
        return original_gtk_file_chooser_get_uri(chooser);
    }

    log_error("[DEBUG] gtk_file_chooser_get_uri: returning NULL");
    return NULL;
}

// Hook: gtk_file_chooser_get_uris (returns GSList* of URI strings)
void* gtk_file_chooser_get_uris(void *chooser) {

    log_error("[DEBUG] gtk_file_chooser_get_uris: chooser=%p, dialog=%p, filename='%s', response=%d",
              chooser, current_dialog.dialog, current_dialog.filename ? current_dialog.filename : "(null)",
              current_dialog.response);

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
            log_error("[DEBUG] gtk_file_chooser_get_uris: RETURNING URI list with '%s'", uri);
            return list;
        } else {
            log_error("[ERROR] Could not find g_slist_prepend");
            return NULL;
        }
    }

    // Otherwise call the original
    LOAD_ORIGINAL(gtk_file_chooser_get_uris);

    if (original_gtk_file_chooser_get_uris) {
        log_error("[DEBUG] gtk_file_chooser_get_uris: calling original GTK function");
        return original_gtk_file_chooser_get_uris(chooser);
    }

    log_error("[DEBUG] gtk_file_chooser_get_uris: returning NULL");
    return NULL;
}

// Hook: gtk_file_chooser_get_file (GTK3 modern API - returns GFile*)
void* gtk_file_chooser_get_file(void *chooser) {

    log_error("[DEBUG] gtk_file_chooser_get_file: chooser=%p, dialog=%p, needs_reqasl=%d, filename='%s'",
              chooser, current_dialog.dialog, current_dialog.needs_reqasl,
              current_dialog.filename ? current_dialog.filename : "(null)");

    // Check if we need to launch ReqASL (async pattern where show() set the flag)
    if (current_dialog.needs_reqasl) {
        log_error("[DEBUG] gtk_file_chooser_get_file: needs_reqasl flag set, launching ReqASL NOW");

        // Launch ReqASL to get user's actual file selection
        char *selected_file = launch_reqasl(
            current_dialog.action,
            current_dialog.title,
            current_dialog.initial_folder,
            current_dialog.initial_name
        );

        if (selected_file) {
            current_dialog.filename = selected_file;
            current_dialog.response = GTK_RESPONSE_OK;
            current_dialog.needs_reqasl = 0; // Clear flag

            // Create GFile for selected file
            LOAD_ORIGINAL(g_file_new_for_path);
            if (original_g_file_new_for_path) {
                log_error("[DEBUG] gtk_file_chooser_get_file: RETURNING GFile for '%s'", selected_file);
                return original_g_file_new_for_path(selected_file);
            }
        }

        // User cancelled or error
        current_dialog.needs_reqasl = 0; // Clear flag
        return NULL;
    }

    // If we have a valid filename from ReqASL, create GFile from it
    // NOTE: Not checking pointer equality due to GObject interface casting
    if (current_dialog.filename &&
        (current_dialog.response == GTK_RESPONSE_OK ||
         current_dialog.response == GTK_RESPONSE_ACCEPT)) {

        // Load GIO function to create GFile from path
        LOAD_ORIGINAL(g_file_new_for_path);

        if (original_g_file_new_for_path) {
            // Create and return GFile object (caller must g_object_unref() it)
            log_error("[DEBUG] gtk_file_chooser_get_file: RETURNING cached GFile for '%s'", current_dialog.filename);
            return original_g_file_new_for_path(current_dialog.filename);
        } else {
            log_error("[ERROR] Could not find g_file_new_for_path");
            return NULL;
        }
    }

    // Otherwise call the original
    log_error("[DEBUG] gtk_file_chooser_get_file: calling original GTK function");
    LOAD_ORIGINAL(gtk_file_chooser_get_file);

    if (original_gtk_file_chooser_get_file) {
        return original_gtk_file_chooser_get_file(chooser);
    }

    return NULL;
}

// Hook: gtk_file_chooser_get_files (GTK3 modern API - returns GSList* of GFile*)
void* gtk_file_chooser_get_files(void *chooser) {

    log_error("[DEBUG] gtk_file_chooser_get_files: chooser=%p, dialog=%p, needs_reqasl=%d, filename='%s'",
              chooser, current_dialog.dialog, current_dialog.needs_reqasl,
              current_dialog.filename ? current_dialog.filename : "(null)");

    // Check if this is for our hooked dialog
    if (chooser == current_dialog.dialog || current_dialog.needs_reqasl) {

        // If we already have a filename (GIMP pattern - launched in gtk_window_present)
        if (current_dialog.filename &&
            (current_dialog.response == GTK_RESPONSE_OK ||
             current_dialog.response == GTK_RESPONSE_ACCEPT)) {

            log_error("[DEBUG] gtk_file_chooser_get_files: returning stored filename (GIMP pattern)");

            // Create GFile for stored filename
            LOAD_ORIGINAL(g_file_new_for_path);
            if (original_g_file_new_for_path) {
                void *gfile = original_g_file_new_for_path(current_dialog.filename);

                if (gfile) {
                    // Return as GSList
                    LOAD_ORIGINAL(g_slist_prepend);
                    if (original_g_slist_prepend) {
                        void *list = NULL;
                        list = original_g_slist_prepend(list, gfile);
                        log_error("[DEBUG] gtk_file_chooser_get_files: RETURNING GSList with GFile");
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

            return NULL;
        }

        // No filename yet - launch ReqASL now (xed callback pattern)
        log_error("[DEBUG] gtk_file_chooser_get_files: launching ReqASL now (xed pattern)");

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

        // Call callback with appropriate response code
        if (captured_callback) {
            int response = get_response_code_for_app(current_dialog.action);
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

// ============================================================================
// Per-App gtk_window_present() Handlers
// ============================================================================

// Handler: transmission-gtk (GTK4 future implementation)
static void handle_transmission_window_present(void *window) {
    log_error("[DEBUG] handle_transmission_window_present: GTK4 pattern (NOT IMPLEMENTED)");

    // TODO: Implement GTK4 async pattern when we get to transmission-gtk
    // For now, fall back to generic handler

    // Placeholder: Use generic pattern until GTK4 is implemented
    log_error("[WARNING] transmission-gtk detected but GTK4 hooks not implemented yet");
    log_error("[WARNING] Falling back to generic handler - may not work correctly");

    // Call original for now
    LOAD_ORIGINAL(gtk_window_present);
    if (original_gtk_window_present) {
        original_gtk_window_present(window);
    }
}

// Handler: Generic fallback for unknown apps
static void handle_generic_window_present(void *window) {
    log_error("[DEBUG] handle_generic_window_present: generic pattern for app");

    // Check if we have a captured callback (async callback pattern like xed)
    if (captured_callback) {
        // Set flag for get_files() to launch ReqASL
        current_dialog.needs_reqasl = 1;

        int response = get_response_code_for_app(current_dialog.action);

        log_error("[DEBUG] handle_generic_window_present: invoking captured callback with response=%d", response);
        captured_callback(window, response, captured_user_data);

    } else {
        // No callback - use GIMP-style pattern (signal emission)
        log_error("[DEBUG] handle_generic_window_present: no callback, using GIMP-style pattern");

        char *selected_file = launch_reqasl(
            current_dialog.action,
            current_dialog.title,
            current_dialog.initial_folder,
            current_dialog.initial_name
        );

        if (selected_file) {
            current_dialog.filename = selected_file;
            current_dialog.response = get_response_code_for_app(current_dialog.action);

            // Emit "response" signal
            LOAD_ORIGINAL(g_signal_emit_by_name);
            if (original_g_signal_emit_by_name) {
                original_g_signal_emit_by_name(window, "response", current_dialog.response);
            }
        } else {
            current_dialog.filename = NULL;
            current_dialog.response = GTK_RESPONSE_CANCEL;

            LOAD_ORIGINAL(g_signal_emit_by_name);
            if (original_g_signal_emit_by_name) {
                original_g_signal_emit_by_name(window, "response", GTK_RESPONSE_CANCEL);
            }
        }
    }
}

// Hook: gtk_window_present (brings window to front and shows it)
// Uses per-app dispatch to isolate app-specific quirks
void gtk_window_present(void *window) {

    log_error("[DEBUG] gtk_window_present: called with window=%p, current_dialog.dialog=%p",
              window, current_dialog.dialog);

    // Check if this is our tracked dialog
    if (window == current_dialog.dialog) {
        log_error("[DEBUG] gtk_window_present: intercepting tracked dialog");
        goto dispatch_to_handler;
    }

    // Unknown window - check if it's a file chooser (opportunistic detection)
    if (is_file_chooser(window)) {
        log_error("[DEBUG] gtk_window_present: opportunistically detected file chooser!");

        // Clear previous dialog state (important for reused dialogs)
        if (current_dialog.filename) {
            free(current_dialog.filename);
            current_dialog.filename = NULL;
        }
        current_dialog.response = 0;
        current_dialog.needs_reqasl = 0;

        // Track this dialog NOW
        current_dialog.dialog = window;

        // Get action from the file chooser
        LOAD_ORIGINAL(gtk_file_chooser_get_action);
        if (original_gtk_file_chooser_get_action) {
            current_dialog.action = original_gtk_file_chooser_get_action(window);
            log_error("[DEBUG] gtk_window_present: detected action=%d", current_dialog.action);
        } else {
            current_dialog.action = GTK_FILE_CHOOSER_ACTION_OPEN; // Default
        }

        goto dispatch_to_handler;
    }

    // Not a file chooser, call original
    log_error("[DEBUG] gtk_window_present: not a file chooser, calling original");
    LOAD_ORIGINAL(gtk_window_present);
    if (original_gtk_window_present) {
        original_gtk_window_present(window);
    }
    return;

dispatch_to_handler:
    // DON'T call original gtk_window_present - dialog never appears!

    // Dispatch to per-app handler based on detected app type
    AppType app = detect_app();

    switch (app) {
        case APP_TRANSMISSION:
            // GTK4 future - needs different handling
            handle_transmission_window_present(window);
            break;

        case APP_GIMP:
        case APP_LEAFPAD:
        case APP_XED:
        case APP_GEANY:
        case APP_XARCHIVER:
        case APP_UNKNOWN:
        default:
            // GIMP, xed, and most GTK2/3 apps use the same pattern:
            // Check for captured callback, use it if present, else emit signal
            handle_generic_window_present(window);
            break;
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

// Hook: g_signal_connect_data (captures callback when apps connect "response" signal)
unsigned long g_signal_connect_data(void *instance, const char *signal, void *callback,
                                     void *data, void *destroy_data, int connect_flags) {
    // Capture "response" signal connections on ANY GtkFileChooser
    // (even before we know it's our dialog - for GIMP's early signal connection)
    if (signal && strcmp(signal, "response") == 0) {
        // Check if this instance is a file chooser
        if (is_file_chooser(instance)) {
            log_error("[DEBUG] g_signal_connect_data: capturing 'response' signal handler on file chooser, instance=%p",
                      instance);
            captured_callback = (ResponseCallback)callback;
            captured_user_data = data;

            // Track this dialog if we haven't already
            if (!current_dialog.dialog) {
                log_error("[DEBUG] g_signal_connect_data: tracking dialog early via signal connection");
                current_dialog.dialog = instance;
            }
        }
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

        // Clear previous state (important for reused dialogs)
        if (current_dialog.filename) {
            free(current_dialog.filename);
            current_dialog.filename = NULL;
        }
        current_dialog.response = 0;
        current_dialog.needs_reqasl = 0;

        // Launch ReqASL with stored initial folder and name
        char *selected_file = launch_reqasl(current_dialog.action, current_dialog.title,
                                            current_dialog.initial_folder, current_dialog.initial_name);

        if (selected_file) {
            // Store result - app will call get_filename() to retrieve it
            current_dialog.filename = selected_file;
            current_dialog.response = get_response_code_for_app(current_dialog.action);

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
            current_dialog.response = get_response_code_for_app(current_dialog.action);
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