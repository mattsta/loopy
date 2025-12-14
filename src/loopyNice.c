#include "loopyPlatform.h"

#include "loopyNice.h"

#include <stdlib.h>
#include <string.h> /* strchr */
#include <unistd.h> /* close */

#if __linux__
#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 30)
#include <sys/eventfd.h>
#define USE_EVENTFD 1
#endif
#endif

/**
 * Create a new loopyNice event loop wrapper.
 *
 * Allocates and initializes a new loopyNice structure with an underlying
 * event loop. The loopyNice wrapper provides convenient high-level APIs
 * for common event loop operations like server socket creation and listening.
 *
 * @param setSize Initial size hint for the event loop's file descriptor set.
 *                 This is a performance optimization; the set will grow as
 * needed. Typical values: 128, 256, 512, or 1024.
 *
 * @return Newly allocated loopyNice structure, or NULL if memory allocation
 * failed
 *
 * @note The returned structure must be freed with loopyNiceFree()
 * @note Thread Safety: The returned loopyNice object should only be used from
 *                      the thread that will run loopyNiceStart() or
 * loopyNiceStartFdOnly()
 * @see loopyNiceFree
 * @see loopyNiceStart
 */
loopyNice *loopyNiceNew(int setSize) {
    loopyNice *n = zcalloc(1, sizeof(*n));

    n->loop = loopyNew(setSize);
    if (!n->loop) {
        return NULL;
    }

    return n;
}

/**
 * Free a loopyNice event loop wrapper and all associated resources.
 *
 * Stops the event loop if running, closes all registered file descriptors,
 * cancels all active timers, and deallocates the loopyNice structure and
 * its underlying event loop.
 *
 * @param n The loopyNice structure to free (may be NULL)
 *
 * @note Thread Safety: Must be called from the event loop thread or after
 *                      ensuring the loop is not running
 * @note After this call, the loopyNice pointer is invalid and should not be
 * used
 * @see loopyNiceNew
 */
void loopyNiceFree(loopyNice *const n) {
    if (n) {
        loopyDelete(n->loop);
        *n = (loopyNice){0};
        zfree(n);
    }
}

/**
 * Start the event loop and run until stopped.
 *
 * Enters the main event loop, which blocks until loopyDelete() or
 * similar shutdown function is called. The loop processes file descriptor
 * events, timers, and other registered callbacks.
 *
 * @param n The loopyNice structure
 *
 * @note This function blocks and does not return until the loop is stopped
 * @note Thread Safety: Should be called from the intended event loop thread
 * @note Typical usage: Create with loopyNiceNew(), register handlers, then call
 * this
 * @see loopyNiceStartFdOnly
 * @see loopyNiceFree
 */
void loopyNiceStart(loopyNice *n) {
    loopyMain(n->loop);
}

/**
 * Start the event loop in file descriptor-only mode.
 *
 * Similar to loopyNiceStart() but only processes file descriptor events,
 * ignoring timers and other non-FD event sources. Useful for special-purpose
 * loops that don't need timer support.
 *
 * @param n The loopyNice structure
 *
 * @note This function blocks and does not return until the loop is stopped
 * @note Thread Safety: Should be called from the intended event loop thread
 * @note File descriptor callbacks will still be invoked, but timer callbacks
 * will not
 * @see loopyNiceStart
 */
void loopyNiceStartFdOnly(loopyNice *n) {
    loopyMainFdOnly(n->loop);
}

static bool sockReadableAdd(loopyNice *n, int sock,
                            const loopyNiceServerDesc *sd) {
    return loopyRegisterRead(n->loop, sock, sd->cb, sd->clientData);
}

static bool loopyNiceFlagSet(loopyNice *n, loopyNetListenFlag flag) {
    if (n->listenFlag & flag) {
        /* flag already set, can't set again. */
        return false;
    }

    n->listenFlag |= flag;
    n->net.listenFlag = n->listenFlag;
    return true;
}

/**
 * Enable kernel-level load balancing for server sockets.
 *
 * Enables SO_REUSEPORT (SO_REUSEADDR on some platforms) to allow multiple
 * processes to bind to the same port and share incoming connections. This
 * enables kernel-level load balancing across multiple processes.
 *
 * @param n The loopyNice structure
 *
 * @return true if load balancing was successfully enabled,
 *         false if already enabled or unsupported on this platform
 *
 * @note This must be called before loopyNiceServerCreateSockets()
 * @note Only effective when creating server sockets bound to specific addresses
 * @note Thread Safety: Must be called from the event loop thread
 * @see loopyNiceServerCreateSockets
 */
bool loopyNiceKernelLoadBalanceEnable(loopyNice *n) {
    return loopyNiceFlagSet(n, LOOPY_NET_SERVER_L4_LOAD_BALANCE);
}

bool loopyNiceServerCreateSocketsAndListen(loopyNice *n,
                                           loopyNiceServerDesc *sd) {
    if (loopyNiceServerCreateSockets(n, sd)) {
        loopyNiceServerListenToSockets(n, sd);
        return true;
    }

    return false;
}

bool loopyNiceServerListenToSockets(loopyNice *n,
                                    const loopyNiceServerDesc *sd) {
    /* Listen on socket(s) created by SocketCreate */
    bool result = true;

    if (sd->socketpair.useSocketPair) {
        for (size_t i = 0; i < 2; i++) {
            result |= sockReadableAdd(n, sd->socketpair.fd[i], sd);
        }
    } else if (sd->pipe.usePipe) {
        /* Attach the readFd to the event loop and assign it
         * the callback and userData requested by 'sd' */
        result |= sockReadableAdd(n, sd->pipe.readFd, sd);
    } else if (sd->domain.path) {
        result |= sockReadableAdd(n, sd->domain.fd, sd);
    } else {
        result |= sockReadableAdd(n, sd->addr.listen[0], sd);
        if (sd->addr.listen[1] > 0) {
            result |= sockReadableAdd(n, sd->addr.listen[1], sd);
        }
    }

    return result;
}

/**
 * Create server sockets according to the given server descriptor.
 *
 * Allocates and configures one or more listening server sockets based on
 * the loopyNiceServerDesc settings. Supports multiple socket types:
 * - TCP/UDP sockets (IPv4 and/or IPv6)
 * - Unix domain sockets
 * - Socketpair for local IPC
 * - Pipes and eventfd for local signaling
 *
 * The actual socket file descriptors are stored in the loopyNiceServerDesc
 * structure (in the addr.listen[], domain.fd, socketpair.fd[], or pipe.*
 * fields).
 *
 * @param n  The loopyNice structure
 * @param sd The server descriptor specifying socket configuration:
 *           - If sd->bindDesc is set, parse it (format: "tcp://host:port",
 * etc.)
 *           - Else if sd->socketpair.useSocketPair, create a Unix socketpair
 *           - Else if sd->pipe.usePipe, create a pipe (or eventfd if available)
 *           - Else if sd->domain.path, create a Unix domain socket
 *           - Else use sd->addr (TCP/UDP with host and port)
 *
 * @return true if sockets were created successfully,
 *         false if socket creation or configuration failed
 *
 * @note The created socket file descriptors are populated in the descriptor
 * structure. The caller is responsible for calling
 * loopyNiceServerListenToSockets() next.
 * @note Thread Safety: Must be called from the event loop thread
 * @note Socket options like SO_REUSEPORT may have been applied if enabled
 * @see loopyNiceServerListenToSockets
 * @see loopyNiceServerCreateSocketsAndListen
 */
bool loopyNiceServerCreateSockets(loopyNice *n, loopyNiceServerDesc *sd) {
    if (sd->bindDesc) {
        /* parse bind desc to Proto & IP & Port */
        /* formats:
         *   tcp://1.2.3.4:9999
         *   tcp://[::1]:9999
         *   tcp://::1:9999
         *   udp://1.2.3.4:9999
         *   udp://[::1]:9999
         *   udp://::1:9999
         *   unix:///tmp/somefile:744
         *   pipe://internal
         *   eventfd://internal */
        /* Verify we got something... */
        switch (sd->bindDesc[1]) {
        case 'c': /* TCP */
        case 'd': /* UDP */
        case 'n': /* UNIX */
        case 'i': /* PIPE */
        case 'v': /* EVENTFD */
        default:
            assert(NULL && "Invalid bind description!");
        }
        assert(NULL && "Not implemented!");
        return false;
    } else if (sd->socketpair.useSocketPair) {
        /* User requested a socketpair! (which is really just one socket) */
        int socketVector[2] = {0};

        if (socketpair(AF_UNIX, SOCK_STREAM, 0, socketVector) == -1) {
            assert(NULL && "no socketpair?");
            return false;
        }

        for (size_t i = 0; i < 2; i++) {
            sd->socketpair.fd[i] = socketVector[i];

            loopyNet ln = {.sock = socketVector[i]};

            if (!loopyNetNonBlockEnable(&ln)) {
                close(socketVector[i]);
                if (i == 1) {
                    close(socketVector[0]);
                }
                return false;
            }
        }
    } else if (sd->pipe.usePipe) {
        /* User requested a pair of read/write pipes! */
        int pipeFd[2] = {0};
        bool reallyUseEventFd = false;

#if USE_EVENTFD
        if (sd->pipe.useEventFd) {
            /* User requested eventfd *and* the system has eventfd */
            reallyUseEventFd = true;
        }

        if (sd->pipe.useEventFd) {
            /* linux-only file descriptor notification API */
            int fd = eventfd(0, EFD_NONBLOCK | EFD_SEMAPHORE);
            pipeFd[0] = fd;
            pipeFd[1] = fd;
        } else {
#endif
            /* double "else" for the macro and for the case of
             * a non-eventfd pipe request when eventfd is available
             * at compile time. */
            if (pipe(pipeFd) == -1) {
                return false;
            }
#if USE_EVENTFD
        }
#endif

        /* attach the obtained pipe endpoints back to the caller so
         * the caller can retrieve them. */
        sd->pipe.readFd = pipeFd[0];
        sd->pipe.writeFd = pipeFd[1];

        /* Set our pipes to nonblock only if we didn't use eventfd
         * (we set NONBLOCK on the eventfd fds when they were created) */
        // cppcheck-suppress knownConditionTrueFalse
        if (!reallyUseEventFd) {
            loopyNet lnRead = {.sock = sd->pipe.readFd};
            loopyNet lnWrite = {.sock = sd->pipe.writeFd};

            /* Pipes aren't non-blocking by default using the pipe() API
             * so fix it ourselves. */
            if (!loopyNetNonBlockEnable(&lnRead) ||
                !loopyNetNonBlockEnable(&lnWrite)) {
                close(sd->pipe.readFd);
                close(sd->pipe.writeFd);
                return false;
            }
        }
    } else if (sd->domain.path) {
        /* User requested a unix domain path! */
        loopyNet *ln = &n->net;
        if (!loopyNetUnixServer(ln, sd->domain.path, sd->domain.perm,
                                sd->backlog)) {
            return false;
        }

        int fd = loopyNetSockGet(ln);
        if (!loopyNetNonBlockEnable(ln)) {
            close(fd);
            return false;
        }

        sd->domain.fd = fd;
    } else {
        /* Regular IPv4 or IPv6 Connectivity */
        const char *ip = sd->addr.bindAddr;
        int port = sd->addr.port;
        loopyNet *ln = &n->net;
        if (!ip) {
            /* if no bind address, bind to both 0.0.0.0 and :: */
            if (!loopyNetTcp6Server(ln, port, NULL, sd->backlog)) {
                return false;
            }

            const int sock6 = loopyNetSockGet(ln);
            sd->addr.listen[0] = sock6;

            if (port == 0) {
                loopyNetSockName(sock6, NULL, 0, &port);
            }

            if (!loopyNetNonBlockEnable(ln)) {
                close(sock6);
                return false;
            }

            if (!loopyNetTcp4Server(ln, port, NULL, sd->backlog)) {
                close(sock6); /* don't leak since not tracking it yet. */
                return false;
            }

            const int sock4 = loopyNetSockGet(ln);
            sd->addr.listen[1] = sock4;

            if (!loopyNetNonBlockEnable(ln)) {
                close(sock6);
                close(sock4);
                return false;
            }
        } else {
            if (strchr(ip, ':')) {
                /* Detected IPv6 address! */
                if (!loopyNetTcp6Server(ln, port, ip, sd->backlog)) {
                    return false;
                }
            } else {
                /* Default is IPv4 */
                if (!loopyNetTcp4Server(ln, port, ip, sd->backlog)) {
                    return false;
                }
            }

            const int sock = loopyNetSockGet(ln);

            if (port == 0) {
                loopyNetSockName(sock, NULL, 0, &port);
            }

            assert(sock > 2);

            sd->addr.listen[0] = sock;
            sd->addr.listen[1] = -1;

            if (!loopyNetNonBlockEnable(ln)) {
                close(sock);
                return false;
            }
        }

        sd->addr.port = port;
    }

    return true;
}
