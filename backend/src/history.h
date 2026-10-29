/*
 * history.h - Persistent download history interface
 *
 * Saves completed/failed/cancelled downloads to a JSON file so the user
 * can review past downloads after restarting the application.
 */

#ifndef HISTORY_H
#define HISTORY_H

#include "common.h"

/* Initialize the history module.  Loads existing history from the JSON file.
 * Returns 0 on success. */
int  history_init(const char *filepath);

/* Add a completed task to the history.
 * Copies the relevant fields from the task struct. */
void history_add(const DownloadTask *task);

/* Get all history entries as a cJSON array string.
 * The caller must free the returned string with free().
 * Returns NULL on failure. */
char* history_get_json(void);

/* Clear all history entries and delete the JSON file. */
void history_clear(void);

/* Save the current history to disk.
 * Called automatically by history_add, but can be called manually. */
void history_save(void);

/* Clean up resources. */
void history_cleanup(void);

#endif /* HISTORY_H */
