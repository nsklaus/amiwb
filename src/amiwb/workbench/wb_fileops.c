// File: wb_fileops.c
// File Operations - copy, move, delete with recursive directory support

#define _POSIX_C_SOURCE 200809L
#include "wb_internal.h"
#include "wb_queue.h"
#include "wb_xattr.h"
#include "../config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <sys/xattr.h>
#include <libgen.h>


// ============================================================================
// Helper Functions
// ============================================================================

// Cleanup file descriptors helper
static void cleanup_file_descriptors(int in_fd, int out_fd) {
    if (out_fd >= 0) close(out_fd);
    if (in_fd >= 0) close(in_fd);
}

// ============================================================================
// Basic File Operations
// ============================================================================

// Copy regular file (basic version without progress)
int wb_fileops_copy(const char *src, const char *dst) {
    int in_fd = -1, out_fd = -1;
    struct stat st;

    // Check source
    if (stat(src, &st) != 0 || !S_ISREG(st.st_mode)) {
        return -1;
    }

    // Open source
    in_fd = open(src, O_RDONLY);
    if (in_fd < 0) {
        return -1;
    }

    // Create destination
    out_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (out_fd < 0) {
        cleanup_file_descriptors(in_fd, -1);
        return -1;
    }

    // Copy contents
    char buf[1 << 16];
    ssize_t r;
    while ((r = read(in_fd, buf, sizeof(buf))) > 0) {
        char *p = buf;
        ssize_t remaining = r;
        while (remaining > 0) {
            ssize_t w = write(out_fd, p, remaining);
            if (w < 0) {
                cleanup_file_descriptors(in_fd, out_fd);
                return -1;
            }
            p += w;
            remaining -= w;
        }
    }

    if (r < 0) {
        cleanup_file_descriptors(in_fd, out_fd);
        return -1;
    }

    // Preserve permissions
    fchmod(out_fd, st.st_mode & 0777);
    cleanup_file_descriptors(in_fd, out_fd);

    // Preserve extended attributes
    wb_xattr_copy_all(src, dst);

    return 0;
}

// Count files in directory tree (iterative)
// Count files in directory (exported for wb_progress.c)
void count_files_in_directory(const char *path, int *count) {
    if (!path || !count) return;

    DirQueue queue;
    wb_queue_init(&queue);

    if (wb_queue_push(&queue, path) != 0) {
        wb_queue_free(&queue);
        return;
    }

    char *current_path;
    while ((current_path = wb_queue_pop(&queue)) != NULL) {
        DIR *dir = opendir(current_path);
        if (!dir) {
            free(current_path);
            continue;
        }

        struct dirent *entry;
        char full_path[PATH_SIZE];

        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            snprintf(full_path, sizeof(full_path), "%s/%s", current_path, entry->d_name);

            struct stat st;
            if (stat(full_path, &st) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    if (wb_queue_push(&queue, full_path) != 0) {
                        log_error("[WARNING] count_files: Failed to queue %s", full_path);
                    }
                } else if (S_ISREG(st.st_mode)) {
                    (*count)++;
                }
            }
        }
        closedir(dir);
        free(current_path);
    }

    wb_queue_free(&queue);
}

// Count files AND total bytes in directory tree (iterative)
void count_files_and_bytes(const char *path, int *file_count, off_t *total_bytes) {
    if (!path || !file_count || !total_bytes) return;

    DirQueue queue;
    wb_queue_init(&queue);

    if (wb_queue_push(&queue, path) != 0) {
        wb_queue_free(&queue);
        return;
    }

    char *current_path;
    while ((current_path = wb_queue_pop(&queue)) != NULL) {
        DIR *dir = opendir(current_path);
        if (!dir) {
            free(current_path);
            continue;
        }

        struct dirent *entry;
        char full_path[PATH_SIZE];

        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            snprintf(full_path, sizeof(full_path), "%s/%s", current_path, entry->d_name);

            struct stat st;
            if (stat(full_path, &st) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    if (wb_queue_push(&queue, full_path) != 0) {
                        log_error("[WARNING] count_files_and_bytes: Failed to queue %s", full_path);
                    }
                } else if (S_ISREG(st.st_mode)) {
                    (*file_count)++;
                    *total_bytes += st.st_size;
                }
            }
        }
        closedir(dir);
        free(current_path);
    }

    wb_queue_free(&queue);
}

// Remove file or directory recursively
int wb_fileops_remove_recursive(const char *path) {
    if (!path || !*path) return -1;

    struct stat st;
    if (stat(path, &st) != 0) return -1;

    // If it's a file, just unlink it
    if (!S_ISDIR(st.st_mode)) {
        return unlink(path);
    }

    // It's a directory - remove recursively using queue
    DirQueue queue;
    wb_queue_init(&queue);
    int result = 0;

    if (wb_queue_push(&queue, path) != 0) {
        wb_queue_free(&queue);
        return -1;
    }

    // Collect all subdirectories first
    char **dirs = NULL;
    int dir_count = 0;
    int dir_capacity = 32;
    dirs = malloc(dir_capacity * sizeof(char*));
    if (!dirs) {
        wb_queue_free(&queue);
        return -1;
    }

    char *current_path;
    while ((current_path = wb_queue_pop(&queue)) != NULL) {
        DIR *dir = opendir(current_path);
        if (!dir) {
            free(current_path);
            result = -1;
            break;
        }

        // Save directory for later removal
        if (dir_count >= dir_capacity) {
            dir_capacity *= 2;
            char **new_dirs = realloc(dirs, dir_capacity * sizeof(char*));
            if (!new_dirs) {
                closedir(dir);
                free(current_path);
                result = -1;
                break;
            }
            dirs = new_dirs;
        }
        dirs[dir_count++] = current_path;

        struct dirent *entry;
        char full_path[PATH_SIZE];

        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            snprintf(full_path, sizeof(full_path), "%s/%s", current_path, entry->d_name);

            struct stat st_entry;
            if (lstat(full_path, &st_entry) == 0) {
                if (S_ISDIR(st_entry.st_mode)) {
                    if (wb_queue_push(&queue, full_path) != 0) {
                        result = -1;
                        break;
                    }
                } else {
                    if (unlink(full_path) != 0) {
                        result = -1;
                    }
                }
            }
        }
        closedir(dir);

        if (result != 0) break;
    }

    // Remove directories in reverse order (children first)
    for (int i = dir_count - 1; i >= 0 && result == 0; i--) {
        if (rmdir(dirs[i]) != 0) {
            result = -1;
        }
        free(dirs[i]);
    }

    // Free remaining paths if we broke early
    while ((current_path = wb_queue_pop(&queue)) != NULL) {
        free(current_path);
    }

    free(dirs);
    wb_queue_free(&queue);
    return result;
}

// ============================================================================
// Move Operations
// ============================================================================

// Extended move with icon creation metadata
int wb_fileops_move_ex(const char *src_path, const char *dst_dir,
                       char *dst_path, size_t dst_sz,
                       Canvas *target_canvas, int icon_x, int icon_y) {
    if (!src_path || !dst_dir || !dst_path || !*src_path || !*dst_dir) return -1;

    if (!wb_fileops_is_directory(dst_dir)) {
        errno = ENOTDIR;
        return -1;
    }

    // Build destination path
    const char *base = strrchr(src_path, '/');
    base = base ? base + 1 : src_path;
    snprintf(dst_path, dst_sz, "%s/%s", dst_dir, base);

    // If source and destination are identical, do nothing
    if (strcmp(src_path, dst_path) == 0) return 0;

    // Check if source is directory
    struct stat st_src;
    bool is_src_dir = (stat(src_path, &st_src) == 0 && S_ISDIR(st_src.st_mode));

    // Clear destination path
    if (!is_src_dir) {
        unlink(dst_path);
    } else {
        rmdir(dst_path);
    }

    if (rename(src_path, dst_path) != 0) {
        if (errno == EXDEV) {
            // Cross-filesystem move - delegate to progress system
            // Note: This creates a circular dependency wb_fileops -> wb_progress
            // The icon metadata struct is defined in wb_progress (ProgressMessage)
            // We pass it as void* to avoid circular header dependencies

            // For now, just return error code 2 to signal async operation
            // The caller (wb_drag.c) will handle the async operation
            return 2;
        } else {
            perror("[amiwb] rename (move) failed");
            return -1;
        }
    }
    return 0;
}

// Basic move without icon metadata
int wb_fileops_move(const char *src_path, const char *dst_dir,
                    char *dst_path, size_t dst_sz) {
    int result = wb_fileops_move_ex(src_path, dst_dir, dst_path, dst_sz, NULL, 0, 0);
    return (result == 2) ? 0 : result;
}

// ============================================================================
// Utility Functions (Public API)
// ============================================================================

bool wb_fileops_is_directory(const char *path) {
    if (!path || !*path) return false;
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

bool wb_fileops_check_exists(const char *path) {
    if (!path || !*path) return false;
    struct stat st;
    return stat(path, &st) == 0;
}

// ============================================================================
// Directory Size Calculation (async with IPC)
// ============================================================================

pid_t calculate_directory_size(const char *path, int *pipe_fd) {
    if (!path || !pipe_fd) {
        log_error("[ERROR] calculate_directory_size: NULL parameters");
        return -1;
    }
    
    // Create pipe for communication
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        log_error("[ERROR] Failed to create pipe for directory size calculation: %s", strerror(errno));
        return -1;
    }
    
    pid_t pid = fork();
    if (pid == -1) {
        log_error("[ERROR] Failed to fork for directory size calculation: %s", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    
    if (pid == 0) {
        // Child process - calculate directory size
        close(pipefd[0]); // Close read end
        
        // Calculate total size recursively
        off_t total_size = 0;
        
        // Use a stack-based approach to avoid deep recursion
        struct dir_entry {
            char path[PATH_SIZE];
            struct dir_entry *next;
        };
        
        struct dir_entry *stack = malloc(sizeof(struct dir_entry));
        if (!stack) {
            log_error("[ERROR] Failed to allocate memory in child process");
            _exit(1);
        }
        
        snprintf(stack->path, PATH_SIZE, "%s", path);
        stack->next = NULL;
        
        while (stack) {
            // Pop from stack
            struct dir_entry *current = stack;
            stack = stack->next;
            
            DIR *dir = opendir(current->path);
            if (!dir) {
                free(current);
                continue;
            }
            
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                // Skip . and ..
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                    continue;
                }
                
                // Build full path safely
                char full_path[PATH_SIZE];
                int written = snprintf(full_path, sizeof(full_path), "%s/%s", current->path, entry->d_name);
                
                // Check if path was truncated
                if (written >= PATH_SIZE) {
                    // Path too long, skip this entry
                    continue;
                }
                
                struct stat st;
                if (lstat(full_path, &st) == 0) {
                    if (S_ISREG(st.st_mode)) {
                        // Regular file - add its size
                        total_size += st.st_size;
                        #ifdef DEBUG_SIZE_CALC
                        log_error("[SIZE_CALC] %s: %ld bytes (total now: %ld)",
                                entry->d_name, (long)st.st_size, (long)total_size);
                        #endif
                    } else if (S_ISDIR(st.st_mode)) {
                        // Directory - push to stack for processing
                        struct dir_entry *new_entry = malloc(sizeof(struct dir_entry));
                        if (new_entry) {
                            snprintf(new_entry->path, PATH_SIZE, "%s", full_path);
                            new_entry->next = stack;
                            stack = new_entry;
                        }
                    }
                    // Skip other file types (symlinks, devices, etc.)
                }
            }
            
            closedir(dir);
            free(current);
        }

        #ifdef DEBUG_SIZE_CALC
        log_error("[SIZE_CALC] Final total size: %ld bytes (%.2f MB)",
                (long)total_size, (double)total_size / (1024.0 * 1024.0));
        #endif

        // Write result to pipe
        if (write(pipefd[1], &total_size, sizeof(total_size)) != sizeof(total_size)) {
            log_error("[ERROR] Failed to write size to pipe");
        }
        
        close(pipefd[1]);
        _exit(0);
    }
    
    // Parent process
    close(pipefd[1]); // Close write end
    *pipe_fd = pipefd[0]; // Return read end
    
    // Make pipe non-blocking
    int flags = fcntl(*pipe_fd, F_GETFL, 0);
    fcntl(*pipe_fd, F_SETFL, flags | O_NONBLOCK);
    
    return pid;
}

// Read directory size result from pipe (non-blocking)
// Returns -1 if not ready yet, otherwise returns size
off_t read_directory_size_result(int pipe_fd) {
    if (pipe_fd < 0) {
        return -1;
    }
    
    off_t size;
    ssize_t bytes_read = read(pipe_fd, &size, sizeof(size));
    
    if (bytes_read == sizeof(size)) {
        close(pipe_fd);
        return size;
    } else if (bytes_read == 0) {
        // End of pipe - child finished but no data
        close(pipe_fd);
        log_error("[WARNING] Directory size calculation completed with no data");
        return 0;
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // Not ready yet
        return -1;
    } else {
        // Error
        log_error("[ERROR] Failed to read from pipe: %s", strerror(errno));
        close(pipe_fd);
        return 0;
    }
}

// ============================================================================
// Device Stats Calculation (async with IPC)
// ============================================================================

// Calculate device statistics asynchronously (fork+pipe pattern)
// For tmpfs: reads /proc/meminfo + counts files in ramdisk
// For regular drives: uses statvfs()
// Returns pid of child process, writes pipe_fd for non-blocking read
pid_t calculate_device_stats(const char *mount_point, const char *fs_type, int *pipe_fd) {
    if (!mount_point || !fs_type || !pipe_fd) {
        log_error("[ERROR] calculate_device_stats: NULL parameters");
        return -1;
    }

    // Create pipe for communication
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        log_error("[ERROR] Failed to create pipe for device stats calculation: %s", strerror(errno));
        return -1;
    }

    pid_t pid = fork();
    if (pid == -1) {
        log_error("[ERROR] Failed to fork for device stats calculation: %s", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        // Child process - calculate device stats
        close(pipefd[0]); // Close read end

        DeviceStats stats = {0};

        // Special case: tmpfs (RAM disk) - dynamic total capacity
        if (strcmp(fs_type, "tmpfs") == 0) {
            // Read /proc/meminfo to get MemAvailable (matches menubar RAM display)
            FILE *fp = fopen("/proc/meminfo", "r");
            if (fp) {
                char line[256];
                unsigned long mem_available_kb = 0;

                while (fgets(line, sizeof(line), fp)) {
                    if (sscanf(line, "MemAvailable: %lu kB", &mem_available_kb) == 1) {
                        break;
                    }
                }
                fclose(fp);

                // Total: available system RAM (dynamic capacity)
                stats.total_bytes = (off_t)mem_available_kb * 1024;

                // Used: actual bytes in ramdisk directory
                int file_count = 0;
                off_t used_bytes = 0;
                count_files_and_bytes(mount_point, &file_count, &used_bytes);

                // Free: total - used
                stats.free_bytes = stats.total_bytes - used_bytes;
            }
        } else {
            // Regular drives: use statvfs() for filesystem stats
            struct statvfs vfs;
            if (statvfs(mount_point, &vfs) == 0) {
                stats.total_bytes = (off_t)vfs.f_blocks * (off_t)vfs.f_frsize;
                stats.free_bytes = (off_t)vfs.f_bavail * (off_t)vfs.f_frsize;
            }
        }

        // Write result to pipe
        if (write(pipefd[1], &stats, sizeof(stats)) != sizeof(stats)) {
            log_error("[ERROR] Failed to write device stats to pipe");
        }

        close(pipefd[1]);
        _exit(0);
    }

    // Parent process
    close(pipefd[1]); // Close write end
    *pipe_fd = pipefd[0]; // Return read end

    // Make pipe non-blocking
    int flags = fcntl(*pipe_fd, F_GETFL, 0);
    fcntl(*pipe_fd, F_SETFL, flags | O_NONBLOCK);

    return pid;
}

// Read device stats result from pipe (non-blocking)
// Returns true if data ready and stats filled, false if not ready yet
bool read_device_stats_result(int pipe_fd, DeviceStats *stats) {
    if (pipe_fd < 0 || !stats) {
        return false;
    }

    ssize_t bytes_read = read(pipe_fd, stats, sizeof(DeviceStats));

    if (bytes_read == sizeof(DeviceStats)) {
        // Success - data ready
        close(pipe_fd);
        return true;
    } else if (bytes_read == 0) {
        // End of pipe - child finished but no data
        close(pipe_fd);
        log_error("[WARNING] Device stats calculation completed with no data");
        return false;
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // Not ready yet - keep pipe open
        return false;
    } else {
        // Error
        log_error("[ERROR] Failed to read device stats from pipe: %s", strerror(errno));
        close(pipe_fd);
        return false;
    }
}
