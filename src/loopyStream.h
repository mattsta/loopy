/* loopyStream - Unified duplex I/O abstraction for loopy event loop
 *
 * Stream abstraction over TCP, pipes, and other bidirectional I/O.
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

#pragma once

#include "loopyPlatform.h"

#include "loopy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* ====================================================================
 * Types
 * ==================================================================== */

/**
 * Opaque stream handle.
 */
typedef struct loopyStream loopyStream;

/**
 * Stream types.
 */
typedef enum loopyStreamType {
    LOOPY_STREAM_TCP,
    LOOPY_STREAM_PIPE,
    LOOPY_STREAM_UNKNOWN
} loopyStreamType;

/**
 * Allocation callback - called before each read to get a buffer.
 *
 * @param stream    The stream
 * @param suggested Suggested buffer size
 * @param buf       OUT: pointer to receive buffer address
 * @param bufLen    OUT: pointer to receive buffer length
 * @param userData  User data from read start
 *
 * The implementation should set *buf to a writable buffer and *bufLen
 * to its size. Set *buf to NULL to indicate allocation failure.
 */
typedef void loopyStreamAllocCallback(loopyStream *stream, size_t suggested,
                                      void **buf, size_t *bufLen,
                                      void *userData);

/**
 * Read callback - called when data is available or on error.
 *
 * @param stream   The stream
 * @param nread    Bytes read (>0), 0 on EOF, <0 on error
 * @param buf      Buffer containing data (only valid if nread > 0)
 * @param userData User data from read start
 *
 * Common nread values:
 *   > 0: bytes successfully read
 *   0: EOF (peer closed connection)
 *   < 0: error (use errno for details)
 */
typedef void loopyStreamReadCallback(loopyStream *stream, ssize_t nread,
                                     const void *buf, void *userData);

/**
 * Write callback - called when write completes.
 *
 * @param stream   The stream
 * @param status   0 on success, < 0 on error
 * @param userData User data from write call
 */
typedef void loopyStreamWriteCallback(loopyStream *stream, int status,
                                      void *userData);

/**
 * Connection callback - called when a new connection arrives.
 *
 * @param server   The server stream
 * @param status   0 on success, < 0 on error
 * @param userData User data from listen call
 */
typedef void loopyStreamConnectionCallback(loopyStream *server, int status,
                                           void *userData);

/**
 * Shutdown callback - called when shutdown completes.
 *
 * @param stream   The stream
 * @param status   0 on success, < 0 on error
 * @param userData User data from shutdown call
 */
typedef void loopyStreamShutdownCallback(loopyStream *stream, int status,
                                         void *userData);

/**
 * Close callback - called when close completes.
 *
 * @param stream   The stream (will be freed after callback returns)
 * @param userData User data from close call
 */
typedef void loopyStreamCloseCallback(loopyStream *stream, void *userData);

/* ====================================================================
 * Lifecycle
 * ==================================================================== */

/**
 * Create a new TCP stream.
 *
 * Creates an unconnected TCP stream socket. The socket is non-blocking and
 * ready for use with event loop operations. Use loopyStreamConnect() to
 * establish a connection.
 *
 * @param loop The event loop (must not be NULL)
 * @return New stream handle, or NULL on error (memory allocation failure)
 *
 * @note The returned stream is owned by the caller and must be freed with
 *       loopyStreamClose(). The socket is already in non-blocking mode.
 *
 * @see loopyStreamConnect()
 * @see loopyStreamNewPipe()
 * @see loopyStreamClose()
 */
loopyStream *loopyStreamNewTcp(loopyLoop *loop);

/**
 * Create a new pipe stream handle.
 *
 * Creates a pipe stream handle without an underlying socket. The actual pipe
 * file descriptors must be set up separately and attached via
 * loopyStreamFromFd(). Useful for IPC over pipes, including Unix domain
 * sockets.
 *
 * @param loop The event loop (must not be NULL)
 * @return New stream handle, or NULL on memory allocation failure
 *
 * @note The returned handle is not connected to any file descriptor. Use
 *       loopyStreamFromFd() to attach an existing fd (e.g., pipe, socket).
 *       Supports file descriptor passing via loopyStreamWriteWithFd().
 *
 * @see loopyStreamFromFd()
 * @see loopyStreamWriteWithFd()
 * @see loopyStreamReadStartWithFd()
 * @see loopyStreamNewTcp()
 */
loopyStream *loopyStreamNewPipe(loopyLoop *loop);

/**
 * Create a stream from an existing file descriptor.
 *
 * Wraps an existing file descriptor (socket, pipe, etc.) in a stream handle.
 * The stream takes ownership of the fd and will close it when the stream
 * is freed via loopyStreamClose(). The fd is set to non-blocking mode.
 *
 * @param loop The event loop (must not be NULL)
 * @param fd   Existing file descriptor (must be >= 0)
 * @param type Stream type (LOOPY_STREAM_TCP, LOOPY_STREAM_PIPE, etc.)
 * @return New stream handle, or NULL on error (bad fd or memory failure)
 *
 * Example:
 * @code
 *   // Wrap an accepted connection from accept()
 *   int clientFd = accept(serverFd, NULL, NULL);
 *   loopyStream *client = loopyStreamFromFd(loop, clientFd, LOOPY_STREAM_TCP);
 *   if (client) {
 *       loopyStreamReadStart(client, allocCb, readCb, userData);
 *   }
 * @endcode
 *
 * @note The stream owns the fd after this call. Do not close() it separately
 *       - loopyStreamClose() will handle it. Stream marks the fd as
 * non-blocking.
 *
 * @see loopyStreamNewTcp()
 * @see loopyStreamNewPipe()
 * @see loopyStreamClose()
 */
loopyStream *loopyStreamFromFd(loopyLoop *loop, int fd, loopyStreamType type);

/**
 * Close and free a stream.
 *
 * Stops all I/O operations, drains pending writes with error status, closes
 * the underlying file descriptor, and calls the close callback. Safe to call
 * on NULL. After return, the stream handle is freed and must not be used.
 *
 * Example:
 * @code
 *   void onStreamClosed(loopyStream *s, void *userData) {
 *       printf("Stream closed\n");
 *       int *count = userData;
 *       (*count)--;
 *   }
 *
 *   int openStreams = 1;
 *   loopyStreamClose(stream, onStreamClosed, &openStreams);
 *   // openStreams is now 0
 * @endcode
 *
 * @param stream   The stream to close, or NULL (safe, no-op)
 * @param cb       Close callback invoked after cleanup (optional, may be NULL)
 * @param userData User data passed to close callback
 *
 * @note All pending write callbacks are called with error status (-1) before
 *       the stream is freed. Reading is automatically stopped. Safe to call
 *       multiple times or from callbacks.
 *
 * @see loopyStreamNewTcp()
 * @see loopyStreamFromFd()
 */
void loopyStreamClose(loopyStream *stream, loopyStreamCloseCallback *cb,
                      void *userData);

/* ====================================================================
 * TCP Server Operations
 * ==================================================================== */

/**
 * Bind a TCP stream to an IPv4 address.
 *
 * Binds the stream's socket to an IPv4 address and port, preparing it for
 * listening. Must be called before loopyStreamListen().
 *
 * @param stream The stream (must be TCP type)
 * @param addr   IPv4 address to bind ("0.0.0.0" for all interfaces, "127.0.0.1"
 * for localhost)
 * @param port   Port number (1-65535, or 0 for OS-assigned)
 * @return true on success, false on error
 *
 * @note After bind succeeds, you may query the assigned port with
 *       loopyStreamGetSockName() if port was 0.
 *
 * @see loopyStreamBind6()
 * @see loopyStreamListen()
 * @see loopyStreamGetSockName()
 */
bool loopyStreamBind(loopyStream *stream, const char *addr, int port);

/**
 * Bind a TCP stream to an IPv6 address.
 *
 * Like loopyStreamBind(), but for IPv6. The socket is recreated as IPv6 and
 * configured with IPV6_V6ONLY based on ipv6Only parameter.
 *
 * @param stream   The stream (will have its socket recreated for IPv6)
 * @param addr     IPv6 address to bind ("::" for all interfaces, "::1" for
 * localhost)
 * @param port     Port number (1-65535, or 0 for OS-assigned)
 * @param ipv6Only If true, set IPV6_V6ONLY (IPv6-only, reject IPv4-mapped).
 *                 If false, allow dual-stack (accepts both IPv4 and IPv6).
 * @return true on success, false on error
 *
 * @note The stream's socket is closed and recreated with AF_INET6.
 *       ipv6Only=true is recommended to avoid address conflicts.
 *
 * @see loopyStreamBind()
 * @see loopyStreamListen()
 */
bool loopyStreamBind6(loopyStream *stream, const char *addr, int port,
                      bool ipv6Only);

/**
 * Start listening for incoming connections.
 *
 * Calls listen() on the bound socket and registers a read callback to detect
 * incoming connections. The callback is invoked each time a connection arrives.
 *
 * Example:
 * @code
 *   void onNewConnection(loopyStream *server, int status, void *userData) {
 *       if (status == 0) {
 *           loopyStream *client = loopyStreamAccept(server);
 *           if (client) {
 *               // Handle the new client
 *               loopyStreamReadStart(client, allocCb, readCb, NULL);
 *           }
 *       }
 *   }
 *
 *   loopyStream *server = loopyStreamNewTcp(loop);
 *   loopyStreamBind(server, "0.0.0.0", 8080);
 *   loopyStreamListen(server, 128, onNewConnection, NULL);
 * @endcode
 *
 * @param stream   The stream (must be TCP and bound via loopyStreamBind())
 * @param backlog  Connection queue size (typically 128-512, OS may increase)
 * @param cb       Callback invoked when connection arrives or on error
 * @param userData User data for callback
 * @return true on success, false on error (check loopyStreamGetError())
 *
 * @note The callback is invoked at event loop time, not immediately.
 *       Call loopyStreamAccept() from the callback to accept connections.
 *
 * @see loopyStreamBind()
 * @see loopyStreamAccept()
 */
bool loopyStreamListen(loopyStream *stream, int backlog,
                       loopyStreamConnectionCallback *cb, void *userData);

/**
 * Accept a new connection from a listening stream.
 *
 * Must be called from the connection callback of a listening stream.
 * Creates and returns a new stream handle for the accepted client connection.
 * The returned stream is connected and ready for read/write operations.
 *
 * @param server The server stream (must be in listening state)
 * @return New client stream handle on success, NULL on error (no pending
 * connection or accept failed). Check loopyStreamGetError(server) for details.
 *
 * Example:
 * @code
 *   void acceptCallback(loopyStream *server, int status, void *userData) {
 *       loopyStream *client = loopyStreamAccept(server);
 *       if (client) {
 *           printf("Accepted connection from %s\n",
 *                  loopyStreamGetError(client)); // For logging peer
 *           loopyStreamReadStart(client, allocCb, readCb, NULL);
 *       }
 *   }
 * @endcode
 *
 * @note The returned stream is owned by the caller and must be freed with
 *       loopyStreamClose(). The socket is already non-blocking.
 *
 * @see loopyStreamListen()
 * @see loopyStreamGetPeerName()
 * @see loopyStreamClose()
 */
loopyStream *loopyStreamAccept(loopyStream *server);

/* ====================================================================
 * TCP Client Operations
 * ==================================================================== */

/**
 * Connect to a remote address (non-blocking).
 *
 * Initiates a non-blocking TCP connection. For local connections (same
 * machine), connection may complete immediately and callback invoked
 * synchronously. For remote connections, the callback is invoked when the
 * connection succeeds or fails.
 *
 * Example:
 * @code
 *   void onConnected(loopyStream *stream, int status, void *userData) {
 *       if (status == 0) {
 *           printf("Connected!\n");
 *           loopyStreamReadStart(stream, allocCb, readCb, NULL);
 *       } else {
 *           printf("Connection failed: %s\n", loopyStreamGetError(stream));
 *       }
 *   }
 *
 *   loopyStream *client = loopyStreamNewTcp(loop);
 *   loopyStreamConnect(client, "example.com", 80, onConnected, NULL);
 * @endcode
 *
 * @param stream   The stream (must be TCP type)
 * @param addr     Remote address (hostname or IP, IPv6 must be IP format)
 * @param port     Remote port number (1-65535)
 * @param cb       Callback invoked when connection completes (optional)
 * @param userData User data for callback
 * @return true if connect initiated successfully, false on immediate error
 *         (invalid parameters, socket creation failed, etc.)
 *
 * @note The connection is asynchronous. Status callback receives status 0 on
 *       success, -1 on failure. If connection completes immediately
 * (localhost), callback is called before function returns.
 *
 * @see loopyStreamNewTcp()
 * @see loopyStreamListen()
 * @see loopyStreamGetError()
 */
bool loopyStreamConnect(loopyStream *stream, const char *addr, int port,
                        loopyStreamWriteCallback *cb, void *userData);

/* ====================================================================
 * Reading
 * ==================================================================== */

/**
 * Start reading from the stream.
 *
 * Registers read callbacks and enables reading. The alloc callback is invoked
 * before each read to acquire a buffer, and the read callback is invoked when
 * data arrives, EOF occurs, or an error happens.
 *
 * Example:
 * @code
 *   void onAllocBuffer(loopyStream *s, size_t suggested, void **buf,
 *                      size_t *bufLen, void *userData) {
 *       static char buffer[4096];
 *       *buf = buffer;
 *       *bufLen = sizeof(buffer);
 *   }
 *
 *   void onDataRead(loopyStream *s, ssize_t nread, const void *buf,
 *                   void *userData) {
 *       if (nread > 0) {
 *           printf("Received %ld bytes\n", nread);
 *       } else if (nread == 0) {
 *           printf("EOF\n");
 *           loopyStreamClose(s, NULL, NULL);
 *       } else {
 *           printf("Error: %s\n", loopyStreamGetError(s));
 *       }
 *   }
 *
 *   loopyStreamReadStart(stream, onAllocBuffer, onDataRead, NULL);
 * @endcode
 *
 * @param stream   The stream
 * @param alloc    Allocation callback (required, called before each read)
 * @param read     Read callback (required, called when data/error arrives)
 * @param userData User data for callbacks
 * @return true on success, false on error (stream invalid or already reading)
 *
 * @note Both callbacks must be non-NULL. The alloc callback provides buffers
 *       for reading. If it returns NULL buffer, reading stops. This enables
 *       backpressure handling.
 *
 * @see loopyStreamReadStop()
 * @see loopyStreamReadStartWithFd()
 */
bool loopyStreamReadStart(loopyStream *stream, loopyStreamAllocCallback *alloc,
                          loopyStreamReadCallback *read, void *userData);

/**
 * Stop reading from the stream.
 *
 * Disables read callbacks. No more data callbacks will be invoked after this.
 * Safe to call if not reading. Does not close the stream.
 *
 * @param stream The stream (may be NULL or not reading)
 *
 * @note This is idempotent. Safe to call multiple times or when not reading.
 *
 * @see loopyStreamReadStart()
 * @see loopyStreamIsReading()
 */
void loopyStreamReadStop(loopyStream *stream);

/**
 * Check if stream is currently reading.
 *
 * Returns true if loopyStreamReadStart() has been called and
 * loopyStreamReadStop() has not been called since.
 *
 * @param stream The stream (may be NULL)
 * @return true if read callbacks are active, false otherwise
 *
 * @see loopyStreamReadStart()
 * @see loopyStreamReadStop()
 */
bool loopyStreamIsReading(const loopyStream *stream);

/* ====================================================================
 * Writing
 * ==================================================================== */

/**
 * Write data to the stream (asynchronous, queued).
 *
 * Queues data for writing. The data is copied internally, so the input buffer
 * can be freed immediately. The callback is invoked when the write completes
 * (data sent to kernel buffer).
 *
 * Example:
 * @code
 *   void onWriteComplete(loopyStream *s, int status, void *userData) {
 *       if (status == 0) {
 *           printf("Data sent\n");
 *       } else {
 *           printf("Write failed\n");
 *       }
 *   }
 *
 *   const char *msg = "Hello\n";
 *   loopyStreamWrite(stream, msg, strlen(msg), onWriteComplete, NULL);
 * @endcode
 *
 * @param stream   The stream (must be writable)
 * @param data     Data to write (copied internally)
 * @param len      Data length (must be > 0)
 * @param cb       Write callback, invoked when write completes (optional)
 * @param userData User data for callback
 * @return true if write queued, false on error (stream invalid/closed)
 *
 * @note Data is copied, so you can free the buffer immediately. Multiple
 *       writes are queued and processed in order. Use
 * loopyStreamGetWriteQueueSize() to implement backpressure.
 *
 * @see loopyStreamTryWrite()
 * @see loopyStreamGetWriteQueueSize()
 * @see loopyStreamWriteWithFd()
 */
bool loopyStreamWrite(loopyStream *stream, const void *data, size_t len,
                      loopyStreamWriteCallback *cb, void *userData);

/**
 * Try to write data synchronously without buffering.
 *
 * Attempts to write immediately without queuing. Useful for low-latency
 * scenarios when you know the socket is likely writable. Unlike
 * loopyStreamWrite(), no data is copied or queued - either all data is written
 * or the call returns indicating the socket would block.
 *
 * @param stream The stream (must be writable)
 * @param data   Data to write
 * @param len    Data length
 * @return Number of bytes written (>= 0), or -1 on error. Returns EAGAIN
 *         (or similar) if socket would block - use loopyStreamWrite() instead.
 *
 * @note This is a synchronous (blocking) call that may write 0 to len bytes.
 *       Returns errno (via return value) for actual write errors. EAGAIN/
 *       EWOULDBLOCK means "try again later" - suitable for fallback to async.
 *
 * @see loopyStreamWrite()
 * @see loopyStreamGetWriteQueueSize()
 */
ssize_t loopyStreamTryWrite(loopyStream *stream, const void *data, size_t len);

/**
 * Get the total bytes queued for writing.
 *
 * Returns the sum of all bytes in pending write requests. Useful for
 * implementing backpressure and flow control.
 *
 * @param stream The stream (may be NULL)
 * @return Total bytes queued for writing, 0 if stream is NULL or no data queued
 *
 * Example:
 * @code
 *   // Only queue if buffer not too full
 *   if (loopyStreamGetWriteQueueSize(stream) < 64*1024) {
 *       loopyStreamWrite(stream, data, len, cb, userData);
 *   } else {
 *       // Backpressure: stop sending data
 *   }
 * @endcode
 *
 * @see loopyStreamGetWriteQueueCount()
 * @see loopyStreamWrite()
 */
size_t loopyStreamGetWriteQueueSize(const loopyStream *stream);

/**
 * Get the number of pending write requests.
 *
 * Returns the count of write() calls that haven't completed yet.
 *
 * @param stream The stream (may be NULL)
 * @return Number of pending writes, 0 if stream is NULL or queue empty
 *
 * @note This counts write requests, not bytes. Use
 * loopyStreamGetWriteQueueSize() for byte count. Multiple writes of varying
 * sizes may queue differently.
 *
 * @see loopyStreamGetWriteQueueSize()
 */
size_t loopyStreamGetWriteQueueCount(const loopyStream *stream);

/* ====================================================================
 * Shutdown
 * ==================================================================== */

/**
 * Shutdown the write side of the stream (half-close).
 *
 * Gracefully closes the write direction of the connection (sends FIN), but
 * reading remains open. Useful for protocols where the client sends all data
 * first, then waits for response. The callback is invoked when shutdown
 * completes.
 *
 * @param stream   The stream
 * @param cb       Shutdown callback invoked when complete (optional)
 * @param userData User data for callback
 * @return true if shutdown initiated successfully, false on error
 *
 * @note Does not close the stream entirely - use loopyStreamClose() for that.
 *       Can still read after shutdown. Typically used in request-response
 * scenarios.
 *
 * @see loopyStreamClose()
 * @see loopyStreamReadStart()
 */
bool loopyStreamShutdown(loopyStream *stream, loopyStreamShutdownCallback *cb,
                         void *userData);

/* ====================================================================
 * Properties
 * ==================================================================== */

/**
 * Check if stream is readable.
 *
 * Returns true if the stream can be read from (not EOF, not closed).
 *
 * @param stream The stream (may be NULL)
 * @return true if readable (no EOF and not closed), false otherwise
 *
 * @see loopyStreamIsWritable()
 * @see loopyStreamReadStart()
 */
bool loopyStreamIsReadable(const loopyStream *stream);

/**
 * Check if stream is writable.
 *
 * Returns true if the stream can be written to (not shut down, not closed).
 *
 * @param stream The stream (may be NULL)
 * @return true if writable (not shut down and not closed), false otherwise
 *
 * @see loopyStreamIsReadable()
 * @see loopyStreamWrite()
 */
bool loopyStreamIsWritable(const loopyStream *stream);

/**
 * Get the stream type.
 *
 * Returns the type of stream (TCP, PIPE, etc.) as set at creation.
 *
 * @param stream The stream (may be NULL)
 * @return Stream type (LOOPY_STREAM_TCP, LOOPY_STREAM_PIPE, or
 * LOOPY_STREAM_UNKNOWN)
 *
 * @see loopyStreamNewTcp()
 * @see loopyStreamNewPipe()
 */
loopyStreamType loopyStreamGetType(const loopyStream *stream);

/**
 * Get the underlying file descriptor.
 *
 * Returns the raw socket/file descriptor. Useful for debugging or low-level
 * operations, but for standard I/O operations, use loopyStreamRead/Write*.
 *
 * @param stream The stream (may be NULL)
 * @return File descriptor (>= 0), or -1 if stream is NULL or invalid
 *
 * @note Direct fd manipulation while the stream is active may interfere
 *       with event loop operations.
 *
 * @see loopyStreamFromFd()
 */
int loopyStreamGetFd(const loopyStream *stream);

/**
 * Get the event loop associated with this stream.
 *
 * @param stream The stream (may be NULL)
 * @return Event loop pointer, or NULL if stream is NULL
 *
 * @see loopyStreamNewTcp()
 * @see loopyStreamFromFd()
 */
loopyLoop *loopyStreamGetLoop(const loopyStream *stream);

/**
 * Get user data from stream.
 *
 * Returns the opaque pointer set via loopyStreamSetData() or NULL if not set.
 * Useful for associating application context with a stream.
 *
 * @param stream The stream (may be NULL)
 * @return User data pointer that was set, or NULL
 *
 * @see loopyStreamSetData()
 */
void *loopyStreamGetData(const loopyStream *stream);

/**
 * Set user data on stream.
 *
 * Associates an opaque pointer with the stream. Retrievable via
 * loopyStreamGetData(). Useful for storing connection-specific state
 * without allocating additional structures.
 *
 * @param stream The stream (may be NULL)
 * @param data User data pointer (may be NULL)
 *
 * @note Can be called multiple times to update the pointer.
 *
 * @see loopyStreamGetData()
 */
void loopyStreamSetData(loopyStream *stream, void *data);

/**
 * Get the local address this stream is bound to.
 *
 * Retrieves the local endpoint address and port (for TCP streams).
 * Useful for logging which address a stream is listening on or connected from.
 *
 * @param stream  The stream (TCP type)
 * @param addr    Output buffer for address string
 * @param addrLen Size of addr buffer (must be at least INET6_ADDRSTRLEN = 46)
 * @param port    Output pointer for port number (optional, may be NULL)
 * @return true on success, false on error (stream invalid or not TCP)
 *
 * @note Useful after binding with port 0 to find the OS-assigned port.
 *
 * @see loopyStreamGetPeerName()
 * @see loopyStreamBind()
 */
bool loopyStreamGetSockName(loopyStream *stream, char *addr, size_t addrLen,
                            int *port);

/**
 * Get the remote peer address for a connected stream.
 *
 * Retrieves the remote endpoint address and port (for TCP streams).
 * Useful for logging connected peer information.
 *
 * @param stream  The stream (must be connected)
 * @param addr    Output buffer for address string
 * @param addrLen Size of addr buffer (must be at least INET6_ADDRSTRLEN = 46)
 * @param port    Output pointer for port number (optional, may be NULL)
 * @return true on success, false on error (not connected or not TCP)
 *
 * @see loopyStreamGetSockName()
 * @see loopyStreamConnect()
 * @see loopyStreamAccept()
 */
bool loopyStreamGetPeerName(loopyStream *stream, char *addr, size_t addrLen,
                            int *port);

/**
 * Get the last error message from the stream.
 *
 * Returns a human-readable error message from the most recent failed operation
 * on this stream. Useful for debugging I/O failures.
 *
 * @param stream The stream (may be NULL)
 * @return Error message string, or empty string if no error or stream is NULL
 *
 * @note The error message is stored internally and persists until the next
 *       failed operation. Safe to call on NULL streams.
 *
 * @see loopyStreamBind()
 * @see loopyStreamConnect()
 * @see loopyStreamReadStart()
 */
const char *loopyStreamGetError(const loopyStream *stream);

/* ====================================================================
 * File Descriptor Passing (IPC)
 *
 * These functions allow passing file descriptors between processes
 * over Unix domain sockets using SCM_RIGHTS control messages.
 *
 * Use cases:
 *   - Load balancing: Pass accepted connections to worker processes
 *   - Privilege separation: Privileged process passes opened fds
 *   - Socket activation: Pass listening sockets from init system
 * ==================================================================== */

/**
 * Maximum file descriptors that can be sent/received in one message.
 */
#define LOOPY_STREAM_MAX_FDS 8

/**
 * Callback for receiving data with file descriptors.
 *
 * @param stream   The stream (must be a pipe/Unix domain socket)
 * @param nread    Bytes read (>0), 0 on EOF, <0 on error
 * @param buf      Buffer containing data (only valid if nread > 0)
 * @param fds      Array of received file descriptors (caller takes ownership)
 * @param nfds     Number of file descriptors received (0 if none)
 * @param userData User data from read start
 *
 * IMPORTANT: The caller takes ownership of received file descriptors
 * and must close them when done. The fds array is only valid during
 * the callback.
 */
typedef void loopyStreamReadFdCallback(loopyStream *stream, ssize_t nread,
                                       const void *buf, const int *fds,
                                       int nfds, void *userData);

/**
 * Start reading with file descriptor receiving capability.
 *
 * Like loopyStreamReadStart(), but the callback also receives any file
 * descriptors sent via SCM_RIGHTS. Used for IPC scenarios like load balancing
 * where servers pass accepted connections to workers.
 *
 * Example:
 * @code
 *   void onAllocBuffer(loopyStream *s, size_t suggested, void **buf,
 *                      size_t *bufLen, void *userData) {
 *       static char buffer[256];
 *       *buf = buffer;
 *       *bufLen = sizeof(buffer);
 *   }
 *
 *   void onDataWithFds(loopyStream *s, ssize_t nread, const void *buf,
 *                      const int *fds, int nfds, void *userData) {
 *       if (nfds > 0) {
 *           printf("Received %d file descriptors\n", nfds);
 *           for (int i = 0; i < nfds; i++) {
 *               // Process received fd
 *               close(fds[i]);  // Example: close immediately
 *           }
 *       }
 *       if (nread > 0) {
 *           printf("And %ld bytes of data\n", nread);
 *       }
 *   }
 *
 *   loopyStreamReadStartWithFd(pipe, onAllocBuffer,
 *                              (loopyStreamReadFdCallback*)onDataWithFds,
 * NULL);
 * @endcode
 *
 * @param stream   The stream (must be LOOPY_STREAM_PIPE type)
 * @param alloc    Allocation callback (required)
 * @param read     Read callback that receives fds (required)
 * @param userData User data for callbacks
 * @return true on success, false if not a pipe or on error
 *
 * @note Only works on Unix domain sockets and pipes. The FD array is passed
 *       to the callback and the caller takes ownership - must close them.
 *       The fds array is only valid during the callback.
 *
 * @see loopyStreamReadStart()
 * @see loopyStreamWriteWithFd()
 * @see loopyStreamCanPassFd()
 */
bool loopyStreamReadStartWithFd(loopyStream *stream,
                                loopyStreamAllocCallback *alloc,
                                loopyStreamReadFdCallback *read,
                                void *userData);

/**
 * Write data and send file descriptor(s) to the peer.
 *
 * Sends data along with one or more file descriptors using SCM_RIGHTS.
 * Used for IPC scenarios like load balancing or privilege separation.
 * Only works on Unix domain sockets.
 *
 * Example - Load balancer passes accepted connection to worker:
 * @code
 *   void acceptCallback(loopyStream *server, int status, void *userData) {
 *       loopyStream *client = loopyStreamAccept(server);
 *       if (client) {
 *           int clientFd = loopyStreamGetFd(client);
 *           loopyStream *workerPipe = (loopyStream*)userData;
 *
 *           // Send connection to worker
 *           loopyStreamWriteWithFd(workerPipe, "C", 1, &clientFd, 1,
 *                                  NULL, NULL);
 *
 *           // Worker now owns the fd
 *           loopyStreamClose(client, NULL, NULL);
 *       }
 *   }
 * @endcode
 *
 * @param stream   The stream (must be LOOPY_STREAM_PIPE type)
 * @param data     Data to write (required, at least 1 byte)
 * @param len      Data length (must be >= 1)
 * @param fds      Array of file descriptors to send (required)
 * @param nfds     Number of file descriptors (1 to LOOPY_STREAM_MAX_FDS)
 * @param cb       Write callback, called when send completes (optional)
 * @param userData User data for callback
 * @return true if write queued successfully, false if not a pipe or on error
 *
 * @note FD passing requires at least 1 byte of data. The fds array is copied,
 *       so you can close the fds immediately after this call (the receiver
 *       gets new file descriptors). This is how load balancing works - master
 *       accepts connection, passes fd to worker, then closes its reference.
 *
 * @see loopyStreamReadStartWithFd()
 * @see loopyStreamCanPassFd()
 * @see loopyStreamWrite()
 */
bool loopyStreamWriteWithFd(loopyStream *stream, const void *data, size_t len,
                            const int *fds, int nfds,
                            loopyStreamWriteCallback *cb, void *userData);

/**
 * Check if file descriptor passing is supported on this stream.
 *
 * Returns true if the stream type (Unix domain socket/pipe) supports
 * SCM_RIGHTS file descriptor passing.
 *
 * @param stream The stream (may be NULL)
 * @return true if stream is a PIPE type (Unix domain socket), false otherwise
 *
 * @note TCP streams return false. Pipe streams return true regardless of
 *       whether actual data has been transmitted yet.
 *
 * @see loopyStreamReadStartWithFd()
 * @see loopyStreamWriteWithFd()
 */
bool loopyStreamCanPassFd(const loopyStream *stream);
