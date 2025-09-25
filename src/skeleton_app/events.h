/* events.h - Event handling system */

/*
 * What could be added later:
 * - Event filtering
 * - Event recording/playback
 * - Custom event types
 * - Event statistics
 * - Gesture recognition
 */

#ifndef SKELETON_EVENTS_H
#define SKELETON_EVENTS_H

#include <X11/Xlib.h>

/* Forward declaration - don't redefine if skeleton.h included */
struct SkeletonApp;

/* Process X11 event - returns 0 if should quit */
int events_dispatch(struct SkeletonApp *app, XEvent *event);

#endif /* SKELETON_EVENTS_H */