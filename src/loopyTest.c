#include "loopyPlatform.h"

#include "loopyInternal.h"
#include "loopyNice.h"
#include <assert.h>
#include <inttypes.h>

#include <stdio.h>
#include <stdlib.h> /* EXIT_FAILURE */
#include <unistd.h>

#include "../deps/datakit/src/timeUtil.h"

#include <errno.h>
#include <signal.h>
#include <string.h> /* strerror */

typedef struct client {
    uint8_t *replyData;
    size_t lenData;
    size_t lenTotal;
    size_t lenSentSoFar;
} client;

static bool timerA(timerWheel *t, timerWheelId id, void *data) {
    (void)t;
    (void)id;
    (void)data;

    static uint64_t previousRun = 0;
    uint64_t now = timeUtilMonotonicNs();

    if (previousRun == 0) {
        printf("Clamped native resolution of %" PRIu64 " (%d bits)\n", now,
               64 - __builtin_clzll(now));
    }

#if 0
    int delayBy = rand() % 5;
    printf("BAZINGA! (timer was delayed for %f seconds (by %d))\n", (now - previousRun)/1e6, delayBy);
    sleep(delayBy);
#else
    printf("BAZINGA! (timer was delayed for %f seconds)\n",
           (now - previousRun) / 1e9);
    fflush(stdout);
#endif

    previousRun = now;
    return true;
}

static void echoServerClientHandlerWrite(loopyLoop *l, int clientSock,
                                         void *clientData, loopyAction mask) {
    (void)l;
    (void)mask;

    printf("WRITING!\n");
    client *c = clientData;

    size_t dataLen = c->lenData - c->lenSentSoFar;
    ssize_t written =
        write(clientSock, c->replyData + c->lenSentSoFar, dataLen);

    if (written == -1) {
        if (errno == EAGAIN || errno == EINTR) {
            perror("WRITING:");
            return;
        }
    }

    c->lenSentSoFar += written;
    if (c->lenSentSoFar == c->lenData) {
        /* if total length sent == total length data, then
         * unregister writer since we have no more to write right now */
        loopyUnregisterWrite(l, clientSock);
    }
}

static void echoServerClientHandlerRead(loopyLoop *l, int clientSock,
                                        void *clientData, loopyAction mask) {
    (void)mask;

    client *c = clientData;
    if (c->lenTotal - c->lenData < 300) {
        c->lenTotal *= 2;
        c->replyData = zrealloc(c->replyData, c->lenTotal);
    }
    /* Note: we don't worry about reading in a loop
     *       because we use level-triggered polling so
     *       the async event system will continue running
     *       this callback until no more data remains.
     *
     *       We're basically outsourcing our while(true) loop
     *       down to the OS.
     *
     *       This also allows better interleaving of client requests
     *       and server-side timers since it's less likely one client
     *       will block the server sending unlimited data all at once. */
    ssize_t readLen =
        read(clientSock, c->replyData + c->lenData, c->lenTotal - c->lenData);
    if (readLen == -1) {
        if (errno == EAGAIN || errno == EPIPE || errno == ECONNRESET) {
            /* done reading */
            return;
        } else {
            /* other error */
            printf("OTHER ERROR! %d, %s\n", errno, strerror(errno));
            assert(NULL);
            return;
        }
    } else if (readLen == 0) {
        /* connection closed */
        loopyUnregisterReadWrite(l, clientSock);
        close(clientSock);

        zfree(c->replyData);
        zfree(c);

        printf("CLOSED!\n");
        return;
    }

    printf("Someone sent: %.*s\n", (int)readLen, c->replyData + c->lenData);
    c->lenData += readLen;
    printf("data so far: %zu\n", c->lenData);

    /* Schedule writes to echo back */
    if (!loopyRegisterWrite(l, clientSock, echoServerClientHandlerWrite, c)) {
        printf("ERROR: %s\n", strerror(errno));
        close(clientSock);
    }
}

static void echoServerConnectionHandler(loopyLoop *l, int serverSock,
                                        void *clientData, loopyAction mask) {
    loopyNet net = {.sock = serverSock};
    char inetAddr[INET6_ADDRSTRLEN] = {0};
    int clientSock;
    int clientPort;

    (void)mask;
    (void)clientData;

    if (!loopyNetTcpAcceptNonBlock(&net, inetAddr, sizeof(inetAddr),
                                   &clientPort, &clientSock)) {
        printf("ACCEPT ERROR: %s\n", strerror(errno));
        return;
    }

    printf("Accepted connection from %s:%d\n", inetAddr, clientPort);

    loopyNet clientNet = {.sock = clientSock};
    loopyNetTcpNoDelayEnable(&clientNet);

    client *c = zcalloc(1, sizeof(*c));
    c->replyData = zcalloc(1, 640);
    c->lenTotal = 640;

    /* Attach reader to client socket */
    if (!loopyRegisterRead(l, clientSock, echoServerClientHandlerRead, c)) {
        printf("ERROR: %s\n", strerror(errno));
        close(clientSock);
    }
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    signal(SIGPIPE, SIG_IGN);

    int port = 7777;
    loopyNice *ln = loopyNiceNew(1);

    if (!loopyRegisterTimer(ln->loop, 1e6, 1e6, timerA, NULL)) {
        assert(NULL && "Failed to create timer!");
    }

    loopyNiceServerDesc l1 = {.clientData = NULL,
                              .cb = echoServerConnectionHandler,
                              .addr = {.bindAddr = NULL, .port = port},
                              .backlog = 511};

    /* Get Socket
     * Bind
     * Connect Outbound as Necessary
     * Listen
     * Process
     * Stop */

    if (!loopyNiceServerCreateSocketsAndListen(ln, &l1)) {
        printf("Failure: %s\n", ln->net.errorString);
        return EXIT_FAILURE;
    }

    loopyNiceStart(ln);

    loopyNiceFree(ln);

    return EXIT_SUCCESS;
}
