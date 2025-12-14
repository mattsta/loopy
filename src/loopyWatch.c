/* loopyWatch - File and directory watching for loopy event loop
 *
 * Implementation uses inotify on Linux, kqueue on BSD/macOS.
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
#include "loopyWatch.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Simple string duplication using zcalloc */
static inline char *watchStrdup(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t len = strlen(s) + 1;
    char *dup = zcalloc(1, len);
    if (dup) {
        memcpy(dup, s, len);
    }
    return dup;
}

/* Platform detection */
#if __linux__
#include <sys/inotify.h>
#define USE_INOTIFY 1
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) ||    \
    defined(__NetBSD__)
#include <sys/event.h>
#define USE_KQUEUE 1
#endif

/* Maximum watches */
#define MAX_WATCHES 256

/* ====================================================================
 * Internal data structures
 * ==================================================================== */

typedef struct loopyWatchEntry {
    loopyWatchId id;
    char *path;
    loopyWatchEvent events;
    loopyWatchCallback *callback;
    void *userData;
    bool active;
#if USE_INOTIFY
    int wd; /* inotify watch descriptor */
#elif USE_KQUEUE
    int fd; /* File descriptor for kqueue */
#endif
} loopyWatchEntry;

struct loopyWatch {
    loopyLoop *loop;
    void *userData; /* User data for handle accessors */
    loopyWatchEntry watches[MAX_WATCHES];
    size_t watchCount;
    loopyWatchId nextId;
#if USE_INOTIFY
    int inotifyFd;
#elif USE_KQUEUE
    int kqueueFd;
#endif
};

/* ====================================================================
 * Forward declarations
 * ==================================================================== */

static void watchEventCallback(loopyLoop *l, int fd, void *data,
                               loopyAction mask);
static loopyWatchEntry *watchFindById(loopyWatch *w, loopyWatchId id);
static loopyWatchEntry *watchFindFree(loopyWatch *w);

#if USE_INOTIFY
static loopyWatchEntry *watchFindByWd(loopyWatch *w, int wd);
static uint32_t eventsToInotify(loopyWatchEvent events);
static loopyWatchEvent inotifyToEvents(uint32_t mask);
#elif USE_KQUEUE
static uint32_t eventsToKqueue(loopyWatchEvent events);
static loopyWatchEvent kqueueToEvents(uint32_t fflags);
#endif

/* ====================================================================
 * Lifecycle
 * ==================================================================== */

loopyWatch *loopyWatchNew(loopyLoop *loop) {
    if (!loop) {
        return NULL;
    }

    loopyWatch *w = zcalloc(1, sizeof(*w));
    if (!w) {
        return NULL;
    }

    w->loop = loop;
    w->nextId = 1;

#if USE_INOTIFY
    w->inotifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (w->inotifyFd == -1) {
        zfree(w);
        return NULL;
    }

    if (!loopyRegisterRead(loop, w->inotifyFd, watchEventCallback, w)) {
        close(w->inotifyFd);
        zfree(w);
        return NULL;
    }
#elif USE_KQUEUE
    w->kqueueFd = kqueue();
    if (w->kqueueFd == -1) {
        zfree(w);
        return NULL;
    }

    /* Set close-on-exec */
    fcntl(w->kqueueFd, F_SETFD, FD_CLOEXEC);

    if (!loopyRegisterRead(loop, w->kqueueFd, watchEventCallback, w)) {
        close(w->kqueueFd);
        zfree(w);
        return NULL;
    }
#else
    /* No supported backend */
    zfree(w);
    return NULL;
#endif

    return w;
}

void loopyWatchFree(loopyWatch *w) {
    if (!w) {
        return;
    }

    /* Remove all watches */
    loopyWatchRemoveAll(w);

#if USE_INOTIFY
    loopyUnregisterReadWrite(w->loop, w->inotifyFd);
    close(w->inotifyFd);
#elif USE_KQUEUE
    loopyUnregisterReadWrite(w->loop, w->kqueueFd);
    close(w->kqueueFd);
#endif

    zfree(w);
}

/* ====================================================================
 * Watch Management
 * ==================================================================== */

loopyWatchId loopyWatchAdd(loopyWatch *w, const char *path,
                           loopyWatchEvent events, loopyWatchCallback *cb,
                           void *userData) {
    if (!w || !path || !cb) {
        return 0;
    }

    loopyWatchEntry *entry = watchFindFree(w);
    if (!entry) {
        return 0; /* No free slots */
    }

    entry->path = watchStrdup(path);
    if (!entry->path) {
        return 0;
    }

    entry->events = events;
    entry->callback = cb;
    entry->userData = userData;
    entry->id = w->nextId++;

#if USE_INOTIFY
    uint32_t inotifyMask = eventsToInotify(events);
    entry->wd = inotify_add_watch(w->inotifyFd, path, inotifyMask);
    if (entry->wd == -1) {
        zfree(entry->path);
        entry->path = NULL;
        return 0;
    }
#elif USE_KQUEUE
    /* Open file descriptor for kqueue monitoring */
    entry->fd = open(path, O_RDONLY | O_CLOEXEC);
    if (entry->fd == -1) {
        zfree(entry->path);
        entry->path = NULL;
        return 0;
    }

    /* Set up kqueue event */
    struct kevent change;
    uint32_t fflags = eventsToKqueue(events);
    EV_SET(&change, entry->fd, EVFILT_VNODE, EV_ADD | EV_CLEAR, fflags, 0,
           entry);

    if (kevent(w->kqueueFd, &change, 1, NULL, 0, NULL) == -1) {
        close(entry->fd);
        zfree(entry->path);
        entry->path = NULL;
        return 0;
    }
#endif

    entry->active = true;
    w->watchCount++;

    return entry->id;
}

bool loopyWatchRemove(loopyWatch *w, loopyWatchId watchId) {
    if (!w || watchId == 0) {
        return false;
    }

    loopyWatchEntry *entry = watchFindById(w, watchId);
    if (!entry || !entry->active) {
        return false;
    }

#if USE_INOTIFY
    inotify_rm_watch(w->inotifyFd, entry->wd);
#elif USE_KQUEUE
    struct kevent change;
    EV_SET(&change, entry->fd, EVFILT_VNODE, EV_DELETE, 0, 0, NULL);
    kevent(w->kqueueFd, &change, 1, NULL, 0, NULL);
    close(entry->fd);
#endif

    zfree(entry->path);
    memset(entry, 0, sizeof(*entry));
    w->watchCount--;

    return true;
}

void loopyWatchRemoveAll(loopyWatch *w) {
    if (!w) {
        return;
    }

    for (size_t i = 0; i < MAX_WATCHES; i++) {
        if (w->watches[i].active) {
            loopyWatchRemove(w, w->watches[i].id);
        }
    }
}

/* ====================================================================
 * Status
 * ==================================================================== */

size_t loopyWatchCount(const loopyWatch *w) {
    return w ? w->watchCount : 0;
}

const char *loopyWatchEventName(loopyWatchEvent event) {
    switch (event) {
    case LOOPY_WATCH_MODIFY:
        return "MODIFY";
    case LOOPY_WATCH_CREATE:
        return "CREATE";
    case LOOPY_WATCH_DELETE:
        return "DELETE";
    case LOOPY_WATCH_RENAME:
        return "RENAME";
    case LOOPY_WATCH_ATTRIB:
        return "ATTRIB";
    case LOOPY_WATCH_ALL:
        return "ALL";
    default:
        return "UNKNOWN";
    }
}

const char *loopyWatchBackendName(void) {
#if USE_INOTIFY
    return "inotify";
#elif USE_KQUEUE
    return "kqueue";
#else
    return "none";
#endif
}

/* ====================================================================
 * Internal: Helpers
 * ==================================================================== */

static loopyWatchEntry *watchFindById(loopyWatch *w, loopyWatchId id) {
    for (size_t i = 0; i < MAX_WATCHES; i++) {
        if (w->watches[i].active && w->watches[i].id == id) {
            return &w->watches[i];
        }
    }
    return NULL;
}

static loopyWatchEntry *watchFindFree(loopyWatch *w) {
    for (size_t i = 0; i < MAX_WATCHES; i++) {
        if (!w->watches[i].active) {
            return &w->watches[i];
        }
    }
    return NULL;
}

#if USE_INOTIFY
static loopyWatchEntry *watchFindByWd(loopyWatch *w, int wd) {
    for (size_t i = 0; i < MAX_WATCHES; i++) {
        if (w->watches[i].active && w->watches[i].wd == wd) {
            return &w->watches[i];
        }
    }
    return NULL;
}

static uint32_t eventsToInotify(loopyWatchEvent events) {
    uint32_t mask = 0;
    if (events & LOOPY_WATCH_MODIFY) {
        mask |= IN_MODIFY | IN_CLOSE_WRITE;
    }
    if (events & LOOPY_WATCH_CREATE) {
        mask |= IN_CREATE;
    }
    if (events & LOOPY_WATCH_DELETE) {
        mask |= IN_DELETE | IN_DELETE_SELF;
    }
    if (events & LOOPY_WATCH_RENAME) {
        mask |= IN_MOVE | IN_MOVED_FROM | IN_MOVED_TO;
    }
    if (events & LOOPY_WATCH_ATTRIB) {
        mask |= IN_ATTRIB;
    }
    return mask;
}

static loopyWatchEvent inotifyToEvents(uint32_t mask) {
    loopyWatchEvent events = 0;
    if (mask & (IN_MODIFY | IN_CLOSE_WRITE)) {
        events |= LOOPY_WATCH_MODIFY;
    }
    if (mask & IN_CREATE) {
        events |= LOOPY_WATCH_CREATE;
    }
    if (mask & (IN_DELETE | IN_DELETE_SELF)) {
        events |= LOOPY_WATCH_DELETE;
    }
    if (mask & (IN_MOVE | IN_MOVED_FROM | IN_MOVED_TO)) {
        events |= LOOPY_WATCH_RENAME;
    }
    if (mask & IN_ATTRIB) {
        events |= LOOPY_WATCH_ATTRIB;
    }
    return events;
}
#elif USE_KQUEUE
static uint32_t eventsToKqueue(loopyWatchEvent events) {
    uint32_t fflags = 0;
    if (events & LOOPY_WATCH_MODIFY) {
        fflags |= NOTE_WRITE | NOTE_EXTEND;
    }
    if (events & LOOPY_WATCH_DELETE) {
        fflags |= NOTE_DELETE;
    }
    if (events & LOOPY_WATCH_RENAME) {
        fflags |= NOTE_RENAME;
    }
    if (events & LOOPY_WATCH_ATTRIB) {
        fflags |= NOTE_ATTRIB;
    }
    /* NOTE: kqueue doesn't have CREATE for directories the same way inotify
     * does. We'd need to monitor the parent directory. For now, CREATE is not
     * supported on kqueue. */
    return fflags;
}

static loopyWatchEvent kqueueToEvents(uint32_t fflags) {
    loopyWatchEvent events = 0;
    if (fflags & (NOTE_WRITE | NOTE_EXTEND)) {
        events |= LOOPY_WATCH_MODIFY;
    }
    if (fflags & NOTE_DELETE) {
        events |= LOOPY_WATCH_DELETE;
    }
    if (fflags & NOTE_RENAME) {
        events |= LOOPY_WATCH_RENAME;
    }
    if (fflags & NOTE_ATTRIB) {
        events |= LOOPY_WATCH_ATTRIB;
    }
    return events;
}
#endif

/* ====================================================================
 * Internal: Event callback
 * ==================================================================== */

static void watchEventCallback(loopyLoop *l, int fd, void *data,
                               loopyAction mask) {
    (void)l;
    (void)mask;

    loopyWatch *w = data;

#if USE_INOTIFY
    /* Read inotify events */
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t len;

    while ((len = read(fd, buf, sizeof(buf))) > 0) {
        char *ptr = buf;
        while (ptr < buf + len) {
            struct inotify_event *event = (struct inotify_event *)ptr;

            loopyWatchEntry *entry = watchFindByWd(w, event->wd);
            if (entry && entry->callback) {
                loopyWatchInfo info = {
                    .watchId = entry->id,
                    .events = inotifyToEvents(event->mask),
                    .path = entry->path,
                    .filename = event->len > 0 ? event->name : NULL,
                    .userData = entry->userData,
                };
                entry->callback(w, &info);
            }

            ptr += sizeof(struct inotify_event) + event->len;
        }
    }
#elif USE_KQUEUE
    struct kevent events[16];
    struct timespec timeout = {0, 0}; /* Don't block */

    int n;
    while ((n = kevent(w->kqueueFd, NULL, 0, events, 16, &timeout)) > 0) {
        for (int i = 0; i < n; i++) {
            loopyWatchEntry *entry = events[i].udata;
            if (entry && entry->active && entry->callback) {
                loopyWatchInfo info = {
                    .watchId = entry->id,
                    .events = kqueueToEvents(events[i].fflags),
                    .path = entry->path,
                    .filename = NULL, /* kqueue doesn't provide this */
                    .userData = entry->userData,
                };
                entry->callback(w, &info);
            }
        }
    }
    (void)fd;
#else
    (void)fd;
    (void)w;
#endif
}

/* ====================================================================
 * Handle Accessors
 * ==================================================================== */

loopyLoop *loopyWatchGetLoop(const loopyWatch *w) {
    return w ? w->loop : NULL;
}

void *loopyWatchGetData(const loopyWatch *w) {
    return w ? w->userData : NULL;
}

void loopyWatchSetData(loopyWatch *w, void *data) {
    if (w) {
        w->userData = data;
    }
}
