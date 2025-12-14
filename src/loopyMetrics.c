/* loopyMetrics - Event loop performance metrics and observability
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

#include "loopyPlatform.h"

#include "../deps/datakit/src/datakit.h"
#include "loopyInternal.h"
#include "loopyMetrics.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

/* ====================================================================
 * Internal structures
 * ==================================================================== */

struct loopyMetricsInternal {
    /* Event loop iterations */
    uint64_t loopIterations;
    uint64_t eventsProcessed;
    uint64_t timersProcessed;

    /* Time tracking (microseconds) */
    uint64_t totalIdleTime;
    uint64_t totalBusyTime;
    uint64_t totalPollTime;
    uint64_t lastIterationTime;

    /* Poll statistics */
    uint64_t pollCalls;
    uint64_t pollTimeouts;
    uint64_t pollEvents;
    uint64_t pollErrors;

    /* File descriptor tracking */
    uint32_t currentFdCount;
    uint32_t peakFdCount;
    uint64_t fdRegistrations;
    uint64_t fdUnregistrations;

    /* Timer tracking */
    uint32_t currentTimerCount;
    uint32_t peakTimerCount;
    uint64_t timerRegistrations;
    uint64_t timerUnregistrations;

    /* Latency histogram */
    uint64_t latencyBuckets[LOOPY_LATENCY_BUCKET_COUNT];

    /* Timestamps */
    uint64_t startTime;

    /* Temporary for poll tracking */
    uint64_t pollStartTime;
};

/* ====================================================================
 * Helper functions
 * ==================================================================== */

static uint64_t getCurrentTimeUs(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

static int getLatencyBucket(uint64_t latencyUs) {
    if (latencyUs < 1) {
        return LOOPY_LATENCY_UNDER_1US;
    } else if (latencyUs < 10) {
        return LOOPY_LATENCY_UNDER_10US;
    } else if (latencyUs < 100) {
        return LOOPY_LATENCY_UNDER_100US;
    } else if (latencyUs < 1000) {
        return LOOPY_LATENCY_UNDER_1MS;
    } else if (latencyUs < 10000) {
        return LOOPY_LATENCY_UNDER_10MS;
    } else if (latencyUs < 100000) {
        return LOOPY_LATENCY_UNDER_100MS;
    } else if (latencyUs < 1000000) {
        return LOOPY_LATENCY_UNDER_1S;
    } else {
        return LOOPY_LATENCY_OVER_1S;
    }
}

/* ====================================================================
 * Lifecycle
 * ==================================================================== */

bool loopyMetricsEnable(loopyLoop *loop) {
    if (!loop) {
        return false;
    }

    if (loop->metrics) {
        return false; /* Already enabled */
    }

    struct loopyMetricsInternal *m = zcalloc(1, sizeof(*m));
    if (!m) {
        return false;
    }

    m->startTime = getCurrentTimeUs();
    loop->metrics = m;
    return true;
}

void loopyMetricsDisable(loopyLoop *loop) {
    if (!loop || !loop->metrics) {
        return;
    }

    zfree(loop->metrics);
    loop->metrics = NULL;
}

bool loopyMetricsEnabled(const loopyLoop *loop) {
    return loop && loop->metrics != NULL;
}

/* ====================================================================
 * Metrics Access
 * ==================================================================== */

bool loopyMetricsGet(const loopyLoop *loop, loopyMetrics *metrics) {
    if (!loop || !metrics) {
        return false;
    }

    const struct loopyMetricsInternal *m = loop->metrics;
    if (!m) {
        memset(metrics, 0, sizeof(*metrics));
        return false;
    }

    uint64_t now = getCurrentTimeUs();

    metrics->loopIterations = m->loopIterations;
    metrics->eventsProcessed = m->eventsProcessed;
    metrics->timersProcessed = m->timersProcessed;

    metrics->totalIdleTime = m->totalIdleTime;
    metrics->totalBusyTime = m->totalBusyTime;
    metrics->totalPollTime = m->totalPollTime;
    metrics->lastIterationTime = m->lastIterationTime;

    metrics->pollCalls = m->pollCalls;
    metrics->pollTimeouts = m->pollTimeouts;
    metrics->pollEvents = m->pollEvents;
    metrics->pollErrors = m->pollErrors;

    metrics->currentFdCount = m->currentFdCount;
    metrics->peakFdCount = m->peakFdCount;
    metrics->fdRegistrations = m->fdRegistrations;
    metrics->fdUnregistrations = m->fdUnregistrations;

    metrics->currentTimerCount = m->currentTimerCount;
    metrics->peakTimerCount = m->peakTimerCount;
    metrics->timerRegistrations = m->timerRegistrations;
    metrics->timerUnregistrations = m->timerUnregistrations;

    memcpy(metrics->latencyBuckets, m->latencyBuckets,
           sizeof(metrics->latencyBuckets));

    metrics->collectionTime = now;
    metrics->startTime = m->startTime;
    metrics->uptimeUs = now - m->startTime;

    return true;
}

void loopyMetricsReset(loopyLoop *loop) {
    if (!loop || !loop->metrics) {
        return;
    }

    struct loopyMetricsInternal *m = loop->metrics;
    memset(m, 0, sizeof(*m));
    m->startTime = getCurrentTimeUs();
}

/* ====================================================================
 * Individual Metric Accessors
 * ==================================================================== */

uint64_t loopyMetricsGetIterations(const loopyLoop *loop) {
    if (!loop || !loop->metrics) {
        return 0;
    }
    return loop->metrics->loopIterations;
}

uint64_t loopyMetricsGetEventsProcessed(const loopyLoop *loop) {
    if (!loop || !loop->metrics) {
        return 0;
    }
    return loop->metrics->eventsProcessed;
}

uint64_t loopyMetricsGetTimersProcessed(const loopyLoop *loop) {
    if (!loop || !loop->metrics) {
        return 0;
    }
    return loop->metrics->timersProcessed;
}

uint32_t loopyMetricsGetFdCount(const loopyLoop *loop) {
    if (!loop || !loop->metrics) {
        return 0;
    }
    return loop->metrics->currentFdCount;
}

uint32_t loopyMetricsGetPeakFdCount(const loopyLoop *loop) {
    if (!loop || !loop->metrics) {
        return 0;
    }
    return loop->metrics->peakFdCount;
}

uint32_t loopyMetricsGetTimerCount(const loopyLoop *loop) {
    if (!loop || !loop->metrics) {
        return 0;
    }
    return loop->metrics->currentTimerCount;
}

uint64_t loopyMetricsGetUptimeUs(const loopyLoop *loop) {
    if (!loop || !loop->metrics) {
        return 0;
    }
    return getCurrentTimeUs() - loop->metrics->startTime;
}

uint64_t loopyMetricsPollTime(const loopyLoop *loop) {
    if (!loop || !loop->metrics) {
        return 0;
    }
    return loop->metrics->totalPollTime;
}

uint64_t loopyMetricsIdleTime(const loopyLoop *loop) {
    if (!loop || !loop->metrics) {
        return 0;
    }
    return loop->metrics->totalIdleTime;
}

/* ====================================================================
 * Computed Metrics
 * ==================================================================== */

double loopyMetricsGetIdleRatio(const loopyLoop *loop) {
    if (!loop || !loop->metrics) {
        return 0.0;
    }

    const struct loopyMetricsInternal *m = loop->metrics;
    uint64_t total = m->totalIdleTime + m->totalBusyTime;
    if (total == 0) {
        return 1.0; /* No data = idle */
    }
    return (double)m->totalIdleTime / (double)total;
}

double loopyMetricsGetEventsPerIteration(const loopyLoop *loop) {
    if (!loop || !loop->metrics) {
        return 0.0;
    }

    const struct loopyMetricsInternal *m = loop->metrics;
    if (m->loopIterations == 0) {
        return 0.0;
    }
    return (double)m->eventsProcessed / (double)m->loopIterations;
}

double loopyMetricsGetAvgPollTimeUs(const loopyLoop *loop) {
    if (!loop || !loop->metrics) {
        return 0.0;
    }

    const struct loopyMetricsInternal *m = loop->metrics;
    if (m->pollCalls == 0) {
        return 0.0;
    }
    return (double)m->totalPollTime / (double)m->pollCalls;
}

double loopyMetricsGetEventsPerSecond(const loopyLoop *loop) {
    if (!loop || !loop->metrics) {
        return 0.0;
    }

    const struct loopyMetricsInternal *m = loop->metrics;
    uint64_t uptimeUs = getCurrentTimeUs() - m->startTime;
    if (uptimeUs == 0) {
        return 0.0;
    }
    return (double)m->eventsProcessed * 1000000.0 / (double)uptimeUs;
}

/* ====================================================================
 * Formatting
 * ==================================================================== */

const char *loopyMetricsLatencyBucketName(loopyLatencyBucket bucket) {
    switch (bucket) {
    case LOOPY_LATENCY_UNDER_1US:
        return "<1us";
    case LOOPY_LATENCY_UNDER_10US:
        return "<10us";
    case LOOPY_LATENCY_UNDER_100US:
        return "<100us";
    case LOOPY_LATENCY_UNDER_1MS:
        return "<1ms";
    case LOOPY_LATENCY_UNDER_10MS:
        return "<10ms";
    case LOOPY_LATENCY_UNDER_100MS:
        return "<100ms";
    case LOOPY_LATENCY_UNDER_1S:
        return "<1s";
    case LOOPY_LATENCY_OVER_1S:
        return ">=1s";
    default:
        return "unknown";
    }
}

size_t loopyMetricsFormat(const loopyMetrics *metrics, char *buf,
                          size_t bufLen) {
    if (!metrics) {
        return 0;
    }

    /* Calculate uptime components */
    uint64_t uptimeSec = metrics->uptimeUs / 1000000;
    uint64_t hours = uptimeSec / 3600;
    uint64_t minutes = (uptimeSec % 3600) / 60;
    uint64_t seconds = uptimeSec % 60;

    /* Calculate poll percentages */
    double eventPct = 0.0;
    double timeoutPct = 0.0;
    if (metrics->pollCalls > 0) {
        eventPct =
            100.0 * (double)metrics->pollEvents / (double)metrics->pollCalls;
        timeoutPct =
            100.0 * (double)metrics->pollTimeouts / (double)metrics->pollCalls;
    }

    /* Calculate idle ratio */
    double idleRatio = 0.0;
    uint64_t totalTime = metrics->totalIdleTime + metrics->totalBusyTime;
    if (totalTime > 0) {
        idleRatio = 100.0 * (double)metrics->totalIdleTime / (double)totalTime;
    }

    /* Calculate average poll time */
    double avgPollMs = 0.0;
    if (metrics->pollCalls > 0) {
        avgPollMs = (double)metrics->totalPollTime /
                    (double)metrics->pollCalls / 1000.0;
    }

    int needed = snprintf(
        buf, bufLen,
        "Uptime: %lluh %llum %llus\n"
        "Iterations: %llu | Events: %llu | Timers: %llu\n"
        "Poll: %llu calls (%.1f%% events, %.1f%% timeouts, %llu errors)\n"
        "FDs: %u current / %u peak | Timers: %u current / %u peak\n"
        "Idle: %.1f%% | Avg poll: %.2fms",
        (unsigned long long)hours, (unsigned long long)minutes,
        (unsigned long long)seconds,
        (unsigned long long)metrics->loopIterations,
        (unsigned long long)metrics->eventsProcessed,
        (unsigned long long)metrics->timersProcessed,
        (unsigned long long)metrics->pollCalls, eventPct, timeoutPct,
        (unsigned long long)metrics->pollErrors,
        (unsigned)metrics->currentFdCount, (unsigned)metrics->peakFdCount,
        (unsigned)metrics->currentTimerCount, (unsigned)metrics->peakTimerCount,
        idleRatio, avgPollMs);

    return (size_t)(needed > 0 ? needed : 0);
}

/* ====================================================================
 * Internal Update Functions
 * ==================================================================== */

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
void loopyMetricsRecordIteration(loopyLoop *loop) {
    if (!loop || !loop->metrics) {
        return;
    }
    loop->metrics->loopIterations++;
}

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
void loopyMetricsRecordEvent(loopyLoop *loop) {
    if (!loop || !loop->metrics) {
        return;
    }
    loop->metrics->eventsProcessed++;
}

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
void loopyMetricsRecordEvents(loopyLoop *loop, uint32_t count) {
    if (!loop || !loop->metrics) {
        return;
    }
    loop->metrics->eventsProcessed += count;
}

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
void loopyMetricsRecordTimer(loopyLoop *loop) {
    if (!loop || !loop->metrics) {
        return;
    }
    loop->metrics->timersProcessed++;
}

void loopyMetricsRecordPoll(loopyLoop *loop, uint64_t durationUs,
                            bool hadEvents, bool hadTimeout, bool hadError) {
    if (!loop || !loop->metrics) {
        return;
    }

    struct loopyMetricsInternal *m = loop->metrics;
    m->pollCalls++;
    m->totalPollTime += durationUs;

    if (hadEvents) {
        m->pollEvents++;
    }
    if (hadTimeout) {
        m->pollTimeouts++;
    }
    if (hadError) {
        m->pollErrors++;
    }
}

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
void loopyMetricsRecordFdRegistration(loopyLoop *loop) {
    if (!loop || !loop->metrics) {
        return;
    }

    struct loopyMetricsInternal *m = loop->metrics;
    m->fdRegistrations++;
    m->currentFdCount++;
    if (m->currentFdCount > m->peakFdCount) {
        m->peakFdCount = m->currentFdCount;
    }
}

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
void loopyMetricsRecordFdUnregistration(loopyLoop *loop) {
    if (!loop || !loop->metrics) {
        return;
    }

    struct loopyMetricsInternal *m = loop->metrics;
    m->fdUnregistrations++;
    if (m->currentFdCount > 0) {
        m->currentFdCount--;
    }
}

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
void loopyMetricsRecordTimerRegistration(loopyLoop *loop) {
    if (!loop || !loop->metrics) {
        return;
    }

    struct loopyMetricsInternal *m = loop->metrics;
    m->timerRegistrations++;
    m->currentTimerCount++;
    if (m->currentTimerCount > m->peakTimerCount) {
        m->peakTimerCount = m->currentTimerCount;
    }
}

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
void loopyMetricsRecordTimerUnregistration(loopyLoop *loop) {
    if (!loop || !loop->metrics) {
        return;
    }

    struct loopyMetricsInternal *m = loop->metrics;
    m->timerUnregistrations++;
    if (m->currentTimerCount > 0) {
        m->currentTimerCount--;
    }
}

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
void loopyMetricsRecordIdleTime(loopyLoop *loop, uint64_t durationUs) {
    if (!loop || !loop->metrics) {
        return;
    }
    loop->metrics->totalIdleTime += durationUs;
}

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
void loopyMetricsRecordBusyTime(loopyLoop *loop, uint64_t durationUs) {
    if (!loop || !loop->metrics) {
        return;
    }
    loop->metrics->totalBusyTime += durationUs;
    loop->metrics->lastIterationTime = durationUs;
}

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
void loopyMetricsRecordEventLatency(loopyLoop *loop, uint64_t latencyUs) {
    if (!loop || !loop->metrics) {
        return;
    }

    int bucket = getLatencyBucket(latencyUs);
    loop->metrics->latencyBuckets[bucket]++;
}

/* ====================================================================
 * Compatibility names used by loopy.c
 * ==================================================================== */

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
void loopyMetricsPollEntry(loopyLoop *loop) {
    if (!loop || !loop->metrics) {
        return;
    }
    loop->metrics->pollStartTime = getCurrentTimeUs();
}

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
void loopyMetricsPollExit(loopyLoop *loop, int numevents) {
    if (!loop || !loop->metrics) {
        return;
    }

    struct loopyMetricsInternal *m = loop->metrics;
    uint64_t duration = getCurrentTimeUs() - m->pollStartTime;
    bool hadEvents = (numevents > 0);
    bool hadTimeout = (numevents == 0);

    m->pollCalls++;
    m->totalPollTime += duration;
    m->totalIdleTime += duration;

    if (hadEvents) {
        m->pollEvents++;
    }
    if (hadTimeout) {
        m->pollTimeouts++;
    }
}

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
void loopyMetricsAddEventsProcessed(loopyLoop *loop, int count) {
    if (!loop || !loop->metrics || count <= 0) {
        return;
    }
    loop->metrics->eventsProcessed += (uint64_t)count;
}

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
void loopyMetricsIncrementTimersProcessed(loopyLoop *loop) {
    if (!loop || !loop->metrics) {
        return;
    }
    loop->metrics->timersProcessed++;
}

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
void loopyMetricsIncrementIterations(loopyLoop *loop) {
    if (!loop || !loop->metrics) {
        return;
    }
    loop->metrics->loopIterations++;
}
