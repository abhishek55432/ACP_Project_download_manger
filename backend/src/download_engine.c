/*
 * download_engine.c - WinINet-based HTTP/HTTPS download engine
 *
 * This is the heart of the download manager.  It uses the Windows Internet
 * API (WinINet) to perform HTTP/HTTPS downloads.
 *
 * Architecture:
 *   1. engine_init() opens an HINTERNET session (like opening a browser).
 *   2. engine_start_download() creates a new Windows thread for each task.
 *   3. The worker thread (download_thread_proc):
 *      a. Opens the URL with InternetOpenUrlA
 *      b. Queries the HTTP status code and Content-Length
 *      c. Checks for Accept-Ranges support (for pause/resume)
 *      d. Opens a local file and reads data in a loop with InternetReadFile
 *      e. Updates progress, speed, and ETA after each read
 *      f. Checks the cancel_flag each iteration to support cancellation
 *   4. engine_pause_download() sets the cancel flag and records the position.
 *   5. engine_resume_download() re-opens the URL with a Range header.
 *
 * Key C concepts demonstrated:
 *   - Windows thread creation with CreateThread
 *   - HINTERNET handles and WinINet API calls
 *   - File I/O with fopen/fwrite/fclose
 *   - Pointer arithmetic for buffer management
 *   - Volatile variables for cross-thread communication
 *   - GetTickCount64 for high-resolution timing
 */

#include "download_engine.h"
#include "logger.h"
#include "file_manager.h"
#include <stdio.h>
#include <string.h>
#include <wininet.h>

/* ── Module state ────────────────────────────────────────────────────── */

/*
 * g_inet is the "internet session" handle.  Think of it as an invisible
 * browser window that all downloads share.  We create it once in
 * engine_init() and close it in engine_cleanup().
 */
static HINTERNET g_inet = NULL;

/* ── Internal: query HTTP response header ────────────────────────────── */

/*
 * Query a single HTTP response header string from an open URL handle.
 * `info_level` is a WinINet constant like HTTP_QUERY_CONTENT_LENGTH.
 * Returns the value in `buf` (up to `buf_size` bytes), or empty string on failure.
 */
static void query_header(HINTERNET hUrl, DWORD info_level, char *buf, DWORD buf_size) {
    DWORD index = 0;
    buf[0] = '\0';
    HttpQueryInfoA(hUrl, info_level, buf, &buf_size, &index);
}

/* ── Internal: check if server supports byte-range requests ──────────── */

/*
 * Servers that support pause/resume send an "Accept-Ranges: bytes" header.
 * We check for this after opening the URL.
 */
static int check_range_support(HINTERNET hUrl) {
    char buf[128] = {0};
    query_header(hUrl, HTTP_QUERY_ACCEPT_RANGES, buf, sizeof(buf));
    return (strstr(buf, "bytes") != NULL) ? 1 : 0;
}

/* ── Internal: get Content-Length ─────────────────────────────────────── */

static int64_t get_content_length(HINTERNET hUrl) {
    char buf[64] = {0};
    query_header(hUrl, HTTP_QUERY_CONTENT_LENGTH, buf, sizeof(buf));
    if (buf[0] == '\0') return -1;  /* Unknown size */

    int64_t len = 0;
    /*
     * Manual string-to-int64 conversion because atoi only handles int,
     * and _atoi64 is MSVC-specific.  This works on MinGW.
     */
    for (int i = 0; buf[i] >= '0' && buf[i] <= '9'; i++) {
        len = len * 10 + (buf[i] - '0');
    }
    return len;
}

/* ── Internal: get HTTP status code ──────────────────────────────────── */

static int get_status_code(HINTERNET hUrl) {
    DWORD status = 0;
    DWORD size = sizeof(status);
    DWORD index = 0;
    if (HttpQueryInfoA(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                       &status, &size, &index)) {
        return (int)status;
    }
    return 0;
}

/* ── Internal: the download worker thread ────────────────────────────── */

/*
 * This function runs in a separate thread for each download.
 * It receives a pointer to the DownloadTask as its parameter.
 *
 * DWORD WINAPI is the standard signature for Windows thread functions:
 *   - DWORD is the return type (unsigned 32-bit integer)
 *   - WINAPI specifies the calling convention (__stdcall on x86)
 *   - LPVOID is a generic pointer (void*) for passing arbitrary data
 */
static DWORD WINAPI download_thread_proc(LPVOID param) {
    DownloadTask *task = (DownloadTask *)param;

    log_msg(LOG_INFO, "Download #%d started: %s", task->id, task->url);

    /* ── Step 1: Open the URL ─────────────────────────────────────── */

    /*
     * If we're resuming, add a Range header to request only the
     * remaining bytes.  The format is "Range: bytes=START-\r\n".
     */
    char range_header[128] = {0};
    int  is_resume = (task->bytes_downloaded > 0);

    HINTERNET hUrl;
    if (is_resume) {
        snprintf(range_header, sizeof(range_header),
                 "Range: bytes=%I64d-\r\n", (long long)task->bytes_downloaded);
        log_msg(LOG_INFO, "Download #%d resuming from byte %I64d",
                task->id, (long long)task->bytes_downloaded);
    }

    /*
     * InternetOpenUrlA sends the HTTP request and returns a handle.
     * Flags:
     *   INTERNET_FLAG_RELOAD       — don't use cached version
     *   INTERNET_FLAG_NO_CACHE_WRITE — don't write to WinINet cache
     *   INTERNET_FLAG_KEEP_CONNECTION — use HTTP keep-alive
     */
    hUrl = InternetOpenUrlA(
        g_inet,
        task->url,
        is_resume ? range_header : NULL,  /* Additional headers */
        is_resume ? (DWORD)strlen(range_header) : 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_KEEP_CONNECTION,
        0
    );

    if (!hUrl) {
        TASK_LOCK(task);
        task->state = STATE_FAILED;
        snprintf(task->error_msg, sizeof(task->error_msg),
                 "Failed to open URL (WinINet error: %lu)", GetLastError());
        TASK_UNLOCK(task);
        log_msg(LOG_ERROR, "Download #%d: %s", task->id, task->error_msg);
        return 1;
    }

    /* ── Step 2: Check HTTP status code ───────────────────────────── */

    int status_code = get_status_code(hUrl);
    log_msg(LOG_INFO, "Download #%d: HTTP status %d", task->id, status_code);

    /*
     * 200 = OK (full content)
     * 206 = Partial Content (resume successful)
     * Anything else is an error.
     */
    if (status_code != 200 && status_code != 206) {
        TASK_LOCK(task);
        task->state = STATE_FAILED;
        snprintf(task->error_msg, sizeof(task->error_msg),
                 "HTTP error: %d", status_code);
        TASK_UNLOCK(task);
        InternetCloseHandle(hUrl);
        log_msg(LOG_ERROR, "Download #%d: HTTP %d", task->id, status_code);
        return 1;
    }

    /* ── Step 3: Get file size and range support ──────────────────── */

    int64_t content_length = get_content_length(hUrl);

    TASK_LOCK(task);
    if (!is_resume) {
        task->total_bytes = content_length;
    } else if (content_length > 0) {
        /* For resumed downloads, Content-Length is the remaining bytes */
        task->total_bytes = task->bytes_downloaded + content_length;
    }
    task->supports_resume = check_range_support(hUrl);
    TASK_UNLOCK(task);

    log_msg(LOG_INFO, "Download #%d: total=%lld bytes, resume=%s",
            task->id, (long long)task->total_bytes,
            task->supports_resume ? "yes" : "no");

    /* ── Step 4: Open the local file ──────────────────────────────── */

    /*
     * "ab" = append binary mode (for resume)
     * "wb" = write binary mode (fresh download)
     * Binary mode is critical: without it, \n gets converted to \r\n
     * on Windows, corrupting binary files.
     */
    FILE *fp = fopen(task->filepath, is_resume ? "ab" : "wb");
    if (!fp) {
        TASK_LOCK(task);
        task->state = STATE_FAILED;
        snprintf(task->error_msg, sizeof(task->error_msg),
                 "Cannot create file: %s", task->filepath);
        TASK_UNLOCK(task);
        InternetCloseHandle(hUrl);
        log_msg(LOG_ERROR, "Download #%d: %s", task->id, task->error_msg);
        return 1;
    }

    /* ── Step 5: Read loop ────────────────────────────────────────── */

    /*
     * The core download loop:
     *   1. Read a chunk of data from the internet (InternetReadFile)
     *   2. Write it to the local file
     *   3. Update progress metrics
     *   4. Check if the user cancelled
     *
     * InternetReadFile works like fread — it fills a buffer and tells
     * you how many bytes were actually read.  When it returns 0 bytes,
     * the transfer is complete.
     */
    BYTE buffer[READ_BUFFER_SIZE];
    DWORD bytes_read = 0;
    DWORD start_tick = GetTickCount();
    DWORD last_tick  = start_tick;
    int64_t   last_bytes = task->bytes_downloaded;

    while (1) {
        /* Check cancellation flag before each read */
        if (task->cancel_flag) {
            log_msg(LOG_INFO, "Download #%d: cancel flag detected", task->id);
            break;
        }

        /*
         * InternetReadFile: reads data from the open URL.
         * Parameters:
         *   hUrl       — handle from InternetOpenUrlA
         *   buffer     — destination buffer
         *   sizeof(buffer) — max bytes to read
         *   &bytes_read — actual bytes read (output)
         *
         * Returns FALSE on error, TRUE on success.
         * bytes_read == 0 means end of data.
         */
        if (!InternetReadFile(hUrl, buffer, sizeof(buffer), &bytes_read)) {
            TASK_LOCK(task);
            task->state = STATE_FAILED;
            snprintf(task->error_msg, sizeof(task->error_msg),
                     "Network read error (WinINet error: %lu)", GetLastError());
            TASK_UNLOCK(task);
            log_msg(LOG_ERROR, "Download #%d: %s", task->id, task->error_msg);
            break;
        }

        if (bytes_read == 0) {
            /* Transfer complete */
            log_msg(LOG_INFO, "Download #%d: transfer complete", task->id);
            break;
        }

        /* Write received data to the local file */
        size_t written = fwrite(buffer, 1, bytes_read, fp);
        if (written != bytes_read) {
            TASK_LOCK(task);
            task->state = STATE_FAILED;
            snprintf(task->error_msg, sizeof(task->error_msg),
                     "File write error at byte %I64d", (long long)task->bytes_downloaded);
            TASK_UNLOCK(task);
            log_msg(LOG_ERROR, "Download #%d: %s", task->id, task->error_msg);
            break;
        }

        /* ── Update progress metrics ─────────────────────────────── */

        TASK_LOCK(task);
        task->bytes_downloaded += bytes_read;

        /*
         * Speed calculation:
         *   We measure elapsed time since the last speed update.
         *   If at least 500ms have passed, we recalculate:
         *     speed = (bytes since last update) / (seconds elapsed)
         *
         * Using GetTickCount() for millisecond-precision timing.
         * It returns the number of milliseconds since system start.
         */
        DWORD now_tick = GetTickCount();
        DWORD elapsed_ms = now_tick - last_tick;

        if (elapsed_ms >= 500) {  /* Update speed every 500ms */
            double elapsed_sec = (double)elapsed_ms / 1000.0;
            int64_t bytes_delta = task->bytes_downloaded - last_bytes;
            task->speed = (double)bytes_delta / elapsed_sec;

            last_tick  = now_tick;
            last_bytes = task->bytes_downloaded;
        }

        /* Progress percentage */
        if (task->total_bytes > 0) {
            task->progress = ((double)task->bytes_downloaded / (double)task->total_bytes) * 100.0;

            /* ETA = remaining bytes / current speed */
            if (task->speed > 0) {
                double remaining = (double)(task->total_bytes - task->bytes_downloaded);
                task->eta = remaining / task->speed;
            }
        }
        TASK_UNLOCK(task);
    }

    /* ── Step 6: Finalize ─────────────────────────────────────────── */

    fclose(fp);
    InternetCloseHandle(hUrl);

    TASK_LOCK(task);
    if (task->cancel_flag) {
        /* If cancelled while STATE_DOWNLOADING, keep existing state set by caller.
         * The caller (pause or cancel) sets the appropriate state. */
        if (task->state == STATE_DOWNLOADING) {
            task->state = STATE_CANCELLED;
        }
    } else if (task->state == STATE_DOWNLOADING) {
        /* Normal completion */
        task->state = STATE_COMPLETED;
        task->progress = 100.0;
        task->completed_at = time(NULL);
        task->speed = 0;
        task->eta = 0;
        log_msg(LOG_INFO, "Download #%d completed: %s (%lld bytes)",
                task->id, task->filename, (long long)task->bytes_downloaded);
    }
    TASK_UNLOCK(task);

    return 0;
}

/* ── Public API ──────────────────────────────────────────────────────── */

int engine_init(void) {
    /*
     * InternetOpenA creates the top-level WinINet session.
     * Parameters:
     *   "DownloadManager/1.0" — User-Agent string (identifies our app)
     *   INTERNET_OPEN_TYPE_PRECONFIG — use system proxy settings
     *   NULL, NULL — no explicit proxy
     *   0 — no special flags
     */
    g_inet = InternetOpenA(
        "DownloadManager/1.0",
        INTERNET_OPEN_TYPE_PRECONFIG,
        NULL, NULL, 0
    );
    if (!g_inet) {
        log_msg(LOG_ERROR, "Failed to initialize WinINet (error: %lu)", GetLastError());
        return -1;
    }
    log_msg(LOG_INFO, "Download engine initialized (WinINet session open)");
    return 0;
}

int engine_start_download(DownloadTask *task) {
    if (!task) return -1;
    if (!g_inet) {
        log_msg(LOG_ERROR, "Engine not initialized");
        return -1;
    }

    TASK_LOCK(task);
    task->state = STATE_DOWNLOADING;
    task->cancel_flag = 0;
    task->error_msg[0] = '\0';
    TASK_UNLOCK(task);

    /*
     * CreateThread: creates a new OS-level thread.
     * Parameters:
     *   NULL              — default security attributes
     *   0                 — default stack size
     *   download_thread_proc — the function to run
     *   task              — parameter passed to the function (our task pointer)
     *   0                 — run immediately (no CREATE_SUSPENDED)
     *   NULL              — we don't need the thread ID
     *
     * Returns a HANDLE used to manage the thread (wait, close, etc.).
     */
    task->thread_handle = CreateThread(
        NULL, 0,
        download_thread_proc,
        task,   /* The thread receives a pointer to its task */
        0, NULL
    );

    if (!task->thread_handle) {
        TASK_LOCK(task);
        task->state = STATE_FAILED;
        snprintf(task->error_msg, sizeof(task->error_msg),
                 "Failed to create download thread (error: %lu)", GetLastError());
        TASK_UNLOCK(task);
        log_msg(LOG_ERROR, "Download #%d: %s", task->id, task->error_msg);
        return -1;
    }

    return 0;
}

int engine_pause_download(DownloadTask *task) {
    if (!task) return -1;

    TASK_LOCK(task);

    if (task->state != STATE_DOWNLOADING) {
        TASK_UNLOCK(task);
        log_msg(LOG_WARN, "Download #%d: cannot pause (state=%s)",
                task->id, state_to_string(task->state));
        return -1;
    }

    if (!task->supports_resume) {
        TASK_UNLOCK(task);
        log_msg(LOG_WARN, "Download #%d: server does not support resume", task->id);
        return -1;
    }

    /* Set the cancel flag to stop the read loop */
    task->cancel_flag = 1;
    task->state = STATE_PAUSED;
    task->speed = 0;
    task->eta = 0;
    TASK_UNLOCK(task);

    /* Wait for the download thread to actually exit */
    if (task->thread_handle) {
        WaitForSingleObject(task->thread_handle, 5000);  /* 5 second timeout */
        CloseHandle(task->thread_handle);
        task->thread_handle = NULL;
    }

    log_msg(LOG_INFO, "Download #%d paused at byte %lld",
            task->id, (long long)task->bytes_downloaded);
    return 0;
}

int engine_resume_download(DownloadTask *task) {
    if (!task) return -1;

    TASK_LOCK(task);
    if (task->state != STATE_PAUSED) {
        TASK_UNLOCK(task);
        log_msg(LOG_WARN, "Download #%d: cannot resume (state=%s)",
                task->id, state_to_string(task->state));
        return -1;
    }

    /* Reset cancel flag and set state back to downloading */
    task->cancel_flag = 0;
    task->state = STATE_DOWNLOADING;
    task->error_msg[0] = '\0';
    TASK_UNLOCK(task);

    /* Start a new thread — it will see bytes_downloaded > 0 and send Range header */
    task->thread_handle = CreateThread(NULL, 0, download_thread_proc, task, 0, NULL);
    if (!task->thread_handle) {
        TASK_LOCK(task);
        task->state = STATE_FAILED;
        snprintf(task->error_msg, sizeof(task->error_msg),
                 "Failed to create resume thread");
        TASK_UNLOCK(task);
        return -1;
    }

    log_msg(LOG_INFO, "Download #%d resumed from byte %lld",
            task->id, (long long)task->bytes_downloaded);
    return 0;
}

int engine_cancel_download(DownloadTask *task) {
    if (!task) return -1;

    TASK_LOCK(task);

    if (task->state != STATE_DOWNLOADING && task->state != STATE_PAUSED &&
        task->state != STATE_QUEUED) {
        TASK_UNLOCK(task);
        return 0;  /* Already in a terminal state */
    }

    task->cancel_flag = 1;
    task->state = STATE_CANCELLED;
    task->speed = 0;
    task->eta = 0;
    TASK_UNLOCK(task);

    /* Wait for the thread to exit */
    if (task->thread_handle) {
        WaitForSingleObject(task->thread_handle, 5000);
        CloseHandle(task->thread_handle);
        task->thread_handle = NULL;
    }

    log_msg(LOG_INFO, "Download #%d cancelled", task->id);
    return 0;
}

void engine_cleanup(void) {
    if (g_inet) {
        InternetCloseHandle(g_inet);
        g_inet = NULL;
    }
    log_msg(LOG_INFO, "Download engine shut down");
}
