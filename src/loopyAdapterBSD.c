/* Kqueue(2)-based loopy.c module
 *
 * Copyright (C) 2009 Harish Mallipeddi - harish.mallipeddi@gmail.com
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

#include "loopy.c"

#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>

#include "loopy.h"
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct loopyInternalState {
    int kqfd;
    struct kevent *events;
} loopyInternalState;

static void loopyInternalStateFree(loopyInternalState *s) {
    if (s) {
        zfree(s->events);
        zfree(s);
    }
}

static bool loopyInternalNew(loopyLoop *l) {
    loopyInternalState *s = zcalloc(1, sizeof(*s));

    if (!s) {
        return false;
    }

    s->events = zcalloc(l->setSize, sizeof(*s->events));
    if (!s->events) {
        loopyInternalStateFree(s);
        return false;
    }

    s->kqfd = kqueue();
    if (s->kqfd == -1) {
        loopyInternalStateFree(s);
        return false;
    }

    l->state = s;
    return true;
}

static bool loopyInternalResize(loopyLoop *l, size_t setSize) {
    loopyInternalState *s = l->state;

    s->events = zrealloc(s->events, sizeof(*s->events) * setSize);
    return true;
}

static void loopyInternalFree(loopyLoop *l) {
    if (l && l->state) {
        loopyInternalState *s = l->state;

        close(s->kqfd);
        loopyInternalStateFree(s);
        l->state = NULL;  /* Clear state pointer to prevent double-free */
    }
}

static bool loopyInternalAddEvent(loopyLoop *l, int fd, loopyAction mask) {
    loopyInternalState *s = l->state;
    struct kevent changes[2];
    int nchanges = 0;

    /* Batch multiple filter registrations into a single kevent() syscall.
     * This reduces syscalls by 50% when registering both read and write. */
    if (loopyActionIsRead(mask)) {
        EV_SET(&changes[nchanges++], fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    }

    if (loopyActionIsWrite(mask)) {
        EV_SET(&changes[nchanges++], fd, EVFILT_WRITE, EV_ADD, 0, 0, NULL);
    }

    if (nchanges > 0 &&
        kevent(s->kqfd, changes, nchanges, NULL, 0, NULL) == -1) {
        return false;
    }

    return true;
}

static void loopyInternalDelEvent(loopyLoop *l, int fd, loopyAction mask) {
    loopyInternalState *s = l->state;
    struct kevent changes[2];
    int nchanges = 0;

    /* Batch multiple filter deletions into a single kevent() syscall. */
    if (loopyActionIsRead(mask)) {
        EV_SET(&changes[nchanges++], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    }

    if (loopyActionIsWrite(mask)) {
        EV_SET(&changes[nchanges++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    }

    if (nchanges > 0) {
        kevent(s->kqfd, changes, nchanges, NULL, 0, NULL);
    }
}

static int loopyInternalPoll(loopyLoop *l, const struct timeval *timeoutVal) {
    loopyInternalState *s = l->state;

    int numevents = 0;
    if (timeoutVal) {
        const struct timespec timeout = {
            .tv_sec = timeoutVal->tv_sec,
            .tv_nsec = (uint64_t)timeoutVal->tv_usec * 1000};
        numevents = kevent(s->kqfd, NULL, 0, s->events, l->setSize, &timeout);
    } else {
        numevents = kevent(s->kqfd, NULL, 0, s->events, l->setSize, NULL);
    }

    assert(numevents <= l->setSize);

    for (int j = 0; j < numevents; j++) {
        loopyAction mask = 0;
        const struct kevent *e = &s->events[j];

        if (e->filter == EVFILT_READ) {
            mask |= LOOPY_ACTION_READ;
        }

        if (e->filter == EVFILT_WRITE) {
            mask |= LOOPY_ACTION_WRITE;
        }

        l->fired[j].fd = e->ident;
        l->fired[j].mask = mask;
    }

    return numevents;
}

static char *loopyInternalName(void) {
    return "kqueue";
}

/* io_uring is Linux-only; kqueue platforms always return false */
bool loopyUsingIoUring(const loopyLoop *l) {
    (void)l;
    return false;
}
