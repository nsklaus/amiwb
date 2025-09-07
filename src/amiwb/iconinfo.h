// File: iconinfo.h
// Icon Information dialog for AmiWB
// Shows detailed file information and allows editing of various properties
#ifndef ICONINFO_H
#define ICONINFO_H

#include "config.h"
#include "intuition.h"
#include "icons.h"
#include "../toolkit/inputfield.h"
#include "../toolkit/button.h"
#include "../toolkit/listview.h"
#include <stdbool.h>
#include <sys/types.h>

// Dialog dimensions
#define ICONINFO_WIDTH 350
#define ICONINFO_HEIGHT 500  // Increased by 50

// Layout constants
#define ICONINFO_MARGIN 15
#define ICONINFO_SPACING 8
#define ICONINFO_BUTTON_WIDTH 80
#define ICONINFO_BUTTON_HEIGHT 25
#define ICONINFO_LABEL_WIDTH 80
#define ICONINFO_ICON_SIZE 64  // Original icon size, will be displayed at 2x

// Icon Information Dialog structure
typedef struct IconInfoDialog {
    Canvas *canvas;               // Dialog window (350x450)
    FileIcon *icon;              // Icon being inspected
    
    // Display elements
    Picture icon_2x;             // Scaled 2x icon for display
    int icon_display_size;       // Calculated display size (2x original)
    
    // Editable fields (toolkit InputFields)
    InputField *name_field;      // Filename (editable)
    InputField *comment_field;   // File comment via xattr (editable)
    ListView *comment_list;      // List of comment lines
    InputField *app_field;       // Default application (editable)
    InputField *path_field;      // Directory path (read-only, for copying)
    
    // Read-only display strings
    char size_text[64];          // File/dir size display
    char perms_text[32];         // Permission string (rwxrwxrwx)
    char owner_text[32];         // Owner username
    char group_text[32];         // Group name
    char created_text[64];       // Creation date/time
    char modified_text[64];      // Last modified date/time
    
    // Permission checkbox states
    bool perm_user_read;
    bool perm_user_write;
    bool perm_user_exec;
    bool perm_group_read;
    bool perm_group_write;
    bool perm_group_exec;
    bool perm_other_read;
    bool perm_other_write;
    bool perm_other_exec;
    
    // Button states
    bool ok_pressed;
    bool cancel_pressed;
    bool get_size_pressed;       // For directory size calculation
    
    // Toolkit buttons (for proper hit testing)
    Button *get_size_button;     // Get Size button for directories
    Button *ok_button;           // OK button at bottom
    Button *cancel_button;       // Cancel button at bottom
    
    // Directory size calculation
    bool calculating_size;       // Currently calculating
    bool is_directory;          // True if icon represents a directory
    pid_t size_calc_pid;        // Child process PID
    int size_pipe_fd;           // Pipe for receiving results
    
    // Linked list for multiple dialogs
    struct IconInfoDialog *next;
} IconInfoDialog;

// Public API functions

// Main entry point - shows dialog for selected icon
void show_icon_info_dialog(FileIcon *icon);

// Event handlers (called from events.c)
bool iconinfo_handle_key_press(XKeyEvent *event);
bool iconinfo_handle_button_press(XButtonEvent *event);
bool iconinfo_handle_button_release(XButtonEvent *event);
bool iconinfo_handle_motion(XMotionEvent *event);

// Query functions
bool is_iconinfo_canvas(Canvas *canvas);
IconInfoDialog* get_iconinfo_for_canvas(Canvas *canvas);

// Rendering (called from render.c)
void render_iconinfo_content(Canvas *canvas);

// Cleanup
void close_icon_info_dialog(IconInfoDialog *dialog);
void close_icon_info_dialog_by_canvas(Canvas *canvas);
void cleanup_all_iconinfo_dialogs(void);

// Initialize/cleanup subsystem
void init_iconinfo(void);
void cleanup_iconinfo(void);

// Process monitoring for directory size calculation
void iconinfo_check_size_calculations(void);

#endif // ICONINFO_H