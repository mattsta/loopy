/* loopyUDP - UDP networking for loopy event loop
 *
 * Connectionless datagram networking for DNS, QUIC, gaming, streaming
 * protocols.
 *
 * Copyright 2024 Matt Stancliff <matt@genges.com>
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

#include "../deps/datakit/src/datakit.h"
#include "loopyUDP.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/udp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Platform detection for batch syscalls */
#if defined(__linux__)
#define LOOPY_HAS_RECVMMSG 1
#define LOOPY_HAS_SENDMMSG 1
#else
#define LOOPY_HAS_RECVMMSG 0
#define LOOPY_HAS_SENDMMSG 0
#endif

/* Platform detection for advanced UDP features (Linux 4.18+/5.0+) */
#if defined(__linux__)
#include <linux/version.h>

/* UDP GSO - Generic Segmentation Offload (Linux 4.18+) */
#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 18, 0)
#define LOOPY_HAS_UDP_GSO 1
#else
#define LOOPY_HAS_UDP_GSO 0
#endif

/* UDP GRO - Generic Receive Offload (Linux 5.0+) */
#ifndef UDP_GRO
#define UDP_GRO 104
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
#define LOOPY_HAS_UDP_GRO 1
#else
#define LOOPY_HAS_UDP_GRO 0
#endif

/* PMTU Discovery (Linux 2.2+, but we use modern constants) */
#include <netinet/ip.h>
#ifndef IP_MTU_DISCOVER
#define IP_MTU_DISCOVER 10
#endif
#ifndef IPV6_MTU_DISCOVER
#define IPV6_MTU_DISCOVER 23
#endif
#ifndef IP_PMTUDISC_DONT
#define IP_PMTUDISC_DONT 0
#endif
#ifndef IP_PMTUDISC_WANT
#define IP_PMTUDISC_WANT 1
#endif
#ifndef IP_PMTUDISC_DO
#define IP_PMTUDISC_DO 2
#endif
#ifndef IP_PMTUDISC_PROBE
#define IP_PMTUDISC_PROBE 3
#endif
#define LOOPY_HAS_PMTU 1

#else
#define LOOPY_HAS_UDP_GSO 0
#define LOOPY_HAS_UDP_GRO 0
#define LOOPY_HAS_PMTU 0
#endif

/* Maximum UDP datagram size (practical limit with IPv4) */
#define UDP_RECV_BUFFER_SIZE 65535

/* Only available in Linux 2.6.27+ */
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif

/* ====================================================================
 * Internal data structures
 * ==================================================================== */

typedef struct loopyUDPSendReq {
    struct sockaddr_storage dest;
    socklen_t destLen;
    void *data;
    size_t len;
    loopyUDPSendCallback *cb;
    void *userData;
    struct loopyUDPSendReq *next;
} loopyUDPSendReq;

struct loopyUDP {
    loopyLoop *loop;
    void *userData; /* User data for handle accessors */
    int fd;
    int af; /* AF_INET or AF_INET6 */
    bool bound;
    bool connected;
    bool receiving;

    /* Receive state */
    loopyUDPRecvCallback *recvCb;
    void *recvUserData;
    char recvBuffer[UDP_RECV_BUFFER_SIZE];

    /* Send queue */
    loopyUDPSendReq *sendHead;
    loopyUDPSendReq *sendTail;
    size_t sendQueueCount;

    /* Advanced UDP features */
    bool gsoEnabled;
    uint16_t gsoSegmentSize;
    bool groEnabled;
    loopyUDPPMTUMode pmtuMode;

    /* Error tracking */
    char errorString[128];
};

/* ====================================================================
 * Forward declarations
 * ==================================================================== */

static void udpSetError(loopyUDP *udp, const char *field);
static bool udpSetNonBlock(loopyUDP *udp);
static void udpReadCallback(loopyLoop *l, int fd, void *clientData,
                            loopyAction mask);
static void udpWriteCallback(loopyLoop *l, int fd, void *clientData,
                             loopyAction mask);
static bool udpResolveAddress(loopyUDP *udp, const char *addr, int port,
                              struct sockaddr_storage *out, socklen_t *outLen);
static void udpProcessSendQueue(loopyUDP *udp);

/* ====================================================================
 * Error Management
 * ==================================================================== */

static void udpSetError(loopyUDP *udp, const char *field) {
    snprintf(udp->errorString, sizeof(udp->errorString), "%s: %s", field,
             strerror(errno));
}

/* ====================================================================
 * Socket helpers
 * ==================================================================== */

static bool udpSetNonBlock(loopyUDP *udp) {
    int flags = fcntl(udp->fd, F_GETFL);
    if (flags == -1) {
        udpSetError(udp, "fcntl getfl");
        return false;
    }

    if (fcntl(udp->fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        udpSetError(udp, "fcntl setfl");
        return false;
    }

    return true;
}

static bool udpResolveAddress(loopyUDP *udp, const char *addr, int port,
                              struct sockaddr_storage *out, socklen_t *outLen) {
    char portStr[6];
    snprintf(portStr, sizeof(portStr), "%d", port);

    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;

    struct addrinfo *info;
    int rv = getaddrinfo(addr, portStr, &hints, &info);
    if (rv != 0) {
        snprintf(udp->errorString, sizeof(udp->errorString), "getaddrinfo: %s",
                 gai_strerror(rv));
        return false;
    }

    memcpy(out, info->ai_addr, info->ai_addrlen);
    *outLen = info->ai_addrlen;
    freeaddrinfo(info);
    return true;
}

/* ====================================================================
 * Lifecycle
 * ==================================================================== */

loopyUDP *loopyUDPNew(loopyLoop *loop) {
    if (!loop) {
        return NULL;
    }

    loopyUDP *udp = zcalloc(1, sizeof(*udp));
    if (!udp) {
        return NULL;
    }

    udp->loop = loop;
    udp->fd = -1;
    udp->af = AF_UNSPEC;

    return udp;
}

void loopyUDPFree(loopyUDP *udp) {
    if (!udp) {
        return;
    }

    /* Stop receiving */
    loopyUDPRecvStop(udp);

    /* Close socket */
    if (udp->fd >= 0) {
        loopyUnregisterReadWrite(udp->loop, udp->fd);
        close(udp->fd);
    }

    /* Free pending send requests */
    loopyUDPSendReq *req = udp->sendHead;
    while (req) {
        loopyUDPSendReq *next = req->next;
        if (req->cb) {
            req->cb(udp, -1, req->userData); /* Notify send failed */
        }
        zfree(req);
        req = next;
    }

    zfree(udp);
}

/* ====================================================================
 * Binding
 * ==================================================================== */

static bool udpCreateSocket(loopyUDP *udp, int af, unsigned flags) {
    /* Create socket if not already created */
    if (udp->fd < 0) {
        udp->fd = socket(af, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (udp->fd < 0) {
            udpSetError(udp, "socket");
            return false;
        }
        udp->af = af;

        if (!udpSetNonBlock(udp)) {
            close(udp->fd);
            udp->fd = -1;
            return false;
        }
    }

    /* Apply socket options based on flags */
    if (flags & LOOPY_UDP_REUSEADDR) {
        int val = 1;
        if (setsockopt(udp->fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) <
            0) {
            udpSetError(udp, "setsockopt reuseaddr");
            return false;
        }
    }

    if (flags & LOOPY_UDP_REUSEPORT) {
        int val = 1;
        if (setsockopt(udp->fd, SOL_SOCKET, SO_REUSEPORT, &val, sizeof(val)) <
            0) {
            udpSetError(udp, "setsockopt reuseport");
            return false;
        }
    }

    if ((flags & LOOPY_UDP_IPV6ONLY) && af == AF_INET6) {
        int val = 1;
        if (setsockopt(udp->fd, IPPROTO_IPV6, IPV6_V6ONLY, &val, sizeof(val)) <
            0) {
            udpSetError(udp, "setsockopt v6only");
            return false;
        }
    }

    return true;
}

bool loopyUDPBind(loopyUDP *udp, const char *addr, int port, unsigned flags) {
    if (!udp || udp->bound) {
        return false;
    }

    if (!udpCreateSocket(udp, AF_INET, flags)) {
        return false;
    }

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);

    if (!addr || addr[0] == '\0') {
        sa.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1) {
            udpSetError(udp, "inet_pton");
            return false;
        }
    }

    if (bind(udp->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        udpSetError(udp, "bind");
        return false;
    }

    udp->bound = true;
    return true;
}

bool loopyUDPBind6(loopyUDP *udp, const char *addr, int port, unsigned flags) {
    if (!udp || udp->bound) {
        return false;
    }

    if (!udpCreateSocket(udp, AF_INET6, flags)) {
        return false;
    }

    struct sockaddr_in6 sa = {0};
    sa.sin6_family = AF_INET6;
    sa.sin6_port = htons(port);

    if (!addr || addr[0] == '\0') {
        sa.sin6_addr = in6addr_any;
    } else {
        if (inet_pton(AF_INET6, addr, &sa.sin6_addr) != 1) {
            udpSetError(udp, "inet_pton");
            return false;
        }
    }

    if (bind(udp->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        udpSetError(udp, "bind");
        return false;
    }

    udp->bound = true;
    return true;
}

/* ====================================================================
 * Connect (Optional - for default destination)
 * ==================================================================== */

bool loopyUDPConnect(loopyUDP *udp, const char *addr, int port) {
    if (!udp || !addr) {
        return false;
    }

    struct sockaddr_storage dest;
    socklen_t destLen;
    if (!udpResolveAddress(udp, addr, port, &dest, &destLen)) {
        return false;
    }

    /* Create socket if needed */
    if (udp->fd < 0) {
        if (!udpCreateSocket(udp, dest.ss_family, 0)) {
            return false;
        }
    }

    if (connect(udp->fd, (struct sockaddr *)&dest, destLen) < 0) {
        udpSetError(udp, "connect");
        return false;
    }

    udp->connected = true;
    return true;
}

void loopyUDPDisconnect(loopyUDP *udp) {
    if (!udp || !udp->connected || udp->fd < 0) {
        return;
    }

    /* Disconnect by connecting to AF_UNSPEC */
    struct sockaddr sa = {0};
    sa.sa_family = AF_UNSPEC;
    connect(udp->fd, &sa, sizeof(sa));
    udp->connected = false;
}

bool loopyUDPIsConnected(const loopyUDP *udp) {
    return udp ? udp->connected : false;
}

/* ====================================================================
 * Receiving
 * ==================================================================== */

static void udpReadCallback(loopyLoop *l, int fd, void *clientData,
                            loopyAction mask) {
    (void)l;
    (void)fd;
    (void)mask;

    loopyUDP *udp = clientData;
    struct sockaddr_storage addr;
    socklen_t addrLen = sizeof(addr);

    ssize_t nread = recvfrom(udp->fd, udp->recvBuffer, sizeof(udp->recvBuffer),
                             0, (struct sockaddr *)&addr, &addrLen);

    if (nread < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return; /* No data available */
        }
        /* Report error to callback */
        if (udp->recvCb) {
            udp->recvCb(udp, -1, NULL, NULL, 0, udp->recvUserData);
        }
        return;
    }

    /* Deliver data to callback */
    if (udp->recvCb) {
        udp->recvCb(udp, nread, udp->recvBuffer, (struct sockaddr *)&addr,
                    addrLen, udp->recvUserData);
    }
}

bool loopyUDPRecvStart(loopyUDP *udp, loopyUDPRecvCallback *cb,
                       void *userData) {
    if (!udp || !cb || udp->fd < 0) {
        return false;
    }

    /* Need to have a socket (bound or connected) */
    if (!udp->bound && !udp->connected) {
        /* Auto-bind to any address */
        if (!loopyUDPBind(udp, NULL, 0, 0)) {
            return false;
        }
    }

    udp->recvCb = cb;
    udp->recvUserData = userData;

    if (!loopyRegisterRead(udp->loop, udp->fd, udpReadCallback, udp)) {
        return false;
    }

    udp->receiving = true;
    return true;
}

void loopyUDPRecvStop(loopyUDP *udp) {
    if (!udp || !udp->receiving) {
        return;
    }

    loopyUnregisterRead(udp->loop, udp->fd);
    udp->receiving = false;
    udp->recvCb = NULL;
    udp->recvUserData = NULL;
}

bool loopyUDPIsReceiving(const loopyUDP *udp) {
    return udp ? udp->receiving : false;
}

/* ====================================================================
 * Sending
 * ==================================================================== */

static void udpWriteCallback(loopyLoop *l, int fd, void *clientData,
                             loopyAction mask) {
    (void)l;
    (void)fd;
    (void)mask;

    loopyUDP *udp = clientData;
    udpProcessSendQueue(udp);
}

static void udpProcessSendQueue(loopyUDP *udp) {
    while (udp->sendHead) {
        loopyUDPSendReq *req = udp->sendHead;

        ssize_t sent;
        if (req->destLen > 0) {
            sent = sendto(udp->fd, req->data, req->len, 0,
                          (struct sockaddr *)&req->dest, req->destLen);
        } else {
            /* Connected send */
            sent = send(udp->fd, req->data, req->len, 0);
        }

        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Socket buffer full, wait for writable */
                return;
            }
            /* Error - notify callback */
            if (req->cb) {
                req->cb(udp, -1, req->userData);
            }
        } else {
            /* Success */
            if (req->cb) {
                req->cb(udp, 0, req->userData);
            }
        }

        /* Remove from queue */
        udp->sendHead = req->next;
        if (!udp->sendHead) {
            udp->sendTail = NULL;
        }
        udp->sendQueueCount--;
        zfree(req);
    }

    /* Queue empty, stop write notifications */
    loopyUnregisterWrite(udp->loop, udp->fd);
}

bool loopyUDPSend(loopyUDP *udp, const char *addr, int port, const void *data,
                  size_t len, loopyUDPSendCallback *cb, void *userData) {
    if (!udp || !addr || !data || len == 0) {
        return false;
    }

    /* Create socket if needed */
    if (udp->fd < 0) {
        /* Need to determine address family from the destination */
        struct sockaddr_storage dest;
        socklen_t destLen;
        if (!udpResolveAddress(udp, addr, port, &dest, &destLen)) {
            return false;
        }
        if (!udpCreateSocket(udp, dest.ss_family, 0)) {
            return false;
        }
    }

    /* Try immediate send first */
    struct sockaddr_storage dest;
    socklen_t destLen;
    if (!udpResolveAddress(udp, addr, port, &dest, &destLen)) {
        return false;
    }

    if (!udp->sendHead) {
        /* Try non-queued send */
        ssize_t sent =
            sendto(udp->fd, data, len, 0, (struct sockaddr *)&dest, destLen);
        if (sent >= 0) {
            if (cb) {
                cb(udp, 0, userData);
            }
            return true;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            udpSetError(udp, "sendto");
            return false;
        }
    }

    /* Queue the send request */
    loopyUDPSendReq *req = zcalloc(1, sizeof(*req));
    if (!req) {
        return false;
    }

    memcpy(&req->dest, &dest, destLen);
    req->destLen = destLen;
    req->data = (void *)data; /* Note: caller must keep data valid */
    req->len = len;
    req->cb = cb;
    req->userData = userData;

    if (udp->sendTail) {
        udp->sendTail->next = req;
    } else {
        udp->sendHead = req;
    }
    udp->sendTail = req;
    udp->sendQueueCount++;

    /* Register for write events */
    loopyRegisterWrite(udp->loop, udp->fd, udpWriteCallback, udp);

    return true;
}

bool loopyUDPSendConnected(loopyUDP *udp, const void *data, size_t len,
                           loopyUDPSendCallback *cb, void *userData) {
    if (!udp || !udp->connected || !data || len == 0) {
        return false;
    }

    /* Try immediate send first */
    if (!udp->sendHead) {
        ssize_t sent = send(udp->fd, data, len, 0);
        if (sent >= 0) {
            if (cb) {
                cb(udp, 0, userData);
            }
            return true;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            udpSetError(udp, "send");
            return false;
        }
    }

    /* Queue the send request */
    loopyUDPSendReq *req = zcalloc(1, sizeof(*req));
    if (!req) {
        return false;
    }

    req->destLen = 0; /* Indicates connected send */
    req->data = (void *)data;
    req->len = len;
    req->cb = cb;
    req->userData = userData;

    if (udp->sendTail) {
        udp->sendTail->next = req;
    } else {
        udp->sendHead = req;
    }
    udp->sendTail = req;
    udp->sendQueueCount++;

    /* Register for write events */
    loopyRegisterWrite(udp->loop, udp->fd, udpWriteCallback, udp);

    return true;
}

ssize_t loopyUDPTrySend(loopyUDP *udp, const char *addr, int port,
                        const void *data, size_t len) {
    if (!udp || !addr || !data || len == 0) {
        errno = EINVAL;
        return -1;
    }

    /* Create socket if needed */
    if (udp->fd < 0) {
        struct sockaddr_storage dest;
        socklen_t destLen;
        if (!udpResolveAddress(udp, addr, port, &dest, &destLen)) {
            return -1;
        }
        if (!udpCreateSocket(udp, dest.ss_family, 0)) {
            return -1;
        }
    }

    struct sockaddr_storage dest;
    socklen_t destLen;
    if (!udpResolveAddress(udp, addr, port, &dest, &destLen)) {
        return -1;
    }

    return sendto(udp->fd, data, len, 0, (struct sockaddr *)&dest, destLen);
}

/* ====================================================================
 * Configuration
 * ==================================================================== */

bool loopyUDPSetBroadcast(loopyUDP *udp, bool enable) {
    if (!udp || udp->fd < 0) {
        return false;
    }

    int val = enable ? 1 : 0;
    if (setsockopt(udp->fd, SOL_SOCKET, SO_BROADCAST, &val, sizeof(val)) < 0) {
        udpSetError(udp, "setsockopt broadcast");
        return false;
    }

    return true;
}

bool loopyUDPSetTTL(loopyUDP *udp, int ttl) {
    if (!udp || udp->fd < 0 || ttl < 1 || ttl > 255) {
        return false;
    }

    int level = (udp->af == AF_INET6) ? IPPROTO_IPV6 : IPPROTO_IP;
    int opt = (udp->af == AF_INET6) ? IPV6_UNICAST_HOPS : IP_TTL;

    if (setsockopt(udp->fd, level, opt, &ttl, sizeof(ttl)) < 0) {
        udpSetError(udp, "setsockopt ttl");
        return false;
    }

    return true;
}

bool loopyUDPSetMulticastTTL(loopyUDP *udp, int ttl) {
    if (!udp || udp->fd < 0 || ttl < 1 || ttl > 255) {
        return false;
    }

    int level = (udp->af == AF_INET6) ? IPPROTO_IPV6 : IPPROTO_IP;
    int opt = (udp->af == AF_INET6) ? IPV6_MULTICAST_HOPS : IP_MULTICAST_TTL;

    if (setsockopt(udp->fd, level, opt, &ttl, sizeof(ttl)) < 0) {
        udpSetError(udp, "setsockopt multicast ttl");
        return false;
    }

    return true;
}

bool loopyUDPSetMulticastLoop(loopyUDP *udp, bool enable) {
    if (!udp || udp->fd < 0) {
        return false;
    }

    int level = (udp->af == AF_INET6) ? IPPROTO_IPV6 : IPPROTO_IP;
    int opt = (udp->af == AF_INET6) ? IPV6_MULTICAST_LOOP : IP_MULTICAST_LOOP;
    int val = enable ? 1 : 0;

    if (setsockopt(udp->fd, level, opt, &val, sizeof(val)) < 0) {
        udpSetError(udp, "setsockopt multicast loop");
        return false;
    }

    return true;
}

bool loopyUDPJoinMulticast(loopyUDP *udp, const char *group,
                           const char *iface) {
    if (!udp || !group || udp->fd < 0) {
        return false;
    }

    if (udp->af == AF_INET6) {
        struct ipv6_mreq mreq = {0};
        if (inet_pton(AF_INET6, group, &mreq.ipv6mr_multiaddr) != 1) {
            udpSetError(udp, "inet_pton multicast group");
            return false;
        }
        mreq.ipv6mr_interface = iface ? if_nametoindex(iface) : 0;

        if (setsockopt(udp->fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq,
                       sizeof(mreq)) < 0) {
            udpSetError(udp, "setsockopt join multicast");
            return false;
        }
    } else {
        struct ip_mreq mreq = {0};
        if (inet_pton(AF_INET, group, &mreq.imr_multiaddr) != 1) {
            udpSetError(udp, "inet_pton multicast group");
            return false;
        }
        if (iface) {
            if (inet_pton(AF_INET, iface, &mreq.imr_interface) != 1) {
                mreq.imr_interface.s_addr = INADDR_ANY;
            }
        } else {
            mreq.imr_interface.s_addr = INADDR_ANY;
        }

        if (setsockopt(udp->fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq,
                       sizeof(mreq)) < 0) {
            udpSetError(udp, "setsockopt join multicast");
            return false;
        }
    }

    return true;
}

bool loopyUDPLeaveMulticast(loopyUDP *udp, const char *group,
                            const char *iface) {
    if (!udp || !group || udp->fd < 0) {
        return false;
    }

    if (udp->af == AF_INET6) {
        struct ipv6_mreq mreq = {0};
        if (inet_pton(AF_INET6, group, &mreq.ipv6mr_multiaddr) != 1) {
            udpSetError(udp, "inet_pton multicast group");
            return false;
        }
        mreq.ipv6mr_interface = iface ? if_nametoindex(iface) : 0;

        if (setsockopt(udp->fd, IPPROTO_IPV6, IPV6_LEAVE_GROUP, &mreq,
                       sizeof(mreq)) < 0) {
            udpSetError(udp, "setsockopt leave multicast");
            return false;
        }
    } else {
        struct ip_mreq mreq = {0};
        if (inet_pton(AF_INET, group, &mreq.imr_multiaddr) != 1) {
            udpSetError(udp, "inet_pton multicast group");
            return false;
        }
        if (iface) {
            if (inet_pton(AF_INET, iface, &mreq.imr_interface) != 1) {
                mreq.imr_interface.s_addr = INADDR_ANY;
            }
        } else {
            mreq.imr_interface.s_addr = INADDR_ANY;
        }

        if (setsockopt(udp->fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq,
                       sizeof(mreq)) < 0) {
            udpSetError(udp, "setsockopt leave multicast");
            return false;
        }
    }

    return true;
}

/* ====================================================================
 * Information
 * ==================================================================== */

bool loopyUDPGetSockName(loopyUDP *udp, char *addr, size_t addrLen, int *port) {
    if (!udp || udp->fd < 0) {
        return false;
    }

    struct sockaddr_storage sa;
    socklen_t saLen = sizeof(sa);

    if (getsockname(udp->fd, (struct sockaddr *)&sa, &saLen) < 0) {
        udpSetError(udp, "getsockname");
        return false;
    }

    if (sa.ss_family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&sa;
        if (addr && addrLen > 0) {
            inet_ntop(AF_INET, &sin->sin_addr, addr, addrLen);
        }
        if (port) {
            *port = ntohs(sin->sin_port);
        }
    } else if (sa.ss_family == AF_INET6) {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&sa;
        if (addr && addrLen > 0) {
            inet_ntop(AF_INET6, &sin6->sin6_addr, addr, addrLen);
        }
        if (port) {
            *port = ntohs(sin6->sin6_port);
        }
    } else {
        return false;
    }

    return true;
}

int loopyUDPGetFd(const loopyUDP *udp) {
    return udp ? udp->fd : -1;
}

loopyLoop *loopyUDPGetLoop(const loopyUDP *udp) {
    return udp ? udp->loop : NULL;
}

size_t loopyUDPSendQueueCount(const loopyUDP *udp) {
    return udp ? udp->sendQueueCount : 0;
}

const char *loopyUDPGetError(const loopyUDP *udp) {
    return udp ? udp->errorString : "";
}

/* ====================================================================
 * Batch Operations
 * ==================================================================== */

bool loopyUDPHasNativeBatch(void) {
#if LOOPY_HAS_RECVMMSG && LOOPY_HAS_SENDMMSG
    return true;
#else
    return false;
#endif
}

#if LOOPY_HAS_RECVMMSG

int loopyUDPRecvMulti(loopyUDP *udp, loopyUDPMessage *msgs, int nmsg) {
    if (!udp || !msgs || nmsg <= 0 || udp->fd < 0) {
        errno = EINVAL;
        return -1;
    }

    if (nmsg > LOOPY_UDP_BATCH_MAX) {
        nmsg = LOOPY_UDP_BATCH_MAX;
    }

    /* Set up mmsghdr array for recvmmsg */
    struct mmsghdr mmsg[LOOPY_UDP_BATCH_MAX];
    struct iovec iov[LOOPY_UDP_BATCH_MAX];

    for (int i = 0; i < nmsg; i++) {
        iov[i].iov_base = msgs[i].data;
        iov[i].iov_len = msgs[i].len;

        mmsg[i].msg_hdr.msg_name = &msgs[i].addr;
        mmsg[i].msg_hdr.msg_namelen = sizeof(msgs[i].addr);
        mmsg[i].msg_hdr.msg_iov = &iov[i];
        mmsg[i].msg_hdr.msg_iovlen = 1;
        mmsg[i].msg_hdr.msg_control = NULL;
        mmsg[i].msg_hdr.msg_controllen = 0;
        mmsg[i].msg_hdr.msg_flags = 0;
        mmsg[i].msg_len = 0;
    }

    int ret = recvmmsg(udp->fd, mmsg, nmsg, MSG_DONTWAIT, NULL);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        udpSetError(udp, "recvmmsg");
        return -1;
    }

    /* Copy results back to caller's structures */
    for (int i = 0; i < ret; i++) {
        msgs[i].bytesTransferred = mmsg[i].msg_len;
        msgs[i].addrLen = mmsg[i].msg_hdr.msg_namelen;
    }

    return ret;
}

#else /* Fallback for non-Linux platforms */

int loopyUDPRecvMulti(loopyUDP *udp, loopyUDPMessage *msgs, int nmsg) {
    if (!udp || !msgs || nmsg <= 0 || udp->fd < 0) {
        errno = EINVAL;
        return -1;
    }

    if (nmsg > LOOPY_UDP_BATCH_MAX) {
        nmsg = LOOPY_UDP_BATCH_MAX;
    }

    int received = 0;
    for (int i = 0; i < nmsg; i++) {
        msgs[i].addrLen = sizeof(msgs[i].addr);
        ssize_t n =
            recvfrom(udp->fd, msgs[i].data, msgs[i].len, MSG_DONTWAIT,
                     (struct sockaddr *)&msgs[i].addr, &msgs[i].addrLen);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; /* No more data available */
            }
            if (received == 0) {
                udpSetError(udp, "recvfrom");
                return -1;
            }
            break; /* Return what we have */
        }
        msgs[i].bytesTransferred = n;
        received++;
    }

    return received;
}

#endif /* LOOPY_HAS_RECVMMSG */

#if LOOPY_HAS_SENDMMSG

int loopyUDPSendMulti(loopyUDP *udp, loopyUDPMessage *msgs, int nmsg) {
    if (!udp || !msgs || nmsg <= 0 || udp->fd < 0) {
        errno = EINVAL;
        return -1;
    }

    if (nmsg > LOOPY_UDP_BATCH_MAX) {
        nmsg = LOOPY_UDP_BATCH_MAX;
    }

    /* Set up mmsghdr array for sendmmsg */
    struct mmsghdr mmsg[LOOPY_UDP_BATCH_MAX];
    struct iovec iov[LOOPY_UDP_BATCH_MAX];

    for (int i = 0; i < nmsg; i++) {
        iov[i].iov_base = msgs[i].data;
        iov[i].iov_len = msgs[i].len;

        mmsg[i].msg_hdr.msg_name = &msgs[i].addr;
        mmsg[i].msg_hdr.msg_namelen = msgs[i].addrLen;
        mmsg[i].msg_hdr.msg_iov = &iov[i];
        mmsg[i].msg_hdr.msg_iovlen = 1;
        mmsg[i].msg_hdr.msg_control = NULL;
        mmsg[i].msg_hdr.msg_controllen = 0;
        mmsg[i].msg_hdr.msg_flags = 0;
        mmsg[i].msg_len = 0;
    }

    int ret = sendmmsg(udp->fd, mmsg, nmsg, MSG_DONTWAIT);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        udpSetError(udp, "sendmmsg");
        return -1;
    }

    /* Copy results back to caller's structures */
    for (int i = 0; i < ret; i++) {
        msgs[i].bytesTransferred = mmsg[i].msg_len;
    }

    return ret;
}

int loopyUDPSendMultiConnected(loopyUDP *udp, loopyUDPMessage *msgs, int nmsg) {
    if (!udp || !msgs || nmsg <= 0 || udp->fd < 0 || !udp->connected) {
        errno = EINVAL;
        return -1;
    }

    if (nmsg > LOOPY_UDP_BATCH_MAX) {
        nmsg = LOOPY_UDP_BATCH_MAX;
    }

    /* Set up mmsghdr array for sendmmsg (no destination - connected) */
    struct mmsghdr mmsg[LOOPY_UDP_BATCH_MAX];
    struct iovec iov[LOOPY_UDP_BATCH_MAX];

    for (int i = 0; i < nmsg; i++) {
        iov[i].iov_base = msgs[i].data;
        iov[i].iov_len = msgs[i].len;

        mmsg[i].msg_hdr.msg_name = NULL;
        mmsg[i].msg_hdr.msg_namelen = 0;
        mmsg[i].msg_hdr.msg_iov = &iov[i];
        mmsg[i].msg_hdr.msg_iovlen = 1;
        mmsg[i].msg_hdr.msg_control = NULL;
        mmsg[i].msg_hdr.msg_controllen = 0;
        mmsg[i].msg_hdr.msg_flags = 0;
        mmsg[i].msg_len = 0;
    }

    int ret = sendmmsg(udp->fd, mmsg, nmsg, MSG_DONTWAIT);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        udpSetError(udp, "sendmmsg");
        return -1;
    }

    /* Copy results back */
    for (int i = 0; i < ret; i++) {
        msgs[i].bytesTransferred = mmsg[i].msg_len;
    }

    return ret;
}

#else /* Fallback for non-Linux platforms */

int loopyUDPSendMulti(loopyUDP *udp, loopyUDPMessage *msgs, int nmsg) {
    if (!udp || !msgs || nmsg <= 0 || udp->fd < 0) {
        errno = EINVAL;
        return -1;
    }

    if (nmsg > LOOPY_UDP_BATCH_MAX) {
        nmsg = LOOPY_UDP_BATCH_MAX;
    }

    int sent = 0;
    for (int i = 0; i < nmsg; i++) {
        ssize_t n = sendto(udp->fd, msgs[i].data, msgs[i].len, MSG_DONTWAIT,
                           (struct sockaddr *)&msgs[i].addr, msgs[i].addrLen);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; /* Socket buffer full */
            }
            if (sent == 0) {
                udpSetError(udp, "sendto");
                return -1;
            }
            break; /* Return what we sent */
        }
        msgs[i].bytesTransferred = n;
        sent++;
    }

    return sent;
}

int loopyUDPSendMultiConnected(loopyUDP *udp, loopyUDPMessage *msgs, int nmsg) {
    if (!udp || !msgs || nmsg <= 0 || udp->fd < 0 || !udp->connected) {
        errno = EINVAL;
        return -1;
    }

    if (nmsg > LOOPY_UDP_BATCH_MAX) {
        nmsg = LOOPY_UDP_BATCH_MAX;
    }

    int sent = 0;
    for (int i = 0; i < nmsg; i++) {
        ssize_t n = send(udp->fd, msgs[i].data, msgs[i].len, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (sent == 0) {
                udpSetError(udp, "send");
                return -1;
            }
            break;
        }
        msgs[i].bytesTransferred = n;
        sent++;
    }

    return sent;
}

#endif /* LOOPY_HAS_SENDMMSG */

/* ====================================================================
 * Advanced UDP Features: GSO, GRO, PMTU
 * ==================================================================== */

/* GSO - Generic Segmentation Offload */

bool loopyUDPHasGSO(void) {
#if LOOPY_HAS_UDP_GSO
    /* Runtime detection - try to set UDP_SEGMENT on a test socket */
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }

    uint16_t segmentSize = 1200;
    int result =
        setsockopt(fd, SOL_UDP, UDP_SEGMENT, &segmentSize, sizeof(segmentSize));
    close(fd);
    return result == 0;
#else
    return false;
#endif
}

bool loopyUDPSetGSO(loopyUDP *udp, const loopyUDPGSOConfig *config) {
    if (!udp || !config) {
        return false;
    }

#if LOOPY_HAS_UDP_GSO
    /* Create socket if needed */
    if (udp->fd < 0) {
        if (!udpCreateSocket(udp, AF_INET, 0)) {
            return false;
        }
    }

    if (config->enabled) {
        /* Validate segment size (must be reasonable) */
        if (config->segmentSize < 512 || config->segmentSize > 65507) {
            snprintf(udp->errorString, sizeof(udp->errorString),
                     "invalid GSO segment size: %u", config->segmentSize);
            return false;
        }

        /* Enable GSO on socket */
        uint16_t segmentSize = config->segmentSize;
        if (setsockopt(udp->fd, SOL_UDP, UDP_SEGMENT, &segmentSize,
                       sizeof(segmentSize)) < 0) {
            udpSetError(udp, "setsockopt UDP_SEGMENT");
            return false;
        }

        udp->gsoEnabled = true;
        udp->gsoSegmentSize = config->segmentSize;
    } else {
        /* Disable GSO */
        uint16_t segmentSize = 0;
        if (setsockopt(udp->fd, SOL_UDP, UDP_SEGMENT, &segmentSize,
                       sizeof(segmentSize)) < 0) {
            udpSetError(udp, "setsockopt UDP_SEGMENT");
            return false;
        }

        udp->gsoEnabled = false;
        udp->gsoSegmentSize = 0;
    }

    return true;
#else
    (void)udp;
    (void)config;
    errno = ENOTSUP;
    return false;
#endif
}

bool loopyUDPSendGSO(loopyUDP *udp, const char *addr, int port,
                     const void *data, size_t len, loopyUDPSendCallback *cb,
                     void *userData) {
    if (!udp || !addr || !data || len == 0) {
        return false;
    }

#if LOOPY_HAS_UDP_GSO
    if (!udp->gsoEnabled) {
        snprintf(udp->errorString, sizeof(udp->errorString),
                 "GSO not enabled on socket");
        return false;
    }

    /* Create socket if needed */
    if (udp->fd < 0) {
        struct sockaddr_storage dest;
        socklen_t destLen;
        if (!udpResolveAddress(udp, addr, port, &dest, &destLen)) {
            return false;
        }
        if (!udpCreateSocket(udp, dest.ss_family, 0)) {
            return false;
        }
    }

    /* Resolve destination */
    struct sockaddr_storage dest;
    socklen_t destLen;
    if (!udpResolveAddress(udp, addr, port, &dest, &destLen)) {
        return false;
    }

    /* Send with GSO using sendmsg with control message */
    struct iovec iov = {.iov_base = (void *)data, .iov_len = len};

    /* Control message buffer for UDP_SEGMENT */
    union {
        char buf[CMSG_SPACE(sizeof(uint16_t))];
        struct cmsghdr align;
    } u;

    struct msghdr msg = {.msg_name = &dest,
                         .msg_namelen = destLen,
                         .msg_iov = &iov,
                         .msg_iovlen = 1,
                         .msg_control = u.buf,
                         .msg_controllen = sizeof(u.buf)};

    struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_level = SOL_UDP;
    cm->cmsg_type = UDP_SEGMENT;
    cm->cmsg_len = CMSG_LEN(sizeof(uint16_t));
    *(uint16_t *)CMSG_DATA(cm) = udp->gsoSegmentSize;

    ssize_t sent = sendmsg(udp->fd, &msg, 0);
    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* Would need to queue this - for now just fail */
            udpSetError(udp, "sendmsg GSO would block");
            return false;
        }
        udpSetError(udp, "sendmsg GSO");
        return false;
    }

    if (cb) {
        cb(udp, 0, userData);
    }
    return true;
#else
    (void)udp;
    (void)addr;
    (void)port;
    (void)data;
    (void)len;
    (void)cb;
    (void)userData;
    errno = ENOTSUP;
    return false;
#endif
}

bool loopyUDPSendGSOConnected(loopyUDP *udp, const void *data, size_t len,
                              loopyUDPSendCallback *cb, void *userData) {
    if (!udp || !udp->connected || !data || len == 0) {
        return false;
    }

#if LOOPY_HAS_UDP_GSO
    if (!udp->gsoEnabled) {
        snprintf(udp->errorString, sizeof(udp->errorString),
                 "GSO not enabled on socket");
        return false;
    }

    /* Send with GSO using sendmsg with control message */
    struct iovec iov = {.iov_base = (void *)data, .iov_len = len};

    /* Control message buffer for UDP_SEGMENT */
    union {
        char buf[CMSG_SPACE(sizeof(uint16_t))];
        struct cmsghdr align;
    } u;

    struct msghdr msg = {.msg_name = NULL,
                         .msg_namelen = 0,
                         .msg_iov = &iov,
                         .msg_iovlen = 1,
                         .msg_control = u.buf,
                         .msg_controllen = sizeof(u.buf)};

    struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_level = SOL_UDP;
    cm->cmsg_type = UDP_SEGMENT;
    cm->cmsg_len = CMSG_LEN(sizeof(uint16_t));
    *(uint16_t *)CMSG_DATA(cm) = udp->gsoSegmentSize;

    ssize_t sent = sendmsg(udp->fd, &msg, 0);
    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            udpSetError(udp, "sendmsg GSO would block");
            return false;
        }
        udpSetError(udp, "sendmsg GSO");
        return false;
    }

    if (cb) {
        cb(udp, 0, userData);
    }
    return true;
#else
    (void)udp;
    (void)data;
    (void)len;
    (void)cb;
    (void)userData;
    errno = ENOTSUP;
    return false;
#endif
}

/* GRO - Generic Receive Offload */

bool loopyUDPHasGRO(void) {
#if LOOPY_HAS_UDP_GRO
    /* Runtime detection - try to set UDP_GRO on a test socket */
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }

    int enable = 1;
    int result = setsockopt(fd, SOL_UDP, UDP_GRO, &enable, sizeof(enable));
    close(fd);
    return result == 0;
#else
    return false;
#endif
}

bool loopyUDPSetGRO(loopyUDP *udp, const loopyUDPGROConfig *config) {
    if (!udp || !config) {
        return false;
    }

#if LOOPY_HAS_UDP_GRO
    /* Create socket if needed */
    if (udp->fd < 0) {
        if (!udpCreateSocket(udp, AF_INET, 0)) {
            return false;
        }
    }

    int enable = config->enabled ? 1 : 0;
    if (setsockopt(udp->fd, SOL_UDP, UDP_GRO, &enable, sizeof(enable)) < 0) {
        udpSetError(udp, "setsockopt UDP_GRO");
        return false;
    }

    udp->groEnabled = config->enabled;
    return true;
#else
    (void)udp;
    (void)config;
    errno = ENOTSUP;
    return false;
#endif
}

/* PMTU Discovery */

bool loopyUDPSetPMTUMode(loopyUDP *udp, loopyUDPPMTUMode mode) {
    if (!udp) {
        return false;
    }

#if LOOPY_HAS_PMTU
    /* Create socket if needed */
    if (udp->fd < 0) {
        if (!udpCreateSocket(udp, AF_INET, 0)) {
            return false;
        }
    }

    /* Map loopy mode to system constants */
    int sysMode;
    switch (mode) {
    case LOOPY_UDP_PMTU_DISABLED:
        sysMode = IP_PMTUDISC_DONT;
        break;
    case LOOPY_UDP_PMTU_WANT:
        sysMode = IP_PMTUDISC_WANT;
        break;
    case LOOPY_UDP_PMTU_DO:
        sysMode = IP_PMTUDISC_DO;
        break;
    case LOOPY_UDP_PMTU_PROBE:
        sysMode = IP_PMTUDISC_PROBE;
        break;
    case LOOPY_UDP_PMTU_UNSPEC:
    default:
        /* Don't change current setting */
        return true;
    }

    int level = (udp->af == AF_INET6) ? IPPROTO_IPV6 : IPPROTO_IP;
    int opt = (udp->af == AF_INET6) ? IPV6_MTU_DISCOVER : IP_MTU_DISCOVER;

    if (setsockopt(udp->fd, level, opt, &sysMode, sizeof(sysMode)) < 0) {
        udpSetError(udp, "setsockopt MTU_DISCOVER");
        return false;
    }

    udp->pmtuMode = mode;
    return true;
#else
    (void)udp;
    (void)mode;
    errno = ENOTSUP;
    return false;
#endif
}

int loopyUDPGetPMTU(const loopyUDP *udp) {
    if (!udp || udp->fd < 0) {
        return -1;
    }

#if LOOPY_HAS_PMTU
    int level = (udp->af == AF_INET6) ? IPPROTO_IPV6 : IPPROTO_IP;
    int opt = (udp->af == AF_INET6) ? IPV6_MTU : IP_MTU;

    int mtu = 0;
    socklen_t mtuLen = sizeof(mtu);
    if (getsockopt(udp->fd, level, opt, &mtu, &mtuLen) < 0) {
        return -1;
    }

    return mtu;
#else
    (void)udp;
    return -1;
#endif
}

void *loopyUDPGetData(const loopyUDP *udp) {
    return udp ? udp->userData : NULL;
}

void loopyUDPSetData(loopyUDP *udp, void *data) {
    if (udp) {
        udp->userData = data;
    }
}
