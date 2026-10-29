/*
 * history.c - Persistent download history using cJSON
 *
 * How it works:
 *   1. On startup, history_init() loads existing history from a JSON file.
 *   2. When a download completes (or fails/cancels), history_add() appends
 *      a new entry and writes the file to disk.
 *   3. history_get_json() returns the full history as a JSON string
 *      for the API server to send to the frontend.
 *
 * Key C concepts:
 *   - File I/O: fopen, fread, fwrite, fseek, ftell
 *   - Dynamic memory: malloc/free for reading file contents
 *   - cJSON library for building and parsing JSON
 *   - CRITICAL_SECTION for thread-safe access
 */

#include "history.h"
#include "logger.h"
#include "cjson/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Module state ────────────────────────────────────────────────────── */

static cJSON           *g_history_array = NULL;   /* In-memory JSON array     */
static char             g_filepath[MAX_PATH] = {0};
static CRITICAL_SECTION g_history_lock;

/* ── Internal: read entire file into a string ────────────────────────── */

/*
 * Reads a file into a heap-allocated string.
 * The caller must free() the returned pointer.
 *
 * Algorithm:
 *   1. fseek to end to get file size (ftell)
 *   2. Rewind to start
 *   3. malloc(size + 1) for the content plus null terminator
 *   4. fread the entire file
 *   5. Null-terminate
 */
static char* read_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (size <= 0) {
        fclose(fp);
        return NULL;
    }

    char *data = (char *)malloc(size + 1);
    if (!data) {
        fclose(fp);
        return NULL;
    }

    fread(data, 1, size, fp);
    data[size] = '\0';
    fclose(fp);
    return data;
}

/* ── Public API ──────────────────────────────────────────────────────── */

int history_init(const char *filepath) {
    InitializeCriticalSection(&g_history_lock);
    strncpy(g_filepath, filepath, MAX_PATH - 1);

    /* Try to load existing history */
    char *data = read_file(filepath);
    if (data) {
        g_history_array = cJSON_Parse(data);
        free(data);
    }

    /* If no valid history found, start with an empty array */
    if (!g_history_array || !cJSON_IsArray(g_history_array)) {
        if (g_history_array) cJSON_Delete(g_history_array);
        g_history_array = cJSON_CreateArray();
    }

    log_msg(LOG_INFO, "History loaded: %d entries from %s",
            cJSON_GetArraySize(g_history_array), filepath);
    return 0;
}

void history_add(const DownloadTask *task) {
    if (!task || !g_history_array) return;

    EnterCriticalSection(&g_history_lock);

    /*
     * Build a JSON object for this history entry.
     * cJSON_CreateObject() creates an empty {} object.
     * cJSON_AddStringToObject() adds a key-value pair like "url": "...".
     * cJSON_AddNumberToObject() adds a numeric pair like "size": 12345.
     */
    cJSON *entry = cJSON_CreateObject();
    cJSON_AddNumberToObject(entry, "id", task->id);
    cJSON_AddStringToObject(entry, "url", task->url);
    cJSON_AddStringToObject(entry, "filename", task->filename);
    cJSON_AddStringToObject(entry, "filepath", task->filepath);
    cJSON_AddStringToObject(entry, "status", state_to_string(task->state));
    cJSON_AddNumberToObject(entry, "size", (double)task->bytes_downloaded);
    cJSON_AddNumberToObject(entry, "created_at", (double)task->created_at);
    cJSON_AddNumberToObject(entry, "completed_at", (double)task->completed_at);
    if (task->error_msg[0] != '\0') {
        cJSON_AddStringToObject(entry, "error", task->error_msg);
    }

    cJSON_AddItemToArray(g_history_array, entry);

    LeaveCriticalSection(&g_history_lock);

    history_save();
    log_msg(LOG_INFO, "Added to history: #%d %s (%s)",
            task->id, task->filename, state_to_string(task->state));
}

char* history_get_json(void) {
    EnterCriticalSection(&g_history_lock);
    char *json = cJSON_PrintUnformatted(g_history_array);
    LeaveCriticalSection(&g_history_lock);
    return json;  /* Caller must free() */
}

void history_clear(void) {
    EnterCriticalSection(&g_history_lock);

    cJSON_Delete(g_history_array);
    g_history_array = cJSON_CreateArray();

    /* Delete the file on disk */
    remove(g_filepath);

    LeaveCriticalSection(&g_history_lock);
    log_msg(LOG_INFO, "History cleared");
}

void history_save(void) {
    EnterCriticalSection(&g_history_lock);

    char *json = cJSON_Print(g_history_array);  /* Pretty-printed */
    if (json) {
        FILE *fp = fopen(g_filepath, "w");
        if (fp) {
            fputs(json, fp);
            fclose(fp);
        } else {
            log_msg(LOG_ERROR, "Failed to write history file: %s", g_filepath);
        }
        free(json);  /* cJSON_Print allocates with malloc, must free */
    }

    LeaveCriticalSection(&g_history_lock);
}

void history_cleanup(void) {
    history_save();
    EnterCriticalSection(&g_history_lock);
    if (g_history_array) {
        cJSON_Delete(g_history_array);
        g_history_array = NULL;
    }
    LeaveCriticalSection(&g_history_lock);
    DeleteCriticalSection(&g_history_lock);
}
