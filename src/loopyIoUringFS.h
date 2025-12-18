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

#include "loopy.h"
#include "loopyPlatform.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/uio.h>

#ifdef __linux__
#include <fcntl.h>      /* For AT_FDCWD */
#include <linux/stat.h> /* For struct statx */
#include <sys/epoll.h>  /* For struct epoll_event */
#endif                  /* For struct iovec */

#ifdef __linux__

/* Forward declare structs used in API */
struct futex_waitv;

/* Forward declare buffer callback type from loopyIoUringBufferPool.h */
typedef void loopyIoUringBufferCallback(void *userData, int32_t result,
                                        uint16_t bufferId, void *bufferAddr,
                                        size_t bufferSize);

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
 * Submit a READ_MULTISHOT operation via io_uring (if available).
 *
 * Reads data from a file descriptor using automatic buffer selection from
 * a buffer pool. The operation is automatically re-armed after each read
 * completion. The callback is invoked multiple times for successive reads
 * until EOF or error is encountered.
 *
 * Benefits:
 *  - Callback fires automatically on each read completion
 *  - No need to re-submit operation after each read
 *  - Automatic buffer selection from registered pool
 *  - Reduced syscall overhead compared to repeated READ operations
 *  - Kernel automatically continues reading until EOF
 *
 * The operation completes (stops generating callbacks) when:
 *  - EOF is reached (0 bytes read)
 *  - An error occurs (-errno returned to callback)
 *  - Manual cancellation via loopyIoUringCancel()
 *
 * @param l            Event loop
 * @param fd           File descriptor to read from
 * @param len          Maximum bytes to read per iteration (0 = use buffer size)
 * @param offset       File offset to read from (-1 for current position)
 * @param buffer_group Buffer pool group ID for automatic buffer selection
 * @param cb           Completion callback (loopyIoUringBufferCallback type)
 * @param userData     User data for callback
 * @return Operation ID on success, 0 on failure or if io_uring not available
 *
 * Requirements:
 *  - Linux kernel 6.0+ for IORING_OP_READ_MULTISHOT
 *  - Buffer pool must be registered with buffer_group ID
 *  - Callback receives bufferId, bufferAddr, and bufferSize on each read
 *
 * Example:
 *   void onMultishotRead(void *userData, int32_t result, uint16_t bufferId,
 *                        void *bufferAddr, size_t bufferSize) {
 *       if (result > 0) {
 *           // Process result bytes from bufferAddr
 *       } else if (result == 0) {
 *           // EOF reached, operation complete
 *       } else {
 *           // Error: -errno
 *       }
 *   }
 *   loopyIoUringBufferPool *pool = loopyIoUringBufferPoolNew(l, 4096, 16, 10);
 *   loopyIoUringReadMultishot(l, fd, 0, -1, 10, onMultishotRead, NULL);
 */
uint64_t loopyIoUringReadMultishot(loopyLoop *l, int fd, size_t len,
                                   off_t offset, uint16_t buffer_group,
                                   loopyIoUringBufferCallback *cb,
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
 * Close a file in the fixed file table via io_uring.
 *
 * Closes a file that was opened with ACCEPT_DIRECT, SOCKET_DIRECT, or
 * OPENAT_DIRECT. The file must be in the fixed file table at the specified
 * index.
 *
 * @param l         Event loop
 * @param fileIndex Index of file in fixed file table (0-based)
 * @param cb        Completion callback (receives 0 on success, -errno on error)
 * @param userData  User data for callback
 * @return Operation ID on success, 0 on failure
 */
uint64_t loopyIoUringCloseDirect(loopyLoop *l, uint32_t fileIndex,
                                 loopyIoUringFileCallback *cb, void *userData);

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
 * Install FD into fixed file table dynamically.
 *
 * @param l        Event loop
 * @param fd       File descriptor to install
 * @param flags    Installation flags
 * @param cb       Completion callback (result = allocated index)
 * @param userData User data
 * @return Operation ID on success, 0 on failure
 */
uint64_t loopyIoUringFixedFdInstall(loopyLoop *l, int fd, uint32_t flags,
                                    loopyIoUringFileCallback *cb,
                                    void *userData);

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
 * Add a LINK_TIMEOUT operation to the link chain.
 *
 * The timeout applies to the next operation in the chain. If the next operation
 * does not complete within the specified time, it will be canceled and its
 * callback will receive -ETIME.
 *
 * This is useful for implementing timeouts on blocking operations like reads
 * from network sockets.
 *
 * @param chain    Link chain
 * @param timeoutUs Timeout in microseconds
 * @param cb       Completion callback (receives 0 on successful arm, -errno on
 *                 error; only called on actual timeout via next operation's
 *                 callback)
 * @param userData User data for callback
 * @return Chain handle for chaining (same as input), or NULL on error
 *
 * Example:
 *   loopyIoUringLinkChainTimeout(chain, 5000000, cb, NULL);  // 5 second
 * timeout loopyIoUringLinkChainRead(chain, fd, buf, 4096, 0, readCb, NULL);
 */
loopyIoUringLinkChain *
loopyIoUringLinkChainTimeout(loopyIoUringLinkChain *chain, uint64_t timeoutUs,
                             loopyIoUringFileCallback *cb, void *userData);

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

/* ====================================================================
 * File System Metadata Operations
 * ==================================================================== */

/**
 * Get extended file metadata (STATX operation).
 *
 * @param l        Event loop
 * @param dirfd    Directory fd (AT_FDCWD for current dir)
 * @param pathname Path to file
 * @param flags    AT_* flags
 * @param mask     STATX_* fields to retrieve
 * @param statxbuf Buffer for statx result
 * @param cb       Completion callback (result=0 success, -errno error)
 * @param userData User data
 * @return Operation ID on success, 0 on failure
 */
uint64_t loopyIoUringStatx(loopyLoop *l, int dirfd, const char *pathname,
                           int flags, uint32_t mask, struct statx *statxbuf,
                           loopyIoUringFileCallback *cb, void *userData);

uint64_t loopyIoUringRenameat(loopyLoop *l, int olddirfd, const char *oldpath,
                              int newdirfd, const char *newpath,
                              loopyIoUringFileCallback *cb, void *userData);

uint64_t loopyIoUringUnlinkat(loopyLoop *l, int dirfd, const char *pathname,
                              int flags, loopyIoUringFileCallback *cb,
                              void *userData);

uint64_t loopyIoUringMkdirat(loopyLoop *l, int dirfd, const char *pathname,
                             mode_t mode, loopyIoUringFileCallback *cb,
                             void *userData);

uint64_t loopyIoUringSymlinkat(loopyLoop *l, const char *target, int newdirfd,
                               const char *linkpath,
                               loopyIoUringFileCallback *cb, void *userData);

uint64_t loopyIoUringLinkat(loopyLoop *l, int olddirfd, const char *oldpath,
                            int newdirfd, const char *newpath, int flags,
                            loopyIoUringFileCallback *cb, void *userData);

/* ====================================================================
 * File Maintenance Operations
 * ==================================================================== */

/**
 * Pre-allocate file space (FALLOCATE operation).
 *
 * @param l        Event loop
 * @param fd       File descriptor
 * @param mode     Fallocate mode flags (e.g., FALLOC_FL_KEEP_SIZE)
 * @param offset   File offset for allocation
 * @param len      Number of bytes to allocate
 * @param cb       Completion callback
 * @param userData User data
 * @return Operation ID on success, 0 on failure
 */
uint64_t loopyIoUringFallocate(loopyLoop *l, int fd, int mode, off_t offset,
                               off_t len, loopyIoUringFileCallback *cb,
                               void *userData);

/**
 * Provide file access pattern hints (FADVISE operation).
 *
 * @param l        Event loop
 * @param fd       File descriptor
 * @param offset   File offset
 * @param len      Length of region
 * @param advice   Access pattern advice (POSIX_FADV_*)
 * @param cb       Completion callback
 * @param userData User data
 * @return Operation ID on success, 0 on failure
 */
uint64_t loopyIoUringFadvise(loopyLoop *l, int fd, off_t offset, off_t len,
                             int advice, loopyIoUringFileCallback *cb,
                             void *userData);

/**
 * Sync file data range (SYNC_FILE_RANGE operation).
 *
 * @param l        Event loop
 * @param fd       File descriptor
 * @param offset   File offset
 * @param len      Number of bytes
 * @param flags    Sync flags
 * @param cb       Completion callback
 * @param userData User data
 * @return Operation ID on success, 0 on failure
 */
uint64_t loopyIoUringSyncFileRange(loopyLoop *l, int fd, off_t offset,
                                   off_t len, uint32_t flags,
                                   loopyIoUringFileCallback *cb,
                                   void *userData);

/**
 * Truncate file to specified length (FTRUNCATE operation).
 *
 * @param l        Event loop
 * @param fd       File descriptor
 * @param length   New file length
 * @param cb       Completion callback
 * @param userData User data
 * @return Operation ID on success, 0 on failure
 */
uint64_t loopyIoUringFtruncate(loopyLoop *l, int fd, off_t length,
                               loopyIoUringFileCallback *cb, void *userData);

/* ====================================================================
 * Data Movement Operations
 * ==================================================================== */

/**
 * Zero-copy data transfer between file descriptors (SPLICE operation).
 *
 * @param l        Event loop
 * @param fd_in    Source file descriptor (one of fd_in/fd_out must be a pipe)
 * @param off_in   Source offset (-1 for current position)
 * @param fd_out   Destination file descriptor
 * @param off_out  Destination offset (-1 for current position)
 * @param len      Number of bytes to transfer
 * @param flags    Splice flags (SPLICE_F_*)
 * @param cb       Completion callback
 * @param userData User data
 * @return Operation ID on success, 0 on failure
 */
uint64_t loopyIoUringSplice(loopyLoop *l, int fd_in, off_t off_in, int fd_out,
                            off_t off_out, size_t len, uint32_t flags,
                            loopyIoUringFileCallback *cb, void *userData);

/**
 * Duplicate pipe data without consuming it (TEE operation).
 *
 * @param l        Event loop
 * @param fd_in    Source pipe
 * @param fd_out   Destination pipe
 * @param len      Number of bytes to duplicate
 * @param flags    Splice flags
 * @param cb       Completion callback
 * @param userData User data
 * @return Operation ID on success, 0 on failure
 */
uint64_t loopyIoUringTee(loopyLoop *l, int fd_in, int fd_out, size_t len,
                         uint32_t flags, loopyIoUringFileCallback *cb,
                         void *userData);

/**
 * Scatter-gather read (READV operation).
 *
 * @param l        Event loop
 * @param fd       File descriptor
 * @param iov      Array of iovec buffers
 * @param iovcnt   Number of buffers
 * @param offset   File offset (-1 for current position)
 * @param cb       Completion callback
 * @param userData User data
 * @return Operation ID on success, 0 on failure
 */
uint64_t loopyIoUringReadv(loopyLoop *l, int fd, const struct iovec *iov,
                           int iovcnt, off_t offset,
                           loopyIoUringFileCallback *cb, void *userData);

/**
 * Scatter-gather write (WRITEV operation).
 *
 * @param l        Event loop
 * @param fd       File descriptor
 * @param iov      Array of iovec buffers
 * @param iovcnt   Number of buffers
 * @param offset   File offset (-1 for current position)
 * @param cb       Completion callback
 * @param userData User data
 * @return Operation ID on success, 0 on failure
 */
uint64_t loopyIoUringWritev(loopyLoop *l, int fd, const struct iovec *iov,
                            int iovcnt, off_t offset,
                            loopyIoUringFileCallback *cb, void *userData);

/**
 * Scatter-gather read with registered fixed buffers (READV_FIXED operation).
 *
 * Performs scatter-gather read using pre-registered buffers for zero-copy I/O.
 * Requires buffers to be registered first with loopyIoUringRegisterBuffers().
 *
 * @param l            Event loop
 * @param fd           File descriptor
 * @param iov          Array of iovec buffers (pointing to registered buffers)
 * @param iovcnt       Number of buffers
 * @param offset       File offset (-1 for current position)
 * @param buf_indices  Array of registered buffer indices (one per iovec)
 * @param cb           Completion callback
 * @param userData     User data
 * @return Operation ID on success, 0 on failure
 *
 * Requirements:
 *  - Linux kernel 5.10+
 *  - Buffers must be pre-registered
 *  - buf_indices array must have iovcnt elements
 *
 * Example:
 *   struct iovec iov[2];
 *   uint32_t indices[2] = {0, 1};
 *   // iov[0/1] point to registered buffers 0/1
 *   loopyIoUringReadvFixed(l, fd, iov, 2, 0, indices, cb, NULL);
 */
uint64_t loopyIoUringReadvFixed(loopyLoop *l, int fd, const struct iovec *iov,
                                int iovcnt, off_t offset, uint32_t *buf_indices,
                                loopyIoUringFileCallback *cb, void *userData);

/**
 * Scatter-gather write with registered fixed buffers (WRITEV_FIXED operation).
 *
 * Performs scatter-gather write using pre-registered buffers for zero-copy I/O.
 * Requires buffers to be registered first with loopyIoUringRegisterBuffers().
 *
 * @param l            Event loop
 * @param fd           File descriptor
 * @param iov          Array of iovec buffers (pointing to registered buffers)
 * @param iovcnt       Number of buffers
 * @param offset       File offset (-1 for current position)
 * @param buf_indices  Array of registered buffer indices (one per iovec)
 * @param cb           Completion callback
 * @param userData     User data
 * @return Operation ID on success, 0 on failure
 *
 * Requirements:
 *  - Linux kernel 5.10+
 *  - Buffers must be pre-registered
 *  - buf_indices array must have iovcnt elements
 *
 * Example:
 *   struct iovec iov[2];
 *   uint32_t indices[2] = {0, 1};
 *   // iov[0/1] point to registered buffers 0/1
 *   loopyIoUringWritevFixed(l, fd, iov, 2, 0, indices, cb, NULL);
 */
uint64_t loopyIoUringWritevFixed(loopyLoop *l, int fd, const struct iovec *iov,
                                 int iovcnt, off_t offset,
                                 uint32_t *buf_indices,
                                 loopyIoUringFileCallback *cb, void *userData);

/* ====================================================================
 * Extended Attributes Operations
 * ==================================================================== */

/**
 * Set extended attribute on path (SETXATTR operation).
 */
uint64_t loopyIoUringSetxattr(loopyLoop *l, const char *path, const char *name,
                              const void *value, size_t size, int flags,
                              loopyIoUringFileCallback *cb, void *userData);

/**
 * Set extended attribute on file descriptor (FSETXATTR operation).
 */
uint64_t loopyIoUringFsetxattr(loopyLoop *l, int fd, const char *name,
                               const void *value, size_t size, int flags,
                               loopyIoUringFileCallback *cb, void *userData);

/**
 * Get extended attribute from path (GETXATTR operation).
 */
uint64_t loopyIoUringGetxattr(loopyLoop *l, const char *path, const char *name,
                              void *value, size_t size,
                              loopyIoUringFileCallback *cb, void *userData);

/**
 * Get extended attribute from file descriptor (FGETXATTR operation).
 */
uint64_t loopyIoUringFgetxattr(loopyLoop *l, int fd, const char *name,
                               void *value, size_t size,
                               loopyIoUringFileCallback *cb, void *userData);

/**
 * Provide memory access advice (MADVISE operation).
 *
 * @param l        Event loop
 * @param addr     Memory address
 * @param length   Region length
 * @param advice   Advice (MADV_*)
 * @param cb       Completion callback
 * @param userData User data
 * @return Operation ID on success, 0 on failure
 */
uint64_t loopyIoUringMadvise(loopyLoop *l, void *addr, size_t length,
                             int advice, loopyIoUringFileCallback *cb,
                             void *userData);

/* ====================================================================
 * Direct File Operations
 * ==================================================================== */

/**
 * Open file directly into the fixed file table (OPENAT_DIRECT operation).
 *
 * @param l          Event loop
 * @param dirfd      Directory fd (AT_FDCWD for cwd)
 * @param pathname   Path to open
 * @param flags      Open flags
 * @param mode       File mode for O_CREAT
 * @param file_index Target slot in fixed file table
 * @param cb         Completion callback
 * @param userData   User data
 * @return Operation ID on success, 0 on failure
 */
uint64_t loopyIoUringOpenatDirect(loopyLoop *l, int dirfd, const char *pathname,
                                  int flags, mode_t mode, uint32_t file_index,
                                  loopyIoUringFileCallback *cb, void *userData);

/* ====================================================================
 * Timeout Operations
 * ==================================================================== */

/**
 * Submit a timeout operation.
 *
 * @param l           Event loop
 * @param nanoseconds Timeout in nanoseconds
 * @param absolute    If true, timeout is absolute time; if false, relative
 * @param cb          Completion callback
 * @param userData    User data
 * @return Operation ID on success, 0 on failure
 */
uint64_t loopyIoUringTimeout(loopyLoop *l, uint64_t nanoseconds, bool absolute,
                             loopyIoUringFileCallback *cb, void *userData);

/**
 * Cancel a pending timeout operation.
 *
 * @param l           Event loop
 * @param timeoutOpId Operation ID of the timeout to cancel
 * @return true on success, false on failure
 */
bool loopyIoUringTimeoutRemove(loopyLoop *l, uint64_t timeoutOpId);

/**
 * Update an existing timeout operation with a new timeout value.
 *
 * This operation modifies the timeout of an existing timeout without needing
 * to cancel and re-submit. This is more efficient than TIMEOUT_REMOVE followed
 * by TIMEOUT for updating timeout durations.
 *
 * The operation is synchronous and waits for kernel acknowledgment before
 * returning. This ensures the timeout update is committed before the function
 * returns.
 *
 * @param l           Event loop
 * @param timeoutOpId Operation ID of the existing timeout to update
 * @param nanoseconds New timeout duration in nanoseconds
 * @param absolute    If true, nanoseconds is absolute time; if false, relative
 * @return Original timeoutOpId on success, 0 on failure
 *
 * Requirements:
 *  - timeoutOpId must be a valid, active timeout operation ID
 *  - The existing timeout must not have already completed
 *
 * Example:
 *   uint64_t timeoutId = loopyIoUringTimeout(l, 5000000000, false, cb, NULL);
 *   // Update timeout to 10 seconds
 *   uint64_t result = loopyIoUringTimeoutUpdate(l, timeoutId, 10000000000,
 * false); if (result) {
 *       // Timeout updated successfully, timeoutId is still valid
 *   }
 */
uint64_t loopyIoUringTimeoutUpdate(loopyLoop *l, uint64_t timeoutOpId,
                                   uint64_t nanoseconds, bool absolute);

/* ====================================================================
 * IPC Operations
 * ==================================================================== */

/**
 * Create a pipe (PIPE operation).
 * Note: May not be supported on all kernels (AWS kernels may disable).
 *
 * @param l        Event loop
 * @param pipefds  Array of 2 ints to receive [read_fd, write_fd]
 * @param flags    Pipe flags (O_NONBLOCK, O_CLOEXEC)
 * @param cb       Completion callback
 * @param userData User data
 * @return Operation ID on success, 0 on failure
 */
uint64_t loopyIoUringPipe(loopyLoop *l, int *pipefds, int flags,
                          loopyIoUringFileCallback *cb, void *userData);

/**
 * Send message to another io_uring ring (MSG_RING operation).
 *
 * @param l         Event loop
 * @param target_fd Target ring file descriptor
 * @param len       Length value to pass
 * @param data      Data value to pass
 * @param cb        Completion callback
 * @param userData  User data
 * @return Operation ID on success, 0 on failure
 */
uint64_t loopyIoUringMsgRing(loopyLoop *l, int target_fd, uint32_t len,
                             uint64_t data, loopyIoUringFileCallback *cb,
                             void *userData);

/**
 * Send FD to another io_uring (MSG_RING with FD passing).
 */
uint64_t loopyIoUringMsgRingFd(loopyLoop *l, int target_fd, int source_fd,
                               int target_fixed, uint64_t data,
                               loopyIoUringFileCallback *cb, void *userData);

/**
 * Send FD with kernel-allocated slot.
 */
uint64_t loopyIoUringMsgRingFdAlloc(loopyLoop *l, int target_fd, int source_fd,
                                    uint64_t data, loopyIoUringFileCallback *cb,
                                    void *userData);

/**
 * Send message with CQE flags.
 */
uint64_t loopyIoUringMsgRingCqeFlags(loopyLoop *l, int target_fd, uint32_t len,
                                     uint64_t data, uint32_t cqe_flags,
                                     loopyIoUringFileCallback *cb,
                                     void *userData);

/**
 * Wait for child process (WAITID operation).
 *
 * @param l        Event loop
 * @param which    ID type (P_PID, P_PGID, P_ALL)
 * @param upid     Process ID
 * @param cb       Completion callback
 * @param userData User data
 * @return Operation ID on success, 0 on failure
 */
uint64_t loopyIoUringWaitid(loopyLoop *l, int which, int upid,
                            loopyIoUringFileCallback *cb, void *userData);

/**
 * Wait on futex (FUTEX_WAIT operation).
 *
 * @param l           Event loop
 * @param futex       Futex address
 * @param val         Expected value
 * @param mask        Wait mask
 * @param futex_flags Futex flags
 * @param cb          Completion callback
 * @param userData    User data
 * @return Operation ID on success, 0 on failure
 */
uint64_t loopyIoUringFutexWait(loopyLoop *l, uint32_t *futex, uint64_t val,
                               uint64_t mask, uint32_t futex_flags,
                               loopyIoUringFileCallback *cb, void *userData);

/**
 * Wake futex waiters (FUTEX_WAKE operation).
 *
 * @param l           Event loop
 * @param futex       Futex address
 * @param val         Number to wake
 * @param mask        Wake mask
 * @param futex_flags Futex flags
 * @param cb          Completion callback
 * @param userData    User data
 * @return Operation ID on success, 0 on failure
 */
uint64_t loopyIoUringFutexWake(loopyLoop *l, uint32_t *futex, uint64_t val,
                               uint64_t mask, uint32_t futex_flags,
                               loopyIoUringFileCallback *cb, void *userData);

/**
 * Control epoll from io_uring (EPOLL_CTL operation).
 *
 * @param l        Event loop
 * @param epfd     Epoll file descriptor
 * @param fd       File descriptor to add/modify/delete
 * @param op       Operation (EPOLL_CTL_ADD, EPOLL_CTL_MOD, EPOLL_CTL_DEL)
 * @param ev       Epoll event structure
 * @param cb       Completion callback
 * @param userData User data
 * @return Operation ID on success, 0 on failure
 */
uint64_t loopyIoUringEpollCtl(loopyLoop *l, int epfd, int fd, int op,
                              struct epoll_event *ev,
                              loopyIoUringFileCallback *cb, void *userData);

/**
 * Wait for events on epoll file descriptor (EPOLL_WAIT operation).
 *
 * Waits for events on an epoll file descriptor and returns when events occur
 * or the timeout expires. The operation completes asynchronously and the
 * callback receives the number of events returned.
 *
 * This operation uses io_uring's native EPOLL_WAIT support, eliminating
 * the need for separate syscalls to poll file descriptors.
 *
 * Benefits:
 *  - Zero-syscall epoll_wait when io_uring is available
 *  - Can be combined with other io_uring operations in batch
 *  - Single operation ID to track multiple event completions
 *  - No need to manage separate event loop state
 *
 * @param l          Event loop
 * @param epfd       Epoll file descriptor
 * @param events     Array of epoll_event structures to receive events
 *                   (must remain valid until completion)
 * @param maxevents  Maximum number of events to return (must be > 0)
 * @param timeout_ms Timeout in milliseconds (-1 for infinite wait, 0 for poll)
 * @param cb         Completion callback (receives number of events, or -errno)
 * @param userData   User data for callback
 * @return Operation ID on success, 0 on failure or if io_uring not available
 *
 * Requirements:
 *  - Linux kernel 6.1+ for IORING_OP_EPOLL_WAIT
 *  - epfd must be a valid epoll file descriptor
 *  - events array must be large enough for maxevents
 *  - callback result will be number of events (0 or positive) or -errno
 *
 * Example:
 *   struct epoll_event events[64];
 *   void onEpollWait(void *userData, int32_t result) {
 *       if (result > 0) {
 *           // Process result events from events array
 *       } else if (result == 0) {
 *           // Timeout occurred, no events
 *       } else {
 *           // Error: -errno
 *       }
 *   }
 *   loopyIoUringEpollWait(l, epfd, events, 64, 5000, onEpollWait, NULL);
 */
uint64_t loopyIoUringEpollWait(loopyLoop *l, int epfd,
                               struct epoll_event *events, int maxevents,
                               int timeout_ms, loopyIoUringFileCallback *cb,
                               void *userData);

/**
 * Wait on multiple futexes atomically (FUTEX_WAITV operation).
 *
 * Waits on multiple futexes with a single atomic operation. The operation
 * completes when any futex in the array is woken or the timeout expires.
 * More efficient than waiting on futexes individually.
 *
 * This operation provides better performance when multiple futexes need to be
 * monitored together compared to submitting individual FUTEX_WAIT operations.
 *
 * Benefits:
 *  - Atomic wait on multiple futexes
 *  - Single operation to monitor multiple futex addresses
 *  - Better cache locality than individual FUTEX_WAIT operations
 *  - Reduced syscall overhead
 *
 * @param l         Event loop
 * @param futexes   Array of futex_waitv structures specifying futexes to wait
 * on (must remain valid until completion)
 * @param nfutexes  Number of futexes in array (must be > 0)
 * @param flags     Futex flags (e.g., FUTEX_WAITV_MAX for timeout clock)
 * @param cb        Completion callback (receives futex index woken, or -errno)
 * @param userData  User data for callback
 * @return Operation ID on success, 0 on failure or if io_uring not available
 *
 * Requirements:
 *  - Linux kernel 5.16+ for IORING_OP_FUTEX_WAITV
 *  - All futex addresses must be readable
 *  - futexes array must be properly initialized with futex addresses and values
 *  - callback result will be the index of woken futex, or -errno on error
 *
 * futex_waitv structure (from Linux headers):
 *   struct futex_waitv {
 *     __u64 val;       // Expected value
 *     __u64 uaddr;     // Futex address
 *     __u32 bitset;    // Wakeup mask (0 for all bits)
 *     __u32 __reserved; // Reserved for future use
 *   };
 *
 * Example:
 *   struct futex_waitv futexes[2];
 *   futexes[0].uaddr = (uint64_t)&futex1;
 *   futexes[0].val = expected_val1;
 *   futexes[0].bitset = 0;
 *   futexes[1].uaddr = (uint64_t)&futex2;
 *   futexes[1].val = expected_val2;
 *   futexes[1].bitset = 0;
 *
 *   void onFutexWaitv(void *userData, int32_t result) {
 *       if (result >= 0) {
 *           // Futex at index result was woken
 *       } else {
 *           // Error: -errno
 *       }
 *   }
 *   loopyIoUringFutexWaitv(l, futexes, 2, 0, onFutexWaitv, NULL);
 */
uint64_t loopyIoUringFutexWaitv(loopyLoop *l, const struct futex_waitv *futexes,
                                uint32_t nfutexes, uint32_t flags,
                                loopyIoUringFileCallback *cb, void *userData);

#endif /* __linux__ */

#endif /* LOOPY_IOURING_FS_H */
