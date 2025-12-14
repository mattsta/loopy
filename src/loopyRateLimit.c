/* loopyRateLimit - Rate limiting and concurrency control
 *
 * Copyright 2024 Matt Stancliff <matt@genges.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 */

#include "loopyPlatform.h"

#include "loopyAsync.h"
#include "loopyRateLimit.h"

#include "../deps/datakit/src/datakit.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

/* ====================================================================
 * Constants
 * ==================================================================== */

#define SLIDING_WINDOW_DEFAULT_MAX_ENTRIES 10000
#define MICROSECONDS_PER_SECOND 1000000.0
#define MILLISECONDS_PER_SECOND 1000.0

/* ====================================================================
 * Time Utilities
 * ==================================================================== */

static uint64_t getNowUs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static uint64_t getNowMs(void) {
    return getNowUs() / 1000;
}

/* ====================================================================
 * Token Bucket Implementation
 * ==================================================================== */

typedef struct loopyTokenBucket {
    double tokens;
    double capacity;
    double refillRate;
    double tokensPerRequest;
    uint64_t lastRefillUs;
    pthread_mutex_t mutex;
} loopyTokenBucket;

static void tokenBucketInit(loopyTokenBucket *tb,
                            const loopyTokenBucketConfig *config) {
    tb->capacity = config->capacity;
    tb->refillRate = config->refillRate;
    tb->tokensPerRequest =
        config->tokensPerRequest > 0 ? config->tokensPerRequest : 1.0;
    tb->tokens =
        config->initialTokens >= 0 ? config->initialTokens : config->capacity;
    tb->lastRefillUs = getNowUs();
    pthread_mutex_init(&tb->mutex, NULL);
}

static void tokenBucketDestroy(loopyTokenBucket *tb) {
    pthread_mutex_destroy(&tb->mutex);
}

static void tokenBucketRefill(loopyTokenBucket *tb) {
    uint64_t now = getNowUs();
    double elapsedSec = (now - tb->lastRefillUs) / MICROSECONDS_PER_SECOND;
    double newTokens = elapsedSec * tb->refillRate;

    tb->tokens = fmin(tb->capacity, tb->tokens + newTokens);
    tb->lastRefillUs = now;
}

static loopyRateLimitResult tokenBucketCheck(loopyTokenBucket *tb, double cost,
                                             loopyRateLimitInfo *info) {
    pthread_mutex_lock(&tb->mutex);

    tokenBucketRefill(tb);

    double actualCost = cost > 0 ? cost : tb->tokensPerRequest;
    bool allowed = tb->tokens >= actualCost;

    if (info) {
        info->allowed = allowed;
        info->remaining = tb->tokens;
        info->limit = tb->capacity;
        if (!allowed && tb->refillRate > 0) {
            double needed = actualCost - tb->tokens;
            info->retryAfterMs =
                (uint64_t)(needed / tb->refillRate * MILLISECONDS_PER_SECOND);
        } else {
            info->retryAfterMs = 0;
        }
        info->resetMs = 0; /* Token bucket doesn't have reset */
    }

    if (allowed) {
        tb->tokens -= actualCost;
    }

    pthread_mutex_unlock(&tb->mutex);

    return allowed ? LOOPY_RATE_LIMIT_ALLOWED : LOOPY_RATE_LIMIT_DENIED;
}

static loopyRateLimitResult tokenBucketPeek(loopyTokenBucket *tb,
                                            loopyRateLimitInfo *info) {
    pthread_mutex_lock(&tb->mutex);

    tokenBucketRefill(tb);

    if (info) {
        info->allowed = tb->tokens >= tb->tokensPerRequest;
        info->remaining = tb->tokens;
        info->limit = tb->capacity;
        if (!info->allowed && tb->refillRate > 0) {
            double needed = tb->tokensPerRequest - tb->tokens;
            info->retryAfterMs =
                (uint64_t)(needed / tb->refillRate * MILLISECONDS_PER_SECOND);
        } else {
            info->retryAfterMs = 0;
        }
        info->resetMs = 0;
    }

    loopyRateLimitResult result = (tb->tokens >= tb->tokensPerRequest)
                                      ? LOOPY_RATE_LIMIT_ALLOWED
                                      : LOOPY_RATE_LIMIT_DENIED;

    pthread_mutex_unlock(&tb->mutex);
    return result;
}

static void tokenBucketReset(loopyTokenBucket *tb) {
    pthread_mutex_lock(&tb->mutex);
    tb->tokens = tb->capacity;
    tb->lastRefillUs = getNowUs();
    pthread_mutex_unlock(&tb->mutex);
}

/* ====================================================================
 * Sliding Window Implementation
 * ==================================================================== */

typedef struct loopySlidingWindowEntry {
    uint64_t timestampMs;
} loopySlidingWindowEntry;

typedef struct loopySlidingWindow {
    loopySlidingWindowEntry *entries;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
    uint64_t windowMs;
    uint64_t maxRequests;
    pthread_mutex_t mutex;
} loopySlidingWindow;

static void slidingWindowInit(loopySlidingWindow *sw,
                              const loopySlidingWindowConfig *config) {
    sw->capacity = config->maxEntries > 0 ? config->maxEntries
                                          : SLIDING_WINDOW_DEFAULT_MAX_ENTRIES;
    sw->entries = zcalloc(sw->capacity, sizeof(loopySlidingWindowEntry));
    sw->head = 0;
    sw->tail = 0;
    sw->count = 0;
    sw->windowMs = config->windowMs;
    sw->maxRequests = config->maxRequests;
    pthread_mutex_init(&sw->mutex, NULL);
}

static void slidingWindowDestroy(loopySlidingWindow *sw) {
    pthread_mutex_destroy(&sw->mutex);
    zfree(sw->entries);
}

static void slidingWindowPrune(loopySlidingWindow *sw, uint64_t now) {
    uint64_t cutoff = (now > sw->windowMs) ? now - sw->windowMs : 0;

    while (sw->count > 0) {
        loopySlidingWindowEntry *oldest = &sw->entries[sw->tail];
        if (oldest->timestampMs >= cutoff) {
            break;
        }
        sw->tail = (sw->tail + 1) % sw->capacity;
        sw->count--;
    }
}

static loopyRateLimitResult slidingWindowCheck(loopySlidingWindow *sw,
                                               loopyRateLimitInfo *info) {
    pthread_mutex_lock(&sw->mutex);

    uint64_t now = getNowMs();
    slidingWindowPrune(sw, now);

    bool allowed = sw->count < sw->maxRequests && sw->count < sw->capacity;

    if (info) {
        info->allowed = allowed;
        info->remaining =
            (sw->maxRequests > sw->count) ? sw->maxRequests - sw->count : 0;
        info->limit = (double)sw->maxRequests;

        if (!allowed && sw->count > 0) {
            /* Find oldest entry to determine when we'll have capacity */
            loopySlidingWindowEntry *oldest = &sw->entries[sw->tail];
            uint64_t oldestExpiry = oldest->timestampMs + sw->windowMs;
            info->retryAfterMs = (oldestExpiry > now) ? oldestExpiry - now : 0;
        } else {
            info->retryAfterMs = 0;
        }

        /* Reset is when the current window ends */
        if (sw->count > 0) {
            loopySlidingWindowEntry *newest =
                &sw->entries[(sw->head + sw->capacity - 1) % sw->capacity];
            info->resetMs = newest->timestampMs + sw->windowMs - now;
        } else {
            info->resetMs = sw->windowMs;
        }
    }

    if (allowed) {
        sw->entries[sw->head].timestampMs = now;
        sw->head = (sw->head + 1) % sw->capacity;
        sw->count++;
    }

    pthread_mutex_unlock(&sw->mutex);

    return allowed ? LOOPY_RATE_LIMIT_ALLOWED : LOOPY_RATE_LIMIT_DENIED;
}

static loopyRateLimitResult slidingWindowPeek(loopySlidingWindow *sw,
                                              loopyRateLimitInfo *info) {
    pthread_mutex_lock(&sw->mutex);

    uint64_t now = getNowMs();
    slidingWindowPrune(sw, now);

    if (info) {
        info->allowed = sw->count < sw->maxRequests;
        info->remaining =
            (sw->maxRequests > sw->count) ? sw->maxRequests - sw->count : 0;
        info->limit = (double)sw->maxRequests;

        if (!info->allowed && sw->count > 0) {
            loopySlidingWindowEntry *oldest = &sw->entries[sw->tail];
            uint64_t oldestExpiry = oldest->timestampMs + sw->windowMs;
            info->retryAfterMs = (oldestExpiry > now) ? oldestExpiry - now : 0;
        } else {
            info->retryAfterMs = 0;
        }
        info->resetMs = sw->windowMs;
    }

    loopyRateLimitResult result = (sw->count < sw->maxRequests)
                                      ? LOOPY_RATE_LIMIT_ALLOWED
                                      : LOOPY_RATE_LIMIT_DENIED;

    pthread_mutex_unlock(&sw->mutex);
    return result;
}

static void slidingWindowReset(loopySlidingWindow *sw) {
    pthread_mutex_lock(&sw->mutex);
    sw->head = 0;
    sw->tail = 0;
    sw->count = 0;
    pthread_mutex_unlock(&sw->mutex);
}

/* ====================================================================
 * Leaky Bucket Implementation
 * ==================================================================== */

typedef struct loopyLeakyBucket {
    size_t queueSize;
    size_t queueCapacity;
    double drainRate;
    uint64_t lastDrainUs;
    double partialDrain; /* Fractional drain accumulator */
    pthread_mutex_t mutex;
} loopyLeakyBucket;

static void leakyBucketInit(loopyLeakyBucket *lb,
                            const loopyLeakyBucketConfig *config) {
    lb->queueSize = 0;
    lb->queueCapacity = config->queueCapacity;
    lb->drainRate = config->drainRate;
    lb->lastDrainUs = getNowUs();
    lb->partialDrain = 0;
    pthread_mutex_init(&lb->mutex, NULL);
}

static void leakyBucketDestroy(loopyLeakyBucket *lb) {
    pthread_mutex_destroy(&lb->mutex);
}

static void leakyBucketDrain(loopyLeakyBucket *lb) {
    uint64_t now = getNowUs();
    double elapsedSec = (now - lb->lastDrainUs) / MICROSECONDS_PER_SECOND;
    double drainAmount = elapsedSec * lb->drainRate + lb->partialDrain;

    size_t wholeDrain = (size_t)drainAmount;
    lb->partialDrain = drainAmount - wholeDrain;

    if (wholeDrain > lb->queueSize) {
        lb->queueSize = 0;
    } else {
        lb->queueSize -= wholeDrain;
    }

    lb->lastDrainUs = now;
}

static loopyRateLimitResult leakyBucketCheck(loopyLeakyBucket *lb,
                                             loopyRateLimitInfo *info) {
    pthread_mutex_lock(&lb->mutex);

    leakyBucketDrain(lb);

    bool allowed = lb->queueSize < lb->queueCapacity;

    if (info) {
        info->allowed = allowed;
        info->remaining = (double)(lb->queueCapacity - lb->queueSize);
        info->limit = (double)lb->queueCapacity;
        if (!allowed && lb->drainRate > 0) {
            info->retryAfterMs =
                (uint64_t)(1.0 / lb->drainRate * MILLISECONDS_PER_SECOND);
        } else {
            info->retryAfterMs = 0;
        }
        if (lb->queueSize > 0 && lb->drainRate > 0) {
            info->resetMs = (uint64_t)(lb->queueSize / lb->drainRate *
                                       MILLISECONDS_PER_SECOND);
        } else {
            info->resetMs = 0;
        }
    }

    if (allowed) {
        lb->queueSize++;
    }

    pthread_mutex_unlock(&lb->mutex);

    return allowed ? LOOPY_RATE_LIMIT_ALLOWED : LOOPY_RATE_LIMIT_DENIED;
}

static loopyRateLimitResult leakyBucketPeek(loopyLeakyBucket *lb,
                                            loopyRateLimitInfo *info) {
    pthread_mutex_lock(&lb->mutex);

    leakyBucketDrain(lb);

    if (info) {
        info->allowed = lb->queueSize < lb->queueCapacity;
        info->remaining = (double)(lb->queueCapacity - lb->queueSize);
        info->limit = (double)lb->queueCapacity;
        if (!info->allowed && lb->drainRate > 0) {
            info->retryAfterMs =
                (uint64_t)(1.0 / lb->drainRate * MILLISECONDS_PER_SECOND);
        } else {
            info->retryAfterMs = 0;
        }
        info->resetMs = 0;
    }

    loopyRateLimitResult result = (lb->queueSize < lb->queueCapacity)
                                      ? LOOPY_RATE_LIMIT_ALLOWED
                                      : LOOPY_RATE_LIMIT_DENIED;

    pthread_mutex_unlock(&lb->mutex);
    return result;
}

static void leakyBucketReset(loopyLeakyBucket *lb) {
    pthread_mutex_lock(&lb->mutex);
    lb->queueSize = 0;
    lb->partialDrain = 0;
    lb->lastDrainUs = getNowUs();
    pthread_mutex_unlock(&lb->mutex);
}

/* ====================================================================
 * Fixed Window Implementation
 * ==================================================================== */

typedef struct loopyFixedWindow {
    _Atomic uint64_t count;
    _Atomic uint64_t windowStart;
    uint64_t windowMs;
    uint64_t maxRequests;
    pthread_mutex_t mutex; /* For reset operations */
} loopyFixedWindow;

static void fixedWindowInit(loopyFixedWindow *fw,
                            const loopyFixedWindowConfig *config) {
    atomic_store(&fw->count, 0);
    atomic_store(&fw->windowStart, getNowMs());
    fw->windowMs = config->windowMs;
    fw->maxRequests = config->maxRequests;
    pthread_mutex_init(&fw->mutex, NULL);
}

static void fixedWindowDestroy(loopyFixedWindow *fw) {
    pthread_mutex_destroy(&fw->mutex);
}

static void fixedWindowMaybeReset(loopyFixedWindow *fw) {
    uint64_t now = getNowMs();
    uint64_t windowStart = atomic_load(&fw->windowStart);

    if (now >= windowStart + fw->windowMs) {
        pthread_mutex_lock(&fw->mutex);
        /* Double-check under lock */
        windowStart = atomic_load(&fw->windowStart);
        if (now >= windowStart + fw->windowMs) {
            atomic_store(&fw->count, 0);
            atomic_store(&fw->windowStart, now);
        }
        pthread_mutex_unlock(&fw->mutex);
    }
}

static loopyRateLimitResult fixedWindowCheck(loopyFixedWindow *fw,
                                             loopyRateLimitInfo *info) {
    fixedWindowMaybeReset(fw);

    uint64_t count = atomic_fetch_add(&fw->count, 1);
    bool allowed = count < fw->maxRequests;

    if (!allowed) {
        /* Undo the increment */
        atomic_fetch_sub(&fw->count, 1);
    }

    if (info) {
        uint64_t now = getNowMs();
        uint64_t windowStart = atomic_load(&fw->windowStart);

        info->allowed = allowed;
        info->remaining = allowed ? (double)(fw->maxRequests - count - 1) : 0;
        info->limit = (double)fw->maxRequests;
        info->resetMs = (windowStart + fw->windowMs > now)
                            ? windowStart + fw->windowMs - now
                            : 0;
        info->retryAfterMs = allowed ? 0 : info->resetMs;
    }

    return allowed ? LOOPY_RATE_LIMIT_ALLOWED : LOOPY_RATE_LIMIT_DENIED;
}

static loopyRateLimitResult fixedWindowPeek(loopyFixedWindow *fw,
                                            loopyRateLimitInfo *info) {
    fixedWindowMaybeReset(fw);

    uint64_t count = atomic_load(&fw->count);
    bool allowed = count < fw->maxRequests;

    if (info) {
        uint64_t now = getNowMs();
        uint64_t windowStart = atomic_load(&fw->windowStart);

        info->allowed = allowed;
        info->remaining =
            (fw->maxRequests > count) ? (double)(fw->maxRequests - count) : 0;
        info->limit = (double)fw->maxRequests;
        info->resetMs = (windowStart + fw->windowMs > now)
                            ? windowStart + fw->windowMs - now
                            : 0;
        info->retryAfterMs = allowed ? 0 : info->resetMs;
    }

    return allowed ? LOOPY_RATE_LIMIT_ALLOWED : LOOPY_RATE_LIMIT_DENIED;
}

static void fixedWindowReset(loopyFixedWindow *fw) {
    pthread_mutex_lock(&fw->mutex);
    atomic_store(&fw->count, 0);
    atomic_store(&fw->windowStart, getNowMs());
    pthread_mutex_unlock(&fw->mutex);
}

/* ====================================================================
 * Rate Limiter Structure
 * ==================================================================== */

struct loopyRateLimiter {
    loopyRateLimitAlgorithm algorithm;
    loopyLoop *loop;

    union {
        loopyTokenBucket tokenBucket;
        loopySlidingWindow slidingWindow;
        loopyLeakyBucket leakyBucket;
        loopyFixedWindow fixedWindow;
    } impl;

    /* Statistics */
    _Atomic uint64_t totalRequests;
    _Atomic uint64_t allowedRequests;
    _Atomic uint64_t deniedRequests;
    _Atomic uint64_t peakRate;

    /* Rate tracking for peak calculation */
    _Atomic uint64_t currentSecondRequests;
    _Atomic uint64_t currentSecond;
};

/* ====================================================================
 * Rate Limiter Lifecycle
 * ==================================================================== */

loopyRateLimiter *loopyRateLimiterNew(loopyLoop *loop,
                                      const loopyRateLimiterConfig *config) {
    if (!config) {
        return NULL;
    }

    loopyRateLimiter *limiter = zcalloc(1, sizeof(loopyRateLimiter));
    if (!limiter) {
        return NULL;
    }

    limiter->algorithm = config->algorithm;
    limiter->loop = loop;

    switch (config->algorithm) {
    case LOOPY_RATE_LIMIT_TOKEN_BUCKET:
        tokenBucketInit(&limiter->impl.tokenBucket,
                        &config->params.tokenBucket);
        break;
    case LOOPY_RATE_LIMIT_SLIDING_WINDOW:
        slidingWindowInit(&limiter->impl.slidingWindow,
                          &config->params.slidingWindow);
        break;
    case LOOPY_RATE_LIMIT_LEAKY_BUCKET:
        leakyBucketInit(&limiter->impl.leakyBucket,
                        &config->params.leakyBucket);
        break;
    case LOOPY_RATE_LIMIT_FIXED_WINDOW:
        fixedWindowInit(&limiter->impl.fixedWindow,
                        &config->params.fixedWindow);
        break;
    default:
        zfree(limiter);
        return NULL;
    }

    return limiter;
}

void loopyRateLimiterFree(loopyRateLimiter *limiter) {
    if (!limiter) {
        return;
    }

    switch (limiter->algorithm) {
    case LOOPY_RATE_LIMIT_TOKEN_BUCKET:
        tokenBucketDestroy(&limiter->impl.tokenBucket);
        break;
    case LOOPY_RATE_LIMIT_SLIDING_WINDOW:
        slidingWindowDestroy(&limiter->impl.slidingWindow);
        break;
    case LOOPY_RATE_LIMIT_LEAKY_BUCKET:
        leakyBucketDestroy(&limiter->impl.leakyBucket);
        break;
    case LOOPY_RATE_LIMIT_FIXED_WINDOW:
        fixedWindowDestroy(&limiter->impl.fixedWindow);
        break;
    }

    zfree(limiter);
}

/* ====================================================================
 * Rate Limiter Operations
 * ==================================================================== */

static void updateStats(loopyRateLimiter *limiter, bool allowed) {
    atomic_fetch_add(&limiter->totalRequests, 1);

    if (allowed) {
        atomic_fetch_add(&limiter->allowedRequests, 1);
    } else {
        atomic_fetch_add(&limiter->deniedRequests, 1);
    }

    /* Track peak rate */
    uint64_t now = getNowMs() / 1000;
    uint64_t lastSecond = atomic_load(&limiter->currentSecond);

    if (now != lastSecond) {
        uint64_t lastCount =
            atomic_exchange(&limiter->currentSecondRequests, 1);
        atomic_store(&limiter->currentSecond, now);

        /* Update peak if needed */
        uint64_t peak = atomic_load(&limiter->peakRate);
        while (lastCount > peak) {
            if (atomic_compare_exchange_weak(&limiter->peakRate, &peak,
                                             lastCount)) {
                break;
            }
        }
    } else {
        atomic_fetch_add(&limiter->currentSecondRequests, 1);
    }
}

loopyRateLimitResult loopyRateLimitCheck(loopyRateLimiter *limiter,
                                         double cost) {
    if (!limiter) {
        return LOOPY_RATE_LIMIT_ERROR;
    }

    loopyRateLimitResult result;
    loopyRateLimitInfo info = {0};

    switch (limiter->algorithm) {
    case LOOPY_RATE_LIMIT_TOKEN_BUCKET:
        result = tokenBucketCheck(&limiter->impl.tokenBucket, cost, &info);
        break;
    case LOOPY_RATE_LIMIT_SLIDING_WINDOW:
        result = slidingWindowCheck(&limiter->impl.slidingWindow, &info);
        break;
    case LOOPY_RATE_LIMIT_LEAKY_BUCKET:
        result = leakyBucketCheck(&limiter->impl.leakyBucket, &info);
        break;
    case LOOPY_RATE_LIMIT_FIXED_WINDOW:
        result = fixedWindowCheck(&limiter->impl.fixedWindow, &info);
        break;
    default:
        return LOOPY_RATE_LIMIT_ERROR;
    }

    updateStats(limiter, result == LOOPY_RATE_LIMIT_ALLOWED);
    return result;
}

loopyRateLimitResult loopyRateLimitPeek(loopyRateLimiter *limiter,
                                        loopyRateLimitInfo *info) {
    if (!limiter) {
        return LOOPY_RATE_LIMIT_ERROR;
    }

    switch (limiter->algorithm) {
    case LOOPY_RATE_LIMIT_TOKEN_BUCKET:
        return tokenBucketPeek(&limiter->impl.tokenBucket, info);
    case LOOPY_RATE_LIMIT_SLIDING_WINDOW:
        return slidingWindowPeek(&limiter->impl.slidingWindow, info);
    case LOOPY_RATE_LIMIT_LEAKY_BUCKET:
        return leakyBucketPeek(&limiter->impl.leakyBucket, info);
    case LOOPY_RATE_LIMIT_FIXED_WINDOW:
        return fixedWindowPeek(&limiter->impl.fixedWindow, info);
    default:
        return LOOPY_RATE_LIMIT_ERROR;
    }
}

loopyRateLimitResult loopyRateLimitCheckKey(loopyRateLimiter *limiter,
                                            const char *key, size_t keyLen,
                                            double cost) {
    /* TODO: Implement per-key rate limiting with hash map */
    (void)key;
    (void)keyLen;
    return loopyRateLimitCheck(limiter, cost);
}

loopyRateLimitResult loopyRateLimitPeekKey(loopyRateLimiter *limiter,
                                           const char *key, size_t keyLen,
                                           loopyRateLimitInfo *info) {
    /* TODO: Implement per-key rate limiting with hash map */
    (void)key;
    (void)keyLen;
    return loopyRateLimitPeek(limiter, info);
}

void loopyRateLimitReset(loopyRateLimiter *limiter, const char *key,
                         size_t keyLen) {
    if (!limiter) {
        return;
    }

    /* TODO: Handle per-key reset */
    (void)key;
    (void)keyLen;

    switch (limiter->algorithm) {
    case LOOPY_RATE_LIMIT_TOKEN_BUCKET:
        tokenBucketReset(&limiter->impl.tokenBucket);
        break;
    case LOOPY_RATE_LIMIT_SLIDING_WINDOW:
        slidingWindowReset(&limiter->impl.slidingWindow);
        break;
    case LOOPY_RATE_LIMIT_LEAKY_BUCKET:
        leakyBucketReset(&limiter->impl.leakyBucket);
        break;
    case LOOPY_RATE_LIMIT_FIXED_WINDOW:
        fixedWindowReset(&limiter->impl.fixedWindow);
        break;
    }
}

bool loopyRateLimitCheckAsync(loopyRateLimiter *limiter, double cost,
                              loopyRateLimitCallback *cb, void *userData) {
    if (!limiter || !cb) {
        return false;
    }

    /* For non-leaky bucket algorithms, just check synchronously and call back
     */
    loopyRateLimitInfo info = {0};
    loopyRateLimitResult result = loopyRateLimitCheck(limiter, cost);
    loopyRateLimitPeek(limiter, &info);

    cb(limiter, result, &info, userData);
    return true;
}

/* ====================================================================
 * Rate Limiter Introspection
 * ==================================================================== */

void loopyRateLimitGetStats(const loopyRateLimiter *limiter,
                            loopyRateLimitStats *stats) {
    if (!limiter || !stats) {
        return;
    }

    stats->totalRequests = atomic_load(&limiter->totalRequests);
    stats->allowedRequests = atomic_load(&limiter->allowedRequests);
    stats->deniedRequests = atomic_load(&limiter->deniedRequests);
    stats->peakRate = atomic_load(&limiter->peakRate);
    stats->avgWaitMs = 0; /* TODO: Track wait times */
}

void loopyRateLimitResetStats(loopyRateLimiter *limiter) {
    if (!limiter) {
        return;
    }

    atomic_store(&limiter->totalRequests, 0);
    atomic_store(&limiter->allowedRequests, 0);
    atomic_store(&limiter->deniedRequests, 0);
    atomic_store(&limiter->peakRate, 0);
    atomic_store(&limiter->currentSecondRequests, 0);
}

loopyRateLimitAlgorithm
loopyRateLimitGetAlgorithm(const loopyRateLimiter *limiter) {
    return limiter ? limiter->algorithm : LOOPY_RATE_LIMIT_TOKEN_BUCKET;
}

/* ====================================================================
 * Concurrency Limiter Structure
 * ==================================================================== */

typedef struct loopyConcurrencyWaiter {
    struct loopyConcurrencyWaiter *next;
    loopyConcurrencyCallback *cb;
    void *userData;
    uint64_t deadlineMs;
} loopyConcurrencyWaiter;

struct loopyConcurrencyLimiter {
    size_t maxConcurrent;
    _Atomic size_t current;
    uint64_t defaultTimeoutMs;
    loopyLoop *loop;

    /* Waiter queue */
    loopyConcurrencyWaiter *waiters;
    pthread_mutex_t mutex;
    pthread_cond_t available;

    /* Statistics */
    _Atomic uint64_t totalAcquires;
    _Atomic uint64_t successfulAcquires;
    _Atomic uint64_t failedAcquires;
    _Atomic uint64_t totalReleases;
    _Atomic size_t peakConcurrent;
    _Atomic uint64_t totalWaitMs;
};

/* ====================================================================
 * Concurrency Limiter Lifecycle
 * ==================================================================== */

loopyConcurrencyLimiter *
loopyConcurrencyLimiterNew(loopyLoop *loop,
                           const loopyConcurrencyConfig *config) {
    if (!config || config->maxConcurrent == 0) {
        return NULL;
    }

    loopyConcurrencyLimiter *limiter =
        zcalloc(1, sizeof(loopyConcurrencyLimiter));
    if (!limiter) {
        return NULL;
    }

    limiter->maxConcurrent = config->maxConcurrent;
    limiter->defaultTimeoutMs = config->timeoutMs;
    limiter->loop = loop;
    atomic_store(&limiter->current, 0);

    if (pthread_mutex_init(&limiter->mutex, NULL) != 0) {
        zfree(limiter);
        return NULL;
    }

    if (pthread_cond_init(&limiter->available, NULL) != 0) {
        pthread_mutex_destroy(&limiter->mutex);
        zfree(limiter);
        return NULL;
    }

    return limiter;
}

void loopyConcurrencyLimiterFree(loopyConcurrencyLimiter *limiter) {
    if (!limiter) {
        return;
    }

    /* Free any pending waiters */
    pthread_mutex_lock(&limiter->mutex);
    loopyConcurrencyWaiter *waiter = limiter->waiters;
    while (waiter) {
        loopyConcurrencyWaiter *next = waiter->next;
        zfree(waiter);
        waiter = next;
    }
    pthread_mutex_unlock(&limiter->mutex);

    pthread_cond_destroy(&limiter->available);
    pthread_mutex_destroy(&limiter->mutex);
    zfree(limiter);
}

/* ====================================================================
 * Concurrency Limiter Operations
 * ==================================================================== */

static void updatePeakConcurrent(loopyConcurrencyLimiter *limiter,
                                 size_t current) {
    size_t peak = atomic_load(&limiter->peakConcurrent);
    while (current > peak) {
        if (atomic_compare_exchange_weak(&limiter->peakConcurrent, &peak,
                                         current)) {
            break;
        }
    }
}

bool loopyConcurrencyTryAcquire(loopyConcurrencyLimiter *limiter) {
    if (!limiter) {
        return false;
    }

    atomic_fetch_add(&limiter->totalAcquires, 1);

    size_t current = atomic_load(&limiter->current);
    while (current < limiter->maxConcurrent) {
        if (atomic_compare_exchange_weak(&limiter->current, &current,
                                         current + 1)) {
            atomic_fetch_add(&limiter->successfulAcquires, 1);
            updatePeakConcurrent(limiter, current + 1);
            return true;
        }
    }

    atomic_fetch_add(&limiter->failedAcquires, 1);
    return false;
}

bool loopyConcurrencyAcquire(loopyConcurrencyLimiter *limiter,
                             uint64_t timeoutMs) {
    if (!limiter) {
        return false;
    }

    atomic_fetch_add(&limiter->totalAcquires, 1);

    /* Try fast path first */
    size_t current = atomic_load(&limiter->current);
    while (current < limiter->maxConcurrent) {
        if (atomic_compare_exchange_weak(&limiter->current, &current,
                                         current + 1)) {
            atomic_fetch_add(&limiter->successfulAcquires, 1);
            updatePeakConcurrent(limiter, current + 1);
            return true;
        }
    }

    /* Slow path: wait for availability */
    uint64_t actualTimeout =
        timeoutMs > 0 ? timeoutMs : limiter->defaultTimeoutMs;
    uint64_t startMs = getNowMs();

    pthread_mutex_lock(&limiter->mutex);

    while (atomic_load(&limiter->current) >= limiter->maxConcurrent) {
        if (actualTimeout > 0) {
            uint64_t elapsedMs = getNowMs() - startMs;
            if (elapsedMs >= actualTimeout) {
                pthread_mutex_unlock(&limiter->mutex);
                atomic_fetch_add(&limiter->failedAcquires, 1);
                return false;
            }

            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            uint64_t remainingMs = actualTimeout - elapsedMs;
            ts.tv_sec += remainingMs / 1000;
            ts.tv_nsec += (remainingMs % 1000) * 1000000;
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }

            int ret = pthread_cond_timedwait(&limiter->available,
                                             &limiter->mutex, &ts);
            if (ret == ETIMEDOUT) {
                pthread_mutex_unlock(&limiter->mutex);
                atomic_fetch_add(&limiter->failedAcquires, 1);
                return false;
            }
        } else {
            pthread_cond_wait(&limiter->available, &limiter->mutex);
        }
    }

    atomic_fetch_add(&limiter->current, 1);
    pthread_mutex_unlock(&limiter->mutex);

    uint64_t waitMs = getNowMs() - startMs;
    atomic_fetch_add(&limiter->totalWaitMs, waitMs);
    atomic_fetch_add(&limiter->successfulAcquires, 1);
    updatePeakConcurrent(limiter, atomic_load(&limiter->current));

    return true;
}

void loopyConcurrencyRelease(loopyConcurrencyLimiter *limiter) {
    if (!limiter) {
        return;
    }

    size_t prev = atomic_fetch_sub(&limiter->current, 1);
    if (prev == 0) {
        /* Underflow - restore */
        atomic_fetch_add(&limiter->current, 1);
        return;
    }

    atomic_fetch_add(&limiter->totalReleases, 1);

    /* Signal any waiters */
    pthread_mutex_lock(&limiter->mutex);
    pthread_cond_signal(&limiter->available);
    pthread_mutex_unlock(&limiter->mutex);
}

bool loopyConcurrencyTryAcquireKey(loopyConcurrencyLimiter *limiter,
                                   const char *key, size_t keyLen) {
    /* TODO: Implement per-key concurrency tracking */
    (void)key;
    (void)keyLen;
    return loopyConcurrencyTryAcquire(limiter);
}

void loopyConcurrencyReleaseKey(loopyConcurrencyLimiter *limiter,
                                const char *key, size_t keyLen) {
    /* TODO: Implement per-key concurrency tracking */
    (void)key;
    (void)keyLen;
    loopyConcurrencyRelease(limiter);
}

bool loopyConcurrencyAcquireAsync(loopyConcurrencyLimiter *limiter,
                                  uint64_t timeoutMs,
                                  loopyConcurrencyCallback *cb,
                                  void *userData) {
    if (!limiter || !cb) {
        return false;
    }

    /* Try fast path */
    if (loopyConcurrencyTryAcquire(limiter)) {
        loopyConcurrencyInfo info;
        loopyConcurrencyGetInfo(limiter, &info);
        cb(limiter, true, &info, userData);
        return true;
    }

    /* For now, just fail immediately for async
     * TODO: Queue waiter and use timer for timeout */
    loopyConcurrencyInfo info;
    loopyConcurrencyGetInfo(limiter, &info);
    cb(limiter, false, &info, userData);
    (void)timeoutMs;

    return true;
}

/* ====================================================================
 * Concurrency Limiter Introspection
 * ==================================================================== */

void loopyConcurrencyGetInfo(const loopyConcurrencyLimiter *limiter,
                             loopyConcurrencyInfo *info) {
    if (!limiter || !info) {
        return;
    }

    info->current = atomic_load(&limiter->current);
    info->limit = limiter->maxConcurrent;
    info->available =
        (info->limit > info->current) ? info->limit - info->current : 0;
    info->waiting = 0; /* TODO: Track waiter count */
}

void loopyConcurrencyGetStats(const loopyConcurrencyLimiter *limiter,
                              loopyConcurrencyStats *stats) {
    if (!limiter || !stats) {
        return;
    }

    stats->totalAcquires = atomic_load(&limiter->totalAcquires);
    stats->successfulAcquires = atomic_load(&limiter->successfulAcquires);
    stats->failedAcquires = atomic_load(&limiter->failedAcquires);
    stats->totalReleases = atomic_load(&limiter->totalReleases);
    stats->peakConcurrent = atomic_load(&limiter->peakConcurrent);
    stats->totalWaitMs = atomic_load(&limiter->totalWaitMs);

    uint64_t successfulBlocking = stats->successfulAcquires;
    if (successfulBlocking > 0) {
        stats->avgWaitMs = (double)stats->totalWaitMs / successfulBlocking;
    } else {
        stats->avgWaitMs = 0;
    }
}

void loopyConcurrencyResetStats(loopyConcurrencyLimiter *limiter) {
    if (!limiter) {
        return;
    }

    atomic_store(&limiter->totalAcquires, 0);
    atomic_store(&limiter->successfulAcquires, 0);
    atomic_store(&limiter->failedAcquires, 0);
    atomic_store(&limiter->totalReleases, 0);
    atomic_store(&limiter->peakConcurrent, 0);
    atomic_store(&limiter->totalWaitMs, 0);
}

/* ====================================================================
 * Configuration Helpers
 * ==================================================================== */

void loopyTokenBucketConfigInit(loopyTokenBucketConfig *config, double rate,
                                double burst) {
    if (!config) {
        return;
    }

    config->capacity = burst;
    config->refillRate = rate;
    config->tokensPerRequest = 1.0;
    config->initialTokens = burst;
}

void loopySlidingWindowConfigInit(loopySlidingWindowConfig *config,
                                  uint64_t windowMs, uint64_t maxRequests) {
    if (!config) {
        return;
    }

    config->windowMs = windowMs;
    config->maxRequests = maxRequests;
    config->maxEntries = SLIDING_WINDOW_DEFAULT_MAX_ENTRIES;
}

void loopyLeakyBucketConfigInit(loopyLeakyBucketConfig *config,
                                double drainRate, size_t queueCapacity) {
    if (!config) {
        return;
    }

    config->drainRate = drainRate;
    config->queueCapacity = queueCapacity;
}

void loopyFixedWindowConfigInit(loopyFixedWindowConfig *config,
                                uint64_t windowMs, uint64_t maxRequests) {
    if (!config) {
        return;
    }

    config->windowMs = windowMs;
    config->maxRequests = maxRequests;
}

void loopyConcurrencyConfigInit(loopyConcurrencyConfig *config,
                                size_t maxConcurrent) {
    if (!config) {
        return;
    }

    config->maxConcurrent = maxConcurrent;
    config->timeoutMs = 0;
    config->trackPerKey = false;
    config->maxKeys = 0;
}

/* ====================================================================
 * Utility
 * ==================================================================== */

const char *loopyRateLimitAlgorithmName(loopyRateLimitAlgorithm algorithm) {
    switch (algorithm) {
    case LOOPY_RATE_LIMIT_TOKEN_BUCKET:
        return "TOKEN_BUCKET";
    case LOOPY_RATE_LIMIT_SLIDING_WINDOW:
        return "SLIDING_WINDOW";
    case LOOPY_RATE_LIMIT_LEAKY_BUCKET:
        return "LEAKY_BUCKET";
    case LOOPY_RATE_LIMIT_FIXED_WINDOW:
        return "FIXED_WINDOW";
    default:
        return "UNKNOWN";
    }
}

const char *loopyRateLimitResultName(loopyRateLimitResult result) {
    switch (result) {
    case LOOPY_RATE_LIMIT_ALLOWED:
        return "ALLOWED";
    case LOOPY_RATE_LIMIT_DENIED:
        return "DENIED";
    case LOOPY_RATE_LIMIT_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}
