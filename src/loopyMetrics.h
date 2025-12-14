/* loopyMetrics - Event loop performance metrics and observability
 *
 * Provides comprehensive metrics for monitoring event loop health,
 * performance analysis, and debugging. Metrics are collected with
 * minimal overhead and can be enabled/disabled at runtime.
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
 * Metrics snapshot - point-in-time copy of all metrics.
 */
typedef struct loopyMetrics {
    /* Event loop iterations */
    uint64_t loopIterations;  /* Total event loop iterations */
    uint64_t eventsProcessed; /* Total file events processed */
    uint64_t timersProcessed; /* Total timer callbacks executed */

    /* Time tracking (microseconds) */
    uint64_t totalIdleTime;     /* Time spent in poll/sleep */
    uint64_t totalBusyTime;     /* Time spent processing callbacks */
    uint64_t totalPollTime;     /* Time in backend poll calls */
    uint64_t lastIterationTime; /* Duration of last iteration */

    /* Poll statistics */
    uint64_t pollCalls;    /* Number of poll/kevent/epoll calls */
    uint64_t pollTimeouts; /* Polls that returned due to timeout */
    uint64_t pollEvents;   /* Polls that returned with events */
    uint64_t pollErrors;   /* Poll errors */

    /* File descriptor tracking */
    uint32_t currentFdCount;    /* Currently registered FDs */
    uint32_t peakFdCount;       /* Maximum FDs ever registered */
    uint64_t fdRegistrations;   /* Total FD registrations */
    uint64_t fdUnregistrations; /* Total FD unregistrations */

    /* Timer tracking */
    uint32_t currentTimerCount;    /* Currently active timers */
    uint32_t peakTimerCount;       /* Maximum timers ever active */
    uint64_t timerRegistrations;   /* Total timer registrations */
    uint64_t timerUnregistrations; /* Total timer unregistrations */

    /* Latency buckets (microseconds) - histogram of event processing time */
    uint64_t latencyBuckets[8]; /* <1us, <10us, <100us, <1ms, <10ms, <100ms,
                                   <1s, >=1s */

    /* Timestamps */
    uint64_t collectionTime; /* When these metrics were collected */
    uint64_t startTime;      /* When metrics collection started */
    uint64_t uptimeUs;       /* Total uptime in microseconds */
} loopyMetrics;

/**
 * Latency bucket indices for loopyMetrics.latencyBuckets array.
 */
typedef enum loopyLatencyBucket {
    LOOPY_LATENCY_UNDER_1US = 0, /* < 1 microsecond */
    LOOPY_LATENCY_UNDER_10US,    /* < 10 microseconds */
    LOOPY_LATENCY_UNDER_100US,   /* < 100 microseconds */
    LOOPY_LATENCY_UNDER_1MS,     /* < 1 millisecond */
    LOOPY_LATENCY_UNDER_10MS,    /* < 10 milliseconds */
    LOOPY_LATENCY_UNDER_100MS,   /* < 100 milliseconds */
    LOOPY_LATENCY_UNDER_1S,      /* < 1 second */
    LOOPY_LATENCY_OVER_1S,       /* >= 1 second */
    LOOPY_LATENCY_BUCKET_COUNT = 8
} loopyLatencyBucket;

/* ====================================================================
 * Lifecycle
 * ==================================================================== */

/**
 * Enable metrics collection for an event loop.
 *
 * Once enabled, metrics are collected automatically with minimal overhead.
 * Metrics remain enabled until explicitly disabled or the loop is destroyed.
 *
 * @param loop The event loop
 * @return true on success, false if already enabled or error
 *
 * Thread Safety: Must be called from the event loop thread.
 */
bool loopyMetricsEnable(loopyLoop *loop);

/**
 * Disable metrics collection.
 *
 * Stops collecting metrics and frees internal resources.
 *
 * @param loop The event loop
 *
 * Thread Safety: Must be called from the event loop thread.
 */
void loopyMetricsDisable(loopyLoop *loop);

/**
 * Check if metrics collection is enabled.
 *
 * @param loop The event loop
 * @return true if metrics are being collected
 */
bool loopyMetricsEnabled(const loopyLoop *loop);

/* ====================================================================
 * Metrics Access
 * ==================================================================== */

/**
 * Get a snapshot of current metrics.
 *
 * Provides a consistent point-in-time view of all metrics.
 * The returned structure is a copy and won't change.
 *
 * @param loop    The event loop
 * @param metrics Output structure to fill (must not be NULL)
 * @return true on success, false if metrics disabled or error
 *
 * Thread Safety: Safe to call from any thread.
 *
 * Example:
 * @code
 * loopyMetrics m;
 * if (loopyMetricsGet(loop, &m)) {
 *     printf("Iterations: %llu, Events: %llu\n",
 *            m.loopIterations, m.eventsProcessed);
 * }
 * @endcode
 */
bool loopyMetricsGet(const loopyLoop *loop, loopyMetrics *metrics);

/**
 * Reset all metrics to zero.
 *
 * Clears all accumulated metrics but keeps collection enabled.
 * The startTime is reset to the current time.
 *
 * @param loop The event loop
 *
 * Thread Safety: Must be called from the event loop thread.
 */
void loopyMetricsReset(loopyLoop *loop);

/* ====================================================================
 * Individual Metric Accessors (lock-free, minimal overhead)
 * ==================================================================== */

/**
 * Get the number of event loop iterations.
 */
uint64_t loopyMetricsGetIterations(const loopyLoop *loop);

/**
 * Get total file events processed.
 */
uint64_t loopyMetricsGetEventsProcessed(const loopyLoop *loop);

/**
 * Get total timer callbacks executed.
 */
uint64_t loopyMetricsGetTimersProcessed(const loopyLoop *loop);

/**
 * Get current registered file descriptor count.
 */
uint32_t loopyMetricsGetFdCount(const loopyLoop *loop);

/**
 * Get peak file descriptor count.
 */
uint32_t loopyMetricsGetPeakFdCount(const loopyLoop *loop);

/**
 * Get current active timer count.
 */
uint32_t loopyMetricsGetTimerCount(const loopyLoop *loop);

/**
 * Get uptime in microseconds since metrics were enabled (or last reset).
 */
uint64_t loopyMetricsGetUptimeUs(const loopyLoop *loop);

/**
 * Get total poll time in microseconds.
 */
uint64_t loopyMetricsPollTime(const loopyLoop *loop);

/**
 * Get total idle time in microseconds.
 */
uint64_t loopyMetricsIdleTime(const loopyLoop *loop);

/* ====================================================================
 * Computed Metrics
 * ==================================================================== */

/**
 * Get the ratio of idle time to total time (0.0 to 1.0).
 *
 * Higher values indicate the loop is spending more time waiting for events,
 * which usually means it has capacity for more work.
 *
 * @param loop The event loop
 * @return Idle ratio (0.0 = always busy, 1.0 = always idle)
 */
double loopyMetricsGetIdleRatio(const loopyLoop *loop);

/**
 * Get average events processed per iteration.
 *
 * @param loop The event loop
 * @return Average events per iteration
 */
double loopyMetricsGetEventsPerIteration(const loopyLoop *loop);

/**
 * Get average poll duration in microseconds.
 *
 * @param loop The event loop
 * @return Average microseconds per poll call
 */
double loopyMetricsGetAvgPollTimeUs(const loopyLoop *loop);

/**
 * Get event processing rate (events per second).
 *
 * @param loop The event loop
 * @return Events processed per second
 */
double loopyMetricsGetEventsPerSecond(const loopyLoop *loop);

/* ====================================================================
 * Formatting
 * ==================================================================== */

/**
 * Format metrics as a human-readable string.
 *
 * @param metrics Metrics to format
 * @param buf     Output buffer
 * @param bufLen  Buffer size
 * @return Number of characters written (excluding null terminator),
 *         or required size if buf is NULL or bufLen is 0
 *
 * Example output:
 *   Uptime: 1h 23m 45s
 *   Iterations: 1,234,567 | Events: 9,876,543 | Timers: 123,456
 *   Poll: 1,234,567 calls (99.1% events, 0.9% timeouts)
 *   FDs: 42 current / 128 peak | Timers: 5 current / 32 peak
 *   Idle: 87.3% | Avg poll: 1.23ms
 */
size_t loopyMetricsFormat(const loopyMetrics *metrics, char *buf,
                          size_t bufLen);

/**
 * Get name of a latency bucket.
 *
 * @param bucket The bucket index
 * @return Human-readable bucket name (e.g., "<1us", "<10ms")
 */
const char *loopyMetricsLatencyBucketName(loopyLatencyBucket bucket);

/* ====================================================================
 * Internal Update Functions (called by event loop implementation)
 * ==================================================================== */

/* These are internal functions called by the event loop to update metrics.
 * They are exposed here for use by loopy adapters but should not be called
 * directly by application code. */

/**
 * Record completion of one event loop iteration.
 *
 * This function increments the event loop iteration counter. Called once per
 * iteration of the main event loop to track how many iterations have occurred
 * and provide context for computing per-iteration metrics like
 * events/iteration.
 *
 * @param loop The event loop
 *
 * @note Internal function - called by event loop implementation only
 * @note Thread Safety: Must be called from the event loop thread
 * @note This is a non-blocking operation with minimal overhead
 */
void loopyMetricsRecordIteration(loopyLoop *loop);

/**
 * Record processing of a single file descriptor event.
 *
 * Increments the total count of processed file descriptor events. This includes
 * all events from monitored file descriptors (readable, writable, error
 * states).
 *
 * @param loop The event loop
 *
 * @note Internal function - called by event loop implementation only
 * @note Thread Safety: Must be called from the event loop thread
 * @note Use loopyMetricsRecordEvents() when processing multiple events in batch
 */
void loopyMetricsRecordEvent(loopyLoop *loop);

/**
 * Record processing of multiple file descriptor events.
 *
 * Adds count to the total events processed counter. Used when multiple file
 * descriptor events are processed in a single operation (e.g., batch processing
 * from epoll/kevent/kqueue).
 *
 * @param loop  The event loop
 * @param count Number of events processed (must be >= 0)
 *
 * @note Internal function - called by event loop implementation only
 * @note Thread Safety: Must be called from the event loop thread
 * @note If count is 0 or negative, this function has no effect
 */
void loopyMetricsRecordEvents(loopyLoop *loop, uint32_t count);

/**
 * Record execution of a single timer callback.
 *
 * Increments the total count of executed timer callbacks. Called once per
 * timer callback that fires and is executed by the event loop.
 *
 * @param loop The event loop
 *
 * @note Internal function - called by event loop implementation only
 * @note Thread Safety: Must be called from the event loop thread
 * @note This records executed timers, not registered timers
 * @see loopyMetricsRecordTimerRegistration
 */
void loopyMetricsRecordTimer(loopyLoop *loop);

/**
 * Record the result and timing of a poll/wait system call.
 *
 * Updates poll statistics including call count, total poll time, and
 * categorizes the result (events returned, timeout, or error). Time is measured
 * in microseconds from poll entry to exit by the backend adapter.
 *
 * @param loop       The event loop
 * @param durationUs Duration of the poll call in microseconds
 * @param hadEvents  True if poll returned with file descriptor events ready
 * @param hadTimeout True if poll returned due to timeout
 * @param hadError   True if poll returned due to an error
 *
 * @note Internal function - called by event loop adapters
 * @note Thread Safety: Must be called from the event loop thread
 * @note Multiple flags can be set simultaneously (e.g., hadEvents and
 * hadTimeout)
 * @note The poll duration contributes to idle time metrics
 */
void loopyMetricsRecordPoll(loopyLoop *loop, uint64_t durationUs,
                            bool hadEvents, bool hadTimeout, bool hadError);

/**
 * Record registration of a file descriptor for monitoring.
 *
 * Increments the file descriptor registration counter and updates the current
 * and peak file descriptor counts. Called when a file descriptor is added to
 * the event loop's monitoring set.
 *
 * @param loop The event loop
 *
 * @note Internal function - called by file descriptor registration functions
 * @note Thread Safety: Must be called from the event loop thread
 * @note Increments currentFdCount and updates peakFdCount if necessary
 * @see loopyMetricsRecordFdUnregistration
 */
void loopyMetricsRecordFdRegistration(loopyLoop *loop);

/**
 * Record unregistration of a file descriptor from monitoring.
 *
 * Increments the file descriptor unregistration counter and decrements the
 * current file descriptor count. Called when a file descriptor is removed from
 * the event loop's monitoring set.
 *
 * @param loop The event loop
 *
 * @note Internal function - called by file descriptor unregistration functions
 * @note Thread Safety: Must be called from the event loop thread
 * @note Decrements currentFdCount (prevents underflow)
 * @see loopyMetricsRecordFdRegistration
 */
void loopyMetricsRecordFdUnregistration(loopyLoop *loop);

/**
 * Record registration of a timer with the event loop.
 *
 * Increments the timer registration counter and updates the current and peak
 * timer counts. Called when a new timer is added to the event loop.
 *
 * @param loop The event loop
 *
 * @note Internal function - called by timer registration functions
 * @note Thread Safety: Must be called from the event loop thread
 * @note Increments currentTimerCount and updates peakTimerCount if necessary
 * @see loopyMetricsRecordTimerUnregistration
 */
void loopyMetricsRecordTimerRegistration(loopyLoop *loop);

/**
 * Record unregistration of a timer from the event loop.
 *
 * Increments the timer unregistration counter and decrements the current timer
 * count. Called when a timer is removed or completes in the event loop.
 *
 * @param loop The event loop
 *
 * @note Internal function - called by timer unregistration functions
 * @note Thread Safety: Must be called from the event loop thread
 * @note Decrements currentTimerCount (prevents underflow)
 * @see loopyMetricsRecordTimerRegistration
 */
void loopyMetricsRecordTimerUnregistration(loopyLoop *loop);

/**
 * Record time spent by the event loop waiting for events.
 *
 * Adds the specified duration to total idle time. Idle time represents periods
 * when the event loop is blocked in poll/wait operations, waiting for file
 * descriptors or timer events.
 *
 * @param loop      The event loop
 * @param durationUs Duration of idle time in microseconds
 *
 * @note Internal function - called by event loop iteration logic
 * @note Thread Safety: Must be called from the event loop thread
 * @note Time units are microseconds (1,000,000 = 1 second)
 * @note Used to compute idle ratio and busy ratio metrics
 */
void loopyMetricsRecordIdleTime(loopyLoop *loop, uint64_t durationUs);

/**
 * Record time spent by the event loop processing callbacks.
 *
 * Adds the specified duration to total busy time and updates the last iteration
 * time. Busy time represents periods when the event loop is executing callbacks
 * for file descriptor or timer events.
 *
 * @param loop      The event loop
 * @param durationUs Duration of busy time in microseconds
 *
 * @note Internal function - called by event loop iteration logic
 * @note Thread Safety: Must be called from the event loop thread
 * @note Time units are microseconds (1,000,000 = 1 second)
 * @note Used to compute idle ratio and per-iteration timing metrics
 */
void loopyMetricsRecordBusyTime(loopyLoop *loop, uint64_t durationUs);

/**
 * Record latency of event processing in the event loop.
 *
 * Categorizes and records the latency of processing a single event into the
 * appropriate latency histogram bucket. Latency is the time from when an event
 * becomes available until it is processed by the event loop.
 *
 * @param loop      The event loop
 * @param latencyUs Latency duration in microseconds
 *
 * @note Internal function - called by event processing logic
 * @note Thread Safety: Must be called from the event loop thread
 * @note Time units are microseconds (1,000,000 = 1 second)
 * @note Latency is bucketed: <1us, <10us, <100us, <1ms, <10ms, <100ms, <1s,
 * >=1s
 * @see loopyLatencyBucket for bucket definitions
 */
void loopyMetricsRecordEventLatency(loopyLoop *loop, uint64_t latencyUs);

/* Compatibility names used by loopy.c */

/**
 * Record entry into a poll/wait system call.
 *
 * Captures the current timestamp before a poll/kevent/epoll call begins.
 * Used in conjunction with loopyMetricsPollExit() to measure poll duration
 * and categorize the result.
 *
 * @param loop The event loop
 *
 * @note Internal function - compatibility wrapper for poll timing
 * @note Thread Safety: Must be called from the event loop thread
 * @note Must be paired with loopyMetricsPollExit() to properly record poll
 * metrics
 * @see loopyMetricsPollExit
 */
void loopyMetricsPollEntry(loopyLoop *loop);

/**
 * Record exit from a poll/wait system call.
 *
 * Calculates the duration since loopyMetricsPollEntry() and categorizes the
 * result based on whether events were returned. Increments poll call count,
 * updates idle time, and tracks poll outcome statistics.
 *
 * @param loop      The event loop
 * @param numevents Number of events returned from the poll call (0 = timeout)
 *
 * @note Internal function - compatibility wrapper for poll timing
 * @note Thread Safety: Must be called from the event loop thread
 * @note Must be paired with loopyMetricsPollEntry() to properly measure poll
 * time
 * @note numevents > 0 indicates file descriptor events, numevents == 0
 * indicates timeout
 * @see loopyMetricsPollEntry
 */
void loopyMetricsPollExit(loopyLoop *loop, int numevents);

/**
 * Add a count of processed file descriptor events.
 *
 * Increments the total events processed counter by the specified count.
 * This is a compatibility wrapper that provides the same functionality as
 * loopyMetricsRecordEvents() but validates the count parameter.
 *
 * @param loop  The event loop
 * @param count Number of events to add (must be positive, ignores non-positive
 * values)
 *
 * @note Internal function - compatibility wrapper
 * @note Thread Safety: Must be called from the event loop thread
 * @note Non-positive count values are silently ignored
 * @see loopyMetricsRecordEvents
 */
void loopyMetricsAddEventsProcessed(loopyLoop *loop, int count);

/**
 * Increment the count of executed timer callbacks.
 *
 * Increments the total timers processed counter by one. This is a compatibility
 * wrapper that provides the same functionality as loopyMetricsRecordTimer().
 *
 * @param loop The event loop
 *
 * @note Internal function - compatibility wrapper
 * @note Thread Safety: Must be called from the event loop thread
 * @see loopyMetricsRecordTimer
 */
void loopyMetricsIncrementTimersProcessed(loopyLoop *loop);

/**
 * Increment the event loop iteration counter.
 *
 * Increments the loop iterations counter by one. This is a compatibility
 * wrapper that provides the same functionality as
 * loopyMetricsRecordIteration().
 *
 * @param loop The event loop
 *
 * @note Internal function - compatibility wrapper
 * @note Thread Safety: Must be called from the event loop thread
 * @see loopyMetricsRecordIteration
 */
void loopyMetricsIncrementIterations(loopyLoop *loop);
