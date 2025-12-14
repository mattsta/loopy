/* loopyConcurrencyPool - Multi-tenant concurrency management
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
#include "loopyConcurrencyPool.h"

#include "../deps/datakit/src/datakit.h"
#include "../deps/rax/src/rax.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

/* ====================================================================
 * Internal Structures
 * ==================================================================== */

struct loopyConcurrencyUser {
    loopyConcurrencyPool *pool;
    char *userId;
    size_t limit;
    size_t reserved;
    uint32_t priority;
    void *userData;

    /* Current state */
    atomic_size_t active;
    atomic_size_t peak;

    /* Statistics */
    atomic_uint_least64_t acquires;
    atomic_uint_least64_t releases;
    atomic_uint_least64_t denied;
    atomic_uint_least64_t waits;
};

struct loopyConcurrencyPool {
    rax *users; /* userId -> loopyConcurrencyUser* */
    pthread_rwlock_t lock;

    /* Configuration */
    loopyConcurrencyPoolConfig config;

    /* Global state */
    atomic_size_t globalActive;
    atomic_size_t globalPeak;
    atomic_size_t userCount;

    /* Statistics */
    atomic_uint_least64_t totalAcquires;
    atomic_uint_least64_t totalReleases;
    atomic_uint_least64_t totalDenied;
    atomic_uint_least64_t totalWaits;
};

/* ====================================================================
 * Utilities
 * ==================================================================== */

static char *poolStrdup(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t len = strlen(s) + 1;
    char *copy = zmalloc(len);
    if (copy) {
        memcpy(copy, s, len);
    }
    return copy;
}

static void updatePeak(atomic_size_t *peak, size_t current) {
    size_t oldPeak = atomic_load(peak);
    while (current > oldPeak) {
        if (atomic_compare_exchange_weak(peak, &oldPeak, current)) {
            break;
        }
    }
}

/* ====================================================================
 * Configuration
 * ==================================================================== */

void loopyConcurrencyPoolConfigInit(loopyConcurrencyPoolConfig *config) {
    if (!config) {
        return;
    }

    memset(config, 0, sizeof(*config));
    config->globalLimit = 0; /* Unlimited */
    config->defaultUserLimit = 10;
    config->maxUsers = 0; /* Unlimited */
    config->fairScheduling = false;
    config->autoCreateUsers = true;
    config->trackStats = true;
}

void loopyConcurrencyUserConfigInit(loopyConcurrencyUserConfig *config) {
    if (!config) {
        return;
    }

    memset(config, 0, sizeof(*config));
    config->limit = 10;
    config->reserved = 0;
    config->priority = 0;
    config->userData = NULL;
}

/* ====================================================================
 * Pool Lifecycle
 * ==================================================================== */

loopyConcurrencyPool *
loopyConcurrencyPoolNew(const loopyConcurrencyPoolConfig *config) {
    loopyConcurrencyPool *pool = zcalloc(1, sizeof(loopyConcurrencyPool));
    if (!pool) {
        return NULL;
    }

    pool->users = raxNew();
    if (!pool->users) {
        zfree(pool);
        return NULL;
    }

    if (pthread_rwlock_init(&pool->lock, NULL) != 0) {
        raxFree(pool->users);
        zfree(pool);
        return NULL;
    }

    if (config) {
        pool->config = *config;
    } else {
        loopyConcurrencyPoolConfigInit(&pool->config);
    }

    atomic_init(&pool->globalActive, 0);
    atomic_init(&pool->globalPeak, 0);
    atomic_init(&pool->userCount, 0);
    atomic_init(&pool->totalAcquires, 0);
    atomic_init(&pool->totalReleases, 0);
    atomic_init(&pool->totalDenied, 0);
    atomic_init(&pool->totalWaits, 0);

    return pool;
}

static void freeUser(void *ptr) {
    loopyConcurrencyUser *user = ptr;
    if (!user) {
        return;
    }
    zfree(user->userId);
    zfree(user);
}

void loopyConcurrencyPoolFree(loopyConcurrencyPool *pool) {
    if (!pool) {
        return;
    }

    pthread_rwlock_wrlock(&pool->lock);

    /* Free all users */
    raxIterator iter;
    raxStart(&iter, pool->users);
    raxSeek(&iter, "^", NULL, 0);
    while (raxNext(&iter)) {
        loopyConcurrencyUser *user = iter.data;
        freeUser(user);
    }
    raxStop(&iter);
    raxFree(pool->users);

    pthread_rwlock_unlock(&pool->lock);
    pthread_rwlock_destroy(&pool->lock);

    zfree(pool);
}

/* ====================================================================
 * User Management
 * ==================================================================== */

loopyConcurrencyUser *
loopyConcurrencyPoolAddUser(loopyConcurrencyPool *pool, const char *userId,
                            const loopyConcurrencyUserConfig *config) {
    if (!pool || !userId) {
        return NULL;
    }

    /* Check user limit */
    if (pool->config.maxUsers > 0 &&
        atomic_load(&pool->userCount) >= pool->config.maxUsers) {
        return NULL;
    }

    loopyConcurrencyUser *user = zcalloc(1, sizeof(loopyConcurrencyUser));
    if (!user) {
        return NULL;
    }

    user->pool = pool;
    user->userId = poolStrdup(userId);
    if (!user->userId) {
        zfree(user);
        return NULL;
    }

    if (config) {
        user->limit = config->limit;
        user->reserved = config->reserved;
        user->priority = config->priority;
        user->userData = config->userData;
    } else {
        user->limit = pool->config.defaultUserLimit;
        user->reserved = 0;
        user->priority = 0;
        user->userData = NULL;
    }

    atomic_init(&user->active, 0);
    atomic_init(&user->peak, 0);
    atomic_init(&user->acquires, 0);
    atomic_init(&user->releases, 0);
    atomic_init(&user->denied, 0);
    atomic_init(&user->waits, 0);

    pthread_rwlock_wrlock(&pool->lock);

    /* Check if user already exists - use TryInsert to avoid overwriting */
    void *old = NULL;
    int ret = raxTryInsert(pool->users, (unsigned char *)userId, strlen(userId),
                           user, &old);
    if (ret == 0) {
        /* Key already exists - old has the existing user, tree unchanged */
        pthread_rwlock_unlock(&pool->lock);
        zfree(user->userId);
        zfree(user);
        return old; /* Return existing user, or NULL if OOM */
    }

    atomic_fetch_add(&pool->userCount, 1);

    pthread_rwlock_unlock(&pool->lock);

    return user;
}

bool loopyConcurrencyPoolRemoveUser(loopyConcurrencyPool *pool,
                                    const char *userId) {
    if (!pool || !userId) {
        return false;
    }

    pthread_rwlock_wrlock(&pool->lock);

    loopyConcurrencyUser *user =
        raxFind(pool->users, (unsigned char *)userId, strlen(userId));
    if (user == RAX_NOT_FOUND) {
        pthread_rwlock_unlock(&pool->lock);
        return false;
    }

    /* Release any active slots */
    size_t active = atomic_load(&user->active);
    if (active > 0) {
        atomic_fetch_sub(&pool->globalActive, active);
    }

    raxRemove(pool->users, (unsigned char *)userId, strlen(userId), NULL);
    atomic_fetch_sub(&pool->userCount, 1);

    pthread_rwlock_unlock(&pool->lock);

    freeUser(user);
    return true;
}

loopyConcurrencyUser *loopyConcurrencyPoolGetUser(loopyConcurrencyPool *pool,
                                                  const char *userId) {
    if (!pool || !userId) {
        return NULL;
    }

    pthread_rwlock_rdlock(&pool->lock);

    loopyConcurrencyUser *user =
        raxFind(pool->users, (unsigned char *)userId, strlen(userId));
    if (user == RAX_NOT_FOUND) {
        user = NULL;
    }

    pthread_rwlock_unlock(&pool->lock);

    return user;
}

bool loopyConcurrencyUserUpdate(loopyConcurrencyUser *user,
                                const loopyConcurrencyUserConfig *config) {
    if (!user || !config) {
        return false;
    }

    /* Note: Not locking here as individual fields are either atomic or
     * rarely updated. For full consistency, would need locking. */
    user->limit = config->limit;
    user->reserved = config->reserved;
    user->priority = config->priority;
    user->userData = config->userData;

    return true;
}

const char *loopyConcurrencyUserId(const loopyConcurrencyUser *user) {
    return user ? user->userId : NULL;
}

void *loopyConcurrencyUserData(const loopyConcurrencyUser *user) {
    return user ? user->userData : NULL;
}

/* ====================================================================
 * Slot Acquisition and Release
 * ==================================================================== */

loopyConcurrencyResult
loopyConcurrencyUserTryAcquire(loopyConcurrencyUser *user, size_t count) {
    if (!user || count == 0) {
        return LOOPY_CONCURRENCY_INVALID;
    }

    loopyConcurrencyPool *pool = user->pool;
    size_t globalLimit = pool->config.globalLimit;

    /*
     * CRITICAL: Both user limit and global limit must be enforced atomically.
     *
     * Algorithm:
     * 1. Try to reserve global slots first (if global limit is set)
     * 2. Then try to reserve user slots
     * 3. If user reservation fails, roll back global reservation
     *
     * This prevents the race where we reserve user slots but global
     * goes over limit due to concurrent acquisitions.
     */

    /* Step 1: Reserve global slots atomically if global limit is set */
    size_t newGlobal = 0;
    if (globalLimit > 0) {
        while (true) {
            size_t globalActive = atomic_load(&pool->globalActive);
            if (globalActive + count > globalLimit) {
                if (pool->config.trackStats) {
                    atomic_fetch_add(&user->denied, 1);
                    atomic_fetch_add(&pool->totalDenied, 1);
                }
                return LOOPY_CONCURRENCY_GLOBAL_LIMIT;
            }

            if (atomic_compare_exchange_weak(&pool->globalActive, &globalActive,
                                             globalActive + count)) {
                newGlobal = globalActive + count;
                break;
            }
        }
    }

    /* Step 2: Reserve user slots atomically */
    size_t active;
    while (true) {
        active = atomic_load(&user->active);
        if (active + count > user->limit) {
            /* Roll back global reservation if we made one */
            if (globalLimit > 0) {
                atomic_fetch_sub(&pool->globalActive, count);
            }
            if (pool->config.trackStats) {
                atomic_fetch_add(&user->denied, 1);
                atomic_fetch_add(&pool->totalDenied, 1);
            }
            return LOOPY_CONCURRENCY_LIMIT;
        }

        if (atomic_compare_exchange_weak(&user->active, &active,
                                         active + count)) {
            break;
        }
    }

    /* Step 3: If no global limit, update global counter now */
    if (globalLimit == 0) {
        newGlobal = atomic_fetch_add(&pool->globalActive, count) + count;
    }

    /* Update peaks */
    updatePeak(&user->peak, active + count);
    updatePeak(&pool->globalPeak, newGlobal);

    /* Update stats */
    if (pool->config.trackStats) {
        atomic_fetch_add(&user->acquires, 1);
        atomic_fetch_add(&pool->totalAcquires, 1);
    }

    return LOOPY_CONCURRENCY_OK;
}

loopyConcurrencyResult
loopyConcurrencyPoolTryAcquire(loopyConcurrencyPool *pool, const char *userId,
                               size_t count) {
    if (!pool || !userId || count == 0) {
        return LOOPY_CONCURRENCY_INVALID;
    }

    loopyConcurrencyUser *user = loopyConcurrencyPoolGetUser(pool, userId);

    /* Auto-create user if enabled */
    if (!user && pool->config.autoCreateUsers) {
        user = loopyConcurrencyPoolAddUser(pool, userId, NULL);
    }

    if (!user) {
        return LOOPY_CONCURRENCY_NOT_FOUND;
    }

    return loopyConcurrencyUserTryAcquire(user, count);
}

bool loopyConcurrencyUserRelease(loopyConcurrencyUser *user, size_t count) {
    if (!user || count == 0) {
        return false;
    }

    loopyConcurrencyPool *pool = user->pool;

    /* Atomically decrease, ensuring we don't go negative */
    size_t active;
    while (true) {
        active = atomic_load(&user->active);
        if (count > active) {
            count = active; /* Can only release what's held */
        }
        if (count == 0) {
            return true;
        }

        if (atomic_compare_exchange_weak(&user->active, &active,
                                         active - count)) {
            break;
        }
    }

    /* Update global counter */
    atomic_fetch_sub(&pool->globalActive, count);

    /* Update stats */
    if (pool->config.trackStats) {
        atomic_fetch_add(&user->releases, 1);
        atomic_fetch_add(&pool->totalReleases, 1);
    }

    return true;
}

bool loopyConcurrencyPoolRelease(loopyConcurrencyPool *pool, const char *userId,
                                 size_t count) {
    if (!pool || !userId || count == 0) {
        return false;
    }

    loopyConcurrencyUser *user = loopyConcurrencyPoolGetUser(pool, userId);
    if (!user) {
        return false;
    }

    return loopyConcurrencyUserRelease(user, count);
}

size_t loopyConcurrencyUserReleaseAll(loopyConcurrencyUser *user) {
    if (!user) {
        return 0;
    }

    size_t active = atomic_exchange(&user->active, 0);
    if (active > 0) {
        atomic_fetch_sub(&user->pool->globalActive, active);
        if (user->pool->config.trackStats) {
            atomic_fetch_add(&user->releases, 1);
            atomic_fetch_add(&user->pool->totalReleases, 1);
        }
    }

    return active;
}

size_t loopyConcurrencyPoolReleaseAll(loopyConcurrencyPool *pool,
                                      const char *userId) {
    if (!pool || !userId) {
        return 0;
    }

    loopyConcurrencyUser *user = loopyConcurrencyPoolGetUser(pool, userId);
    if (!user) {
        return 0;
    }

    return loopyConcurrencyUserReleaseAll(user);
}

/* ====================================================================
 * Querying
 * ==================================================================== */

size_t loopyConcurrencyUserAvailable(const loopyConcurrencyUser *user) {
    if (!user) {
        return 0;
    }

    size_t active = atomic_load(&user->active);
    if (active >= user->limit) {
        return 0;
    }
    return user->limit - active;
}

size_t loopyConcurrencyAvailable(loopyConcurrencyPool *pool,
                                 const char *userId) {
    if (!pool || !userId) {
        return 0;
    }

    loopyConcurrencyUser *user = loopyConcurrencyPoolGetUser(pool, userId);
    if (!user) {
        return 0;
    }

    return loopyConcurrencyUserAvailable(user);
}

size_t loopyConcurrencyUserActive(const loopyConcurrencyUser *user) {
    if (!user) {
        return 0;
    }
    return atomic_load(&user->active);
}

size_t loopyConcurrencyActive(loopyConcurrencyPool *pool, const char *userId) {
    if (!pool || !userId) {
        return 0;
    }

    loopyConcurrencyUser *user = loopyConcurrencyPoolGetUser(pool, userId);
    if (!user) {
        return 0;
    }

    return loopyConcurrencyUserActive(user);
}

bool loopyConcurrencyCanAcquire(loopyConcurrencyPool *pool, const char *userId,
                                size_t count) {
    if (!pool || !userId || count == 0) {
        return false;
    }

    loopyConcurrencyUser *user = loopyConcurrencyPoolGetUser(pool, userId);
    if (!user) {
        /* Would auto-create, so check global limit only */
        if (pool->config.autoCreateUsers) {
            if (pool->config.globalLimit > 0) {
                return atomic_load(&pool->globalActive) + count <=
                       pool->config.globalLimit;
            }
            return true;
        }
        return false;
    }

    size_t active = atomic_load(&user->active);
    if (active + count > user->limit) {
        return false;
    }

    if (pool->config.globalLimit > 0) {
        if (atomic_load(&pool->globalActive) + count >
            pool->config.globalLimit) {
            return false;
        }
    }

    return true;
}

size_t loopyConcurrencyPoolAvailable(const loopyConcurrencyPool *pool) {
    if (!pool) {
        return 0;
    }
    if (pool->config.globalLimit == 0) {
        return SIZE_MAX;
    }

    size_t active = atomic_load(&pool->globalActive);
    if (active >= pool->config.globalLimit) {
        return 0;
    }
    return pool->config.globalLimit - active;
}

size_t loopyConcurrencyPoolActive(const loopyConcurrencyPool *pool) {
    if (!pool) {
        return 0;
    }
    return atomic_load(&pool->globalActive);
}

/* ====================================================================
 * Statistics
 * ==================================================================== */

void loopyConcurrencyPoolGetStats(const loopyConcurrencyPool *pool,
                                  loopyConcurrencyPoolStats *stats) {
    if (!pool || !stats) {
        return;
    }

    memset(stats, 0, sizeof(*stats));
    stats->globalLimit = pool->config.globalLimit;
    stats->globalActive = atomic_load(&pool->globalActive);
    stats->globalPeak = atomic_load(&pool->globalPeak);
    stats->userCount = atomic_load(&pool->userCount);
    stats->totalAcquires = atomic_load(&pool->totalAcquires);
    stats->totalReleases = atomic_load(&pool->totalReleases);
    stats->totalDenied = atomic_load(&pool->totalDenied);
    stats->totalWaits = atomic_load(&pool->totalWaits);
}

void loopyConcurrencyUserGetStats(const loopyConcurrencyUser *user,
                                  loopyConcurrencyUserStats *stats) {
    if (!user || !stats) {
        return;
    }

    memset(stats, 0, sizeof(*stats));
    stats->limit = user->limit;
    stats->active = atomic_load(&user->active);
    stats->peak = atomic_load(&user->peak);
    stats->reserved = user->reserved;
    stats->acquires = atomic_load(&user->acquires);
    stats->releases = atomic_load(&user->releases);
    stats->denied = atomic_load(&user->denied);
    stats->waits = atomic_load(&user->waits);
}

void loopyConcurrencyPoolResetStats(loopyConcurrencyPool *pool) {
    if (!pool) {
        return;
    }

    atomic_store(&pool->globalPeak, atomic_load(&pool->globalActive));
    atomic_store(&pool->totalAcquires, 0);
    atomic_store(&pool->totalReleases, 0);
    atomic_store(&pool->totalDenied, 0);
    atomic_store(&pool->totalWaits, 0);
}

void loopyConcurrencyUserResetStats(loopyConcurrencyUser *user) {
    if (!user) {
        return;
    }

    atomic_store(&user->peak, atomic_load(&user->active));
    atomic_store(&user->acquires, 0);
    atomic_store(&user->releases, 0);
    atomic_store(&user->denied, 0);
    atomic_store(&user->waits, 0);
}

/* ====================================================================
 * Iteration
 * ==================================================================== */

size_t loopyConcurrencyPoolIterate(loopyConcurrencyPool *pool,
                                   loopyConcurrencyUserIterFn *fn, void *arg) {
    if (!pool || !fn) {
        return 0;
    }

    size_t count = 0;

    pthread_rwlock_rdlock(&pool->lock);

    raxIterator iter;
    raxStart(&iter, pool->users);
    raxSeek(&iter, "^", NULL, 0);

    while (raxNext(&iter)) {
        loopyConcurrencyUser *user = iter.data;
        count++;

        /* Create null-terminated userId for callback */
        char userId[256];
        size_t len = iter.keyLen < 255 ? iter.keyLen : 255;
        memcpy(userId, iter.key, len);
        userId[len] = '\0';

        if (fn(userId, user, arg) != 0) {
            break;
        }
    }

    raxStop(&iter);
    pthread_rwlock_unlock(&pool->lock);

    return count;
}

/* ====================================================================
 * Utility
 * ==================================================================== */

const char *loopyConcurrencyResultName(loopyConcurrencyResult result) {
    switch (result) {
    case LOOPY_CONCURRENCY_OK:
        return "OK";
    case LOOPY_CONCURRENCY_LIMIT:
        return "LIMIT";
    case LOOPY_CONCURRENCY_GLOBAL_LIMIT:
        return "GLOBAL_LIMIT";
    case LOOPY_CONCURRENCY_INVALID:
        return "INVALID";
    case LOOPY_CONCURRENCY_NOT_FOUND:
        return "NOT_FOUND";
    case LOOPY_CONCURRENCY_TIMEOUT:
        return "TIMEOUT";
    default:
        return "UNKNOWN";
    }
}

size_t loopyConcurrencyPoolUserCount(const loopyConcurrencyPool *pool) {
    if (!pool) {
        return 0;
    }
    return atomic_load(&pool->userCount);
}
