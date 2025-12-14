/* loopyAsync - Thread-safe event loop wake-up for loopy
 *
 * Implementation uses eventfd on Linux, self-pipe on BSD/macOS.
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

#include "loopyAsync.h"
#include "../deps/datakit/src/datakit.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Platform detection */
#if __linux__
#include <sys/eventfd.h>
#define USE_EVENTFD 1
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) ||    \
    defined(__NetBSD__)
#define USE_PIPE 1
#endif

/* ====================================================================
 * Internal data structure
 * ==================================================================== */

struct loopyAsync {
    loopyLoop *loop;
    loopyAsyncCallback *callback;
    void *userData;
    atomic_int pending; /* Atomic flag for thread safety */
#if USE_EVENTFD
    int eventfd;
#elif USE_PIPE
    int pipe[2]; /* [0] = read end, [1] = write end */
#endif
};

/* ====================================================================
 * Forward declarations
 * ==================================================================== */

static void asyncEventCallback(loopyLoop *l, int fd, void *data,
                               loopyAction mask);

/* ====================================================================
 * Helper functions
 * ==================================================================== */

#if USE_PIPE
static bool setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

static bool setCloseOnExec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    if (flags == -1) {
        return false;
    }
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != -1;
}
#endif

/* ====================================================================
 * Lifecycle
 * ==================================================================== */

loopyAsync *loopyAsyncNew(loopyLoop *loop, loopyAsyncCallback *cb,
                          void *userData) {
    if (!loop || !cb) {
        return NULL;
    }

    loopyAsync *async = zcalloc(1, sizeof(*async));
    if (!async) {
        return NULL;
    }

    async->loop = loop;
    async->callback = cb;
    async->userData = userData;
    atomic_init(&async->pending, 0);

#if USE_EVENTFD
    async->eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (async->eventfd == -1) {
        zfree(async);
        return NULL;
    }

    if (!loopyRegisterRead(loop, async->eventfd, asyncEventCallback, async)) {
        close(async->eventfd);
        zfree(async);
        return NULL;
    }
#elif USE_PIPE
    if (pipe(async->pipe) == -1) {
        zfree(async);
        return NULL;
    }

    /* Make both ends non-blocking and close-on-exec */
    if (!setNonBlocking(async->pipe[0]) || !setNonBlocking(async->pipe[1]) ||
        !setCloseOnExec(async->pipe[0]) || !setCloseOnExec(async->pipe[1])) {
        close(async->pipe[0]);
        close(async->pipe[1]);
        zfree(async);
        return NULL;
    }

    /* Register read end with event loop */
    if (!loopyRegisterRead(loop, async->pipe[0], asyncEventCallback, async)) {
        close(async->pipe[0]);
        close(async->pipe[1]);
        zfree(async);
        return NULL;
    }
#else
    /* No supported backend */
    zfree(async);
    return NULL;
#endif

    return async;
}

void loopyAsyncFree(loopyAsync *async) {
    if (!async) {
        return;
    }

#if USE_EVENTFD
    loopyUnregisterReadWrite(async->loop, async->eventfd);
    close(async->eventfd);
#elif USE_PIPE
    loopyUnregisterReadWrite(async->loop, async->pipe[0]);
    close(async->pipe[0]);
    close(async->pipe[1]);
#endif

    zfree(async);
}

/* ====================================================================
 * Operations
 * ==================================================================== */

void loopyAsyncSend(loopyAsync *async) {
    if (!async) {
        return;
    }

    /* Set pending flag atomically - if already set, we're done.
     * This provides coalescing: multiple sends = one callback */
    int expected = 0;
    if (!atomic_compare_exchange_strong(&async->pending, &expected, 1)) {
        /* Already pending, no need to write to fd again */
        return;
    }

#if USE_EVENTFD
    /* Write 1 to eventfd to wake up the event loop */
    uint64_t val = 1;
    /* Ignore errors - if we can't write, the previous value is still there.
     * This is async-signal-safe because write() is async-signal-safe. */
    ssize_t ret;
    do {
        ret = write(async->eventfd, &val, sizeof(val));
    } while (ret == -1 && errno == EINTR);
#elif USE_PIPE
    /* Write a single byte to wake up the event loop */
    char c = 1;
    ssize_t ret;
    do {
        ret = write(async->pipe[1], &c, 1);
    } while (ret == -1 && errno == EINTR);
    /* Ignore EAGAIN - pipe may be full, but fd is still readable */
#endif
}

bool loopyAsyncPending(const loopyAsync *async) {
    if (!async) {
        return false;
    }
    return atomic_load(&async->pending) != 0;
}

/* ====================================================================
 * Information
 * ==================================================================== */

loopyLoop *loopyAsyncGetLoop(const loopyAsync *async) {
    return async ? async->loop : NULL;
}

void *loopyAsyncGetData(const loopyAsync *async) {
    return async ? async->userData : NULL;
}

void loopyAsyncSetData(loopyAsync *async, void *userData) {
    if (async) {
        async->userData = userData;
    }
}

const char *loopyAsyncBackendName(void) {
#if USE_EVENTFD
    return "eventfd";
#elif USE_PIPE
    return "pipe";
#else
    return "none";
#endif
}

/* ====================================================================
 * Internal: Event callback
 * ==================================================================== */

static void asyncEventCallback(loopyLoop *l, int fd, void *data,
                               loopyAction mask) {
    (void)mask;

    loopyAsync *async = data;

#if USE_EVENTFD
    /* Drain the eventfd */
    uint64_t val;
    ssize_t ret;
    do {
        ret = read(fd, &val, sizeof(val));
    } while (ret == -1 && errno == EINTR);
#elif USE_PIPE
    /* Drain all bytes from the pipe */
    char buf[64];
    ssize_t ret;
    while ((ret = read(fd, buf, sizeof(buf))) > 0) {
        /* Keep draining */
    }
    (void)ret;
#else
    (void)fd;
#endif

    /* Clear pending flag and invoke callback */
    atomic_store(&async->pending, 0);

    if (async->callback) {
        async->callback(l, async, async->userData);
    }
}
