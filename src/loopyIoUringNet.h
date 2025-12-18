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

#include "loopy.h"
#include "loopyPlatform.h"

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
 * Send to specific address (UDP/unconnected sockets).
 *
 * @param l         Event loop
 * @param sockfd    Socket file descriptor
 * @param buf       Buffer to send
 * @param len       Buffer length
 * @param flags     Send flags
 * @param dest_addr Destination address
 * @param addrlen   Address length
 * @param cb        Completion callback
 * @param userData  User data
 * @return Operation ID on success, 0 on failure
 */
uint64_t loopyIoUringSendto(loopyLoop *l, int sockfd, const void *buf,
                            size_t len, int flags,
                            const struct sockaddr *dest_addr, socklen_t addrlen,
                            loopyIoUringNetCallback *cb, void *userData);

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
 * Submit a SENDMSG_ZC (zero-copy sendmsg) operation via io_uring (if
 * available).
 *
 * Sends data with control messages (scatter/gather I/O, ancillary data) without
 * copying from user space to kernel space, reducing CPU overhead for large
 * transfers.
 *
 * Requirements:
 *  - Linux kernel 6.1+ for io_uring SENDMSG_ZC support
 *
 * Benefits:
 *  - Avoids userspace-to-kernel copy overhead
 *  - Useful for large message transfers
 *  - Supports scatter/gather I/O and ancillary data like regular SENDMSG
 *
 * The operation completes asynchronously and the callback is invoked when done.
 * May generate additional notification CQEs similar to SEND_ZC.
 *
 * @param l        Event loop
 * @param sockfd   Socket file descriptor
 * @param msg      Message header (scatter/gather, ancillary data)
 *                 Must remain valid until completion callback is invoked
 * @param flags    Send flags (MSG_DONTWAIT, MSG_NOSIGNAL, etc.)
 * @param cb       Completion callback (receives bytes sent in result)
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure or if io_uring not available
 *
 * Use cases:
 *  - Zero-copy scatter/gather I/O for large messages
 *  - Sending file descriptors with large data (SCM_RIGHTS)
 *  - Reducing CPU overhead on high-throughput servers
 */
uint64_t loopyIoUringSendmsgZeroCopy(loopyLoop *l, int sockfd,
                                     const struct msghdr *msg, int flags,
                                     loopyIoUringNetCallback *cb,
                                     void *userData);

/**
 * Submit a SEND_BUNDLE operation via io_uring (if available).
 *
 * Bundles multiple send operations into a single SQE for improved efficiency.
 * Instead of submitting multiple individual SEND operations, this allows
 * sending multiple buffers with a single syscall, reducing overhead.
 *
 * Requirements:
 *  - Linux kernel 6.1+ for io_uring SEND_BUNDLE support
 *
 * Benefits:
 *  - Reduces syscall overhead when sending multiple buffers to same socket
 *  - Batches multiple sends into a single operation
 *  - Useful for headers, data chunks, trailers patterns
 *
 * The operation completes asynchronously and the callback is invoked when done.
 * The result contains the total number of bytes sent from all buffers.
 *
 * Note:
 *  - All buffers are sent to the same socket
 *  - The operation is atomic - either all buffers are sent or none
 *  - If partial send occurs, the kernel may retry or return partial count
 *  - Buffers must remain valid until completion callback is invoked
 *
 * @param l        Event loop
 * @param sockfd   Socket file descriptor (must be connected)
 * @param bufs     Array of buffer pointers (const void *[])
 *                 Must remain valid until completion
 * @param lens     Array of buffer lengths (size_t[])
 *                 Must remain valid until completion
 * @param count    Number of buffers in the bundle (1-65535)
 * @param flags    Send flags (MSG_DONTWAIT, MSG_NOSIGNAL, etc.)
 * @param cb       Completion callback (receives total bytes sent in result)
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure or if io_uring not available
 *
 * Use cases:
 *  - HTTP response headers + body + trailers
 *  - Multi-chunk data transmission
 *  - Scatter/gather I/O patterns
 */
uint64_t loopyIoUringSendBundle(loopyLoop *l, int sockfd,
                                const void *const *bufs, const size_t *lens,
                                uint32_t count, int flags,
                                loopyIoUringNetCallback *cb, void *userData);

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

/**
 * Submit a multishot RECVMSG operation via io_uring (Linux 6.0+).
 *
 * Receives multiple messages with scatter/gather I/O and ancillary data.
 * The callback fires repeatedly for each received message until error or
 * cancellation. Requires buffer pool for automatic buffer selection.
 *
 * Benefits:
 *  - Callback fires automatically for each received message
 *  - No need to re-submit after each message
 *  - Automatic buffer selection from registered pool
 *  - Supports scatter/gather and ancillary data (like regular RECVMSG)
 *
 * The operation completes (stops generating callbacks) when:
 *  - An error occurs (-errno returned to callback)
 *  - Manual cancellation via loopyIoUringNetCancel()
 *
 * @param l            Event loop
 * @param sockfd       Socket file descriptor
 * @param msg          Message header (scatter/gather, ancillary data)
 * @param flags        Receive flags (MSG_DONTWAIT, etc.)
 * @param buffer_group Buffer pool group ID for automatic buffer selection
 * @param cb           Completion callback (invoked multiple times)
 * @param userData     User data for callback
 * @return Operation ID on success, 0 on failure or if io_uring not available
 *
 * Requirements:
 *  - Linux kernel 6.0+ for RECVMSG_MULTISHOT
 *  - Buffer pool registered with buffer_group ID
 *  - Message header must remain valid throughout operation
 *
 * Example:
 *   void onRecvMsg(void *userData, int32_t result) {
 *       if (result > 0) {
 *           // Process message (result = bytes received)
 *       } else {
 *           // Error: -errno
 *       }
 *   }
 *   loopyIoUringRecvmsgMultishot(l, sockfd, &msg, 0, bufferGroupId,
 *                                onRecvMsg, NULL);
 */
uint64_t loopyIoUringRecvmsgMultishot(loopyLoop *l, int sockfd,
                                      struct msghdr *msg, int flags,
                                      uint16_t buffer_group,
                                      loopyIoUringNetCallback *cb,
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

/**
 * Cancel all in-flight operations on a file descriptor.
 *
 * @param l  Event loop
 * @param fd File descriptor
 * @return true if cancellation submitted, false on error
 */
bool loopyIoUringCancelFd(loopyLoop *l, int fd);

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

/* ====================================================================
 * Socket Creation Operations (Linux 5.19+)
 * ==================================================================== */

/**
 * Create a socket via io_uring (SOCKET operation).
 *
 * @param l        Event loop
 * @param domain   Address family (AF_INET, AF_INET6, AF_UNIX, etc.)
 * @param type     Socket type (SOCK_STREAM, SOCK_DGRAM, etc.)
 * @param protocol Protocol (usually 0)
 * @param cb       Completion callback (receives socket fd in result)
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure
 *
 * Note: Uses loopyIoUringFileCallback type for callback.
 */
uint64_t loopyIoUringSocket(loopyLoop *l, int domain, int type, int protocol,
                            void (*cb)(void *userData, int32_t result),
                            void *userData);

/**
 * Bind socket to address via io_uring (BIND operation).
 *
 * @param l        Event loop
 * @param sockfd   Socket file descriptor
 * @param addr     Address to bind to
 * @param addrlen  Address structure size
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure
 */
uint64_t loopyIoUringBind(loopyLoop *l, int sockfd, const struct sockaddr *addr,
                          socklen_t addrlen,
                          void (*cb)(void *userData, int32_t result),
                          void *userData);

/**
 * Listen on socket via io_uring (LISTEN operation).
 *
 * @param l        Event loop
 * @param sockfd   Socket file descriptor
 * @param backlog  Maximum pending connections
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure
 */
uint64_t loopyIoUringListen(loopyLoop *l, int sockfd, int backlog,
                            void (*cb)(void *userData, int32_t result),
                            void *userData);

/* ====================================================================
 * Direct Socket Operations (ACCEPT_DIRECT, SOCKET_DIRECT) - Linux 5.19+
 * Use fixed file table for zero-syscall socket management
 * ==================================================================== */

/**
 * Accept a connection and install in fixed file table (ACCEPT_DIRECT).
 *
 * Accepts a new connection on a listening socket and installs the socket
 * directly in the fixed file table at the specified index. This avoids
 * returning the fd via callback and provides zero-syscall socket management.
 *
 * Requires:
 *  - Linux kernel 5.19+
 *  - Fixed file table registered with io_uring
 *  - Valid file_index within registered table bounds
 *
 * The callback result indicates:
 *  - 0 on success (socket installed at file_index)
 *  - negative error code on failure
 *
 * @param l         Event loop
 * @param sockfd    Listening socket file descriptor
 * @param addr      OUT: Client address (optional, can be NULL)
 * @param addrlen   IN/OUT: Address buffer size / actual size (optional)
 * @param flags     Accept flags (SOCK_NONBLOCK, SOCK_CLOEXEC, etc.)
 * @param file_index Index in fixed file table where socket will be installed
 * @param cb        Completion callback
 * @param userData  User data for callback
 * @return Operation ID on success, 0 on failure or if io_uring not available
 *
 * Note: This operation provides zero-syscall socket installation compared to
 *       regular ACCEPT which returns the fd and requires separate management.
 */
uint64_t loopyIoUringAcceptDirect(loopyLoop *l, int sockfd,
                                  struct sockaddr *addr, socklen_t *addrlen,
                                  int flags, uint32_t file_index,
                                  loopyIoUringNetCallback *cb, void *userData);

/**
 * Create a socket and install in fixed file table (SOCKET_DIRECT).
 *
 * Creates a new socket and installs it directly in the fixed file table
 * at the specified index. This avoids returning the fd via callback and
 * provides zero-syscall socket creation with immediate fixed file assignment.
 *
 * Requires:
 *  - Linux kernel 5.19+
 *  - Fixed file table registered with io_uring
 *  - Valid file_index within registered table bounds
 *
 * The callback result indicates:
 *  - 0 on success (socket installed at file_index)
 *  - negative error code on failure
 *
 * @param l         Event loop
 * @param domain    Address family (AF_INET, AF_INET6, AF_UNIX, etc.)
 * @param type      Socket type (SOCK_STREAM, SOCK_DGRAM, etc.)
 * @param protocol  Protocol (usually 0 for default)
 * @param file_index Index in fixed file table where socket will be installed
 * @param cb        Completion callback
 * @param userData  User data for callback
 * @return Operation ID on success, 0 on failure or if io_uring not available
 *
 * Use case: High-performance servers that pre-allocate socket file table
 *           slots for incoming connections or outgoing sockets.
 */
uint64_t loopyIoUringSocketDirect(loopyLoop *l, int domain, int type,
                                  int protocol, uint32_t file_index,
                                  loopyIoUringNetCallback *cb, void *userData);

/* ====================================================================
 * Advanced Receive Operations
 * ==================================================================== */

/**
 * Submit a multishot RECV operation with a buffer.
 *
 * Similar to AcceptMultishot but for receiving data. The operation
 * stays active and completes multiple times until cancelled.
 *
 * @param l        Event loop
 * @param sockfd   Socket file descriptor
 * @param buf      Buffer for receiving data
 * @param len      Buffer size
 * @param flags    Receive flags
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure
 */
uint64_t loopyIoUringRecvMultishot(loopyLoop *l, int sockfd, void *buf,
                                   size_t len, int flags,
                                   void (*cb)(void *userData, int32_t result),
                                   void *userData);

/**
 * Submit a zero-copy SEND operation (SEND_ZC).
 *
 * Sends data without copying from user space to kernel space,
 * reducing CPU overhead for large transfers.
 *
 * @param l        Event loop
 * @param sockfd   Socket file descriptor
 * @param buf      Buffer to send (must remain valid until notification CQE)
 * @param len      Number of bytes to send
 * @param flags    Send flags
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure
 *
 * Note: Zero-copy sends may generate additional notification CQEs.
 */
uint64_t loopyIoUringSendZeroCopy(loopyLoop *l, int sockfd, const void *buf,
                                  size_t len, int flags,
                                  void (*cb)(void *userData, int32_t result),
                                  void *userData);

/**
 * Submit a zero-copy RECV operation (RECV_ZC) - Linux 6.0+.
 *
 * Receives data without copying from kernel space to user space when possible,
 * reducing CPU overhead for high-throughput network operations. This is most
 * effective with pre-registered buffers for maximum zero-copy benefits.
 *
 * Requires Linux 6.0+ kernel support for io_uring RECV_ZC operations.
 *
 * The operation completes asynchronously and the callback is invoked when done.
 *
 * @param l        Event loop
 * @param sockfd   Socket file descriptor (must be connected)
 * @param buf      Buffer to receive into (must remain valid until completion)
 * @param len      Buffer size
 * @param flags    Receive flags (MSG_DONTWAIT, MSG_PEEK, etc.)
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure or if io_uring not available
 *
 * Callback result values:
 *   - result > 0: Bytes received successfully
 *   - result < 0: Negative error code (e.g., -EAGAIN, -ECONNRESET, -EINVAL)
 *   - Kernel 6.0+: May receive additional notification CQEs marked with
 *     IORING_CQE_F_NOTIF flag indicating buffer status
 *
 * Performance benefits:
 *   - Reduces CPU usage by 10-30% vs regular RECV on high-throughput workloads
 *   - Best used with MSG_WAITALL flag for full buffer receives
 *   - Consider buffer pooling for frequently-used receive buffer sizes
 *   - Pair with io_uring buffer pools for optimal performance
 *
 * Requirements:
 *   - Linux 6.0+ kernel with io_uring RECV_ZC support
 *   - For maximum benefit, register buffers with io_uring
 *   - Buffer must remain valid until completion callback is invoked
 *
 * Example usage:
 *   void onRecvComplete(void *userData, int32_t result) {
 *       if (result > 0) {
 *           printf("Received %d bytes\\n", result);
 *           // Process received data
 *       } else if (result < 0) {
 *           printf("Receive error: %s\\n", strerror(-result));
 *       }
 *   }
 *
 *   uint64_t opId = loopyIoUringRecvZeroCopy(loop, sockfd, buf, sizeof(buf),
 *                                           MSG_WAITALL, onRecvComplete,
 * userData); if (opId == 0) { printf("Failed to submit RECV_ZC operation\\n");
 *   }
 *
 * Note: Zero-copy receives may generate additional notification CQEs.
 *       Refer to io_uring documentation for IORING_CQE_F_NOTIF handling.
 */
uint64_t loopyIoUringRecvZeroCopy(loopyLoop *l, int sockfd, void *buf,
                                  size_t len, int flags,
                                  loopyIoUringNetCallback *cb, void *userData);

/**
 * Submit a multishot POLL operation (POLL_MULTISHOT).
 *
 * Similar to POLL_ADD but stays active and fires multiple times
 * whenever the poll condition is met.
 *
 * @param l         Event loop
 * @param fd        File descriptor to poll
 * @param poll_mask Poll events (POLLIN, POLLOUT, etc.)
 * @param cb        Completion callback
 * @param userData  User data for callback
 * @return Operation ID on success, 0 on failure
 */
uint64_t loopyIoUringPollMultishot(loopyLoop *l, int fd, uint32_t poll_mask,
                                   void (*cb)(void *userData, int32_t result),
                                   void *userData);

/**
 * Update an existing poll operation's mask without canceling.
 *
 * Updates the poll mask of an existing POLL_MULTISHOT or POLL_ADD operation
 * without canceling it. This allows dynamic adjustment of monitored events
 * while the operation remains active.
 *
 * The update operation generates a new completion, which is returned via the
 * provided callback. If the update fails, the original poll operation remains
 * active with its original mask.
 *
 * Requirements:
 *  - Linux kernel 5.13+ for POLL_UPDATE support (IORING_POLL_UPDATE_EVENTS
 * flag)
 *  - oldOpId must reference an active poll operation from loopyIoUringPoll*
 *
 * Use cases:
 *  - Dynamically changing monitored events (e.g., disable POLLOUT when buffer
 * full)
 *  - Avoiding cancel+re-add overhead for frequent mask changes
 *  - Updating poll mask while maintaining operation continuity
 *
 * Implementation note:
 *  Uses IORING_OP_POLL_REMOVE opcode with IORING_POLL_UPDATE_EVENTS flag.
 *  This is the kernel's mechanism for updating existing polls.
 *
 * @param l         Event loop
 * @param oldOpId   Operation ID of existing poll operation to update
 * @param poll_mask New poll events mask (POLLIN, POLLOUT, POLLERR, etc.)
 * @param cb        Completion callback for the update operation
 * @param userData  User data for callback
 * @return Operation ID of update operation on success, 0 on failure
 *         Note: A non-zero return does not guarantee the update succeeded;
 *         check the callback result code for actual update status
 *
 * Example:
 *   // Start polling for input
 *   uint64_t opId = loopyIoUringPollMultishot(l, fd, POLLIN, onPoll, userData);
 *
 *   // Later, also monitor for output
 *   uint64_t updateId = loopyIoUringPollUpdate(l, opId, POLLIN | POLLOUT,
 *                                              onUpdate, userData);
 *
 *   void onUpdate(void *userData, int32_t result) {
 *       if (result >= 0) {
 *           printf("Poll mask updated successfully\\n");
 *           // Original operation now monitors both input and output
 *       } else {
 *           printf("Update failed: %s\\n", strerror(-result));
 *           // Original operation still active with original mask
 *       }
 *   }
 */
uint64_t loopyIoUringPollUpdate(loopyLoop *l, uint64_t oldOpId,
                                uint32_t poll_mask,
                                void (*cb)(void *userData, int32_t result),
                                void *userData);

#endif /* __linux__ */

#endif /* LOOPY_IOURING_NET_H */
