/* loopyFSPoll - Stat-based file change detection for loopy event loop
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
#include "loopyFSPoll.h"
#include "loopyInternal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Custom fspStrdup using zcalloc to avoid deprecation warnings */
static inline char *fspStrdup(const char *s) {
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

/* ====================================================================
 * Internal structures
 * ==================================================================== */

typedef struct loopyFSPollEntry {
    loopyFSPollId id;
    char *path;
    loopyFSPollEvent events;
    loopyFSPollCallback *cb;
    void *userData;
    struct stat prevStat;
    bool exists;
    bool followSymlinks;
    struct loopyFSPollEntry *next;
} loopyFSPollEntry;

struct loopyFSPoll {
    loopyLoop *loop;
    void *userData; /* User data for handle accessors */
    loopyFSPollEntry *entries;
    uint64_t nextId;
    uint64_t intervalMs;
    uint64_t timerId;
    bool followSymlinks;
};

/* ====================================================================
 * Helper functions
 * ==================================================================== */

static loopyFSPollEntry *findEntry(const loopyFSPoll *fsp, loopyFSPollId id) {
    loopyFSPollEntry *e = fsp->entries;
    while (e) {
        if (e->id == id) {
            return e;
        }
        e = e->next;
    }
    return NULL;
}

static int doStat(const char *path, struct stat *st, bool followSymlinks) {
    if (followSymlinks) {
        return stat(path, st);
    } else {
        return lstat(path, st);
    }
}

static loopyFSPollEvent detectChanges(const loopyFSPollEntry *entry,
                                      const struct stat *newStat,
                                      bool newExists) {
    loopyFSPollEvent events = LOOPY_FSPOLL_NONE;

    /* File creation/deletion */
    if (!entry->exists && newExists) {
        events |= LOOPY_FSPOLL_CREATED;
    } else if (entry->exists && !newExists) {
        events |= LOOPY_FSPOLL_DELETED;
        return events; /* No need to check other changes if deleted */
    }

    if (!entry->exists || !newExists) {
        return events;
    }

    const struct stat *oldStat = &entry->prevStat;

    /* Modification time */
    if (oldStat->st_mtime != newStat->st_mtime) {
        events |= LOOPY_FSPOLL_MODIFIED;
    }

    /* Size */
    if (oldStat->st_size != newStat->st_size) {
        events |= LOOPY_FSPOLL_SIZE;
    }

    /* Permissions */
    if ((oldStat->st_mode & 07777) != (newStat->st_mode & 07777)) {
        events |= LOOPY_FSPOLL_PERMISSIONS;
    }

    /* Owner/group */
    if (oldStat->st_uid != newStat->st_uid ||
        oldStat->st_gid != newStat->st_gid) {
        events |= LOOPY_FSPOLL_OWNER;
    }

    /* File type (rare but possible) */
    if ((oldStat->st_mode & S_IFMT) != (newStat->st_mode & S_IFMT)) {
        events |= LOOPY_FSPOLL_TYPE;
    }

    return events;
}

static bool pollTimerCallback(timerWheel *t, timerWheelId timerId,
                              void *clientData) {
    (void)t;
    (void)timerId;

    loopyFSPoll *fsp = clientData;

    loopyFSPollEntry *e = fsp->entries;
    while (e) {
        struct stat newStat;
        bool newExists = (doStat(e->path, &newStat, e->followSymlinks) == 0);

        loopyFSPollEvent detected = detectChanges(e, &newStat, newExists);
        loopyFSPollEvent relevant = detected & e->events;

        if (relevant != LOOPY_FSPOLL_NONE && e->cb) {
            loopyFSPollInfo info = {.pollId = e->id,
                                    .events = relevant,
                                    .path = e->path,
                                    .prevStat = e->exists ? &e->prevStat : NULL,
                                    .currStat = newExists ? &newStat : NULL,
                                    .userData = e->userData};
            e->cb(fsp, &info);
        }

        /* Update state */
        if (newExists) {
            e->prevStat = newStat;
        }
        e->exists = newExists;

        e = e->next;
    }
    return true; /* Continue timer */
}

/* ====================================================================
 * Configuration
 * ==================================================================== */

void loopyFSPollConfigInit(loopyFSPollConfig *config) {
    if (config) {
        *config = LOOPY_FSPOLL_CONFIG_DEFAULT;
    }
}

/* ====================================================================
 * Lifecycle
 * ==================================================================== */

loopyFSPoll *loopyFSPollNew(loopyLoop *loop, const loopyFSPollConfig *config) {
    if (!loop) {
        return NULL;
    }

    loopyFSPoll *fsp = zcalloc(1, sizeof(*fsp));
    if (!fsp) {
        return NULL;
    }

    fsp->loop = loop;
    fsp->nextId = 1;

    if (config) {
        fsp->intervalMs = config->intervalMs;
        fsp->followSymlinks = config->followSymlinks;
    } else {
        fsp->intervalMs = 1000;
        fsp->followSymlinks = true;
    }

    /* Minimum interval */
    if (fsp->intervalMs < 10) {
        fsp->intervalMs = 10;
    }

    return fsp;
}

void loopyFSPollFree(loopyFSPoll *fsp) {
    if (!fsp) {
        return;
    }

    /* Stop timer */
    if (fsp->timerId) {
        loopyUnregisterTimer(fsp->loop, fsp->timerId);
    }

    /* Free all entries */
    loopyFSPollEntry *e = fsp->entries;
    while (e) {
        loopyFSPollEntry *next = e->next;
        zfree(e->path);
        zfree(e);
        e = next;
    }

    zfree(fsp);
}

loopyLoop *loopyFSPollGetLoop(const loopyFSPoll *fsp) {
    return fsp ? fsp->loop : NULL;
}

/* ====================================================================
 * File Monitoring
 * ==================================================================== */

loopyFSPollId loopyFSPollStart(loopyFSPoll *fsp, const char *path,
                               loopyFSPollEvent events, loopyFSPollCallback *cb,
                               void *userData) {
    if (!fsp || !path || !cb || events == LOOPY_FSPOLL_NONE) {
        return 0;
    }

    loopyFSPollEntry *e = zcalloc(1, sizeof(*e));
    if (!e) {
        return 0;
    }

    e->path = fspStrdup(path);
    if (!e->path) {
        zfree(e);
        return 0;
    }

    e->id = fsp->nextId++;
    e->events = events;
    e->cb = cb;
    e->userData = userData;
    e->followSymlinks = fsp->followSymlinks;

    /* Get initial stat */
    e->exists = (doStat(path, &e->prevStat, e->followSymlinks) == 0);

    /* Add to list */
    e->next = fsp->entries;
    fsp->entries = e;

    /* Start timer if this is the first entry */
    if (!fsp->timerId) {
        fsp->timerId = loopyRegisterTimer(
            fsp->loop, fsp->intervalMs * 1000, /* microseconds */
            fsp->intervalMs * 1000, pollTimerCallback, fsp);
        if (!fsp->timerId) {
            /* Remove entry on timer failure */
            fsp->entries = e->next;
            zfree(e->path);
            zfree(e);
            return 0;
        }
    }

    return e->id;
}

bool loopyFSPollStop(loopyFSPoll *fsp, loopyFSPollId pollId) {
    if (!fsp || pollId == 0) {
        return false;
    }

    loopyFSPollEntry **pp = &fsp->entries;
    while (*pp) {
        if ((*pp)->id == pollId) {
            loopyFSPollEntry *e = *pp;
            *pp = e->next;
            zfree(e->path);
            zfree(e);

            /* Stop timer if no more entries */
            if (!fsp->entries && fsp->timerId) {
                loopyUnregisterTimer(fsp->loop, fsp->timerId);
                fsp->timerId = 0;
            }

            return true;
        }
        pp = &(*pp)->next;
    }

    return false;
}

void loopyFSPollStopAll(loopyFSPoll *fsp) {
    if (!fsp) {
        return;
    }

    /* Stop timer first */
    if (fsp->timerId) {
        loopyUnregisterTimer(fsp->loop, fsp->timerId);
        fsp->timerId = 0;
    }

    /* Free all entries */
    loopyFSPollEntry *e = fsp->entries;
    while (e) {
        loopyFSPollEntry *next = e->next;
        zfree(e->path);
        zfree(e);
        e = next;
    }
    fsp->entries = NULL;
}

void loopyFSPollSetInterval(loopyFSPoll *fsp, uint64_t intervalMs) {
    if (!fsp) {
        return;
    }

    /* Minimum interval */
    if (intervalMs < 10) {
        intervalMs = 10;
    }

    fsp->intervalMs = intervalMs;

    /* Restart timer if running */
    if (fsp->timerId) {
        loopyUnregisterTimer(fsp->loop, fsp->timerId);
        fsp->timerId =
            loopyRegisterTimer(fsp->loop, fsp->intervalMs * 1000,
                               fsp->intervalMs * 1000, pollTimerCallback, fsp);
    }
}

uint64_t loopyFSPollGetInterval(const loopyFSPoll *fsp) {
    return fsp ? fsp->intervalMs : 0;
}

/* ====================================================================
 * Status
 * ==================================================================== */

size_t loopyFSPollCount(const loopyFSPoll *fsp) {
    if (!fsp) {
        return 0;
    }

    size_t count = 0;
    loopyFSPollEntry *e = fsp->entries;
    while (e) {
        count++;
        e = e->next;
    }
    return count;
}

const char *loopyFSPollEventName(loopyFSPollEvent event) {
    switch (event) {
    case LOOPY_FSPOLL_NONE:
        return "none";
    case LOOPY_FSPOLL_MODIFIED:
        return "modified";
    case LOOPY_FSPOLL_SIZE:
        return "size";
    case LOOPY_FSPOLL_DELETED:
        return "deleted";
    case LOOPY_FSPOLL_CREATED:
        return "created";
    case LOOPY_FSPOLL_PERMISSIONS:
        return "permissions";
    case LOOPY_FSPOLL_OWNER:
        return "owner";
    case LOOPY_FSPOLL_TYPE:
        return "type";
    case LOOPY_FSPOLL_ALL:
        return "all";
    default:
        return "unknown";
    }
}

bool loopyFSPollGetStat(const loopyFSPoll *fsp, loopyFSPollId pollId,
                        struct stat *st) {
    if (!fsp || !st || pollId == 0) {
        return false;
    }

    const loopyFSPollEntry *e = findEntry(fsp, pollId);
    if (!e || !e->exists) {
        return false;
    }

    *st = e->prevStat;
    return true;
}

const char *loopyFSPollGetPath(const loopyFSPoll *fsp, loopyFSPollId pollId) {
    if (!fsp || pollId == 0) {
        return NULL;
    }

    const loopyFSPollEntry *e = findEntry(fsp, pollId);
    return e ? e->path : NULL;
}

void *loopyFSPollGetData(const loopyFSPoll *fsp) {
    return fsp ? fsp->userData : NULL;
}

void loopyFSPollSetData(loopyFSPoll *fsp, void *data) {
    if (fsp) {
        fsp->userData = data;
    }
}
