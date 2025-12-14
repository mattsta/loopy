/* loopyIdle - Idle, Prepare, and Check handles for loopy event loop
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
#include "loopyIdle.h"
#include "loopyInternal.h"

#include <stdlib.h>

/* ====================================================================
 * Internal data structures
 * ==================================================================== */

struct loopyIdleHandle {
    loopyLoop *loop;
    loopyIdleCallback *cb;
    void *userData;
    bool active;
    struct loopyIdleHandle *next;
    struct loopyIdleHandle *prev;
};

struct loopyPrepareHandle {
    loopyLoop *loop;
    loopyPrepareCallback *cb;
    void *userData;
    bool active;
    struct loopyPrepareHandle *next;
    struct loopyPrepareHandle *prev;
};

struct loopyCheckHandle {
    loopyLoop *loop;
    loopyCheckCallback *cb;
    void *userData;
    bool active;
    struct loopyCheckHandle *next;
    struct loopyCheckHandle *prev;
};

/* Per-loop handle manager */
typedef struct loopyIdleManager {
    loopyLoop *loop;

    /* Handle lists */
    loopyIdleHandle *idleList;
    loopyPrepareHandle *prepareList;
    loopyCheckHandle *checkList;

    /* Counts of active handles */
    size_t idleActiveCount;
    size_t prepareActiveCount;
    size_t checkActiveCount;

    /* Saved original callbacks to chain */
    loopyCallback *origBeforeCb;
    void *origBeforeData;
    loopyCallback *origAfterCb;
    void *origAfterData;

    /* Timer for idle wake-up (0-timeout timer when idle is active) */
    uint64_t idleTimerId;
    bool idleTimerActive;

    /* Linked list of managers */
    struct loopyIdleManager *next;
} loopyIdleManager;

/* Global list of managers (one per loop) */
static loopyIdleManager *g_managers = NULL;

/* ====================================================================
 * Forward declarations
 * ==================================================================== */

static loopyIdleManager *idleGetManager(const loopyLoop *loop);
static loopyIdleManager *idleCreateManager(loopyLoop *loop);
static void idleDestroyManager(loopyIdleManager *mgr);
static void idleBeforeSleepCallback(loopyLoop *loop, void *userData);
static void idleAfterSleepCallback(loopyLoop *loop, void *userData);
static void idleUpdateTimer(loopyIdleManager *mgr);
static bool idleTimerCallback(timerWheel *t, timerWheelId id, void *userData);

/* ====================================================================
 * Manager functions
 * ==================================================================== */

static loopyIdleManager *idleGetManager(const loopyLoop *loop) {
    loopyIdleManager *mgr = g_managers;
    while (mgr) {
        if (mgr->loop == loop) {
            return mgr;
        }
        mgr = mgr->next;
    }
    return NULL;
}

static loopyIdleManager *idleCreateManager(loopyLoop *loop) {
    loopyIdleManager *mgr = idleGetManager(loop);
    if (mgr) {
        return mgr;
    }

    mgr = zcalloc(1, sizeof(*mgr));
    if (!mgr) {
        return NULL;
    }

    mgr->loop = loop;

    /* Save original callbacks */
    mgr->origBeforeCb = loop->sleep.before.cb;
    mgr->origBeforeData = loop->sleep.before.clientData;
    mgr->origAfterCb = loop->sleep.after.cb;
    mgr->origAfterData = loop->sleep.after.clientData;

    /* Install our callbacks */
    loopySetBeforeSleepCallback(loop, idleBeforeSleepCallback, mgr);
    loopySetAfterSleepCallback(loop, idleAfterSleepCallback, mgr);

    /* Add to global list */
    mgr->next = g_managers;
    g_managers = mgr;

    return mgr;
}

static void idleDestroyManager(loopyIdleManager *mgr) {
    if (!mgr) {
        return;
    }

    /* Remove idle timer if active */
    if (mgr->idleTimerActive) {
        loopyUnregisterTimer(mgr->loop, mgr->idleTimerId);
    }

    /* Restore original callbacks */
    loopySetBeforeSleepCallback(mgr->loop, mgr->origBeforeCb,
                                mgr->origBeforeData);
    loopySetAfterSleepCallback(mgr->loop, mgr->origAfterCb, mgr->origAfterData);

    /* Remove from global list */
    if (g_managers == mgr) {
        g_managers = mgr->next;
    } else {
        loopyIdleManager *prev = g_managers;
        while (prev && prev->next != mgr) {
            prev = prev->next;
        }
        if (prev) {
            prev->next = mgr->next;
        }
    }

    zfree(mgr);
}

/* ====================================================================
 * Timer for idle wake-up
 * ==================================================================== */

static bool idleTimerCallback(timerWheel *t, timerWheelId id, void *userData) {
    /* This timer just ensures the loop doesn't block when idle is active */
    (void)t;
    (void)id;
    (void)userData;
    return true; /* Keep repeating */
}

static void idleUpdateTimer(loopyIdleManager *mgr) {
    if (mgr->idleActiveCount > 0 && !mgr->idleTimerActive) {
        /* Start a 0-microsecond repeating timer to prevent blocking */
        mgr->idleTimerId =
            loopyRegisterTimer(mgr->loop, 0, 1, idleTimerCallback, mgr);
        if (mgr->idleTimerId != 0) {
            mgr->idleTimerActive = true;
        }
    } else if (mgr->idleActiveCount == 0 && mgr->idleTimerActive) {
        /* Stop the timer */
        loopyUnregisterTimer(mgr->loop, mgr->idleTimerId);
        mgr->idleTimerActive = false;
    }
}

/* ====================================================================
 * Sleep callbacks
 * ==================================================================== */

static void idleBeforeSleepCallback(loopyLoop *loop, void *userData) {
    loopyIdleManager *mgr = userData;

    /* Run prepare callbacks */
    loopyPrepareHandle *prep = mgr->prepareList;
    while (prep) {
        loopyPrepareHandle *next = prep->next;
        if (prep->active && prep->cb) {
            prep->cb(loop, prep, prep->userData);
        }
        prep = next;
    }

    /* Run idle callbacks */
    loopyIdleHandle *idle = mgr->idleList;
    while (idle) {
        loopyIdleHandle *next = idle->next;
        if (idle->active && idle->cb) {
            bool continueRunning = idle->cb(loop, idle, idle->userData);
            if (!continueRunning) {
                loopyIdleStop(idle);
            }
        }
        idle = next;
    }

    /* Chain to original callback */
    if (mgr->origBeforeCb) {
        mgr->origBeforeCb(loop, mgr->origBeforeData);
    }
}

static void idleAfterSleepCallback(loopyLoop *loop, void *userData) {
    loopyIdleManager *mgr = userData;

    /* Run check callbacks */
    loopyCheckHandle *check = mgr->checkList;
    while (check) {
        loopyCheckHandle *next = check->next;
        if (check->active && check->cb) {
            check->cb(loop, check, check->userData);
        }
        check = next;
    }

    /* Chain to original callback */
    if (mgr->origAfterCb) {
        mgr->origAfterCb(loop, mgr->origAfterData);
    }
}

/* ====================================================================
 * Idle Handle API
 * ==================================================================== */

loopyIdleHandle *loopyIdleStart(loopyLoop *loop, loopyIdleCallback *cb,
                                void *userData) {
    if (!loop || !cb) {
        return NULL;
    }

    loopyIdleManager *mgr = idleCreateManager(loop);
    if (!mgr) {
        return NULL;
    }

    loopyIdleHandle *handle = zcalloc(1, sizeof(*handle));
    if (!handle) {
        return NULL;
    }

    handle->loop = loop;
    handle->cb = cb;
    handle->userData = userData;
    handle->active = true;

    /* Add to list */
    handle->next = mgr->idleList;
    if (mgr->idleList) {
        mgr->idleList->prev = handle;
    }
    mgr->idleList = handle;

    mgr->idleActiveCount++;
    idleUpdateTimer(mgr);

    return handle;
}

void loopyIdleStop(loopyIdleHandle *handle) {
    if (!handle || !handle->active) {
        return;
    }

    handle->active = false;

    loopyIdleManager *mgr = idleGetManager(handle->loop);
    if (mgr && mgr->idleActiveCount > 0) {
        mgr->idleActiveCount--;
        idleUpdateTimer(mgr);
    }
}

bool loopyIdleRestart(loopyIdleHandle *handle) {
    if (!handle || handle->active) {
        return false;
    }

    handle->active = true;

    loopyIdleManager *mgr = idleGetManager(handle->loop);
    if (mgr) {
        mgr->idleActiveCount++;
        idleUpdateTimer(mgr);
    }

    return true;
}

void loopyIdleFree(loopyIdleHandle *handle) {
    if (!handle) {
        return;
    }

    /* Stop if active */
    if (handle->active) {
        loopyIdleStop(handle);
    }

    /* Remove from list */
    loopyIdleManager *mgr = idleGetManager(handle->loop);
    if (mgr) {
        if (handle->prev) {
            handle->prev->next = handle->next;
        } else {
            mgr->idleList = handle->next;
        }
        if (handle->next) {
            handle->next->prev = handle->prev;
        }

        /* Destroy manager if empty */
        if (!mgr->idleList && !mgr->prepareList && !mgr->checkList) {
            idleDestroyManager(mgr);
        }
    }

    zfree(handle);
}

bool loopyIdleIsActive(const loopyIdleHandle *handle) {
    return handle ? handle->active : false;
}

loopyLoop *loopyIdleGetLoop(const loopyIdleHandle *handle) {
    return handle ? handle->loop : NULL;
}

/* ====================================================================
 * Prepare Handle API
 * ==================================================================== */

loopyPrepareHandle *loopyPrepareStart(loopyLoop *loop, loopyPrepareCallback *cb,
                                      void *userData) {
    if (!loop || !cb) {
        return NULL;
    }

    loopyIdleManager *mgr = idleCreateManager(loop);
    if (!mgr) {
        return NULL;
    }

    loopyPrepareHandle *handle = zcalloc(1, sizeof(*handle));
    if (!handle) {
        return NULL;
    }

    handle->loop = loop;
    handle->cb = cb;
    handle->userData = userData;
    handle->active = true;

    /* Add to list */
    handle->next = mgr->prepareList;
    if (mgr->prepareList) {
        mgr->prepareList->prev = handle;
    }
    mgr->prepareList = handle;

    mgr->prepareActiveCount++;

    return handle;
}

void loopyPrepareStop(loopyPrepareHandle *handle) {
    if (!handle || !handle->active) {
        return;
    }

    handle->active = false;

    loopyIdleManager *mgr = idleGetManager(handle->loop);
    if (mgr && mgr->prepareActiveCount > 0) {
        mgr->prepareActiveCount--;
    }
}

bool loopyPrepareRestart(loopyPrepareHandle *handle) {
    if (!handle || handle->active) {
        return false;
    }

    handle->active = true;

    loopyIdleManager *mgr = idleGetManager(handle->loop);
    if (mgr) {
        mgr->prepareActiveCount++;
    }

    return true;
}

void loopyPrepareFree(loopyPrepareHandle *handle) {
    if (!handle) {
        return;
    }

    /* Stop if active */
    if (handle->active) {
        loopyPrepareStop(handle);
    }

    /* Remove from list */
    loopyIdleManager *mgr = idleGetManager(handle->loop);
    if (mgr) {
        if (handle->prev) {
            handle->prev->next = handle->next;
        } else {
            mgr->prepareList = handle->next;
        }
        if (handle->next) {
            handle->next->prev = handle->prev;
        }

        /* Destroy manager if empty */
        if (!mgr->idleList && !mgr->prepareList && !mgr->checkList) {
            idleDestroyManager(mgr);
        }
    }

    zfree(handle);
}

bool loopyPrepareIsActive(const loopyPrepareHandle *handle) {
    return handle ? handle->active : false;
}

loopyLoop *loopyPrepareGetLoop(const loopyPrepareHandle *handle) {
    return handle ? handle->loop : NULL;
}

/* ====================================================================
 * Check Handle API
 * ==================================================================== */

loopyCheckHandle *loopyCheckStart(loopyLoop *loop, loopyCheckCallback *cb,
                                  void *userData) {
    if (!loop || !cb) {
        return NULL;
    }

    loopyIdleManager *mgr = idleCreateManager(loop);
    if (!mgr) {
        return NULL;
    }

    loopyCheckHandle *handle = zcalloc(1, sizeof(*handle));
    if (!handle) {
        return NULL;
    }

    handle->loop = loop;
    handle->cb = cb;
    handle->userData = userData;
    handle->active = true;

    /* Add to list */
    handle->next = mgr->checkList;
    if (mgr->checkList) {
        mgr->checkList->prev = handle;
    }
    mgr->checkList = handle;

    mgr->checkActiveCount++;

    return handle;
}

void loopyCheckStop(loopyCheckHandle *handle) {
    if (!handle || !handle->active) {
        return;
    }

    handle->active = false;

    loopyIdleManager *mgr = idleGetManager(handle->loop);
    if (mgr && mgr->checkActiveCount > 0) {
        mgr->checkActiveCount--;
    }
}

bool loopyCheckRestart(loopyCheckHandle *handle) {
    if (!handle || handle->active) {
        return false;
    }

    handle->active = true;

    loopyIdleManager *mgr = idleGetManager(handle->loop);
    if (mgr) {
        mgr->checkActiveCount++;
    }

    return true;
}

void loopyCheckFree(loopyCheckHandle *handle) {
    if (!handle) {
        return;
    }

    /* Stop if active */
    if (handle->active) {
        loopyCheckStop(handle);
    }

    /* Remove from list */
    loopyIdleManager *mgr = idleGetManager(handle->loop);
    if (mgr) {
        if (handle->prev) {
            handle->prev->next = handle->next;
        } else {
            mgr->checkList = handle->next;
        }
        if (handle->next) {
            handle->next->prev = handle->prev;
        }

        /* Destroy manager if empty */
        if (!mgr->idleList && !mgr->prepareList && !mgr->checkList) {
            idleDestroyManager(mgr);
        }
    }

    zfree(handle);
}

bool loopyCheckIsActive(const loopyCheckHandle *handle) {
    return handle ? handle->active : false;
}

loopyLoop *loopyCheckGetLoop(const loopyCheckHandle *handle) {
    return handle ? handle->loop : NULL;
}

/* ====================================================================
 * Query Functions
 * ==================================================================== */

size_t loopyIdleCount(const loopyLoop *loop) {
    const loopyIdleManager *mgr = idleGetManager(loop);
    return mgr ? mgr->idleActiveCount : 0;
}

size_t loopyPrepareCount(const loopyLoop *loop) {
    const loopyIdleManager *mgr = idleGetManager(loop);
    return mgr ? mgr->prepareActiveCount : 0;
}

size_t loopyCheckCount(const loopyLoop *loop) {
    const loopyIdleManager *mgr = idleGetManager(loop);
    return mgr ? mgr->checkActiveCount : 0;
}

bool loopyHasActiveIdle(const loopyLoop *loop) {
    const loopyIdleManager *mgr = idleGetManager(loop);
    return mgr && mgr->idleActiveCount > 0;
}

/* ====================================================================
 * Handle Accessors
 * ==================================================================== */

void *loopyIdleGetData(const loopyIdleHandle *handle) {
    return handle ? handle->userData : NULL;
}

void loopyIdleSetData(loopyIdleHandle *handle, void *data) {
    if (handle) {
        handle->userData = data;
    }
}

void *loopyPrepareGetData(const loopyPrepareHandle *handle) {
    return handle ? handle->userData : NULL;
}

void loopyPrepareSetData(loopyPrepareHandle *handle, void *data) {
    if (handle) {
        handle->userData = data;
    }
}

void *loopyCheckGetData(const loopyCheckHandle *handle) {
    return handle ? handle->userData : NULL;
}

void loopyCheckSetData(loopyCheckHandle *handle, void *data) {
    if (handle) {
        handle->userData = data;
    }
}
