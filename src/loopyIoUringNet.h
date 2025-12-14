/* loopyIoUringNet - io_uring network operations interface
 *
 * Public API for submitting network operations via io_uring when available.
 * These operations provide zero-syscall network I/O on Linux 5.1+ with
 * io_uring support, offering significant performance improvements over
 * traditional epoll-based async networking.
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

#ifndef LOOPY_IOURING_NET_H
#define LOOPY_IOURING_NET_H

#include "loopyPlatform.h"
#include "loopy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>

#ifdef __linux__

/* ====================================================================
 * Types
 * ==================================================================== */

/**
 * Callback for io_uring network operation completion.
 *
 * @param userData User data passed during submission
 * @param result   Operation result:
 *                 - SEND/RECV: bytes transferred, or -errno on error
 *                 - ACCEPT: new socket fd, or -errno on error
 *                 - CONNECT: 0 on success, or -errno on error
 *                 - SHUTDOWN: 0 on success, or -errno on error
 */
typedef void loopyIoUringNetCallback(void *userData, int32_t result);

/* ====================================================================
 * TCP/Stream Operations (Linux 5.6+)
 * ==================================================================== */

/**
 * Submit a SEND operation via io_uring (if available).
 *
 * Sends data on a connected socket. The operation completes asynchronously
 * and the callback is invoked when done.
 *
 * @param l        Event loop
 * @param sockfd   Socket file descriptor (must be connected)
 * @param buf      Buffer to send from (must remain valid until completion)
 * @param len      Number of bytes to send
 * @param flags    Send flags (MSG_DONTWAIT, MSG_NOSIGNAL, etc.)
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure or if io_uring not available
 */
uint64_t loopyIoUringSend(loopyLoop *l, int sockfd, const void *buf, size_t len,
                          int flags, loopyIoUringNetCallback *cb,
                          void *userData);

/**
 * Submit a RECV operation via io_uring (if available).
 *
 * Receives data from a connected socket. The operation completes
 * asynchronously and the callback is invoked when done.
 *
 * @param l        Event loop
 * @param sockfd   Socket file descriptor (must be connected)
 * @param buf      Buffer to receive into (must remain valid until completion)
 * @param len      Buffer size
 * @param flags    Receive flags (MSG_DONTWAIT, MSG_PEEK, etc.)
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure or if io_uring not available
 */
uint64_t loopyIoUringRecv(loopyLoop *l, int sockfd, void *buf, size_t len,
                          int flags, loopyIoUringNetCallback *cb,
                          void *userData);

/**
 * Submit an ACCEPT operation via io_uring (if available).
 *
 * Accepts a new connection on a listening socket. The operation completes
 * asynchronously and the callback receives the new client socket fd.
 *
 * @param l        Event loop
 * @param sockfd   Listening socket file descriptor
 * @param addr     OUT: Client address (optional, can be NULL)
 * @param addrlen  IN/OUT: Address buffer size / actual size (optional)
 * @param flags    Accept flags (SOCK_NONBLOCK, SOCK_CLOEXEC, etc.)
 * @param cb       Completion callback (receives new socket fd in result)
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure or if io_uring not available
 *
 * Note: For high-performance servers, consider loopyIoUringAcceptMultishot()
 *       which can accept multiple connections with a single operation.
 */
uint64_t loopyIoUringAccept(loopyLoop *l, int sockfd, struct sockaddr *addr,
                            socklen_t *addrlen, int flags,
                            loopyIoUringNetCallback *cb, void *userData);

/**
 * Submit a CONNECT operation via io_uring (if available).
 *
 * Initiates a connection to a remote address. The operation completes
 * asynchronously and the callback is invoked when the connection succeeds
 * or fails.
 *
 * @param l        Event loop
 * @param sockfd   Socket file descriptor (must not be connected)
 * @param addr     Remote address to connect to (must remain valid)
 * @param addrlen  Address structure size
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure or if io_uring not available
 */
uint64_t loopyIoUringConnect(loopyLoop *l, int sockfd,
                             const struct sockaddr *addr, socklen_t addrlen,
                             loopyIoUringNetCallback *cb, void *userData);

/**
 * Submit a SHUTDOWN operation via io_uring (if available).
 *
 * Shuts down part of a full-duplex connection. The operation completes
 * asynchronously and the callback is invoked when done.
 *
 * @param l        Event loop
 * @param sockfd   Socket file descriptor
 * @param how      SHUT_RD, SHUT_WR, or SHUT_RDWR
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure or if io_uring not available
 */
uint64_t loopyIoUringShutdown(loopyLoop *l, int sockfd, int how,
                              loopyIoUringNetCallback *cb, void *userData);

/* ====================================================================
 * Advanced Message Operations (Linux 5.1+)
 * ==================================================================== */

/**
 * Submit a SENDMSG operation via io_uring (if available).
 *
 * Sends data with control messages (for scatter/gather I/O, ancillary data,
 * etc.). The operation completes asynchronously and the callback is invoked
 * when done.
 *
 * @param l        Event loop
 * @param sockfd   Socket file descriptor
 * @param msg      Message header (must remain valid until completion)
 * @param flags    Send flags (MSG_DONTWAIT, MSG_NOSIGNAL, etc.)
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure or if io_uring not available
 *
 * Use cases:
 *  - Scatter/gather I/O (iovec arrays)
 *  - Sending file descriptors (SCM_RIGHTS)
 *  - Sending to specific destinations (for unconnected UDP)
 */
uint64_t loopyIoUringSendmsg(loopyLoop *l, int sockfd, const struct msghdr *msg,
                             int flags, loopyIoUringNetCallback *cb,
                             void *userData);

/**
 * Submit a RECVMSG operation via io_uring (if available).
 *
 * Receives data with control messages (for scatter/gather I/O, ancillary
 * data, etc.). The operation completes asynchronously and the callback is
 * invoked when done.
 *
 * @param l        Event loop
 * @param sockfd   Socket file descriptor
 * @param msg      Message header (must remain valid until completion)
 * @param flags    Receive flags (MSG_DONTWAIT, MSG_PEEK, etc.)
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure or if io_uring not available
 *
 * Use cases:
 *  - Scatter/gather I/O (iovec arrays)
 *  - Receiving file descriptors (SCM_RIGHTS)
 *  - Getting source address (for unconnected UDP)
 */
uint64_t loopyIoUringRecvmsg(loopyLoop *l, int sockfd, struct msghdr *msg,
                             int flags, loopyIoUringNetCallback *cb,
                             void *userData);

/* ====================================================================
 * High-Performance Features (Linux 5.19+)
 * ==================================================================== */

/**
 * Submit a multishot ACCEPT operation via io_uring (if available).
 *
 * This is a **high-performance** accept mode where a single operation
 * stays active and completes multiple times - once for each new connection.
 * Eliminates syscall overhead for accept() entirely.
 *
 * The callback will be invoked repeatedly, once for each new connection.
 * Each invocation receives the new client socket fd in the result parameter.
 *
 * The operation continues until:
 *  - An error occurs (result < 0)
 *  - The listening socket is closed
 *  - The operation is explicitly cancelled
 *
 * @param l        Event loop
 * @param sockfd   Listening socket file descriptor
 * @param flags    Accept flags (SOCK_NONBLOCK, SOCK_CLOEXEC, etc.)
 * @param cb       Completion callback (called for each new connection)
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure or if multishot not available
 *
 * Performance: Can accept 500K+ connections/sec vs ~50K with regular accept()
 *
 * Example:
 *   void onAccept(void *userData, int32_t result) {
 *       if (result < 0) {
 *           // Error or shutdown
 *           return;
 *       }
 *       int clientFd = result;
 *       // Handle new connection...
 *   }
 *
 *   loopyIoUringAcceptMultishot(loop, listenFd, SOCK_NONBLOCK | SOCK_CLOEXEC,
 *                               onAccept, userData);
 */
uint64_t loopyIoUringAcceptMultishot(loopyLoop *l, int sockfd, int flags,
                                     loopyIoUringNetCallback *cb,
                                     void *userData);

/* ====================================================================
 * Operation Management
 * ==================================================================== */

/**
 * Cancel a pending io_uring network operation.
 *
 * Attempts to cancel an in-flight operation. The operation's callback
 * will be invoked with result = -ECANCELED if cancellation succeeds.
 *
 * @param l    Event loop
 * @param opId Operation ID returned from submission function
 * @return true if cancellation was queued, false on error
 *
 * Note: Cancellation is best-effort. The operation may complete normally
 *       if it finishes before the cancel request is processed.
 */
bool loopyIoUringNetCancel(loopyLoop *l, uint64_t opId);

/* ====================================================================
 * Platform Detection
 * ==================================================================== */

/**
 * Check if io_uring network operations are available.
 *
 * @param l Event loop
 * @return true if io_uring is active and network ops are supported
 */
bool loopyIoUringNetAvailable(const loopyLoop *l);

/**
 * Check if multishot accept is available.
 *
 * Multishot accept requires Linux 5.19+ and io_uring support.
 *
 * @param l Event loop
 * @return true if multishot accept is supported
 */
bool loopyIoUringNetHasMultishot(const loopyLoop *l);

#endif /* __linux__ */

#endif /* LOOPY_IOURING_NET_H */
