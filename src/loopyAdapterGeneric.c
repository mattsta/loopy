/* Select()-based loopy.c module.
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

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <string.h>
#include <sys/select.h>

typedef struct loopyInternalState {
    fd_set rfds;
    fd_set wfds;
    /* We need to have a copy of the fd sets as it's not safe to reuse
     * FD sets after select(). */
    fd_set _rfds;
    fd_set _wfds;
} loopyInternalState;

static bool loopyInternalNew(loopyLoop *l) {
    loopyInternalState *state = zcalloc(1, sizeof(*state));

    if (!state) {
        return false;
    }

    FD_ZERO(&state->rfds);
    FD_ZERO(&state->wfds);

    l->state = state;
    return true;
}

static bool loopyInternalResize(loopyLoop *l, size_t setSize) {
    (void)l;

    /* Just ensure we have enough room in the fd_set type. */
    /* FD_SETSIZE is usually defined to be INT32_MAX or the highest
     * possible file descriptor integer */
    if (setSize >= FD_SETSIZE) {
        return false;
    }

    return true;
}

static void loopyInternalFree(loopyLoop *l) {
    if (l) {
        zfree(l->state);
        l->state = NULL;  /* Clear state pointer to prevent double-free */
    }
}

static bool loopyInternalAddEvent(loopyLoop *l, int fd, loopyAction mask) {
    loopyInternalState *state = l->state;

    if (loopyActionIsRead(mask)) {
        FD_SET(fd, &state->rfds);
    }

    if (loopyActionIsWrite(mask)) {
        FD_SET(fd, &state->wfds);
    }

    return true;
}

static void loopyInternalDelEvent(loopyLoop *l, int fd, loopyAction mask) {
    loopyInternalState *state = l->state;

    if (loopyActionIsRead(mask)) {
        FD_CLR(fd, &state->rfds);
    }

    if (loopyActionIsWrite(mask)) {
        FD_CLR(fd, &state->wfds);
    }
}

static int loopyInternalPoll(loopyLoop *l, const struct timeval *tvp) {
    loopyInternalState *state = l->state;

    memcpy(&state->_rfds, &state->rfds, sizeof(state->_rfds));
    memcpy(&state->_wfds, &state->wfds, sizeof(state->_wfds));

    int numevents = 0;
    /* Make a local copy since select() may modify the timeout on some systems
     */
    struct timeval tv_copy;
    struct timeval *tv_ptr = NULL;
    if (tvp) {
        tv_copy = *tvp;
        tv_ptr = &tv_copy;
    }
    int retval =
        select(l->maxfd + 1, &state->_rfds, &state->_wfds, NULL, tv_ptr);
    if (retval > 0) {
        for (int j = 0; j <= l->maxfd; j++) {
            loopyAction mask = LOOPY_ACTION_NONE;
            const loopyFileEvent *fe = &l->events[j];

            if (fe->mask == LOOPY_ACTION_NONE) {
                continue;
            }

            if (loopyActionIsRead(fe->mask) && FD_ISSET(j, &state->_rfds)) {
                mask |= LOOPY_ACTION_READ;
            }

            if (loopyActionIsWrite(fe->mask) && FD_ISSET(j, &state->_wfds)) {
                mask |= LOOPY_ACTION_WRITE;
            }

            l->fired[numevents].fd = j;
            l->fired[numevents].mask = mask;

            /* We increment here because the 'continue' above may let us
             * skip some of the 'j' indices. */
            numevents++;
        }
    }

    return numevents;
}

static char *loopyInternalName(void) {
    return "select";
}

/* io_uring is Linux-only; generic adapter uses select() */
bool loopyUsingIoUring(const loopyLoop *l) {
    (void)l;
    return false;
}
