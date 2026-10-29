/*
 * download_queue.h - Thread-safe download queue interface
 *
 * Manages a fixed-size array of DownloadTask pointers.  The queue:
 *   - Assigns unique IDs to new tasks
 *   - Limits the number of concurrent active downloads
 *   - Provides lookup by ID
 *   - Processes queued tasks when active slots become available
 *   - Is protected by a CRITICAL_SECTION for thread safety
 */

#ifndef DOWNLOAD_QUEUE_H
#define DOWNLOAD_QUEUE_H

#include "common.h"

/* Initialize the queue.  Must be called once at startup.
 * `max_concurrent` sets how many downloads run simultaneously. */
void queue_init(int max_concurrent);

/* Add a new download task.  The URL is required; filename is optional
 * (extracted from URL if NULL or empty).
 * Returns the task ID on success, -1 on failure. */
int  queue_add(const char *url, const char *filename);

/* Look up a task by its ID.
 * Returns a pointer to the task, or NULL if not found.
 * The caller must NOT free the returned pointer. */
DownloadTask* queue_get_task(int id);

/* Get all tasks.  Writes task pointers into `out_tasks` array
 * (up to `max_count` entries).  Returns the actual count. */
int  queue_get_all(DownloadTask **out_tasks, int max_count);

/* Get the current number of tasks in the queue (all states). */
int  queue_count(void);

/* Get statistics: total, active (downloading), completed, failed. */
void queue_get_stats(int *total, int *active, int *completed, int *failed);

/* Remove a completed/cancelled/failed task from the queue.
 * Does NOT remove active or queued tasks.
 * Returns 0 on success, -1 if not found or cannot be removed. */
int  queue_remove(int id);

/* Check the queue and start queued tasks if active slots are available.
 * Should be called periodically (e.g., after a download completes). */
void queue_process(void);

/* Retry a failed download.  Resets its state to QUEUED and
 * clears error info.
 * Returns 0 on success, -1 on failure. */
int  queue_retry(int id);

/* Get/set the maximum concurrent downloads. */
int  queue_get_max_concurrent(void);
void queue_set_max_concurrent(int max);

/* Clean up all tasks and free resources. */
void queue_cleanup(void);

#endif /* DOWNLOAD_QUEUE_H */
