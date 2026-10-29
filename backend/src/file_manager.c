/*
 * file_manager.c - Safe file operations implementation
 *
 * Key C concepts demonstrated:
 *   - String manipulation with strrchr, strncpy, snprintf
 *   - Path construction and boundary checking
 *   - File existence checking with GetFileAttributesA (Windows API)
 *   - Directory creation with CreateDirectoryA
 *   - Input sanitization to prevent directory-traversal attacks
 */

#include "file_manager.h"
#include "common.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <windows.h>

/* ── Module state ────────────────────────────────────────────────────── */

static char g_download_dir[MAX_PATH] = {0};

/* ── Internal helpers ────────────────────────────────────────────────── */

/* Ensure a directory exists; create it if necessary. */
static int ensure_dir(const char *path) {
    DWORD attr = GetFileAttributesA(path);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        return 0;  /* Already exists */
    }
    if (CreateDirectoryA(path, NULL)) {
        log_msg(LOG_INFO, "Created directory: %s", path);
        return 0;
    }
    /* CreateDirectoryA fails if it already exists — that's OK */
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0;
    }
    log_msg(LOG_ERROR, "Failed to create directory: %s (error %lu)", path, GetLastError());
    return -1;
}

/* ── Public API ──────────────────────────────────────────────────────── */

int fm_init(const char *default_dir) {
    if (!default_dir || strlen(default_dir) == 0) {
        default_dir = DEFAULT_DOWNLOAD_DIR;
    }
    strncpy(g_download_dir, default_dir, MAX_PATH - 1);
    g_download_dir[MAX_PATH - 1] = '\0';

    if (ensure_dir(g_download_dir) != 0) {
        return -1;
    }
    log_msg(LOG_INFO, "File manager initialized. Download dir: %s", g_download_dir);
    return 0;
}

const char* fm_get_download_dir(void) {
    return g_download_dir;
}

int fm_set_download_dir(const char *new_dir) {
    if (!new_dir || strlen(new_dir) == 0) {
        log_msg(LOG_ERROR, "Cannot set empty download directory");
        return -1;
    }

    /* Reject paths with ".." to prevent directory traversal attacks.
     *
     * Directory traversal: an attacker sends a path like "../../etc/passwd"
     * to escape the intended directory.  We block any path containing ".." */
    if (strstr(new_dir, "..") != NULL) {
        log_msg(LOG_ERROR, "Directory path rejected (contains '..'): %s", new_dir);
        return -1;
    }

    if (strlen(new_dir) >= MAX_PATH) {
        log_msg(LOG_ERROR, "Directory path too long: %s", new_dir);
        return -1;
    }

    if (ensure_dir(new_dir) != 0) {
        return -1;
    }

    strncpy(g_download_dir, new_dir, MAX_PATH - 1);
    g_download_dir[MAX_PATH - 1] = '\0';
    log_msg(LOG_INFO, "Download directory changed to: %s", g_download_dir);
    return 0;
}

int fm_extract_filename(const char *url, char *out_name, int out_size) {
    if (!url || !out_name || out_size <= 0) return -1;

    /*
     * Strategy: find the last '/' in the URL, then take everything
     * after it up to the first '?' or '#' (query/fragment).
     *
     * Example: "https://example.com/path/file.zip?token=abc"
     *          last '/' is before "file.zip?token=abc"
     *          we copy "file.zip"
     */
    const char *last_slash = strrchr(url, '/');
    if (!last_slash || *(last_slash + 1) == '\0') {
        /* No filename in URL — use a default */
        strncpy(out_name, "download", out_size - 1);
        out_name[out_size - 1] = '\0';
        return 0;
    }

    const char *name_start = last_slash + 1;

    /* Find where the filename ends (at '?' or '#' or end of string) */
    const char *name_end = name_start;
    while (*name_end && *name_end != '?' && *name_end != '#') {
        name_end++;
    }

    int name_len = (int)(name_end - name_start);
    if (name_len <= 0) {
        strncpy(out_name, "download", out_size - 1);
        out_name[out_size - 1] = '\0';
        return 0;
    }

    if (name_len >= out_size) name_len = out_size - 1;
    strncpy(out_name, name_start, name_len);
    out_name[name_len] = '\0';

    return 0;
}

void fm_sanitize_filename(char *name) {
    if (!name) return;

    /*
     * Remove dangerous characters that could be used for:
     *   - Path traversal: / \ ..
     *   - Command injection: ; | & < > ` $
     *   - File system issues: : * ? " (illegal on Windows)
     *   - Control characters: anything < 32
     */
    char *src = name, *dst = name;
    while (*src) {
        char c = *src;
        /* Skip dangerous characters */
        if (c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' ||
            c == '|' || c == ';' || c == '&' || c == '`' ||
            c == '$' || (unsigned char)c < 32) {
            src++;
            continue;
        }
        *dst++ = c;
        src++;
    }
    *dst = '\0';

    /* Remove leading dots and spaces (e.g., ".hidden" or "  file.txt") */
    src = name;
    while (*src == '.' || *src == ' ') src++;
    if (src != name) {
        memmove(name, src, strlen(src) + 1);
    }

    /* Remove ".." sequences that might have survived */
    while (1) {
        char *dotdot = strstr(name, "..");
        if (!dotdot) break;
        memmove(dotdot, dotdot + 2, strlen(dotdot + 2) + 1);
    }

    /* If sanitization left an empty string, use a default */
    if (strlen(name) == 0) {
        strcpy(name, "download");
    }

    /* Truncate to a safe length */
    if (strlen(name) > MAX_FILENAME_LENGTH - 1) {
        name[MAX_FILENAME_LENGTH - 1] = '\0';
    }
}

int fm_build_unique_path(const char *filename, char *out_path, int out_size) {
    if (!filename || !out_path || out_size <= 0) return -1;

    /*
     * Build path: download_dir/filename
     * If it already exists, try download_dir/name_1.ext, name_2.ext, etc.
     *
     * We separate the name and extension so "report.pdf" becomes
     * "report_1.pdf" instead of "report.pdf_1".
     */
    snprintf(out_path, out_size, "%s\\%s", g_download_dir, filename);

    if (!fm_file_exists(out_path)) {
        return 0;  /* No collision */
    }

    /* Split filename into name + extension */
    char name_part[MAX_FILENAME_LENGTH] = {0};
    char ext_part[64] = {0};
    strncpy(name_part, filename, MAX_FILENAME_LENGTH - 1);

    char *dot = strrchr(name_part, '.');
    if (dot && dot != name_part) {
        strncpy(ext_part, dot, sizeof(ext_part) - 1);
        *dot = '\0';  /* Truncate name_part at the dot */
    }

    /* Try name_1.ext, name_2.ext, ... up to 999 */
    for (int i = 1; i < 1000; i++) {
        snprintf(out_path, out_size, "%s\\%s_%d%s",
                 g_download_dir, name_part, i, ext_part);
        if (!fm_file_exists(out_path)) {
            return 0;
        }
    }

    log_msg(LOG_ERROR, "Could not generate unique path for: %s", filename);
    return -1;
}

int fm_file_exists(const char *path) {
    /*
     * GetFileAttributesA returns INVALID_FILE_ATTRIBUTES if the file
     * doesn't exist.  This is more reliable than fopen() for checking
     * existence because it doesn't create/lock the file.
     */
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES &&
            !(attr & FILE_ATTRIBUTE_DIRECTORY));
}
