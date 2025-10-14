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
static void (*original_gtk_file_chooser_set_current_folder)(void *chooser, const char *folder) = NULL;
static void (*original_gtk_file_chooser_set_current_name)(void *chooser, const char *name) = NULL;
static int (*original_gtk_file_chooser_set_filename)(void *chooser, const char *filename) = NULL;
static void (*original_gtk_widget_destroy)(void *widget) = NULL;

// GIO (GFile) functions
static void* (*original_g_file_new_for_path)(const char *path) = NULL;

// GLib (GSList) functions
static void* (*original_g_slist_prepend)(void *list, void *data) = NULL;

// GObject (signal) functions
static void (*original_g_signal_emit_by_name)(void *instance, const char *signal_name, ...) = NULL;

// GTK3 Native Dialog API (GTK 3.20+)
static void* (*original_gtk_file_chooser_native_new)(const char *title,
                                                       void *parent,
                                                       int action,
                                                       const char *accept_label,
                                                       const char *cancel_label) = NULL;
static int (*original_gtk_native_dialog_run)(void *dialog) = NULL;

// Launch ReqASL and get result
static char* launch_reqasl(int action, const char *title, const char *initial_folder, const char *initial_name) {

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
            log_error("[DEBUG] ReqASL cancelled");
            return NULL;
        }

        pclose(fp);
        log_error("[DEBUG] ReqASL returned: %s", result);
        return strdup(result);
    }

    pclose(fp);
    log_error("[DEBUG] ReqASL returned nothing");
    return NULL;
}

// Hook: gtk_file_chooser_dialog_new
void* gtk_file_chooser_dialog_new(const char *title,
                                  void *parent,
                                  int action,
                                  const char *first_button_text,
                                  ...) {

    log_error("[DEBUG] gtk_file_chooser_dialog_new called: title='%s', action=%d",
              title ? title : "NULL", action);

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
        dialog = original_gtk_file_chooser_dialog_new(title, parent, action,
                                                      "_Cancel", GTK_RESPONSE_CANCEL,
                                                      "_Open", GTK_RESPONSE_OK,
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
    return dialog;
}

// Hook: gtk_dialog_run
int gtk_dialog_run(void *dialog) {

    log_error("[DEBUG] gtk_dialog_run called: dialog=%p, our_dialog=%p",
              dialog, current_dialog.dialog);

    // Check if this is our file chooser dialog
    if (dialog == current_dialog.dialog) {

        log_error("[DEBUG] This is our dialog, launching ReqASL");

        // Launch ReqASL with stored initial folder and name
        char *selected_file = launch_reqasl(current_dialog.action, current_dialog.title,
                                            current_dialog.initial_folder, current_dialog.initial_name);
        
        if (selected_file) {
            current_dialog.filename = selected_file;
            // Use GTK_RESPONSE_ACCEPT for all actions (geany might expect this)
            current_dialog.response = GTK_RESPONSE_ACCEPT;

            // Set filename in GTK dialog
            LOAD_ORIGINAL(gtk_file_chooser_set_filename);
            if (original_gtk_file_chooser_set_filename) {
                int success = original_gtk_file_chooser_set_filename(dialog, selected_file);
                log_error("[DEBUG] gtk_file_chooser_set_filename returned: %d", success);
            } else {
                log_error("[ERROR] Could not find gtk_file_chooser_set_filename");
            }

            // CRITICAL: Emit "response" signal so geany's signal handler fires
            // This must happen BEFORE returning, while dialog is still alive
            LOAD_ORIGINAL(g_signal_emit_by_name);
            if (original_g_signal_emit_by_name) {
                log_error("[DEBUG] Emitting 'response' signal with response=%d", current_dialog.response);
                original_g_signal_emit_by_name(dialog, "response", current_dialog.response);
                log_error("[DEBUG] Signal emitted successfully");
            } else {
                log_error("[ERROR] Could not find g_signal_emit_by_name");
            }

            return current_dialog.response;
        } else {
            current_dialog.filename = NULL;
            current_dialog.response = GTK_RESPONSE_CANCEL;
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
    log_error("[DEBUG] get_filename: chooser=%p, dialog=%p, filename=%s, response=%d",
              chooser, current_dialog.dialog,
              current_dialog.filename ? current_dialog.filename : "NULL",
              current_dialog.response);

    // If we have a valid filename from ReqASL, return it
    // NOTE: Not checking pointer equality because GObject interface casting
    // means the same object has different pointers for different interfaces
    if (current_dialog.filename &&
        (current_dialog.response == GTK_RESPONSE_OK ||
         current_dialog.response == GTK_RESPONSE_ACCEPT)) {
        return strdup(current_dialog.filename);
    }

    // Otherwise call the original
    LOAD_ORIGINAL(gtk_file_chooser_get_filename);

    if (original_gtk_file_chooser_get_filename) {
        return original_gtk_file_chooser_get_filename(chooser);
    }

    return NULL;
}

// Hook: gtk_file_chooser_get_filenames (returns GSList* of strings)
void* gtk_file_chooser_get_filenames(void *chooser) {
    log_error("[DEBUG] get_filenames: chooser=%p, dialog=%p, filename=%s, response=%d",
              chooser, current_dialog.dialog,
              current_dialog.filename ? current_dialog.filename : "NULL",
              current_dialog.response);

    // If we have a valid filename from ReqASL, return it as list
    // NOTE: Not checking pointer equality due to GObject interface casting
    if (current_dialog.filename &&
        (current_dialog.response == GTK_RESPONSE_OK ||
         current_dialog.response == GTK_RESPONSE_ACCEPT)) {

        // Load GLib list function
        LOAD_ORIGINAL(g_slist_prepend);

        if (original_g_slist_prepend) {
            log_error("[DEBUG] Returning filename as list: %s", current_dialog.filename);
            // Create a GSList with one element (our filename)
            void *list = NULL;
            list = original_g_slist_prepend(list, strdup(current_dialog.filename));
            return list;
        } else {
            log_error("[ERROR] Could not find g_slist_prepend");
            return NULL;
        }
    }

    // Otherwise call the original
    LOAD_ORIGINAL(gtk_file_chooser_get_filenames);

    if (original_gtk_file_chooser_get_filenames) {
        log_error("[DEBUG] Calling original get_filenames");
        return original_gtk_file_chooser_get_filenames(chooser);
    }

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
    // Initialize log file (append mode - preserves messages on multiple loads)
    const char *log_path = "/home/klaus/Sources/amiwb/reqasl_hook.log";
    FILE *log = fopen(log_path, "a");
    if (log) {
        time_t now = time(NULL);
        fprintf(log, "\n========================================\n");
        fprintf(log, "Hook library loaded: %s", ctime(&now));
        fprintf(log, "========================================\n");
        fclose(log);
    }

    // Test that log_error works
    log_error("[DEBUG] Hook initialization complete");

    // Pre-load gtk_file_chooser_get_filename to ensure it's hooked properly
    LOAD_ORIGINAL(gtk_file_chooser_get_filename);
}

// Hook: gtk_file_chooser_get_uri
char* gtk_file_chooser_get_uri(void *chooser) {
    log_error("[DEBUG] get_uri: chooser=%p, dialog=%p",
              chooser, current_dialog.dialog);

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
    log_error("[DEBUG] get_uris: chooser=%p, dialog=%p",
              chooser, current_dialog.dialog);

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

            log_error("[DEBUG] Returning URI as list: %s", uri);

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
        log_error("[DEBUG] Calling original get_uris");
        return original_gtk_file_chooser_get_uris(chooser);
    }

    return NULL;
}

// Hook: gtk_file_chooser_get_file (GTK3 modern API - returns GFile*)
void* gtk_file_chooser_get_file(void *chooser) {
    log_error("[DEBUG] get_file: chooser=%p, dialog=%p",
              chooser, current_dialog.dialog);

    // If we have a valid filename from ReqASL, create GFile from it
    // NOTE: Not checking pointer equality due to GObject interface casting
    if (current_dialog.filename &&
        (current_dialog.response == GTK_RESPONSE_OK ||
         current_dialog.response == GTK_RESPONSE_ACCEPT)) {

        // Load GIO function to create GFile from path
        LOAD_ORIGINAL(g_file_new_for_path);

        if (original_g_file_new_for_path) {
            log_error("[DEBUG] Returning GFile: %s", current_dialog.filename);
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
        log_error("[DEBUG] Calling original get_file");
        return original_gtk_file_chooser_get_file(chooser);
    }

    return NULL;
}

// Hook: gtk_widget_destroy
void gtk_widget_destroy(void *widget) {
    log_error("[DEBUG] gtk_widget_destroy: widget=%p, dialog=%p",
              widget, current_dialog.dialog);

    if (widget == current_dialog.dialog) {
        log_error("[DEBUG] Our dialog is being destroyed!");
    }

    LOAD_ORIGINAL(gtk_widget_destroy);

    if (original_gtk_widget_destroy) {
        original_gtk_widget_destroy(widget);
    }
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

    log_error("[DEBUG] gtk_file_chooser_native_new called: title='%s', action=%d",
              title ? title : "NULL", action);

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

    log_error("[DEBUG] gtk_native_dialog_run called: dialog=%p, our_dialog=%p",
              dialog, current_dialog.dialog);

    // Check if this is our file chooser native dialog
    if (dialog == current_dialog.dialog) {

        log_error("[DEBUG] This is our native dialog, launching ReqASL");

        // Launch ReqASL with stored initial folder and name
        char *selected_file = launch_reqasl(current_dialog.action, current_dialog.title,
                                            current_dialog.initial_folder, current_dialog.initial_name);

        if (selected_file) {
            current_dialog.filename = selected_file;
            // Use GTK_RESPONSE_ACCEPT for all actions (geany might expect this)
            current_dialog.response = GTK_RESPONSE_ACCEPT;

            // Set filename in GTK dialog (native dialogs implement GtkFileChooser interface)
            LOAD_ORIGINAL(gtk_file_chooser_set_filename);
            if (original_gtk_file_chooser_set_filename) {
                int success = original_gtk_file_chooser_set_filename(dialog, selected_file);
                log_error("[DEBUG] gtk_file_chooser_set_filename returned: %d", success);
            } else {
                log_error("[ERROR] Could not find gtk_file_chooser_set_filename");
            }

            // CRITICAL: Emit "response" signal so geany's signal handler fires
            // This must happen BEFORE returning, while dialog is still alive
            LOAD_ORIGINAL(g_signal_emit_by_name);
            if (original_g_signal_emit_by_name) {
                log_error("[DEBUG] Emitting 'response' signal with response=%d", current_dialog.response);
                original_g_signal_emit_by_name(dialog, "response", current_dialog.response);
                log_error("[DEBUG] Signal emitted successfully");
            } else {
                log_error("[ERROR] Could not find g_signal_emit_by_name");
            }

            return current_dialog.response;
        } else {
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

// Library destructor - cleanup
// REMOVED: Destructor was causing segfaults in forked child processes
// Child processes inherit parent's memory but shouldn't free it
// The OS will clean up memory when process exits anyway