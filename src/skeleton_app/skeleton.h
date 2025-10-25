/* skeleton.h - Main application structure */

/*
 * What could be added later:
 * - Application state management
 * - Document/project handling
 * - Undo/redo system
 * - Plugin architecture
 * - Settings persistence
 */

#ifndef SKELETON_H
#define SKELETON_H

#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>
#include <X11/Xft/Xft.h>

/* Forward declarations for toolkit widgets */
typedef struct Button Button;
typedef struct InputField InputField;

typedef struct SkeletonApp {
    Display *display;
    Window main_window;
    int width, height;

    /* Rendering resources (draw directly to window under compositor) */
    Picture picture;
    XftDraw *xft_draw;

    /* Example toolkit widgets */
    Button *example_button;
    InputField *example_input;

    /* Add more widgets and app state here */
} SkeletonApp;

/* Create application instance */
SkeletonApp *skeleton_create(Display *display);

/* Draw the application content */
void skeleton_draw(SkeletonApp *app);

/* Destroy and cleanup */
void skeleton_destroy(SkeletonApp *app);

#endif /* SKELETON_H */