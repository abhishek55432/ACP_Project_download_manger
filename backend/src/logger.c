/*
 * logger.c - Thread-safe logging implementation
 *
 * How it works:
 *   1. logger_init() opens a file for appending and creates a CRITICAL_SECTION.
 *   2. log_msg() acquires the lock, formats the timestamp + level + message,
 *      writes to both stdout and the file, then releases the lock.
 *   3. logger_cleanup() closes the file and destroys the lock.
 *
 * Key C concepts:
 *   - va_list / va_start / va_end for variadic printf-style functions
 *   - CRITICAL_SECTION for mutual exclusion on Windows
 *   - time() + localtime() for wall-clock timestamps
 *   - fprintf to multiple output streams
 */

#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <windows.h>

/* ── Module-level state ──────────────────────────────────────────────── */

static FILE             *g_log_file  = NULL;   /* Log file handle              */
static CRITICAL_SECTION  g_log_lock;           /* Protects concurrent writes   */
static int               g_initialized = 0;    /* Guard against double init    */

/* ── Severity label lookup ───────────────────────────────────────────── */

static const char *level_strings[] = {
    "DEBUG", "INFO", "WARN", "ERROR"
};

/* ── Public API ──────────────────────────────────────────────────────── */

int logger_init(const char *log_filepath) {
    if (g_initialized) return 0;  /* Already initialized */

    g_log_file = fopen(log_filepath, "a");  /* Append mode */
    if (!g_log_file) {
        fprintf(stderr, "[LOGGER] Failed to open log file: %s\n", log_filepath);
        return -1;
    }

    InitializeCriticalSection(&g_log_lock);
    g_initialized = 1;

    log_msg(LOG_INFO, "Logger initialized. Log file: %s", log_filepath);
    return 0;
}

void log_msg(LogLevel level, const char *fmt, ...) {
    if (!g_initialized) {
        /* Fallback: print to stderr if logger not initialized yet */
        va_list args;
        va_start(args, fmt);
        fprintf(stderr, "[%s] ", level_strings[level]);
        vfprintf(stderr, fmt, args);
        fprintf(stderr, "\n");
        va_end(args);
        return;
    }

    EnterCriticalSection(&g_log_lock);

    /* Get current time and format as YYYY-MM-DD HH:MM:SS */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);

    /*
     * Build the full message using va_list.
     *
     * va_start() initializes the argument list pointer.
     * vfprintf() writes formatted output using the va_list.
     * va_end() cleans up the argument list.
     *
     * We call va_start/va_end twice because after vfprintf consumes
     * the va_list, its state is undefined — we must reinitialize it.
     */
    va_list args;

    /* Write to stdout */
    va_start(args, fmt);
    printf("[%s] [%s] ", time_buf, level_strings[level]);
    vprintf(fmt, args);
    printf("\n");
    va_end(args);

    /* Write to log file */
    va_start(args, fmt);
    fprintf(g_log_file, "[%s] [%s] ", time_buf, level_strings[level]);
    vfprintf(g_log_file, fmt, args);
    fprintf(g_log_file, "\n");
    fflush(g_log_file);   /* Flush immediately so logs survive a crash */
    va_end(args);

    LeaveCriticalSection(&g_log_lock);
}

void logger_cleanup(void) {
    if (!g_initialized) return;

    log_msg(LOG_INFO, "Logger shutting down.");

    if (g_log_file) {
        fflush(g_log_file);
        fclose(g_log_file);
        g_log_file = NULL;
    }

    DeleteCriticalSection(&g_log_lock);
    g_initialized = 0;
}
