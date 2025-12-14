/* loopyChannel - Thread-safe inter-thread communication channels
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
#include "loopyChannel.h"

#include "../deps/datakit/src/datakit.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

/* ====================================================================
 * Constants and Macros
 * ==================================================================== */

/* Cache line size for padding to prevent false sharing */
#define CACHE_LINE_SIZE 64

/* Align to cache line boundary */
#define CACHE_ALIGNED __attribute__((aligned(CACHE_LINE_SIZE)))

/* Global channel ID counter */
static _Atomic uint64_t globalChannelId = 0;

/* ====================================================================
 * Internal Structures
 * ==================================================================== */

/**
 * SPSC lock-free ring buffer implementation.
 *
 * Uses C11 atomics for memory ordering. Head is written by producer,
 * tail by consumer. Cache line padding prevents false sharing.
 */
typedef struct loopyChannelSPSC {
    uint8_t *buffer;    /* Ring buffer storage */
    size_t capacity;    /* Number of elements */
    size_t elementSize; /* Size of each element */
    size_t mask;        /* capacity - 1, for fast modulo */

    /* Producer side - cache line aligned */
    CACHE_ALIGNED _Atomic size_t head; /* Next write position */
    size_t cachedTail;                 /* Cached tail for producer */

    /* Consumer side - cache line aligned */
    CACHE_ALIGNED _Atomic size_t tail; /* Next read position */
    size_t cachedHead;                 /* Cached head for consumer */
} loopyChannelSPSC;

/**
 * MPSC/MPMC mutex-based implementation.
 */
typedef struct loopyChannelMPMC {
    uint8_t *buffer;    /* Ring buffer storage */
    size_t capacity;    /* Number of elements */
    size_t elementSize; /* Size of each element */
    size_t head;        /* Next write position */
    size_t tail;        /* Next read position */
    size_t count;       /* Current number of elements */

    pthread_mutex_t mutex;   /* Protects all fields */
    pthread_cond_t notEmpty; /* Signaled when data available */
    pthread_cond_t notFull;  /* Signaled when space available */
} loopyChannelMPMC;

/**
 * Async operation pending structure.
 */
typedef struct loopyChannelPendingOp {
    struct loopyChannelPendingOp *next;
    union {
        struct {
            loopyChannelSendCallback *cb;
            void *data;
            size_t len;
        } send;
        struct {
            loopyChannelRecvCallback *cb;
        } recv;
    } op;
    void *userData;
    bool isSend;
} loopyChannelPendingOp;

/**
 * Main channel structure.
 */
struct loopyChannel {
    loopyChannelId id;     /* Unique identifier */
    loopyChannelType type; /* SPSC, MPSC, or MPMC */
    loopyLoop *loop;       /* Associated event loop (may be NULL) */
    void *userData;        /* User data for handle accessors */

    union {
        loopyChannelSPSC spsc;
        loopyChannelMPMC mpmc;
    } impl;

    loopyChannelConfig config; /* Original configuration */
    _Atomic bool closed;       /* Channel closed flag */

    /* Async support */
    loopyAsync *async;                 /* For waking the event loop */
    loopyChannelPendingOp *pendingOps; /* Pending async operations */
    pthread_mutex_t asyncMutex;        /* Protects pendingOps list */

    /* Statistics */
    _Atomic uint64_t totalSent;
    _Atomic uint64_t totalReceived;
    _Atomic uint64_t sendBlocked;
    _Atomic uint64_t recvBlocked;
    _Atomic uint64_t sendFailed;
    _Atomic uint64_t recvFailed;
    _Atomic uint64_t peakUsage;
};

/* ====================================================================
 * Helper Functions
 * ==================================================================== */

/* Round up to next power of 2 */
static size_t nextPowerOf2(size_t n) {
    if (n == 0) {
        return 1;
    }
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n |= n >> 32;
    return n + 1;
}

/* Get current time in microseconds */
static uint64_t getMicroseconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* Convert microseconds to timespec */
static struct timespec usToTimespec(uint64_t us) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += us / 1000000;
    ts.tv_nsec += (us % 1000000) * 1000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    return ts;
}

/* Update peak usage statistic */
static void updatePeakUsage(loopyChannel *ch, size_t current) {
    uint64_t peak = atomic_load(&ch->peakUsage);
    while (current > peak) {
        if (atomic_compare_exchange_weak(&ch->peakUsage, &peak, current)) {
            break;
        }
    }
}

/* ====================================================================
 * SPSC Implementation
 * ==================================================================== */

static bool spscInit(loopyChannelSPSC *spsc, size_t capacity,
                     size_t elementSize) {
    /* Ring buffer needs +1 slot to distinguish full from empty.
     * We round up to power of 2 for fast modulo. */
    size_t actualCapacity = nextPowerOf2(capacity + 1);

    spsc->buffer = zmalloc(actualCapacity * elementSize);
    if (!spsc->buffer) {
        return false;
    }

    spsc->capacity = capacity; /* User-requested capacity */
    spsc->elementSize = elementSize;
    spsc->mask = actualCapacity - 1;
    atomic_store(&spsc->head, 0);
    atomic_store(&spsc->tail, 0);
    spsc->cachedTail = 0;
    spsc->cachedHead = 0;

    return true;
}

static void spscDestroy(loopyChannelSPSC *spsc) {
    zfree(spsc->buffer);
    spsc->buffer = NULL;
}

static bool spscTrySend(loopyChannelSPSC *spsc, const void *data, size_t len) {
    if (len != spsc->elementSize) {
        return false;
    }

    size_t head = atomic_load_explicit(&spsc->head, memory_order_relaxed);

    /* Check if full using cached tail and count */
    size_t count = (head - spsc->cachedTail) & spsc->mask;
    if (count >= spsc->capacity) {
        /* Refresh cached tail */
        spsc->cachedTail =
            atomic_load_explicit(&spsc->tail, memory_order_acquire);
        count = (head - spsc->cachedTail) & spsc->mask;
        if (count >= spsc->capacity) {
            return false; /* Still full */
        }
    }

    /* Copy data into buffer */
    memcpy(spsc->buffer + (head & spsc->mask) * spsc->elementSize, data, len);

    /* Publish write */
    size_t next = head + 1;
    atomic_store_explicit(&spsc->head, next, memory_order_release);
    return true;
}

static ssize_t spscTryRecv(loopyChannelSPSC *spsc, void *buf, size_t bufLen) {
    if (bufLen < spsc->elementSize) {
        return LOOPY_CHANNEL_INVALID;
    }

    size_t tail = atomic_load_explicit(&spsc->tail, memory_order_relaxed);

    /* Check if empty using cached head */
    if (tail == spsc->cachedHead) {
        /* Refresh cached head */
        spsc->cachedHead =
            atomic_load_explicit(&spsc->head, memory_order_acquire);
        if (tail == spsc->cachedHead) {
            return LOOPY_CHANNEL_EMPTY; /* Still empty */
        }
    }

    /* Copy data from buffer */
    memcpy(buf, spsc->buffer + (tail & spsc->mask) * spsc->elementSize,
           spsc->elementSize);

    /* Publish read */
    size_t next = tail + 1;
    atomic_store_explicit(&spsc->tail, next, memory_order_release);

    return (ssize_t)spsc->elementSize;
}

static size_t spscLen(const loopyChannelSPSC *spsc) {
    size_t head = atomic_load_explicit(&spsc->head, memory_order_acquire);
    size_t tail = atomic_load_explicit(&spsc->tail, memory_order_acquire);
    /* head and tail grow without wrapping; difference is the count */
    return head - tail;
}

static bool spscIsFull(const loopyChannelSPSC *spsc) {
    size_t head = atomic_load_explicit(&spsc->head, memory_order_acquire);
    size_t tail = atomic_load_explicit(&spsc->tail, memory_order_acquire);
    return (head - tail) >= spsc->capacity;
}

static bool spscIsEmpty(const loopyChannelSPSC *spsc) {
    size_t head = atomic_load_explicit(&spsc->head, memory_order_acquire);
    size_t tail = atomic_load_explicit(&spsc->tail, memory_order_acquire);
    return head == tail;
}

/* ====================================================================
 * MPMC Implementation
 * ==================================================================== */

static bool mpmcInit(loopyChannelMPMC *mpmc, size_t capacity,
                     size_t elementSize) {
    mpmc->buffer = zmalloc(capacity * elementSize);
    if (!mpmc->buffer) {
        return false;
    }

    mpmc->capacity = capacity;
    mpmc->elementSize = elementSize;
    mpmc->head = 0;
    mpmc->tail = 0;
    mpmc->count = 0;

    if (pthread_mutex_init(&mpmc->mutex, NULL) != 0) {
        zfree(mpmc->buffer);
        return false;
    }

    if (pthread_cond_init(&mpmc->notEmpty, NULL) != 0) {
        pthread_mutex_destroy(&mpmc->mutex);
        zfree(mpmc->buffer);
        return false;
    }

    if (pthread_cond_init(&mpmc->notFull, NULL) != 0) {
        pthread_cond_destroy(&mpmc->notEmpty);
        pthread_mutex_destroy(&mpmc->mutex);
        zfree(mpmc->buffer);
        return false;
    }

    return true;
}

static void mpmcDestroy(loopyChannelMPMC *mpmc) {
    pthread_cond_destroy(&mpmc->notFull);
    pthread_cond_destroy(&mpmc->notEmpty);
    pthread_mutex_destroy(&mpmc->mutex);
    zfree(mpmc->buffer);
    mpmc->buffer = NULL;
}

static loopyChannelStatus mpmcSend(loopyChannelMPMC *mpmc, const void *data,
                                   size_t len, bool blocking,
                                   uint64_t timeoutUs, loopyChannel *ch) {
    if (len != mpmc->elementSize) {
        return LOOPY_CHANNEL_INVALID;
    }

    pthread_mutex_lock(&mpmc->mutex);

    /* Wait for space if blocking */
    while (mpmc->count >= mpmc->capacity) {
        if (atomic_load(&ch->closed)) {
            pthread_mutex_unlock(&mpmc->mutex);
            return LOOPY_CHANNEL_CLOSED;
        }

        if (!blocking) {
            pthread_mutex_unlock(&mpmc->mutex);
            return LOOPY_CHANNEL_FULL;
        }

        atomic_fetch_add(&ch->sendBlocked, 1);

        if (timeoutUs > 0) {
            struct timespec ts = usToTimespec(timeoutUs);
            int ret = pthread_cond_timedwait(&mpmc->notFull, &mpmc->mutex, &ts);
            if (ret == ETIMEDOUT) {
                pthread_mutex_unlock(&mpmc->mutex);
                return LOOPY_CHANNEL_TIMEOUT;
            }
        } else {
            pthread_cond_wait(&mpmc->notFull, &mpmc->mutex);
        }
    }

    /* Check closed again after wait */
    if (atomic_load(&ch->closed)) {
        pthread_mutex_unlock(&mpmc->mutex);
        return LOOPY_CHANNEL_CLOSED;
    }

    /* Copy data into buffer */
    memcpy(mpmc->buffer + mpmc->head * mpmc->elementSize, data, len);
    mpmc->head = (mpmc->head + 1) % mpmc->capacity;
    mpmc->count++;

    /* Signal waiting receivers */
    pthread_cond_signal(&mpmc->notEmpty);
    pthread_mutex_unlock(&mpmc->mutex);

    return LOOPY_CHANNEL_OK;
}

static ssize_t mpmcRecv(loopyChannelMPMC *mpmc, void *buf, size_t bufLen,
                        bool blocking, uint64_t timeoutUs, loopyChannel *ch) {
    if (bufLen < mpmc->elementSize) {
        return LOOPY_CHANNEL_INVALID;
    }

    pthread_mutex_lock(&mpmc->mutex);

    /* Wait for data if blocking */
    while (mpmc->count == 0) {
        if (atomic_load(&ch->closed)) {
            pthread_mutex_unlock(&mpmc->mutex);
            return LOOPY_CHANNEL_CLOSED;
        }

        if (!blocking) {
            pthread_mutex_unlock(&mpmc->mutex);
            return LOOPY_CHANNEL_EMPTY;
        }

        atomic_fetch_add(&ch->recvBlocked, 1);

        if (timeoutUs > 0) {
            struct timespec ts = usToTimespec(timeoutUs);
            int ret =
                pthread_cond_timedwait(&mpmc->notEmpty, &mpmc->mutex, &ts);
            if (ret == ETIMEDOUT) {
                pthread_mutex_unlock(&mpmc->mutex);
                return LOOPY_CHANNEL_TIMEOUT;
            }
        } else {
            pthread_cond_wait(&mpmc->notEmpty, &mpmc->mutex);
        }
    }

    /* Copy data from buffer */
    memcpy(buf, mpmc->buffer + mpmc->tail * mpmc->elementSize,
           mpmc->elementSize);
    mpmc->tail = (mpmc->tail + 1) % mpmc->capacity;
    mpmc->count--;

    /* Signal waiting senders */
    pthread_cond_signal(&mpmc->notFull);
    pthread_mutex_unlock(&mpmc->mutex);

    return (ssize_t)mpmc->elementSize;
}

static loopyChannelStatus mpmcTrySend(loopyChannelMPMC *mpmc, const void *data,
                                      size_t len, loopyChannel *ch) {
    return mpmcSend(mpmc, data, len, false, 0, ch);
}

static ssize_t mpmcTryRecv(loopyChannelMPMC *mpmc, void *buf, size_t bufLen,
                           loopyChannel *ch) {
    return mpmcRecv(mpmc, buf, bufLen, false, 0, ch);
}

static size_t mpmcLen(loopyChannelMPMC *mpmc) {
    pthread_mutex_lock(&mpmc->mutex);
    size_t count = mpmc->count;
    pthread_mutex_unlock(&mpmc->mutex);
    return count;
}

static bool mpmcIsFull(loopyChannelMPMC *mpmc) {
    pthread_mutex_lock(&mpmc->mutex);
    bool full = mpmc->count >= mpmc->capacity;
    pthread_mutex_unlock(&mpmc->mutex);
    return full;
}

static bool mpmcIsEmpty(loopyChannelMPMC *mpmc) {
    pthread_mutex_lock(&mpmc->mutex);
    bool empty = mpmc->count == 0;
    pthread_mutex_unlock(&mpmc->mutex);
    return empty;
}

/* ====================================================================
 * Async Callback Handler
 * ==================================================================== */

static void asyncHandler(loopyLoop *l, loopyAsync *async, void *userData) {
    loopyChannel *ch = userData;
    (void)l;
    (void)async;

    /* Process pending async operations */
    pthread_mutex_lock(&ch->asyncMutex);

    loopyChannelPendingOp *op = ch->pendingOps;
    ch->pendingOps = NULL;

    pthread_mutex_unlock(&ch->asyncMutex);

    while (op) {
        loopyChannelPendingOp *next = op->next;

        if (op->isSend) {
            loopyChannelStatus status =
                loopyChannelTrySend(ch, op->op.send.data, op->op.send.len);
            if (op->op.send.cb) {
                op->op.send.cb(ch, status, op->userData);
            }
            zfree(op->op.send.data);
        } else {
            uint8_t *buf = zmalloc(ch->config.elementSize);
            if (buf) {
                ssize_t n =
                    loopyChannelTryRecv(ch, buf, ch->config.elementSize);
                loopyChannelStatus status =
                    (n > 0) ? LOOPY_CHANNEL_OK : (loopyChannelStatus)n;
                if (op->op.recv.cb) {
                    op->op.recv.cb(ch, (n > 0) ? buf : NULL,
                                   (n > 0) ? (size_t)n : 0, status,
                                   op->userData);
                }
                zfree(buf);
            }
        }

        zfree(op);
        op = next;
    }
}

/* ====================================================================
 * Configuration
 * ==================================================================== */

void loopyChannelConfigInit(loopyChannelConfig *config) {
    memset(config, 0, sizeof(*config));
    config->type = LOOPY_CHANNEL_SPSC;
    config->capacity = 1024;
    config->elementSize = 0; /* Must be set by caller */
    config->blocking = false;
    config->sendTimeoutUs = 0;
    config->recvTimeoutUs = 0;
}

/* ====================================================================
 * Lifecycle
 * ==================================================================== */

loopyChannel *loopyChannelNew(loopyLoop *loop,
                              const loopyChannelConfig *config) {
    if (!config || config->elementSize == 0 || config->capacity == 0) {
        return NULL;
    }

    loopyChannel *ch = zcalloc(1, sizeof(loopyChannel));
    if (!ch) {
        return NULL;
    }

    ch->id = atomic_fetch_add(&globalChannelId, 1);
    ch->type = config->type;
    ch->loop = loop;
    ch->config = *config;
    atomic_store(&ch->closed, false);

    /* Initialize implementation */
    bool ok = false;
    switch (config->type) {
    case LOOPY_CHANNEL_SPSC:
        ok = spscInit(&ch->impl.spsc, config->capacity, config->elementSize);
        break;
    case LOOPY_CHANNEL_MPSC:
    case LOOPY_CHANNEL_MPMC:
        ok = mpmcInit(&ch->impl.mpmc, config->capacity, config->elementSize);
        break;
    }

    if (!ok) {
        zfree(ch);
        return NULL;
    }

    /* Initialize async support if we have an event loop */
    if (loop) {
        if (pthread_mutex_init(&ch->asyncMutex, NULL) != 0) {
            switch (config->type) {
            case LOOPY_CHANNEL_SPSC:
                spscDestroy(&ch->impl.spsc);
                break;
            case LOOPY_CHANNEL_MPSC:
            case LOOPY_CHANNEL_MPMC:
                mpmcDestroy(&ch->impl.mpmc);
                break;
            }
            zfree(ch);
            return NULL;
        }

        ch->async = loopyAsyncNew(loop, asyncHandler, ch);
        if (!ch->async) {
            pthread_mutex_destroy(&ch->asyncMutex);
            switch (config->type) {
            case LOOPY_CHANNEL_SPSC:
                spscDestroy(&ch->impl.spsc);
                break;
            case LOOPY_CHANNEL_MPSC:
            case LOOPY_CHANNEL_MPMC:
                mpmcDestroy(&ch->impl.mpmc);
                break;
            }
            zfree(ch);
            return NULL;
        }
    }

    return ch;
}

void loopyChannelClose(loopyChannel *ch) {
    if (!ch) {
        return;
    }

    atomic_store(&ch->closed, true);

    /* Wake any waiting threads */
    if (ch->type != LOOPY_CHANNEL_SPSC) {
        pthread_mutex_lock(&ch->impl.mpmc.mutex);
        pthread_cond_broadcast(&ch->impl.mpmc.notEmpty);
        pthread_cond_broadcast(&ch->impl.mpmc.notFull);
        pthread_mutex_unlock(&ch->impl.mpmc.mutex);
    }
}

void loopyChannelFree(loopyChannel *ch) {
    if (!ch) {
        return;
    }

    loopyChannelClose(ch);

    /* Free async resources */
    if (ch->async) {
        loopyAsyncFree(ch->async);

        /* Free pending operations */
        pthread_mutex_lock(&ch->asyncMutex);
        loopyChannelPendingOp *op = ch->pendingOps;
        while (op) {
            loopyChannelPendingOp *next = op->next;
            if (op->isSend) {
                zfree(op->op.send.data);
            }
            zfree(op);
            op = next;
        }
        pthread_mutex_unlock(&ch->asyncMutex);
        pthread_mutex_destroy(&ch->asyncMutex);
    }

    /* Free implementation */
    switch (ch->type) {
    case LOOPY_CHANNEL_SPSC:
        spscDestroy(&ch->impl.spsc);
        break;
    case LOOPY_CHANNEL_MPSC:
    case LOOPY_CHANNEL_MPMC:
        mpmcDestroy(&ch->impl.mpmc);
        break;
    }

    zfree(ch);
}

/* ====================================================================
 * Synchronous Operations
 * ==================================================================== */

loopyChannelStatus loopyChannelSend(loopyChannel *ch, const void *data,
                                    size_t len) {
    if (!ch || !data) {
        return LOOPY_CHANNEL_INVALID;
    }

    if (atomic_load(&ch->closed)) {
        atomic_fetch_add(&ch->sendFailed, 1);
        return LOOPY_CHANNEL_CLOSED;
    }

    loopyChannelStatus status;

    switch (ch->type) {
    case LOOPY_CHANNEL_SPSC:
        if (ch->config.blocking) {
            /* Spin-wait for SPSC (no condition variable) */
            uint64_t startTime = getMicroseconds();
            while (!spscTrySend(&ch->impl.spsc, data, len)) {
                if (atomic_load(&ch->closed)) {
                    atomic_fetch_add(&ch->sendFailed, 1);
                    return LOOPY_CHANNEL_CLOSED;
                }
                if (ch->config.sendTimeoutUs > 0) {
                    if (getMicroseconds() - startTime >=
                        ch->config.sendTimeoutUs) {
                        atomic_fetch_add(&ch->sendFailed, 1);
                        return LOOPY_CHANNEL_TIMEOUT;
                    }
                }
                atomic_fetch_add(&ch->sendBlocked, 1);
                /* Brief pause to avoid burning CPU */
                struct timespec ts = {0, 1000}; /* 1 microsecond */
                nanosleep(&ts, NULL);
            }
            status = LOOPY_CHANNEL_OK;
        } else {
            status = spscTrySend(&ch->impl.spsc, data, len)
                         ? LOOPY_CHANNEL_OK
                         : LOOPY_CHANNEL_FULL;
        }
        break;

    case LOOPY_CHANNEL_MPSC:
    case LOOPY_CHANNEL_MPMC:
        status = mpmcSend(&ch->impl.mpmc, data, len, ch->config.blocking,
                          ch->config.sendTimeoutUs, ch);
        break;

    default:
        status = LOOPY_CHANNEL_ERROR;
    }

    if (status == LOOPY_CHANNEL_OK) {
        atomic_fetch_add(&ch->totalSent, 1);
        updatePeakUsage(ch, loopyChannelLen(ch));
    } else {
        atomic_fetch_add(&ch->sendFailed, 1);
    }

    return status;
}

ssize_t loopyChannelRecv(loopyChannel *ch, void *buf, size_t bufLen) {
    if (!ch || !buf) {
        return LOOPY_CHANNEL_INVALID;
    }

    ssize_t result;

    switch (ch->type) {
    case LOOPY_CHANNEL_SPSC:
        if (ch->config.blocking) {
            /* Spin-wait for SPSC */
            uint64_t startTime = getMicroseconds();
            while ((result = spscTryRecv(&ch->impl.spsc, buf, bufLen)) ==
                   LOOPY_CHANNEL_EMPTY) {
                if (atomic_load(&ch->closed)) {
                    atomic_fetch_add(&ch->recvFailed, 1);
                    return LOOPY_CHANNEL_CLOSED;
                }
                if (ch->config.recvTimeoutUs > 0) {
                    if (getMicroseconds() - startTime >=
                        ch->config.recvTimeoutUs) {
                        atomic_fetch_add(&ch->recvFailed, 1);
                        return LOOPY_CHANNEL_TIMEOUT;
                    }
                }
                atomic_fetch_add(&ch->recvBlocked, 1);
                struct timespec ts = {0, 1000};
                nanosleep(&ts, NULL);
            }
        } else {
            result = spscTryRecv(&ch->impl.spsc, buf, bufLen);
        }
        break;

    case LOOPY_CHANNEL_MPSC:
    case LOOPY_CHANNEL_MPMC:
        result = mpmcRecv(&ch->impl.mpmc, buf, bufLen, ch->config.blocking,
                          ch->config.recvTimeoutUs, ch);
        break;

    default:
        result = LOOPY_CHANNEL_ERROR;
    }

    if (result > 0) {
        atomic_fetch_add(&ch->totalReceived, 1);
    } else {
        atomic_fetch_add(&ch->recvFailed, 1);
    }

    return result;
}

loopyChannelStatus loopyChannelTrySend(loopyChannel *ch, const void *data,
                                       size_t len) {
    if (!ch || !data) {
        return LOOPY_CHANNEL_INVALID;
    }

    if (atomic_load(&ch->closed)) {
        atomic_fetch_add(&ch->sendFailed, 1);
        return LOOPY_CHANNEL_CLOSED;
    }

    loopyChannelStatus status;

    switch (ch->type) {
    case LOOPY_CHANNEL_SPSC:
        status = spscTrySend(&ch->impl.spsc, data, len) ? LOOPY_CHANNEL_OK
                                                        : LOOPY_CHANNEL_FULL;
        break;
    case LOOPY_CHANNEL_MPSC:
    case LOOPY_CHANNEL_MPMC:
        status = mpmcTrySend(&ch->impl.mpmc, data, len, ch);
        break;
    default:
        status = LOOPY_CHANNEL_ERROR;
    }

    if (status == LOOPY_CHANNEL_OK) {
        atomic_fetch_add(&ch->totalSent, 1);
        updatePeakUsage(ch, loopyChannelLen(ch));
    } else if (status != LOOPY_CHANNEL_FULL) {
        atomic_fetch_add(&ch->sendFailed, 1);
    }

    return status;
}

ssize_t loopyChannelTryRecv(loopyChannel *ch, void *buf, size_t bufLen) {
    if (!ch || !buf) {
        return LOOPY_CHANNEL_INVALID;
    }

    ssize_t result;

    switch (ch->type) {
    case LOOPY_CHANNEL_SPSC:
        result = spscTryRecv(&ch->impl.spsc, buf, bufLen);
        break;
    case LOOPY_CHANNEL_MPSC:
    case LOOPY_CHANNEL_MPMC:
        result = mpmcTryRecv(&ch->impl.mpmc, buf, bufLen, ch);
        break;
    default:
        result = LOOPY_CHANNEL_ERROR;
    }

    if (result > 0) {
        atomic_fetch_add(&ch->totalReceived, 1);
    } else if (result != LOOPY_CHANNEL_EMPTY) {
        atomic_fetch_add(&ch->recvFailed, 1);
    }

    return result;
}

/* ====================================================================
 * Asynchronous Operations
 * ==================================================================== */

bool loopyChannelSendAsync(loopyChannel *ch, const void *data, size_t len,
                           loopyChannelSendCallback *cb, void *userData) {
    if (!ch || !ch->async || !data) {
        return false;
    }

    /* Allocate pending operation */
    loopyChannelPendingOp *op = zmalloc(sizeof(loopyChannelPendingOp));
    if (!op) {
        return false;
    }

    /* Copy data for async delivery */
    op->op.send.data = zmalloc(len);
    if (!op->op.send.data) {
        zfree(op);
        return false;
    }
    memcpy(op->op.send.data, data, len);

    op->op.send.len = len;
    op->op.send.cb = cb;
    op->userData = userData;
    op->isSend = true;

    /* Add to pending list */
    pthread_mutex_lock(&ch->asyncMutex);
    op->next = ch->pendingOps;
    ch->pendingOps = op;
    pthread_mutex_unlock(&ch->asyncMutex);

    /* Wake the event loop */
    loopyAsyncSend(ch->async);

    return true;
}

bool loopyChannelRecvAsync(loopyChannel *ch, loopyChannelRecvCallback *cb,
                           void *userData) {
    if (!ch || !ch->async) {
        return false;
    }

    loopyChannelPendingOp *op = zmalloc(sizeof(loopyChannelPendingOp));
    if (!op) {
        return false;
    }

    op->op.recv.cb = cb;
    op->userData = userData;
    op->isSend = false;

    pthread_mutex_lock(&ch->asyncMutex);
    op->next = ch->pendingOps;
    ch->pendingOps = op;
    pthread_mutex_unlock(&ch->asyncMutex);

    loopyAsyncSend(ch->async);

    return true;
}

/* ====================================================================
 * Select API
 * ==================================================================== */

int loopyChannelSelect(loopyChannelCase *cases, size_t nCases,
                       int64_t timeoutUs) {
    if (!cases || nCases == 0) {
        return -1;
    }

    uint64_t startTime = getMicroseconds();
    uint64_t deadline = (timeoutUs < 0)    ? UINT64_MAX
                        : (timeoutUs == 0) ? 0
                                           : startTime + (uint64_t)timeoutUs;

    do {
        /* Check each channel */
        for (size_t i = 0; i < nCases; i++) {
            loopyChannelCase *c = &cases[i];

            if (c->send) {
                /* Try to send */
                loopyChannelStatus status =
                    loopyChannelTrySend(c->ch, c->data, c->len);
                if (status == LOOPY_CHANNEL_OK) {
                    c->result = (ssize_t)c->len;
                    return (int)i;
                } else if (status != LOOPY_CHANNEL_FULL) {
                    c->result = status;
                    return (int)i;
                }
            } else {
                /* Try to receive */
                ssize_t n = loopyChannelTryRecv(c->ch, c->data, c->len);
                if (n > 0) {
                    c->result = n;
                    return (int)i;
                } else if (n != LOOPY_CHANNEL_EMPTY) {
                    c->result = n;
                    return (int)i;
                }
            }
        }

        /* All channels would block - sleep briefly */
        if (timeoutUs == 0) {
            break; /* Non-blocking poll mode */
        }

        struct timespec ts = {0, 1000000}; /* 1ms */
        nanosleep(&ts, NULL);

    } while (getMicroseconds() < deadline);

    return -1; /* Timeout */
}

/* ====================================================================
 * Introspection
 * ==================================================================== */

size_t loopyChannelLen(const loopyChannel *ch) {
    if (!ch) {
        return 0;
    }

    switch (ch->type) {
    case LOOPY_CHANNEL_SPSC:
        return spscLen(&ch->impl.spsc);
    case LOOPY_CHANNEL_MPSC:
    case LOOPY_CHANNEL_MPMC:
        return mpmcLen((loopyChannelMPMC *)&ch->impl.mpmc);
    default:
        return 0;
    }
}

size_t loopyChannelCap(const loopyChannel *ch) {
    if (!ch) {
        return 0;
    }

    switch (ch->type) {
    case LOOPY_CHANNEL_SPSC:
        return ch->impl.spsc.capacity;
    case LOOPY_CHANNEL_MPSC:
    case LOOPY_CHANNEL_MPMC:
        return ch->impl.mpmc.capacity;
    default:
        return 0;
    }
}

bool loopyChannelIsClosed(const loopyChannel *ch) {
    if (!ch) {
        return true;
    }
    return atomic_load(&ch->closed);
}

bool loopyChannelIsFull(const loopyChannel *ch) {
    if (!ch) {
        return true;
    }

    switch (ch->type) {
    case LOOPY_CHANNEL_SPSC:
        return spscIsFull(&ch->impl.spsc);
    case LOOPY_CHANNEL_MPSC:
    case LOOPY_CHANNEL_MPMC:
        return mpmcIsFull((loopyChannelMPMC *)&ch->impl.mpmc);
    default:
        return true;
    }
}

bool loopyChannelIsEmpty(const loopyChannel *ch) {
    if (!ch) {
        return true;
    }

    switch (ch->type) {
    case LOOPY_CHANNEL_SPSC:
        return spscIsEmpty(&ch->impl.spsc);
    case LOOPY_CHANNEL_MPSC:
    case LOOPY_CHANNEL_MPMC:
        return mpmcIsEmpty((loopyChannelMPMC *)&ch->impl.mpmc);
    default:
        return true;
    }
}

loopyChannelId loopyChannelGetId(const loopyChannel *ch) {
    return ch ? ch->id : 0;
}

loopyChannelType loopyChannelGetType(const loopyChannel *ch) {
    return ch ? ch->type : LOOPY_CHANNEL_SPSC;
}

size_t loopyChannelGetElementSize(const loopyChannel *ch) {
    return ch ? ch->config.elementSize : 0;
}

loopyLoop *loopyChannelGetLoop(const loopyChannel *ch) {
    return ch ? ch->loop : NULL;
}

/* ====================================================================
 * Statistics
 * ==================================================================== */

void loopyChannelGetStats(const loopyChannel *ch, loopyChannelStats *stats) {
    if (!ch || !stats) {
        return;
    }

    stats->totalSent = atomic_load(&ch->totalSent);
    stats->totalReceived = atomic_load(&ch->totalReceived);
    stats->sendBlocked = atomic_load(&ch->sendBlocked);
    stats->recvBlocked = atomic_load(&ch->recvBlocked);
    stats->sendFailed = atomic_load(&ch->sendFailed);
    stats->recvFailed = atomic_load(&ch->recvFailed);
    stats->peakUsage = atomic_load(&ch->peakUsage);
}

void loopyChannelResetStats(loopyChannel *ch) {
    if (!ch) {
        return;
    }

    atomic_store(&ch->totalSent, 0);
    atomic_store(&ch->totalReceived, 0);
    atomic_store(&ch->sendBlocked, 0);
    atomic_store(&ch->recvBlocked, 0);
    atomic_store(&ch->sendFailed, 0);
    atomic_store(&ch->recvFailed, 0);
    atomic_store(&ch->peakUsage, 0);
}

/* ====================================================================
 * Utility
 * ==================================================================== */

const char *loopyChannelTypeName(loopyChannelType type) {
    switch (type) {
    case LOOPY_CHANNEL_SPSC:
        return "SPSC";
    case LOOPY_CHANNEL_MPSC:
        return "MPSC";
    case LOOPY_CHANNEL_MPMC:
        return "MPMC";
    default:
        return "UNKNOWN";
    }
}

const char *loopyChannelStatusName(loopyChannelStatus status) {
    switch (status) {
    case LOOPY_CHANNEL_OK:
        return "OK";
    case LOOPY_CHANNEL_FULL:
        return "FULL";
    case LOOPY_CHANNEL_EMPTY:
        return "EMPTY";
    case LOOPY_CHANNEL_CLOSED:
        return "CLOSED";
    case LOOPY_CHANNEL_TIMEOUT:
        return "TIMEOUT";
    case LOOPY_CHANNEL_ERROR:
        return "ERROR";
    case LOOPY_CHANNEL_INVALID:
        return "INVALID";
    default:
        return "UNKNOWN";
    }
}

void *loopyChannelGetData(const loopyChannel *ch) {
    return ch ? ch->userData : NULL;
}

void loopyChannelSetData(loopyChannel *ch, void *data) {
    if (ch) {
        ch->userData = data;
    }
}
