/* loopyRateLimit - Rate limiting and concurrency control for loopy
 *
 * Provides multiple rate limiting algorithms and concurrent request tracking
 * for building robust, resource-controlled applications. Supports both
 * synchronous and asynchronous operation patterns.
 *
 * Algorithms:
 * - Token Bucket: Smooth rate limiting with burst capacity
 * - Sliding Window: Precise per-window request counting
 * - Leaky Bucket: Constant output rate smoothing
 * - Fixed Window: Simple time-based counting
 * - Concurrent: Active request/connection limiting
 *
 * Key Features:
 * - Thread-safe implementations using atomics where possible
 * - Event loop integration for async rate-limited operations
 * - Hierarchical rate limiters (global + per-key)
 * - Statistics and metrics tracking
 * - Graceful degradation with configurable backpressure
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
 * Opaque rate limiter structure.
 */
typedef struct loopyRateLimiter loopyRateLimiter;

/**
 * Opaque concurrency limiter structure.
 */
typedef struct loopyConcurrencyLimiter loopyConcurrencyLimiter;

/**
 * Rate limiting algorithm types.
 */
typedef enum loopyRateLimitAlgorithm {
    LOOPY_RATE_LIMIT_TOKEN_BUCKET = 0,   /* Token bucket with burst */
    LOOPY_RATE_LIMIT_SLIDING_WINDOW = 1, /* Sliding window log */
    LOOPY_RATE_LIMIT_LEAKY_BUCKET = 2,   /* Leaky bucket queue */
    LOOPY_RATE_LIMIT_FIXED_WINDOW = 3,   /* Fixed time window */
} loopyRateLimitAlgorithm;

/**
 * Rate limit check result.
 *
 * Common codes reference base loopyStatus values directly.
 * Module-specific codes use the -400 range.
 */
typedef enum loopyRateLimitResult {
    LOOPY_RATE_LIMIT_OK = LOOPY_OK,       /* Request allowed */
    LOOPY_RATE_LIMIT_ERROR = LOOPY_ERROR, /* Internal error */
    /* Module-specific codes */
    LOOPY_RATE_LIMIT_DENIED = -400, /* Request denied (rate exceeded) */
    /* Alias for clarity */
    LOOPY_RATE_LIMIT_ALLOWED = LOOPY_RATE_LIMIT_OK,
} loopyRateLimitResult;

/**
 * Token bucket configuration.
 *
 * Tokens refill at 'refillRate' per second, up to 'capacity'.
 * Each request consumes 'tokensPerRequest' tokens.
 */
typedef struct loopyTokenBucketConfig {
    double capacity;         /* Maximum tokens (burst size) */
    double refillRate;       /* Tokens per second */
    double tokensPerRequest; /* Tokens consumed per request (default: 1.0) */
    double initialTokens;    /* Initial tokens (default: capacity) */
} loopyTokenBucketConfig;

/**
 * Sliding window configuration.
 *
 * Tracks exact timestamps of recent requests within windowMs.
 * More memory-intensive but accurate for bursty traffic.
 */
typedef struct loopySlidingWindowConfig {
    uint64_t windowMs;    /* Window duration in milliseconds */
    uint64_t maxRequests; /* Maximum requests per window */
    size_t maxEntries;    /* Maximum tracked entries (memory limit) */
} loopySlidingWindowConfig;

/**
 * Leaky bucket configuration.
 *
 * Requests queue and drain at constant rate.
 * Provides smooth output rate regardless of input bursts.
 */
typedef struct loopyLeakyBucketConfig {
    double drainRate;     /* Requests per second (output rate) */
    size_t queueCapacity; /* Maximum queued requests */
} loopyLeakyBucketConfig;

/**
 * Fixed window configuration.
 *
 * Simple counter reset at fixed intervals.
 * Vulnerable to burst at window boundaries.
 */
typedef struct loopyFixedWindowConfig {
    uint64_t windowMs;    /* Window duration in milliseconds */
    uint64_t maxRequests; /* Maximum requests per window */
} loopyFixedWindowConfig;

/**
 * Rate limiter configuration union.
 */
typedef struct loopyRateLimiterConfig {
    loopyRateLimitAlgorithm algorithm;
    union {
        loopyTokenBucketConfig tokenBucket;
        loopySlidingWindowConfig slidingWindow;
        loopyLeakyBucketConfig leakyBucket;
        loopyFixedWindowConfig fixedWindow;
    } params;
} loopyRateLimiterConfig;

/**
 * Concurrency limiter configuration.
 */
typedef struct loopyConcurrencyConfig {
    size_t maxConcurrent; /* Maximum concurrent operations */
    uint64_t timeoutMs; /* Operation timeout in milliseconds (0 = no timeout) */
    bool trackPerKey;   /* Enable per-key tracking */
    size_t maxKeys;     /* Maximum tracked keys (when trackPerKey is true) */
} loopyConcurrencyConfig;

/* ====================================================================
 * Rate Limit Info
 * ==================================================================== */

/**
 * Information about current rate limit state.
 */
typedef struct loopyRateLimitInfo {
    bool allowed;          /* Would a request be allowed now? */
    uint64_t retryAfterMs; /* Suggested wait time in milliseconds */
    double remaining;      /* Remaining capacity (algorithm-specific) */
    double limit;          /* Total limit (algorithm-specific) */
    uint64_t resetMs;      /* Time until reset in milliseconds */
} loopyRateLimitInfo;

/**
 * Concurrency limit information.
 */
typedef struct loopyConcurrencyInfo {
    size_t current;   /* Current concurrent operations */
    size_t limit;     /* Maximum allowed */
    size_t waiting;   /* Operations waiting for a slot */
    size_t available; /* Available slots */
} loopyConcurrencyInfo;

/* ====================================================================
 * Statistics
 * ==================================================================== */

/**
 * Rate limiter statistics.
 */
typedef struct loopyRateLimitStats {
    uint64_t totalRequests;   /* Total requests checked */
    uint64_t allowedRequests; /* Requests that were allowed */
    uint64_t deniedRequests;  /* Requests that were denied */
    uint64_t peakRate;        /* Peak requests per second observed */
    double avgWaitMs;         /* Average wait time for queued requests */
} loopyRateLimitStats;

/**
 * Concurrency limiter statistics.
 */
typedef struct loopyConcurrencyStats {
    uint64_t totalAcquires;      /* Total acquire attempts */
    uint64_t successfulAcquires; /* Successful acquires */
    uint64_t failedAcquires;     /* Failed acquires (timeout/limit) */
    uint64_t totalReleases;      /* Total releases */
    size_t peakConcurrent;       /* Peak concurrent operations */
    uint64_t totalWaitMs;        /* Total wait time across all operations */
    double avgWaitMs;            /* Average wait time for queued acquires */
} loopyConcurrencyStats;

/* ====================================================================
 * Callbacks
 * ==================================================================== */

/**
 * Callback for async rate limit check completion.
 *
 * @param limiter   The rate limiter
 * @param result    Check result
 * @param info      Rate limit info
 * @param userData  User-provided data
 */
typedef void loopyRateLimitCallback(loopyRateLimiter *limiter,
                                    loopyRateLimitResult result,
                                    const loopyRateLimitInfo *info,
                                    void *userData);

/**
 * Callback for async concurrency acquire completion.
 *
 * @param limiter   The concurrency limiter
 * @param acquired  true if slot was acquired
 * @param info      Concurrency info
 * @param userData  User-provided data
 */
typedef void loopyConcurrencyCallback(loopyConcurrencyLimiter *limiter,
                                      bool acquired,
                                      const loopyConcurrencyInfo *info,
                                      void *userData);

/* ====================================================================
 * Rate Limiter - Lifecycle
 * ==================================================================== */

/**
 * Create a new rate limiter.
 *
 * @param loop   Event loop for async operations (may be NULL for sync-only)
 * @param config Rate limiter configuration
 * @return New rate limiter, or NULL on error
 *
 * Thread Safety: Must be called from event loop thread if loop is non-NULL.
 */
loopyRateLimiter *loopyRateLimiterNew(loopyLoop *loop,
                                      const loopyRateLimiterConfig *config);

/**
 * Destroy a rate limiter.
 *
 * @param limiter The rate limiter to destroy
 */
void loopyRateLimiterFree(loopyRateLimiter *limiter);

/* ====================================================================
 * Rate Limiter - Operations
 * ==================================================================== */

/**
 * Check if a request is allowed and consume quota if so.
 *
 * @param limiter The rate limiter
 * @param cost    Cost of this request (default 1.0 for token bucket)
 * @return LOOPY_RATE_LIMIT_ALLOWED if allowed, LOOPY_RATE_LIMIT_DENIED if not
 *
 * Thread Safety: Safe from any thread.
 */
loopyRateLimitResult loopyRateLimitCheck(loopyRateLimiter *limiter,
                                         double cost);

/**
 * Check rate limit without consuming quota.
 *
 * @param limiter The rate limiter
 * @param info    Output for rate limit state info
 * @return LOOPY_RATE_LIMIT_ALLOWED if request would be allowed
 *
 * Thread Safety: Safe from any thread.
 */
loopyRateLimitResult loopyRateLimitPeek(loopyRateLimiter *limiter,
                                        loopyRateLimitInfo *info);

/**
 * Check rate limit for a specific key.
 *
 * Creates per-key state if it doesn't exist.
 *
 * @param limiter The rate limiter
 * @param key     Key string (e.g., IP address, user ID)
 * @param keyLen  Key length
 * @param cost    Cost of this request
 * @return Result
 */
loopyRateLimitResult loopyRateLimitCheckKey(loopyRateLimiter *limiter,
                                            const char *key, size_t keyLen,
                                            double cost);

/**
 * Get rate limit info for a specific key.
 *
 * @param limiter The rate limiter
 * @param key     Key string
 * @param keyLen  Key length
 * @param info    Output for rate limit state info
 * @return Result
 */
loopyRateLimitResult loopyRateLimitPeekKey(loopyRateLimiter *limiter,
                                           const char *key, size_t keyLen,
                                           loopyRateLimitInfo *info);

/**
 * Reset rate limit state for a key.
 *
 * @param limiter The rate limiter
 * @param key     Key string (NULL for global state)
 * @param keyLen  Key length
 */
void loopyRateLimitReset(loopyRateLimiter *limiter, const char *key,
                         size_t keyLen);

/**
 * Async rate limit check.
 *
 * For leaky bucket, queues the request and calls back when allowed.
 * For other algorithms, calls back immediately with result.
 *
 * @param limiter  The rate limiter
 * @param cost     Cost of this request
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return true if operation was queued
 */
bool loopyRateLimitCheckAsync(loopyRateLimiter *limiter, double cost,
                              loopyRateLimitCallback *cb, void *userData);

/* ====================================================================
 * Rate Limiter - Introspection
 * ==================================================================== */

/**
 * Get rate limiter statistics.
 *
 * @param limiter The rate limiter
 * @param stats   Output statistics structure
 */
void loopyRateLimitGetStats(const loopyRateLimiter *limiter,
                            loopyRateLimitStats *stats);

/**
 * Reset rate limiter statistics.
 *
 * @param limiter The rate limiter
 */
void loopyRateLimitResetStats(loopyRateLimiter *limiter);

/**
 * Get the algorithm type.
 *
 * @param limiter The rate limiter
 * @return Algorithm type
 */
loopyRateLimitAlgorithm
loopyRateLimitGetAlgorithm(const loopyRateLimiter *limiter);

/* ====================================================================
 * Concurrency Limiter - Lifecycle
 * ==================================================================== */

/**
 * Create a new concurrency limiter.
 *
 * @param loop   Event loop for async operations (may be NULL)
 * @param config Concurrency limiter configuration
 * @return New concurrency limiter, or NULL on error
 */
loopyConcurrencyLimiter *
loopyConcurrencyLimiterNew(loopyLoop *loop,
                           const loopyConcurrencyConfig *config);

/**
 * Destroy a concurrency limiter.
 *
 * @param limiter The limiter to destroy
 */
void loopyConcurrencyLimiterFree(loopyConcurrencyLimiter *limiter);

/* ====================================================================
 * Concurrency Limiter - Operations
 * ==================================================================== */

/**
 * Try to acquire a concurrency slot.
 *
 * @param limiter The concurrency limiter
 * @return true if slot was acquired
 *
 * Thread Safety: Safe from any thread.
 */
bool loopyConcurrencyTryAcquire(loopyConcurrencyLimiter *limiter);

/**
 * Acquire a concurrency slot, blocking if necessary.
 *
 * @param limiter   The concurrency limiter
 * @param timeoutMs Maximum wait time (0 = use configured timeout)
 * @return true if slot was acquired, false on timeout
 *
 * Thread Safety: Safe from any thread.
 */
bool loopyConcurrencyAcquire(loopyConcurrencyLimiter *limiter,
                             uint64_t timeoutMs);

/**
 * Release a concurrency slot.
 *
 * @param limiter The concurrency limiter
 *
 * Thread Safety: Safe from any thread.
 */
void loopyConcurrencyRelease(loopyConcurrencyLimiter *limiter);

/**
 * Try to acquire a slot for a specific key.
 *
 * @param limiter The concurrency limiter
 * @param key     Key string
 * @param keyLen  Key length
 * @return true if slot was acquired
 */
bool loopyConcurrencyTryAcquireKey(loopyConcurrencyLimiter *limiter,
                                   const char *key, size_t keyLen);

/**
 * Release a slot for a specific key.
 *
 * @param limiter The concurrency limiter
 * @param key     Key string
 * @param keyLen  Key length
 */
void loopyConcurrencyReleaseKey(loopyConcurrencyLimiter *limiter,
                                const char *key, size_t keyLen);

/**
 * Async acquire with callback.
 *
 * Queues the acquire and calls back when a slot becomes available
 * or timeout occurs.
 *
 * @param limiter   The concurrency limiter
 * @param timeoutMs Timeout in milliseconds (0 = use configured)
 * @param cb        Callback function
 * @param userData  User data for callback
 * @return true if request was queued
 */
bool loopyConcurrencyAcquireAsync(loopyConcurrencyLimiter *limiter,
                                  uint64_t timeoutMs,
                                  loopyConcurrencyCallback *cb, void *userData);

/* ====================================================================
 * Concurrency Limiter - Introspection
 * ==================================================================== */

/**
 * Get current concurrency info.
 *
 * @param limiter The concurrency limiter
 * @param info    Output info structure
 */
void loopyConcurrencyGetInfo(const loopyConcurrencyLimiter *limiter,
                             loopyConcurrencyInfo *info);

/**
 * Get concurrency limiter statistics.
 *
 * @param limiter The concurrency limiter
 * @param stats   Output statistics structure
 */
void loopyConcurrencyGetStats(const loopyConcurrencyLimiter *limiter,
                              loopyConcurrencyStats *stats);

/**
 * Reset concurrency limiter statistics.
 *
 * @param limiter The limiter
 */
void loopyConcurrencyResetStats(loopyConcurrencyLimiter *limiter);

/* ====================================================================
 * Configuration Helpers
 * ==================================================================== */

/**
 * Initialize token bucket config with defaults.
 *
 * @param config Config to initialize
 * @param rate   Requests per second
 * @param burst  Maximum burst size
 */
void loopyTokenBucketConfigInit(loopyTokenBucketConfig *config, double rate,
                                double burst);

/**
 * Initialize sliding window config with defaults.
 *
 * @param config      Config to initialize
 * @param windowMs    Window duration in milliseconds
 * @param maxRequests Maximum requests per window
 */
void loopySlidingWindowConfigInit(loopySlidingWindowConfig *config,
                                  uint64_t windowMs, uint64_t maxRequests);

/**
 * Initialize leaky bucket config with defaults.
 *
 * @param config        Config to initialize
 * @param drainRate     Requests per second
 * @param queueCapacity Maximum queue size
 */
void loopyLeakyBucketConfigInit(loopyLeakyBucketConfig *config,
                                double drainRate, size_t queueCapacity);

/**
 * Initialize fixed window config with defaults.
 *
 * @param config      Config to initialize
 * @param windowMs    Window duration in milliseconds
 * @param maxRequests Maximum requests per window
 */
void loopyFixedWindowConfigInit(loopyFixedWindowConfig *config,
                                uint64_t windowMs, uint64_t maxRequests);

/**
 * Initialize concurrency config with defaults.
 *
 * @param config       Config to initialize
 * @param maxConcurrent Maximum concurrent operations
 */
void loopyConcurrencyConfigInit(loopyConcurrencyConfig *config,
                                size_t maxConcurrent);

/* ====================================================================
 * Utility
 * ==================================================================== */

/**
 * Get algorithm name string.
 *
 * @param algorithm The algorithm type
 * @return Static string name
 */
const char *loopyRateLimitAlgorithmName(loopyRateLimitAlgorithm algorithm);

/**
 * Get result name string.
 *
 * @param result The result code
 * @return Static string name
 */
const char *loopyRateLimitResultName(loopyRateLimitResult result);
