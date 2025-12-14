/* loopyConcurrencyPool - Multi-tenant concurrency management
 *
 * Provides per-user/tenant concurrency limiting within a coherent system:
 * - Each user has their own configurable concurrency limit
 * - Users acquire and release slots for concurrent operations
 * - System-wide limits can cap total concurrency across all users
 * - Fair scheduling options to prevent starvation
 *
 * Use Cases:
 * - API rate limiting per client/API key
 * - Database connection pools per tenant
 * - Task queue parallelism per job type
 * - Resource allocation in multi-tenant systems
 *
 * Copyright 2024 Matt Stancliff <matt@genges.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
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
 * Opaque concurrency pool handle.
 */
typedef struct loopyConcurrencyPool loopyConcurrencyPool;

/**
 * Opaque user/tenant handle within a pool.
 */
typedef struct loopyConcurrencyUser loopyConcurrencyUser;

/**
 * Acquisition result codes.
 *
 * Common codes reference base loopyStatus values directly.
 * Module-specific codes use the -500 range.
 */
typedef enum loopyConcurrencyResult {
    LOOPY_CONCURRENCY_OK = LOOPY_OK,               /* Slots acquired */
    LOOPY_CONCURRENCY_ERROR = LOOPY_ERROR,         /* Internal error */
    LOOPY_CONCURRENCY_INVALID = LOOPY_INVALID,     /* Invalid params */
    LOOPY_CONCURRENCY_TIMEOUT = LOOPY_TIMEOUT,     /* Wait timed out */
    LOOPY_CONCURRENCY_NOT_FOUND = LOOPY_NOT_FOUND, /* User not found */
    /* Module-specific codes */
    LOOPY_CONCURRENCY_LIMIT = -500,        /* User limit reached */
    LOOPY_CONCURRENCY_GLOBAL_LIMIT = -501, /* Global limit reached */
} loopyConcurrencyResult;

/**
 * Pool configuration.
 */
typedef struct loopyConcurrencyPoolConfig {
    size_t globalLimit; /* Max total slots across all users (0 = unlimited) */
    size_t defaultUserLimit; /* Default limit for new users */
    size_t maxUsers;         /* Max number of users (0 = unlimited) */
    bool fairScheduling;     /* Enable fair scheduling across users */
    bool autoCreateUsers;    /* Auto-create users on first acquire */
    bool trackStats;         /* Enable statistics tracking */
} loopyConcurrencyPoolConfig;

/**
 * User configuration.
 */
typedef struct loopyConcurrencyUserConfig {
    size_t limit;      /* Max concurrent slots for this user */
    size_t reserved;   /* Guaranteed reserved slots */
    uint32_t priority; /* Priority (higher = more priority) */
    void *userData;    /* User-provided context */
} loopyConcurrencyUserConfig;

/**
 * Pool statistics.
 */
typedef struct loopyConcurrencyPoolStats {
    size_t globalLimit;     /* Configured global limit */
    size_t globalActive;    /* Currently active slots (all users) */
    size_t globalPeak;      /* Peak concurrent usage */
    size_t userCount;       /* Number of registered users */
    uint64_t totalAcquires; /* Total successful acquisitions */
    uint64_t totalReleases; /* Total releases */
    uint64_t totalDenied;   /* Total denied acquisitions */
    uint64_t totalWaits;    /* Total times a request had to wait */
} loopyConcurrencyPoolStats;

/**
 * User statistics.
 */
typedef struct loopyConcurrencyUserStats {
    size_t limit;      /* User's concurrency limit */
    size_t active;     /* Currently active slots */
    size_t peak;       /* Peak concurrent usage */
    size_t reserved;   /* Reserved slots */
    uint64_t acquires; /* Total acquisitions */
    uint64_t releases; /* Total releases */
    uint64_t denied;   /* Times denied */
    uint64_t waits;    /* Times had to wait */
} loopyConcurrencyUserStats;

/* ====================================================================
 * Pool Lifecycle
 * ==================================================================== */

/**
 * Initialize pool configuration with defaults.
 *
 * @param config  Configuration to initialize
 */
void loopyConcurrencyPoolConfigInit(loopyConcurrencyPoolConfig *config);

/**
 * Initialize user configuration with defaults.
 *
 * @param config  Configuration to initialize
 */
void loopyConcurrencyUserConfigInit(loopyConcurrencyUserConfig *config);

/**
 * Create a new concurrency pool.
 *
 * @param config  Pool configuration (NULL for defaults)
 * @return New pool, or NULL on error
 */
loopyConcurrencyPool *
loopyConcurrencyPoolNew(const loopyConcurrencyPoolConfig *config);

/**
 * Destroy a concurrency pool.
 *
 * All users are automatically removed. Active slots are force-released.
 *
 * @param pool  The pool to destroy
 */
void loopyConcurrencyPoolFree(loopyConcurrencyPool *pool);

/* ====================================================================
 * User Management
 * ==================================================================== */

/**
 * Register a new user in the pool.
 *
 * @param pool    The pool
 * @param userId  Unique user identifier (string, will be copied)
 * @param config  User configuration (NULL for defaults)
 * @return User handle, or NULL on error
 */
loopyConcurrencyUser *
loopyConcurrencyPoolAddUser(loopyConcurrencyPool *pool, const char *userId,
                            const loopyConcurrencyUserConfig *config);

/**
 * Remove a user from the pool.
 *
 * Active slots are force-released.
 *
 * @param pool    The pool
 * @param userId  User identifier
 * @return true if user was found and removed
 */
bool loopyConcurrencyPoolRemoveUser(loopyConcurrencyPool *pool,
                                    const char *userId);

/**
 * Get a user handle by ID.
 *
 * @param pool    The pool
 * @param userId  User identifier
 * @return User handle, or NULL if not found
 */
loopyConcurrencyUser *loopyConcurrencyPoolGetUser(loopyConcurrencyPool *pool,
                                                  const char *userId);

/**
 * Update a user's configuration.
 *
 * @param user    The user handle
 * @param config  New configuration
 * @return true on success
 */
bool loopyConcurrencyUserUpdate(loopyConcurrencyUser *user,
                                const loopyConcurrencyUserConfig *config);

/**
 * Get a user's ID.
 *
 * @param user  The user handle
 * @return User ID string (do not free)
 */
const char *loopyConcurrencyUserId(const loopyConcurrencyUser *user);

/**
 * Get a user's custom data.
 *
 * @param user  The user handle
 * @return User data pointer
 */
void *loopyConcurrencyUserData(const loopyConcurrencyUser *user);

/* ====================================================================
 * Slot Acquisition and Release
 * ==================================================================== */

/**
 * Try to acquire slots for a user (non-blocking).
 *
 * @param pool    The pool
 * @param userId  User identifier
 * @param count   Number of slots to acquire
 * @return Result code (LOOPY_CONCURRENCY_OK on success)
 */
loopyConcurrencyResult
loopyConcurrencyPoolTryAcquire(loopyConcurrencyPool *pool, const char *userId,
                               size_t count);

/**
 * Try to acquire slots using user handle (non-blocking).
 *
 * @param user   The user handle
 * @param count  Number of slots to acquire
 * @return Result code
 */
loopyConcurrencyResult
loopyConcurrencyUserTryAcquire(loopyConcurrencyUser *user, size_t count);

/**
 * Release slots for a user.
 *
 * @param pool    The pool
 * @param userId  User identifier
 * @param count   Number of slots to release
 * @return true on success
 */
bool loopyConcurrencyPoolRelease(loopyConcurrencyPool *pool, const char *userId,
                                 size_t count);

/**
 * Release slots using user handle.
 *
 * @param user   The user handle
 * @param count  Number of slots to release
 * @return true on success
 */
bool loopyConcurrencyUserRelease(loopyConcurrencyUser *user, size_t count);

/**
 * Release all slots for a user.
 *
 * @param pool    The pool
 * @param userId  User identifier
 * @return Number of slots released
 */
size_t loopyConcurrencyPoolReleaseAll(loopyConcurrencyPool *pool,
                                      const char *userId);

/**
 * Release all slots using user handle.
 *
 * @param user  The user handle
 * @return Number of slots released
 */
size_t loopyConcurrencyUserReleaseAll(loopyConcurrencyUser *user);

/* ====================================================================
 * Querying
 * ==================================================================== */

/**
 * Get number of available slots for a user.
 *
 * @param pool    The pool
 * @param userId  User identifier
 * @return Available slots, or 0 if user not found
 */
size_t loopyConcurrencyAvailable(loopyConcurrencyPool *pool,
                                 const char *userId);

/**
 * Get number of available slots using user handle.
 *
 * @param user  The user handle
 * @return Available slots
 */
size_t loopyConcurrencyUserAvailable(const loopyConcurrencyUser *user);

/**
 * Get number of active slots for a user.
 *
 * @param pool    The pool
 * @param userId  User identifier
 * @return Active slots
 */
size_t loopyConcurrencyActive(loopyConcurrencyPool *pool, const char *userId);

/**
 * Get number of active slots using user handle.
 *
 * @param user  The user handle
 * @return Active slots
 */
size_t loopyConcurrencyUserActive(const loopyConcurrencyUser *user);

/**
 * Check if user can acquire slots.
 *
 * @param pool    The pool
 * @param userId  User identifier
 * @param count   Number of slots
 * @return true if acquisition would succeed
 */
bool loopyConcurrencyCanAcquire(loopyConcurrencyPool *pool, const char *userId,
                                size_t count);

/**
 * Get global available slots.
 *
 * @param pool  The pool
 * @return Available slots globally
 */
size_t loopyConcurrencyPoolAvailable(const loopyConcurrencyPool *pool);

/**
 * Get global active slots.
 *
 * @param pool  The pool
 * @return Active slots globally
 */
size_t loopyConcurrencyPoolActive(const loopyConcurrencyPool *pool);

/* ====================================================================
 * Statistics
 * ==================================================================== */

/**
 * Get pool statistics.
 *
 * @param pool   The pool
 * @param stats  Output statistics
 */
void loopyConcurrencyPoolGetStats(const loopyConcurrencyPool *pool,
                                  loopyConcurrencyPoolStats *stats);

/**
 * Get user statistics.
 *
 * @param user   The user handle
 * @param stats  Output statistics
 */
void loopyConcurrencyUserGetStats(const loopyConcurrencyUser *user,
                                  loopyConcurrencyUserStats *stats);

/**
 * Reset pool statistics.
 *
 * @param pool  The pool
 */
void loopyConcurrencyPoolResetStats(loopyConcurrencyPool *pool);

/**
 * Reset user statistics.
 *
 * @param user  The user handle
 */
void loopyConcurrencyUserResetStats(loopyConcurrencyUser *user);

/* ====================================================================
 * Iteration
 * ==================================================================== */

/**
 * User iteration callback.
 *
 * @param userId  User identifier
 * @param user    User handle
 * @param arg     User argument
 * @return 0 to continue, non-zero to stop
 */
typedef int loopyConcurrencyUserIterFn(const char *userId,
                                       loopyConcurrencyUser *user, void *arg);

/**
 * Iterate all users in the pool.
 *
 * @param pool  The pool
 * @param fn    Iterator callback
 * @param arg   User argument
 * @return Number of users visited
 */
size_t loopyConcurrencyPoolIterate(loopyConcurrencyPool *pool,
                                   loopyConcurrencyUserIterFn *fn, void *arg);

/* ====================================================================
 * Utility
 * ==================================================================== */

/**
 * Get result code name.
 *
 * @param result  Result code
 * @return Static string
 */
const char *loopyConcurrencyResultName(loopyConcurrencyResult result);

/**
 * Get number of registered users.
 *
 * @param pool  The pool
 * @return User count
 */
size_t loopyConcurrencyPoolUserCount(const loopyConcurrencyPool *pool);
