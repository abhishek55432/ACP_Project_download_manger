/*
 * download_engine.h - WinINet-based download engine interface
 *
 * This module performs the actual HTTP/HTTPS file downloads using the
 * Windows Internet API (WinINet).  Each download runs in its own thread.
 *
 * Key capabilities:
 *   - Download files from HTTP/HTTPS URLs
 *   - Track progress (bytes downloaded, total size, speed, ETA)
 *   - Support pause/resume via HTTP Range headers (when server allows)
 *   - Support cancellation via a cancel flag
 *   - Detect and report network errors, HTTP errors, and file I/O errors
 */

#ifndef DOWNLOAD_ENGINE_H
#define DOWNLOAD_ENGINE_H

#include "common.h"

/* Initialize the download engine (opens a WinINet session).
 * Must be called once before any downloads.
 * Returns 0 on success, -1 on failure. */
int  engine_init(void);

/* Start downloading a task in a new thread.
 * The task must already be populated with url, filename, filepath.
 * Sets task->state to STATE_DOWNLOADING and creates a worker thread.
 * Returns 0 on success, -1 on failure. */
int  engine_start_download(DownloadTask *task);

/* Pause a running download (if the server supports Range headers).
 * Sets the cancel_flag to stop the download loop, records bytes_downloaded,
 * and sets state to STATE_PAUSED.
 * Returns 0 on success, -1 if pause is not supported or task is not downloading. */
int  engine_pause_download(DownloadTask *task);

/* Resume a paused download from where it left off.
 * Uses an HTTP Range header to request the remaining bytes.
 * Returns 0 on success, -1 on failure. */
int  engine_resume_download(DownloadTask *task);

/* Cancel a running download.
 * Sets the cancel_flag and waits for the thread to exit.
 * Sets state to STATE_CANCELLED.
 * Returns 0 on success. */
int  engine_cancel_download(DownloadTask *task);

/* Clean up the WinINet session.
 * Call once at shutdown. */
void engine_cleanup(void);

#endif /* DOWNLOAD_ENGINE_H */
