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

// GTK file chooser actions
#define GTK_FILE_CHOOSER_ACTION_OPEN           0
#define GTK_FILE_CHOOSER_ACTION_SAVE           1
#define GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER  2
#define GTK_FILE_CHOOSER_ACTION_CREATE_FOLDER  3

// GTK dialog response codes
#define GTK_RESPONSE_CANCEL -6
#define GTK_RESPONSE_ACCEPT -3
#define GTK_RESPONSE_OK     -5

// Store dialog state
typedef struct {
    void *dialog;
    int action;
    char *title;
    char *filename;  // Selected file from ReqASL
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
static char* (*original_gtk_file_chooser_get_uri)(void *chooser) = NULL;
static void (*original_gtk_file_chooser_set_current_folder)(void *chooser, const char *folder) = NULL;
static void (*original_gtk_file_chooser_set_current_name)(void *chooser, const char *name) = NULL;
static int (*original_gtk_file_chooser_set_filename)(void *chooser, const char *filename) = NULL;
static void (*original_gtk_widget_destroy)(void *widget) = NULL;

// Launch ReqASL and get result
static char* launch_reqasl(int action, const char *title) {
    
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
    
    // Get user home directory
    const char *home = getenv("HOME");
    if (!home) {
        home = "/home";
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
             mode, home, window_title);
    
    // Execute and read result
    FILE *fp = popen(command, "r");
    if (!fp) {
        printf("[HOOK] ERROR: Failed to launch ReqASL\n");
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
            printf("[HOOK] ReqASL cancelled\n");
            return NULL;
        }
        
        pclose(fp);
        printf("[HOOK] ReqASL returned: %s\n", result);
        return strdup(result);
    }
    
    pclose(fp);
    printf("[HOOK] ReqASL returned nothing\n");
    return NULL;
}

// Hook: gtk_file_chooser_dialog_new
void* gtk_file_chooser_dialog_new(const char *title, 
                                  void *parent,
                                  int action,
                                  const char *first_button_text,
                                  ...) {
    
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
    if (!original_gtk_file_chooser_dialog_new) {
        original_gtk_file_chooser_dialog_new = dlsym(RTLD_NEXT, "gtk_file_chooser_dialog_new");
        if (!original_gtk_file_chooser_dialog_new) {
            printf("[HOOK] ERROR: Could not find original gtk_file_chooser_dialog_new\n");
            return NULL;
        }
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
    
    // Check if this is our file chooser dialog
    if (dialog == current_dialog.dialog) {
        
        // Launch ReqASL
        char *selected_file = launch_reqasl(current_dialog.action, current_dialog.title);
        
        if (selected_file) {
            current_dialog.filename = selected_file;
            current_dialog.response = GTK_RESPONSE_OK;
            // Set filename in GTK dialog
            if (!original_gtk_file_chooser_set_filename) {
                original_gtk_file_chooser_set_filename = dlsym(RTLD_NEXT, "gtk_file_chooser_set_filename");
            }
            if (original_gtk_file_chooser_set_filename) {
                original_gtk_file_chooser_set_filename(dialog, selected_file);
            }
            
            return GTK_RESPONSE_OK;
        } else {
            current_dialog.filename = NULL;
            current_dialog.response = GTK_RESPONSE_CANCEL;
            return GTK_RESPONSE_CANCEL;
        }
    }
    
    // Not our dialog, call original
    if (!original_gtk_dialog_run) {
        original_gtk_dialog_run = dlsym(RTLD_NEXT, "gtk_dialog_run");
    }
    
    if (original_gtk_dialog_run) {
        return original_gtk_dialog_run(dialog);
    }
    
    return GTK_RESPONSE_CANCEL;
}

// Hook: gtk_file_chooser_get_filename
char* gtk_file_chooser_get_filename(void *chooser) {
    // If we have a filename from ReqASL, return it
    if (current_dialog.filename && (current_dialog.response == GTK_RESPONSE_OK || 
                                    current_dialog.response == GTK_RESPONSE_ACCEPT)) {
        return strdup(current_dialog.filename);
    }
    
    // Otherwise call the original
    if (!original_gtk_file_chooser_get_filename) {
        original_gtk_file_chooser_get_filename = dlsym(RTLD_NEXT, "gtk_file_chooser_get_filename");
    }
    
    if (original_gtk_file_chooser_get_filename) {
        return original_gtk_file_chooser_get_filename(chooser);
    }
    
    return NULL;
}

// Hook: gtk_file_chooser_set_current_folder (stub for now)
void gtk_file_chooser_set_current_folder(void *chooser, const char *folder) {
    
    // TODO: Pass this to ReqASL as initial directory
    
    if (!original_gtk_file_chooser_set_current_folder) {
        original_gtk_file_chooser_set_current_folder = dlsym(RTLD_NEXT, "gtk_file_chooser_set_current_folder");
    }
    
    if (original_gtk_file_chooser_set_current_folder) {
        original_gtk_file_chooser_set_current_folder(chooser, folder);
    }
}

// Hook: gtk_file_chooser_set_current_name (stub for now)
void gtk_file_chooser_set_current_name(void *chooser, const char *name) {
    
    // TODO: Pass this to ReqASL as initial filename (for save dialogs)
    
    if (!original_gtk_file_chooser_set_current_name) {
        original_gtk_file_chooser_set_current_name = dlsym(RTLD_NEXT, "gtk_file_chooser_set_current_name");
    }
    
    if (original_gtk_file_chooser_set_current_name) {
        original_gtk_file_chooser_set_current_name(chooser, name);
    }
}

// Library constructor - runs when loaded
__attribute__((constructor))
static void init(void) {
    // Pre-load gtk_file_chooser_get_filename to ensure it's hooked properly
    original_gtk_file_chooser_get_filename = dlsym(RTLD_NEXT, "gtk_file_chooser_get_filename");
}

// Hook: gtk_file_chooser_get_uri
char* gtk_file_chooser_get_uri(void *chooser) {
    
    if (current_dialog.filename) {
        // Convert filename to URI format
        char uri[1024];
        snprintf(uri, sizeof(uri), "file://%s", current_dialog.filename);
        return strdup(uri);
    }
    
    if (!original_gtk_file_chooser_get_uri) {
        original_gtk_file_chooser_get_uri = dlsym(RTLD_NEXT, "gtk_file_chooser_get_uri");
    }
    
    if (original_gtk_file_chooser_get_uri) {
        return original_gtk_file_chooser_get_uri(chooser);
    }
    
    return NULL;
}

// Hook: gtk_widget_destroy
void gtk_widget_destroy(void *widget) {
    if (widget == current_dialog.dialog) {
    }
    
    if (!original_gtk_widget_destroy) {
        original_gtk_widget_destroy = dlsym(RTLD_NEXT, "gtk_widget_destroy");
    }
    
    if (original_gtk_widget_destroy) {
        original_gtk_widget_destroy(widget);
    }
}

// Library destructor - cleanup
// REMOVED: Destructor was causing segfaults in forked child processes
// Child processes inherit parent's memory but shouldn't free it
// The OS will clean up memory when process exits anyway