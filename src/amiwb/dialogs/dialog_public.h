// File: dialog_public.h
// Public API for AmiWB dialog system
// This header is included by external modules (workbench/, intuition/, menus/, etc.)

#ifndef DIALOG_PUBLIC_H
#define DIALOG_PUBLIC_H

#include "../config.h"
#include "../intuition/itn_public.h"
#include "../icons.h"
#include <stdbool.h>

// Forward declarations (opaque types for external modules)
typedef struct Dialog Dialog;

// ============================================================================
// Lifecycle Management
// ============================================================================

void init_dialogs(void);
void cleanup_dialogs(void);

// ============================================================================
// Standard Dialogs
// ============================================================================

// Show rename dialog with callbacks
void show_rename_dialog(const char *old_name,
                       void (*on_ok)(const char *new_name),
                       void (*on_cancel)(void),
                       void *user_data);

// Show delete confirmation dialog - CRITICAL FOR DATA SAFETY
void show_delete_confirmation(const char *message,
                             void (*on_confirm)(void),
                             void (*on_cancel)(void));

// Show execute command dialog
void show_execute_dialog(void (*on_ok)(const char *command),
                        void (*on_cancel)(void));

// ============================================================================
// Dialog Lifecycle - General
// ============================================================================

// Destroy dialog (complete cleanup)
void destroy_dialog(Dialog *dialog);

// Close dialog by canvas (special case during canvas destruction)
void close_dialog_by_canvas(Canvas *canvas);

// ============================================================================
// Query Functions
// ============================================================================

// Check if canvas is a dialog
bool is_dialog_canvas(Canvas *canvas);

// Get dialog for canvas
Dialog* get_dialog_for_canvas(Canvas *canvas);

// ============================================================================
// Event Handlers (called from events.c)
// ============================================================================

bool dialogs_handle_key_press(XKeyEvent *event);
bool dialogs_handle_button_press(XButtonEvent *event);
bool dialogs_handle_button_release(XButtonEvent *event);
bool dialogs_handle_motion(XMotionEvent *event);

// ============================================================================
// Rendering (called from render.c)
// ============================================================================

void render_dialog_content(Canvas *canvas);

#endif // DIALOG_PUBLIC_H
