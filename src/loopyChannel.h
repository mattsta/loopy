/* loopyChannel - Thread-safe inter-thread communication channels for loopy
 *
 * Provides high-performance communication channels between threads with
 * three modes: SPSC (lock-free), MPSC, and MPMC (mutex-based). Channels
 * support both synchronous and asynchronous operations with optional
 * blocking and timeouts.
 *
 * Key Features:
 * - Lock-free SPSC ring buffer for single producer/consumer scenarios
 * - Thread-safe MPSC/MPMC with efficient mutex implementation
 * - Bounded buffering with configurable backpressure behavior
 * - Event loop integration for async I/O patterns
 * - Select-style API for waiting on multiple channels
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
 * Opaque channel structure.
 * Created with loopyChannelNew(), freed with loopyChannelFree().
 */
typedef struct loopyChannel loopyChannel;

/**
 * Unique channel identifier for debugging and tracking.
 */
typedef uint64_t loopyChannelId;

/**
 * Channel types determining threading model and performance characteristics.
 *
 * SPSC: Single-producer/single-consumer. Lock-free implementation using
 *       atomic operations. Highest performance but restricted to exactly
 *       one sender and one receiver thread.
 *
 * MPSC: Multi-producer/single-consumer. Uses mutex for producer coordination.
 *       Common pattern for worker thread result aggregation.
 *
 * MPMC: Multi-producer/multi-consumer. Full mutex synchronization.
 *       Most flexible but with higher contention overhead.
 */
typedef enum loopyChannelType {
    LOOPY_CHANNEL_SPSC = 0, /* Lock-free single producer/consumer */
    LOOPY_CHANNEL_MPSC = 1, /* Multi-producer, single-consumer */
    LOOPY_CHANNEL_MPMC = 2, /* Multi-producer, multi-consumer */
} loopyChannelType;

/**
 * Status codes for channel operations.
 *
 * Common codes reference base loopyStatus values directly.
 * Module-specific codes use the -100 range.
 */
typedef enum loopyChannelStatus {
    LOOPY_CHANNEL_OK = LOOPY_OK,           /* Operation succeeded */
    LOOPY_CHANNEL_ERROR = LOOPY_ERROR,     /* Internal error */
    LOOPY_CHANNEL_INVALID = LOOPY_INVALID, /* Invalid argument */
    LOOPY_CHANNEL_TIMEOUT = LOOPY_TIMEOUT, /* Operation timed out */
    LOOPY_CHANNEL_CLOSED = LOOPY_CLOSED,   /* Channel has been closed */
    /* Module-specific codes */
    LOOPY_CHANNEL_FULL = -100,  /* Channel is full (non-blocking send) */
    LOOPY_CHANNEL_EMPTY = -101, /* Channel is empty (non-blocking recv) */
} loopyChannelStatus;

/**
 * Channel configuration.
 *
 * All fields have sensible defaults (obtained via loopyChannelConfigInit()).
 */
typedef struct loopyChannelConfig {
    loopyChannelType type;  /* Channel type (default: LOOPY_CHANNEL_SPSC) */
    size_t capacity;        /* Buffer capacity in elements (default: 1024) */
    size_t elementSize;     /* Size of each element in bytes (required) */
    bool blocking;          /* Block on full/empty (default: false) */
    uint64_t sendTimeoutUs; /* Send timeout in microseconds (0 = infinite) */
    uint64_t recvTimeoutUs; /* Receive timeout in microseconds (0 = infinite) */
} loopyChannelConfig;

/* ====================================================================
 * Callbacks
 * ==================================================================== */

/**
 * Callback for async send completion.
 *
 * @param ch      The channel
 * @param status  Result status (LOOPY_CHANNEL_OK on success)
 * @param userData User-provided data
 *
 * Thread Safety: Called on the event loop thread.
 */
typedef void loopyChannelSendCallback(loopyChannel *ch,
                                      loopyChannelStatus status,
                                      void *userData);

/**
 * Callback for async receive completion.
 *
 * @param ch       The channel
 * @param data     Pointer to received data (valid only during callback)
 * @param len      Length of received data in bytes
 * @param status   Result status (LOOPY_CHANNEL_OK on success)
 * @param userData User-provided data
 *
 * Thread Safety: Called on the event loop thread.
 *
 * Note: The data pointer is only valid during the callback. Copy if needed.
 */
typedef void loopyChannelRecvCallback(loopyChannel *ch, const void *data,
                                      size_t len, loopyChannelStatus status,
                                      void *userData);

/**
 * Callback for channel close notification.
 *
 * @param ch       The channel being closed
 * @param userData User-provided data
 */
typedef void loopyChannelCloseCallback(loopyChannel *ch, void *userData);

/* ====================================================================
 * Configuration
 * ==================================================================== */

/**
 * Initialize a channel configuration with default values.
 *
 * @param config The configuration to initialize
 *
 * Default values:
 * - type: LOOPY_CHANNEL_SPSC
 * - capacity: 1024
 * - elementSize: 0 (must be set by caller)
 * - blocking: false
 * - sendTimeoutUs: 0 (infinite when blocking)
 * - recvTimeoutUs: 0 (infinite when blocking)
 */
void loopyChannelConfigInit(loopyChannelConfig *config);

/* ====================================================================
 * Lifecycle
 * ==================================================================== */

/**
 * Create a new channel.
 *
 * @param loop   The event loop for async operations (may be NULL for sync-only)
 * @param config Channel configuration (elementSize must be > 0)
 * @return New channel, or NULL on error
 *
 * Thread Safety: Must be called from the event loop thread if loop is non-NULL.
 *
 * Memory: Allocates sizeof(loopyChannel) + capacity * elementSize bytes.
 *         SPSC adds cache line padding (~128 bytes).
 *
 * Example:
 * @code
 * loopyChannelConfig config;
 * loopyChannelConfigInit(&config);
 * config.elementSize = sizeof(struct MyMessage);
 * config.capacity = 256;
 * config.type = LOOPY_CHANNEL_MPSC;
 *
 * loopyChannel *ch = loopyChannelNew(loop, &config);
 * @endcode
 */
loopyChannel *loopyChannelNew(loopyLoop *loop,
                              const loopyChannelConfig *config);

/**
 * Close a channel.
 *
 * Signals that no more sends will occur. Pending receives can still
 * drain the buffer. After close, sends return LOOPY_CHANNEL_CLOSED.
 *
 * @param ch The channel to close
 *
 * Thread Safety: Safe to call from any thread.
 *
 * Note: This does not free the channel. Use loopyChannelFree() to free.
 */
void loopyChannelClose(loopyChannel *ch);

/**
 * Destroy a channel and free all resources.
 *
 * Closes the channel if not already closed, then frees memory.
 * Safe to call with NULL.
 *
 * @param ch The channel to destroy, or NULL
 *
 * Thread Safety: Must ensure no other threads are accessing the channel.
 *
 * Warning: Any pending async operations will be cancelled.
 */
void loopyChannelFree(loopyChannel *ch);

/* ====================================================================
 * Synchronous Operations
 * ==================================================================== */

/**
 * Send data to the channel (blocking if configured).
 *
 * If blocking is true and the channel is full, blocks until space is
 * available or timeout expires.
 *
 * @param ch   The channel
 * @param data Pointer to data to send (copied into channel)
 * @param len  Length of data (must equal config.elementSize)
 * @return Status code
 *
 * Thread Safety:
 * - SPSC: Must be called from producer thread only
 * - MPSC/MPMC: Safe from any thread
 *
 * Example:
 * @code
 * struct Message msg = { .type = MSG_HELLO, .value = 42 };
 * if (loopyChannelSend(ch, &msg, sizeof(msg)) != LOOPY_CHANNEL_OK) {
 *     // Handle error
 * }
 * @endcode
 */
loopyChannelStatus loopyChannelSend(loopyChannel *ch, const void *data,
                                    size_t len);

/**
 * Receive data from the channel (blocking if configured).
 *
 * If blocking is true and the channel is empty, blocks until data is
 * available or timeout expires.
 *
 * @param ch     The channel
 * @param buf    Buffer to receive data into
 * @param bufLen Size of buffer (must be >= config.elementSize)
 * @return Bytes received (config.elementSize), or negative status code
 *
 * Thread Safety:
 * - SPSC: Must be called from consumer thread only
 * - MPSC: Must be called from single consumer thread
 * - MPMC: Safe from any thread
 *
 * Example:
 * @code
 * struct Message msg;
 * ssize_t n = loopyChannelRecv(ch, &msg, sizeof(msg));
 * if (n > 0) {
 *     printf("Received: type=%d value=%d\n", msg.type, msg.value);
 * }
 * @endcode
 */
ssize_t loopyChannelRecv(loopyChannel *ch, void *buf, size_t bufLen);

/**
 * Try to send data without blocking.
 *
 * Returns immediately if the channel is full.
 *
 * @param ch   The channel
 * @param data Pointer to data to send
 * @param len  Length of data
 * @return Status code (LOOPY_CHANNEL_FULL if channel is full)
 *
 * Thread Safety: Same as loopyChannelSend()
 */
loopyChannelStatus loopyChannelTrySend(loopyChannel *ch, const void *data,
                                       size_t len);

/**
 * Try to receive data without blocking.
 *
 * Returns immediately if the channel is empty.
 *
 * @param ch     The channel
 * @param buf    Buffer to receive data into
 * @param bufLen Size of buffer
 * @return Bytes received, or negative status code (LOOPY_CHANNEL_EMPTY if
 * empty)
 *
 * Thread Safety: Same as loopyChannelRecv()
 */
ssize_t loopyChannelTryRecv(loopyChannel *ch, void *buf, size_t bufLen);

/* ====================================================================
 * Asynchronous Operations
 * ==================================================================== */

/**
 * Send data asynchronously.
 *
 * The callback is invoked on the event loop thread when the send completes
 * or fails. The data is copied immediately, so the caller can reuse the
 * buffer after this call returns.
 *
 * @param ch       The channel (must have been created with a loop)
 * @param data     Pointer to data to send
 * @param len      Length of data
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return true if operation was queued, false on error
 *
 * Thread Safety: Safe from any thread.
 */
bool loopyChannelSendAsync(loopyChannel *ch, const void *data, size_t len,
                           loopyChannelSendCallback *cb, void *userData);

/**
 * Receive data asynchronously.
 *
 * The callback is invoked on the event loop thread when data is available.
 * The buffer is only valid during the callback.
 *
 * @param ch       The channel (must have been created with a loop)
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return true if operation was queued, false on error
 *
 * Thread Safety: Must be called from event loop thread or with proper
 *                synchronization for MPMC.
 */
bool loopyChannelRecvAsync(loopyChannel *ch, loopyChannelRecvCallback *cb,
                           void *userData);

/* ====================================================================
 * Select API (Multiple Channel Waiting)
 * ==================================================================== */

/**
 * Case structure for select operation.
 */
typedef struct loopyChannelCase {
    loopyChannel *ch; /* The channel */
    bool send;        /* true = send operation, false = receive */
    void *data;       /* Data to send, or buffer for receive */
    size_t len;       /* Data length (send) or buffer size (receive) */
    ssize_t result;   /* Result: bytes transferred or negative status */
} loopyChannelCase;

/**
 * Wait for any of multiple channels to be ready.
 *
 * Blocks until at least one channel is ready for the requested operation
 * or the timeout expires. Returns the index of the first ready channel.
 *
 * @param cases     Array of channel cases
 * @param nCases    Number of cases
 * @param timeoutUs Timeout in microseconds (0 = poll, -1 = infinite)
 * @return Index of ready channel, or -1 on timeout, or negative status on error
 *
 * Thread Safety: Cases must be properly synchronized for their channel types.
 *
 * Example:
 * @code
 * loopyChannelCase cases[2] = {
 *     { .ch = inputCh, .send = false, .data = &msg, .len = sizeof(msg) },
 *     { .ch = outputCh, .send = true, .data = &result, .len = sizeof(result) },
 * };
 *
 * int ready = loopyChannelSelect(cases, 2, 1000000);  // 1 second timeout
 * if (ready >= 0) {
 *     printf("Channel %d is ready\n", ready);
 * }
 * @endcode
 */
int loopyChannelSelect(loopyChannelCase *cases, size_t nCases,
                       int64_t timeoutUs);

/* ====================================================================
 * Introspection
 * ==================================================================== */

/**
 * Get the number of elements currently in the channel.
 *
 * @param ch The channel
 * @return Number of elements (may be approximate for MPMC under contention)
 *
 * Thread Safety: Safe from any thread (may be approximate).
 */
size_t loopyChannelLen(const loopyChannel *ch);

/**
 * Get the channel's capacity.
 *
 * @param ch The channel
 * @return Capacity in elements
 *
 * Thread Safety: Safe from any thread.
 */
size_t loopyChannelCap(const loopyChannel *ch);

/**
 * Check if the channel is closed.
 *
 * @param ch The channel
 * @return true if closed
 *
 * Thread Safety: Safe from any thread.
 */
bool loopyChannelIsClosed(const loopyChannel *ch);

/**
 * Check if the channel is full.
 *
 * @param ch The channel
 * @return true if full (may be approximate for MPMC)
 *
 * Thread Safety: Safe from any thread (may be approximate).
 */
bool loopyChannelIsFull(const loopyChannel *ch);

/**
 * Check if the channel is empty.
 *
 * @param ch The channel
 * @return true if empty (may be approximate for MPMC)
 *
 * Thread Safety: Safe from any thread (may be approximate).
 */
bool loopyChannelIsEmpty(const loopyChannel *ch);

/**
 * Get the channel's unique identifier.
 *
 * @param ch The channel
 * @return Unique ID
 */
loopyChannelId loopyChannelGetId(const loopyChannel *ch);

/**
 * Get the channel type.
 *
 * @param ch The channel
 * @return Channel type
 */
loopyChannelType loopyChannelGetType(const loopyChannel *ch);

/**
 * Get the element size.
 *
 * @param ch The channel
 * @return Element size in bytes
 */
size_t loopyChannelGetElementSize(const loopyChannel *ch);

/**
 * Get the associated event loop.
 *
 * @param ch The channel
 * @return Event loop, or NULL if created without one
 */
loopyLoop *loopyChannelGetLoop(const loopyChannel *ch);

/**
 * Get user data from channel.
 *
 * @param ch The channel
 * @return User data pointer, or NULL
 */
void *loopyChannelGetData(const loopyChannel *ch);

/**
 * Set user data on channel.
 *
 * @param ch The channel
 * @param data User data pointer
 */
void loopyChannelSetData(loopyChannel *ch, void *data);

/* ====================================================================
 * Statistics
 * ==================================================================== */

/**
 * Channel statistics.
 */
typedef struct loopyChannelStats {
    uint64_t totalSent;     /* Total elements sent successfully */
    uint64_t totalReceived; /* Total elements received */
    uint64_t sendBlocked;   /* Number of times send blocked */
    uint64_t recvBlocked;   /* Number of times recv blocked */
    uint64_t sendFailed;    /* Failed sends (full, closed, etc.) */
    uint64_t recvFailed;    /* Failed receives (empty, closed, etc.) */
    uint64_t peakUsage;     /* Peak number of elements in channel */
} loopyChannelStats;

/**
 * Get channel statistics.
 *
 * @param ch    The channel
 * @param stats Output statistics structure
 */
void loopyChannelGetStats(const loopyChannel *ch, loopyChannelStats *stats);

/**
 * Reset channel statistics to zero.
 *
 * @param ch The channel
 */
void loopyChannelResetStats(loopyChannel *ch);

/* ====================================================================
 * Utility
 * ==================================================================== */

/**
 * Get a human-readable name for a channel type.
 *
 * @param type Channel type
 * @return Static string ("SPSC", "MPSC", or "MPMC")
 */
const char *loopyChannelTypeName(loopyChannelType type);

/**
 * Get a human-readable description for a status code.
 *
 * @param status Status code
 * @return Static string describing the status
 */
const char *loopyChannelStatusName(loopyChannelStatus status);
