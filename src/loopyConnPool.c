/* loopyConnPool - Connection Pooling Implementation
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

#include "loopy.h"
#include "loopyConnPool.h"
#include "loopyTimer.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "../deps/datakit/src/datakit.h"

/* ====================================================================
 * Internal Structures
 * ==================================================================== */

/**
 * Pending async acquisition request.
 */
typedef struct WaitRequest {
    loopyConnPoolAcquireAsyncCallback *cb;
    void *userData;
    struct WaitRequest *next;
} WaitRequest;

/**
 * Pool connection internal structure.
 */
struct loopyConnPoolConn {
    void *userConn;                 /* User connection from create callback */
    loopyConnPool *pool;            /* Parent pool */
    uint64_t createTime;            /* Creation timestamp (microseconds) */
    uint64_t lastUseTime;           /* Last use timestamp (microseconds) */
    bool inUse;                     /* Is connection currently in use? */
    struct loopyConnPoolConn *next; /* Next in idle list */
};

/**
 * Pool internal structure.
 */
struct loopyConnPool {
    loopyLoop *loop;
    loopyConnPoolConfig config;

    /* Lifecycle callbacks */
    loopyConnPoolCreateCallback *createCb;
    loopyConnPoolFreeCallback *destroyCb;
    loopyConnPoolValidateCallback *validateCb;
    loopyConnPoolAcquireCallback *acquireCb;
    loopyConnPoolReleaseCallback *releaseCb;
    void *userData;

    /* Connection tracking */
    loopyConnPoolConn *idleConns; /* Linked list of idle connections */
    uint32_t totalConns;          /* Total connections (idle + active) */
    uint32_t idleCount;           /* Number of idle connections */
    uint32_t activeCount;         /* Number of active connections */

    /* Wait queue for async acquisition */
    WaitRequest *waitQueueHead;
    WaitRequest *waitQueueTail;

    /* Timers for maintenance */
    loopyTimer *healthCheckTimer;
    loopyTimer *idleCleanupTimer;

    /* Statistics */
    uint64_t totalAcquires;
    uint64_t totalReleases;
    uint64_t totalCreates;
    uint64_t totalDestroys;
    uint64_t totalTimeouts;
    uint64_t totalValidationFailures;

    /* State */
    bool destroyed;
};

/* Thread-local error storage */
static __thread char lastError[256] = {0};

/* ====================================================================
 * Error Handling
 * ==================================================================== */

static void setError(const char *msg) {
    strncpy(lastError, msg, sizeof(lastError) - 1);
    lastError[sizeof(lastError) - 1] = '\0';
}

const char *loopyConnPoolGetError(void) {
    return lastError[0] ? lastError : NULL;
}

/* ====================================================================
 * Time Utilities
 * ==================================================================== */

static uint64_t getMicroSeconds(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + (uint64_t)tv.tv_usec;
}

/* ====================================================================
 * Configuration
 * ==================================================================== */

loopyConnPoolConfig loopyConnPoolConfigDefault(void) {
    loopyConnPoolConfig cfg;
    cfg.minIdle = 0;
    cfg.maxTotal = 10;
    cfg.idleTimeoutUs = 30000000;         /* 30 seconds */
    cfg.maxLifetimeUs = 3600000000;       /* 1 hour */
    cfg.healthCheckIntervalUs = 60000000; /* 60 seconds */
    cfg.acquireTimeoutUs = 5000000;       /* 5 seconds */
    return cfg;
}

/* ====================================================================
 * Connection Management
 * ==================================================================== */

static loopyConnPoolConn *createConnection(loopyConnPool *pool) {
    if (pool->totalConns >= pool->config.maxTotal) {
        setError("Pool at maximum capacity");
        return NULL;
    }

    /* Call user creation callback */
    void *userConn = pool->createCb(pool->userData);
    if (!userConn) {
        setError("Connection creation callback failed");
        return NULL;
    }

    /* Allocate pool connection wrapper */
    loopyConnPoolConn *conn = zcalloc(1, sizeof(loopyConnPoolConn));
    if (!conn) {
        setError("Memory allocation failed");
        pool->destroyCb(userConn, pool->userData);
        return NULL;
    }

    uint64_t now = getMicroSeconds();
    conn->userConn = userConn;
    conn->pool = pool;
    conn->createTime = now;
    conn->lastUseTime = now;
    conn->inUse = false;
    conn->next = NULL;

    pool->totalConns++;
    pool->totalCreates++;

    lastError[0] = '\0';
    return conn;
}

static void destroyConnection(loopyConnPool *pool, loopyConnPoolConn *conn) {
    if (!conn) {
        return;
    }

    /* Call user destruction callback */
    if (conn->userConn && pool->destroyCb) {
        pool->destroyCb(conn->userConn, pool->userData);
    }

    pool->totalConns--;
    pool->totalDestroys++;

    zfree(conn);
}

static bool validateConnection(loopyConnPool *pool, loopyConnPoolConn *conn) {
    if (!conn || !conn->userConn) {
        return false;
    }

    /* If no validation callback, assume healthy */
    if (!pool->validateCb) {
        return true;
    }

    return pool->validateCb(conn->userConn, pool->userData);
}

static bool isConnectionExpired(loopyConnPool *pool, loopyConnPoolConn *conn) {
    if (pool->config.maxLifetimeUs == 0) {
        return false;
    }

    uint64_t now = getMicroSeconds();
    uint64_t age = now - conn->createTime;
    return age >= pool->config.maxLifetimeUs;
}

static bool isConnectionIdleTooLong(loopyConnPool *pool,
                                    loopyConnPoolConn *conn) {
    if (pool->config.idleTimeoutUs == 0) {
        return false;
    }

    uint64_t now = getMicroSeconds();
    uint64_t idleTime = now - conn->lastUseTime;
    return idleTime >= pool->config.idleTimeoutUs;
}

/* ====================================================================
 * Idle Connection List Management
 * ==================================================================== */

static void addToIdleList(loopyConnPool *pool, loopyConnPoolConn *conn) {
    conn->next = pool->idleConns;
    pool->idleConns = conn;
    pool->idleCount++;
}

static loopyConnPoolConn *removeFromIdleList(loopyConnPool *pool) {
    if (!pool->idleConns) {
        return NULL;
    }

    loopyConnPoolConn *conn = pool->idleConns;
    pool->idleConns = conn->next;
    conn->next = NULL;
    pool->idleCount--;
    pool->activeCount++;

    return conn;
}

/* ====================================================================
 * Wait Queue Management
 * ==================================================================== */

static void enqueueWaitRequest(loopyConnPool *pool,
                               loopyConnPoolAcquireAsyncCallback *cb,
                               void *userData) {
    WaitRequest *req = zcalloc(1, sizeof(WaitRequest));
    if (!req) {
        return;
    }

    req->cb = cb;
    req->userData = userData;
    req->next = NULL;

    if (!pool->waitQueueHead) {
        pool->waitQueueHead = req;
        pool->waitQueueTail = req;
    } else {
        pool->waitQueueTail->next = req;
        pool->waitQueueTail = req;
    }
}

static WaitRequest *dequeueWaitRequest(loopyConnPool *pool) {
    if (!pool->waitQueueHead) {
        return NULL;
    }

    WaitRequest *req = pool->waitQueueHead;
    pool->waitQueueHead = req->next;

    if (!pool->waitQueueHead) {
        pool->waitQueueTail = NULL;
    }

    return req;
}

/* ====================================================================
 * Maintenance Callbacks
 * ==================================================================== */

static void healthCheckCallback(loopyLoop *l, loopyTimer *timer,
                                void *userData) {
    (void)l;
    (void)timer;

    loopyConnPool *pool = (loopyConnPool *)userData;
    loopyConnPoolHealthCheck(pool);
}

static void idleCleanupCallback(loopyLoop *l, loopyTimer *timer,
                                void *userData) {
    (void)l;
    (void)timer;

    loopyConnPool *pool = (loopyConnPool *)userData;
    loopyConnPoolCleanupIdle(pool);
}

/* ====================================================================
 * Pool Creation and Destruction
 * ==================================================================== */

loopyConnPool *loopyConnPoolNew(loopyLoop *loop,
                                const loopyConnPoolConfig *config,
                                loopyConnPoolCreateCallback *createCb,
                                loopyConnPoolFreeCallback *destroyCb,
                                loopyConnPoolValidateCallback *validateCb,
                                void *userData) {
    if (!loop) {
        setError("NULL event loop");
        return NULL;
    }

    if (!config) {
        setError("NULL configuration");
        return NULL;
    }

    if (!createCb || !destroyCb) {
        setError("NULL required callback");
        return NULL;
    }

    /* Allocate pool structure */
    loopyConnPool *pool = zcalloc(1, sizeof(loopyConnPool));
    if (!pool) {
        setError("Memory allocation failed");
        return NULL;
    }

    /* Initialize pool */
    pool->loop = loop;
    pool->config = *config;
    pool->createCb = createCb;
    pool->destroyCb = destroyCb;
    pool->validateCb = validateCb;
    pool->acquireCb = NULL;
    pool->releaseCb = NULL;
    pool->userData = userData;
    pool->idleConns = NULL;
    pool->totalConns = 0;
    pool->idleCount = 0;
    pool->activeCount = 0;
    pool->waitQueueHead = NULL;
    pool->waitQueueTail = NULL;
    pool->healthCheckTimer = NULL;
    pool->idleCleanupTimer = NULL;
    pool->destroyed = false;

    /* Create periodic health check timer if configured */
    if (config->healthCheckIntervalUs > 0) {
        pool->healthCheckTimer = loopyTimerPeriodic(
            loop, config->healthCheckIntervalUs, healthCheckCallback, pool);
        if (!pool->healthCheckTimer) {
            zfree(pool);
            setError("Failed to create health check timer");
            return NULL;
        }
    }

    /* Create periodic idle cleanup timer */
    if (config->idleTimeoutUs > 0) {
        /* Run cleanup every half of idle timeout */
        uint64_t cleanupInterval = config->idleTimeoutUs / 2;
        if (cleanupInterval < 1000000) {
            cleanupInterval = 1000000; /* Min 1 second */
        }

        pool->idleCleanupTimer = loopyTimerPeriodic(loop, cleanupInterval,
                                                    idleCleanupCallback, pool);
        if (!pool->idleCleanupTimer) {
            if (pool->healthCheckTimer) {
                loopyTimerCancel(pool->healthCheckTimer);
            }
            zfree(pool);
            setError("Failed to create idle cleanup timer");
            return NULL;
        }
    }

    /* Prewarm pool with minIdle connections */
    if (config->minIdle > 0) {
        uint32_t created = loopyConnPoolPrewarm(pool, config->minIdle);
        if (created < config->minIdle) {
            /* Failed to create minimum connections - destroy pool */
            loopyConnPoolFree(pool);
            setError("Failed to create minimum idle connections");
            return NULL;
        }
    }

    lastError[0] = '\0';
    return pool;
}

void loopyConnPoolFree(loopyConnPool *pool) {
    if (!pool) {
        return;
    }

    pool->destroyed = true;

    /* Cancel timers */
    if (pool->healthCheckTimer) {
        loopyTimerCancel(pool->healthCheckTimer);
        pool->healthCheckTimer = NULL;
    }

    if (pool->idleCleanupTimer) {
        loopyTimerCancel(pool->idleCleanupTimer);
        pool->idleCleanupTimer = NULL;
    }

    /* Destroy all idle connections */
    while (pool->idleConns) {
        loopyConnPoolConn *conn = removeFromIdleList(pool);
        destroyConnection(pool, conn);
    }

    /* Clean up wait queue */
    while (pool->waitQueueHead) {
        WaitRequest *req = dequeueWaitRequest(pool);
        if (req->cb) {
            req->cb(NULL, req->userData);
        }
        zfree(req);
    }

    /* Note: Active connections will be destroyed when released */

    zfree(pool);
}

void loopyConnPoolSetAcquireCallback(loopyConnPool *pool,
                                     loopyConnPoolAcquireCallback *acquireCb) {
    if (pool) {
        pool->acquireCb = acquireCb;
    }
}

void loopyConnPoolSetReleaseCallback(loopyConnPool *pool,
                                     loopyConnPoolReleaseCallback *releaseCb) {
    if (pool) {
        pool->releaseCb = releaseCb;
    }
}

/* ====================================================================
 * Connection Acquisition and Release
 * ==================================================================== */

loopyConnPoolConn *loopyConnPoolAcquire(loopyConnPool *pool) {
    if (!pool || pool->destroyed) {
        setError("Invalid or destroyed pool");
        return NULL;
    }

    loopyConnPoolConn *conn = NULL;

    /* Try to get idle connection */
    while (pool->idleConns) {
        conn = removeFromIdleList(pool);
        if (!conn) {
            break;
        }

        /* Validate connection */
        if (!validateConnection(pool, conn)) {
            pool->totalValidationFailures++;
            destroyConnection(pool, conn);
            conn = NULL;
            continue;
        }

        /* Check if connection expired */
        if (isConnectionExpired(pool, conn)) {
            destroyConnection(pool, conn);
            conn = NULL;
            continue;
        }

        /* Connection is valid */
        break;
    }

    /* If no idle connection, create a new one */
    if (!conn) {
        conn = createConnection(pool);
        if (!conn) {
            pool->totalTimeouts++;
            return NULL;
        }
        pool->activeCount++;
    }

    /* Update usage time */
    conn->lastUseTime = getMicroSeconds();
    conn->inUse = true;

    /* Call acquire callback */
    if (pool->acquireCb && conn->userConn) {
        pool->acquireCb(conn->userConn, pool->userData);
    }

    pool->totalAcquires++;
    lastError[0] = '\0';
    return conn;
}

bool loopyConnPoolAcquireAsync(loopyConnPool *pool,
                               loopyConnPoolAcquireAsyncCallback *cb,
                               void *userData) {
    if (!pool || !cb) {
        setError("NULL pool or callback");
        return false;
    }

    if (pool->destroyed) {
        setError("Pool is destroyed");
        return false;
    }

    /* Try immediate acquisition */
    if (pool->idleConns || pool->totalConns < pool->config.maxTotal) {
        loopyConnPoolConn *conn = loopyConnPoolAcquire(pool);
        cb(conn, userData);
        return true;
    }

    /* No connection available - enqueue request */
    enqueueWaitRequest(pool, cb, userData);
    lastError[0] = '\0';
    return true;
}

void loopyConnPoolRelease(loopyConnPoolConn *conn, bool healthy) {
    if (!conn || !conn->pool) {
        return;
    }

    loopyConnPool *pool = conn->pool;
    conn->inUse = false;

    /* Call release callback */
    if (pool->releaseCb && conn->userConn) {
        pool->releaseCb(conn->userConn, pool->userData);
    }

    pool->totalReleases++;

    /* If pool is destroyed, destroy connection immediately */
    if (pool->destroyed) {
        destroyConnection(pool, conn);
        return;
    }

    /* If connection is unhealthy or expired, destroy it */
    if (!healthy || isConnectionExpired(pool, conn)) {
        pool->activeCount--;
        destroyConnection(pool, conn);

        /* Try to create replacement if below minIdle */
        if (pool->totalConns < pool->config.minIdle) {
            loopyConnPoolConn *newConn = createConnection(pool);
            if (newConn) {
                addToIdleList(pool, newConn);
            }
        }

        return;
    }

    /* Check if there are waiting requests */
    if (pool->waitQueueHead) {
        WaitRequest *req = dequeueWaitRequest(pool);
        if (req) {
            conn->inUse = true;
            conn->lastUseTime = getMicroSeconds();

            if (pool->acquireCb && conn->userConn) {
                pool->acquireCb(conn->userConn, pool->userData);
            }

            if (req->cb) {
                req->cb(conn, req->userData);
            }

            zfree(req);
            pool->totalAcquires++;
            return;
        }
    }

    /* Return to idle pool */
    conn->lastUseTime = getMicroSeconds();
    pool->activeCount--;
    addToIdleList(pool, conn);
}

void *loopyConnPoolGetUserConn(const loopyConnPoolConn *conn) {
    return conn ? conn->userConn : NULL;
}

/* ====================================================================
 * Pool Statistics
 * ==================================================================== */

bool loopyConnPoolGetStats(const loopyConnPool *pool,
                           loopyConnPoolStats *stats) {
    if (!pool || !stats) {
        return false;
    }

    stats->totalConns = pool->totalConns;
    stats->idleConns = pool->idleCount;
    stats->activeConns = pool->activeCount;
    stats->totalAcquires = pool->totalAcquires;
    stats->totalReleases = pool->totalReleases;
    stats->totalCreates = pool->totalCreates;
    stats->totalDestroys = pool->totalDestroys;
    stats->totalTimeouts = pool->totalTimeouts;
    stats->totalValidationFailures = pool->totalValidationFailures;

    return true;
}

uint32_t loopyConnPoolGetIdleCount(const loopyConnPool *pool) {
    return pool ? pool->idleCount : 0;
}

uint32_t loopyConnPoolGetActiveCount(const loopyConnPool *pool) {
    return pool ? pool->activeCount : 0;
}

uint32_t loopyConnPoolGetTotalCount(const loopyConnPool *pool) {
    return pool ? pool->totalConns : 0;
}

/* ====================================================================
 * Pool Maintenance
 * ==================================================================== */

uint32_t loopyConnPoolHealthCheck(loopyConnPool *pool) {
    if (!pool || pool->destroyed) {
        return 0;
    }

    uint32_t destroyed = 0;
    loopyConnPoolConn *prev = NULL;
    loopyConnPoolConn *curr = pool->idleConns;

    while (curr) {
        loopyConnPoolConn *next = curr->next;

        if (!validateConnection(pool, curr)) {
            /* Remove from list */
            if (prev) {
                prev->next = next;
            } else {
                pool->idleConns = next;
            }

            pool->idleCount--;
            pool->totalValidationFailures++;
            destroyConnection(pool, curr);
            destroyed++;
        } else {
            prev = curr;
        }

        curr = next;
    }

    return destroyed;
}

uint32_t loopyConnPoolCleanupIdle(loopyConnPool *pool) {
    if (!pool || pool->destroyed) {
        return 0;
    }

    uint32_t closed = 0;
    loopyConnPoolConn *prev = NULL;
    loopyConnPoolConn *curr = pool->idleConns;

    while (curr) {
        loopyConnPoolConn *next = curr->next;

        if (isConnectionIdleTooLong(pool, curr) &&
            pool->totalConns > pool->config.minIdle) {
            /* Remove from list */
            if (prev) {
                prev->next = next;
            } else {
                pool->idleConns = next;
            }

            pool->idleCount--;
            destroyConnection(pool, curr);
            closed++;
        } else {
            prev = curr;
        }

        curr = next;
    }

    return closed;
}

uint32_t loopyConnPoolPrewarm(loopyConnPool *pool, uint32_t count) {
    if (!pool || pool->destroyed) {
        return 0;
    }

    if (count == 0) {
        count = pool->config.minIdle;
    }

    uint32_t created = 0;

    for (uint32_t i = 0; i < count && pool->totalConns < pool->config.maxTotal;
         i++) {
        loopyConnPoolConn *conn = createConnection(pool);
        if (!conn) {
            break;
        }

        addToIdleList(pool, conn);
        created++;
    }

    return created;
}
