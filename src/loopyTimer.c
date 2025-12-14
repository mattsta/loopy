/* loopyTimer - High-level Timer Abstraction Implementation
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

#include "loopyInternal.h"
#include "loopyTimer.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../deps/datakit/src/datakit.h"

/* ====================================================================
 * Internal Structure
 * ==================================================================== */

/**
 * Timer handle internal structure.
 *
 * Manages both one-shot and periodic timers with automatic cleanup for
 * one-shot timers.
 */
struct loopyTimer {
    loopyLoop *loop;        /* Event loop */
    timerWheelId timerId;   /* Underlying timerWheel ID */
    loopyTimerCallback *cb; /* User callback */
    void *userData;         /* User context */
    bool isPeriodic;        /* true for periodic, false for one-shot */
    bool isActive;          /* true if timer is active */
    bool inCallback;        /* true if currently executing callback */
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

const char *loopyTimerGetError(void) {
    return lastError[0] ? lastError : NULL;
}

/* ====================================================================
 * Internal Timer Callback Wrapper
 * ==================================================================== */

/**
 * Internal callback that wraps user callback and handles cleanup.
 *
 * For one-shot timers: Calls user callback, then automatically frees the timer
 * For periodic timers: Calls user callback and returns true to continue
 *
 * This bridges between timerWheel's callback signature and loopyTimer's
 * callback signature.
 */
static bool timerWheelCallbackWrapper(timerWheel *tw, timerWheelId id,
                                      void *clientData) {
    (void)tw; /* Unused */
    (void)id; /* Unused - we use timer->timerId */

    loopyTimer *timer = (loopyTimer *)clientData;
    if (!timer || !timer->isActive) {
        return false; /* Timer was cancelled */
    }

    /* Cache timer type before calling callback (callback might cancel timer) */
    bool isPeriodic = timer->isPeriodic;

    /* Mark that we're in callback - prevents loopyTimerCancel from freeing */
    timer->inCallback = true;

    /* Call user callback */
    if (timer->cb) {
        timer->cb(timer->loop, timer, timer->userData);
    }

    /* Mark callback as done */
    timer->inCallback = false;

    /* Check if timer was cancelled during callback */
    if (!timer->isActive) {
        /* Timer was cancelled in callback - free it now */
        zfree(timer);
        return false;
    }

    /* Handle cleanup based on timer type */
    if (isPeriodic) {
        /* Periodic timer - keep running */
        return true;
    } else {
        /* One-shot timer - auto-cleanup */
        timer->isActive = false;
        zfree(timer);
        return false; /* Don't reschedule */
    }
}

/* ====================================================================
 * Internal Timer Creation
 * ==================================================================== */

/**
 * Internal function to create a timer.
 *
 * @param loop Event loop
 * @param startAfterUs Initial delay in microseconds
 * @param intervalUs Repeat interval in microseconds (0 for one-shot)
 * @param cb User callback
 * @param userData User context
 * @return Timer handle, or NULL on failure
 */
static loopyTimer *createTimer(loopyLoop *loop, uint64_t startAfterUs,
                               uint64_t intervalUs, loopyTimerCallback *cb,
                               void *userData) {
    if (!loop) {
        setError("NULL event loop");
        return NULL;
    }

    if (!cb) {
        setError("NULL callback");
        return NULL;
    }

    /* Allocate timer structure */
    loopyTimer *timer = zcalloc(1, sizeof(loopyTimer));
    if (!timer) {
        setError("Memory allocation failed");
        return NULL;
    }

    /* Initialize timer */
    timer->loop = loop;
    timer->cb = cb;
    timer->userData = userData;
    timer->isPeriodic = (intervalUs > 0);
    timer->isActive = true;

    /* Register with underlying timerWheel */
    timer->timerId = loopyRegisterTimer(loop, startAfterUs, intervalUs,
                                        timerWheelCallbackWrapper, timer);

    if (timer->timerId == 0) {
        /* Registration failed */
        setError("Failed to register timer with event loop");
        zfree(timer);
        return NULL;
    }

    lastError[0] = '\0';
    return timer;
}

/* ====================================================================
 * One-Shot Timers
 * ==================================================================== */

loopyTimer *loopyTimerOneShot(loopyLoop *loop, uint64_t delayUs,
                              loopyTimerCallback *cb, void *userData) {
    return createTimer(loop, delayUs, 0, cb, userData);
}

loopyTimer *loopyTimerOneShotMs(loopyLoop *loop, uint64_t delayMs,
                                loopyTimerCallback *cb, void *userData) {
    return loopyTimerOneShot(loop, delayMs * 1000, cb, userData);
}

loopyTimer *loopyTimerOneShotSeconds(loopyLoop *loop, uint64_t delaySeconds,
                                     loopyTimerCallback *cb, void *userData) {
    return loopyTimerOneShot(loop, delaySeconds * 1000000, cb, userData);
}

/* ====================================================================
 * Periodic Timers
 * ==================================================================== */

loopyTimer *loopyTimerPeriodic(loopyLoop *loop, uint64_t intervalUs,
                               loopyTimerCallback *cb, void *userData) {
    return createTimer(loop, intervalUs, intervalUs, cb, userData);
}

loopyTimer *loopyTimerPeriodicDelayed(loopyLoop *loop, uint64_t initialDelayUs,
                                      uint64_t intervalUs,
                                      loopyTimerCallback *cb, void *userData) {
    return createTimer(loop, initialDelayUs, intervalUs, cb, userData);
}

loopyTimer *loopyTimerPeriodicMs(loopyLoop *loop, uint64_t intervalMs,
                                 loopyTimerCallback *cb, void *userData) {
    return loopyTimerPeriodic(loop, intervalMs * 1000, cb, userData);
}

loopyTimer *loopyTimerPeriodicSeconds(loopyLoop *loop, uint64_t intervalSeconds,
                                      loopyTimerCallback *cb, void *userData) {
    return loopyTimerPeriodic(loop, intervalSeconds * 1000000, cb, userData);
}

/* ====================================================================
 * Timer Management
 * ==================================================================== */

void loopyTimerCancel(loopyTimer *timer) {
    if (!timer) {
        return;
    }

    if (timer->isActive) {
        /* Unregister from timerWheel */
        loopyUnregisterTimer(timer->loop, timer->timerId);
        timer->isActive = false;
    }

    /* If called from within callback, let callback wrapper handle freeing */
    if (timer->inCallback) {
        return;
    }

    /* Free the timer structure */
    zfree(timer);
}

bool loopyTimerIsActive(const loopyTimer *timer) {
    return timer != NULL && timer->isActive;
}

loopyLoop *loopyTimerGetLoop(const loopyTimer *timer) {
    return timer ? timer->loop : NULL;
}

void *loopyTimerGetData(const loopyTimer *timer) {
    return timer ? timer->userData : NULL;
}

bool loopyTimerSetData(loopyTimer *timer, void *userData) {
    if (!timer || !timer->isActive) {
        return false;
    }

    timer->userData = userData;
    return true;
}
