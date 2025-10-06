// File: wb_progress.c
// Progress System - async file operations with IPC-based progress reporting

#define _POSIX_C_SOURCE 200809L
#include "wb_internal.h"
#include "wb_queue.h"
#include "wb_xattr.h"
#include "../config.h"
#include "../dialogs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/xattr.h>
#include <libgen.h>
#include <time.h>

// Forward declarations from dialogs.c
extern void add_progress_dialog_to_list(ProgressDialog *dialog);
extern ProgressDialog *get_all_progress_dialogs(void);
extern void update_progress_dialog(ProgressDialog *dialog, const char *filename, float percent);
extern void close_progress_dialog(ProgressDialog *dialog);
extern void remove_progress_dialog_from_list(ProgressDialog *dialog);

// Forward declarations from intuition
extern Canvas *itn_canvas_find_by_window(Window win);
extern void compute_max_scroll(Canvas *canvas);

// Forward declarations from render.c
extern void redraw_canvas(Canvas *canvas);

// Forward declarations from wb_layout.c
extern void compute_content_bounds(Canvas *canvas);
extern void apply_view_layout(Canvas *canvas);
extern void find_free_slot(Canvas *canvas, int *out_x, int *out_y);

// Forward declarations from wb_deficons.c
extern const char *wb_deficons_get_for_file(const char *filename, bool is_dir);

// Forward declarations from wb_icons_create.c
extern FileIcon *create_icon_with_metadata(const char *icon_path, Canvas *canvas, int x, int y,
                                            const char *full_path, const char *name, int type);

// Forward declarations from wb_fileops.c
extern void count_files_in_directory(const char *path, int *count);
extern void count_files_and_bytes(const char *path, int *file_count, off_t *total_bytes);

// ============================================================================
// IPC Message Structures for Progress Updates
// ============================================================================

// Message types for IPC protocol
#define MSG_TYPE_UPDATE   1   // Lightweight progress update
#define MSG_TYPE_FULL     2   // Full progress message

// Message header for robust IPC (8 bytes)
typedef struct {
    uint32_t magic;      // 0x414D4942 ('AMIB' in hex)
    uint16_t msg_type;   // MSG_TYPE_UPDATE or MSG_TYPE_FULL
    uint16_t msg_size;   // Size of payload (not including header)
} __attribute__((packed)) MessageHeader;

// Lightweight progress update sent frequently during copy (24 bytes payload)
typedef struct {
    int files_done;
    int files_total;
    off_t bytes_done;   // Actual bytes copied (8 bytes on 64-bit)
    off_t bytes_total;  // Total bytes to copy (8 bytes on 64-bit)
} __attribute__((packed)) ProgressUpdate;

// Full progress message sent at START and COMPLETE only (2240 bytes)
typedef struct {
    enum {
        MSG_START,
        MSG_PROGRESS,  // Now deprecated - use ProgressUpdate instead
        MSG_COMPLETE,
        MSG_ERROR
    } type;
    time_t start_time;
    int files_done;
    int files_total;
    char current_file[NAME_SIZE];
    size_t bytes_done;
    size_t bytes_total;

    // Icon creation metadata (used on MSG_COMPLETE)
    char dest_path[PATH_SIZE];
    char dest_dir[PATH_SIZE];
    bool create_icon;
    bool has_sidecar;
    char sidecar_src[PATH_SIZE];
    char sidecar_dst[PATH_SIZE];
    int icon_x, icon_y;
    Window target_window;
} ProgressMessage;

// Progress tracking for directory operations
typedef struct {
    int total_files;
    int files_processed;
    off_t total_bytes;
    off_t bytes_copied;
    ProgressDialog *dialog;
    bool abort;
    int pipe_fd;
    time_t last_update_time;  // For time-based update throttling
} CopyProgress;

#define PROGRESS_DIALOG_THRESHOLD 1  // Show dialog after 1 second

// ============================================================================
// IPC Helper Functions
// ============================================================================

// Send a message with header (returns 0 on success, -1 on error)
static int send_message(int fd, uint16_t msg_type, const void *payload, uint16_t payload_size) {
    if (fd <= 0) return -1;

    MessageHeader header = {
        .magic = 0x414D4942,  // 'AMIB'
        .msg_type = msg_type,
        .msg_size = payload_size
    };

    // Write header
    if (write(fd, &header, sizeof(header)) != sizeof(header)) {
        return -1;
    }

    // Write payload
    if (payload_size > 0 && payload) {
        if (write(fd, payload, payload_size) != payload_size) {
            return -1;
        }
    }

    return 0;
}

// Read message header (returns 0 on success, -1 on error, 1 on no data)
static int read_message_header(int fd, MessageHeader *header) {
    if (fd <= 0 || !header) return -1;

    ssize_t bytes_read = read(fd, header, sizeof(*header));

    if (bytes_read == 0) return 1;  // No data
    if (bytes_read != sizeof(*header)) return -1;  // Partial read or error

    // Verify magic number
    if (header->magic != 0x414D4942) {
        log_error("[ERROR] Invalid message magic: 0x%08x", header->magic);
        return -1;
    }

    return 0;
}

// ============================================================================
// File Operations with Progress Reporting
// ============================================================================

// Copy file with byte-level progress
static int copy_file_with_progress(const char *src, const char *dst, int pipe_fd) {
    int in_fd = -1, out_fd = -1;
    struct stat st;

    if (stat(src, &st) != 0 || !S_ISREG(st.st_mode)) {
        return -1;
    }

    in_fd = open(src, O_RDONLY);
    if (in_fd < 0) {
        return -1;
    }

    out_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (out_fd < 0) {
        close(in_fd);
        return -1;
    }

    // Prepare progress message
    ProgressMessage msg = {
        .type = MSG_PROGRESS,
        .start_time = time(NULL),
        .files_done = 0,
        .files_total = 1,
        .bytes_done = 0,
        .bytes_total = st.st_size
    };
    // Extract basename without memory leak
    char temp_path[PATH_SIZE];
    strncpy(temp_path, src, PATH_SIZE - 1);
    temp_path[PATH_SIZE - 1] = '\0';
    strncpy(msg.current_file, basename(temp_path), NAME_SIZE - 1);

    // Copy with progress
    char buf[1 << 16];  // 64KB
    ssize_t r;
    size_t total_copied = 0;
    size_t last_progress_update = 0;

    while ((r = read(in_fd, buf, sizeof(buf))) > 0) {
        char *p = buf;
        ssize_t remaining = r;
        while (remaining > 0) {
            ssize_t w = write(out_fd, p, remaining);
            if (w < 0) {
                close(in_fd);
                close(out_fd);
                return -1;
            }
            p += w;
            remaining -= w;
        }

        total_copied += r;

        // Update progress every 1MB or at completion
        if (pipe_fd > 0 && (total_copied - last_progress_update > 1024*1024 ||
                            total_copied == (size_t)st.st_size)) {
            msg.bytes_done = total_copied;
            // Ignore errors - if pipe breaks, parent died, continue anyway
            send_message(pipe_fd, MSG_TYPE_FULL, &msg, sizeof(msg));
            last_progress_update = total_copied;
        }
    }

    if (r < 0) {
        close(in_fd);
        close(out_fd);
        return -1;
    }

    // Final progress if not sent
    if (pipe_fd > 0 && total_copied != last_progress_update) {
        msg.bytes_done = total_copied;
        msg.files_done = 1;
        send_message(pipe_fd, MSG_TYPE_FULL, &msg, sizeof(msg));
    }

    fchmod(out_fd, st.st_mode & 0777);
    close(in_fd);
    close(out_fd);

    // Preserve extended attributes
    wb_xattr_copy_all(src, dst);

    return 0;
}

// Copy directory tree with progress (iterative)
static int copy_directory_recursive_with_progress(const char *src_dir, const char *dst_dir,
                                                   CopyProgress *progress);

// ============================================================================
// Generic File Operation with Progress
// ============================================================================

int perform_file_operation_with_progress_ex(
    FileOperation op,
    const char *src_path,
    const char *dst_path,
    const char *custom_title,
    ProgressMessage *icon_metadata
) {
    if (!src_path) return -1;
    if ((op == FILE_OP_COPY || op == FILE_OP_MOVE) && !dst_path) return -1;

    // Determine if directory
    struct stat st;
    if (stat(src_path, &st) != 0) {
        log_error("[ERROR] Cannot stat: %s", src_path);
        return -1;
    }

    bool is_directory = S_ISDIR(st.st_mode);

    // Create pipe for IPC
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        log_error("[ERROR] Failed to create pipe for progress");
        // Fallback to sync operations
        switch (op) {
            case FILE_OP_COPY:
                return wb_fileops_copy(src_path, dst_path);
            case FILE_OP_MOVE: {
                char temp_dst[PATH_SIZE];
                strncpy(temp_dst, dst_path, PATH_SIZE - 1);
                temp_dst[PATH_SIZE - 1] = '\0';
                return wb_fileops_move(src_path, dirname(temp_dst), NULL, 0);
            }
            case FILE_OP_DELETE:
                return is_directory ? wb_fileops_remove_recursive(src_path) : unlink(src_path);
        }
    }

    // Set read end non-blocking
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    // Fork to perform in background
    pid_t pid = fork();
    if (pid == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        log_error("[ERROR] Fork failed");
        switch (op) {
            case FILE_OP_COPY:
                return wb_fileops_copy(src_path, dst_path);
            case FILE_OP_MOVE: {
                char temp_dst[PATH_SIZE];
                strncpy(temp_dst, dst_path, PATH_SIZE - 1);
                temp_dst[PATH_SIZE - 1] = '\0';
                return wb_fileops_move(src_path, dirname(temp_dst), NULL, 0);
            }
            case FILE_OP_DELETE:
                return is_directory ? wb_fileops_remove_recursive(src_path) : unlink(src_path);
        }
    }

    if (pid == 0) {
        // ===== CHILD PROCESS =====
        close(pipefd[0]);

        // Send START message
        ProgressMessage msg = {
            .type = MSG_START,
            .start_time = time(NULL),
            .files_done = 0,
            .files_total = -1,
            .bytes_done = 0,
            .bytes_total = is_directory ? 0 : (size_t)st.st_size
        };
        // Extract basename without memory leak
        char temp_path[PATH_SIZE];
        strncpy(temp_path, src_path, PATH_SIZE - 1);
        temp_path[PATH_SIZE - 1] = '\0';
        strncpy(msg.current_file, basename(temp_path), NAME_SIZE - 1);

        // Copy icon metadata if provided
        if (icon_metadata) {
            msg.create_icon = icon_metadata->create_icon;
            msg.has_sidecar = icon_metadata->has_sidecar;
            msg.icon_x = icon_metadata->icon_x;
            msg.icon_y = icon_metadata->icon_y;
            msg.target_window = icon_metadata->target_window;
            strncpy(msg.dest_path, icon_metadata->dest_path, PATH_SIZE - 1);
            strncpy(msg.dest_dir, icon_metadata->dest_dir, PATH_SIZE - 1);
            strncpy(msg.sidecar_src, icon_metadata->sidecar_src, PATH_SIZE - 1);
            strncpy(msg.sidecar_dst, icon_metadata->sidecar_dst, PATH_SIZE - 1);
        }

        // Send START message - if this fails, parent won't get progress updates
        // but operation will still complete
        if (send_message(pipefd[1], MSG_TYPE_FULL, &msg, sizeof(msg)) < 0) {
            log_error("[WARNING] Failed to send START message to parent");
        }

        int result = 0;

        switch (op) {
            case FILE_OP_COPY:
                if (is_directory) {
                    // Child process will count files/bytes - no blocking in parent
                    CopyProgress progress = {
                        .total_files = -1,  // Unknown until child counts
                        .files_processed = 0,
                        .total_bytes = -1,  // Unknown until child counts
                        .bytes_copied = 0,
                        .dialog = NULL,
                        .abort = false,
                        .pipe_fd = pipefd[1],
                        .last_update_time = time(NULL)
                    };
                    result = copy_directory_recursive_with_progress(src_path, dst_path, &progress);
                    // Update msg with final count for completion message
                    if (result == 0) {
                        msg.files_done = progress.files_processed;
                        msg.files_total = progress.files_processed;
                    }
                } else {
                    result = copy_file_with_progress(src_path, dst_path, pipefd[1]);
                }
                break;

            case FILE_OP_MOVE:
                if (rename(src_path, dst_path) == 0) {
                    result = 0;
                } else if (errno == EXDEV) {
                    if (is_directory) {
                        // Child process will count files/bytes - no blocking in parent
                        CopyProgress progress = {
                            .total_files = -1,  // Unknown until child counts
                            .files_processed = 0,
                            .total_bytes = -1,  // Unknown until child counts
                            .bytes_copied = 0,
                            .dialog = NULL,
                            .abort = false,
                            .pipe_fd = pipefd[1],
                            .last_update_time = time(NULL)
                        };
                        result = copy_directory_recursive_with_progress(src_path, dst_path, &progress);
                        if (result == 0) {
                            result = wb_fileops_remove_recursive(src_path);
                        }
                        // Update msg with final count for completion message
                        if (result == 0) {
                            msg.files_done = progress.files_processed;
                            msg.files_total = progress.files_processed;
                        }
                    } else {
                        result = copy_file_with_progress(src_path, dst_path, pipefd[1]);
                        if (result == 0) {
                            result = unlink(src_path);
                        }
                    }
                } else {
                    result = -1;
                }
                break;

            case FILE_OP_DELETE:
                if (is_directory) {
                    result = wb_fileops_remove_recursive(src_path);
                } else {
                    result = unlink(src_path);
                }
                break;
        }

        // Send completion message - critical for parent to know we're done
        msg.type = (result == 0) ? MSG_COMPLETE : MSG_ERROR;
        if (send_message(pipefd[1], MSG_TYPE_FULL, &msg, sizeof(msg)) < 0) {
            log_error("[ERROR] Failed to send COMPLETE message - parent may not update UI");
        }
        close(pipefd[1]);
        _exit(result);
    }

    // ===== PARENT PROCESS =====
    close(pipefd[1]);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);

    ProgressOperation prog_op = (op == FILE_OP_COPY) ? PROGRESS_COPY :
                                (op == FILE_OP_MOVE) ? PROGRESS_MOVE : PROGRESS_DELETE;

    const char *title = custom_title;
    if (!title) {
        switch (op) {
            case FILE_OP_COPY: title = "Copying Files..."; break;
            case FILE_OP_MOVE: title = "Moving Files..."; break;
            case FILE_OP_DELETE: title = "Deleting Files..."; break;
        }
    }

    ProgressDialog *dialog = calloc(1, sizeof(ProgressDialog));
    if (!dialog) {
        close(pipefd[0]);
        int status;
        waitpid(pid, &status, 0);
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

    dialog->operation = prog_op;
    dialog->pipe_fd = pipefd[0];
    dialog->child_pid = pid;
    dialog->start_time = time(NULL);
    dialog->canvas = NULL;
    dialog->percent = -1.0f;
    // Extract basename without memory leak
    char temp_path[PATH_SIZE];
    strncpy(temp_path, src_path, PATH_SIZE - 1);
    temp_path[PATH_SIZE - 1] = '\0';
    strncpy(dialog->current_file, basename(temp_path), PATH_SIZE - 1);

    add_progress_dialog_to_list(dialog);

    return 0;
}

// Wrapper for backward compatibility
int perform_file_operation_with_progress(
    FileOperation op,
    const char *src_path,
    const char *dst_path,
    const char *custom_title
) {
    return perform_file_operation_with_progress_ex(op, src_path, dst_path, custom_title, NULL);
}

// ============================================================================
// Copy Directory with Progress (Iterative Implementation)
// ============================================================================

// NOTE: This function is defined after the main API to keep related code together
// Uses shared wb_queue utility (wb_queue.c)

static int copy_directory_recursive_with_progress(const char *src_dir, const char *dst_dir,
                                                   CopyProgress *progress) {
    if (!src_dir || !dst_dir || !*src_dir || !*dst_dir) return -1;

    // Send initial message immediately so parent can show dialog after 1 second
    if (progress && progress->pipe_fd > 0) {
        ProgressUpdate initial = {
            .files_done = 0,
            .files_total = -1,  // Unknown - still counting
            .bytes_done = 0,
            .bytes_total = -1   // Unknown - still counting
        };
        send_message(progress->pipe_fd, MSG_TYPE_UPDATE, &initial, sizeof(initial));
        progress->last_update_time = time(NULL);
    }

    // Count files and bytes before starting the copy (runs in child process)
    // This allows parent to show "Calculating size..." until we send real totals
    if (progress) {
        int total_files = 0;
        off_t total_bytes = 0;
        count_files_and_bytes(src_dir, &total_files, &total_bytes);

        // Update the progress struct with real totals
        progress->total_files = total_files;
        progress->total_bytes = total_bytes;

        // Send message to parent with the real totals (after counting completes)
        if (progress->pipe_fd > 0) {
            ProgressUpdate update = {
                .files_done = 0,
                .files_total = total_files,
                .bytes_done = 0,
                .bytes_total = total_bytes
            };
            send_message(progress->pipe_fd, MSG_TYPE_UPDATE, &update, sizeof(update));
            progress->last_update_time = time(NULL);
        }
    }

    DirQueue queue;
    wb_queue_init(&queue);
    int result = 0;

    if (wb_queue_push_pair(&queue, src_dir, dst_dir) != 0) {
        wb_queue_free(&queue);
        return -1;
    }

    char *current_src;
    char *current_dst;

    while ((current_src = wb_queue_pop_pair(&queue, &current_dst)) != NULL) {
        // Check for abort
        if (progress && progress->dialog && progress->dialog->abort_requested) {
            progress->abort = true;
            result = -1;
            free(current_src);
            free(current_dst);
            break;
        }

        struct stat src_stat;
        if (stat(current_src, &src_stat) != 0 || !S_ISDIR(src_stat.st_mode)) {
            log_error("[ERROR] Not a directory: %s", current_src);
            result = -1;
            free(current_src);
            free(current_dst);
            break;
        }

        if (mkdir(current_dst, 0755) != 0) {
            struct stat dst_stat;
            if (stat(current_dst, &dst_stat) != 0 || !S_ISDIR(dst_stat.st_mode)) {
                log_error("[ERROR] Cannot create directory: %s", current_dst);
                result = -1;
                free(current_src);
                free(current_dst);
                break;
            }
        }

        // Preserve extended attributes
        wb_xattr_copy_all(current_src, current_dst);

        DIR *dir = opendir(current_src);
        if (!dir) {
            log_error("[ERROR] Cannot open directory: %s", current_src);
            result = -1;
            free(current_src);
            free(current_dst);
            break;
        }

        struct dirent *entry;
        char src_path[PATH_SIZE];
        char dst_path[PATH_SIZE];

        while ((entry = readdir(dir)) != NULL && result == 0) {
            if (progress && progress->dialog && progress->dialog->abort_requested) {
                progress->abort = true;
                result = -1;
                break;
            }

            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            snprintf(src_path, sizeof(src_path), "%s/%s", current_src, entry->d_name);
            snprintf(dst_path, sizeof(dst_path), "%s/%s", current_dst, entry->d_name);

            struct stat st;
            if (stat(src_path, &st) != 0) {
                log_error("[ERROR] Cannot stat: %s", src_path);
                result = -1;
                break;
            }

            if (S_ISDIR(st.st_mode)) {
                if (wb_queue_push_pair(&queue, src_path, dst_path) != 0) {
                    log_error("[WARNING] Failed to queue directory: %s", src_path);
                    result = -1;
                    break;
                }
            } else if (S_ISREG(st.st_mode)) {
                // Send heartbeat BEFORE copying if 1+ seconds passed
                if (progress && progress->pipe_fd > 0) {
                    time_t now = time(NULL);
                    if (now != progress->last_update_time) {
                        ProgressUpdate heartbeat = {
                            .files_done = progress->files_processed,
                            .files_total = progress->total_files,
                            .bytes_done = progress->bytes_copied,
                            .bytes_total = progress->total_bytes
                        };
                        send_message(progress->pipe_fd, MSG_TYPE_UPDATE, &heartbeat, sizeof(heartbeat));
                        progress->last_update_time = now;
                    }
                }

                // Copy file with chunked I/O to allow heartbeat during large files
                int in_fd = open(src_path, O_RDONLY);
                if (in_fd < 0) {
                    log_error("[ERROR] Cannot open source file: %s", src_path);
                    result = -1;
                    break;
                }

                int out_fd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
                if (out_fd < 0) {
                    close(in_fd);
                    log_error("[ERROR] Cannot create destination file: %s", dst_path);
                    result = -1;
                    break;
                }

                // Copy in chunks, sending heartbeat every second
                char buf[64 * 1024];  // 64KB chunks
                ssize_t bytes_read;
                int copy_failed = 0;
                while ((bytes_read = read(in_fd, buf, sizeof(buf))) > 0) {
                    ssize_t bytes_written = 0;
                    while (bytes_written < bytes_read) {
                        ssize_t n = write(out_fd, buf + bytes_written, bytes_read - bytes_written);
                        if (n < 0) {
                            copy_failed = 1;
                            break;
                        }
                        bytes_written += n;
                    }
                    if (copy_failed) break;

                    // Update byte count as we copy
                    progress->bytes_copied += bytes_read;

                    // Send heartbeat if 1 second passed
                    if (progress->pipe_fd > 0) {
                        time_t now = time(NULL);
                        if (now != progress->last_update_time) {
                            ProgressUpdate heartbeat = {
                                .files_done = progress->files_processed,
                                .files_total = progress->total_files,
                                .bytes_done = progress->bytes_copied,
                                .bytes_total = progress->total_bytes
                            };
                            send_message(progress->pipe_fd, MSG_TYPE_UPDATE, &heartbeat, sizeof(heartbeat));
                            progress->last_update_time = now;
                        }
                    }
                }

                if (bytes_read < 0 || copy_failed) {
                    close(in_fd);
                    close(out_fd);
                    log_error("[ERROR] Failed to copy file: %s to %s", src_path, dst_path);
                    result = -1;
                    break;
                }

                // Preserve permissions and close
                fchmod(out_fd, st.st_mode & 0777);
                close(in_fd);
                close(out_fd);

                // Preserve extended attributes
                wb_xattr_copy_all(src_path, dst_path);

                // File copy complete - update file count
                if (progress) {
                    progress->files_processed++;
                    // Note: bytes_copied already updated during copy loop

                    if (progress->pipe_fd > 0) {
                        // Send final update for this file
                        time_t now = time(NULL);
                        if (now != progress->last_update_time) {
                            ProgressUpdate update = {
                                .files_done = progress->files_processed,
                                .files_total = progress->total_files,
                                .bytes_done = progress->bytes_copied,
                                .bytes_total = progress->total_bytes
                            };
                            send_message(progress->pipe_fd, MSG_TYPE_UPDATE, &update, sizeof(update));
                            progress->last_update_time = now;
                        }
                    } else if (progress->dialog) {
                        float percent = (progress->total_bytes > 0) ?
                            ((float)progress->bytes_copied / progress->total_bytes * 100.0f) : 0.0f;
                        update_progress_dialog(progress->dialog, entry->d_name, percent);
                    }
                }
            }
        }

        closedir(dir);
        free(current_src);
        free(current_dst);
    }

    // Send final progress update to ensure UI shows 100%
    if (progress && progress->pipe_fd > 0 && result == 0) {
        ProgressUpdate final_update = {
            .files_done = progress->files_processed,
            .files_total = progress->total_files,
            .bytes_done = progress->bytes_copied,
            .bytes_total = progress->total_bytes
        };
        // Ignore errors - COMPLETE message will arrive anyway
        send_message(progress->pipe_fd, MSG_TYPE_UPDATE, &final_update, sizeof(final_update));
    }

    wb_queue_free(&queue);
    return result;
}

// ============================================================================
// Progress Dialog Polling (called from event loop)
// ============================================================================

void workbench_check_progress_dialogs(void) {
    extern ProgressDialog* get_all_progress_dialogs(void);  // From dialogs.c
    extern Canvas* create_progress_window(ProgressOperation op, const char *title);  // From dialogs.c
    ProgressDialog *dialog = get_all_progress_dialogs();
    time_t now = time(NULL);
    
    
    while (dialog) {
        ProgressDialog *next = dialog->next;  // Save next before potential deletion
        
        if (dialog->pipe_fd > 0) {
            // Check for messages from child using header-based protocol
            MessageHeader header;
            int header_result = read_message_header(dialog->pipe_fd, &header);

            if (header_result == 1) {
                // No data available yet - but still check timeout for window creation
                // Show dialog after 1 second even if no message received yet
                if (!dialog->canvas && now - dialog->start_time >= PROGRESS_DIALOG_THRESHOLD) {
                    const char *title = dialog->operation == PROGRESS_COPY ? "Copying Files..." :
                                      dialog->operation == PROGRESS_MOVE ? "Moving Files..." :
                                      dialog->operation == PROGRESS_DELETE ? "Deleting Files..." :
                                      dialog->operation == PROGRESS_EXTRACT ? "Extracting Archive..." :
                                      "Processing...";
                    dialog->canvas = create_progress_window(dialog->operation, title);
                    if (dialog->canvas) {
                        float percent = (dialog->percent >= 0) ? dialog->percent : 0.0f;
                        update_progress_dialog(dialog, dialog->current_file, percent);
                    }
                }
                dialog = next;
                continue;
            } else if (header_result == -1) {
                // Error reading header - pipe may be closed
                dialog = next;
                continue;
            }

            // Header valid - read payload based on type
            if (header.msg_type == MSG_TYPE_UPDATE) {
                // Lightweight progress update (24 bytes payload)
                ProgressUpdate update;
                if (read(dialog->pipe_fd, &update, sizeof(update)) != sizeof(update)) {
                    dialog = next;
                    continue;
                }

                // Mark as started when we get first message
                if (dialog->percent < 0) {
                    dialog->percent = 0.0f;
                }

                // Update file and byte counts
                dialog->files_done = update.files_done;
                dialog->files_total = update.files_total;
                dialog->bytes_done = update.bytes_done;
                dialog->bytes_total = update.bytes_total;

                // Calculate percent from BYTES (not files)
                float percent = 0.0f;
                if (update.bytes_total > 0) {
                    percent = (double)update.bytes_done / (double)update.bytes_total * 100.0;
                }

                // Create window if threshold passed
                if (!dialog->canvas && now - dialog->start_time >= PROGRESS_DIALOG_THRESHOLD) {
                    const char *title = dialog->operation == PROGRESS_COPY ? "Copying Files..." :
                                      dialog->operation == PROGRESS_MOVE ? "Moving Files..." :
                                      dialog->operation == PROGRESS_DELETE ? "Deleting Files..." :
                                      dialog->operation == PROGRESS_EXTRACT ? "Extracting Archive..." :
                                      "Processing...";
                    dialog->canvas = create_progress_window(dialog->operation, title);
                    if (dialog->canvas) {
                        update_progress_dialog(dialog, dialog->current_file, percent);
                    }
                } else if (dialog->canvas) {
                    // Update existing dialog
                    update_progress_dialog(dialog, dialog->current_file, percent);
                }

                dialog->percent = percent;

            } else if (header.msg_type == MSG_TYPE_FULL) {
                // Full message (START/PROGRESS/COMPLETE/ERROR - 2240 bytes payload)
                ProgressMessage msg;
                if (read(dialog->pipe_fd, &msg, sizeof(msg)) != sizeof(msg)) {
                    dialog = next;
                    continue;
                }

                // Mark as started when we get first message
                if (dialog->percent < 0) {
                    dialog->percent = 0.0f;  // Mark as started
                }

                if (msg.type == MSG_START) {
                    // Handle START message - DON'T update start_time from child, keep parent's original
                    // dialog->start_time = msg.start_time;  // BUG: This makes diff always 0!
                    dialog->percent = 0.0f;
                    strncpy(dialog->current_file, msg.current_file, PATH_SIZE - 1);

                    // Create window if threshold has passed
                    if (!dialog->canvas && now - dialog->start_time >= PROGRESS_DIALOG_THRESHOLD) {
                        const char *title = dialog->operation == PROGRESS_COPY ? "Copying Files..." :
                                          dialog->operation == PROGRESS_MOVE ? "Moving Files..." :
                                          dialog->operation == PROGRESS_DELETE ? "Deleting Files..." :
                                          dialog->operation == PROGRESS_EXTRACT ? "Extracting Archive..." :
                                          "Processing...";
                        dialog->canvas = create_progress_window(dialog->operation, title);
                        if (dialog->canvas) {
                            update_progress_dialog(dialog, dialog->current_file, 0.0f);
                        } else {
                            log_error("[ERROR] Failed to create progress window");
                        }
                    }
                } else if (msg.type == MSG_PROGRESS) {
                    // Calculate percent - prioritize bytes for smooth progress on large files
                    float percent = 0.0f;
                    if (msg.bytes_total > 0) {
                        percent = (float)msg.bytes_done / msg.bytes_total * 100.0f;
                    } else if (msg.files_total > 0) {
                        percent = (float)msg.files_done / msg.files_total * 100.0f;
                    }

                    // Create window if 1 second has passed and window doesn't exist
                    if (!dialog->canvas && dialog->start_time > 0) {
                        if (now - dialog->start_time >= PROGRESS_DIALOG_THRESHOLD) {
                            // Create the actual dialog window now
                            const char *title = dialog->operation == PROGRESS_COPY ? "Copying Files..." :
                                              dialog->operation == PROGRESS_MOVE ? "Moving Files..." :
                                              dialog->operation == PROGRESS_DELETE ? "Deleting Files..." :
                                              dialog->operation == PROGRESS_EXTRACT ? "Extracting Archive..." :
                                              "Processing...";
                            dialog->canvas = create_progress_window(dialog->operation, title);
                            if (dialog->canvas) {
                                update_progress_dialog(dialog, msg.current_file, percent);
                            }
                        } else {
                        }
                    } else if (dialog->canvas) {
                        // Update existing dialog
                        update_progress_dialog(dialog, msg.current_file, percent);
                    }

                    dialog->percent = percent;
                    strncpy(dialog->current_file, msg.current_file, PATH_SIZE - 1);

                } else if (msg.type == MSG_COMPLETE || msg.type == MSG_ERROR) {
                    // Operation finished
                    // Operation finished - check if we need to create icons

                    // If extraction succeeded, create icon for the extracted directory
                    if (msg.type == MSG_COMPLETE && dialog->operation == PROGRESS_EXTRACT &&
                        !msg.create_icon && strlen(msg.dest_path) > 0 && msg.target_window != None) {
                        // This is an extraction operation - create icon for the directory we extracted into
                        // This is an extraction operation - create icon for extracted directory

                        // Verify the directory was actually created
                        struct stat st;
                        if (stat(msg.dest_path, &st) == 0) {
                            // Directory was successfully created
                        } else {
                            log_error("[ERROR] Directory does not exist: %s (errno=%d: %s)",
                                     msg.dest_path, errno, strerror(errno));
                        }

                        Canvas *canvas = itn_canvas_find_by_window(msg.target_window);
                        if (canvas) {
                            // Found the target canvas for icon creation
                            // Get the directory name from the path
                            const char *dir_name = strrchr(msg.dest_path, '/');
                            dir_name = dir_name ? dir_name + 1 : msg.dest_path;
                            // Extract directory name for icon label

                            // Get the def_dir.info icon path for directories
                            const char *icon_path = wb_deficons_get_for_file(dir_name, true);
                            if (!icon_path) {
                                log_error("[ERROR] No def_dir.info available for directory icon");
                                break;
                            }
                            // Got appropriate icon for directory

                            // Find a free spot for the new directory icon
                            int new_x, new_y;
                            find_free_slot(canvas, &new_x, &new_y);
                            // Found position for new icon

                            // Create the directory icon using the proper metadata function
                            // icon_path = def_dir.info, msg.dest_path = actual directory, dir_name = label
                            // Create icon with proper metadata
                            FileIcon *new_icon = create_icon_with_metadata(icon_path, canvas, new_x, new_y,
                                                    msg.dest_path, dir_name, TYPE_DRAWER);

                            if (new_icon) {
                                // Icon created successfully - update canvas to show it
                                compute_content_bounds(canvas);
                                compute_max_scroll(canvas);
                                redraw_canvas(canvas);
                                // Canvas updated with new icon
                            } else {
                                log_error("[ERROR] Failed to create icon for extracted directory: %s", msg.dest_path);
                            }
                        } else {
                            log_error("[ERROR] Canvas not found for window 0x%lx - cannot create extracted directory icon", msg.target_window);
                        }
                    }

                    // If copy succeeded and we have icon metadata, create the icon now
                    if (msg.type == MSG_COMPLETE && msg.create_icon && strlen(msg.dest_path) > 0) {
                        // Copy sidecar if needed (small file, do synchronously)
                        if (msg.has_sidecar && strlen(msg.sidecar_src) > 0 && strlen(msg.sidecar_dst) > 0) {
                            wb_fileops_copy(msg.sidecar_src, msg.sidecar_dst);
                        }

                        // Find the target canvas by window
                        Canvas *target = NULL;
                        if (msg.target_window != None) {
                            target = itn_canvas_find_by_window(msg.target_window);
                        }

                        if (target) {
                            // Determine file type NOW (after copy is done)
                            struct stat st;
                            bool is_dir = (stat(msg.dest_path, &st) == 0 && S_ISDIR(st.st_mode));
                            int file_type = is_dir ? TYPE_DRAWER : TYPE_FILE;

                            // Get appropriate icon path
                            const char *icon_path = NULL;
                            const char *filename = strrchr(msg.dest_path, '/');
                            filename = filename ? filename + 1 : msg.dest_path;

                            if (msg.has_sidecar && strlen(msg.sidecar_dst) > 0) {
                                icon_path = msg.sidecar_dst;
                            } else {
                                icon_path = wb_deficons_get_for_file(filename, is_dir);
                            }

                            if (icon_path) {
                                // Create the icon at the specified position
                                create_icon_with_metadata(icon_path, target, msg.icon_x, msg.icon_y,
                                                        msg.dest_path, filename, file_type);

                                // Apply layout if in list view
                                if (target->view_mode == VIEW_NAMES) {
                                    apply_view_layout(target);
                                }

                                // Refresh display
                                compute_content_bounds(target);
                                compute_max_scroll(target);
                                redraw_canvas(target);
                            }
                        }
                    }
                    
                    close(dialog->pipe_fd);
                    dialog->pipe_fd = -1;
                    if (dialog->canvas) {
                        close_progress_dialog(dialog);
                    } else {
                        // No window was created, just free the structure
                        extern void remove_progress_dialog_from_list(ProgressDialog *dialog);
                        remove_progress_dialog_from_list(dialog);
                        free(dialog);
                    }
                    dialog = next;
                    continue;
                }
            }
        }
        
        // Check if we need to create progress window based on elapsed time
        // This handles the case where no messages arrive but time has passed
        if (!dialog->canvas && dialog->start_time > 0 && dialog->percent >= 0) {
            // Dialog has started (percent >= 0) but no window yet
            if (now - dialog->start_time >= PROGRESS_DIALOG_THRESHOLD) {
                
                // Determine appropriate title based on operation
                const char *title = "Processing...";
                if (dialog->operation == PROGRESS_COPY) {
                    title = "Copying Files...";
                } else if (dialog->operation == PROGRESS_MOVE) {
                    title = "Moving Files...";
                } else if (dialog->operation == PROGRESS_DELETE) {
                    title = "Deleting Files...";
                } else if (dialog->operation == PROGRESS_EXTRACT) {
                    title = "Extracting Archive...";
                }
                
                dialog->canvas = create_progress_window(dialog->operation, title);
                if (dialog->canvas) {
                    // Update with current state
                    float percent = 0.0f;
                    if (dialog->percent > 0) {
                        percent = dialog->percent;
                    }
                    update_progress_dialog(dialog, dialog->current_file, percent);
                } else {
                    log_error("[ERROR] Failed to create progress window from timer check");
                }
            }
        }
        
        // Check if child process finished
        if (dialog->child_pid > 0) {
            int status;
            pid_t wait_result = waitpid(dialog->child_pid, &status, WNOHANG);
            if (wait_result == dialog->child_pid) {
                // Child finished
                if (dialog->pipe_fd > 0) {
                    close(dialog->pipe_fd);
                    dialog->pipe_fd = -1;
                }
                if (dialog->canvas) {
                    close_progress_dialog(dialog);
                } else {
                    // No window was created, just free the structure
                    extern void remove_progress_dialog_from_list(ProgressDialog *dialog);
                    remove_progress_dialog_from_list(dialog);
                    free(dialog);
                }
                dialog = next;
                continue;
            }
        }
        
        dialog = next;
    }
}
