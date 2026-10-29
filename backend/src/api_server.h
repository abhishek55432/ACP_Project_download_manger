/*
 * api_server.h - REST API server interface
 *
 * Implements an HTTP server using Winsock2 that listens on 127.0.0.1:8080.
 * Serves JSON responses for the React frontend to consume.
 */

#ifndef API_SERVER_H
#define API_SERVER_H

/* Start the API server on the given port.
 * This function blocks (runs the accept loop).
 * Should be called in a dedicated thread from main().
 * Returns 0 on clean shutdown, -1 on error. */
int api_server_start(int port);

/* Signal the server to stop its accept loop. */
void api_server_stop(void);

#endif /* API_SERVER_H */
