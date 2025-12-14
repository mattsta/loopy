/* Linux epoll(2) based loopy.c module
 *
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include <sys/epoll.h>

typedef struct loopyInternalState {
    struct epoll_event *events;
#if 0
    struct itimerspec timerTimeout;
    int timerFd; /* not implemented yet */
#endif
    int epollFd;
} loopyInternalState;

static bool loopyInternalNew(loopyLoop *l) {
    loopyInternalState *state = zcalloc(1, sizeof(*state));

    if (!state) {
        return false;
    }

    state->events = zcalloc(l->setSize, sizeof(*state->events));
    if (!state->events) {
        zfree(state);
        return false;
    }

    state->epollFd = epoll_create(1024); /* 1024 is just a hint */
    if (state->epollFd == -1) {
        zfree(state->events);
        zfree(state);
        return false;
    }

#if 0
#ifdef TFD_NONBLOCK
    state->timerFd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
#else
    state->timerFd = timerfd_create(CLOCK_MONOTONIC, 0);
    fcntl(state->timerFd, F_SETFL, O_NONBLOCK);
#endif
#endif

    l->state = state;
    return true;
}

static bool loopyInternalResize(loopyLoop *l, size_t setSize) {
    loopyInternalState *state = l->state;

    state->events = zrealloc(state->events, sizeof(*state->events) * setSize);
    return true;
}

static void loopyInternalFree(loopyLoop *l) {
    if (l && l->state) {
        loopyInternalState *state = l->state;

        close(state->epollFd);
#if 0
        close(state->timerFd);
#endif
        zfree(state->events);
        zfree(state);
        l->state = NULL;  /* Clear state pointer to prevent double-free */
    }
}

static bool loopyInternalAddEvent(loopyLoop *l, int fd, loopyAction mask) {
    loopyInternalState *state = l->state;
    struct epoll_event ee = {0};

    /* If fd was already monitored for some event, we need a MOD
     * operation. Otherwise need ADD operation. */
    int op =
        l->events[fd].mask == LOOPY_ACTION_NONE ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;

    ee.events = 0;
    mask |= l->events[fd].mask; /* Merge old events */
    if (loopyActionIsRead(mask)) {
        ee.events |= EPOLLIN;
    }

    if (loopyActionIsWrite(mask)) {
        ee.events |= EPOLLOUT;
    }

    ee.data.fd = fd;

    if (epoll_ctl(state->epollFd, op, fd, &ee) == -1) {
        return false;
    }

    return true;
}

static void loopyInternalDelEvent(loopyLoop *l, int fd, loopyAction delmask) {
    loopyInternalState *state = l->state;
    struct epoll_event ee = {0};
    loopyAction mask = l->events[fd].mask & (~delmask);

    ee.events = 0;
    if (loopyActionIsRead(mask)) {
        ee.events |= EPOLLIN;
    }

    if (loopyActionIsWrite(mask)) {
        ee.events |= EPOLLOUT;
    }

    ee.data.fd = fd;

    if (mask != LOOPY_ACTION_NONE) {
        epoll_ctl(state->epollFd, EPOLL_CTL_MOD, fd, &ee);
    } else {
        /* Note, Kernel < 2.6.9 requires a non null event pointer even for
         * EPOLL_CTL_DEL. */
        epoll_ctl(state->epollFd, EPOLL_CTL_DEL, fd, &ee);
    }
}

static int loopyInternalPoll(loopyLoop *l, const struct timeval *tvp) {
    loopyInternalState *state = l->state;
    int retval, numevents = 0;

    /* If you need greater than 1 ms timeout resolution,
     * look into creating a custom timerfd_create interface for
     * unblocking the epoll with greater than 1 ms resolution */

    /* timeout of -1 means block forever waiting for events.
     * timeout of 0 means return immediately. */
    int timeoutMs = tvp ? ((tvp->tv_sec * 1000) + (tvp->tv_usec / 1000)) : -1;

#if 0
    state->timerTimeout.it_value.tv_sec = tvp->tv_sec;
    state->timerTimeout.it_value.tv_nsec = tvp->tv_usec * 1000;
    timerfd_settime(state->timerFd, 0, &state->timerTimeout, NULL);
#endif

    retval = epoll_wait(state->epollFd, state->events, l->setSize, timeoutMs);
    if (retval > 0) {
        numevents = retval;
        for (int j = 0; j < numevents; j++) {
            loopyAction mask = LOOPY_ACTION_NONE;
            struct epoll_event *e = state->events + j;

            if (e->events & EPOLLIN) {
                mask |= LOOPY_ACTION_READ;
            }

            if (e->events & EPOLLOUT) {
                mask |= LOOPY_ACTION_WRITE;
            }

            if (e->events & EPOLLERR) {
                mask |= LOOPY_ACTION_WRITE;
            }

            if (e->events & EPOLLHUP) {
                mask |= LOOPY_ACTION_WRITE;
            }

            l->fired[j].fd = e->data.fd;
            l->fired[j].mask = mask;
        }
    }

    return numevents;
}

static char *loopyInternalName(void) {
    return "epoll";
}

/* io_uring stub - this adapter uses epoll only */
bool loopyUsingIoUring(const loopyLoop *l) {
    (void)l;
    return false;
}
