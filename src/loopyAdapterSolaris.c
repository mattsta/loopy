/* loopy.c module for illumos event ports.
 *
 * Copyright (c) 2012, Joyent, Inc. All rights reserved.
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

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <port.h>

#include <sys/time.h>
#include <sys/types.h>

#include <stdio.h>

static int evport_debug = 0;

/*
 * This file implements the loopy API using event ports, present on
 * Solaris-based
 * systems since Solaris 10.  Using the event port interface, we associate file
 * descriptors with the port.  Each association also includes the set of poll(2)
 * events that the consumer is interested in (e.g., POLLIN and POLLOUT).
 *
 * There's one tricky piece to this implementation: when we return events via
 * loopyInternalPoll, the corresponding file descriptors become dissociated from
 * the
 * port.  This is necessary because poll events are level-triggered, so if the
 * fd didn't become dissociated, it would immediately fire another event since
 * the underlying state hasn't changed yet.  We must re-associate the file
 * descriptor, but only after we know that our caller has actually read from it.
 * The loopy API does not tell us exactly when that happens, but we do know that
 * it must happen by the time loopyInternalPoll is called again.  Our solution
 * is to
 * keep track of the last fds returned by loopyInternalPoll and re-associate
 * them next
 * time loopyInternalPoll is invoked.
 *
 * To summarize, in this module, each fd association is EITHER (a) represented
 * only via the in-kernel association OR (b) represented by pending_fds and
 * pending_masks.  (b) is only true for the last fds we returned from
 * loopyInternalPoll,
 * and only until we enter loopyInternalPoll again (at which point we restore
 * the
 * in-kernel association).
 */
#define MAX_EVENT_BATCHSZ 512

typedef struct loopyInternalState {
    int portfd;                           /* event port */
    int npending;                         /* # of pending fds */
    int pending_fds[MAX_EVENT_BATCHSZ];   /* pending fds */
    int pending_masks[MAX_EVENT_BATCHSZ]; /* pending fds' masks */
} loopyInternalState;

static bool loopyInternalNew(loopyLoop *l) {
    loopyInternalState *state = zcalloc(1, sizeof(*state));
    if (!state) {
        return false;
    }

    state->portfd = port_create();
    if (state->portfd == -1) {
        zfree(state);
        return false;
    }

    state->npending = 0;

    for (int i = 0; i < MAX_EVENT_BATCHSZ; i++) {
        state->pending_fds[i] = -1;
        state->pending_masks[i] = LOOPY_ACTION_NONE;
    }

    l->state = state;
    return true;
}

static bool loopyInternalResize(loopyLoop *l, size_t setSize) {
    (void)l;
    (void)setSize;
    /* Nothing to resize here. */
    return true;
}

static void loopyInternalFree(loopyLoop *l) {
    if (l && l->state) {
        loopyInternalState *state = l->state;

        close(state->portfd);
        zfree(state);
        l->state = NULL;  /* Clear state pointer to prevent double-free */
    }
}

static int loopyInternalLookupPending(const loopyInternalState *state, int fd) {
    int i;

    for (i = 0; i < state->npending; i++) {
        if (state->pending_fds[i] == fd) {
            return (i);
        }
    }

    return (-1);
}

/*
 * Helper function to invoke port_associate for the given fd and mask.
 */
static int loopyInternalAssociate(const char *where, int portfd, int fd,
                                  loopyAction mask) {
    int events = 0;
    int rv, err;

    if (loopyActionIsRead(mask)) {
        events |= POLLIN;
    }

    if (loopyActionIsWrite(mask)) {
        events |= POLLOUT;
    }

    if (evport_debug) {
        fprintf(stderr, "%s: port_associate(%d, 0x%x) = ", where, fd, events);
    }

    rv = port_associate(portfd, PORT_SOURCE_FD, fd, events,
                        (void *)(uintptr_t)mask);
    err = errno;

    if (evport_debug) {
        fprintf(stderr, "%d (%s)\n", rv, rv == 0 ? "no error" : strerror(err));
    }

    if (rv == -1) {
        fprintf(stderr, "%s: port_associate: %s\n", where, strerror(err));

        if (err == EAGAIN) {
            fprintf(stderr,
                    "loopyInternalAssociate: event port limit exceeded.");
        }
    }

    return rv;
}

static bool loopyInternalAddEvent(loopyLoop *l, int fd, loopyAction mask) {
    loopyInternalState *state = l->state;

    if (evport_debug) {
        fprintf(stderr, "loopyInternalAddEvent: fd %d mask 0x%x\n", fd, mask);
    }

    /*
     * Since port_associate's "events" argument replaces any existing events, we
     * must be sure to include whatever events are already associated when
     * we call port_associate() again.
     */
    loopyAction fullmask = mask | l->events[fd].mask;
    int pfd = loopyInternalLookupPending(state, fd);

    if (pfd != -1) {
        /*
         * This fd was recently returned from loopyInternalPoll.  It should be
         * safe to
         * assume that the consumer has processed that poll event, but we play
         * it safer by simply updating pending_mask.  The fd will be
         * re-associated as usual when loopyInternalPoll is called again.
         */
        if (evport_debug) {
            fprintf(stderr, "loopyInternalAddEvent: adding to pending fd %d\n",
                    fd);
        }

        state->pending_masks[pfd] |= fullmask;
        return true;
    }

    return loopyInternalAssociate("loopyInternalAddEvent", state->portfd, fd,
                                  fullmask) == 0;
}

static void loopyInternalDelEvent(loopyLoop *l, int fd, loopyAction mask) {
    loopyInternalState *state = l->state;
    int fullmask, pfd;

    if (evport_debug) {
        fprintf(stderr, "del fd %d mask 0x%x\n", fd, mask);
    }

    pfd = loopyInternalLookupPending(state, fd);

    if (pfd != -1) {
        if (evport_debug) {
            fprintf(stderr, "deleting event from pending fd %d\n", fd);
        }

        /*
         * This fd was just returned from loopyInternalPoll, so it's not
         * currently
         * associated with the port.  All we need to do is update
         * pending_mask appropriately.
         */
        state->pending_masks[pfd] &= ~mask;

        if (state->pending_masks[pfd] == LOOPY_ACTION_NONE) {
            state->pending_fds[pfd] = -1;
        }

        return;
    }

    /*
     * The fd is currently associated with the port.  Like with the add case
     * above, we must look at the full mask for the file descriptor before
     * updating that association.  We don't have a good way of knowing what the
     * events are without looking into the l state directly.  We rely on
     * the fact that our caller has already updated the mask in the l.
     */

    fullmask = l->events[fd].mask;
    if (fullmask == LOOPY_ACTION_NONE) {
        /*
         * We're removing *all* events, so use port_dissociate to remove the
         * association completely.  Failure here indicates a bug.
         */
        if (evport_debug) {
            fprintf(stderr, "loopyInternalDelEvent: port_dissociate(%d)\n", fd);
        }

        if (port_dissociate(state->portfd, PORT_SOURCE_FD, fd) != 0) {
            perror("loopyInternalDelEvent: port_dissociate");
            abort(); /* will not return */
        }
    } else if (loopyInternalAssociate("loopyInternalDelEvent", state->portfd,
                                      fd, fullmask) != 0) {
        /*
         * ENOMEM is a potentially transient condition, but the kernel won't
         * generally return it unless things are really bad.  EAGAIN indicates
         * we've reached an resource limit, for which it doesn't make sense to
         * retry (counter-intuitively).  All other errors indicate a bug.  In
         * any
         * of these cases, the best we can do is to abort.
         */
        abort(); /* will not return */
    }
}

static int loopyInternalPoll(loopyLoop *l, const struct timeval *tvp) {
    loopyInternalState *state = l->state;
    struct timespec timeout, *tsp;
    uint_t nevents;
    port_event_t event[MAX_EVENT_BATCHSZ];

    /*
     * If we've returned fd events before, we must re-associate them with the
     * port now, before calling port_get().  See the block comment at the top of
     * this file for an explanation of why.
     */
    for (int i = 0; i < state->npending; i++) {
        if (state->pending_fds[i] == -1) {
            /* This fd has since been deleted. */
            continue;
        }

        if (loopyInternalAssociate("loopyInternalPoll", state->portfd,
                                   state->pending_fds[i],
                                   state->pending_masks[i]) != 0) {
            /* See loopyInternalDelEvent for why this case is fatal. */
            abort();
        }

        state->pending_masks[i] = LOOPY_ACTION_NONE;
        state->pending_fds[i] = -1;
    }

    state->npending = 0;

    if (tvp != NULL) {
        timeout.tv_sec = tvp->tv_sec;
        timeout.tv_nsec = tvp->tv_usec * 1000;
        tsp = &timeout;
    } else {
        tsp = NULL;
    }

    /*
     * port_getn can return with errno == ETIME having returned some events (!).
     * So if we get ETIME, we check nevents, too.
     */
    nevents = 1;
    if (port_getn(state->portfd, event, MAX_EVENT_BATCHSZ, &nevents, tsp) ==
            -1 &&
        (errno != ETIME || nevents == 0)) {
        if (errno == ETIME || errno == EINTR) {
            return 0;
        }

        /* Any other error indicates a bug. */
        perror("loopyInternalPoll: port_get");
        abort();
    }

    state->npending = nevents;

    for (uint32_t i = 0; i < nevents; i++) {
        loopyAction mask = LOOPY_ACTION_NONE;
        if (event[i].portev_events & POLLIN) {
            mask |= LOOPY_ACTION_READ;
        }

        if (event[i].portev_events & POLLOUT) {
            mask |= LOOPY_ACTION_WRITE;
        }

        l->fired[i].fd = event[i].portev_object;
        l->fired[i].mask = mask;

        if (evport_debug) {
            fprintf(stderr, "loopyInternalPoll: fd %d mask 0x%x\n",
                    (int)event[i].portev_object, mask);
        }

        state->pending_fds[i] = event[i].portev_object;
        state->pending_masks[i] = (uintptr_t)event[i].portev_user;
    }

    return nevents;
}

static char *loopyInternalName(void) {
    return "evport";
}

/* io_uring is Linux-only; Solaris uses event ports */
bool loopyUsingIoUring(const loopyLoop *l) {
    (void)l;
    return false;
}
