/* echo_server.c - Simple TCP echo server using loopy
 *
 * This example demonstrates:
 * - Creating an event loop
 * - Setting up a TCP server with loopyStream
 * - Accepting connections
 * - Reading and writing data
 * - Proper cleanup
 *
 * Build (from build directory):
 *   Already built as part of loopy: ./examples/echo_server
 *
 * Run:
 *   ./examples/echo_server 8080
 *   ./examples/echo_server --test    # Test mode: starts, accepts one
 * connection, exits
 *
 * Test with:
 *   nc localhost 8080
 *   echo "Hello" | nc localhost 8080
 */

#include "loopy.h"
#include "loopySignal.h"
#include "loopyStream.h"
#include "loopyTimer.h"

#include "../deps/datakit/src/datakit.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

/* Configuration
 *
 * DEFAULT_PORT: TCP port to listen on. 8080 is a common choice for development
 *               servers as it doesn't require root privileges (ports < 1024
 * do).
 *
 * BACKLOG: Maximum pending connections in the listen queue. This is how many
 *          connections can be waiting for accept() before new ones are refused.
 *          128 is a reasonable default; nginx uses 511, SOMAXCONN is system
 * max.
 *
 * READ_BUFFER_SIZE: Size of per-connection read buffer. 4KB is efficient for
 *                   most TCP traffic (fits in one page, good for small
 * messages). Increase to 64KB+ for high-throughput bulk transfers.
 */
#define DEFAULT_PORT 8080
#define BACKLOG 128
#define READ_BUFFER_SIZE 4096

/* Global for signal handling */
static loopyLoop *g_loop = NULL;
static int g_testMode = 0;
static int g_testResult = 0; /* 0 = success */
static int g_connectionsHandled = 0;

/* Forward declarations */
static void onConnection(loopyStream *server, int status, void *userData);
static void onAlloc(loopyStream *stream, size_t suggested, void **buf,
                    size_t *bufLen, void *userData);
static void onRead(loopyStream *stream, ssize_t nread, const void *buf,
                   void *userData);
static void onWrite(loopyStream *stream, int status, void *userData);
static void onClose(loopyStream *stream, void *userData);
static void onSignal(loopyLoop *loop, int signum, void *userData);

/* Client context */
typedef struct {
    char readBuf[READ_BUFFER_SIZE];
    char peerAddr[64];
    int peerPort;
} ClientContext;

/* ====================================================================
 * Main
 * ==================================================================== */

/* Test mode timeout - stop server after timeout */
static void testTimeout(loopyLoop *loop, loopyTimer *timer, void *userData) {
    (void)timer;
    (void)userData;
    printf("[TEST] Timeout reached, stopping server\n");
    if (g_connectionsHandled == 0) {
        g_testResult = 1; /* Fail if no connections in test mode */
        fprintf(stderr, "[TEST] FAIL: No connections handled\n");
    }
    loopyStop(loop);
}

int main(int argc, char **argv) {
    int port = DEFAULT_PORT;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--test") == 0) {
            g_testMode = 1;
        } else {
            port = atoi(argv[i]);
            if (port <= 0 || port > 65535) {
                fprintf(stderr, "Invalid port: %s\n", argv[i]);
                return 1;
            }
        }
    }

    if (g_testMode) {
        /* Use a random high port in test mode to avoid conflicts */
        port = 30000 + (getpid() % 10000);
        printf("[TEST] Running in test mode on port %d\n", port);
    } else {
        printf("Starting echo server on port %d...\n", port);
        printf("Press Ctrl+C to stop\n\n");
    }

    /* Create event loop with capacity for 1024 file descriptors.
     *
     * The capacity parameter determines how many file descriptors (sockets,
     * pipes, files) can be monitored simultaneously. Each client connection
     * uses one FD, plus one for the server socket itself.
     *
     * 1024 allows ~1000 concurrent clients, which is plenty for most servers.
     * For high-concurrency servers (10K+ connections), increase accordingly.
     * The array grows automatically but starting at the right size is more
     * efficient.
     */
    loopyLoop *loop = loopyNew(1024);
    if (!loop) {
        fprintf(stderr, "Failed to create event loop\n");
        return 1;
    }
    g_loop = loop;

    printf("Using %s backend\n", loopyAdapterName());

    /* Set up signal handler for graceful shutdown */
    loopySignalHandler *sigHandler = loopySignalNew(loop);
    if (!sigHandler) {
        fprintf(stderr, "Failed to set up signal handler\n");
        loopyDelete(loop);
        return 1;
    }
    loopySignalRegister(sigHandler, SIGINT, onSignal, NULL);

    /* Create TCP server stream */
    loopyStream *server = loopyStreamNewTcp(loop);
    if (!server) {
        fprintf(stderr, "Failed to create server stream\n");
        loopySignalFree(sigHandler);
        loopyDelete(loop);
        return 1;
    }

    /* Bind to all interfaces */
    if (!loopyStreamBind(server, "0.0.0.0", port)) {
        fprintf(stderr, "Failed to bind to port %d: %s\n", port,
                loopyStreamGetError(server));
        loopyStreamClose(server, NULL, NULL);
        loopySignalFree(sigHandler);
        loopyDelete(loop);
        return 1;
    }

    /* Start listening */
    if (!loopyStreamListen(server, BACKLOG, onConnection, NULL)) {
        fprintf(stderr, "Failed to listen: %s\n", loopyStreamGetError(server));
        loopyStreamClose(server, NULL, NULL);
        loopySignalFree(sigHandler);
        loopyDelete(loop);
        return 1;
    }

    printf("Listening on 0.0.0.0:%d\n", port);

    /* In test mode, set a timeout and spawn a client connection */
    loopyTimer *testTimer = NULL;
    if (g_testMode) {
        testTimer = loopyTimerOneShotMs(loop, 2000, testTimeout, NULL);

        /* Fork a simple test client */
        pid_t pid = fork();
        if (pid == 0) {
            /* Child process - act as client */
            usleep(100000); /* Wait 100ms for server to be ready */

            int sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock >= 0) {
                struct sockaddr_in addr = {0};
                addr.sin_family = AF_INET;
                addr.sin_port = htons(port);
                addr.sin_addr.s_addr = inet_addr("127.0.0.1");

                if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) ==
                    0) {
                    const char *msg = "Hello from test client!";
                    send(sock, msg, strlen(msg), 0);

                    char buf[256] = {0};
                    ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
                    if (n > 0 && strcmp(buf, msg) == 0) {
                        printf("[TEST CLIENT] Echo verified!\n");
                    }
                }
                close(sock);
            }
            _exit(0);
        }
    }

    /* Run the event loop */
    loopyMain(loop);

    /* Cleanup */
    if (!g_testMode) {
        printf("\nShutting down...\n");
    }
    if (testTimer) {
        loopyTimerCancel(testTimer);
    }
    loopyStreamClose(server, NULL, NULL);
    loopySignalFree(sigHandler);
    loopyDelete(loop);

    if (g_testMode) {
        /* Wait for child process */
        int status;
        wait(&status);

        if (g_testResult == 0 && g_connectionsHandled > 0) {
            printf("[TEST] PASS: Echo server handled %d connection(s)\n",
                   g_connectionsHandled);
        }
        return g_testResult;
    }

    printf("Goodbye!\n");
    return 0;
}

/* ====================================================================
 * Callbacks
 * ==================================================================== */

/* Called when a new connection arrives */
static void onConnection(loopyStream *server, int status, void *userData) {
    (void)userData;

    if (status < 0) {
        fprintf(stderr, "Connection error: %s\n", loopyStreamGetError(server));
        return;
    }

    /* Accept the connection */
    loopyStream *client = loopyStreamAccept(server);
    if (!client) {
        fprintf(stderr, "Failed to accept connection\n");
        return;
    }

    /* Create client context */
    ClientContext *ctx = zcalloc(1, sizeof(ClientContext));
    if (!ctx) {
        fprintf(stderr, "Out of memory\n");
        loopyStreamClose(client, NULL, NULL);
        return;
    }

    /* Get peer info for logging */
    loopyStreamGetPeerName(client, ctx->peerAddr, sizeof(ctx->peerAddr),
                           &ctx->peerPort);

    /* Attach context to stream */
    loopyStreamSetData(client, ctx);

    printf("[%s:%d] Connected\n", ctx->peerAddr, ctx->peerPort);
    g_connectionsHandled++;

    /* Start reading from client */
    if (!loopyStreamReadStart(client, onAlloc, onRead, ctx)) {
        fprintf(stderr, "[%s:%d] Failed to start reading\n", ctx->peerAddr,
                ctx->peerPort);
        zfree(ctx);
        loopyStreamClose(client, NULL, NULL);
    }
}

/* Called to allocate a read buffer */
static void onAlloc(loopyStream *stream, size_t suggested, void **buf,
                    size_t *bufLen, void *userData) {
    (void)stream;
    (void)suggested;

    ClientContext *ctx = userData;
    *buf = ctx->readBuf;
    *bufLen = sizeof(ctx->readBuf);
}

/* Called when data is received */
static void onRead(loopyStream *stream, ssize_t nread, const void *buf,
                   void *userData) {
    ClientContext *ctx = userData;

    if (nread < 0) {
        /* Error */
        fprintf(stderr, "[%s:%d] Read error\n", ctx->peerAddr, ctx->peerPort);
        loopyStreamClose(stream, onClose, ctx);
        return;
    }

    if (nread == 0) {
        /* EOF - client disconnected */
        printf("[%s:%d] Disconnected\n", ctx->peerAddr, ctx->peerPort);
        loopyStreamClose(stream, onClose, ctx);

        /* In test mode, stop after first connection completes */
        if (g_testMode) {
            loopyStop(g_loop);
        }
        return;
    }

    /* Echo the data back */
    printf("[%s:%d] Received %zd bytes, echoing back\n", ctx->peerAddr,
           ctx->peerPort, nread);

    /* Copy data for write (since buf may be reused) */
    char *writeBuf = zmalloc(nread);
    if (!writeBuf) {
        fprintf(stderr, "Out of memory for write buffer\n");
        loopyStreamClose(stream, onClose, ctx);
        return;
    }
    memcpy(writeBuf, buf, nread);

    /* Write echoed data (writeBuf will be freed in onWrite) */
    if (!loopyStreamWrite(stream, writeBuf, nread, onWrite, writeBuf)) {
        fprintf(stderr, "[%s:%d] Write failed\n", ctx->peerAddr, ctx->peerPort);
        zfree(writeBuf);
        loopyStreamClose(stream, onClose, ctx);
    }
}

/* Called when write completes */
static void onWrite(loopyStream *stream, int status, void *userData) {
    char *writeBuf = userData;
    zfree(writeBuf);

    if (status < 0) {
        ClientContext *ctx = loopyStreamGetData(stream);
        fprintf(stderr, "[%s:%d] Write error\n", ctx->peerAddr, ctx->peerPort);
        loopyStreamClose(stream, onClose, ctx);
    }
}

/* Called when close completes */
static void onClose(loopyStream *stream, void *userData) {
    (void)stream;
    ClientContext *ctx = userData;
    zfree(ctx);
}

/* Called on SIGINT */
static void onSignal(loopyLoop *loop, int signum, void *userData) {
    (void)signum;
    (void)userData;

    printf("\nReceived SIGINT, stopping event loop...\n");
    loopyStop(loop);
}
