/*
 * main.c - Entry point for the Download Manager
 *
 * Startup sequence:
 *   1. Initialize logger, file manager, download engine, queue, and history
 *   2. Start the API server (blocks on accept loop)
 *   3. On shutdown: clean up all modules in reverse order
 *
 * The API server runs in the main thread. A console handler (SetConsoleCtrlHandler)
 * catches Ctrl+C to trigger a graceful shutdown.
 */

#include "common.h"
#include "logger.h"
#include "file_manager.h"
#include "download_engine.h"
#include "download_queue.h"
#include "history.h"
#include "api_server.h"
#include <stdio.h>
#include <stdlib.h>

/* ── Ctrl+C handler for graceful shutdown ────────────────────────────── */

static BOOL WINAPI console_handler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_CLOSE_EVENT) {
        log_msg(LOG_INFO, "Shutdown signal received...");
        api_server_stop();
        return TRUE;  /* We handled it */
    }
    return FALSE;
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(void) {
    printf("==============================================\n");
    printf("  Console-Based Download Manager v1.0\n");
    printf("  Backend: C with WinINet + Winsock2\n");
    printf("  API: http://127.0.0.1:%d\n", API_PORT);
    printf("==============================================\n\n");

    /* Step 1: Initialize logger */
    if (logger_init(LOG_FILE) != 0) {
        fprintf(stderr, "Failed to initialize logger\n");
        return 1;
    }

    /* Step 2: Initialize file manager */
    if (fm_init(DEFAULT_DOWNLOAD_DIR) != 0) {
        log_msg(LOG_ERROR, "Failed to initialize file manager");
        logger_cleanup();
        return 1;
    }

    /* Step 3: Initialize download engine (WinINet session) */
    if (engine_init() != 0) {
        log_msg(LOG_ERROR, "Failed to initialize download engine");
        logger_cleanup();
        return 1;
    }

    /* Step 4: Initialize download queue */
    queue_init(MAX_CONCURRENT);

    /* Step 5: Initialize history */
    history_init(HISTORY_FILE);

    /* Step 6: Register Ctrl+C handler */
    SetConsoleCtrlHandler(console_handler, TRUE);

    log_msg(LOG_INFO, "All modules initialized. Starting API server...");
    printf("Press Ctrl+C to shut down.\n\n");

    /* Step 7: Start API server (blocks here until shutdown) */
    api_server_start(API_PORT);

    /* ── Cleanup (runs after Ctrl+C) ─────────────────────────────── */
    log_msg(LOG_INFO, "Shutting down...");
    queue_cleanup();
    engine_cleanup();
    history_cleanup();
    logger_cleanup();

    printf("Download Manager stopped.\n");
    return 0;
}
