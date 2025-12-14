/* loopyFlock - File Locking Implementation
 *
 * Cross-platform file locking supporting both flock() (BSD-style) and
 * fcntl() (POSIX-style) locks.
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

#include "loopyFlock.h"

#include "../deps/datakit/src/datakit.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

/* ====================================================================
 * Internal Structure
 * ==================================================================== */

/**
 * Lock handle (opaque to users).
 */
struct loopyFlock {
    int fd;                        /* File descriptor */
    loopyFlockMechanism mechanism; /* FLOCK or FCNTL */
    loopyFlockType type;           /* SHARED or EXCLUSIVE */
    off_t offset;                  /* Byte offset (fcntl only) */
    off_t length;                  /* Byte length (fcntl only) */
    bool valid;                    /* Is lock still held? */
    bool ownsFd;                   /* Should we close fd on free? */
    char *lockfilePath;            /* Path for lockfile (or NULL) */
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

static void setErrorErrno(const char *prefix) {
    snprintf(lastError, sizeof(lastError), "%s: %s", prefix, strerror(errno));
}

const char *loopyFlockGetError(void) {
    return lastError[0] ? lastError : NULL;
}

/* ====================================================================
 * Capability Detection
 * ==================================================================== */

bool loopyFlockHasFlock(void) {
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) ||        \
    defined(__OpenBSD__) || defined(__NetBSD__)
    return true;
#else
    return false; /* Platform may not support flock() */
#endif
}

bool loopyFlockHasFcntl(void) {
    return true; /* fcntl() is POSIX standard */
}

/* ====================================================================
 * BSD-Style flock() Implementation
 * ==================================================================== */

bool loopyFlockLock(int fd, loopyFlockType type, loopyFlockMode mode) {
    if (fd < 0) {
        setError("Invalid file descriptor");
        return false;
    }

    // cppcheck-suppress knownConditionTrueFalse
    if (!loopyFlockHasFlock()) {
        setError("flock() not supported on this platform");
        return false;
    }

    int operation = 0;

    /* Set lock type */
    switch (type) {
    case LOOPY_FLOCK_SHARED:
        operation = LOCK_SH;
        break;
    case LOOPY_FLOCK_EXCLUSIVE:
        operation = LOCK_EX;
        break;
    default:
        setError("Invalid lock type");
        return false;
    }

    /* Set blocking mode */
    if (mode == LOOPY_FLOCK_NONBLOCKING) {
        operation |= LOCK_NB;
    }

    /* Acquire lock */
    if (flock(fd, operation) != 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            setError("Lock would block (already held by another process)");
        } else {
            setErrorErrno("flock failed");
        }
        return false;
    }

    lastError[0] = '\0';
    return true;
}

bool loopyFlockUnlock(int fd) {
    if (fd < 0) {
        setError("Invalid file descriptor");
        return false;
    }

    // cppcheck-suppress knownConditionTrueFalse
    if (!loopyFlockHasFlock()) {
        setError("flock() not supported on this platform");
        return false;
    }

    if (flock(fd, LOCK_UN) != 0) {
        setErrorErrno("flock unlock failed");
        return false;
    }

    lastError[0] = '\0';
    return true;
}

bool loopyFlockUpgrade(int fd, loopyFlockMode mode) {
    /* Upgrade shared to exclusive */
    return loopyFlockLock(fd, LOOPY_FLOCK_EXCLUSIVE, mode);
}

bool loopyFlockDowngrade(int fd) {
    /* Downgrade exclusive to shared */
    return loopyFlockLock(fd, LOOPY_FLOCK_SHARED, LOOPY_FLOCK_BLOCKING);
}

/* ====================================================================
 * POSIX fcntl() Implementation
 * ==================================================================== */

static bool fcntlLockOp(int fd, loopyFlockType type, loopyFlockMode mode,
                        off_t offset, off_t length, int cmd) {
    if (fd < 0) {
        setError("Invalid file descriptor");
        return false;
    }

    struct flock fl = {0};

    /* Set lock type */
    switch (type) {
    case LOOPY_FLOCK_SHARED:
        fl.l_type = F_RDLCK;
        break;
    case LOOPY_FLOCK_EXCLUSIVE:
        fl.l_type = F_WRLCK;
        break;
    default:
        setError("Invalid lock type");
        return false;
    }

    fl.l_whence = SEEK_SET;
    fl.l_start = offset;
    fl.l_len = length;
    fl.l_pid = 0;

    /* Determine command (blocking vs nonblocking) */
    int fcntlCmd = (mode == LOOPY_FLOCK_NONBLOCKING) ? F_SETLK : F_SETLKW;
    if (cmd != -1) {
        fcntlCmd = cmd; /* Override for F_GETLK */
    }

    if (fcntl(fd, fcntlCmd, &fl) == -1) {
        if (errno == EACCES || errno == EAGAIN) {
            setError("Lock would block (already held by another process)");
        } else if (errno == EDEADLK) {
            setError("Deadlock detected");
        } else {
            setErrorErrno("fcntl lock failed");
        }
        return false;
    }

    lastError[0] = '\0';
    return true;
}

bool loopyFlockLockRange(int fd, loopyFlockType type, loopyFlockMode mode,
                         off_t offset, off_t length) {
    return fcntlLockOp(fd, type, mode, offset, length, -1);
}

bool loopyFlockUnlockRange(int fd, off_t offset, off_t length) {
    if (fd < 0) {
        setError("Invalid file descriptor");
        return false;
    }

    struct flock fl = {0};
    fl.l_type = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = offset;
    fl.l_len = length;

    if (fcntl(fd, F_SETLK, &fl) == -1) {
        setErrorErrno("fcntl unlock failed");
        return false;
    }

    lastError[0] = '\0';
    return true;
}

bool loopyFlockTestRange(int fd, loopyFlockType type, off_t offset,
                         off_t length, pid_t *holder) {
    if (fd < 0) {
        setError("Invalid file descriptor");
        return false;
    }

    struct flock fl = {0};

    /* Set lock type to test */
    switch (type) {
    case LOOPY_FLOCK_SHARED:
        fl.l_type = F_RDLCK;
        break;
    case LOOPY_FLOCK_EXCLUSIVE:
        fl.l_type = F_WRLCK;
        break;
    default:
        setError("Invalid lock type");
        return false;
    }

    fl.l_whence = SEEK_SET;
    fl.l_start = offset;
    fl.l_len = length;

    /* F_GETLK tests the lock without acquiring it */
    if (fcntl(fd, F_GETLK, &fl) == -1) {
        setErrorErrno("fcntl test failed");
        return false;
    }

    /* F_GETLK modifies fl.l_type: F_UNLCK means lock can be placed */
    if (fl.l_type == F_UNLCK) {
        if (holder) {
            *holder = -1;
        }
        lastError[0] = '\0';
        return true; /* Can lock */
    } else {
        /* Lock is held by another process */
        if (holder) {
            *holder = fl.l_pid;
        }
        return false; /* Cannot lock */
    }
}

/* ====================================================================
 * Configuration
 * ==================================================================== */

void loopyFlockConfigInit(loopyFlockConfig *config) {
    if (config) {
        loopyFlockConfig defaults = LOOPY_FLOCK_CONFIG_DEFAULT;
        *config = defaults;
    }
}

/* ====================================================================
 * Unified High-Level API
 * ==================================================================== */

loopyFlock *loopyFlockNew(int fd, const loopyFlockConfig *config) {
    if (fd < 0) {
        setError("Invalid file descriptor");
        return NULL;
    }

    /* Use default config if not provided */
    loopyFlockConfig defaultCfg = LOOPY_FLOCK_CONFIG_DEFAULT;
    if (!config) {
        config = &defaultCfg;
    }

    /* Allocate handle */
    loopyFlock *lock = zcalloc(1, sizeof(loopyFlock));
    if (!lock) {
        setError("Memory allocation failed");
        return NULL;
    }

    lock->fd = fd;
    lock->type = config->type;
    lock->offset = config->offset;
    lock->length = config->length;
    lock->valid = false;
    lock->ownsFd = false;
    lock->lockfilePath = NULL;

    /* Select mechanism */
    loopyFlockMechanism mechanism = config->mechanism;
    if (mechanism == LOOPY_FLOCK_AUTO) {
        /* Choose platform default */
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
        mechanism = LOOPY_FLOCK_FLOCK;
#else
        mechanism = LOOPY_FLOCK_FCNTL;
#endif
    }

    lock->mechanism = mechanism;

    /* Acquire lock using selected mechanism */
    bool success = false;
    if (mechanism == LOOPY_FLOCK_FLOCK) {
        // cppcheck-suppress knownConditionTrueFalse
        if (!loopyFlockHasFlock()) {
            setError("flock() not available, try FCNTL mechanism");
            zfree(lock);
            return NULL;
        }
        success = loopyFlockLock(fd, config->type, config->mode);
    } else if (mechanism == LOOPY_FLOCK_FCNTL) {
        success = loopyFlockLockRange(fd, config->type, config->mode,
                                      config->offset, config->length);
    } else {
        setError("Invalid mechanism");
        zfree(lock);
        return NULL;
    }

    if (!success) {
        zfree(lock);
        return NULL;
    }

    lock->valid = true;
    lastError[0] = '\0';
    return lock;
}

void loopyFlockFree(loopyFlock *lock) {
    if (!lock) {
        return;
    }

    if (lock->valid && lock->fd >= 0) {
        /* Unlock */
        if (lock->mechanism == LOOPY_FLOCK_FLOCK) {
            loopyFlockUnlock(lock->fd);
        } else if (lock->mechanism == LOOPY_FLOCK_FCNTL) {
            loopyFlockUnlockRange(lock->fd, lock->offset, lock->length);
        }
        lock->valid = false;
    }

    /* Don't close fd unless we own it (lockfile case) */
    if (lock->ownsFd && lock->fd >= 0) {
        close(lock->fd);
    }

    zfree(lock);
}

int loopyFlockGetFd(const loopyFlock *lock) {
    if (!lock) {
        return -1;
    }
    return lock->fd;
}

loopyFlockType loopyFlockGetType(const loopyFlock *lock) {
    if (!lock) {
        return LOOPY_FLOCK_EXCLUSIVE;
    }
    return lock->type;
}

loopyFlockMechanism loopyFlockGetMechanism(const loopyFlock *lock) {
    if (!lock) {
        return LOOPY_FLOCK_AUTO;
    }
    return lock->mechanism;
}

bool loopyFlockGetRange(const loopyFlock *lock, off_t *offset, off_t *length) {
    if (!lock || lock->mechanism != LOOPY_FLOCK_FCNTL) {
        return false;
    }

    if (offset) {
        *offset = lock->offset;
    }
    if (length) {
        *length = lock->length;
    }

    return true;
}

bool loopyFlockIsLocked(const loopyFlock *lock) {
    return lock != NULL && lock->valid;
}

/* ====================================================================
 * Lock File Pattern
 * ==================================================================== */

loopyFlock *loopyFlockNewLockfile(const char *path, loopyFlockMode mode) {
    if (!path) {
        setError("NULL path");
        return NULL;
    }

    /* Create/open lock file */
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        setErrorErrno("Failed to create lock file");
        return NULL;
    }

    /* Try to acquire exclusive lock */
    loopyFlockConfig config = LOOPY_FLOCK_CONFIG_DEFAULT;
    config.type = LOOPY_FLOCK_EXCLUSIVE;
    config.mode = mode;

    loopyFlock *lock = loopyFlockNew(fd, &config);
    if (!lock) {
        close(fd);
        return NULL;
    }

    /* Mark that we own the fd and track the path */
    lock->ownsFd = true;
    size_t pathLen = strlen(path) + 1;
    lock->lockfilePath = zcalloc(1, pathLen);
    if (lock->lockfilePath) {
        memcpy(lock->lockfilePath, path, pathLen);
    }

    return lock;
}

void loopyFlockFreeLockfile(loopyFlock *lock) {
    if (!lock) {
        return;
    }

    /* Remove lock file if we created it */
    if (lock->lockfilePath) {
        unlink(lock->lockfilePath);
        zfree(lock->lockfilePath);
        lock->lockfilePath = NULL;
    }

    /* Free the lock (also closes fd) */
    loopyFlockFree(lock);
}
