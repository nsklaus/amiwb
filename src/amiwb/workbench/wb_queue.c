// File: wb_queue.c
// Directory Queue - Shared utility for iterative directory traversal

#include "wb_queue.h"
#include "../config.h"
#include <stdlib.h>
#include <string.h>

// Initialize empty queue
void wb_queue_init(DirQueue *q) {
    if (!q) return;
    q->head = NULL;
    q->tail = NULL;
    q->size = 0;
}

// Push path pair (source and optional destination) to queue
int wb_queue_push_pair(DirQueue *q, const char *path, const char *dest_path) {
    if (!q || !path) return -1;

    DirQueueNode *node = malloc(sizeof(DirQueueNode));
    if (!node) {
        log_error("[ERROR] wb_queue_push: Failed to allocate queue node");
        return -1;
    }

    node->path = strdup(path);
    if (!node->path) {
        log_error("[ERROR] wb_queue_push: Failed to duplicate path");
        free(node);
        return -1;
    }

    if (dest_path) {
        node->dest_path = strdup(dest_path);
        if (!node->dest_path) {
            log_error("[ERROR] wb_queue_push: Failed to duplicate dest_path");
            free(node->path);
            free(node);
            return -1;
        }
    } else {
        node->dest_path = NULL;
    }

    node->next = NULL;

    if (q->tail) {
        q->tail->next = node;
    } else {
        q->head = node;
    }
    q->tail = node;
    q->size++;

    if (q->size > 10000) {
        log_error("[WARNING] Directory queue size exceeds 10000 entries");
    }

    return 0;
}

// Push single path to queue (no destination)
int wb_queue_push(DirQueue *q, const char *path) {
    return wb_queue_push_pair(q, path, NULL);
}

// Pop path pair from queue, returning source and optionally destination
char *wb_queue_pop_pair(DirQueue *q, char **dest_out) {
    if (!q || !q->head) return NULL;

    DirQueueNode *node = q->head;
    char *path = node->path;

    if (dest_out) {
        *dest_out = node->dest_path;
    } else if (node->dest_path) {
        free(node->dest_path);
    }

    q->head = node->next;
    if (!q->head) {
        q->tail = NULL;
    }

    q->size--;
    free(node);

    return path;
}

// Pop single path from queue
char *wb_queue_pop(DirQueue *q) {
    return wb_queue_pop_pair(q, NULL);
}

// Free all queue contents
void wb_queue_free(DirQueue *q) {
    if (!q) return;

    char *path;
    while ((path = wb_queue_pop(q)) != NULL) {
        free(path);
    }

    q->head = NULL;
    q->tail = NULL;
    q->size = 0;
}
