/* loopy - An efficient space-time aware event loop
 *
 * Copyright 2016 Matt Stancliff <matt@genges.com>
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
#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "loopyInternal.h"
#include "loopyMetrics.h"

#include "../deps/datakit/src/datakit.h"
#include "../deps/datakit/src/timeUtil.h"

/* These are protocol functions defined by each individual adapter */
static bool loopyInternalNew(loopyLoop *l);
static bool loopyInternalResize(loopyLoop *l, size_t setSize);
static void loopyInternalFree(loopyLoop *l);
static bool loopyInternalAddEvent(loopyLoop *l, int fd, loopyAction mask);
static void loopyInternalDelEvent(loopyLoop *l, int fd, loopyAction mask);
static int loopyInternalPoll(loopyLoop *l, const struct timeval *tvp);
static char *loopyInternalName(void);

/* Local interface */
static void loopyFree(loopyLoop *l);

/* Tiny array-based data structure doubling as an O(1) lookup
 * table for FDs. We just use the FD as an index into the
 * array. */
#define fdStorageOkay(checkFd) ((checkFd) < l->setSize)

static bool fdStorageResize(loopyLoop *l, int setSize) {
    if (setSize == l->setSize) {
        return false;
    }

    /* Grow-only for now */
    assert(setSize > l->setSize);

    if (l->maxfd >= setSize) {
        /* can't resize becase our highest tracked fd is larger
         * than this resize request (which would end up being a shrink) */
        return false;
    }

    /* Grow (or shrink) to new 'setSize' */
    l->events = zrealloc(l->events, sizeof(*l->events) * setSize);
    l->fired = zrealloc(l->fired, sizeof(*l->fired) * setSize);

#if 1
    /* zero out newly allocated memory starting at previously highest offset */
    /* This math is okay becasue we are only GROWING here */
    memset(l->events + l->setSize, 0,
           (setSize - l->setSize) * sizeof(*l->events));
    memset(l->fired + l->setSize, 0,
           (setSize - l->setSize) * sizeof(*l->fired));
#endif

    l->setSize = setSize;

    if (l->state) {
        return loopyInternalResize(l, l->setSize);
    }

    return true;
}

/* Smart growth: directly calculate target size instead of repeated doubling.
 * For a fd of 100000 with initial setSize of 16, the naive loop would require
 * 12 realloc calls. This version computes the target size and resizes once. */
static inline void fdConformStorageBounds(loopyLoop *l, int fd) {
    if (likely(fdStorageOkay(fd))) {
        return;
    }

    /* Calculate minimum power-of-two size to hold fd.
     * We double until we exceed fd, then use that as the new size.
     * This provides O(1) amortized growth while ensuring alignment. */
    int newSize = l->setSize;
    while (newSize <= fd) {
        newSize *= 2;
    }

    fdStorageResize(l, newSize);
}

bool loopyInit(loopyLoop *l, int setSize) {
    if (!fdStorageResize(l, setSize)) {
        return false;
    }

    l->maxfd = -1;

    /* Initialize the fd max-heap for O(1) maxfd tracking */
    if (!loopyMaxHeapInit(&l->fdHeap, setSize)) {
        return false;
    }

    if (!loopyInternalNew(l)) {
        /* We don't free 'l' here because it could
         * be a stack allocated loopy */
        loopyMaxHeapDeinit(&l->fdHeap);
        return false;
    }

    return true;
}

bool loopyInited(const loopyLoop *l) {
    return !!l->state;
}

loopyLoop *loopyNew(int setSize) {
    loopyLoop *l = zcalloc(1, sizeof(*l));

    if (!l) {
        return NULL;
    }

    /* We also allow 'loopyLoop' to be struct allocated and initialized to all
     * zeroes, so set an 'allocated' flag for loopyFree() to check if we can
     * free 'l' directly later. */
    l->allocated = true;

    if (!loopyInit(l, setSize)) {
        loopyFree(l);
        return NULL;
    }

    assert(l->allocated);
    return l;
}

static void loopyFree(loopyLoop *l) {
    if (l) {
        loopyDeinit(l);

        if (l->allocated) {
            zfree(l);
        }
    }
}

/* Return the current set size. */
int loopyGetSetSize(const loopyLoop *l) {
    return l->setSize;
}

/* Return the highest fd currently registered. */
int loopyGetMaxFd(const loopyLoop *l) {
    return l->maxfd;
}

/* Check if the event loop has been stopped. */
bool loopyIsStopped(const loopyLoop *l) {
    return l->stop;
}

/* Get user-associated data. */
void *loopyGetUserData(const loopyLoop *l) {
    return l->userData;
}

/* Set user-associated data. */
void loopySetUserData(loopyLoop *l, void *data) {
    l->userData = data;
}

/* Return size of loopyLoop struct for stack allocation. */
size_t loopySize(void) {
    return sizeof(loopyLoop);
}

bool loopyResizeSetSize(loopyLoop *l, int setSize) {
    fdStorageResize(l, setSize);
    return true;
}

void loopyDeinit(loopyLoop *l) {
    if (l) {
        /* Mark as deleting FIRST to prevent use-after-free in callbacks */
        l->deleting = true;

        /* Disable metrics first as it may access internal state */
        loopyMetricsDisable(l);

        /* Free timer wheel if exists */
        if (l->timer) {
            timerWheelFree(l->timer);
            l->timer = NULL;
        }

        /* Free max heap - this handles its own null checks */
        loopyMaxHeapDeinit(&l->fdHeap);

        /* Free fd storage arrays */
        if (l->events) {
            zfree(l->events);
            l->events = NULL;
        }

        if (l->fired) {
            zfree(l->fired);
            l->fired = NULL;
        }

        /* Free platform-specific internal state, safe to call even if already
         * freed */
        loopyInternalFree(l);

        /* Zero out structure to ensure clean state */
        *l = (loopyLoop){0};
    }
}

void loopyDelete(loopyLoop *l) {
    if (l) {
        /* IDEMPOTENT: If already deleted (state is NULL), nothing to do.
         * This handles test suite cleanup calling delete after deferred
         * completion. */
        if (!l->state && !l->timer && !l->events) {
            return; /* Already deleted */
        }

        /* DEFERRED DELETION: If we're currently processing events,
         * defer the actual deletion until processing completes.
         * This prevents use-after-free when callbacks trigger deletion. */
        if (l->processingDepth > 0) {
            l->pendingDelete = true;
            loopyStop(l); /* Stop the event loop */
            return;       /* Defer actual deletion */
        }

        /* Safe to delete now - not currently processing events */
        /* Save allocated because loopyDeinit() zeroes out 'l' memory */
        const bool isAllocated = l->allocated;
        loopyInternalFree(l);

        loopyDeinit(l);

        if (isAllocated) {
            zfree(l);
        }
    }
}

void loopyStop(loopyLoop *l) {
    l->stop = true;
}

static bool _loopyNewFileEvent(loopyLoop *l, int fd, loopyAction mask,
                               loopyFileCallback *cb, void *clientData) {
    fdConformStorageBounds(l, fd);

    loopyFileEvent *fe = &l->events[fd];
    const bool wasEmpty = (fe->mask == LOOPY_ACTION_NONE);

    // cppcheck-suppress knownConditionTrueFalse
    if (!loopyInternalAddEvent(l, fd, mask)) {
        return false;
    }

    fe->mask |= mask;
    if (loopyActionIsRead(mask)) {
        fe->readCallback = cb;
    }

    if (loopyActionIsWrite(mask)) {
        fe->writeCallback = cb;
    }

    fe->clientData = clientData;

    /* Add fd to heap if this is a new registration */
    if (wasEmpty) {
        loopyMaxHeapInsert(&l->fdHeap, fd);
    }

    /* Update maxfd from heap - O(1) */
    loopyHeapValue heapMax = loopyMaxHeapPeek(&l->fdHeap);
    l->maxfd = (heapMax != LOOPY_HEAP_EMPTY_VALUE) ? heapMax : -1;

    return true;
}

bool loopyRegisterRead(loopyLoop *l, int fd, loopyFileCallback *cb,
                       void *clientData) {
    return _loopyNewFileEvent(l, fd, LOOPY_ACTION_READ, cb, clientData);
}

bool loopyRegisterWrite(loopyLoop *l, int fd, loopyFileCallback *cb,
                        void *clientData) {
    return _loopyNewFileEvent(l, fd, LOOPY_ACTION_WRITE, cb, clientData);
}

bool loopyRegisterWriteIfNoneExists(loopyLoop *l, int fd, loopyFileCallback *cb,
                                    void *clientData) {
    fdConformStorageBounds(l, fd);

    const loopyFileEvent *const fe = &l->events[fd];
    if (fe->mask & LOOPY_ACTION_WRITE) {
        /* Already registered for writing, don't register again. */
        return false;
    }

    return loopyRegisterWrite(l, fd, cb, clientData);
}

static void _loopyDeleteFileEvent(loopyLoop *l, int fd, loopyAction mask) {
    if (!fdStorageOkay(fd)) {
        /* 'fd' is beyond our storage extent, so we can't delete anything. */
        return;
    }

    loopyFileEvent *fe = &l->events[fd];

    /* If mask is LOOPY_ACTION_NONE, then this 'fd' slot in 'l->events'
     * is already empty.  Nothing to delete. */
    if (fe->mask == LOOPY_ACTION_NONE) {
        return;
    }

    loopyInternalDelEvent(l, fd, mask);
    fe->mask = fe->mask & (~mask);

    /* If we just deleted all active masks, remove fd from heap and update
     * maxfd. Using max-heap gives O(log n) removal instead of O(n) scan. */
    if (fe->mask == LOOPY_ACTION_NONE) {
        loopyMaxHeapRemove(&l->fdHeap, fd);

        /* Update maxfd from heap - O(1) */
        loopyHeapValue heapMax = loopyMaxHeapPeek(&l->fdHeap);
        l->maxfd = (heapMax != LOOPY_HEAP_EMPTY_VALUE) ? heapMax : -1;
    }
}

void loopyUnregisterRead(loopyLoop *l, int fd) {
    _loopyDeleteFileEvent(l, fd, LOOPY_ACTION_READ);
}

void loopyUnregisterWrite(loopyLoop *l, int fd) {
    _loopyDeleteFileEvent(l, fd, LOOPY_ACTION_WRITE);
}

void loopyUnregisterReadWrite(loopyLoop *l, int fd) {
    _loopyDeleteFileEvent(l, fd, LOOPY_ACTION_READ | LOOPY_ACTION_WRITE);
}

loopyAction loopyGetEvents(loopyLoop *l, int fd) {
    if (!fdStorageOkay(fd)) {
        return 0;
    }

    const loopyFileEvent *fe = &l->events[fd];

    return fe->mask;
}

uint64_t loopyRegisterTimer(loopyLoop *l, uint64_t startAfterMicroseconds,
                            uint64_t repeatEveryMicroseconds,
                            timerWheelCallback *cb, void *clientData) {
    if (!l->timer) {
        l->timer = timerWheelNew();
        if (!l->timer) {
            return false;
        }
    }

    return timerWheelRegister(l->timer, startAfterMicroseconds,
                              repeatEveryMicroseconds, cb, clientData);
}

bool loopyUnregisterTimer(loopyLoop *l, timerWheelId id) {
    return timerWheelUnregister(l->timer, id);
}

/* Process every pending time event, then every pending file event
 * (that may be registered by time event callbacks just processed).
 * Without special flags the function sleeps until some file event
 * fires, or when the next time event occurs (if any).
 *
 * If flags is 0, the function does nothing and returns.
 * LOOPY_EVENTS_ALL processes all events
 * LOOPY_EVENTS_FILE processes only file events
 * LOOPY_EVENTS_TIME processes only time events
 * LOOPY_EVENTS_NOWAIT processes all events without any blocking
 *
 * Returns number of events processed. */

#define NOWAIT(f) ((f) & LOOPY_EVENTS_NOWAIT)
#define WAIT(f) (!NOWAIT(f))
#define HAS_TIMERS(f) ((l->timer) && ((f) & LOOPY_EVENTS_TIME))
#define HAS_FDS(f) ((f) & LOOPY_EVENTS_FILE)

static size_t loopyProcessEvents(loopyLoop *l, loopyEvents flags) {
    /* Track processing depth for deferred deletion */
    l->processingDepth++;

    size_t processed = 0;

    const bool hasTimers = HAS_TIMERS(flags);
    const bool requestedWait = WAIT(flags);
    int numevents = 0;
    timerWheelSystemMonotonicUs shortest = 0;
    if (l->maxfd >= 0 || (hasTimers && requestedWait)) {
        struct timeval tv = {0};

        if (hasTimers && requestedWait) {
            shortest = timerWheelNextTimerEventStartUs(l->timer);
            if (shortest > 0) {
                /* Calculate time missing for nearest timer to fire. */
                const int64_t nowUs = timeUtilMonotonicUs();

                /* How long until next timer event? */
                const int64_t startOffsetUs = (int64_t)shortest - nowUs;

                if (startOffsetUs > 0) {
                    tv.tv_sec = startOffsetUs / 1000000;
                    tv.tv_usec = startOffsetUs % 1000000;
                }

                loopyMetricsPollEntry(l);
                numevents = loopyInternalPoll(l, &tv);
                loopyMetricsPollExit(l, numevents);
            } else {
                /* else, 'requestedWait' with no timer, so wait forever */
                loopyMetricsPollEntry(l);
                numevents = loopyInternalPoll(l, NULL);
                loopyMetricsPollExit(l, numevents);
            }
        } else {
            /* If nowait requested, run with immediate return 'tv',
             * else wait forever. */
            if (NOWAIT(flags)) {
                /* poll for any waiting events;
                 * if no events, return immediately. */

                /* here, 'tv' is still initialized zero. */
                loopyMetricsPollEntry(l);
                numevents = loopyInternalPoll(l, &tv);
                loopyMetricsPollExit(l, numevents);
            } else {
                /* poll for events forever until an inbound event appears. */
                loopyMetricsPollEntry(l);
                numevents = loopyInternalPoll(l, NULL);
                loopyMetricsPollExit(l, numevents);
            }
        }

        /* Track events processed */
        loopyMetricsAddEventsProcessed(l, numevents);

        for (int j = 0; j < numevents; j++, processed++) {
            const int fd = l->fired[j].fd;
            loopyFileEvent *fe = &l->events[fd];
            const loopyAction mask = l->fired[j].mask;

            /* fe->mask & mask & ... code: maybe an already processed
             * event removed an element that fired and we still didn't
             * process, so we check if event is still valid. */
            if (fe->mask & loopyActionIsRead(mask)) {
                /* Cache events pointer to detect reallocation during callback.
                 * This avoids unconditional re-fetch on the common path where
                 * no reallocation occurs. */
                const loopyFileEvent *const eventsBefore = l->events;
                fe->readCallback(l, fd, fe->clientData, mask);
                /* Only re-fetch if events array was reallocated during callback
                 */
                if (unlikely(l->events != eventsBefore)) {
                    fe = &l->events[fd];
                }
            }

            if (fe->mask & loopyActionIsWrite(mask)) {
                fe->writeCallback(l, fd, fe->clientData, mask);
            }
        }
    }

    /* Check time events */
    if (hasTimers) {
        /* Only run timer if we have 0 'numevents' (meaning
         * we triggered the timeout) OR if the shortest duration
         * is less than the current monotonic time (or would activate really
         * soon anyway) */
        if (!l->stop && shortest &&
            (!numevents || (shortest + 10) <= (int64_t)timeUtilMonotonicUs())) {
            loopyMetricsIncrementTimersProcessed(l);
            timerWheelProcessTimerEvents(l->timer);

            /* CRITICAL: If timer callback deleted the loop, exit immediately.
             * Accessing ANY field after deletion would be use-after-free. */
            if (l->deleting) {
                l->processingDepth--;
                return processed;
            }
        }
    }

    /* Decrement processing depth before returning */
    l->processingDepth--;

    /* return count of total processed file events */
    return processed;
}

#if 0
/* Removed because... why use it? */
/* Wait for milliseconds until the given file descriptor becomes
 * writable/readable/exception */
int loopyWait(int fd, loopyAction mask, uint64_t milliseconds) {
    struct pollfd pfd = {0};
    int retmask = 0;
    int retval = 0;

    pfd.fd = fd;
    if (loopyActionIsRead(mask)) {
        pfd.events |= POLLIN;
    }

    if (loopyActionIsWrite(mask)) {
        pfd.events |= POLLOUT;
    }

    if ((retval = poll(&pfd, 1, milliseconds)) == 1) {
        if (pfd.revents & POLLIN) {
            retmask |= LOOPY_ACTION_READ;
        }

        if (pfd.revents & POLLOUT) {
            retmask |= LOOPY_ACTION_WRITE;
        }

        if (pfd.revents & POLLERR) {
            retmask |= LOOPY_ACTION_WRITE;
        }

        if (pfd.revents & POLLHUP) {
            retmask |= LOOPY_ACTION_WRITE;
        }

        return retmask;
    }

    return retval;
}
#endif

DK_INLINE_ALWAYS void loopyDo(loopyLoop *l, loopyEvents mode) {
    l->stop = false;
    while (!l->stop) {
        /* Track loop iterations for metrics */
        loopyMetricsIncrementIterations(l);

        if (l->sleep.before.cb) {
            l->sleep.before.cb(l, l->sleep.before.clientData);
            /* Event loop callback could have stopped or deleted us */
            if (l->stop || l->deleting) {
                break;
            }
        }

        loopyProcessEvents(l, mode);

        /* CRITICAL: Check deleting flag after event processing.
         * Timer callbacks may have triggered deletion. */
        if (l->deleting) {
            break;
        }

        if (l->sleep.after.cb && !l->stop) {
            l->sleep.after.cb(l, l->sleep.after.clientData);
            /* Check again after callback */
            if (l->deleting) {
                break;
            }
        }
    }

    /* DEFERRED DELETION COMPLETION: If deletion was requested during
     * event processing, complete it now that the loop has exited safely. */
    if (l->pendingDelete) {
        const bool isAllocated = l->allocated;
        loopyInternalFree(l);
        loopyDeinit(l);
        if (isAllocated) {
            zfree(l);
        }
    }
}

void loopyMain(loopyLoop *l) {
    loopyDo(l, LOOPY_EVENTS_ALL);
}

void loopyMainFdOnly(loopyLoop *l) {
    loopyDo(l, LOOPY_EVENTS_FILE);
}

const char *loopyAdapterName(void) {
    return loopyInternalName();
}

const char *loopyStatusString(loopyStatus status) {
    switch (status) {
    case LOOPY_OK:
        return "OK";
    case LOOPY_ERROR:
        return "Error";
    case LOOPY_INVALID:
        return "Invalid argument";
    case LOOPY_TIMEOUT:
        return "Timeout";
    case LOOPY_CANCELLED:
        return "Cancelled";
    case LOOPY_CLOSED:
        return "Closed";
    case LOOPY_WOULD_BLOCK:
        return "Would block";
    case LOOPY_NOMEM:
        return "Out of memory";
    case LOOPY_NOT_FOUND:
        return "Not found";
    case LOOPY_BUSY:
        return "Busy";
    case LOOPY_AGAIN:
        return "Try again";
    case LOOPY_EOF:
        return "End of file";
    default:
        return "Unknown status";
    }
}

bool loopyHasCapability(loopyCapability cap) {
    switch (cap) {
    /* I/O backends - determined at compile time */
    case LOOPY_CAP_EPOLL:
#ifdef __linux__
        return true;
#else
        return false;
#endif

    case LOOPY_CAP_KQUEUE:
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) ||      \
    defined(__NetBSD__) || defined(__DragonFly__)
        return true;
#else
        return false;
#endif

    case LOOPY_CAP_IOURING:
#ifdef LOOPY_HAVE_IOURING
        return true;
#else
        return false;
#endif

    case LOOPY_CAP_DEVPOLL:
#ifdef __sun
        return true;
#else
        return false;
#endif

    case LOOPY_CAP_SELECT:
        return true; /* Always available as fallback */

    /* Features - always available in this build */
    case LOOPY_CAP_TLS:
        return true; /* mbedTLS is always linked */

    case LOOPY_CAP_DNS_ASYNC:
        return true; /* loopyDNS always available */

    case LOOPY_CAP_FS_EVENTS:
#if defined(__APPLE__) || defined(__linux__) || defined(__FreeBSD__)
        return true; /* kqueue or inotify */
#else
        return false;
#endif

    case LOOPY_CAP_PROCESS:
        return true; /* loopyProcess always available on POSIX */

    case LOOPY_CAP_SIGNALS:
        return true; /* loopySignal always available on POSIX */

    default:
        return false;
    }
}

void loopySetBeforeSleepCallback(loopyLoop *l, loopyCallback *cb,
                                 void *clientData) {
    l->sleep.before.cb = cb;
    l->sleep.before.clientData = clientData;
}

void loopySetAfterSleepCallback(loopyLoop *l, loopyCallback *cb,
                                void *clientData) {
    l->sleep.after.cb = cb;
    l->sleep.after.clientData = clientData;
}

/* Originally, long ago, before updating modern features, and modern standards:
 * Copyright (c) 2006-2010, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
