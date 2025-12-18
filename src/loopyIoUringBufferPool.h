/*
 * loopy - High-Performance Event Loop Library
 * Copyright (c) 2024, Matt Stancliff <matt@genges.com>
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#ifdef __linux__

#include "loopy.h"
#include "loopyPlatform.h"

#include <stdbool.h>
#include <stdint.h>
#include <sys/uio.h>

/* Forward declarations */
typedef void loopyIoUringFileCallback(void *userData, int32_t result);

/* ====================================================================
 * Buffer Pool Management
 * ==================================================================== */

/**
 * Opaque buffer pool handle.
 *
 * Manages a kernel-side pool of buffers for automatic buffer selection.
 * The kernel provides buffers from the pool for operations using
 * IOSQE_BUFFER_SELECT flag, eliminating manual buffer management.
 *
 * Benefits over fixed buffers:
 * - Kernel automatically selects available buffer
 * - No need to track which buffer is in use
 * - Better for high-concurrency recv-heavy workloads
 * - Automatic buffer recycling
 *
 * Introduced in Linux 5.7 (IORING_OP_PROVIDE_BUFFERS)
 */
typedef struct loopyIoUringBufferPool loopyIoUringBufferPool;

/**
 * Callback for operations using buffer pool.
 *
 * When an operation uses automatic buffer selection, the callback
 * receives information about which buffer was selected by the kernel.
 *
 * @param userData   User data passed to the operation
 * @param result     Operation result (bytes transferred or -errno)
 * @param bufferId   Which buffer was used (0 to bufferCount-1)
 * @param bufferAddr Pointer to the buffer data
 * @param bufferSize Total buffer capacity
 *
 * The application MUST process or copy the buffer data before returning,
 * as the buffer will be returned to the pool for reuse.
 *
 * Example:
 * @code
 * void recv_callback(void *userData, int32_t result, uint16_t bufferId,
 *                    void *bufferAddr, size_t bufferSize) {
 *     if (result < 0) {
 *         fprintf(stderr, "recv error: %s\n", strerror(-result));
 *         return;
 *     }
 *
 *     // Process the data immediately
 *     printf("Received %d bytes in buffer %u: %.*s\n",
 *            result, bufferId, result, (char *)bufferAddr);
 *
 *     // Buffer automatically returned to pool after callback returns
 * }
 * @endcode
 */
typedef void loopyIoUringBufferCallback(void *userData, int32_t result,
                                        uint16_t bufferId, void *bufferAddr,
                                        size_t bufferSize);

/**
 * Create a kernel-managed buffer pool for automatic buffer selection.
 *
 * Registers a pool of fixed-size buffers with the kernel. Operations can
 * then use IOSQE_BUFFER_SELECT to automatically select an available buffer
 * from the pool, eliminating manual buffer management.
 *
 * The kernel tracks which buffers are in-use and automatically selects
 * available buffers for incoming operations. When the operation completes,
 * the buffer is automatically returned to the pool.
 *
 * @param l            Event loop
 * @param bufferSize   Size of each buffer in bytes (must be power of 2)
 * @param bufferCount  Number of buffers to allocate (1-65535)
 * @param groupId      Buffer group ID (0-65535) - operations specify which
 *                     group to use
 * @return Buffer pool handle, or NULL on failure
 *
 * Requirements:
 * - Linux kernel 5.7+ (IORING_OP_PROVIDE_BUFFERS support)
 * - bufferSize should be power of 2 for optimal kernel allocation
 * - bufferCount limited by available memory
 * - groupId allows multiple independent pools
 *
 * Example:
 * @code
 * // Create pool of 1024 4KB buffers in group 0
 * loopyIoUringBufferPool *pool =
 *     loopyIoUringBufferPoolNew(loop, 4096, 1024, 0);
 *
 * if (!pool) {
 *     fprintf(stderr, "Buffer pool not supported on this kernel\n");
 *     // Fall back to manual buffer management
 * }
 *
 * // Use pooled recv - kernel automatically selects buffer
 * loopyIoUringRecvPooled(loop, sockfd, 0, recv_callback, ctx);
 *
 * // Cleanup
 * loopyIoUringBufferPoolFree(pool);
 * @endcode
 */
loopyIoUringBufferPool *loopyIoUringBufferPoolNew(loopyLoop *l,
                                                  uint32_t bufferSize,
                                                  uint32_t bufferCount,
                                                  uint16_t groupId);

/**
 * Free a buffer pool and remove buffers from kernel.
 *
 * Unregisters all buffers from the specified group and frees
 * the buffer pool memory. Any operations in-flight using buffers
 * from this pool will complete normally, but new operations
 * cannot use this group.
 *
 * @param pool Buffer pool to free (NULL safe - no-op)
 *
 * Note: Always call this before destroying the event loop to avoid
 * kernel resource leaks.
 */
void loopyIoUringBufferPoolFree(loopyIoUringBufferPool *pool);

/**
 * Get statistics about buffer pool usage.
 *
 * @param pool         Buffer pool
 * @param totalBuffers OUT: Total number of buffers in pool
 * @param inUse        OUT: Number of buffers currently in use
 * @return true if stats retrieved, false if pool is NULL
 *
 * Useful for monitoring buffer exhaustion and sizing pools appropriately.
 */
bool loopyIoUringBufferPoolStats(const loopyIoUringBufferPool *pool,
                                 uint32_t *totalBuffers, uint32_t *inUse);

/* ====================================================================
 * Operations Using Buffer Pools
 * ==================================================================== */

/**
 * Receive data using automatic buffer selection from pool.
 *
 * Receives data into a kernel-selected buffer from the specified group.
 * The kernel automatically chooses an available buffer, eliminating
 * the need for manual buffer management.
 *
 * @param l        Event loop
 * @param sockfd   Socket file descriptor
 * @param groupId  Buffer group to use (must have buffers provided)
 * @param flags    Receive flags (MSG_WAITALL, etc.)
 * @param cb       Completion callback (receives buffer info)
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure
 *
 * The callback receives:
 * - result: Bytes received or -errno
 * - bufferId: Which buffer was used (for tracking)
 * - bufferAddr: Pointer to received data
 * - bufferSize: Total buffer capacity
 *
 * Requirements:
 * - Buffer pool must be created for groupId
 * - Available buffer in pool (operation fails if pool exhausted)
 * - Linux kernel 5.7+
 *
 * Example:
 * @code
 * void on_recv(void *ctx, int32_t result, uint16_t bufId,
 *              void *buf, size_t bufSize) {
 *     if (result < 0) {
 *         fprintf(stderr, "recv failed: %s\n", strerror(-result));
 *         return;
 *     }
 *
 *     printf("Received %d bytes: %.*s\n", result, result, (char *)buf);
 *     // Buffer automatically returned to pool after this returns
 * }
 *
 * // Submit recv with automatic buffer selection
 * uint64_t opId = loopyIoUringRecvPooled(loop, sockfd, 0, on_recv, NULL);
 * @endcode
 */
uint64_t loopyIoUringRecvPooled(loopyLoop *l, int sockfd, uint16_t groupId,
                                int flags, loopyIoUringBufferCallback *cb,
                                void *userData);

/**
 * Read data using automatic buffer selection from pool.
 *
 * Similar to loopyIoUringRecvPooled but for file I/O instead of sockets.
 * The kernel selects an available buffer from the pool for the read operation.
 *
 * @param l        Event loop
 * @param fd       File descriptor
 * @param groupId  Buffer group to use
 * @param offset   File offset to read from
 * @param cb       Completion callback
 * @param userData User data
 * @return Operation ID on success, 0 on failure
 *
 * Useful for file serving applications where you want the kernel to
 * manage buffer allocation automatically.
 */
uint64_t loopyIoUringReadPooled(loopyLoop *l, int fd, uint16_t groupId,
                                off_t offset, loopyIoUringBufferCallback *cb,
                                void *userData);

/* ====================================================================
 * Feature Detection
 * ==================================================================== */

/**
 * Check if buffer pools are supported on this system.
 *
 * @param l Event loop
 * @return true if PROVIDE_BUFFERS/REMOVE_BUFFERS are available
 *
 * Returns false if:
 * - Not using io_uring backend
 * - Kernel doesn't support PROVIDE_BUFFERS (pre-5.7)
 * - io_uring initialization failed
 */
bool loopyIoUringBufferPoolsAvailable(loopyLoop *l);

/* ====================================================================
 * Advanced Buffer Management
 * ==================================================================== */

/**
 * Provide buffers dynamically to io_uring (advanced API).
 *
 * For most use cases, use loopyIoUringBufferPoolNew instead.
 */
uint64_t loopyIoUringProvideBuffers(loopyLoop *l, void *buffers, size_t bufSize,
                                    uint32_t count, uint16_t groupId,
                                    uint16_t bufId,
                                    loopyIoUringFileCallback *cb,
                                    void *userData);

/**
 * Remove buffers from io_uring pool (advanced API).
 */
uint64_t loopyIoUringRemoveBuffers(loopyLoop *l, uint32_t count,
                                   uint16_t groupId,
                                   loopyIoUringFileCallback *cb,
                                   void *userData);

#endif /* __linux__ */
