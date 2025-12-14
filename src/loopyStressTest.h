/* loopyStressTest - Comprehensive concurrent stress testing framework
 *
 * Provides a clean, well-encapsulated framework for stress testing concurrent
 * data structures with:
 *   - Worker thread management with barrier synchronization
 *   - Atomic statistics collection
 *   - Invariant checking with detailed failure reports
 *   - Configurable duration, iterations, thread counts
 *   - Deterministic seeding for reproducibility
 *   - Data integrity verification (checksums, sequence tracking)
 *
 * Copyright 2024 Matt Stancliff <matt@genges.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 */

#ifndef LOOPY_STRESS_TEST_H
#define LOOPY_STRESS_TEST_H

#include "loopyPlatform.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ====================================================================
 * Types
 * ==================================================================== */

/**
 * Opaque stress test harness.
 */
typedef struct loopyStressHarness loopyStressHarness;

/**
 * Per-worker context passed to worker functions.
 */
typedef struct loopyStressWorker {
    uint32_t workerId;           /* Worker ID (0 to numWorkers-1) */
    uint32_t numWorkers;         /* Total number of workers */
    void *sharedContext;         /* Shared context across all workers */
    void *workerContext;         /* Per-worker private context */
    uint64_t seed;               /* Per-worker random seed */
    uint64_t iterations;         /* Iterations to run (0 = duration-based) */
    uint64_t durationMs;         /* Duration to run (0 = iteration-based) */
    loopyStressHarness *harness; /* Parent harness for stats/control */
} loopyStressWorker;

/**
 * Worker function type.
 * Called repeatedly until iterations/duration exhausted or harness stopped.
 *
 * @param worker  Worker context
 * @return 0 to continue, non-zero to stop (error code)
 */
typedef int loopyStressWorkerFn(loopyStressWorker *worker);

/**
 * Setup function called once per worker before iterations start.
 */
typedef void loopyStressSetupFn(loopyStressWorker *worker);

/**
 * Teardown function called once per worker after iterations complete.
 */
typedef void loopyStressTeardownFn(loopyStressWorker *worker);

/**
 * Invariant check function called periodically during stress test.
 * Should return true if invariants hold, false otherwise.
 *
 * @param harness    The harness
 * @param context    Shared context
 * @param errorMsg   Output buffer for error message (if returning false)
 * @param errorLen   Size of error message buffer
 * @return true if all invariants hold
 */
typedef bool loopyStressInvariantFn(loopyStressHarness *harness, void *context,
                                    char *errorMsg, size_t errorLen);

/**
 * Harness configuration.
 */
typedef struct loopyStressConfig {
    uint32_t numWorkers;          /* Number of worker threads (default: 8) */
    uint64_t iterationsPerWorker; /* Iterations per worker (0 = use duration) */
    uint64_t durationMs;          /* Max duration in ms (default: 5000) */
    uint64_t seed;                /* Random seed (0 = use time) */
    uint32_t
        invariantCheckMs; /* How often to check invariants (default: 100) */
    bool verbose;         /* Print progress (default: false) */
    bool stopOnError;     /* Stop all workers on first error */

    /* Callbacks */
    loopyStressWorkerFn *workerFn;       /* Main worker function (required) */
    loopyStressSetupFn *setupFn;         /* Per-worker setup (optional) */
    loopyStressTeardownFn *teardownFn;   /* Per-worker teardown (optional) */
    loopyStressInvariantFn *invariantFn; /* Invariant checker (optional) */

    /* Context */
    void *sharedContext;      /* Shared across all workers */
    size_t workerContextSize; /* Size of per-worker context to allocate */
} loopyStressConfig;

/**
 * Atomic statistics counter IDs.
 */
typedef enum loopyStressStat {
    STRESS_STAT_ITERATIONS = 0,    /* Total iterations completed */
    STRESS_STAT_OPERATIONS = 1,    /* Total operations performed */
    STRESS_STAT_SUCCESSES = 2,     /* Successful operations */
    STRESS_STAT_FAILURES = 3,      /* Failed operations */
    STRESS_STAT_RETRIES = 4,       /* Retried operations */
    STRESS_STAT_BYTES_SENT = 5,    /* Bytes sent/written */
    STRESS_STAT_BYTES_RECV = 6,    /* Bytes received/read */
    STRESS_STAT_MESSAGES_SENT = 7, /* Messages sent */
    STRESS_STAT_MESSAGES_RECV = 8, /* Messages received */
    STRESS_STAT_ACQUIRES = 9,      /* Resource acquires */
    STRESS_STAT_RELEASES = 10,     /* Resource releases */
    STRESS_STAT_LIMIT_HIT = 11,    /* Hit rate/concurrency limit */
    STRESS_STAT_ERRORS = 12,       /* Errors encountered */
    STRESS_STAT_USER1 = 13,        /* User-defined stat 1 */
    STRESS_STAT_USER2 = 14,        /* User-defined stat 2 */
    STRESS_STAT_USER3 = 15,        /* User-defined stat 3 */
    STRESS_STAT_USER4 = 16,        /* User-defined stat 4 */
    STRESS_STAT_USER5 = 17,        /* User-defined stat 5 */
    STRESS_STAT_MAX = 18,
} loopyStressStat;

/**
 * Stress test results.
 */
typedef struct loopyStressResults {
    bool passed;                     /* True if all invariants held */
    uint32_t numWorkers;             /* Number of workers that ran */
    uint64_t totalIterations;        /* Total iterations across all workers */
    uint64_t elapsedMs;              /* Actual elapsed time in ms */
    uint64_t stats[STRESS_STAT_MAX]; /* All collected statistics */
    int errorCode;                   /* First error code (0 if passed) */
    char errorMsg[256];              /* Error message if failed */

    /* Derived metrics */
    double opsPerSecond;   /* Operations per second */
    double bytesPerSecond; /* Bytes per second (if applicable) */
} loopyStressResults;

/* ====================================================================
 * Harness Lifecycle
 * ==================================================================== */

/**
 * Initialize configuration with defaults.
 *
 * @param config  Configuration to initialize
 */
void loopyStressConfigInit(loopyStressConfig *config);

/**
 * Create a new stress test harness.
 *
 * @param config  Configuration (workerFn is required)
 * @return New harness, or NULL on error
 */
loopyStressHarness *loopyStressHarnessNew(const loopyStressConfig *config);

/**
 * Destroy a stress test harness.
 * Must not be called while test is running.
 *
 * @param harness  The harness to destroy
 */
void loopyStressHarnessFree(loopyStressHarness *harness);

/* ====================================================================
 * Test Execution
 * ==================================================================== */

/**
 * Run the stress test synchronously.
 * Spawns worker threads, runs until completion or failure, collects results.
 *
 * @param harness  The harness
 * @param results  Output results (optional, can be NULL)
 * @return true if test passed, false if invariant violated or error
 */
bool loopyStressRun(loopyStressHarness *harness, loopyStressResults *results);

/**
 * Start stress test asynchronously.
 * Use loopyStressWait() or loopyStressStop() to control.
 *
 * @param harness  The harness
 * @return true if started successfully
 */
bool loopyStressStart(loopyStressHarness *harness);

/**
 * Wait for async stress test to complete.
 *
 * @param harness  The harness
 * @param results  Output results (optional)
 * @return true if test passed
 */
bool loopyStressWait(loopyStressHarness *harness, loopyStressResults *results);

/**
 * Stop a running stress test.
 *
 * @param harness  The harness
 */
void loopyStressStop(loopyStressHarness *harness);

/**
 * Check if stress test is currently running.
 *
 * @param harness  The harness
 * @return true if running
 */
bool loopyStressIsRunning(const loopyStressHarness *harness);

/* ====================================================================
 * Statistics (Thread-Safe)
 * ==================================================================== */

/**
 * Increment a statistic atomically.
 *
 * @param harness  The harness
 * @param stat     Statistic to increment
 * @param delta    Amount to add
 */
void loopyStressStatAdd(loopyStressHarness *harness, loopyStressStat stat,
                        uint64_t delta);

/**
 * Get current value of a statistic.
 *
 * @param harness  The harness
 * @param stat     Statistic to read
 * @return Current value
 */
uint64_t loopyStressStatGet(const loopyStressHarness *harness,
                            loopyStressStat stat);

/**
 * Record a maximum value (only updates if new value is larger).
 *
 * @param harness  The harness
 * @param stat     Statistic to update
 * @param value    Value to compare and potentially store
 */
void loopyStressStatMax(loopyStressHarness *harness, loopyStressStat stat,
                        uint64_t value);

/* ====================================================================
 * Worker Utilities
 * ==================================================================== */

/**
 * Check if the worker should stop.
 *
 * @param worker  The worker context
 * @return true if worker should stop
 */
bool loopyStressWorkerShouldStop(const loopyStressWorker *worker);

/**
 * Get next random value using worker's PRNG.
 *
 * @param worker  The worker context
 * @return Random 64-bit value
 */
uint64_t loopyStressWorkerRand(loopyStressWorker *worker);

/**
 * Get random value in range [0, max).
 *
 * @param worker  The worker context
 * @param max     Upper bound (exclusive)
 * @return Random value in range
 */
uint64_t loopyStressWorkerRandRange(loopyStressWorker *worker, uint64_t max);

/**
 * Report an error from within a worker.
 * If stopOnError is set, this will stop all workers.
 *
 * @param worker  The worker context
 * @param code    Error code (non-zero)
 * @param fmt     Error message format string
 * @param ...     Format arguments
 */
void loopyStressWorkerError(loopyStressWorker *worker, int code,
                            const char *fmt, ...);

/* ====================================================================
 * Data Integrity Helpers
 * ==================================================================== */

/**
 * Message with integrity checking.
 */
typedef struct loopyStressMessage {
    uint64_t sequence;   /* Sequence number */
    uint32_t producerId; /* Producer worker ID */
    uint32_t checksum;   /* CRC32 checksum */
    uint64_t timestamp;  /* Timestamp when created */
    size_t payloadLen;   /* Payload length */
    uint8_t payload[];   /* Variable-length payload */
} loopyStressMessage;

/**
 * Allocate a stress test message with payload.
 *
 * @param producerId  Producer worker ID
 * @param sequence    Sequence number
 * @param payloadLen  Payload size
 * @return Allocated message (caller must free)
 */
loopyStressMessage *loopyStressMessageNew(uint32_t producerId,
                                          uint64_t sequence, size_t payloadLen);

/**
 * Fill message payload with deterministic pattern based on sequence.
 *
 * @param msg  The message to fill
 */
void loopyStressMessageFillPayload(loopyStressMessage *msg);

/**
 * Compute and set the checksum.
 *
 * @param msg  The message
 */
void loopyStressMessageSign(loopyStressMessage *msg);

/**
 * Verify message integrity (checksum and payload pattern).
 *
 * @param msg  The message to verify
 * @return true if message is valid
 */
bool loopyStressMessageVerify(const loopyStressMessage *msg);

/**
 * Get total message size including header and payload.
 *
 * @param msg  The message
 * @return Total size in bytes
 */
size_t loopyStressMessageSize(const loopyStressMessage *msg);

/* ====================================================================
 * Sequence Tracker (for verifying no messages lost/duplicated)
 * ==================================================================== */

/**
 * Opaque sequence tracker for verifying ordered delivery.
 */
typedef struct loopyStressSeqTracker loopyStressSeqTracker;

/**
 * Create a new sequence tracker.
 *
 * @param numProducers  Number of producers to track
 * @param allowGaps     Allow gaps in sequences (for unordered channels)
 * @return New tracker, or NULL on error
 */
loopyStressSeqTracker *loopyStressSeqTrackerNew(uint32_t numProducers,
                                                bool allowGaps);

/**
 * Destroy a sequence tracker.
 *
 * @param tracker  The tracker to destroy
 */
void loopyStressSeqTrackerFree(loopyStressSeqTracker *tracker);

/**
 * Record a received sequence number.
 *
 * @param tracker     The tracker
 * @param producerId  Producer that sent the message
 * @param sequence    Sequence number received
 * @return 0 if OK, -1 if duplicate, -2 if out of order (when gaps not allowed)
 */
int loopyStressSeqTrackerRecord(loopyStressSeqTracker *tracker,
                                uint32_t producerId, uint64_t sequence);

/**
 * Get count of messages received from a producer.
 *
 * @param tracker     The tracker
 * @param producerId  Producer ID
 * @return Count of messages received
 */
uint64_t loopyStressSeqTrackerCount(const loopyStressSeqTracker *tracker,
                                    uint32_t producerId);

/**
 * Get highest sequence seen from a producer.
 *
 * @param tracker     The tracker
 * @param producerId  Producer ID
 * @return Highest sequence number
 */
uint64_t loopyStressSeqTrackerHighest(const loopyStressSeqTracker *tracker,
                                      uint32_t producerId);

/**
 * Check if all sequences up to expected count were received.
 *
 * @param tracker        The tracker
 * @param producerId     Producer ID
 * @param expectedCount  Expected number of messages
 * @return true if all received, false if gaps
 */
bool loopyStressSeqTrackerComplete(const loopyStressSeqTracker *tracker,
                                   uint32_t producerId, uint64_t expectedCount);

/* ====================================================================
 * Concurrent Counter (for limit verification)
 * ==================================================================== */

/**
 * Atomic counter with max tracking.
 */
typedef struct loopyStressCounter {
    _Atomic(int64_t) value;       /* Current value */
    _Atomic(int64_t) max;         /* Maximum value seen */
    _Atomic(uint64_t) increments; /* Total increments */
    _Atomic(uint64_t) decrements; /* Total decrements */
} loopyStressCounter;

/**
 * Initialize a counter.
 *
 * @param counter  The counter to initialize
 */
void loopyStressCounterInit(loopyStressCounter *counter);

/**
 * Increment and return new value.
 *
 * @param counter  The counter
 * @param delta    Amount to add
 * @return New value after increment
 */
int64_t loopyStressCounterAdd(loopyStressCounter *counter, int64_t delta);

/**
 * Decrement and return new value.
 *
 * @param counter  The counter
 * @param delta    Amount to subtract
 * @return New value after decrement
 */
int64_t loopyStressCounterSub(loopyStressCounter *counter, int64_t delta);

/**
 * Get current value.
 *
 * @param counter  The counter
 * @return Current value
 */
int64_t loopyStressCounterGet(const loopyStressCounter *counter);

/**
 * Get maximum value ever seen.
 *
 * @param counter  The counter
 * @return Maximum value
 */
int64_t loopyStressCounterGetMax(const loopyStressCounter *counter);

/**
 * Check if value ever exceeded a limit.
 *
 * @param counter  The counter
 * @param limit    Limit to check against
 * @return true if max ever exceeded limit
 */
bool loopyStressCounterExceeded(const loopyStressCounter *counter,
                                int64_t limit);

/* ====================================================================
 * Timing Utilities
 * ==================================================================== */

/**
 * Get current time in nanoseconds.
 *
 * @return Monotonic nanoseconds
 */
uint64_t loopyStressTimeNs(void);

/**
 * Get current time in milliseconds.
 *
 * @return Monotonic milliseconds
 */
uint64_t loopyStressTimeMs(void);

/**
 * Sleep for specified milliseconds.
 *
 * @param ms  Milliseconds to sleep
 */
void loopyStressSleepMs(uint64_t ms);

#endif /* LOOPY_STRESS_TEST_H */
