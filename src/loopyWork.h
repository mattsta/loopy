/* loopyWork - Thread pool work queue for loopy event loop
 *
 * Offload blocking or CPU-intensive work to background threads without
 * blocking the event loop. Work callbacks run on worker threads, while
 * after-work callbacks run on the event loop thread.
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

/* ====================================================================
 * Types
 * ==================================================================== */

/**
 * Opaque work queue handle.
 */
typedef struct loopyWork loopyWork;

/**
 * Unique identifier for a queued work item.
 * Valid IDs are > 0. A value of 0 indicates an error.
 */
typedef uint64_t loopyWorkId;

/**
 * Work callback - runs on WORKER THREAD.
 *
 * Perform CPU-intensive or blocking operations here. Do NOT access the
 * event loop or other non-thread-safe loopy structures from this callback.
 *
 * @param work      The work queue handle
 * @param workId    ID of this work item
 * @param userData  User data passed to loopyWorkQueue()
 *
 * Thread Safety: Called from a worker thread, NOT the event loop thread.
 */
typedef void loopyWorkCallback(loopyWork *work, loopyWorkId workId,
                               void *userData);

/**
 * Status codes for work completion.
 *
 * Common codes reference base loopyStatus values directly.
 */
typedef enum loopyWorkStatus {
    LOOPY_WORK_OK = LOOPY_OK,               /* Work completed successfully */
    LOOPY_WORK_ERROR = LOOPY_ERROR,         /* Work encountered an error */
    LOOPY_WORK_CANCELLED = LOOPY_CANCELLED, /* Work was cancelled */
} loopyWorkStatus;

/**
 * After-work callback - runs on EVENT LOOP THREAD.
 *
 * Called after the work callback completes (or was cancelled). Safe to access
 * event loop and other loopy structures here.
 *
 * @param loop      The event loop
 * @param work      The work queue handle
 * @param workId    ID of this work item
 * @param status    Result status (OK, CANCELLED, or ERROR)
 * @param userData  User data passed to loopyWorkQueue()
 *
 * Thread Safety: Always called on the event loop thread.
 */
typedef void loopyAfterWorkCallback(loopyLoop *loop, loopyWork *work,
                                    loopyWorkId workId, loopyWorkStatus status,
                                    void *userData);

/**
 * Configuration for work queue.
 */
typedef struct loopyWorkConfig {
    size_t minThreads; /* Minimum worker threads (default: 1) */
    size_t maxThreads; /* Maximum worker threads (default: 4) */
    size_t
        maxQueueSize; /* Max pending work items (default: 256, 0 = unlimited) */
} loopyWorkConfig;

/**
 * Default configuration.
 */
#define LOOPY_WORK_CONFIG_DEFAULT                                              \
    (loopyWorkConfig){.minThreads = 1, .maxThreads = 4, .maxQueueSize = 256}

/**
 * Initialize a work config with default values.
 *
 * @param config Config struct to initialize (must not be NULL)
 */
void loopyWorkConfigInit(loopyWorkConfig *config);

/* ====================================================================
 * Lifecycle
 * ==================================================================== */

/**
 * Create a new work queue.
 *
 * The work queue manages a thread pool for executing work callbacks.
 * Completion notifications are delivered via the event loop.
 *
 * @param loop   The event loop (must not be NULL)
 * @param config Configuration (NULL for defaults)
 * @return New work queue, or NULL on error
 *
 * Thread Safety: Must be called from the event loop thread.
 *
 * Example:
 * @code
 * loopyWorkConfig config = LOOPY_WORK_CONFIG_DEFAULT;
 * config.maxThreads = 8;
 * loopyWork *work = loopyWorkNew(loop, &config);
 * @endcode
 */
loopyWork *loopyWorkNew(loopyLoop *loop, const loopyWorkConfig *config);

/**
 * Free a work queue.
 *
 * Cancels all pending work, waits for running work to complete, and releases
 * all resources. Safe to call with NULL.
 *
 * @param work The work queue to free, or NULL
 *
 * Thread Safety: Must be called from the event loop thread.
 * Note: This will block until all running workers finish.
 */
void loopyWorkFree(loopyWork *work);

/* ====================================================================
 * Work Queue Operations
 * ==================================================================== */

/**
 * Queue work for execution on a worker thread.
 *
 * The work callback will be executed on a worker thread. When complete,
 * the after-work callback will be invoked on the event loop thread.
 *
 * @param work      The work queue (must not be NULL)
 * @param workCb    Work callback - runs on worker thread (must not be NULL)
 * @param afterCb   After-work callback - runs on loop thread (may be NULL)
 * @param userData  User data passed to both callbacks (may be NULL)
 * @return Work ID > 0 on success, 0 on error (queue full or invalid params)
 *
 * Thread Safety: Safe to call from any thread.
 *
 * Example:
 * @code
 * void doWork(loopyWork *w, loopyWorkId id, void *data) {
 *     // CPU-intensive work here
 *     compute_something_heavy(data);
 * }
 *
 * void afterWork(loopyLoop *l, loopyWork *w, loopyWorkId id,
 *                loopyWorkStatus status, void *data) {
 *     if (status == LOOPY_WORK_OK) {
 *         printf("Work completed!\n");
 *     }
 * }
 *
 * loopyWorkId id = loopyWorkQueue(work, doWork, afterWork, myData);
 * @endcode
 */
loopyWorkId loopyWorkQueue(loopyWork *work, loopyWorkCallback *workCb,
                           loopyAfterWorkCallback *afterCb, void *userData);

/**
 * Cancel pending work.
 *
 * Attempts to cancel work that has not yet started executing. If the work
 * is already running, it cannot be cancelled and this returns false.
 *
 * @param work   The work queue
 * @param workId ID of work item to cancel
 * @return true if work was cancelled, false if not found or already running
 *
 * Thread Safety: Safe to call from any thread.
 *
 * @note There is a race condition: work may be picked up by a worker thread
 *       between this function checking the queue and the worker thread
 *       checking the cancelled flag. Once started, work cannot be cancelled.
 */
bool loopyWorkCancel(loopyWork *work, loopyWorkId workId);

/**
 * Cancel all pending work.
 *
 * Marks all pending (not yet running) work items as cancelled.
 *
 * @param work The work queue
 *
 * Thread Safety: Safe to call from any thread.
 *
 * @note Work items that have already started executing will continue to
 *       completion and their after-work callbacks will be invoked with
 *       the completion status they achieve, not cancelled status.
 */
void loopyWorkCancelAll(loopyWork *work);

/* ====================================================================
 * Status
 * ==================================================================== */

/**
 * Get count of pending work items (not yet started).
 *
 * @param work The work queue
 * @return Number of pending work items
 *
 * @note This is a racy read - the actual count may change between this call
 *       and when the value is used, since other threads may queue or start
 * work. Intended for monitoring/statistics only, not synchronization.
 */
size_t loopyWorkPendingCount(const loopyWork *work);

/**
 * Get count of currently running work items.
 *
 * @param work The work queue
 * @return Number of running work items
 *
 * @note This is a racy read - the actual count may change between this call
 *       and when the value is used. Intended for monitoring/statistics only.
 */
size_t loopyWorkRunningCount(const loopyWork *work);

/**
 * Get current thread pool size.
 *
 * @param work The work queue
 * @return Number of worker threads
 */
size_t loopyWorkThreadCount(const loopyWork *work);

/**
 * Get status string.
 *
 * @param status The status code
 * @return Human-readable status string ("OK", "CANCELLED", "ERROR")
 */
const char *loopyWorkStatusString(loopyWorkStatus status);

/**
 * Get the event loop associated with this work queue.
 *
 * @param work The work queue
 * @return The event loop, or NULL if work is NULL
 */
loopyLoop *loopyWorkGetLoop(const loopyWork *work);

/**
 * Get user data from work queue.
 *
 * @param work The work queue
 * @return User data pointer previously set by loopyWorkSetData(), or NULL
 *
 * Thread Safety: Safe to call from any thread, but the returned pointer
 *                is only valid if the caller prevents concurrent modification.
 */
void *loopyWorkGetData(const loopyWork *work);

/**
 * Set user data on work queue.
 *
 * Stores an opaque user data pointer for retrieval via loopyWorkGetData().
 * This is useful for associating the work queue with application-specific
 * context (e.g., a database connection pool, logging context, etc.).
 *
 * @param work The work queue
 * @param data User data pointer
 *
 * Thread Safety: Not thread-safe. Must be called from event loop thread
 *                if other threads may call loopyWorkGetData().
 */
void loopyWorkSetData(loopyWork *work, void *data);
