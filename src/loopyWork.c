/* loopyWork - Thread pool work queue for loopy event loop
 *
 * Implementation uses a thread pool with loopyAsync for completion
 * notification.
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
#include "loopyAsync.h"
#include "loopyWork.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* ====================================================================
 * Internal data structures
 * ==================================================================== */

typedef struct loopyWorkItem {
    loopyWorkId id;
    loopyWorkCallback *workCb;
    loopyAfterWorkCallback *afterCb;
    void *userData;
    loopyWorkStatus status;
    bool cancelled;
    struct loopyWorkItem *next;
} loopyWorkItem;

struct loopyWork {
    loopyLoop *loop;
    void *userData; /* User data for handle accessors */
    loopyWorkConfig config;
    loopyAsync *async; /* For completion notification */

    /* Thread pool */
    pthread_t *workers;
    size_t workerCount;
    bool shutdown;

    /* Work queue */
    loopyWorkItem *pendingHead;
    loopyWorkItem *pendingTail;
    size_t pendingCount;
    size_t runningCount;
    loopyWorkId nextWorkId;

    /* Completed work queue (for delivery to event loop) */
    loopyWorkItem *completedHead;
    loopyWorkItem *completedTail;

    /* Synchronization */
    pthread_mutex_t mutex;
    pthread_cond_t workAvailable;
};

/* ====================================================================
 * Forward declarations
 * ==================================================================== */

static void *workWorkerThread(void *arg);
static void workAsyncCallback(loopyLoop *l, loopyAsync *async, void *userData);
static loopyWorkItem *workDequeue(loopyWork *work);
static void workEnqueueCompleted(loopyWork *work, loopyWorkItem *item);
static void workProcessCompleted(loopyWork *work);

/* ====================================================================
 * Configuration
 * ==================================================================== */

void loopyWorkConfigInit(loopyWorkConfig *config) {
    if (config) {
        *config = LOOPY_WORK_CONFIG_DEFAULT;
    }
}

/* ====================================================================
 * Lifecycle
 * ==================================================================== */

loopyWork *loopyWorkNew(loopyLoop *loop, const loopyWorkConfig *config) {
    if (!loop) {
        return NULL;
    }

    loopyWork *work = zcalloc(1, sizeof(*work));
    if (!work) {
        return NULL;
    }

    work->loop = loop;
    work->config = config ? *config : LOOPY_WORK_CONFIG_DEFAULT;
    work->nextWorkId = 1;

    /* Ensure valid thread counts */
    if (work->config.minThreads == 0) {
        work->config.minThreads = 1;
    }
    if (work->config.maxThreads == 0) {
        work->config.maxThreads = 4;
    }
    if (work->config.maxThreads < work->config.minThreads) {
        work->config.maxThreads = work->config.minThreads;
    }

    /* Initialize synchronization */
    if (pthread_mutex_init(&work->mutex, NULL) != 0) {
        zfree(work);
        return NULL;
    }

    if (pthread_cond_init(&work->workAvailable, NULL) != 0) {
        pthread_mutex_destroy(&work->mutex);
        zfree(work);
        return NULL;
    }

    /* Create async handle for completion notification */
    work->async = loopyAsyncNew(loop, workAsyncCallback, work);
    if (!work->async) {
        pthread_cond_destroy(&work->workAvailable);
        pthread_mutex_destroy(&work->mutex);
        zfree(work);
        return NULL;
    }

    /* Create worker threads (start with min threads) */
    work->workerCount = work->config.minThreads;
    work->workers = zcalloc(work->workerCount, sizeof(pthread_t));
    if (!work->workers) {
        loopyAsyncFree(work->async);
        pthread_cond_destroy(&work->workAvailable);
        pthread_mutex_destroy(&work->mutex);
        zfree(work);
        return NULL;
    }

    for (size_t i = 0; i < work->workerCount; i++) {
        if (pthread_create(&work->workers[i], NULL, workWorkerThread, work) !=
            0) {
            /* Shutdown already-created threads */
            work->shutdown = true;
            pthread_cond_broadcast(&work->workAvailable);
            for (size_t j = 0; j < i; j++) {
                pthread_join(work->workers[j], NULL);
            }
            loopyAsyncFree(work->async);
            pthread_cond_destroy(&work->workAvailable);
            pthread_mutex_destroy(&work->mutex);
            zfree(work->workers);
            zfree(work);
            return NULL;
        }
    }

    return work;
}

void loopyWorkFree(loopyWork *work) {
    if (!work) {
        return;
    }

    /* Signal shutdown to workers */
    pthread_mutex_lock(&work->mutex);
    work->shutdown = true;
    pthread_cond_broadcast(&work->workAvailable);
    pthread_mutex_unlock(&work->mutex);

    /* Wait for workers to finish */
    for (size_t i = 0; i < work->workerCount; i++) {
        pthread_join(work->workers[i], NULL);
    }

    /* Free async handle */
    loopyAsyncFree(work->async);

    /* Free pending work items */
    loopyWorkItem *item = work->pendingHead;
    while (item) {
        loopyWorkItem *next = item->next;
        zfree(item);
        item = next;
    }

    /* Free completed work items */
    item = work->completedHead;
    while (item) {
        loopyWorkItem *next = item->next;
        zfree(item);
        item = next;
    }

    pthread_cond_destroy(&work->workAvailable);
    pthread_mutex_destroy(&work->mutex);
    zfree(work->workers);
    zfree(work);
}

/* ====================================================================
 * Work Queue Operations
 * ==================================================================== */

loopyWorkId loopyWorkQueue(loopyWork *work, loopyWorkCallback *workCb,
                           loopyAfterWorkCallback *afterCb, void *userData) {
    if (!work || !workCb) {
        return 0;
    }

    pthread_mutex_lock(&work->mutex);

    /* Check queue capacity */
    if (work->config.maxQueueSize > 0 &&
        work->pendingCount >= work->config.maxQueueSize) {
        pthread_mutex_unlock(&work->mutex);
        return 0;
    }

    /* Create work item */
    loopyWorkItem *item = zcalloc(1, sizeof(*item));
    if (!item) {
        pthread_mutex_unlock(&work->mutex);
        return 0;
    }

    item->id = work->nextWorkId++;
    item->workCb = workCb;
    item->afterCb = afterCb;
    item->userData = userData;
    item->status = LOOPY_WORK_OK;

    /* Enqueue */
    if (work->pendingTail) {
        work->pendingTail->next = item;
    } else {
        work->pendingHead = item;
    }
    work->pendingTail = item;
    work->pendingCount++;

    loopyWorkId id = item->id;

    /* Wake up a worker */
    pthread_cond_signal(&work->workAvailable);
    pthread_mutex_unlock(&work->mutex);

    return id;
}

bool loopyWorkCancel(loopyWork *work, loopyWorkId workId) {
    if (!work || workId == 0) {
        return false;
    }

    pthread_mutex_lock(&work->mutex);

    /* Find and mark as cancelled */
    loopyWorkItem *item = work->pendingHead;
    while (item) {
        if (item->id == workId) {
            item->cancelled = true;
            pthread_mutex_unlock(&work->mutex);
            return true;
        }
        item = item->next;
    }

    pthread_mutex_unlock(&work->mutex);
    return false;
}

void loopyWorkCancelAll(loopyWork *work) {
    if (!work) {
        return;
    }

    pthread_mutex_lock(&work->mutex);
    loopyWorkItem *item = work->pendingHead;
    while (item) {
        item->cancelled = true;
        item = item->next;
    }
    pthread_mutex_unlock(&work->mutex);
}

/* ====================================================================
 * Status
 * ==================================================================== */

size_t loopyWorkPendingCount(const loopyWork *work) {
    if (!work) {
        return 0;
    }
    /* Note: Racy read but acceptable for status queries */
    return work->pendingCount;
}

size_t loopyWorkRunningCount(const loopyWork *work) {
    if (!work) {
        return 0;
    }
    return work->runningCount;
}

size_t loopyWorkThreadCount(const loopyWork *work) {
    if (!work) {
        return 0;
    }
    return work->workerCount;
}

const char *loopyWorkStatusString(loopyWorkStatus status) {
    switch (status) {
    case LOOPY_WORK_OK:
        return "OK";
    case LOOPY_WORK_CANCELLED:
        return "CANCELLED";
    case LOOPY_WORK_ERROR:
    default:
        return "ERROR";
    }
}

loopyLoop *loopyWorkGetLoop(const loopyWork *work) {
    return work ? work->loop : NULL;
}

/* ====================================================================
 * Internal: Worker thread
 * ==================================================================== */

static loopyWorkItem *workDequeue(loopyWork *work) {
    /* Must be called with mutex held */
    if (!work->pendingHead) {
        return NULL;
    }

    loopyWorkItem *item = work->pendingHead;
    work->pendingHead = item->next;
    if (!work->pendingHead) {
        work->pendingTail = NULL;
    }
    work->pendingCount--;
    item->next = NULL;
    return item;
}

static void *workWorkerThread(void *arg) {
    loopyWork *work = arg;

    while (true) {
        pthread_mutex_lock(&work->mutex);

        /* Wait for work or shutdown */
        while (!work->pendingHead && !work->shutdown) {
            pthread_cond_wait(&work->workAvailable, &work->mutex);
        }

        if (work->shutdown) {
            pthread_mutex_unlock(&work->mutex);
            break;
        }

        /* Dequeue work item */
        loopyWorkItem *item = workDequeue(work);
        if (item) {
            work->runningCount++;
        }
        pthread_mutex_unlock(&work->mutex);

        if (!item) {
            continue;
        }

        /* Check if cancelled */
        if (item->cancelled) {
            item->status = LOOPY_WORK_CANCELLED;
        } else {
            /* Execute work callback */
            if (item->workCb) {
                item->workCb(work, item->id, item->userData);
            }
            item->status = LOOPY_WORK_OK;
        }

        /* Decrement running count and enqueue completion */
        pthread_mutex_lock(&work->mutex);
        work->runningCount--;
        pthread_mutex_unlock(&work->mutex);

        workEnqueueCompleted(work, item);

        /* Notify event loop */
        loopyAsyncSend(work->async);
    }

    return NULL;
}

/* ====================================================================
 * Internal: Completion queue and notification
 * ==================================================================== */

static void workEnqueueCompleted(loopyWork *work, loopyWorkItem *item) {
    pthread_mutex_lock(&work->mutex);

    if (work->completedTail) {
        work->completedTail->next = item;
    } else {
        work->completedHead = item;
    }
    work->completedTail = item;
    item->next = NULL;

    pthread_mutex_unlock(&work->mutex);
}

static void workAsyncCallback(loopyLoop *l, loopyAsync *async, void *userData) {
    (void)l;
    (void)async;

    loopyWork *work = userData;
    workProcessCompleted(work);
}

static void workProcessCompleted(loopyWork *work) {
    while (true) {
        pthread_mutex_lock(&work->mutex);
        loopyWorkItem *item = work->completedHead;
        if (item) {
            work->completedHead = item->next;
            if (!work->completedHead) {
                work->completedTail = NULL;
            }
        }
        pthread_mutex_unlock(&work->mutex);

        if (!item) {
            break;
        }

        /* Invoke after-work callback on event loop thread */
        if (item->afterCb) {
            item->afterCb(work->loop, work, item->id, item->status,
                          item->userData);
        }

        zfree(item);
    }
}

/* ====================================================================
 * Handle Accessors
 * ==================================================================== */

void *loopyWorkGetData(const loopyWork *work) {
    return work ? work->userData : NULL;
}

void loopyWorkSetData(loopyWork *work, void *data) {
    if (work) {
        work->userData = data;
    }
}
