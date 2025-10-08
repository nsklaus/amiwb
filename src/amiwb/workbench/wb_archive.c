// File: wb_archive.c
// Archive Extraction - handles various archive formats

#define _POSIX_C_SOURCE 200809L
#include "wb_internal.h"
#include "../config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>

// ============================================================================
// Archive Detection
// ============================================================================

// Check if file is archive based on extension
__attribute__((unused))
static bool is_archive_file(const char *path) {
    if (!path) return false;
    
    const char *ext = strrchr(path, '.');
    if (!ext) return false;
    ext++;
    
    const char *archive_exts[] = {
        "lha", "lzh", "zip", "tar", "gz", "tgz", "bz2", "tbz",
        "xz", "txz", "rar", "7z", NULL
    };
    
    for (int i = 0; archive_exts[i]; i++) {
        if (strcasecmp(ext, archive_exts[i]) == 0) {
            return true;
        }
    }
    
    // Compound extensions
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    if (strstr(name, ".tar.gz") || strstr(name, ".tar.bz2") || strstr(name, ".tar.xz")) {
        return true;
    }
    
    return false;
}

// ============================================================================
// Archive Extraction
// ============================================================================

int extract_file_at_path(const char *archive_path, Canvas *canvas) {
    if (!archive_path) {
        log_error("[ERROR] extract_file_at_path: NULL archive path");
        return -1;
    }
    
    struct stat st;
    if (stat(archive_path, &st) != 0) {
        log_error("[ERROR] Archive file not found: %s", archive_path);
        return -1;
    }
    
    // Get directory and filename
    char dir_path[PATH_SIZE];
    char archive_name[NAME_SIZE];
    
    const char *last_slash = strrchr(archive_path, '/');
    if (last_slash) {
        size_t dir_len = last_slash - archive_path;
        if (dir_len >= PATH_SIZE) dir_len = PATH_SIZE - 1;
        strncpy(dir_path, archive_path, dir_len);
        dir_path[dir_len] = '\0';
        strncpy(archive_name, last_slash + 1, NAME_SIZE - 1);
    } else {
        snprintf(dir_path, sizeof(dir_path), ".");
        strncpy(archive_name, archive_path, NAME_SIZE - 1);
    }
    archive_name[NAME_SIZE - 1] = '\0';
    
    // Get base name
    char base_name[NAME_SIZE];
    strncpy(base_name, archive_name, NAME_SIZE - 1);
    base_name[NAME_SIZE - 1] = '\0';
    
    char *ext = strstr(base_name, ".tar.");
    if (ext) {
        *ext = '\0';
    } else {
        ext = strrchr(base_name, '.');
        if (ext) *ext = '\0';
    }
    
    // Build target directory
    char target_dir[PATH_SIZE];
    
    size_t dir_len = strlen(dir_path);
    size_t base_len = strlen(base_name);
    
    if (dir_len + 1 + base_len >= PATH_SIZE) {
        log_error("[ERROR] Path too long for extraction");
        return -1;
    }
    
    int written = snprintf(target_dir, PATH_SIZE, "%s/%s", dir_path, base_name);
    if (written >= PATH_SIZE) {
        log_error("[ERROR] Path truncated");
        return -1;
    }
    
    // Handle existing directory with copy_ prefix
    int copy_num = 1;
    while (stat(target_dir, &st) == 0 && copy_num < 100) {
        size_t prefix_len = 5 * copy_num;  // "copy_" per iteration
        
        if (dir_len + 1 + prefix_len + base_len >= PATH_SIZE) {
            log_error("[ERROR] Too many copies, path too long");
            return -1;
        }
        
        char prefix[PATH_SIZE] = "";  // Max 99 * 5 = 495 bytes for "copy_" prefixes
        for (int i = 0; i < copy_num; i++) {
            strcat(prefix, "copy_");
        }
        
        written = snprintf(target_dir, PATH_SIZE, "%s/%s%s", dir_path, prefix, base_name);
        if (written >= PATH_SIZE) {
            log_error("[ERROR] Path truncated with prefix");
            return -1;
        }
        copy_num++;
    }
    
    // Create target directory
    if (mkdir(target_dir, 0755) != 0) {
        log_error("[ERROR] mkdir failed for %s", target_dir);
        return -1;
    }
    
    // Determine extraction command
    const char *archive_ext = strrchr(archive_path, '.');
    if (!archive_ext) {
        log_error("[ERROR] No extension in archive path");
        rmdir(target_dir);
        return -1;
    }
    archive_ext++;
    
    char *extract_cmd = NULL;
    
    if (strcasecmp(archive_ext, "lha") == 0 || strcasecmp(archive_ext, "lzh") == 0) {
        extract_cmd = "lha";
    } else if (strcasecmp(archive_ext, "zip") == 0) {
        extract_cmd = "unzip";
    } else if (strcasecmp(archive_ext, "rar") == 0) {
        extract_cmd = "unrar";
    } else if (strcasecmp(archive_ext, "7z") == 0) {
        extract_cmd = "7z";
    } else if (strstr(archive_name, ".tar.gz") || strstr(archive_name, ".tgz")) {
        extract_cmd = "tar";
    } else if (strstr(archive_name, ".tar.bz2") || strstr(archive_name, ".tbz")) {
        extract_cmd = "tar";
    } else if (strstr(archive_name, ".tar.xz") || strstr(archive_name, ".txz")) {
        extract_cmd = "tar";
    } else if (strcasecmp(archive_ext, "tar") == 0) {
        extract_cmd = "tar";
    } else if (strcasecmp(archive_ext, "gz") == 0) {
        extract_cmd = "gzip";
    } else if (strcasecmp(archive_ext, "bz2") == 0) {
        extract_cmd = "bzip2";
    } else if (strcasecmp(archive_ext, "xz") == 0) {
        extract_cmd = "xz";
    } else {
        log_error("[ERROR] Unsupported archive format: %s", archive_ext);
        rmdir(target_dir);
        return -1;
    }
    
    // Create pipe for progress
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        log_error("[ERROR] pipe failed");
        rmdir(target_dir);
        return -1;
    }
    
    // Fork extraction process
    pid_t pid = fork();
    if (pid == -1) {
        log_error("[ERROR] fork failed");
        close(pipefd[0]);
        close(pipefd[1]);
        rmdir(target_dir);
        return -1;
    }
    
    if (pid == 0) {
        // Child process
        close(pipefd[0]);
        
        if (chdir(target_dir) != 0) {
            _exit(1);
        }
        
        // Execute extraction
        if (strcmp(extract_cmd, "lha") == 0) {
            execl("/usr/bin/lha", "lha", "xw", archive_path, NULL);
        } else if (strcmp(extract_cmd, "unzip") == 0) {
            execl("/usr/bin/unzip", "unzip", "-q", archive_path, NULL);
        } else if (strcmp(extract_cmd, "unrar") == 0) {
            execl("/usr/bin/unrar", "unrar", "x", "-o+", archive_path, NULL);
        } else if (strcmp(extract_cmd, "7z") == 0) {
            execl("/usr/bin/7z", "7z", "x", "-y", archive_path, NULL);
        } else if (strcmp(extract_cmd, "tar") == 0) {
            if (strstr(archive_name, ".tar.gz") || strstr(archive_name, ".tgz")) {
                execl("/usr/bin/tar", "tar", "xzf", archive_path, NULL);
            } else if (strstr(archive_name, ".tar.bz2") || strstr(archive_name, ".tbz")) {
                execl("/usr/bin/tar", "tar", "xjf", archive_path, NULL);
            } else if (strstr(archive_name, ".tar.xz") || strstr(archive_name, ".txz")) {
                execl("/usr/bin/tar", "tar", "xJf", archive_path, NULL);
            } else {
                execl("/usr/bin/tar", "tar", "xf", archive_path, NULL);
            }
        }
        
        _exit(1);
    }
    
    // Parent process
    close(pipefd[1]);
    
    // Create background progress monitor (no UI initially, monitored via polling)
    ProgressMonitor *monitor = wb_progress_monitor_create_background(
        PROGRESS_EXTRACT, archive_name, pipefd[0], pid);
    if (!monitor) {
        log_error("[ERROR] Failed to create background progress monitor");
        close(pipefd[0]);
        int status;
        waitpid(pid, &status, 0);
        return -1;
    }
    
    return 0;
}
