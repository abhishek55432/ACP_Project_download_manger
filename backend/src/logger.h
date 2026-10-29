/*
 * logger.h - Thread-safe logging interface
 *
 * Provides timestamped log messages to both stdout and a log file.
 * Each message is prefixed with a severity level (INFO, WARN, ERROR, DEBUG)
 * and a timestamp in ISO-8601 format.
 *
 * Thread safety:
 *   A CRITICAL_SECTION protects the file write so that messages from
 *   concurrent download threads don't interleave.
 */

#ifndef LOGGER_H
#define LOGGER_H

typedef enum {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
} LogLevel;

/* Initialize the logger; opens the log file and creates the mutex.
 * Must be called once at startup before any log_msg() calls.
 * Returns 0 on success, -1 on failure. */
int  logger_init(const char *log_filepath);

/* Write a log message.  `fmt` is a printf-style format string.
 * The message is written to both stdout and the log file. */
void log_msg(LogLevel level, const char *fmt, ...);

/* Flush and close the log file; destroy the mutex. */
void logger_cleanup(void);

#endif /* LOGGER_H */
