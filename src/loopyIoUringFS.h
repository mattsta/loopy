/* loopyIoUringFS - io_uring file operations interface
 *
 * Public API for submitting file operations via io_uring when available.
 * These operations provide zero-syscall file I/O on Linux 5.1+ with io_uring
 * support.
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

#ifndef LOOPY_IOURING_FS_H
#define LOOPY_IOURING_FS_H

#include "loopyPlatform.h"
#include "loopy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/uio.h> /* For struct iovec */

#ifdef __linux__

/* ====================================================================
 * Types
 * ==================================================================== */

/**
 * Callback for io_uring file operation completion.
 *
 * @param userData User data passed during submission
 * @param result   Operation result:
 *                 - READ/WRITE: bytes transferred, or -errno on error
 *                 - OPEN: file descriptor, or -errno on error
 *                 - CLOSE: 0 on success, or -errno on error
 *                 - FSYNC: 0 on success, or -errno on error
 */
typedef void loopyIoUringFileCallback(void *userData, int32_t result);

/* ====================================================================
 * File Operations
 * ==================================================================== */

/**
 * Submit a READ operation via io_uring (if available).
 *
 * Reads data from a file descriptor into a buffer. The operation completes
 * asynchronously and the callback is invoked when done.
 *
 * @param l        Event loop
 * @param fd       File descriptor to read from
 * @param buf      Buffer to read into (must remain valid until completion)
 * @param len      Number of bytes to read
 * @param offset   File offset to read from (-1 for current position)
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure or if io_uring not available
 */
uint64_t loopyIoUringRead(loopyLoop *l, int fd, void *buf, size_t len,
                          off_t offset, loopyIoUringFileCallback *cb,
                          void *userData);

/**
 * Submit a WRITE operation via io_uring (if available).
 *
 * Writes data from a buffer to a file descriptor. The operation completes
 * asynchronously and the callback is invoked when done.
 *
 * @param l        Event loop
 * @param fd       File descriptor to write to
 * @param buf      Buffer to write from (must remain valid until completion)
 * @param len      Number of bytes to write
 * @param offset   File offset to write to (-1 for current position)
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure or if io_uring not available
 */
uint64_t loopyIoUringWrite(loopyLoop *l, int fd, const void *buf, size_t len,
                           off_t offset, loopyIoUringFileCallback *cb,
                           void *userData);

/**
 * Submit an OPENAT operation via io_uring (if available).
 *
 * Opens a file relative to a directory file descriptor. The operation
 * completes asynchronously and the callback receives the opened file
 * descriptor or an error.
 *
 * @param l        Event loop
 * @param dirfd    Directory fd (AT_FDCWD for current working directory)
 * @param path     Path to open (must remain valid until completion)
 * @param flags    Open flags (O_RDONLY, O_WRONLY, O_CREAT, etc.)
 * @param mode     File mode for O_CREAT (e.g., 0644)
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure or if io_uring not available
 */
uint64_t loopyIoUringOpenat(loopyLoop *l, int dirfd, const char *path,
                            int flags, mode_t mode,
                            loopyIoUringFileCallback *cb, void *userData);

/**
 * Submit a CLOSE operation via io_uring (if available).
 *
 * Closes a file descriptor. The operation completes asynchronously and
 * the callback is invoked when done.
 *
 * @param l        Event loop
 * @param fd       File descriptor to close
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure or if io_uring not available
 */
uint64_t loopyIoUringClose(loopyLoop *l, int fd, loopyIoUringFileCallback *cb,
                           void *userData);

/**
 * Submit an FSYNC operation via io_uring (if available).
 *
 * Synchronizes a file's in-core state with storage. The operation completes
 * asynchronously and the callback is invoked when done.
 *
 * @param l        Event loop
 * @param fd       File descriptor to sync
 * @param datasync true for fdatasync (data only), false for fsync (data +
 * metadata)
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure or if io_uring not available
 */
uint64_t loopyIoUringFsync(loopyLoop *l, int fd, bool datasync,
                           loopyIoUringFileCallback *cb, void *userData);

/* ====================================================================
 * Fixed Buffer Operations (Zero-Copy I/O)
 * ==================================================================== */

/**
 * Register fixed buffers with io_uring for zero-copy operations.
 *
 * Fixed buffers are registered once and then referenced by index in
 * subsequent read/write operations, eliminating buffer mapping overhead.
 *
 * Benefits:
 *  - Zero buffer mapping overhead on every I/O operation
 *  - Improved performance for repeated I/O on same buffers
 *  - Kernel can optimize buffer access paths
 *
 * Requirements:
 *  - Buffers must remain valid while registered
 *  - Buffer addresses and sizes are fixed (cannot be changed)
 *  - Maximum of 1024 buffers can be registered (kernel limit)
 *
 * @param l        Event loop
 * @param buffers  Array of iovec structures describing buffers to register
 * @param count    Number of buffers in array (max 1024)
 * @return true on success, false on failure or if io_uring not available
 *
 * Example:
 *   struct iovec buffers[2];
 *   buffers[0].iov_base = malloc(4096);
 *   buffers[0].iov_len = 4096;
 *   buffers[1].iov_base = malloc(8192);
 *   buffers[1].iov_len = 8192;
 *
 *   if (loopyIoUringRegisterBuffers(l, buffers, 2)) {
 *       // Use loopyIoUringReadFixed/WriteFixed with index 0 or 1
 *   }
 */
bool loopyIoUringRegisterBuffers(loopyLoop *l, struct iovec *buffers,
                                 uint32_t count);

/**
 * Unregister previously registered fixed buffers.
 *
 * After unregistering, the buffers can be freed or modified. Any pending
 * operations using fixed buffers will complete with the old registrations.
 *
 * @param l Event loop
 * @return true on success, false on failure or if no buffers registered
 */
bool loopyIoUringUnregisterBuffers(loopyLoop *l);

/**
 * Check if fixed buffers are currently registered.
 *
 * @param l Event loop
 * @return true if buffers are registered, false otherwise
 */
bool loopyIoUringHasFixedBuffers(loopyLoop *l);

/**
 * Submit a READ operation using a fixed buffer (by index).
 *
 * Reads data into a previously registered fixed buffer. This is more
 * efficient than loopyIoUringRead() as it avoids buffer mapping overhead.
 *
 * @param l         Event loop
 * @param fd        File descriptor to read from
 * @param bufIndex  Index of registered buffer (0 to count-1)
 * @param len       Number of bytes to read (must be <= buffer size)
 * @param offset    File offset to read from (-1 for current position)
 * @param cb        Completion callback
 * @param userData  User data for callback
 * @return Operation ID on success, 0 on failure or if buffers not registered
 *
 * Example:
 *   loopyIoUringReadFixed(l, fd, 0, 4096, 0, readCallback, NULL);
 */
uint64_t loopyIoUringReadFixed(loopyLoop *l, int fd, uint32_t bufIndex,
                               size_t len, off_t offset,
                               loopyIoUringFileCallback *cb, void *userData);

/**
 * Submit a WRITE operation using a fixed buffer (by index).
 *
 * Writes data from a previously registered fixed buffer. This is more
 * efficient than loopyIoUringWrite() as it avoids buffer mapping overhead.
 *
 * @param l         Event loop
 * @param fd        File descriptor to write to
 * @param bufIndex  Index of registered buffer (0 to count-1)
 * @param len       Number of bytes to write (must be <= buffer size)
 * @param offset    File offset to write to (-1 for current position)
 * @param cb        Completion callback
 * @param userData  User data for callback
 * @return Operation ID on success, 0 on failure or if buffers not registered
 *
 * Example:
 *   loopyIoUringWriteFixed(l, fd, 1, 8192, -1, writeCallback, NULL);
 */
uint64_t loopyIoUringWriteFixed(loopyLoop *l, int fd, uint32_t bufIndex,
                                size_t len, off_t offset,
                                loopyIoUringFileCallback *cb, void *userData);

/* ====================================================================
 * Fixed File Operations (Zero-Overhead File Access)
 * ==================================================================== */

/**
 * Register file descriptors with io_uring for zero-overhead operations.
 *
 * Fixed files are registered once and then referenced by index in subsequent
 * I/O operations, eliminating file descriptor lookup overhead.
 *
 * Benefits:
 *  - Zero file descriptor lookup overhead on every I/O operation
 *  - Improved performance for repeated I/O on same files
 *  - Kernel can optimize file access paths
 *  - Reduces overhead of file table locking
 *
 * Requirements:
 *  - File descriptors must remain open while registered
 *  - Maximum of 1024 files can be registered (kernel limit)
 *  - Files are referenced by index (0 to count-1) in I/O operations
 *
 * @param l      Event loop
 * @param files  Array of file descriptors to register
 * @param count  Number of file descriptors in array (max 1024)
 * @return true on success, false on failure or if io_uring not available
 *
 * Example:
 *   int fds[2];
 *   fds[0] = open("file1.txt", O_RDONLY);
 *   fds[1] = open("file2.txt", O_WRONLY);
 *
 *   if (loopyIoUringRegisterFiles(l, fds, 2)) {
 *       // Use index 0 and 1 with loopyIoUringReadFixedFile/WriteFixedFile
 *   }
 */
bool loopyIoUringRegisterFiles(loopyLoop *l, int *files, uint32_t count);

/**
 * Unregister previously registered file descriptors.
 *
 * After unregistering, the file descriptors can be closed. Any pending
 * operations using fixed files will complete with the old registrations.
 *
 * @param l Event loop
 * @return true on success, false on failure or if no files registered
 */
bool loopyIoUringUnregisterFiles(loopyLoop *l);

/**
 * Check if fixed files are currently registered.
 *
 * @param l Event loop
 * @return true if files are registered, false otherwise
 */
bool loopyIoUringHasFixedFiles(loopyLoop *l);

/**
 * Submit a READ operation using a registered fixed file (by index).
 *
 * Reads data from a previously registered file descriptor. This is more
 * efficient than loopyIoUringRead() as it avoids file descriptor lookup.
 *
 * @param l         Event loop
 * @param fileIndex Index of registered file (0 to count-1)
 * @param buf       Buffer to read into (must remain valid until completion)
 * @param len       Number of bytes to read
 * @param offset    File offset to read from (-1 for current position)
 * @param cb        Completion callback
 * @param userData  User data for callback
 * @return Operation ID on success, 0 on failure or if files not registered
 *
 * Example:
 *   char buffer[4096];
 *   loopyIoUringReadFixedFile(l, 0, buffer, 4096, 0, readCallback, NULL);
 */
uint64_t loopyIoUringReadFixedFile(loopyLoop *l, uint32_t fileIndex, void *buf,
                                   size_t len, off_t offset,
                                   loopyIoUringFileCallback *cb,
                                   void *userData);

/**
 * Submit a WRITE operation using a registered fixed file (by index).
 *
 * Writes data to a previously registered file descriptor. This is more
 * efficient than loopyIoUringWrite() as it avoids file descriptor lookup.
 *
 * @param l         Event loop
 * @param fileIndex Index of registered file (0 to count-1)
 * @param buf       Buffer to write from (must remain valid until completion)
 * @param len       Number of bytes to write
 * @param offset    File offset to write to (-1 for current position)
 * @param cb        Completion callback
 * @param userData  User data for callback
 * @return Operation ID on success, 0 on failure or if files not registered
 *
 * Example:
 *   const char *data = "Hello, fixed files!";
 *   loopyIoUringWriteFixedFile(l, 1, data, strlen(data), 0, writeCallback,
 * NULL);
 */
uint64_t loopyIoUringWriteFixedFile(loopyLoop *l, uint32_t fileIndex,
                                    const void *buf, size_t len, off_t offset,
                                    loopyIoUringFileCallback *cb,
                                    void *userData);

/* ====================================================================
 * Linked Operations (Operation Chaining)
 * ==================================================================== */

/**
 * Opaque handle for a chain of linked io_uring operations.
 *
 * Linked operations execute sequentially, with automatic cancellation of
 * subsequent operations if any operation in the chain fails. This is useful
 * for dependent file operations that must execute in order.
 */
typedef struct loopyIoUringLinkChain loopyIoUringLinkChain;

/**
 * Link mode for operation chains.
 */
typedef enum {
    /**
     * Soft link - If an operation fails, subsequent linked operations are
     * canceled and receive -ECANCELED in their completion callbacks.
     */
    LOOPY_IOURING_LINK_SOFT = 0,

    /**
     * Hard link - Operations continue executing even if a previous operation
     * fails. Use this when you want all operations to attempt execution
     * regardless of earlier failures.
     */
    LOOPY_IOURING_LINK_HARD = 1
} loopyIoUringLinkMode;

/**
 * Create a new linked operation chain.
 *
 * A link chain allows you to submit multiple io_uring operations that
 * execute in sequence. If any operation in a soft-linked chain fails,
 * subsequent operations are automatically canceled by the kernel.
 *
 * Benefits:
 *  - Atomic submission of multiple dependent operations
 *  - Automatic failure propagation and cleanup
 *  - Reduced syscall overhead (one submission for multiple operations)
 *  - Guaranteed execution order
 *
 * Limitations:
 *  - Chain must fit within available submission queue entries
 *  - All operations execute sequentially (no parallelism within chain)
 *  - Maximum chain length limited by SQ size
 *
 * @param l    Event loop
 * @param mode Link mode (SOFT for fail-fast, HARD for continue-on-error)
 * @return Chain handle on success, NULL on failure
 *
 * Example:
 *   loopyIoUringLinkChain *chain = loopyIoUringLinkChainNew(l,
 *       LOOPY_IOURING_LINK_SOFT);
 */
loopyIoUringLinkChain *loopyIoUringLinkChainNew(loopyLoop *l,
                                                loopyIoUringLinkMode mode);

/**
 * Add a READ operation to the link chain.
 *
 * The read will execute after all previously added operations complete
 * successfully (in soft link mode).
 *
 * @param chain    Link chain
 * @param fd       File descriptor to read from
 * @param buf      Buffer to read into (must remain valid until completion)
 * @param len      Number of bytes to read
 * @param offset   File offset to read from (-1 for current position)
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return Chain handle for chaining (same as input), or NULL on error
 *
 * Example:
 *   loopyIoUringLinkChainRead(chain, fd, buf, 4096, 0, cb, NULL);
 */
loopyIoUringLinkChain *loopyIoUringLinkChainRead(loopyIoUringLinkChain *chain,
                                                 int fd, void *buf, size_t len,
                                                 off_t offset,
                                                 loopyIoUringFileCallback *cb,
                                                 void *userData);

/**
 * Add a WRITE operation to the link chain.
 *
 * The write will execute after all previously added operations complete
 * successfully (in soft link mode).
 *
 * @param chain    Link chain
 * @param fd       File descriptor to write to
 * @param buf      Buffer to write from (must remain valid until completion)
 * @param len      Number of bytes to write
 * @param offset   File offset to write to (-1 for current position)
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return Chain handle for chaining (same as input), or NULL on error
 *
 * Example:
 *   loopyIoUringLinkChainWrite(chain, fd, data, len, 0, cb, NULL);
 */
loopyIoUringLinkChain *loopyIoUringLinkChainWrite(loopyIoUringLinkChain *chain,
                                                  int fd, const void *buf,
                                                  size_t len, off_t offset,
                                                  loopyIoUringFileCallback *cb,
                                                  void *userData);

/**
 * Add an OPENAT operation to the link chain.
 *
 * The open will execute after all previously added operations complete
 * successfully (in soft link mode).
 *
 * @param chain    Link chain
 * @param dirfd    Directory fd (AT_FDCWD for current working directory)
 * @param path     Path to open (must remain valid until completion)
 * @param flags    Open flags (O_RDONLY, O_WRONLY, O_CREAT, etc.)
 * @param mode     File mode for O_CREAT (e.g., 0644)
 * @param cb       Completion callback (receives fd or -errno)
 * @param userData User data for callback
 * @return Chain handle for chaining (same as input), or NULL on error
 *
 * Example:
 *   loopyIoUringLinkChainOpenat(chain, AT_FDCWD, "/tmp/test.txt",
 *                               O_RDONLY, 0, cb, NULL);
 */
loopyIoUringLinkChain *loopyIoUringLinkChainOpenat(loopyIoUringLinkChain *chain,
                                                   int dirfd, const char *path,
                                                   int flags, mode_t mode,
                                                   loopyIoUringFileCallback *cb,
                                                   void *userData);

/**
 * Add a CLOSE operation to the link chain.
 *
 * The close will execute after all previously added operations complete
 * successfully (in soft link mode).
 *
 * @param chain    Link chain
 * @param fd       File descriptor to close
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return Chain handle for chaining (same as input), or NULL on error
 *
 * Example:
 *   loopyIoUringLinkChainClose(chain, fd, cb, NULL);
 */
loopyIoUringLinkChain *loopyIoUringLinkChainClose(loopyIoUringLinkChain *chain,
                                                  int fd,
                                                  loopyIoUringFileCallback *cb,
                                                  void *userData);

/**
 * Add an FSYNC operation to the link chain.
 *
 * The fsync will execute after all previously added operations complete
 * successfully (in soft link mode).
 *
 * @param chain    Link chain
 * @param fd       File descriptor to sync
 * @param datasync true for fdatasync (data only), false for fsync (data +
 * metadata)
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return Chain handle for chaining (same as input), or NULL on error
 *
 * Example:
 *   loopyIoUringLinkChainFsync(chain, fd, false, cb, NULL);
 */
loopyIoUringLinkChain *loopyIoUringLinkChainFsync(loopyIoUringLinkChain *chain,
                                                  int fd, bool datasync,
                                                  loopyIoUringFileCallback *cb,
                                                  void *userData);

/**
 * Submit all operations in the link chain atomically.
 *
 * All operations in the chain are submitted to the kernel in a single
 * syscall. They will execute in the order they were added to the chain.
 *
 * In soft link mode (LOOPY_IOURING_LINK_SOFT):
 *  - If any operation fails, subsequent operations are canceled
 *  - Canceled operations receive -ECANCELED in their callbacks
 *
 * In hard link mode (LOOPY_IOURING_LINK_HARD):
 *  - All operations execute regardless of failures
 *  - Each operation's callback receives its individual result
 *
 * After submission, the chain is automatically freed and must not be used.
 *
 * @param chain Link chain to submit (must not be NULL)
 * @return true on successful submission, false on error
 *
 * Example:
 *   if (!loopyIoUringLinkChainSubmit(chain)) {
 *       // Submission failed
 *   }
 *   // chain is now invalid, callbacks will be invoked as operations complete
 */
bool loopyIoUringLinkChainSubmit(loopyIoUringLinkChain *chain);

/**
 * Discard a link chain without submitting.
 *
 * Frees all resources associated with the chain. Any operations added to
 * the chain will NOT be executed.
 *
 * Note: loopyIoUringLinkChainSubmit() automatically frees the chain, so
 * you should only call this function if you decide not to submit the chain.
 *
 * @param chain Link chain to discard (can be NULL)
 */
void loopyIoUringLinkChainFree(loopyIoUringLinkChain *chain);

/**
 * Get the number of operations currently in the link chain.
 *
 * @param chain Link chain (must not be NULL)
 * @return Number of operations added to the chain
 */
uint32_t loopyIoUringLinkChainLength(const loopyIoUringLinkChain *chain);

#endif /* __linux__ */

#endif /* LOOPY_IOURING_FS_H */
