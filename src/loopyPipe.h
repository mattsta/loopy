/* loopyPipe - Pipe Creation and Management for loopy
 *
 * Provides low-level pipe operations for IPC and inter-thread communication.
 * Complements loopyStream which provides high-level async I/O on pipes.
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

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* Forward declaration */
struct loopyLoop;

/* ====================================================================
 * Type Definitions
 * ==================================================================== */

/**
 * Opaque pipe handle.
 *
 * Represents a pipe pair with read and write ends. The pipe can be used
 * for IPC, inter-thread communication, or integrated with the event loop
 * for async I/O.
 */
typedef struct loopyPipe loopyPipe;

/**
 * Pipe flags for creation.
 */
typedef enum loopyPipeFlags {
    LOOPY_PIPE_NONE = 0,            /* Default flags */
    LOOPY_PIPE_NONBLOCK = (1 << 0), /* Set both ends non-blocking */
    LOOPY_PIPE_CLOEXEC = (1 << 1),  /* Set close-on-exec on both ends */
} loopyPipeFlags;

/**
 * Read callback for async pipe reads.
 *
 * @param pipe Pipe handle
 * @param nread Bytes read (>0), 0 on EOF, <0 on error
 * @param buf Buffer containing data (only valid if nread > 0)
 * @param userData User-provided context data
 */
typedef void loopyPipeReadCallback(loopyPipe *pipe, ssize_t nread,
                                   const void *buf, void *userData);

/**
 * Write callback for async pipe writes.
 *
 * @param pipe Pipe handle
 * @param status 0 on success, <0 on error
 * @param userData User-provided context data
 */
typedef void loopyPipeWriteCallback(loopyPipe *pipe, int status,
                                    void *userData);

/* ====================================================================
 * Pipe Creation
 * ==================================================================== */

/**
 * Create an anonymous pipe pair.
 *
 * Creates a pipe with separate read and write file descriptors. The pipe
 * can be used for IPC between parent and child processes, or between threads.
 *
 * Example:
 *   loopyPipe *pipe = loopyPipeCreate(LOOPY_PIPE_NONBLOCK |
 * LOOPY_PIPE_CLOEXEC); if (!pipe) { fprintf(stderr, "Failed to create pipe:
 * %s\n", loopyPipeGetError()); return;
 *   }
 *
 *   // Use pipe for IPC...
 *   loopyPipeClose(pipe);
 *
 * @param flags Creation flags (see loopyPipeFlags)
 * @return Pipe handle, or NULL on failure
 */
loopyPipe *loopyPipeCreate(loopyPipeFlags flags);

/**
 * Create a pipe pair from existing file descriptors.
 *
 * Wraps existing pipe file descriptors. Useful when integrating with
 * code that creates pipes directly (e.g., pipe2(), socketpair()).
 *
 * @param readFd Read end file descriptor
 * @param writeFd Write end file descriptor
 * @return Pipe handle, or NULL on failure
 *
 * Note: The pipe takes ownership of both fds and will close them on cleanup.
 */
loopyPipe *loopyPipeFromFds(int readFd, int writeFd);

/**
 * Create a named pipe (FIFO).
 *
 * Creates a FIFO special file that can be used for IPC between unrelated
 * processes. The FIFO persists until explicitly removed.
 *
 * Example:
 *   // Create FIFO
 *   if (!loopyPipeMakeFifo("/tmp/myfifo", 0666)) {
 *       fprintf(stderr, "Failed to create FIFO: %s\n", loopyPipeGetError());
 *       return;
 *   }
 *
 *   // Open FIFO for writing
 *   loopyPipe *pipe = loopyPipeOpenFifo("/tmp/myfifo", true, false);
 *
 * @param path Path to FIFO
 * @param mode Permissions mode (e.g., 0666)
 * @return true on success, false on error
 */
bool loopyPipeMakeFifo(const char *path, mode_t mode);

/**
 * Open a named pipe (FIFO).
 *
 * Opens an existing FIFO for reading or writing. Use loopyPipeMakeFifo()
 * to create the FIFO first.
 *
 * @param path Path to FIFO
 * @param write true to open for writing, false for reading
 * @param nonblock true to open in non-blocking mode
 * @return Pipe handle, or NULL on failure
 */
loopyPipe *loopyPipeOpenFifo(const char *path, bool write, bool nonblock);

/**
 * Remove a named pipe (FIFO).
 *
 * Removes a FIFO from the filesystem. The FIFO must not be in use.
 *
 * @param path Path to FIFO
 * @return true on success, false on error
 */
bool loopyPipeRemoveFifo(const char *path);

/* ====================================================================
 * Pipe Operations
 * ==================================================================== */

/**
 * Get the read end file descriptor.
 *
 * Returns the file descriptor for the read end of the pipe. Can be used
 * with low-level read operations or event loop registration.
 *
 * @param pipe Pipe handle
 * @return Read fd, or -1 if pipe is NULL or read end is closed
 */
int loopyPipeGetReadFd(const loopyPipe *pipe);

/**
 * Get the write end file descriptor.
 *
 * Returns the file descriptor for the write end of the pipe. Can be used
 * with low-level write operations or event loop registration.
 *
 * @param pipe Pipe handle
 * @return Write fd, or -1 if pipe is NULL or write end is closed
 */
int loopyPipeGetWriteFd(const loopyPipe *pipe);

/**
 * Read from pipe (blocking or non-blocking depending on flags).
 *
 * Performs a synchronous read from the pipe's read end. This is a simple
 * wrapper around read() that handles EINTR.
 *
 * @param pipe Pipe handle
 * @param buf Buffer to read into
 * @param count Maximum bytes to read
 * @return Bytes read (>0), 0 on EOF, -1 on error (check errno)
 */
ssize_t loopyPipeRead(loopyPipe *pipe, void *buf, size_t count);

/**
 * Write to pipe (blocking or non-blocking depending on flags).
 *
 * Performs a synchronous write to the pipe's write end. This is a simple
 * wrapper around write() that handles EINTR.
 *
 * @param pipe Pipe handle
 * @param buf Buffer to write from
 * @param count Bytes to write
 * @return Bytes written (>0), -1 on error (check errno)
 */
ssize_t loopyPipeWrite(loopyPipe *pipe, const void *buf, size_t count);

/**
 * Close the read end of the pipe.
 *
 * Closes the read end file descriptor. The write end remains open.
 * Useful for one-way communication patterns.
 *
 * @param pipe Pipe handle
 */
void loopyPipeCloseRead(loopyPipe *pipe);

/**
 * Close the write end of the pipe.
 *
 * Closes the write end file descriptor. The read end remains open.
 * Useful for signaling EOF to readers.
 *
 * @param pipe Pipe handle
 */
void loopyPipeCloseWrite(loopyPipe *pipe);

/**
 * Close both ends and free the pipe.
 *
 * Closes both file descriptors and frees the pipe structure.
 * Safe to call on NULL pointers.
 *
 * @param pipe Pipe handle (may be NULL)
 */
void loopyPipeClose(loopyPipe *pipe);

/* ====================================================================
 * Event Loop Integration
 * ==================================================================== */

/**
 * Start async read from pipe.
 *
 * Registers the pipe's read end with the event loop for non-blocking reads.
 * The callback is invoked when data is available or on error/EOF.
 *
 * Example:
 *   void onRead(loopyPipe *p, ssize_t nread, const void *buf, void *data) {
 *       if (nread < 0) {
 *           fprintf(stderr, "Read error\n");
 *           loopyPipeStopRead(p);
 *       } else if (nread == 0) {
 *           printf("EOF\n");
 *           loopyPipeStopRead(p);
 *       } else {
 *           printf("Read %zd bytes\n", nread);
 *       }
 *   }
 *
 *   loopyPipeReadStart(pipe, loop, onRead, NULL);
 *
 * @param pipe Pipe handle
 * @param loop Event loop
 * @param callback Read callback
 * @param userData User data for callback
 * @return true on success, false on error
 */
bool loopyPipeReadStart(loopyPipe *pipe, struct loopyLoop *loop,
                        loopyPipeReadCallback *cb, void *userData);

/**
 * Stop async read from pipe.
 *
 * Unregisters the pipe's read end from the event loop.
 *
 * @param pipe Pipe handle
 */
void loopyPipeReadStop(loopyPipe *pipe);

/**
 * Write to pipe asynchronously.
 *
 * Queues data for async write to the pipe. The callback is invoked when
 * the write completes or on error.
 *
 * @param pipe Pipe handle
 * @param loop Event loop
 * @param buf Buffer to write
 * @param count Bytes to write
 * @param callback Write callback (optional)
 * @param userData User data for callback
 * @return true on success, false on error
 *
 * Note: The buffer must remain valid until the callback is invoked.
 */
bool loopyPipeWriteAsync(loopyPipe *pipe, struct loopyLoop *loop,
                         const void *buf, size_t count,
                         loopyPipeWriteCallback *cb, void *userData);

/* ====================================================================
 * Pipe Information
 * ==================================================================== */

/**
 * Check if pipe is readable.
 *
 * Returns true if the read end is open and can be read from.
 *
 * @param pipe Pipe handle
 * @return true if readable, false otherwise
 */
bool loopyPipeIsReadable(const loopyPipe *pipe);

/**
 * Check if pipe is writable.
 *
 * Returns true if the write end is open and can be written to.
 *
 * @param pipe Pipe handle
 * @return true if writable, false otherwise
 */
bool loopyPipeIsWritable(const loopyPipe *pipe);

/**
 * Get pipe buffer size.
 *
 * Returns the size of the pipe buffer (F_GETPIPE_SZ on Linux, default on
 * others).
 *
 * @param pipe Pipe handle
 * @return Buffer size in bytes, or -1 on error
 */
ssize_t loopyPipeGetBufferSize(const loopyPipe *pipe);

/**
 * Set pipe buffer size.
 *
 * Sets the size of the pipe buffer (F_SETPIPE_SZ on Linux, no-op on others).
 *
 * @param pipe Pipe handle
 * @param size Desired buffer size in bytes
 * @return true on success, false on error or not supported
 */
bool loopyPipeSetBufferSize(loopyPipe *pipe, size_t size);

/* ====================================================================
 * Error Handling
 * ==================================================================== */

/**
 * Get the last error message.
 *
 * Returns a human-readable description of the last error that occurred in
 * any loopyPipe function on this thread. Returns NULL if no error has occurred.
 *
 * The error message is stored in thread-local storage and is valid until
 * the next loopyPipe function call on this thread.
 *
 * @return Error message string, or NULL if no error
 */
const char *loopyPipeGetError(void);

/* ====================================================================
 * Handle Accessors
 * ==================================================================== */

/**
 * Get the event loop associated with this pipe.
 *
 * Returns the event loop if the pipe has been registered for async I/O,
 * NULL otherwise.
 *
 * @param pipe Pipe handle
 * @return Event loop or NULL
 */
struct loopyLoop *loopyPipeGetLoop(const loopyPipe *pipe);

/**
 * Get user data associated with this pipe.
 *
 * @param pipe Pipe handle
 * @return User data pointer
 */
void *loopyPipeGetData(const loopyPipe *pipe);

/**
 * Set user data associated with this pipe.
 *
 * @param pipe Pipe handle
 * @param data User data pointer
 */
void loopyPipeSetData(loopyPipe *pipe, void *data);

/**
 * Check if pipe is actively reading via the event loop.
 *
 * @param pipe Pipe handle
 * @return true if reading is active
 */
bool loopyPipeIsActive(const loopyPipe *pipe);
