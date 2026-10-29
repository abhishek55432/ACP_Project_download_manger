/*
 * common.h - Shared definitions for the Download Manager
 *
 * This header defines the core data structures and constants used by every
 * module in the C backend.  The central type is `DownloadTask`, which holds
 * all metadata for a single download: URL, local file path, progress, speed,
 * status, and thread-safety primitives.
 *
 * Key concepts for viva:
 *   - `enum` for finite state machines (task states)
 *   - Fixed-size char arrays vs. heap strings (trade-off: simplicity vs. flexibility)
 *   - CRITICAL_SECTION is the Windows equivalent of pthread_mutex_t
 *   - volatile tells the compiler not to cache the value in a register,
 *     important for variables shared between threads
 */

#ifndef COMMON_H
#define COMMON_H

/* MinGW 6.3.0 compatibility:
 * _WIN32_WINNT >= 0x0600 enables Vista+ APIs like GetTickCount64
 * __USE_MINGW_ANSI_STDIO enables %lld in printf on MinGW */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#ifndef __USE_MINGW_ANSI_STDIO
#define __USE_MINGW_ANSI_STDIO 1
#endif

#include <windows.h>
#include <stdint.h>
#include <time.h>

/* ── Constants ───────────────────────────────────────────────────────── */

#define MAX_URL_LENGTH       2048
#define MAX_FILENAME_LENGTH  256
#define MAX_PATH_LENGTH      512
#define MAX_DOWNLOADS        100   /* Maximum simultaneous tasks in memory   */
#define MAX_CONCURRENT       3     /* Default parallel download threads      */
#define READ_BUFFER_SIZE     8192  /* Bytes per InternetReadFile call        */
#define API_PORT             8080
#define HISTORY_FILE         "history.json"
#define SETTINGS_FILE        "settings.json"
#define LOG_FILE             "download_manager.log"
#define DEFAULT_DOWNLOAD_DIR "downloads"

/* ── Download task states ────────────────────────────────────────────── */

/*
 * State machine for a download task:
 *
 *   QUEUED ──► DOWNLOADING ──► COMPLETED
 *                  │               
 *                  ├──► PAUSED ──► DOWNLOADING (resume)
 *                  │
 *                  ├──► FAILED ──► QUEUED (retry)
 *                  │
 *                  └──► CANCELLED
 */
typedef enum {
    STATE_QUEUED,       /* Waiting in queue to start                      */
    STATE_DOWNLOADING,  /* Currently transferring data                    */
    STATE_PAUSED,       /* Paused by user; can be resumed if server allows*/
    STATE_COMPLETED,    /* Download finished successfully                 */
    STATE_FAILED,       /* Download failed due to an error                */
    STATE_CANCELLED     /* Cancelled by user                              */
} DownloadState;

/* ── Download task structure ─────────────────────────────────────────── */

/*
 * Each download is represented by a DownloadTask struct.
 * 
 * Memory layout:
 *   - Fixed-size buffers for URL, filename, filepath, error: no malloc/free
 *     needed, but limits max length.  Good trade-off for a student project.
 *   - `id` is a simple incrementing integer (not a UUID).
 *   - `lock` (CRITICAL_SECTION) protects concurrent access to this task's
 *     mutable fields (state, bytes_downloaded, speed, etc.).
 *   - `cancel_flag` is checked in the download loop; setting it to 1
 *     causes the loop to exit gracefully.
 */
typedef struct {
    int                 id;                          /* Unique task ID             */
    char                url[MAX_URL_LENGTH];         /* Source URL                 */
    char                filename[MAX_FILENAME_LENGTH]; /* Local filename           */
    char                filepath[MAX_PATH_LENGTH];   /* Full local path            */
    char                error_msg[256];              /* Last error message         */

    volatile DownloadState state;                    /* Current task state         */
    volatile int64_t    bytes_downloaded;            /* Bytes received so far      */
    volatile int64_t    total_bytes;                 /* Total file size (-1 if unknown) */
    volatile double     speed;                       /* Bytes per second           */
    volatile double     progress;                    /* 0.0 to 100.0 percent      */
    volatile double     eta;                         /* Estimated seconds remaining*/
    volatile int        cancel_flag;                 /* 1 = cancel requested       */
    volatile int        supports_resume;             /* 1 = server supports Range  */

    time_t              created_at;                  /* When the task was queued    */
    time_t              completed_at;                /* When it finished           */

    CRITICAL_SECTION    lock;                        /* Per-task mutex             */
    HANDLE              thread_handle;               /* Worker thread handle       */
} DownloadTask;

/* ── Utility macros ──────────────────────────────────────────────────── */

/* Thread-safe field access helpers */
#define TASK_LOCK(task)   EnterCriticalSection(&(task)->lock)
#define TASK_UNLOCK(task) LeaveCriticalSection(&(task)->lock)

/* Convert download state enum to a human-readable string */
static inline const char* state_to_string(DownloadState s) {
    switch (s) {
        case STATE_QUEUED:      return "queued";
        case STATE_DOWNLOADING: return "downloading";
        case STATE_PAUSED:      return "paused";
        case STATE_COMPLETED:   return "completed";
        case STATE_FAILED:      return "failed";
        case STATE_CANCELLED:   return "cancelled";
        default:                return "unknown";
    }
}

#endif /* COMMON_H */
