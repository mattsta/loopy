/* loopyNet - networking wrappers with error message propagation
 *
 * Copyright 2016 Matt Stancliff <matt@genges.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "loopyPlatform.h"
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <assert.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <errno.h>

#include "loopyNet.h"

/* Only available in Linux 2.6.27+ */
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif

#define LN_SOCK_CLOSED -1

/* ====================================================================
 * Error Management
 * ==================================================================== */
#ifndef NDEBUG
#include <stdio.h>
static void loopyNetSetError(loopyNet *l, const char *field) {
    snprintf(l->errorString, sizeof(l->errorString), "%s: %s", field,
             strerror(errno));
}
#else
#define loopyNetSetError(l, field)                                             \
    do {                                                                       \
        (void)l;                                                               \
    } while (0)
#endif

/* ====================================================================
 * Blocking Management
 * ==================================================================== */
/* Note: this takes an independent 'sock' instead of using 'l->sock' because
 *       sometimes we need to set the *client* to non-blocking, but 'l' only
 *       holds the server socket (currently). */
bool loopyNetSetNonBlock(loopyNet *l, const int sock, const bool nonBlock) {
    int flags;
    if ((flags = fcntl(sock, F_GETFL)) == -1) {
        loopyNetSetError(l, "getfl");
        return false;
    }

    if (nonBlock) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }

    if (fcntl(sock, F_SETFL, flags) == -1) {
        loopyNetSetError(l, nonBlock ? "set nonblock" : "clear nonblock");
        return false;
    }

    return true;
}

bool loopyNetNonBlockEnable(loopyNet *l) {
    return loopyNetSetNonBlock(l, l->sock, true);
}

bool loopyNetNonBlockDisable(loopyNet *l) {
    return loopyNetSetNonBlock(l, l->sock, false);
}

/* ====================================================================
 * setsockopt affirmative wrapper
 * ==================================================================== */
static bool setSockOptInteger(int sock, int level, int optionName,
                              const int value) {
    return setsockopt(sock, level, optionName, &value, sizeof(value)) == 0;
}

static bool enableSocketOption(int sock, int level, int optionName) {
    return setSockOptInteger(sock, level, optionName, 1);
}

/* ====================================================================
 * Keepalive Management
 * ==================================================================== */
/* Set TCP keep alive option to detect dead peers. The interval option
 * is only used for Linux as we are using Linux-specific APIs to set
 * the probe send time, interval, and count. */
bool loopyNetKeepAlive(loopyNet *l, int interval) {
    (void)interval; /* we don't always use 'interval' */

    if (!enableSocketOption(l->sock, SOL_SOCKET, SO_KEEPALIVE)) {
        loopyNetSetError(l, "keepalive");
        return false;
    }

#ifdef _OSX
    if (!enableSocketOption(l->sock, IPPROTO_TCP, TCP_KEEPALIVE)) {
        loopyNetSetError(l, "tcpkeepalive");
        return false;
    }
#elif defined(__GLIBC__) && !defined(__FreeBSD_kernel__)
    /* Default settings on Linux aren't useful (2 hour keepalive), so use
     * smaller value of 'interval' seconds. */

    /* Send first probe after interval. */
    if (!setSockOptInteger(l->sock, IPPROTO_TCP, TCP_KEEPIDLE, interval)) {
        loopyNetSetError(l, "keepidle");
        return false;
    }

    /* Send next probes after the specified interval. Note that we set the
     * delay as interval / 3, as we send three probes before detecting
     * an error (see the next setsockopt call). */
    int probe = interval / 3;
    if (probe == 0) {
        probe = 1;
    }

    if (!setSockOptInteger(l->sock, IPPROTO_TCP, TCP_KEEPINTVL, probe)) {
        loopyNetSetError(l, "keepintvl");
        return false;
    }

    /* Consider socket in error state after three ACK probes without reply. */
    const int failureCount = 3;
    if (!setSockOptInteger(l->sock, IPPROTO_TCP, TCP_KEEPCNT, failureCount)) {
        loopyNetSetError(l, "keepcnd");
        return false;
    }
#endif

    return true;
}

/* ====================================================================
 * NoDelay Management
 * ==================================================================== */
static bool loopyNetSetTcpNoDelay(loopyNet *l, bool enable) {
    if (!setSockOptInteger(l->sock, IPPROTO_TCP, TCP_NODELAY, enable)) {
        loopyNetSetError(l, "nodelay");
        return false;
    }

    return true;
}

bool loopyNetTcpNoDelayEnable(loopyNet *l) {
    return loopyNetSetTcpNoDelay(l, true);
}

bool loopyNetTcpNoDelayDisable(loopyNet *l) {
    return loopyNetSetTcpNoDelay(l, false);
}

/* ====================================================================
 * Error Checking Management
 * ==================================================================== */
bool loopyNetCheckSocketError(loopyNet *l) {
    int err = 0;
    socklen_t errlen = sizeof(err);

    if ((getsockopt(l->sock, SOL_SOCKET, SO_ERROR, &err, &errlen) == -1) ||
        err) {
        loopyNetSetError(l, "error");
        return false;
    }

    return true;
}

/* ====================================================================
 * Buffer Management
 * ==================================================================== */
bool loopyNetSetSendBuffer(loopyNet *l, int buffsize) {
    if (!setSockOptInteger(l->sock, SOL_SOCKET, SO_SNDBUF, buffsize)) {
        loopyNetSetError(l, "sndbuf");
        return false;
    }

    return true;
}

/* ====================================================================
 * KeepAlive Management
 * ==================================================================== */
bool loopyNetTcpKeepAliveEnable(loopyNet *l) {
    if (!enableSocketOption(l->sock, SOL_SOCKET, SO_KEEPALIVE)) {
        loopyNetSetError(l, "keepalive");
        return false;
    }

    return true;
}

/* ====================================================================
 * SendTimeout Management
 * ==================================================================== */
/* Set the socket send timeout (SO_SNDTIMEO socket option) to the specified
 * number of milliseconds, or disable it if the 'ms' argument is zero. */
bool loopyNetSendTimeout(loopyNet *l, uint64_t ms) {
    const struct timeval tv = {.tv_sec = ms / 1000,
                               .tv_usec = (ms % 1000) * 1000};

    if (setsockopt(l->sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == -1) {
        loopyNetSetError(l, "sndtimeo");
        return false;
    }

    return true;
}

/* ====================================================================
 * Resolvers (warning: synchronous)
 * ==================================================================== */
/* Resolve 'host' and write string IP address into 'ipbuf'.
 *
 * If flags has LOOPY_NET_RESOLVE_ONLY_IP set, the function does not
 * resolve hostnames and just verifies an IP address is valid. */
static bool loopyNetResolveGeneric(loopyNet *l, const char *host, char *ipbuf,
                                   size_t ipbufLen, loopyNetResolveData *extra,
                                   loopyNetResolveFlag flags) {
    struct addrinfo hints = {.ai_family = AF_UNSPEC,
                             .ai_socktype = SOCK_STREAM};

    if (flags & LOOPY_NET_RESOLVE_ONLY_IP) {
        hints.ai_flags = AI_NUMERICHOST;
    }

    struct addrinfo *info;
    int rv = getaddrinfo(host, NULL, &hints, &info);
    if (rv != 0) {
        loopyNetSetError(l, gai_strerror(rv));
        return false;
    }

    if (extra) {
        extra->af = info->ai_family;
    }

    if (info->ai_family == AF_INET) {
        struct sockaddr_in *sa = (struct sockaddr_in *)info->ai_addr;
        inet_ntop(AF_INET, &(sa->sin_addr), ipbuf, ipbufLen);
        if (extra) {
            extra->sa = *(struct sockaddr_in *)info->ai_addr;
        }
    } else {
        struct sockaddr_in6 *sa = (struct sockaddr_in6 *)info->ai_addr;
        inet_ntop(AF_INET6, &(sa->sin6_addr), ipbuf, ipbufLen);
        if (extra) {
            extra->sa6 = *(struct sockaddr_in6 *)info->ai_addr;
        }
    }

    freeaddrinfo(info);
    return true;
}

bool loopyNetResolve(loopyNet *l, const char *host, char *ipbuf,
                     size_t ipbufLen) {
    return loopyNetResolveGeneric(l, host, ipbuf, ipbufLen, NULL,
                                  LOOPY_NET_RESOLVE_NONE);
}

bool loopyNetResolveExtra(loopyNet *l, const char *host,
                          loopyNetResolveData *full) {
    return loopyNetResolveGeneric(l, host, full->ip, sizeof(full->ip), full,
                                  LOOPY_NET_RESOLVE_NONE);
}

bool loopyNetResolveIP(loopyNet *l, const char *host, char *ipbuf,
                       size_t ipbufLen) {
    return loopyNetResolveGeneric(l, host, ipbuf, ipbufLen, NULL,
                                  LOOPY_NET_RESOLVE_ONLY_IP);
}

bool loopyNetResolveIPExtra(loopyNet *l, const char *host,
                            loopyNetResolveData *full) {
    return loopyNetResolveGeneric(l, host, full->ip, sizeof(full->ip), full,
                                  LOOPY_NET_RESOLVE_ONLY_IP);
}

/* ====================================================================
 * Disable SIGPIPE management
 * ==================================================================== */
static bool loopyNetSetDisableSIGPIPEOnWrite(loopyNet *l, int fd) {
#if defined(SO_NOSIGPIPE)
    if (!enableSocketOption(fd, SOL_SOCKET, SO_NOSIGPIPE)) {
        loopyNetSetError(l, "nosigpipe");
        return false;
    }

    return true;
#else
    (void)fd;
    (void)l;
    return false;
#endif
}

/* ====================================================================
 * Address Reuse Management
 * ==================================================================== */
static bool loopyNetSetReuseAddr(loopyNet *l) {
    /* Fixes some high frequency socket creation race condition errors */
    if (!enableSocketOption(l->sock, SOL_SOCKET, SO_REUSEADDR)) {
        loopyNetSetError(l, "reuseaddr");
        return false;
    }

    return true;
}

/* ====================================================================
 * Port Reuse Management
 * ==================================================================== */
static bool loopyNetSetReusePort(loopyNet *l) {
    /* Linux 3.9 kernel added a built in L4 load balancer.
     * The kernel will dispatch incoming connections to multiple
     * processes/threads bound to the same port with this option enabled:
     * ---------------------
     For TCP sockets, this option allows accept(2) load
     distribution in a multi-threaded server to be improved by
     using a distinct listener socket for each thread.
     * ---------------------
     * Linux 4.6 added the ability to inject custom user-defined load balancing
     * into the kernel for REUSEPORT behavior using the eBPF mechanism.
     * Also see SO_ATTACH_REUSEPORT_EBPF
     *
     * FreeBSD and OS X also have this option, but they do *not* perform load
     * balancing.  FreeBSD just sends all traffic to the *last* listener bound,
     * while OS X sends all traffic to the *first* listener bound. */
    if (!enableSocketOption(l->sock, SOL_SOCKET, SO_REUSEPORT)) {
        loopyNetSetError(l, "reuseport");
        return false;
    }

    return true;
}

static bool loopyNetL4LoadBalanceEnable(loopyNet *l) {
#if LOOPY_NET_HAS_KERNEL_LOAD_BALANCER
    return loopyNetSetReusePort(l);
#else
    loopyNetSetError(l, "Kernel load balancing only supported on Linux >= 3.9");
    return false;
#endif
}

/* ====================================================================
 * Socket Closing
 * ==================================================================== */
static bool loopyNetSocketClose(loopyNet *l) {
    if (l->sock != LN_SOCK_CLOSED) {
        if (close(l->sock) == -1) {
            loopyNetSetError(l, "close");
            return false;
        }

        l->sock = LN_SOCK_CLOSED;
    }

    return true;
}

/* ====================================================================
 * Socket Creation
 * ==================================================================== */
static bool loopyNetSocketNewFromAddrinfo(loopyNet *l, struct addrinfo *ai) {
    const int sock =
        socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);

    /* If this type of socket isn't supported, we can't do anything else. */
    if (errno == EAFNOSUPPORT) {
        loopyNetSetError(l, "socket");
        return false;
    }

    if (sock == LN_SOCK_CLOSED) {
        return false;
    }

    l->sock = sock;
    return true;
}

static bool loopyNetSocketNew(loopyNet *l, int domain) {
    if ((l->sock = socket(domain, SOCK_STREAM | SOCK_CLOEXEC, 0)) == -1) {
        loopyNetSetError(l, "socket");
        return false;
    }

    if (!loopyNetSetReuseAddr(l)) {
        loopyNetSocketClose(l);
        return false;
    }

    /* No error checking because not all platforms support it. */
    loopyNetSetDisableSIGPIPEOnWrite(l, l->sock);

    return true;
}

/* ====================================================================
 * TCP Connecting
 * ==================================================================== */
typedef enum loopyNetConnectFlag {
    LOOPY_NET_CONNECT_NONE = 0x00,
    LOOPY_NET_CONNECT_NONBLOCK = 0x01,
    LOOPY_NET_CONNECT_BEST_EFFORT_BIND = 0x02,
} loopyNetConnectFlag;

/* TCP_FASTOPEN (client) on macOS requires:
 * connectx(socket, &endpoints, SAE_ASSOCID_ANY, CONNECT_RESUME_ON_READ_WRITE |
 * CONNECT_DATA_IDEMPOTENT, ...);
 * TCP_FASTOPEN (client) on Linux skips connect() and uses:
 * sendto(fd, buffer, buf_len, MSG_FASTOPEN, ...);
 */
/* TFO details at https://bradleyf.id.au/nix/shaving-your-rtt-wth-tfo/
 * macOS: sysctl net.inet.tcp.fastopen (default == 3)
 * Linux: sysctl net.ipv4.tcp_fastopen (default == 1) */
static bool loopyNetTcpGenericConnect(loopyNet *l, const char *addr, int port,
                                      const char *bindAddr,
                                      loopyNetConnectFlag flags) {
    int rv;
    char portstr[6]; /* strlen("65535") + 1; */
    struct addrinfo hints = {0};
    struct addrinfo *servinfo;
    struct addrinfo *bservinfo = NULL;

    snprintf(portstr, sizeof(portstr), "%d", port);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if ((rv = getaddrinfo(addr, portstr, &hints, &servinfo)) != 0) {
        loopyNetSetError(l, gai_strerror(rv));
        return false;
    }

    /* Resolve bind address ONCE before the connection loop.
     * This avoids redundant getaddrinfo() calls when servinfo has multiple
     * entries (e.g., both IPv4 and IPv6 addresses). */
    if (bindAddr) {
        if ((rv = getaddrinfo(bindAddr, NULL, &hints, &bservinfo)) != 0) {
            loopyNetSetError(l, gai_strerror(rv));
            freeaddrinfo(servinfo);
            return false;
        }
    }

    struct addrinfo *p = NULL;
    for (p = servinfo; p != NULL; p = p->ai_next) {
        /* Try to create the socket and to connect it.
         * If fail in socket() or connect(), retry next entry in servinfo. */
        if (!loopyNetSocketNewFromAddrinfo(l, p)) {
            continue;
        }

        if (!loopyNetSetReuseAddr(l)) {
            goto error;
        }

        if ((flags & LOOPY_NET_CONNECT_NONBLOCK) &&
            !loopyNetNonBlockEnable(l)) {
            goto error;
        }

        if (bservinfo) {
            bool bound = false;
            for (struct addrinfo *b = bservinfo; b != NULL; b = b->ai_next) {
                if (bind(l->sock, b->ai_addr, b->ai_addrlen) != -1) {
                    bound = true;
                    break;
                }
            }

            if (!bound) {
                loopyNetSetError(l, "bind");
                goto error;
            }
        }

        if (connect(l->sock, p->ai_addr, p->ai_addrlen) == -1) {
            /* If socket is non-blocking, EINPROGRESS is valid here. */
            if ((errno == EINPROGRESS) &&
                (flags & LOOPY_NET_CONNECT_NONBLOCK)) {
                goto end;
            }

            loopyNetSocketClose(l);
            continue;
        }

        /* If we ended an iteration of the for loop without errors, we
         * have a connected socket. Let's return to the caller. */
        goto end;
    }

    if (p == NULL) {
        loopyNetSetError(l, "other");
    }

error:
    loopyNetSocketClose(l);

end:
    freeaddrinfo(servinfo);
    if (bservinfo) {
        freeaddrinfo(bservinfo);
    }

    /* Best effort binding:
     *   if specific 'bindAddr' was requested, but we couldn't actually
     *   bind to 'bindAddr', then try to listen again with no bind parameter. */
    if (l->sock == LN_SOCK_CLOSED && bindAddr &&
        (flags & LOOPY_NET_CONNECT_BEST_EFFORT_BIND)) {
        return loopyNetTcpGenericConnect(l, addr, port, NULL, flags);
    }

    return true;
}

bool loopyNetTcpConnect(loopyNet *l, const char *addr, int port) {
    return loopyNetTcpGenericConnect(l, addr, port, NULL,
                                     LOOPY_NET_CONNECT_NONE);
}

bool loopyNetTcpNonBlockConnect(loopyNet *l, const char *addr, int port) {
    return loopyNetTcpGenericConnect(l, addr, port, NULL,
                                     LOOPY_NET_CONNECT_NONBLOCK);
}

bool loopyNetTcpNonBlockConnectBind(loopyNet *l, const char *addr, int port,
                                    const char *bindAddr) {
    return loopyNetTcpGenericConnect(l, addr, port, bindAddr,
                                     LOOPY_NET_CONNECT_NONBLOCK);
}

bool loopyNetTcpNonBlockConnectBindOptional(loopyNet *l, const char *addr,
                                            int port, const char *bindAddr) {
    return loopyNetTcpGenericConnect(l, addr, port, bindAddr,
                                     LOOPY_NET_CONNECT_NONBLOCK |
                                         LOOPY_NET_CONNECT_BEST_EFFORT_BIND);
}

/* ====================================================================
 * Domain Socket Connecting
 * ==================================================================== */
static bool loopyNetUnixGenericConnect(loopyNet *l, const char *path,
                                       int flags) {
    struct sockaddr_un sa;

    if (!loopyNetSocketNew(l, AF_LOCAL)) {
        return false;
    }

    sa.sun_family = AF_LOCAL;
    strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
    if (flags & LOOPY_NET_CONNECT_NONBLOCK) {
        if (!loopyNetNonBlockEnable(l)) {
            return false;
        }
    }

    if (connect(l->sock, (struct sockaddr *)&sa, sizeof(sa)) == -1) {
        if (errno == EINPROGRESS && (flags & LOOPY_NET_CONNECT_NONBLOCK)) {
            return true;
        }

        loopyNetSetError(l, "connect");
        loopyNetSocketClose(l);
        return false;
    }

    return true;
}

bool loopyNetUnixConnect(loopyNet *l, const char *path) {
    return loopyNetUnixGenericConnect(l, path, LOOPY_NET_CONNECT_NONE);
}

bool loopyNetUnixNonBlockConnect(loopyNet *l, const char *path) {
    return loopyNetUnixGenericConnect(l, path, LOOPY_NET_CONNECT_NONBLOCK);
}

/* ====================================================================
 * Listening
 * ==================================================================== */
static bool loopyNetListen(loopyNet *l, struct sockaddr *sa, socklen_t len,
                           int backlog) {
    if (l->listenFlag & LOOPY_NET_SERVER_REUSEPORT) {
        if (!loopyNetSetReusePort(l)) {
            return false;
        }
    }

    if (l->listenFlag & LOOPY_NET_SERVER_L4_LOAD_BALANCE) {
        // cppcheck-suppress knownConditionTrueFalse
        if (!loopyNetL4LoadBalanceEnable(l)) {
            return false;
        }
    }

    if (bind(l->sock, sa, len) == -1) {
        loopyNetSetError(l, "bind");
        loopyNetSocketClose(l);
        return false;
    }

    if (listen(l->sock, backlog) == -1) {
        loopyNetSetError(l, "listen");
        loopyNetSocketClose(l);
        return false;
    }

    return true;
}

/* ====================================================================
 * IPv6-Only
 * ==================================================================== */
static bool loopyNetV6Only(loopyNet *l) {
    if (!enableSocketOption(l->sock, IPPROTO_IPV6, IPV6_V6ONLY)) {
        loopyNetSetError(l, "setsockopt");
        return false;
    }

    return true;
}

/* ====================================================================
 * Server Setup
 * ==================================================================== */
static bool _loopyNetTcpServer(loopyNet *l, int port, const char *bindAddr,
                               int af, int backlog) {
    int rv;
    char _port[6] = {0}; /* strlen("65535") + 1 */
    struct addrinfo hints = {0};
    struct addrinfo *servinfo;
    struct addrinfo *p;

    snprintf(_port, sizeof(_port), "%d", port);
    hints.ai_family = af;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE; /* No effect if bindAddr != NULL */

    if ((rv = getaddrinfo(bindAddr, _port, &hints, &servinfo)) != 0) {
        loopyNetSetError(l, gai_strerror(rv));
        return false;
    }

    for (p = servinfo; p != NULL; p = p->ai_next) {
        if (!loopyNetSocketNewFromAddrinfo(l, p)) {
            continue;
        }

        if (af == AF_INET6 && !loopyNetV6Only(l)) {
            loopyNetSocketClose(l);
            break;
        }

        if (!loopyNetSetReuseAddr(l)) {
            loopyNetSocketClose(l);
            break;
        }

        if (!loopyNetListen(l, p->ai_addr, p->ai_addrlen, backlog)) {
            break;
        }

        /* Once we have a working socket, return success. */
        freeaddrinfo(servinfo);
        return true;
    }

    if (p == NULL) {
        loopyNetSetError(l, "bind");
    }

    freeaddrinfo(servinfo);
    return false;
}

bool loopyNetTcp4Server(loopyNet *l, int port, const char *bindAddr,
                        int backlog) {
    return _loopyNetTcpServer(l, port, bindAddr, AF_INET, backlog);
}

bool loopyNetTcp6Server(loopyNet *l, int port, const char *bindAddr,
                        int backlog) {
    return _loopyNetTcpServer(l, port, bindAddr, AF_INET6, backlog);
}

bool loopyNetUnixServer(loopyNet *l, const char *path, mode_t perm,
                        int backlog) {
    struct sockaddr_un sa = {0};

    if (!loopyNetSocketNew(l, AF_LOCAL)) {
        return false;
    }

    sa.sun_family = AF_LOCAL;
    strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
    if (!loopyNetListen(l, (struct sockaddr *)&sa, sizeof(sa), backlog)) {
        return false;
    }

    if (perm) {
        chmod(sa.sun_path, perm);
    }

    return true;
}

/* ====================================================================
 * Socket Accepting
 * ==================================================================== */
static bool loopyNetGenericAccept(loopyNet *l, struct sockaddr *sa,
                                  socklen_t *len, int *fd) {
    while (true) {
        *fd = accept(l->sock, sa, len);
        if (*fd == -1) {
            if (errno == EINTR) {
                continue;
            }

            loopyNetSetError(l, "accept");
            return false;
        }
        break;
    }

    return true;
}

#ifdef USE_ACCEPT4
static bool loopyNetGenericAccept4NonBlock(loopyNet *l, struct sockaddr *sa,
                                           socklen_t *len, int *fd) {
    while (true) {
        *fd = accept4(l->sock, sa, len, SOCK_NONBLOCK);
        if (*fd == -1) {
            if (errno == EINTR) {
                continue;
            }

            loopyNetSetError(l, "accept4");
            return false;
        }
        break;
    }

    return true;
}
#endif

static void _loopyNetPopulateIPPortFromSockaddr(struct sockaddr_storage *sa,
                                                char *ip, size_t ipLen,
                                                int *port) {
    if (sa->ss_family == AF_INET) {
        struct sockaddr_in *src = (struct sockaddr_in *)sa;
        if (ip) {
            inet_ntop(AF_INET, (void *)&(src->sin_addr), ip, ipLen);
        }

        if (port) {
            *port = ntohs(src->sin_port);
        }
    } else {
        struct sockaddr_in6 *src = (struct sockaddr_in6 *)sa;
        if (ip) {
            inet_ntop(AF_INET6, (void *)&(src->sin6_addr), ip, ipLen);
        }

        if (port) {
            *port = ntohs(src->sin6_port);
        }
    }
}

bool loopyNetTcpAcceptNonBlock(loopyNet *l, char *ip, size_t ipLen, int *port,
                               int *cliSock) {
    struct sockaddr_storage sa = {0};
    socklen_t salen = sizeof(sa);

#if USE_ACCEPT4
    /* accept4() avoids two fcntl() calls on platforms with accept4() */
    if (!loopyNetGenericAccept4NonBlock(l, (struct sockaddr *)&sa, &salen,
                                        cliSock)) {
        return false;
    }
#else
    if (!loopyNetGenericAccept(l, (struct sockaddr *)&sa, &salen, cliSock)) {
        return false;
    }

    loopyNetSetNonBlock(l, *cliSock, true);
#endif

    loopyNetSetDisableSIGPIPEOnWrite(l, *cliSock);
    _loopyNetPopulateIPPortFromSockaddr(&sa, ip, ipLen, port);
    return true;
}

bool loopyNetTcpAccept(loopyNet *l, char *ip, size_t ipLen, int *port,
                       int *cliSock) {
    struct sockaddr_storage sa;
    socklen_t salen = sizeof(sa);
    if (!loopyNetGenericAccept(l, (struct sockaddr *)&sa, &salen, cliSock)) {
        return false;
    }

    loopyNetSetDisableSIGPIPEOnWrite(l, *cliSock);
    _loopyNetPopulateIPPortFromSockaddr(&sa, ip, ipLen, port);
    return true;
}

bool loopyNetUnixAcceptNonBlock(loopyNet *l, int *cliSock) {
    struct sockaddr_un sa;
    socklen_t salen = sizeof(sa);

#if USE_ACCEPT4
    /* Accept4 avoids two fcntl() calls on Linux */
    if (!loopyNetGenericAccept4NonBlock(l, (struct sockaddr *)&sa, &salen,
                                        cliSock)) {
        return false;
    }
#else
    if (!loopyNetGenericAccept(l, (struct sockaddr *)&sa, &salen, cliSock)) {
        return false;
    }
    loopyNetSetNonBlock(l, *cliSock, true);
#endif

    return true;
}

bool loopyNetUnixAccept(loopyNet *l, int *cliSock) {
    struct sockaddr_un sa;
    socklen_t salen = sizeof(sa);
    if (!loopyNetGenericAccept(l, (struct sockaddr *)&sa, &salen, cliSock)) {
        return false;
    }

    return true;
}

/* ====================================================================
 * Address Parsing and Formatting
 * ==================================================================== */

bool loopyNetAddrIsIPv6(const char *addr) {
    return addr && strchr(addr, ':') != NULL;
}

bool loopyNetParseAddr(const char *addr, char *host, size_t hostLen, int *port,
                       int defaultPort) {
    if (!addr || !host || hostLen == 0 || !port) {
        return false;
    }

    *port = defaultPort;
    const char *hostStart = addr;
    const char *hostEnd = NULL;
    const char *portStart = NULL;

    if (addr[0] == '[') {
        /* IPv6 bracketed notation: [::1]:port or [2001:db8::1]:port */
        hostStart = addr + 1;
        const char *bracket = strchr(hostStart, ']');
        if (!bracket) {
            return false; /* Missing closing bracket */
        }

        hostEnd = bracket;
        if (bracket[1] == ':') {
            portStart = bracket + 2;
        } else if (bracket[1] != '\0') {
            return false; /* Invalid character after bracket */
        }
    } else {
        /* IPv4, hostname, or bare IPv6 (unusual) */
        int colonCount = 0;
        const char *lastColon = NULL;

        for (const char *p = addr; *p; p++) {
            if (*p == ':') {
                colonCount++;
                lastColon = p;
            }
        }

        if (colonCount == 0) {
            /* No colon = just host, no port */
            hostEnd = addr + strlen(addr);
        } else if (colonCount == 1) {
            /* Single colon = host:port (IPv4 or hostname) */
            hostEnd = lastColon;
            portStart = lastColon + 1;
        } else {
            /* Multiple colons = bare IPv6 without brackets
             * Treat entire string as host (no port extractable) */
            hostEnd = addr + strlen(addr);
        }
    }

    /* Copy host (without brackets for IPv6) */
    size_t len = hostEnd - hostStart;
    if (len >= hostLen) {
        len = hostLen - 1;
    }

    memcpy(host, hostStart, len);
    host[len] = '\0';

    /* Parse port if present */
    if (portStart && *portStart) {
        int p = atoi(portStart);
        if (p > 0 && p <= 65535) {
            *port = p;
        }
    }

    return true;
}

bool loopyNetPeerToString(int sock, char *ip, size_t ipLen, int *port) {
    if ((ip && ipLen == 0) || (!ip && !port)) {
        return false;
    }

    struct sockaddr_storage sa;
    socklen_t salen = sizeof(sa);
    if (getpeername(sock, (struct sockaddr *)&sa, &salen) == 0) {
        if (sa.ss_family == AF_INET) {
            const struct sockaddr_in *s = (struct sockaddr_in *)&sa;
            if (ip) {
                inet_ntop(AF_INET, (const void *)&(s->sin_addr), ip, ipLen);
            }

            if (port) {
                *port = ntohs(s->sin_port);
            }
        } else if (sa.ss_family == AF_INET6) {
            const struct sockaddr_in6 *s = (struct sockaddr_in6 *)&sa;
            if (ip) {
                inet_ntop(AF_INET6, (const void *)&(s->sin6_addr), ip, ipLen);
            }

            if (port) {
                *port = ntohs(s->sin6_port);
            }
        } else if (sa.ss_family == AF_UNIX) {
            if (ip) {
                strncpy(ip, "[unix]", ipLen);
            }

            if (port) {
                *port = 0;
            }
        }

        return true;
    }

    /* else, we have no real information, so populate error data. */
    if (ip) {
        if (ipLen >= 2) {
            ip[0] = '?';
            ip[1] = '\0';
        } else if (ipLen == 1) {
            ip[0] = '\0';
        }
    }

    if (port) {
        *port = -1;
    }

    return false;
}

int loopyNetFormatAddr(char *buf, size_t bufLen, const char *ip, int port) {
    return snprintf(buf, bufLen, strchr(ip, ':') ? "[%s]:%d" : "%s:%d", ip,
                    port);
}

/* Like loopyNetFormatAddr() but extract ip and port from the socket's peer. */
int loopyNetFormatPeer(int fd, char *buf, size_t bufLen) {
    char ip[INET6_ADDRSTRLEN]; /* snprintf will terminate this for us */
    int port = 0;

    const bool createdString = loopyNetPeerToString(fd, ip, sizeof(ip), &port);
    assert(createdString);
    (void)createdString;

    return loopyNetFormatAddr(buf, bufLen, ip, port);
}

/* Like loopyNetFormatPeer() but use this side of the socket. */
int loopyNetFormatSock(int fd, char *buf, size_t bufLen) {
    char ip[INET6_ADDRSTRLEN] = {0};
    int port = 0;

    loopyNetSockName(fd, ip, sizeof(ip), &port);
    return loopyNetFormatAddr(buf, bufLen, ip, port);
}

bool loopyNetSockName(int sock, char *ip, size_t ipLen, int *port) {
    struct sockaddr_storage sa;
    socklen_t salen = sizeof(sa);

    if (getsockname(sock, (struct sockaddr *)&sa, &salen) == -1) {
        if (port) {
            *port = 0;
        }

        ip[0] = '?';
        ip[1] = '\0';
        return false;
    }

    if (sa.ss_family == AF_INET) {
        const struct sockaddr_in *s = (struct sockaddr_in *)&sa;
        if (ip) {
            inet_ntop(AF_INET, (const void *)&(s->sin_addr), ip, ipLen);
        }

        if (port) {
            *port = ntohs(s->sin_port);
        }
    } else {
        const struct sockaddr_in6 *s = (struct sockaddr_in6 *)&sa;
        if (ip) {
            inet_ntop(AF_INET6, (const void *)&(s->sin6_addr), ip, ipLen);
        }

        if (port) {
            *port = ntohs(s->sin6_port);
        }
    }

    return true;
}

/* Originally, before updating to modern style and standards:
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2014, Matt Stancliff <matt@genges.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
