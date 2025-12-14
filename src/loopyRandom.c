/* loopyRandom - Cryptographically secure random number generation
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
#include "loopyRandom.h"
#include "loopyWork.h"

#include <errno.h>
#include <string.h>

#ifdef __linux__
#include <sys/syscall.h>
#include <unistd.h>
#ifndef SYS_getrandom
#define SYS_getrandom 318
#endif
#ifndef GRND_NONBLOCK
#define GRND_NONBLOCK 1
#endif
#else
#include <stdlib.h> /* arc4random_buf on BSD/macOS */
#endif

/* ====================================================================
 * Internal structures
 * ==================================================================== */

struct loopyRandomRequest {
    loopyLoop *loop;
    loopyRandomCallback *cb;
    void *userData;
    void *buf;
    size_t len;
    int result;
    int savedErrno;
    loopyWork *work;
    loopyWorkId workId;
    bool cancelled;
    bool completed;
};

/* ====================================================================
 * Platform-specific random implementation
 * ==================================================================== */

#ifdef __linux__
static ssize_t getRandomBytes(void *buf, size_t len) {
    ssize_t ret;
    size_t done = 0;

    while (done < len) {
        ret = syscall(SYS_getrandom, (char *)buf + done, len - done, 0);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        done += (size_t)ret;
    }
    return (ssize_t)done;
}
#else
static ssize_t getRandomBytes(void *buf, size_t len) {
    arc4random_buf(buf, len);
    return (ssize_t)len;
}
#endif

/* ====================================================================
 * Synchronous API
 * ==================================================================== */

int loopyRandomSync(void *buf, size_t len) {
    if (!buf || len == 0) {
        errno = EINVAL;
        return -1;
    }

    ssize_t ret = getRandomBytes(buf, len);
    return (ret == (ssize_t)len) ? 0 : -1;
}

/* ====================================================================
 * Async work callbacks
 * ==================================================================== */

static void randomWorkCallback(loopyWork *work, loopyWorkId workId,
                               void *userData) {
    (void)work;
    (void)workId;
    loopyRandomRequest *req = userData;

    if (req->cancelled) {
        req->result = -1;
        req->savedErrno = ECANCELED;
        return;
    }

    ssize_t ret = getRandomBytes(req->buf, req->len);
    if (ret == (ssize_t)req->len) {
        req->result = 0;
    } else {
        req->result = -1;
        req->savedErrno = errno;
    }
}

static void randomAfterWorkCallback(loopyLoop *loop, loopyWork *work,
                                    loopyWorkId workId, loopyWorkStatus status,
                                    void *userData) {
    (void)work;
    (void)workId;
    loopyRandomRequest *req = userData;

    req->completed = true;

    if (status == LOOPY_WORK_CANCELLED) {
        req->result = -1;
        req->savedErrno = ECANCELED;
    }

    if (req->cb) {
        int savedErrno = errno;
        errno = req->savedErrno;
        req->cb(loop, req, req->result, req->userData);
        errno = savedErrno;
    }
}

/* ====================================================================
 * Asynchronous API
 * ==================================================================== */

loopyRandomRequest *loopyRandom(loopyLoop *loop, void *buf, size_t len,
                                loopyRandomCallback *cb, void *userData) {
    if (!loop || !buf || len == 0) {
        errno = EINVAL;
        return NULL;
    }

    loopyRandomRequest *req = zcalloc(1, sizeof(*req));
    if (!req) {
        return NULL;
    }

    req->loop = loop;
    req->cb = cb;
    req->userData = userData;
    req->buf = buf;
    req->len = len;

    /* If no callback, run synchronously */
    if (!cb) {
        req->result = loopyRandomSync(buf, len);
        req->savedErrno = errno;
        req->completed = true;
        return req;
    }

    /* Get or create work queue */
    req->work = loopyWorkNew(loop, NULL);
    if (!req->work) {
        zfree(req);
        return NULL;
    }

    /* Queue the work */
    req->workId = loopyWorkQueue(req->work, randomWorkCallback,
                                 randomAfterWorkCallback, req);
    if (req->workId == 0) {
        zfree(req);
        return NULL;
    }

    return req;
}

bool loopyRandomCancel(loopyRandomRequest *req) {
    if (!req || req->completed) {
        return false;
    }

    req->cancelled = true;

    if (req->work && req->workId) {
        return loopyWorkCancel(req->work, req->workId);
    }

    return false;
}

void loopyRandomFree(loopyRandomRequest *req) {
    if (req) {
        zfree(req);
    }
}

/* ====================================================================
 * Request Accessors
 * ==================================================================== */

int loopyRandomGetResult(const loopyRandomRequest *req) {
    return req ? req->result : -1;
}

size_t loopyRandomGetLength(const loopyRandomRequest *req) {
    return req ? req->len : 0;
}

void *loopyRandomGetBuffer(const loopyRandomRequest *req) {
    return req ? req->buf : NULL;
}
