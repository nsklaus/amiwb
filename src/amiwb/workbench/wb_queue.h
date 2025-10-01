// File: wb_queue.h
// Directory Queue - Shared utility for iterative directory traversal
// Used by wb_fileops.c and wb_progress.c to avoid stack overflow

#ifndef WB_QUEUE_H
#define WB_QUEUE_H

// Queue node with optional destination path (for copy operations)
typedef struct DirQueueNode {
    char *path;
    char *dest_path;
    struct DirQueueNode *next;
} DirQueueNode;

// Queue structure
typedef struct {
    DirQueueNode *head;
    DirQueueNode *tail;
    int size;
} DirQueue;

// Queue operations
void wb_queue_init(DirQueue *q);
int wb_queue_push_pair(DirQueue *q, const char *path, const char *dest_path);
int wb_queue_push(DirQueue *q, const char *path);
char *wb_queue_pop_pair(DirQueue *q, char **dest_out);
char *wb_queue_pop(DirQueue *q);
void wb_queue_free(DirQueue *q);

#endif // WB_QUEUE_H
