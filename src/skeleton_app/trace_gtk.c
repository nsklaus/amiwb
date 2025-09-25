/* trace_gtk.c - Trace GTK file dialog calls to see what Brave uses */

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <stdarg.h>

/* Trace gtk_file_chooser_dialog_new */
void* gtk_file_chooser_dialog_new(const char *title, void *parent,
                                  int action, const char *first_button, ...) {
    fprintf(stderr, "[TRACE] gtk_file_chooser_dialog_new(%s, action=%d)\n",
            title ? title : "NULL", action);

    void* (*original)(const char*, void*, int, const char*, ...) =
        dlsym(RTLD_NEXT, "gtk_file_chooser_dialog_new");

    if (!original) return NULL;

    va_list args;
    va_start(args, first_button);
    void *result = original(title, parent, action, first_button, args);
    va_end(args);

    return result;
}

/* Trace gtk_file_chooser_native_new */
void* gtk_file_chooser_native_new(const char *title, void *parent,
                                  int action, const char *accept,
                                  const char *cancel) {
    fprintf(stderr, "[TRACE] gtk_file_chooser_native_new(%s, action=%d)\n",
            title ? title : "NULL", action);

    void* (*original)(const char*, void*, int, const char*, const char*) =
        dlsym(RTLD_NEXT, "gtk_file_chooser_native_new");

    if (!original) return NULL;
    return original(title, parent, action, accept, cancel);
}

/* Trace gtk_native_dialog_run */
int gtk_native_dialog_run(void *dialog) {
    fprintf(stderr, "[TRACE] gtk_native_dialog_run()\n");

    int (*original)(void*) = dlsym(RTLD_NEXT, "gtk_native_dialog_run");
    if (!original) return -1;
    return original(dialog);
}

/* Trace gtk_native_dialog_show */
void gtk_native_dialog_show(void *dialog) {
    fprintf(stderr, "[TRACE] gtk_native_dialog_show()\n");

    void (*original)(void*) = dlsym(RTLD_NEXT, "gtk_native_dialog_show");
    if (original) original(dialog);
}

/* Trace gtk_dialog_run */
int gtk_dialog_run(void *dialog) {
    fprintf(stderr, "[TRACE] gtk_dialog_run()\n");

    int (*original)(void*) = dlsym(RTLD_NEXT, "gtk_dialog_run");
    if (!original) return -1;
    return original(dialog);
}

/* Trace g_object_new for GTK_TYPE_FILE_CHOOSER_DIALOG */
void* g_object_new(unsigned long type, const char *first_property, ...) {
    static unsigned long gtk_file_chooser_dialog_type = 0;

    /* Try to get the type */
    if (gtk_file_chooser_dialog_type == 0) {
        unsigned long (*get_type)(void) = dlsym(RTLD_NEXT, "gtk_file_chooser_dialog_get_type");
        if (get_type) gtk_file_chooser_dialog_type = get_type();
    }

    if (type == gtk_file_chooser_dialog_type) {
        fprintf(stderr, "[TRACE] g_object_new(GTK_TYPE_FILE_CHOOSER_DIALOG)\n");
    }

    void* (*original)(unsigned long, const char*, ...) = dlsym(RTLD_NEXT, "g_object_new");
    if (!original) return NULL;

    va_list args;
    va_start(args, first_property);
    void *result = original(type, first_property, args);
    va_end(args);

    return result;
}

/* Trace zenity calls (some apps use this) */
int system(const char *command) {
    if (command && strstr(command, "zenity")) {
        fprintf(stderr, "[TRACE] system(%s)\n", command);
    }

    int (*original)(const char*) = dlsym(RTLD_NEXT, "system");
    if (!original) return -1;
    return original(command);
}