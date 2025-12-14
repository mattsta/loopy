/* loopyStream - Unified duplex I/O abstraction for loopy event loop
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
#include "loopyNet.h"
#include "loopyStream.h"

#ifdef __linux__
#include "loopyIoUringNet.h"
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

/* ====================================================================
 * Internal data structures
 * ==================================================================== */

/* Write request */
typedef struct loopyWriteReq {
    void *data;
    size_t len;
    size_t written;
    loopyStreamWriteCallback *cb;
    void *userData;
    struct loopyWriteReq *next;
    /* FD passing support */
    int fds[LOOPY_STREAM_MAX_FDS];
    int nfds;
    bool fdsSent;
} loopyWriteReq;

struct loopyStream {
    loopyLoop *loop;
    void *userData; /* User data for handle accessors */
    int fd;
    loopyStreamType type;

    /* State flags */
    bool readable;
    bool writable;
    bool reading;
    bool listening;
    bool connected;
    bool connecting;
    bool shutdownPending;
    bool closing;

    /* Read callbacks */
    loopyStreamAllocCallback *allocCb;
    loopyStreamReadCallback *readCb;
    loopyStreamReadFdCallback *readFdCb; /* For FD passing */
    void *readUserData;

    /* Write queue */
    loopyWriteReq *writeHead;
    loopyWriteReq *writeTail;
    size_t writeQueueSize;
    size_t writeQueueCount;

    /* Listen callback */
    loopyStreamConnectionCallback *listenCb;
    void *listenUserData;

    /* Connect callback */
    loopyStreamWriteCallback *connectCb;
    void *connectUserData;

    /* Shutdown callback */
    loopyStreamShutdownCallback *shutdownCb;
    void *shutdownUserData;

    /* Close callback */
    loopyStreamCloseCallback *closeCb;
    void *closeUserData;

    /* Error tracking */
    char errorString[128];

#ifdef __linux__
    /* io_uring operation tracking */
    uint64_t sendOpId;
    uint64_t recvOpId;
    uint64_t acceptOpId;
    uint64_t connectOpId;
    void *ioUringReadBuf;      /* Buffer for io_uring read operations */
    size_t ioUringReadBufSize; /* Size of io_uring read buffer */
    bool usingIoUring;
#endif
};

/* ====================================================================
 * Internal helpers
 * ==================================================================== */

static void streamSetError(loopyStream *stream, const char *field) {
    snprintf(stream->errorString, sizeof(stream->errorString), "%s: %s", field,
             strerror(errno));
}

static void streamReadCallback(loopyLoop *l, int fd, void *clientData,
                               loopyAction mask);
static void streamWriteCallback(loopyLoop *l, int fd, void *clientData,
                                loopyAction mask);
static void streamListenCallback(loopyLoop *l, int fd, void *clientData,
                                 loopyAction mask);
static void streamConnectCallback(loopyLoop *l, int fd, void *clientData,
                                  loopyAction mask);

static bool streamSetNonBlock(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

static void streamUpdateWriteWatcher(loopyStream *stream) {
    if (stream->writeHead || stream->shutdownPending || stream->connecting) {
        loopyRegisterWrite(stream->loop, stream->fd, streamWriteCallback,
                           stream);
    } else {
        loopyUnregisterWrite(stream->loop, stream->fd);
    }
}

static void streamDrainWriteQueue(loopyStream *stream, int status) {
    loopyWriteReq *req = stream->writeHead;
    while (req) {
        loopyWriteReq *next = req->next;
        if (req->cb) {
            req->cb(stream, status, req->userData);
        }
        zfree(req->data);
        zfree(req);
        req = next;
    }
    stream->writeHead = NULL;
    stream->writeTail = NULL;
    stream->writeQueueSize = 0;
    stream->writeQueueCount = 0;
}

/* ====================================================================
 * Lifecycle
 * ==================================================================== */

static loopyStream *streamNew(loopyLoop *loop, loopyStreamType type) {
    if (!loop) {
        return NULL;
    }

    loopyStream *stream = zcalloc(1, sizeof(*stream));
    if (!stream) {
        return NULL;
    }

    stream->loop = loop;
    stream->fd = -1;
    stream->type = type;
    stream->readable = true;
    stream->writable = true;

    return stream;
}

loopyStream *loopyStreamNewTcp(loopyLoop *loop) {
    loopyStream *stream = streamNew(loop, LOOPY_STREAM_TCP);
    if (!stream) {
        return NULL;
    }

    stream->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (stream->fd < 0) {
        zfree(stream);
        return NULL;
    }

    if (!streamSetNonBlock(stream->fd)) {
        close(stream->fd);
        zfree(stream);
        return NULL;
    }

    return stream;
}

loopyStream *loopyStreamNewPipe(loopyLoop *loop) {
    return streamNew(loop, LOOPY_STREAM_PIPE);
}

loopyStream *loopyStreamFromFd(loopyLoop *loop, int fd, loopyStreamType type) {
    if (fd < 0) {
        return NULL;
    }

    loopyStream *stream = streamNew(loop, type);
    if (!stream) {
        return NULL;
    }

    stream->fd = fd;
    stream->connected = true;

    if (!streamSetNonBlock(fd)) {
        streamSetError(stream, "fcntl");
    }

    return stream;
}

void loopyStreamClose(loopyStream *stream, loopyStreamCloseCallback *cb,
                      void *userData) {
    if (!stream) {
        return;
    }

    if (stream->closing) {
        return;
    }
    stream->closing = true;
    stream->closeCb = cb;
    stream->closeUserData = userData;

    /* Stop reading */
    if (stream->reading) {
        loopyStreamReadStop(stream);
    }

    /* Unregister from event loop */
    if (stream->fd >= 0) {
        loopyUnregisterReadWrite(stream->loop, stream->fd);
    }

    /* Drain write queue with error */
    streamDrainWriteQueue(stream, -1);

    /* Close fd */
    if (stream->fd >= 0) {
        close(stream->fd);
        stream->fd = -1;
    }

    /* Call close callback */
    if (cb) {
        cb(stream, userData);
    }

    zfree(stream);
}

/* ====================================================================
 * TCP Server Operations
 * ==================================================================== */

bool loopyStreamBind(loopyStream *stream, const char *addr, int port) {
    if (!stream || stream->type != LOOPY_STREAM_TCP || stream->fd < 0) {
        return false;
    }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);

    if (!addr || addr[0] == '\0') {
        sa.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1) {
            streamSetError(stream, "inet_pton");
            return false;
        }
    }

    int yes = 1;
    setsockopt(stream->fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    if (bind(stream->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        streamSetError(stream, "bind");
        return false;
    }

    return true;
}

bool loopyStreamBind6(loopyStream *stream, const char *addr, int port,
                      bool ipv6Only) {
    if (!stream || stream->type != LOOPY_STREAM_TCP) {
        return false;
    }

    /* Need to recreate socket as IPv6 */
    if (stream->fd >= 0) {
        close(stream->fd);
    }

    stream->fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (stream->fd < 0) {
        streamSetError(stream, "socket");
        return false;
    }

    if (!streamSetNonBlock(stream->fd)) {
        streamSetError(stream, "fcntl");
        return false;
    }

    int yes = 1;
    setsockopt(stream->fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    if (ipv6Only) {
        setsockopt(stream->fd, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof(yes));
    }

    struct sockaddr_in6 sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin6_family = AF_INET6;
    sa.sin6_port = htons(port);

    if (!addr || addr[0] == '\0') {
        sa.sin6_addr = in6addr_any;
    } else {
        if (inet_pton(AF_INET6, addr, &sa.sin6_addr) != 1) {
            streamSetError(stream, "inet_pton");
            return false;
        }
    }

    if (bind(stream->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        streamSetError(stream, "bind");
        return false;
    }

    return true;
}

bool loopyStreamListen(loopyStream *stream, int backlog,
                       loopyStreamConnectionCallback *cb, void *userData) {
    if (!stream || stream->type != LOOPY_STREAM_TCP || stream->fd < 0 || !cb) {
        return false;
    }

    if (listen(stream->fd, backlog) < 0) {
        streamSetError(stream, "listen");
        return false;
    }

    stream->listening = true;
    stream->listenCb = cb;
    stream->listenUserData = userData;

    loopyRegisterRead(stream->loop, stream->fd, streamListenCallback, stream);

    return true;
}

/* io_uring Integration Point: ACCEPT operations
 *
 * Current: Epoll-based level-triggered notifications. When readable, user's
 *          listenCb is invoked, which calls loopyStreamAccept() to actually
 *          accept the connection via accept4() syscall.
 *
 * Future:  Could use loopyIoUringAccept() or loopyIoUringAcceptMultishot():
 *          - Submit accept operation when listen starts
 *          - Callback delivers new client fd directly
 *          - Multishot mode: single operation accepts multiple connections
 *          - Track operation ID in stream->acceptOpId
 *
 * Challenge: Current API separates notification (this callback) from actual
 *            accept (loopyStreamAccept). io_uring combines them. Would need
 *            to refactor API or maintain hybrid approach.
 */
static void streamListenCallback(loopyLoop *l, int fd, void *clientData,
                                 loopyAction mask) {
    (void)l;
    (void)fd;
    (void)mask;

    loopyStream *stream = clientData;

    if (stream->listenCb) {
        stream->listenCb(stream, 0, stream->listenUserData);
    }
}

loopyStream *loopyStreamAccept(loopyStream *server) {
    if (!server || !server->listening || server->fd < 0) {
        return NULL;
    }

    struct sockaddr_storage addr;
    socklen_t addrLen = sizeof(addr);

    int clientFd = accept(server->fd, (struct sockaddr *)&addr, &addrLen);
    if (clientFd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            streamSetError(server, "accept");
        }
        return NULL;
    }

    loopyStream *client =
        loopyStreamFromFd(server->loop, clientFd, LOOPY_STREAM_TCP);
    if (!client) {
        close(clientFd);
        return NULL;
    }

    return client;
}

/* ====================================================================
 * TCP Client Operations
 * ==================================================================== */

bool loopyStreamConnect(loopyStream *stream, const char *addr, int port,
                        loopyStreamWriteCallback *cb, void *userData) {
    if (!stream || stream->type != LOOPY_STREAM_TCP || stream->fd < 0) {
        return false;
    }

    /* Determine address family */
    struct sockaddr_storage ss;
    socklen_t ssLen;
    memset(&ss, 0, sizeof(ss));

    /* Try IPv4 first */
    struct sockaddr_in *sa4 = (struct sockaddr_in *)&ss;
    if (inet_pton(AF_INET, addr, &sa4->sin_addr) == 1) {
        sa4->sin_family = AF_INET;
        sa4->sin_port = htons(port);
        ssLen = sizeof(*sa4);
    } else {
        /* Try IPv6 */
        struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)&ss;
        if (inet_pton(AF_INET6, addr, &sa6->sin6_addr) == 1) {
            /* Need IPv6 socket */
            close(stream->fd);
            stream->fd = socket(AF_INET6, SOCK_STREAM, 0);
            if (stream->fd < 0) {
                streamSetError(stream, "socket");
                return false;
            }
            if (!streamSetNonBlock(stream->fd)) {
                streamSetError(stream, "fcntl");
                return false;
            }
            sa6->sin6_family = AF_INET6;
            sa6->sin6_port = htons(port);
            ssLen = sizeof(*sa6);
        } else {
            streamSetError(stream, "inet_pton");
            return false;
        }
    }

    int ret = connect(stream->fd, (struct sockaddr *)&ss, ssLen);
    if (ret < 0 && errno != EINPROGRESS) {
        streamSetError(stream, "connect");
        return false;
    }

    if (ret == 0) {
        /* Connected immediately */
        stream->connected = true;
        if (cb) {
            cb(stream, 0, userData);
        }
    } else {
        /* Connect in progress */
        stream->connecting = true;
        stream->connectCb = cb;
        stream->connectUserData = userData;
        loopyRegisterWrite(stream->loop, stream->fd, streamConnectCallback,
                           stream);
    }

    return true;
}

static void streamConnectCallback(loopyLoop *l, int fd, void *clientData,
                                  loopyAction mask) {
    (void)l;
    (void)fd;
    (void)mask;

    loopyStream *stream = clientData;

    /* Check if connect succeeded */
    int error = 0;
    socklen_t errLen = sizeof(error);
    if (getsockopt(stream->fd, SOL_SOCKET, SO_ERROR, &error, &errLen) < 0) {
        error = errno;
    }

    stream->connecting = false;

    if (error == 0) {
        stream->connected = true;
        loopyUnregisterWrite(stream->loop, stream->fd);
        streamUpdateWriteWatcher(stream);

        if (stream->connectCb) {
            stream->connectCb(stream, 0, stream->connectUserData);
        }
    } else {
        errno = error;
        streamSetError(stream, "connect");
        loopyUnregisterWrite(stream->loop, stream->fd);

        if (stream->connectCb) {
            stream->connectCb(stream, -1, stream->connectUserData);
        }
    }
}

/* ====================================================================
 * Reading
 * ==================================================================== */

bool loopyStreamReadStart(loopyStream *stream, loopyStreamAllocCallback *alloc,
                          loopyStreamReadCallback *read, void *userData) {
    if (!stream || !alloc || !read) {
        return false;
    }

    if (stream->fd < 0 || !stream->readable) {
        return false;
    }

    stream->allocCb = alloc;
    stream->readCb = read;
    stream->readUserData = userData;
    stream->reading = true;

    loopyRegisterRead(stream->loop, stream->fd, streamReadCallback, stream);

    return true;
}

void loopyStreamReadStop(loopyStream *stream) {
    if (!stream || !stream->reading) {
        return;
    }

    stream->reading = false;
    if (!stream->listening) {
        loopyUnregisterRead(stream->loop, stream->fd);
    }
}

bool loopyStreamIsReading(const loopyStream *stream) {
    return stream ? stream->reading : false;
}

/* io_uring Integration Point: RECV/RECVMSG operations
 *
 * Current: Epoll-based level-triggered reads. When readable:
 *          1. Call allocCb to get buffer
 *          2. Call read() or recvmsg() (for FD passing)
 *          3. Invoke readCb/readFdCb with data
 *          4. Repeat until EAGAIN (epoll continues notifying while readable)
 *
 * Future:  Could use loopyIoUringRecv() or loopyIoUringRecvmsg():
 *          - Pre-allocate buffer and submit recv when reading starts
 *          - Callback delivers data when available
 *          - Re-submit recv for next chunk (edge-triggered, one-shot)
 *          - Track operation ID in stream->recvOpId
 *          - Buffer managed in stream->ioUringReadBuf
 *
 * Challenge: Epoll is level-triggered (continuous notifications until read),
 *            io_uring is edge-triggered (one completion per operation).
 *            Must carefully chain operations to avoid missing data.
 *            Buffer management also differs (pre-allocated vs on-demand).
 */
static void streamReadCallback(loopyLoop *l, int fd, void *clientData,
                               loopyAction mask) {
    (void)l;
    (void)fd;
    (void)mask;

    loopyStream *stream = clientData;

    if (!stream->reading || !stream->allocCb ||
        (!stream->readCb && !stream->readFdCb)) {
        return;
    }

    void *buf = NULL;
    size_t bufLen = 0;

    stream->allocCb(stream, 65536, &buf, &bufLen, stream->readUserData);

    if (!buf || bufLen == 0) {
        if (stream->readCb) {
            stream->readCb(stream, -1, NULL, stream->readUserData);
        } else {
            stream->readFdCb(stream, -1, NULL, NULL, 0, stream->readUserData);
        }
        return;
    }

    ssize_t nread;
    int receivedFds[LOOPY_STREAM_MAX_FDS];
    int nfds = 0;

    /* Use recvmsg for FD-capable reads on pipes */
    if (stream->readFdCb && stream->type == LOOPY_STREAM_PIPE) {
        struct iovec iov;
        iov.iov_base = buf;
        iov.iov_len = bufLen;

        /* Control message buffer for SCM_RIGHTS */
        char cmsgbuf[CMSG_SPACE(sizeof(int) * LOOPY_STREAM_MAX_FDS)];

        struct msghdr msg = {0};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cmsgbuf;
        msg.msg_controllen = sizeof(cmsgbuf);

        nread = recvmsg(stream->fd, &msg, 0);

        if (nread > 0) {
            /* Extract any received file descriptors */
            struct cmsghdr *cmsg;
            for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL;
                 cmsg = CMSG_NXTHDR(&msg, cmsg)) {
                if (cmsg->cmsg_level == SOL_SOCKET &&
                    cmsg->cmsg_type == SCM_RIGHTS) {
                    int fdCount = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                    if (fdCount > LOOPY_STREAM_MAX_FDS) {
                        fdCount = LOOPY_STREAM_MAX_FDS;
                    }
                    memcpy(receivedFds, CMSG_DATA(cmsg), fdCount * sizeof(int));
                    nfds = fdCount;
                    break;
                }
            }
        }
    } else {
        /* Standard read */
        nread = read(stream->fd, buf, bufLen);
    }

    if (nread > 0) {
        if (stream->readFdCb) {
            stream->readFdCb(stream, nread, buf, receivedFds, nfds,
                             stream->readUserData);
        } else {
            stream->readCb(stream, nread, buf, stream->readUserData);
        }
    } else if (nread == 0) {
        /* EOF */
        stream->readable = false;
        if (stream->readFdCb) {
            stream->readFdCb(stream, 0, NULL, NULL, 0, stream->readUserData);
        } else {
            stream->readCb(stream, 0, NULL, stream->readUserData);
        }
    } else {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            streamSetError(stream, "read");
            if (stream->readFdCb) {
                stream->readFdCb(stream, -1, NULL, NULL, 0,
                                 stream->readUserData);
            } else {
                stream->readCb(stream, -1, NULL, stream->readUserData);
            }
        }
    }
}

/* ====================================================================
 * Writing
 * ==================================================================== */

bool loopyStreamWrite(loopyStream *stream, const void *data, size_t len,
                      loopyStreamWriteCallback *cb, void *userData) {
    if (!stream || !data || len == 0) {
        return false;
    }

    if (stream->fd < 0 || !stream->writable || stream->closing) {
        return false;
    }

    loopyWriteReq *req = zcalloc(1, sizeof(*req));
    if (!req) {
        return false;
    }

    req->data = zmalloc(len);
    if (!req->data) {
        zfree(req);
        return false;
    }

    memcpy(req->data, data, len);
    req->len = len;
    req->cb = cb;
    req->userData = userData;

    /* Add to queue */
    if (stream->writeTail) {
        stream->writeTail->next = req;
        stream->writeTail = req;
    } else {
        stream->writeHead = stream->writeTail = req;
    }
    stream->writeQueueSize += len;
    stream->writeQueueCount++;

    streamUpdateWriteWatcher(stream);

    return true;
}

ssize_t loopyStreamTryWrite(loopyStream *stream, const void *data, size_t len) {
    if (!stream || !data || len == 0) {
        return -1;
    }

    if (stream->fd < 0 || !stream->writable || !stream->connected) {
        return -1;
    }

    /* Only try-write if queue is empty */
    if (stream->writeHead) {
        errno = EAGAIN;
        return -1;
    }

    ssize_t written = write(stream->fd, data, len);
    if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        streamSetError(stream, "write");
    }

    return written;
}

size_t loopyStreamGetWriteQueueSize(const loopyStream *stream) {
    return stream ? stream->writeQueueSize : 0;
}

size_t loopyStreamGetWriteQueueCount(const loopyStream *stream) {
    return stream ? stream->writeQueueCount : 0;
}

/* io_uring Integration Point: SEND/SENDMSG operations
 *
 * Current: Epoll-based level-triggered writes. When writable:
 *          1. Process write queue (writeHead)
 *          2. Call write() or sendmsg() (for FD passing)
 *          3. Update req->written, advance queue
 *          4. Invoke write callback when request completes
 *          5. Repeat until queue empty or EAGAIN
 *
 * Future:  Could use loopyIoUringSend() or loopyIoUringSendmsg():
 *          - Submit send when write request is queued
 *          - Callback advances write queue when operation completes
 *          - Chain next write request automatically
 *          - Track operation ID in stream->sendOpId
 *
 * Challenge: Current code processes entire write queue in single epoll
 *            notification. io_uring requires submitting operations one at
 *            a time (or batch submission). Write queue management would
 *            need careful coordination with async completions.
 */
static void streamWriteCallback(loopyLoop *l, int fd, void *clientData,
                                loopyAction mask) {
    (void)l;
    (void)fd;
    (void)mask;

    loopyStream *stream = clientData;

    /* Handle connect completion */
    if (stream->connecting) {
        streamConnectCallback(l, fd, clientData, mask);
        return;
    }

    /* Process write queue */
    while (stream->writeHead) {
        loopyWriteReq *req = stream->writeHead;
        const char *ptr = (const char *)req->data + req->written;
        size_t remaining = req->len - req->written;

        ssize_t written;

        /* Use sendmsg if we have FDs to pass and haven't sent them yet */
        if (req->nfds > 0 && !req->fdsSent &&
            stream->type == LOOPY_STREAM_PIPE) {
            struct iovec iov;
            iov.iov_base = (void *)ptr;
            iov.iov_len = remaining;

            /* Prepare control message for SCM_RIGHTS */
            char cmsgbuf[CMSG_SPACE(sizeof(int) * LOOPY_STREAM_MAX_FDS)];
            struct msghdr msg = {0};
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;
            msg.msg_control = cmsgbuf;
            msg.msg_controllen = CMSG_SPACE(sizeof(int) * req->nfds);

            struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_RIGHTS;
            cmsg->cmsg_len = CMSG_LEN(sizeof(int) * req->nfds);
            memcpy(CMSG_DATA(cmsg), req->fds, sizeof(int) * req->nfds);

            written = sendmsg(stream->fd, &msg, 0);
            if (written > 0) {
                req->fdsSent = true; /* FDs sent with first data chunk */
            }
        } else {
            written = write(stream->fd, ptr, remaining);
        }

        if (written < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return; /* Try again later */
            }
            streamSetError(stream, "write");

            /* Error - drain queue with error status */
            streamDrainWriteQueue(stream, -1);
            streamUpdateWriteWatcher(stream);
            return;
        }

        req->written += written;
        stream->writeQueueSize -= written;

        if (req->written >= req->len) {
            /* Request complete */
            stream->writeHead = req->next;
            if (!stream->writeHead) {
                stream->writeTail = NULL;
            }
            stream->writeQueueCount--;

            if (req->cb) {
                req->cb(stream, 0, req->userData);
            }
            zfree(req->data);
            zfree(req);
        }
    }

    /* Handle pending shutdown */
    if (stream->shutdownPending && !stream->writeHead) {
        shutdown(stream->fd, SHUT_WR);
        stream->writable = false;
        stream->shutdownPending = false;

        if (stream->shutdownCb) {
            stream->shutdownCb(stream, 0, stream->shutdownUserData);
        }
    }

    streamUpdateWriteWatcher(stream);
}

/* ====================================================================
 * Shutdown
 * ==================================================================== */

bool loopyStreamShutdown(loopyStream *stream, loopyStreamShutdownCallback *cb,
                         void *userData) {
    if (!stream || stream->fd < 0 || !stream->writable) {
        return false;
    }

    stream->shutdownCb = cb;
    stream->shutdownUserData = userData;

    if (stream->writeHead) {
        /* Wait for writes to complete */
        stream->shutdownPending = true;
        return true;
    }

    /* Shutdown immediately */
    if (shutdown(stream->fd, SHUT_WR) < 0) {
        streamSetError(stream, "shutdown");
        return false;
    }

    stream->writable = false;

    if (cb) {
        cb(stream, 0, userData);
    }

    return true;
}

/* ====================================================================
 * Properties
 * ==================================================================== */

bool loopyStreamIsReadable(const loopyStream *stream) {
    return stream ? stream->readable : false;
}

bool loopyStreamIsWritable(const loopyStream *stream) {
    return stream ? stream->writable : false;
}

loopyStreamType loopyStreamGetType(const loopyStream *stream) {
    return stream ? stream->type : LOOPY_STREAM_UNKNOWN;
}

int loopyStreamGetFd(const loopyStream *stream) {
    return stream ? stream->fd : -1;
}

loopyLoop *loopyStreamGetLoop(const loopyStream *stream) {
    return stream ? stream->loop : NULL;
}

bool loopyStreamGetSockName(loopyStream *stream, char *addr, size_t addrLen,
                            int *port) {
    if (!stream || stream->fd < 0) {
        return false;
    }

    struct sockaddr_storage ss;
    socklen_t ssLen = sizeof(ss);

    if (getsockname(stream->fd, (struct sockaddr *)&ss, &ssLen) < 0) {
        streamSetError(stream, "getsockname");
        return false;
    }

    if (ss.ss_family == AF_INET) {
        struct sockaddr_in *sa = (struct sockaddr_in *)&ss;
        if (addr && addrLen > 0) {
            inet_ntop(AF_INET, &sa->sin_addr, addr, addrLen);
        }
        if (port) {
            *port = ntohs(sa->sin_port);
        }
    } else if (ss.ss_family == AF_INET6) {
        struct sockaddr_in6 *sa = (struct sockaddr_in6 *)&ss;
        if (addr && addrLen > 0) {
            inet_ntop(AF_INET6, &sa->sin6_addr, addr, addrLen);
        }
        if (port) {
            *port = ntohs(sa->sin6_port);
        }
    } else {
        return false;
    }

    return true;
}

bool loopyStreamGetPeerName(loopyStream *stream, char *addr, size_t addrLen,
                            int *port) {
    if (!stream || stream->fd < 0) {
        return false;
    }

    struct sockaddr_storage ss;
    socklen_t ssLen = sizeof(ss);

    if (getpeername(stream->fd, (struct sockaddr *)&ss, &ssLen) < 0) {
        streamSetError(stream, "getpeername");
        return false;
    }

    if (ss.ss_family == AF_INET) {
        struct sockaddr_in *sa = (struct sockaddr_in *)&ss;
        if (addr && addrLen > 0) {
            inet_ntop(AF_INET, &sa->sin_addr, addr, addrLen);
        }
        if (port) {
            *port = ntohs(sa->sin_port);
        }
    } else if (ss.ss_family == AF_INET6) {
        struct sockaddr_in6 *sa = (struct sockaddr_in6 *)&ss;
        if (addr && addrLen > 0) {
            inet_ntop(AF_INET6, &sa->sin6_addr, addr, addrLen);
        }
        if (port) {
            *port = ntohs(sa->sin6_port);
        }
    } else {
        return false;
    }

    return true;
}

const char *loopyStreamGetError(const loopyStream *stream) {
    return stream ? stream->errorString : "";
}

/* ====================================================================
 * File Descriptor Passing (IPC)
 * ==================================================================== */

bool loopyStreamCanPassFd(const loopyStream *stream) {
    return stream && stream->type == LOOPY_STREAM_PIPE && stream->fd >= 0;
}

bool loopyStreamReadStartWithFd(loopyStream *stream,
                                loopyStreamAllocCallback *alloc,
                                loopyStreamReadFdCallback *read,
                                void *userData) {
    if (!stream || !alloc || !read) {
        return false;
    }

    /* FD passing only works on Unix domain sockets (pipes) */
    if (stream->type != LOOPY_STREAM_PIPE) {
        return false;
    }

    if (stream->fd < 0 || !stream->readable || stream->reading) {
        return false;
    }

    stream->allocCb = alloc;
    stream->readCb = NULL;
    stream->readFdCb = read;
    stream->readUserData = userData;

    if (!loopyRegisterRead(stream->loop, stream->fd, streamReadCallback,
                           stream)) {
        return false;
    }

    stream->reading = true;
    return true;
}

bool loopyStreamWriteWithFd(loopyStream *stream, const void *data, size_t len,
                            const int *fds, int nfds,
                            loopyStreamWriteCallback *cb, void *userData) {
    if (!stream || !data || len == 0 || !fds || nfds <= 0) {
        return false;
    }

    /* FD passing only works on Unix domain sockets (pipes) */
    if (stream->type != LOOPY_STREAM_PIPE) {
        return false;
    }

    if (nfds > LOOPY_STREAM_MAX_FDS) {
        return false;
    }

    if (stream->fd < 0 || !stream->writable || stream->closing) {
        return false;
    }

    loopyWriteReq *req = zcalloc(1, sizeof(*req));
    if (!req) {
        return false;
    }

    req->data = zmalloc(len);
    if (!req->data) {
        zfree(req);
        return false;
    }

    memcpy(req->data, data, len);
    req->len = len;
    req->cb = cb;
    req->userData = userData;

    /* Copy file descriptors to pass */
    memcpy(req->fds, fds, sizeof(int) * nfds);
    req->nfds = nfds;
    req->fdsSent = false;

    /* Add to queue */
    if (stream->writeTail) {
        stream->writeTail->next = req;
        stream->writeTail = req;
    } else {
        stream->writeHead = stream->writeTail = req;
    }
    stream->writeQueueSize += len;
    stream->writeQueueCount++;

    streamUpdateWriteWatcher(stream);

    return true;
}

/* ====================================================================
 * Handle Accessors
 * ==================================================================== */

void *loopyStreamGetData(const loopyStream *stream) {
    return stream ? stream->userData : NULL;
}

void loopyStreamSetData(loopyStream *stream, void *data) {
    if (stream) {
        stream->userData = data;
    }
}
