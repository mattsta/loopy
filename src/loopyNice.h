#pragma once

#include "loopyPlatform.h"

#include "loopy.h"
#include "loopyNet.h"

#include <arpa/inet.h> /* INET6_ADDRSTRLEN */
#include <sys/types.h> /* mode_t */

typedef struct loopyNiceServerDesc {
    void *clientData;
    loopyFileCallback *cb;

    /* First, try to use 'bindDesc' */
    char *bindDesc; /* text description of bind target */

    /* Next, try to use 'domain' */
    struct {
        char *path; /* File */
        mode_t perm;
        int fd;
    } domain;

    /* Next, try to create a socketpair */
    struct {
        int fd[2];
        bool useSocketPair;
    } socketpair;

    /* Next, try to create a local pipe */
    struct {
        int readFd;
        int writeFd;
        bool usePipe;
        bool useEventFd;
    } pipe;

    /* else, if those aren't set, use 'bindAddr' / 'port' */
    struct {
        const char *bindAddr; /* TCP / UDP */
        int port;             /* TCP / UDP */
        int listen[2];        /* Server fds created */
    } addr;

    int backlog;
} loopyNiceServerDesc;

typedef struct loopyNiceConnectDesc {
    void *clientData;
    loopyFileCallback *cb;
    char *dst;      /* IP */
    char *bindAddr; /* TCP / UDP */
    int port;       /* TCP / UDP */
    bool bindRequired;
} loopyNiceConnectDesc;

/* todo: make this an anonymous struct */
typedef struct loopyNice {
    loopyLoop *loop;
    loopyNet net;                  /* embedded loopyNet for error reporting */
    loopyNetListenFlag listenFlag; /* copied to net->listenFlag on demand */
} loopyNice;

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
loopyNice *loopyNiceNew(int setSize);

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
void loopyNiceFree(loopyNice *n);

/**
 * Free metadata associated with a loopyNice wrapper without destroying the
 * loop.
 *
 * Cleans up internal metadata structures and nested resources (like loopyNet)
 * but preserves the loopyNice structure and its underlying event loop itself.
 * This is useful for partial cleanup while keeping the loop operational.
 *
 * @param n The loopyNice structure
 *
 * @note This is typically called internally during cleanup operations
 * @note Thread Safety: Must be called from the event loop thread
 * @see loopyNiceFree
 */
void loopyNiceFreeMetadata(loopyNice *n);

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
void loopyNiceStart(loopyNice *n);

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
void loopyNiceStartFdOnly(loopyNice *n);

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
bool loopyNiceKernelLoadBalanceEnable(loopyNice *n);

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
bool loopyNiceServerCreateSockets(loopyNice *n, loopyNiceServerDesc *sd);
bool loopyNiceServerListenToSockets(loopyNice *n,
                                    const loopyNiceServerDesc *sd);
bool loopyNiceServerCreateSocketsAndListen(loopyNice *n,
                                           loopyNiceServerDesc *sd);
