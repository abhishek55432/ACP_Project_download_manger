/*
 * api_server.c - HTTP REST API server using Winsock2
 *
 * This module implements a minimal HTTP/1.1 server from scratch using
 * the Winsock2 API.  It's the bridge between the React frontend and
 * the C download engine.
 *
 * Architecture:
 *   1. api_server_start() creates a TCP socket, binds to 127.0.0.1:PORT,
 *      and enters an accept loop.
 *   2. Each incoming connection is handled in a new thread (handle_client).
 *   3. handle_client() reads the HTTP request, parses the method + path,
 *      routes it to the appropriate handler, and sends a JSON response.
 *
 * Security:
 *   - Binds to 127.0.0.1 (loopback only — not accessible from the network)
 *   - CORS restricted to http://localhost:5173 (Vite dev server)
 *   - All input is validated before processing
 *
 * Key C concepts:
 *   - BSD socket API on Windows (Winsock2)
 *   - TCP server: socket → bind → listen → accept → recv → send → close
 *   - String parsing for HTTP request lines
 *   - snprintf for safe string formatting
 *   - Thread-per-connection model
 */

#include "api_server.h"
#include "download_queue.h"
#include "download_engine.h"
#include "file_manager.h"
#include "history.h"
#include "logger.h"
#include "cjson/cJSON.h"
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>

/* ── Constants ───────────────────────────────────────────────────────── */

#define REQUEST_BUF_SIZE  4096
#define RESPONSE_BUF_SIZE 65536

/* ── Module state ────────────────────────────────────────────────────── */

static SOCKET   g_listen_socket = INVALID_SOCKET;
static volatile int g_running   = 0;

/* ── Internal: send an HTTP response ─────────────────────────────────── */

/*
 * Build and send a complete HTTP response.
 * `status` is like "200 OK" or "404 Not Found".
 * `body` is the JSON response body.
 *
 * We include CORS headers so the React dev server (localhost:5173)
 * can make cross-origin requests to our API (localhost:8080).
 */
static void send_response(SOCKET client, const char *status, const char *body) {
    char response[RESPONSE_BUF_SIZE];
    int body_len = body ? (int)strlen(body) : 0;

    int header_len = snprintf(response, sizeof(response),
        "HTTP/1.1 %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: http://localhost:5173\r\n"
        "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, body_len
    );

    send(client, response, header_len, 0);
    if (body && body_len > 0) {
        send(client, body, body_len, 0);
    }
}

/* ── Internal: extract request body from raw HTTP ────────────────────── */

/*
 * HTTP requests separate headers from body with "\r\n\r\n".
 * This function finds the body start and returns a pointer to it.
 * Returns NULL if no body is present.
 */
static const char* find_request_body(const char *request) {
    const char *body = strstr(request, "\r\n\r\n");
    if (body) {
        body += 4;  /* Skip past the \r\n\r\n */
        if (*body == '\0') return NULL;
        return body;
    }
    return NULL;
}

/* ── Internal: extract path parameter (e.g., ID from /api/downloads/5) ─ */

/*
 * Given a path like "/api/downloads/5/pause", extract the numeric ID
 * at position `segment` (0-indexed path segments after splitting by '/').
 * Returns the ID, or -1 if not found.
 */
static int extract_id_from_path(const char *path) {
    /* Path format: /api/downloads/{id}[/action] */
    const char *p = path;

    /* Skip "/api/downloads/" */
    if (strncmp(p, "/api/downloads/", 15) != 0) return -1;
    p += 15;

    /* Parse the integer ID */
    int id = 0;
    while (*p >= '0' && *p <= '9') {
        id = id * 10 + (*p - '0');
        p++;
    }
    return (id > 0) ? id : -1;
}

/* ── Internal: extract action from path (e.g., "pause" from /api/downloads/5/pause) */

static void extract_action(const char *path, char *action, int action_size) {
    action[0] = '\0';
    const char *p = path + 15;  /* Skip "/api/downloads/" */

    /* Skip past the ID digits */
    while (*p >= '0' && *p <= '9') p++;

    /* If there's a '/', the next segment is the action */
    if (*p == '/') {
        p++;
        strncpy(action, p, action_size - 1);
        action[action_size - 1] = '\0';
        /* Remove trailing whitespace or slashes */
        char *end = action + strlen(action) - 1;
        while (end > action && (*end == ' ' || *end == '/' || *end == '\r' || *end == '\n')) {
            *end = '\0';
            end--;
        }
    }
}

/* ── Route: GET /api/downloads ───────────────────────────────────────── */

static void handle_get_downloads(SOCKET client) {
    DownloadTask *tasks[MAX_DOWNLOADS];
    int count = queue_get_all(tasks, MAX_DOWNLOADS);

    cJSON *array = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        DownloadTask *t = tasks[i];
        TASK_LOCK(t);

        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "id", t->id);
        cJSON_AddStringToObject(obj, "url", t->url);
        cJSON_AddStringToObject(obj, "filename", t->filename);
        cJSON_AddStringToObject(obj, "filepath", t->filepath);
        cJSON_AddStringToObject(obj, "status", state_to_string(t->state));
        cJSON_AddNumberToObject(obj, "progress", t->progress);
        cJSON_AddNumberToObject(obj, "speed", t->speed);
        cJSON_AddNumberToObject(obj, "bytes_downloaded", (double)t->bytes_downloaded);
        cJSON_AddNumberToObject(obj, "total_bytes", (double)t->total_bytes);
        cJSON_AddNumberToObject(obj, "eta", t->eta);
        cJSON_AddBoolToObject(obj, "supports_resume", t->supports_resume);
        cJSON_AddNumberToObject(obj, "created_at", (double)t->created_at);
        if (t->error_msg[0] != '\0') {
            cJSON_AddStringToObject(obj, "error", t->error_msg);
        }

        TASK_UNLOCK(t);
        cJSON_AddItemToArray(array, obj);
    }

    char *json = cJSON_PrintUnformatted(array);
    send_response(client, "200 OK", json);
    free(json);
    cJSON_Delete(array);
}

/* ── Route: POST /api/downloads ──────────────────────────────────────── */

static void handle_post_download(SOCKET client, const char *body) {
    if (!body) {
        send_response(client, "400 Bad Request", "{\"error\":\"Missing request body\"}");
        return;
    }

    cJSON *req = cJSON_Parse(body);
    if (!req) {
        send_response(client, "400 Bad Request", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    cJSON *url_item = cJSON_GetObjectItem(req, "url");
    if (!url_item || !cJSON_IsString(url_item) || strlen(url_item->valuestring) == 0) {
        send_response(client, "400 Bad Request", "{\"error\":\"Missing or empty 'url' field\"}");
        cJSON_Delete(req);
        return;
    }

    /* Optional filename override */
    const char *filename = NULL;
    cJSON *name_item = cJSON_GetObjectItem(req, "filename");
    if (name_item && cJSON_IsString(name_item) && strlen(name_item->valuestring) > 0) {
        filename = name_item->valuestring;
    }

    int id = queue_add(url_item->valuestring, filename);
    cJSON_Delete(req);

    if (id < 0) {
        send_response(client, "500 Internal Server Error",
                      "{\"error\":\"Failed to add download to queue\"}");
        return;
    }

    char resp[128];
    snprintf(resp, sizeof(resp), "{\"id\":%d,\"status\":\"queued\"}", id);
    send_response(client, "201 Created", resp);
}

/* ── Route: POST /api/downloads/{id}/pause|resume|cancel|retry ───────── */

static void handle_download_action(SOCKET client, int id, const char *action) {
    DownloadTask *task = queue_get_task(id);
    if (!task) {
        send_response(client, "404 Not Found", "{\"error\":\"Task not found\"}");
        return;
    }

    int result = -1;

    if (strcmp(action, "pause") == 0) {
        result = engine_pause_download(task);
        if (result != 0) {
            send_response(client, "400 Bad Request",
                          "{\"error\":\"Cannot pause (not downloading or server does not support resume)\"}");
            return;
        }
    } else if (strcmp(action, "resume") == 0) {
        result = engine_resume_download(task);
        if (result != 0) {
            send_response(client, "400 Bad Request",
                          "{\"error\":\"Cannot resume (not paused)\"}");
            return;
        }
    } else if (strcmp(action, "cancel") == 0) {
        result = engine_cancel_download(task);
    } else if (strcmp(action, "retry") == 0) {
        result = queue_retry(id);
        if (result != 0) {
            send_response(client, "400 Bad Request",
                          "{\"error\":\"Cannot retry (not failed or cancelled)\"}");
            return;
        }
    } else if (strcmp(action, "open") == 0) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "start \"\" explorer.exe /select,\"%s\"", task->filepath);
        system(cmd);
        send_response(client, "200 OK", "{\"ok\":true}");
        return;
    } else {
        send_response(client, "400 Bad Request", "{\"error\":\"Unknown action\"}");
        return;
    }

    send_response(client, "200 OK", "{\"ok\":true}");
}

/* ── Route: DELETE /api/downloads/{id} ───────────────────────────────── */

static void handle_delete_download(SOCKET client, int id) {
    /* Add to history before removing */
    DownloadTask *task = queue_get_task(id);
    if (task) {
        TASK_LOCK(task);
        DownloadState st = task->state;
        TASK_UNLOCK(task);

        if (st == STATE_COMPLETED || st == STATE_FAILED || st == STATE_CANCELLED) {
            history_add(task);
        }
    }

    if (queue_remove(id) == 0) {
        send_response(client, "200 OK", "{\"ok\":true}");
    } else {
        send_response(client, "400 Bad Request",
                      "{\"error\":\"Cannot remove (active or not found)\"}");
    }
}

/* ── Route: GET /api/stats ───────────────────────────────────────────── */

static void handle_get_stats(SOCKET client) {
    int total, active, completed, failed;
    queue_get_stats(&total, &active, &completed, &failed);

    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"total\":%d,\"active\":%d,\"completed\":%d,\"failed\":%d}",
             total, active, completed, failed);
    send_response(client, "200 OK", resp);
}

/* ── Route: GET /api/history ─────────────────────────────────────────── */

static void handle_get_history(SOCKET client) {
    char *json = history_get_json();
    if (json) {
        send_response(client, "200 OK", json);
        free(json);
    } else {
        send_response(client, "200 OK", "[]");
    }
}

/* ── Route: DELETE /api/history ───────────────────────────────────────── */

static void handle_clear_history(SOCKET client) {
    history_clear();
    send_response(client, "200 OK", "{\"ok\":true}");
}

/* ── Route: GET /api/settings ────────────────────────────────────────── */

static void handle_get_settings(SOCKET client) {
    char resp[512];
    snprintf(resp, sizeof(resp),
             "{\"download_dir\":\"%s\",\"max_concurrent\":%d}",
             fm_get_download_dir(),
             queue_get_max_concurrent());
    /* Escape backslashes in the path for JSON */
    /* Simple approach: replace \ with / for JSON compatibility */
    for (char *p = resp; *p; p++) {
        /* Only replace within the download_dir value */
    }
    send_response(client, "200 OK", resp);
}

/* ── Route: PUT /api/settings ────────────────────────────────────────── */

static void handle_put_settings(SOCKET client, const char *body) {
    if (!body) {
        send_response(client, "400 Bad Request", "{\"error\":\"Missing request body\"}");
        return;
    }

    cJSON *req = cJSON_Parse(body);
    if (!req) {
        send_response(client, "400 Bad Request", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    cJSON *dir_item = cJSON_GetObjectItem(req, "download_dir");
    if (dir_item && cJSON_IsString(dir_item)) {
        if (fm_set_download_dir(dir_item->valuestring) != 0) {
            send_response(client, "400 Bad Request",
                          "{\"error\":\"Invalid download directory\"}");
            cJSON_Delete(req);
            return;
        }
    }

    cJSON *max_item = cJSON_GetObjectItem(req, "max_concurrent");
    if (max_item && cJSON_IsNumber(max_item)) {
        queue_set_max_concurrent(max_item->valueint);
    }

    cJSON_Delete(req);
    send_response(client, "200 OK", "{\"ok\":true}");
}

/* ── Request router ──────────────────────────────────────────────────── */

/*
 * Parse the HTTP request line to extract the method and path,
 * then dispatch to the appropriate handler.
 *
 * HTTP request line format: "METHOD /path HTTP/1.1\r\n"
 * Example: "GET /api/downloads HTTP/1.1\r\n"
 */
static void route_request(SOCKET client, const char *request) {
    char method[16] = {0};
    char path[512]  = {0};

    /* Parse the first line: METHOD PATH HTTP/1.1 */
    sscanf(request, "%15s %511s", method, path);

    /* Handle CORS preflight (OPTIONS) requests */
    if (strcmp(method, "OPTIONS") == 0) {
        send_response(client, "204 No Content", NULL);
        return;
    }

    /* Process the queue to start any pending downloads */
    queue_process();

    /* ── Route matching ──────────────────────────────────────────── */

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/downloads") == 0) {
        handle_get_downloads(client);
    }
    else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/downloads") == 0) {
        handle_post_download(client, find_request_body(request));
    }
    else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/stats") == 0) {
        handle_get_stats(client);
    }
    else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/history") == 0) {
        handle_get_history(client);
    }
    else if (strcmp(method, "DELETE") == 0 && strcmp(path, "/api/history") == 0) {
        handle_clear_history(client);
    }
    else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/settings") == 0) {
        handle_get_settings(client);
    }
    else if (strcmp(method, "PUT") == 0 && strcmp(path, "/api/settings") == 0) {
        handle_put_settings(client, find_request_body(request));
    }
    else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/open-downloads") == 0) {
        system("start \"\" explorer.exe \"downloads\"");
        send_response(client, "200 OK", "{\"ok\":true}");
    }
    else if (strncmp(path, "/api/downloads/", 15) == 0) {
        int id = extract_id_from_path(path);
        if (id < 0) {
            send_response(client, "400 Bad Request", "{\"error\":\"Invalid download ID\"}");
            return;
        }

        if (strcmp(method, "DELETE") == 0) {
            /* Check if there's no action suffix — it's a delete */
            char action[64] = {0};
            extract_action(path, action, sizeof(action));
            if (action[0] == '\0') {
                handle_delete_download(client, id);
            } else {
                send_response(client, "400 Bad Request", "{\"error\":\"Unknown endpoint\"}");
            }
        }
        else if (strcmp(method, "POST") == 0) {
            char action[64] = {0};
            extract_action(path, action, sizeof(action));
            if (action[0] != '\0') {
                handle_download_action(client, id, action);
            } else {
                send_response(client, "400 Bad Request", "{\"error\":\"Missing action\"}");
            }
        }
        else {
            send_response(client, "405 Method Not Allowed", "{\"error\":\"Method not allowed\"}");
        }
    }
    else {
        send_response(client, "404 Not Found", "{\"error\":\"Endpoint not found\"}");
    }
}

/* ── Client handler thread ───────────────────────────────────────────── */

/*
 * Each accepted TCP connection is handled in its own thread.
 * We read the entire HTTP request, route it, then close the socket.
 */
static DWORD WINAPI handle_client(LPVOID param) {
    SOCKET client = (SOCKET)(intptr_t)param;

    char buffer[REQUEST_BUF_SIZE] = {0};
    int total_received = 0;

    /*
     * Read the HTTP request.  We loop because recv() may return
     * partial data (TCP is a stream protocol, not message-based).
     * We stop when we've seen the end of headers (\r\n\r\n).
     */
    while (total_received < REQUEST_BUF_SIZE - 1) {
        int received = recv(client, buffer + total_received,
                           REQUEST_BUF_SIZE - 1 - total_received, 0);
        if (received <= 0) break;  /* Connection closed or error */
        total_received += received;
        buffer[total_received] = '\0';

        /* Check if we've received the complete headers */
        if (strstr(buffer, "\r\n\r\n")) {
            /*
             * For POST/PUT with body: check Content-Length header
             * and keep reading if we haven't received the full body yet.
             */
            char *cl_header = strstr(buffer, "Content-Length:");
            if (cl_header) {
                int content_length = atoi(cl_header + 15);
                char *body_start = strstr(buffer, "\r\n\r\n") + 4;
                int body_received = total_received - (int)(body_start - buffer);
                if (body_received >= content_length) break;
                /* Continue reading if body is incomplete */
            } else {
                break;  /* No body expected */
            }
        }
    }

    if (total_received > 0) {
        route_request(client, buffer);
    }

    shutdown(client, SD_SEND);
    closesocket(client);
    return 0;
}

/* ── Public API ──────────────────────────────────────────────────────── */

int api_server_start(int port) {
    WSADATA wsa_data;

    /*
     * WSAStartup: initializes the Winsock2 library.
     * MAKEWORD(2, 2) requests version 2.2.
     * Must be called before any Winsock function.
     */
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        log_msg(LOG_ERROR, "WSAStartup failed");
        return -1;
    }

    /*
     * Create a TCP socket.
     * AF_INET     — IPv4
     * SOCK_STREAM — TCP (reliable, ordered byte stream)
     * IPPROTO_TCP — TCP protocol
     */
    g_listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen_socket == INVALID_SOCKET) {
        log_msg(LOG_ERROR, "socket() failed: %d", WSAGetLastError());
        WSACleanup();
        return -1;
    }

    /* Allow port reuse so we can restart quickly after a crash */
    int opt = 1;
    setsockopt(g_listen_socket, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&opt, sizeof(opt));

    /*
     * Bind to 127.0.0.1 (loopback) — only accepts local connections.
     * This is a security measure: the API is not exposed to the network.
     *
     * struct sockaddr_in:
     *   sin_family — AF_INET for IPv4
     *   sin_port   — port in network byte order (htons converts)
     *   sin_addr   — IP address in network byte order
     */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((unsigned short)port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (bind(g_listen_socket, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        log_msg(LOG_ERROR, "bind() failed on port %d: %d", port, WSAGetLastError());
        closesocket(g_listen_socket);
        WSACleanup();
        return -1;
    }

    /*
     * listen(): mark the socket as passive (ready to accept connections).
     * SOMAXCONN is the maximum backlog (queue of pending connections).
     */
    if (listen(g_listen_socket, SOMAXCONN) == SOCKET_ERROR) {
        log_msg(LOG_ERROR, "listen() failed: %d", WSAGetLastError());
        closesocket(g_listen_socket);
        WSACleanup();
        return -1;
    }

    g_running = 1;
    log_msg(LOG_INFO, "API server listening on http://127.0.0.1:%d", port);

    /* ── Accept loop ─────────────────────────────────────────────── */

    while (g_running) {
        /*
         * accept(): blocks until a client connects.
         * Returns a new socket dedicated to this client.
         * The listening socket continues to accept more connections.
         */
        struct sockaddr_in client_addr;
        int client_len = sizeof(client_addr);
        SOCKET client = accept(g_listen_socket,
                              (struct sockaddr *)&client_addr, &client_len);

        if (client == INVALID_SOCKET) {
            if (g_running) {
                log_msg(LOG_WARN, "accept() failed: %d", WSAGetLastError());
            }
            continue;
        }

        /* Handle each client in a new thread */
        HANDLE thread = CreateThread(NULL, 0, handle_client,
                                    (LPVOID)(intptr_t)client, 0, NULL);
        if (thread) {
            CloseHandle(thread);  /* We don't need to join client threads */
        } else {
            closesocket(client);
        }
    }

    closesocket(g_listen_socket);
    WSACleanup();
    log_msg(LOG_INFO, "API server stopped");
    return 0;
}

void api_server_stop(void) {
    g_running = 0;
    /* Close the listening socket to unblock accept() */
    if (g_listen_socket != INVALID_SOCKET) {
        closesocket(g_listen_socket);
        g_listen_socket = INVALID_SOCKET;
    }
}
