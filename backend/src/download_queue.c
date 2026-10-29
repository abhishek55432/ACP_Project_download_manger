/*
 * download_queue.c - Thread-safe download queue implementation
 *
 * Data structure: an array of pointers to dynamically allocated DownloadTask structs.
 * Why pointers? Each task's memory address remains completely constant throughout
 * its lifecycle. This prevents corruption of task CRITICAL_SECTIONs and guarantees
 * thread safety when worker threads or the API access tasks concurrently.
 *
 * Thread safety: g_queue_lock protects all queue additions, removals, and traversals.
 * Individual task fields are protected by each task's own lock.
 *
 * Key C concepts:
 *   - Dynamic memory allocation (calloc / free)
 *   - Pointer arrays for stable object identities
 *   - CRITICAL_SECTION for mutual exclusion
 *   - Waiting on worker thread termination before cleanup
 *   - Enforcing concurrency limits
 */

#include "download_queue.h"
#include "download_engine.h"
#include "file_manager.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Module state ────────────────────────────────────────────────────── */

static DownloadTask*    g_tasks[MAX_DOWNLOADS];   /* Array of task pointers  */
static int              g_task_count = 0;         /* Number of tasks in queue*/
static int              g_next_id    = 1;         /* Next task ID to assign  */
static int              g_max_concurrent = MAX_CONCURRENT;
static CRITICAL_SECTION g_queue_lock;             /* Protects all queue ops  */

/* ── Internal helpers ────────────────────────────────────────────────── */

static int count_active(void) {
    int active = 0;
    for (int i = 0; i < g_task_count; i++) {
        if (g_tasks[i] && g_tasks[i]->state == STATE_DOWNLOADING) {
            active++;
        }
    }
    return active;
}

/* ── Public API ──────────────────────────────────────────────────────── */

void queue_init(int max_concurrent) {
    InitializeCriticalSection(&g_queue_lock);
    g_task_count = 0;
    g_next_id = 1;
    g_max_concurrent = (max_concurrent > 0) ? max_concurrent : MAX_CONCURRENT;
    memset(g_tasks, 0, sizeof(g_tasks));
    log_msg(LOG_INFO, "Download queue initialized (max concurrent: %d)", g_max_concurrent);
}

int queue_add(const char *url, const char *filename) {
    if (!url || strlen(url) == 0) {
        log_msg(LOG_ERROR, "Cannot add download: empty URL");
        return -1;
    }

    EnterCriticalSection(&g_queue_lock);

    if (g_task_count >= MAX_DOWNLOADS) {
        LeaveCriticalSection(&g_queue_lock);
        log_msg(LOG_ERROR, "Queue full (%d tasks). Cannot add more.", MAX_DOWNLOADS);
        return -1;
    }

    /* Allocate task dynamically so its memory address never changes */
    DownloadTask *task = (DownloadTask *)calloc(1, sizeof(DownloadTask));
    if (!task) {
        LeaveCriticalSection(&g_queue_lock);
        log_msg(LOG_ERROR, "Failed to allocate memory for DownloadTask");
        return -1;
    }

    InitializeCriticalSection(&task->lock);

    /* Assign ID and copy URL */
    task->id = g_next_id++;
    strncpy(task->url, url, MAX_URL_LENGTH - 1);

    /* Determine filename: use provided name or extract from URL */
    char extracted_name[MAX_FILENAME_LENGTH] = {0};
    if (filename && strlen(filename) > 0) {
        strncpy(extracted_name, filename, MAX_FILENAME_LENGTH - 1);
    } else {
        fm_extract_filename(url, extracted_name, MAX_FILENAME_LENGTH);
    }

    /* Sanitize the filename to prevent path traversal */
    fm_sanitize_filename(extracted_name);
    strncpy(task->filename, extracted_name, MAX_FILENAME_LENGTH - 1);

    /* Build a unique file path (avoids overwriting existing files) */
    if (fm_build_unique_path(task->filename, task->filepath, MAX_PATH_LENGTH) != 0) {
        DeleteCriticalSection(&task->lock);
        free(task);
        LeaveCriticalSection(&g_queue_lock);
        log_msg(LOG_ERROR, "Cannot build file path for: %s", task->filename);
        return -1;
    }

    task->state = STATE_QUEUED;
    task->total_bytes = -1;  /* Unknown until download starts */
    task->created_at = time(NULL);

    g_tasks[g_task_count] = task;
    g_task_count++;
    int id = task->id;

    LeaveCriticalSection(&g_queue_lock);

    log_msg(LOG_INFO, "Queued download #%d: %s → %s", id, url, task->filepath);

    /* Try to start the download immediately if slots are available */
    queue_process();

    return id;
}

DownloadTask* queue_get_task(int id) {
    EnterCriticalSection(&g_queue_lock);
    for (int i = 0; i < g_task_count; i++) {
        if (g_tasks[i] && g_tasks[i]->id == id) {
            DownloadTask *t = g_tasks[i];
            LeaveCriticalSection(&g_queue_lock);
            return t;
        }
    }
    LeaveCriticalSection(&g_queue_lock);
    return NULL;
}

int queue_get_all(DownloadTask **out_tasks, int max_count) {
    EnterCriticalSection(&g_queue_lock);
    int count = (g_task_count < max_count) ? g_task_count : max_count;
    for (int i = 0; i < count; i++) {
        out_tasks[i] = g_tasks[i];
    }
    LeaveCriticalSection(&g_queue_lock);
    return count;
}

int queue_count(void) {
    EnterCriticalSection(&g_queue_lock);
    int count = g_task_count;
    LeaveCriticalSection(&g_queue_lock);
    return count;
}

void queue_get_stats(int *total, int *active, int *completed, int *failed) {
    EnterCriticalSection(&g_queue_lock);
    *total     = g_task_count;
    *active    = 0;
    *completed = 0;
    *failed    = 0;
    for (int i = 0; i < g_task_count; i++) {
        if (!g_tasks[i]) continue;
        switch (g_tasks[i]->state) {
            case STATE_DOWNLOADING: (*active)++;    break;
            case STATE_COMPLETED:   (*completed)++; break;
            case STATE_FAILED:      (*failed)++;    break;
            default: break;
        }
    }
    LeaveCriticalSection(&g_queue_lock);
}

int queue_remove(int id) {
    EnterCriticalSection(&g_queue_lock);

    for (int i = 0; i < g_task_count; i++) {
        if (g_tasks[i] && g_tasks[i]->id == id) {
            DownloadTask *task = g_tasks[i];
            DownloadState st = task->state;

            /* Don't remove active/queued tasks */
            if (st == STATE_DOWNLOADING || st == STATE_QUEUED) {
                LeaveCriticalSection(&g_queue_lock);
                log_msg(LOG_WARN, "Cannot remove active/queued task #%d", id);
                return -1;
            }

            /* Wait for the thread to exit before deleting locks or freeing */
            if (task->thread_handle != NULL) {
                WaitForSingleObject(task->thread_handle, 500);
                CloseHandle(task->thread_handle);
                task->thread_handle = NULL;
            }

            /* Clean up the task's CRITICAL_SECTION */
            DeleteCriticalSection(&task->lock);
            free(task);

            /*
             * Shift only pointer elements left by 1.
             * Pointer values can be moved safely without disturbing task memory.
             */
            for (int j = i; j < g_task_count - 1; j++) {
                g_tasks[j] = g_tasks[j + 1];
            }
            g_tasks[g_task_count - 1] = NULL;
            g_task_count--;

            LeaveCriticalSection(&g_queue_lock);
            log_msg(LOG_INFO, "Removed task #%d from queue", id);
            return 0;
        }
    }

    LeaveCriticalSection(&g_queue_lock);
    return -1;
}

void queue_process(void) {
    EnterCriticalSection(&g_queue_lock);

    int active = count_active();

    /* Start queued tasks until we hit the concurrency limit */
    for (int i = 0; i < g_task_count && active < g_max_concurrent; i++) {
        if (g_tasks[i] && g_tasks[i]->state == STATE_QUEUED) {
            if (engine_start_download(g_tasks[i]) == 0) {
                active++;
            }
        }
    }

    LeaveCriticalSection(&g_queue_lock);
}

int queue_retry(int id) {
    DownloadTask *task = queue_get_task(id);
    if (!task) return -1;

    TASK_LOCK(task);
    if (task->state != STATE_FAILED && task->state != STATE_CANCELLED) {
        TASK_UNLOCK(task);
        log_msg(LOG_WARN, "Cannot retry task #%d (state=%s)", id, state_to_string(task->state));
        return -1;
    }

    /* Wait for previous thread to finish if handle still exists */
    if (task->thread_handle != NULL) {
        WaitForSingleObject(task->thread_handle, 500);
        CloseHandle(task->thread_handle);
        task->thread_handle = NULL;
    }

    /* Reset the task for a fresh download */
    task->state = STATE_QUEUED;
    task->bytes_downloaded = 0;
    task->total_bytes = -1;
    task->progress = 0;
    task->speed = 0;
    task->eta = 0;
    task->cancel_flag = 0;
    task->error_msg[0] = '\0';

    /* Re-generate unique path in case old partial file exists */
    fm_build_unique_path(task->filename, task->filepath, MAX_PATH_LENGTH);

    TASK_UNLOCK(task);

    log_msg(LOG_INFO, "Retrying download #%d: %s", id, task->url);
    queue_process();
    return 0;
}

int queue_get_max_concurrent(void) {
    return g_max_concurrent;
}

void queue_set_max_concurrent(int max) {
    if (max < 1) max = 1;
    if (max > MAX_DOWNLOADS) max = MAX_DOWNLOADS;
    g_max_concurrent = max;
    log_msg(LOG_INFO, "Max concurrent downloads set to %d", max);
    queue_process();  /* May start queued tasks if we increased the limit */
}

void queue_cleanup(void) {
    EnterCriticalSection(&g_queue_lock);

    /* Cancel and clean up all tasks */
    for (int i = 0; i < g_task_count; i++) {
        if (g_tasks[i]) {
            if (g_tasks[i]->state == STATE_DOWNLOADING) {
                engine_cancel_download(g_tasks[i]);
            }
            if (g_tasks[i]->thread_handle != NULL) {
                WaitForSingleObject(g_tasks[i]->thread_handle, 500);
                CloseHandle(g_tasks[i]->thread_handle);
                g_tasks[i]->thread_handle = NULL;
            }
            DeleteCriticalSection(&g_tasks[i]->lock);
            free(g_tasks[i]);
            g_tasks[i] = NULL;
        }
    }
    g_task_count = 0;

    LeaveCriticalSection(&g_queue_lock);
    DeleteCriticalSection(&g_queue_lock);
    log_msg(LOG_INFO, "Download queue cleaned up");
}
