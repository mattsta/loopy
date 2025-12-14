/* loopyStressTest - Implementation
 *
 * Copyright 2024 Matt Stancliff <matt@genges.com>
 * Licensed under the Apache License, Version 2.0
 */

#include "loopyPlatform.h"

#include "loopyStressTest.h"

#include "../deps/datakit/src/datakit.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ====================================================================
 * Portable Barrier Implementation (for macOS which lacks pthread_barrier)
 * ==================================================================== */

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int count;
    int tripCount;
    int generation;
} portable_barrier_t;

static int portable_barrier_init(portable_barrier_t *barrier, int count) {
    barrier->count = count;
    barrier->tripCount = 0;
    barrier->generation = 0;
    pthread_mutex_init(&barrier->mutex, NULL);
    pthread_cond_init(&barrier->cond, NULL);
    return 0;
}

static int portable_barrier_destroy(portable_barrier_t *barrier) {
    pthread_mutex_destroy(&barrier->mutex);
    pthread_cond_destroy(&barrier->cond);
    return 0;
}

static int portable_barrier_wait(portable_barrier_t *barrier) {
    pthread_mutex_lock(&barrier->mutex);
    int gen = barrier->generation;
    barrier->tripCount++;

    if (barrier->tripCount >= barrier->count) {
        barrier->tripCount = 0;
        barrier->generation++;
        pthread_cond_broadcast(&barrier->cond);
        pthread_mutex_unlock(&barrier->mutex);
        return 1; /* This thread is the "serial" thread */
    }

    while (gen == barrier->generation) {
        pthread_cond_wait(&barrier->cond, &barrier->mutex);
    }

    pthread_mutex_unlock(&barrier->mutex);
    return 0;
}

/* ====================================================================
 * Internal Structures
 * ==================================================================== */

struct loopyStressHarness {
    loopyStressConfig config;

    /* Workers */
    pthread_t *threads;
    loopyStressWorker *workers;
    void *workerContexts; /* Contiguous allocation for worker contexts */

    /* Synchronization */
    portable_barrier_t startBarrier;
    pthread_mutex_t mutex;
    _Atomic(bool) running;
    _Atomic(bool) stopRequested;
    _Atomic(bool) errorOccurred;

    /* Statistics */
    _Atomic(uint64_t) stats[STRESS_STAT_MAX];

    /* Results */
    int firstErrorCode;
    char firstErrorMsg[256];

    /* Timing */
    uint64_t startTimeMs;
    uint64_t endTimeMs;

    /* Invariant checker state */
    pthread_t invariantThread;
    bool invariantThreadRunning;
};

struct loopyStressSeqTracker {
    uint32_t numProducers;
    bool allowGaps;
    _Atomic(uint64_t) *counts;     /* Messages received per producer */
    _Atomic(uint64_t) *highest;    /* Highest sequence per producer */
    _Atomic(uint64_t) *duplicates; /* Duplicate count per producer */
};

/* ====================================================================
 * Time Utilities
 * ==================================================================== */

uint64_t loopyStressTimeNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

uint64_t loopyStressTimeMs(void) {
    return loopyStressTimeNs() / 1000000ULL;
}

void loopyStressSleepMs(uint64_t ms) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)((ms % 1000) * 1000000);
    nanosleep(&ts, NULL);
}

/* ====================================================================
 * Random Number Generator (xorshift64*)
 * ==================================================================== */

static uint64_t xorshift64star(uint64_t *state) {
    uint64_t x = *state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

uint64_t loopyStressWorkerRand(loopyStressWorker *worker) {
    return xorshift64star(&worker->seed);
}

uint64_t loopyStressWorkerRandRange(loopyStressWorker *worker, uint64_t max) {
    if (max == 0) {
        return 0;
    }
    return loopyStressWorkerRand(worker) % max;
}

/* ====================================================================
 * CRC32 (for message integrity)
 * ==================================================================== */

static uint32_t crc32_table[256];
static bool crc32_initialized = false;

static void crc32_init(void) {
    if (crc32_initialized) {
        return;
    }

    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) {
            c = (c >> 1) ^ ((c & 1) ? 0xEDB88320 : 0);
        }
        crc32_table[i] = c;
    }
    crc32_initialized = true;
}

static uint32_t crc32_compute(const void *data, size_t len) {
    crc32_init();

    const uint8_t *buf = data;
    uint32_t crc = 0xFFFFFFFF;

    for (size_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    }

    return crc ^ 0xFFFFFFFF;
}

/* ====================================================================
 * Configuration
 * ==================================================================== */

void loopyStressConfigInit(loopyStressConfig *config) {
    if (!config) {
        return;
    }

    memset(config, 0, sizeof(*config));
    config->numWorkers = 8;
    config->iterationsPerWorker = 0;
    config->durationMs = 5000;
    config->seed = 0;
    config->invariantCheckMs = 100;
    config->verbose = false;
    config->stopOnError = true;
}

/* ====================================================================
 * Harness Lifecycle
 * ==================================================================== */

loopyStressHarness *loopyStressHarnessNew(const loopyStressConfig *config) {
    if (!config || !config->workerFn) {
        return NULL;
    }
    if (config->numWorkers == 0) {
        return NULL;
    }

    loopyStressHarness *harness = zcalloc(1, sizeof(*harness));
    if (!harness) {
        return NULL;
    }

    memcpy(&harness->config, config, sizeof(*config));

    /* Use time-based seed if not provided */
    if (harness->config.seed == 0) {
        harness->config.seed = loopyStressTimeNs();
    }

    /* Allocate threads */
    harness->threads = zcalloc(config->numWorkers, sizeof(pthread_t));
    if (!harness->threads) {
        zfree(harness);
        return NULL;
    }

    /* Allocate workers */
    harness->workers = zcalloc(config->numWorkers, sizeof(loopyStressWorker));
    if (!harness->workers) {
        zfree(harness->threads);
        zfree(harness);
        return NULL;
    }

    /* Allocate per-worker contexts if requested */
    if (config->workerContextSize > 0) {
        harness->workerContexts =
            zcalloc(config->numWorkers, config->workerContextSize);
        if (!harness->workerContexts) {
            zfree(harness->workers);
            zfree(harness->threads);
            zfree(harness);
            return NULL;
        }
    }

    /* Initialize synchronization */
    portable_barrier_init(&harness->startBarrier, config->numWorkers + 1);
    pthread_mutex_init(&harness->mutex, NULL);

    atomic_store(&harness->running, false);
    atomic_store(&harness->stopRequested, false);
    atomic_store(&harness->errorOccurred, false);

    /* Initialize statistics */
    for (int i = 0; i < STRESS_STAT_MAX; i++) {
        atomic_store(&harness->stats[i], 0);
    }

    /* Initialize workers */
    for (uint32_t i = 0; i < config->numWorkers; i++) {
        harness->workers[i].workerId = i;
        harness->workers[i].numWorkers = config->numWorkers;
        harness->workers[i].sharedContext = config->sharedContext;
        harness->workers[i].iterations = config->iterationsPerWorker;
        harness->workers[i].durationMs = config->durationMs;
        harness->workers[i].harness = harness;

        /* Per-worker seed derived from main seed + worker ID */
        harness->workers[i].seed =
            harness->config.seed ^ ((uint64_t)(i + 1) * 0x9E3779B97F4A7C15ULL);

        /* Per-worker context */
        if (config->workerContextSize > 0) {
            harness->workers[i].workerContext =
                (char *)harness->workerContexts +
                (i * config->workerContextSize);
        }
    }

    return harness;
}

void loopyStressHarnessFree(loopyStressHarness *harness) {
    if (!harness) {
        return;
    }

    if (atomic_load(&harness->running)) {
        loopyStressStop(harness);
        loopyStressWait(harness, NULL);
    }

    portable_barrier_destroy(&harness->startBarrier);
    pthread_mutex_destroy(&harness->mutex);

    zfree(harness->workerContexts);
    zfree(harness->workers);
    zfree(harness->threads);
    zfree(harness);
}

/* ====================================================================
 * Worker Thread
 * ==================================================================== */

static void *worker_thread(void *arg) {
    loopyStressWorker *worker = arg;
    loopyStressHarness *harness = worker->harness;

    /* Call setup if provided */
    if (harness->config.setupFn) {
        harness->config.setupFn(worker);
    }

    /* Wait for all workers to be ready */
    portable_barrier_wait(&harness->startBarrier);

    uint64_t startMs = loopyStressTimeMs();
    uint64_t iterations = 0;

    /* Main work loop */
    while (!loopyStressWorkerShouldStop(worker)) {
        /* Check duration limit */
        if (worker->durationMs > 0) {
            uint64_t elapsed = loopyStressTimeMs() - startMs;
            if (elapsed >= worker->durationMs) {
                break;
            }
        }

        /* Check iteration limit */
        if (worker->iterations > 0 && iterations >= worker->iterations) {
            break;
        }

        /* Call worker function */
        int result = harness->config.workerFn(worker);
        if (result != 0) {
            loopyStressWorkerError(worker, result,
                                   "worker function returned %d", result);
            if (harness->config.stopOnError) {
                break;
            }
        }

        iterations++;
    }

    /* Record iterations */
    loopyStressStatAdd(harness, STRESS_STAT_ITERATIONS, iterations);

    /* Call teardown if provided */
    if (harness->config.teardownFn) {
        harness->config.teardownFn(worker);
    }

    return NULL;
}

/* ====================================================================
 * Invariant Checker Thread
 * ==================================================================== */

static void *invariant_thread(void *arg) {
    loopyStressHarness *harness = arg;
    char errorMsg[256];

    while (atomic_load(&harness->running) &&
           !atomic_load(&harness->stopRequested)) {
        loopyStressSleepMs(harness->config.invariantCheckMs);

        if (!atomic_load(&harness->running)) {
            break;
        }

        if (harness->config.invariantFn) {
            bool ok = harness->config.invariantFn(harness,
                                                  harness->config.sharedContext,
                                                  errorMsg, sizeof(errorMsg));
            if (!ok) {
                pthread_mutex_lock(&harness->mutex);
                if (!atomic_load(&harness->errorOccurred)) {
                    atomic_store(&harness->errorOccurred, true);
                    harness->firstErrorCode = -1;
                    strncpy(harness->firstErrorMsg, errorMsg,
                            sizeof(harness->firstErrorMsg) - 1);
                }
                pthread_mutex_unlock(&harness->mutex);

                if (harness->config.stopOnError) {
                    atomic_store(&harness->stopRequested, true);
                }
            }
        }
    }

    return NULL;
}

/* ====================================================================
 * Test Execution
 * ==================================================================== */

bool loopyStressStart(loopyStressHarness *harness) {
    if (!harness) {
        return false;
    }
    if (atomic_load(&harness->running)) {
        return false;
    }

    /* Reset state */
    atomic_store(&harness->running, true);
    atomic_store(&harness->stopRequested, false);
    atomic_store(&harness->errorOccurred, false);
    harness->firstErrorCode = 0;
    harness->firstErrorMsg[0] = '\0';

    for (int i = 0; i < STRESS_STAT_MAX; i++) {
        atomic_store(&harness->stats[i], 0);
    }

    harness->startTimeMs = loopyStressTimeMs();

    /* Start workers */
    for (uint32_t i = 0; i < harness->config.numWorkers; i++) {
        pthread_create(&harness->threads[i], NULL, worker_thread,
                       &harness->workers[i]);
    }

    /* Start invariant checker if provided */
    if (harness->config.invariantFn) {
        harness->invariantThreadRunning = true;
        pthread_create(&harness->invariantThread, NULL, invariant_thread,
                       harness);
    }

    /* Release workers (main thread participates in barrier) */
    portable_barrier_wait(&harness->startBarrier);

    return true;
}

bool loopyStressWait(loopyStressHarness *harness, loopyStressResults *results) {
    if (!harness || !atomic_load(&harness->running)) {
        return false;
    }

    /* Wait for all workers */
    for (uint32_t i = 0; i < harness->config.numWorkers; i++) {
        pthread_join(harness->threads[i], NULL);
    }

    atomic_store(&harness->running, false);
    harness->endTimeMs = loopyStressTimeMs();

    /* Wait for invariant checker */
    if (harness->invariantThreadRunning) {
        pthread_join(harness->invariantThread, NULL);
        harness->invariantThreadRunning = false;
    }

    /* Populate results */
    if (results) {
        memset(results, 0, sizeof(*results));

        results->passed = !atomic_load(&harness->errorOccurred);
        results->numWorkers = harness->config.numWorkers;
        results->elapsedMs = harness->endTimeMs - harness->startTimeMs;
        results->errorCode = harness->firstErrorCode;
        strncpy(results->errorMsg, harness->firstErrorMsg,
                sizeof(results->errorMsg) - 1);

        for (int i = 0; i < STRESS_STAT_MAX; i++) {
            results->stats[i] = atomic_load(&harness->stats[i]);
        }

        results->totalIterations = results->stats[STRESS_STAT_ITERATIONS];

        /* Derived metrics */
        if (results->elapsedMs > 0) {
            results->opsPerSecond =
                (double)results->stats[STRESS_STAT_OPERATIONS] * 1000.0 /
                (double)results->elapsedMs;
            results->bytesPerSecond =
                (double)(results->stats[STRESS_STAT_BYTES_SENT] +
                         results->stats[STRESS_STAT_BYTES_RECV]) *
                1000.0 / (double)results->elapsedMs;
        }
    }

    return !atomic_load(&harness->errorOccurred);
}

void loopyStressStop(loopyStressHarness *harness) {
    if (!harness) {
        return;
    }
    atomic_store(&harness->stopRequested, true);
}

bool loopyStressRun(loopyStressHarness *harness, loopyStressResults *results) {
    if (!loopyStressStart(harness)) {
        return false;
    }
    return loopyStressWait(harness, results);
}

bool loopyStressIsRunning(const loopyStressHarness *harness) {
    if (!harness) {
        return false;
    }
    return atomic_load(&harness->running);
}

/* ====================================================================
 * Statistics
 * ==================================================================== */

void loopyStressStatAdd(loopyStressHarness *harness, loopyStressStat stat,
                        uint64_t delta) {
    if (!harness || stat >= STRESS_STAT_MAX) {
        return;
    }
    atomic_fetch_add(&harness->stats[stat], delta);
}

uint64_t loopyStressStatGet(const loopyStressHarness *harness,
                            loopyStressStat stat) {
    if (!harness || stat >= STRESS_STAT_MAX) {
        return 0;
    }
    return atomic_load(&harness->stats[stat]);
}

void loopyStressStatMax(loopyStressHarness *harness, loopyStressStat stat,
                        uint64_t value) {
    if (!harness || stat >= STRESS_STAT_MAX) {
        return;
    }

    uint64_t current;
    do {
        current = atomic_load(&harness->stats[stat]);
        if (value <= current) {
            return;
        }
    } while (
        !atomic_compare_exchange_weak(&harness->stats[stat], &current, value));
}

/* ====================================================================
 * Worker Utilities
 * ==================================================================== */

bool loopyStressWorkerShouldStop(const loopyStressWorker *worker) {
    if (!worker || !worker->harness) {
        return true;
    }
    return atomic_load(&worker->harness->stopRequested);
}

void loopyStressWorkerError(loopyStressWorker *worker, int code,
                            const char *fmt, ...) {
    if (!worker || !worker->harness) {
        return;
    }

    loopyStressHarness *harness = worker->harness;

    pthread_mutex_lock(&harness->mutex);

    /* Only record first error */
    if (!atomic_load(&harness->errorOccurred)) {
        atomic_store(&harness->errorOccurred, true);
        harness->firstErrorCode = code;

        va_list ap;
        va_start(ap, fmt);
        vsnprintf(harness->firstErrorMsg, sizeof(harness->firstErrorMsg), fmt,
                  ap);
        va_end(ap);
    }

    pthread_mutex_unlock(&harness->mutex);

    loopyStressStatAdd(harness, STRESS_STAT_ERRORS, 1);

    if (harness->config.stopOnError) {
        atomic_store(&harness->stopRequested, true);
    }
}

/* ====================================================================
 * Message Integrity
 * ==================================================================== */

loopyStressMessage *loopyStressMessageNew(uint32_t producerId,
                                          uint64_t sequence,
                                          size_t payloadLen) {
    size_t totalSize = sizeof(loopyStressMessage) + payloadLen;
    loopyStressMessage *msg = zmalloc(totalSize);
    if (!msg) {
        return NULL;
    }

    msg->producerId = producerId;
    msg->sequence = sequence;
    msg->checksum = 0;
    msg->timestamp = loopyStressTimeNs();
    msg->payloadLen = payloadLen;

    return msg;
}

void loopyStressMessageFillPayload(loopyStressMessage *msg) {
    if (!msg) {
        return;
    }

    /* Fill with deterministic pattern based on sequence */
    uint64_t seed = msg->sequence;
    for (size_t i = 0; i < msg->payloadLen; i++) {
        msg->payload[i] = (uint8_t)(xorshift64star(&seed) & 0xFF);
    }
}

/* Header size (everything except the flexible array member) */
#define STRESS_MSG_HEADER_SIZE offsetof(loopyStressMessage, payload)

void loopyStressMessageSign(loopyStressMessage *msg) {
    if (!msg) {
        return;
    }

    /* Compute checksum over everything except checksum field */
    msg->checksum = 0;
    uint32_t crc = crc32_compute(msg, STRESS_MSG_HEADER_SIZE);
    crc = crc32_compute(msg->payload, msg->payloadLen) ^ crc;
    msg->checksum = crc;
}

bool loopyStressMessageVerify(const loopyStressMessage *msg) {
    if (!msg) {
        return false;
    }

    /* Verify checksum */
    size_t totalSize = STRESS_MSG_HEADER_SIZE + msg->payloadLen;
    loopyStressMessage *tmp = zmalloc(totalSize);
    if (!tmp) {
        return false;
    }

    memcpy(tmp, msg, totalSize);
    tmp->checksum = 0;

    uint32_t expectedCrc = crc32_compute(tmp, STRESS_MSG_HEADER_SIZE);
    expectedCrc = crc32_compute(tmp->payload, tmp->payloadLen) ^ expectedCrc;
    zfree(tmp);

    if (expectedCrc != msg->checksum) {
        return false;
    }

    /* Verify payload pattern */
    uint64_t seed = msg->sequence;
    for (size_t i = 0; i < msg->payloadLen; i++) {
        uint8_t expected = (uint8_t)(xorshift64star(&seed) & 0xFF);
        if (msg->payload[i] != expected) {
            return false;
        }
    }

    return true;
}

size_t loopyStressMessageSize(const loopyStressMessage *msg) {
    if (!msg) {
        return 0;
    }
    return sizeof(*msg) + msg->payloadLen;
}

/* ====================================================================
 * Sequence Tracker
 * ==================================================================== */

loopyStressSeqTracker *loopyStressSeqTrackerNew(uint32_t numProducers,
                                                bool allowGaps) {
    if (numProducers == 0) {
        return NULL;
    }

    loopyStressSeqTracker *tracker = zcalloc(1, sizeof(*tracker));
    if (!tracker) {
        return NULL;
    }

    tracker->numProducers = numProducers;
    tracker->allowGaps = allowGaps;

    tracker->counts = zcalloc(numProducers, sizeof(_Atomic(uint64_t)));
    tracker->highest = zcalloc(numProducers, sizeof(_Atomic(uint64_t)));
    tracker->duplicates = zcalloc(numProducers, sizeof(_Atomic(uint64_t)));

    if (!tracker->counts || !tracker->highest || !tracker->duplicates) {
        zfree(tracker->counts);
        zfree(tracker->highest);
        zfree(tracker->duplicates);
        zfree(tracker);
        return NULL;
    }

    return tracker;
}

void loopyStressSeqTrackerFree(loopyStressSeqTracker *tracker) {
    if (!tracker) {
        return;
    }
    zfree(tracker->counts);
    zfree(tracker->highest);
    zfree(tracker->duplicates);
    zfree(tracker);
}

int loopyStressSeqTrackerRecord(loopyStressSeqTracker *tracker,
                                uint32_t producerId, uint64_t sequence) {
    if (!tracker || producerId >= tracker->numProducers) {
        return -1;
    }

    /* Update highest seen */
    uint64_t prevHighest;
    do {
        prevHighest = atomic_load(&tracker->highest[producerId]);
        if (sequence <= prevHighest && prevHighest != 0) {
            /* Potentially duplicate or out of order */
            if (sequence < prevHighest && !tracker->allowGaps) {
                return -2; /* Out of order */
            }
            /* Could be duplicate - for now we allow retransmits in allowGaps
             * mode */
        }
    } while (sequence > prevHighest &&
             !atomic_compare_exchange_weak(&tracker->highest[producerId],
                                           &prevHighest, sequence));

    /* Increment count */
    atomic_fetch_add(&tracker->counts[producerId], 1);

    return 0;
}

uint64_t loopyStressSeqTrackerCount(const loopyStressSeqTracker *tracker,
                                    uint32_t producerId) {
    if (!tracker || producerId >= tracker->numProducers) {
        return 0;
    }
    return atomic_load(&tracker->counts[producerId]);
}

uint64_t loopyStressSeqTrackerHighest(const loopyStressSeqTracker *tracker,
                                      uint32_t producerId) {
    if (!tracker || producerId >= tracker->numProducers) {
        return 0;
    }
    return atomic_load(&tracker->highest[producerId]);
}

bool loopyStressSeqTrackerComplete(const loopyStressSeqTracker *tracker,
                                   uint32_t producerId,
                                   uint64_t expectedCount) {
    if (!tracker || producerId >= tracker->numProducers) {
        return false;
    }
    return atomic_load(&tracker->counts[producerId]) >= expectedCount;
}

/* ====================================================================
 * Concurrent Counter
 * ==================================================================== */

void loopyStressCounterInit(loopyStressCounter *counter) {
    if (!counter) {
        return;
    }
    atomic_store(&counter->value, 0);
    atomic_store(&counter->max, 0);
    atomic_store(&counter->increments, 0);
    atomic_store(&counter->decrements, 0);
}

int64_t loopyStressCounterAdd(loopyStressCounter *counter, int64_t delta) {
    if (!counter) {
        return 0;
    }

    int64_t newVal = atomic_fetch_add(&counter->value, delta) + delta;
    atomic_fetch_add(&counter->increments, 1);

    /* Update max */
    int64_t currentMax;
    do {
        currentMax = atomic_load(&counter->max);
        if (newVal <= currentMax) {
            break;
        }
    } while (!atomic_compare_exchange_weak(&counter->max, &currentMax, newVal));

    return newVal;
}

int64_t loopyStressCounterSub(loopyStressCounter *counter, int64_t delta) {
    if (!counter) {
        return 0;
    }
    atomic_fetch_add(&counter->decrements, 1);
    return atomic_fetch_sub(&counter->value, delta) - delta;
}

int64_t loopyStressCounterGet(const loopyStressCounter *counter) {
    if (!counter) {
        return 0;
    }
    return atomic_load(&counter->value);
}

int64_t loopyStressCounterGetMax(const loopyStressCounter *counter) {
    if (!counter) {
        return 0;
    }
    return atomic_load(&counter->max);
}

bool loopyStressCounterExceeded(const loopyStressCounter *counter,
                                int64_t limit) {
    if (!counter) {
        return false;
    }
    return atomic_load(&counter->max) > limit;
}
