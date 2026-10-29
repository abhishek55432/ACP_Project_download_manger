/*
 * file_manager.h - Safe file operations interface
 *
 * Responsibilities:
 *   - Extract a filename from a URL
 *   - Sanitize filenames (remove path traversal, special chars)
 *   - Generate unique filenames to prevent overwriting
 *   - Manage the configurable download directory
 *   - Validate that a directory exists and is writable
 */

#ifndef FILE_MANAGER_H
#define FILE_MANAGER_H

/* Initialize the file manager with a default download directory.
 * Creates the directory if it doesn't exist.
 * Returns 0 on success, -1 on failure. */
int  fm_init(const char *default_dir);

/* Get the current download directory path.
 * Returns a pointer to an internal buffer — do not free. */
const char* fm_get_download_dir(void);

/* Set a new download directory.  Creates it if it doesn't exist.
 * Returns 0 on success, -1 if the path is invalid or cannot be created. */
int  fm_set_download_dir(const char *new_dir);

/* Extract a filename from a URL.
 * Example: "https://example.com/files/report.pdf?v=2" → "report.pdf"
 * The result is written into `out_name` (up to `out_size` bytes).
 * Returns 0 on success, -1 if no filename could be determined. */
int  fm_extract_filename(const char *url, char *out_name, int out_size);

/* Sanitize a filename: remove path separators, ".." sequences,
 * control characters, and leading/trailing dots/spaces.
 * Modifies `name` in place. */
void fm_sanitize_filename(char *name);

/* Build a full file path that won't overwrite existing files.
 * If "report.pdf" exists, tries "report_1.pdf", "report_2.pdf", etc.
 * Writes the result into `out_path` (up to `out_size` bytes).
 * Returns 0 on success. */
int  fm_build_unique_path(const char *filename, char *out_path, int out_size);

/* Check if a file exists at the given path.
 * Returns 1 if it exists, 0 otherwise. */
int  fm_file_exists(const char *path);

#endif /* FILE_MANAGER_H */
