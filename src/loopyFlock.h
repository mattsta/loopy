/* loopyFlock - File Locking API
 *
 * Cross-platform file locking supporting both flock() (BSD-style) and
 * fcntl() (POSIX-style) locks for coordinating access to shared files.
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

#pragma once

#include "loopyPlatform.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h> /* For off_t, pid_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ====================================================================
 * Lock Types and Modes
 * ==================================================================== */

/**
 * File lock type (compatible with both flock and fcntl).
 *
 * SHARED:    Multiple processes can hold shared locks simultaneously.
 *            Used for read access. Conflicts with exclusive locks.
 *
 * EXCLUSIVE: Only one process can hold an exclusive lock.
 *            Used for write access. Conflicts with all other locks.
 */
typedef enum loopyFlockType {
    LOOPY_FLOCK_SHARED = 1,    /* Shared/read lock (F_RDLCK, LOCK_SH) */
    LOOPY_FLOCK_EXCLUSIVE = 2, /* Exclusive/write lock (F_WRLCK, LOCK_EX) */
} loopyFlockType;

/**
 * Lock operation mode.
 *
 * BLOCKING:    Wait until lock is acquired.
 * NONBLOCKING: Return immediately if lock cannot be acquired.
 */
typedef enum loopyFlockMode {
    LOOPY_FLOCK_BLOCKING = 0,    /* Block until lock acquired */
    LOOPY_FLOCK_NONBLOCKING = 1, /* Fail immediately if unavailable */
} loopyFlockMode;

/**
 * Lock mechanism (implementation strategy).
 *
 * AUTO:  Choose best mechanism for platform (default).
 * FLOCK: Use flock() - whole-file advisory locks (simple, BSD-style).
 * FCNTL: Use fcntl() - byte-range POSIX locks (more features).
 */
typedef enum loopyFlockMechanism {
    LOOPY_FLOCK_AUTO =
        0, /* Platform default (flock on BSD/Linux, fcntl elsewhere) */
    LOOPY_FLOCK_FLOCK = 1, /* Use flock() - whole file only */
    LOOPY_FLOCK_FCNTL = 2, /* Use fcntl() - supports byte ranges */
} loopyFlockMechanism;

/* ====================================================================
 * Capability Detection
 * ==================================================================== */

/**
 * Check if flock() is available on this platform.
 *
 * Returns true on Linux, macOS, BSDs. May be false on some Unix systems.
 */
bool loopyFlockHasFlock(void);

/**
 * Check if fcntl() POSIX locks are available.
 *
 * Returns true on all POSIX-compliant systems.
 */
bool loopyFlockHasFcntl(void);

/* ====================================================================
 * BSD-Style Whole-File Locks (flock)
 * ==================================================================== */

/**
 * Acquire a whole-file lock using flock().
 *
 * Simple advisory locks that lock the entire file. All locks held by
 * a process are released when ANY file descriptor to that file is closed.
 *
 * Features:
 *  - Whole file only (no byte ranges)
 *  - Advisory (processes must cooperate)
 *  - Released when ANY fd to the file closes
 *  - Simple and fast
 *
 * Args:
 *   fd:   Open file descriptor
 *   type: SHARED or EXCLUSIVE
 *   mode: BLOCKING or NONBLOCKING
 *
 * Returns:
 *   true on success, false on failure (check errno)
 *
 * Example:
 *   int fd = open("data.txt", O_RDWR);
 *   loopyFlockLock(fd, LOOPY_FLOCK_EXCLUSIVE, LOOPY_FLOCK_BLOCKING);
 *   // ... exclusive access ...
 *   loopyFlockUnlock(fd);
 *
 * @note In NONBLOCKING mode, returns false with errno set to EAGAIN
 *       or EWOULDBLOCK if the lock cannot be acquired immediately.
 *       Use loopyFlockGetError() for a readable error message.
 */
bool loopyFlockLock(int fd, loopyFlockType type, loopyFlockMode mode);

/**
 * Release a flock() whole-file lock.
 *
 * Args:
 *   fd: File descriptor previously locked with loopyFlockLock()
 *
 * Returns:
 *   true on success, false on failure
 *
 * @note Lock is also automatically released when ANY file descriptor to
 *       the file is closed by this process. If the application holds
 *       multiple fds to the same file, closing one will release all locks.
 *       This is a flock() semantic - callers must be aware of it.
 */
bool loopyFlockUnlock(int fd);

/**
 * Try to upgrade a shared lock to exclusive (flock only).
 *
 * Atomically releases shared lock and acquires exclusive lock.
 *
 * Args:
 *   fd:   File descriptor with existing shared lock
 *   mode: BLOCKING or NONBLOCKING
 *
 * Returns:
 *   true on success, false on failure
 *
 * @note DEADLOCK RISK: In BLOCKING mode, if multiple processes attempt
 *       simultaneous upgrade (shared -> exclusive), they may deadlock
 *       waiting for each other. On some systems, the kernel detects this
 *       and returns EDEADLK. Prefer NONBLOCKING mode or use FCNTL locks
 *       which have more predictable behavior.
 */
bool loopyFlockUpgrade(int fd, loopyFlockMode mode);

/**
 * Downgrade an exclusive lock to shared (flock only).
 *
 * Atomically releases exclusive lock and acquires shared lock.
 *
 * Args:
 *   fd: File descriptor with existing exclusive lock
 *
 * Returns:
 *   true on success, false on failure
 *
 * @note This is always BLOCKING. After downgrade, other processes
 *       can acquire shared locks on the same file. Always succeeds
 *       if the exclusive lock is held. No deadlock risk.
 */
bool loopyFlockDowngrade(int fd);

/* ====================================================================
 * POSIX Byte-Range Locks (fcntl)
 * ==================================================================== */

/**
 * Acquire a byte-range POSIX lock using fcntl().
 *
 * Advanced advisory locks with byte-range granularity. Locks are
 * associated with processes (not file descriptors).
 *
 * Features:
 *  - Byte-range locking (lock portions of file)
 *  - Process-associated (closing one fd doesn't release)
 *  - Supports record locking
 *  - Deadlock detection on some systems
 *
 * Args:
 *   fd:     Open file descriptor
 *   type:   SHARED or EXCLUSIVE
 *   mode:   BLOCKING or NONBLOCKING
 *   offset: Byte offset to start lock (0 = beginning)
 *   length: Number of bytes to lock (0 = to EOF, grows with file)
 *
 * Returns:
 *   true on success, false on failure
 *
 * Example (lock first 1KB):
 *   int fd = open("data.bin", O_RDWR);
 *   loopyFlockLockRange(fd, LOOPY_FLOCK_EXCLUSIVE,
 *                       LOOPY_FLOCK_BLOCKING, 0, 1024);
 *
 * Example (lock entire file including future growth):
 *   loopyFlockLockRange(fd, LOOPY_FLOCK_EXCLUSIVE,
 *                       LOOPY_FLOCK_BLOCKING, 0, 0);
 *
 * @note Locks are PROCESS-ASSOCIATED, not file-descriptor associated.
 *       If the same process opens multiple fds to the same file, locks
 *       set on one fd apply to all. Closing one fd does NOT release locks.
 *       length=0 means "to EOF", including any future growth of the file.
 */
bool loopyFlockLockRange(int fd, loopyFlockType type, loopyFlockMode mode,
                         off_t offset, off_t length);

/**
 * Release a byte-range POSIX lock.
 *
 * Args:
 *   fd:     File descriptor
 *   offset: Start of range to unlock
 *   length: Length of range (0 = to EOF)
 *
 * Returns:
 *   true on success, false on failure
 *
 * @note Can unlock a SUBSET of a locked range, which splits the lock.
 *       Example: lock bytes 0-1000, then unlock 250-750 results in two
 *       separate locks: 0-249 and 751-1000. This is a powerful but subtle
 *       feature that can easily cause confusion. Use carefully.
 */
bool loopyFlockUnlockRange(int fd, off_t offset, off_t length);

/**
 * Test if a byte range can be locked (without actually locking).
 *
 * Checks whether a lock could be placed on the specified range.
 * If the range is locked by another process, returns information
 * about the conflicting lock.
 *
 * Args:
 *   fd:      File descriptor
 *   type:    Lock type to test
 *   offset:  Start of range
 *   length:  Length of range (0 = to EOF)
 *   holder:  Output - PID of process holding conflicting lock (or -1)
 *
 * Returns:
 *   true if range can be locked, false if blocked
 *
 * Example:
 *   pid_t holder;
 *   if (!loopyFlockTestRange(fd, LOOPY_FLOCK_EXCLUSIVE, 0, 0, &holder)) {
 *       printf("Range locked by process %d\n", holder);
 *   }
 *
 * @note Returns the PID of ONE blocking process if multiple are involved.
 *       The specific PID returned is system-dependent. Testing does NOT
 *       acquire the lock, so the result can change immediately. This is
 *       a non-blocking test - it never waits.
 */
bool loopyFlockTestRange(int fd, loopyFlockType type, off_t offset,
                         off_t length, pid_t *holder);

/* ====================================================================
 * Unified High-Level API
 * ==================================================================== */

/**
 * Opaque lock handle for high-level API.
 *
 * Tracks lock state and supports both flock and fcntl mechanisms.
 */
typedef struct loopyFlock loopyFlock;

/**
 * Lock configuration.
 */
typedef struct loopyFlockConfig {
    loopyFlockMechanism mechanism; /* AUTO, FLOCK, or FCNTL */
    loopyFlockType type;           /* SHARED or EXCLUSIVE */
    loopyFlockMode mode;           /* BLOCKING or NONBLOCKING */
    off_t offset;                  /* Byte offset (fcntl only, 0 = start) */
    off_t length;                  /* Byte length (fcntl only, 0 = EOF) */
} loopyFlockConfig;

/**
 * Default configuration (AUTO mechanism, BLOCKING mode).
 */
#define LOOPY_FLOCK_CONFIG_DEFAULT                                             \
    {                                                                          \
        .mechanism = LOOPY_FLOCK_AUTO,                                         \
        .type = LOOPY_FLOCK_EXCLUSIVE,                                         \
        .mode = LOOPY_FLOCK_BLOCKING,                                          \
        .offset = 0,                                                           \
        .length = 0,                                                           \
    }

/**
 * Initialize a flock config with default values.
 *
 * @param config Config struct to initialize (must not be NULL)
 */
void loopyFlockConfigInit(loopyFlockConfig *config);

/**
 * Create and acquire a file lock.
 *
 * High-level interface that manages lock lifecycle and mechanism selection.
 *
 * Args:
 *   fd:     Open file descriptor
 *   config: Lock configuration (or NULL for defaults)
 *
 * Returns:
 *   Lock handle on success, NULL on failure
 *
 * Example (exclusive whole-file lock):
 *   int fd = open("data.txt", O_RDWR);
 *   loopyFlockConfig cfg = LOOPY_FLOCK_CONFIG_DEFAULT;
 *   loopyFlock *lock = loopyFlockNew(fd, &cfg);
 *   if (lock) {
 *       // ... critical section ...
 *       loopyFlockFree(lock);
 *   }
 *
 * Example (shared lock on first 4KB):
 *   loopyFlockConfig cfg = {
 *       .mechanism = LOOPY_FLOCK_FCNTL,
 *       .type = LOOPY_FLOCK_SHARED,
 *       .mode = LOOPY_FLOCK_NONBLOCKING,
 *       .offset = 0,
 *       .length = 4096
 *   };
 *   loopyFlock *lock = loopyFlockNew(fd, &cfg);
 */
loopyFlock *loopyFlockNew(int fd, const loopyFlockConfig *config);

/**
 * Release and free a file lock.
 *
 * Automatically unlocks and deallocates the lock handle.
 *
 * Args:
 *   lock: Lock handle from loopyFlockNew()
 */
void loopyFlockFree(loopyFlock *lock);

/**
 * Get the file descriptor associated with a lock.
 *
 * Returns: File descriptor, or -1 if invalid
 */
int loopyFlockGetFd(const loopyFlock *lock);

/**
 * Get the lock type.
 *
 * Returns: SHARED or EXCLUSIVE
 */
loopyFlockType loopyFlockGetType(const loopyFlock *lock);

/**
 * Get the lock mechanism being used.
 *
 * Returns: FLOCK or FCNTL (never AUTO)
 */
loopyFlockMechanism loopyFlockGetMechanism(const loopyFlock *lock);

/**
 * Get the locked byte range (fcntl locks only).
 *
 * Args:
 *   lock:   Lock handle
 *   offset: Output - start offset
 *   length: Output - length (0 = to EOF)
 *
 * Returns:
 *   true if lock uses fcntl with byte range, false for flock
 */
bool loopyFlockGetRange(const loopyFlock *lock, off_t *offset, off_t *length);

/**
 * Check if a lock is still valid.
 *
 * Returns:
 *   true if lock is active, false if released or invalid
 */
bool loopyFlockIsLocked(const loopyFlock *lock);

/* ====================================================================
 * Lock File Pattern
 * ==================================================================== */

/**
 * Create and lock a lock file (common Unix pattern).
 *
 * Creates a lock file at the specified path and acquires an exclusive
 * lock. Typically used for single-instance enforcement.
 *
 * Args:
 *   path: Path to lock file (e.g., "/var/run/myapp.lock")
 *   mode: BLOCKING or NONBLOCKING
 *
 * Returns:
 *   Lock handle on success, NULL if already locked or on error
 *
 * Example (single-instance enforcement):
 *   loopyFlock *lock = loopyFlockNewLockfile("/var/run/myapp.lock",
 *                                            LOOPY_FLOCK_NONBLOCKING);
 *   if (!lock) {
 *       fprintf(stderr, "Another instance is running\n");
 *       exit(1);
 *   }
 *   // ... application runs ...
 *   loopyFlockFreeLockfile(lock);  // Removes lock file
 *
 * Note: Lock file is automatically removed by loopyFlockFreeLockfile().
 */
loopyFlock *loopyFlockNewLockfile(const char *path, loopyFlockMode mode);

/**
 * Release and remove a lock file.
 *
 * Unlocks and deletes the lock file created by loopyFlockNewLockfile().
 *
 * Args:
 *   lock: Lock handle from loopyFlockNewLockfile()
 */
void loopyFlockFreeLockfile(loopyFlock *lock);

/* ====================================================================
 * Error Handling
 * ==================================================================== */

/**
 * Get a human-readable error message for the last lock operation.
 *
 * Returns:
 *   Error message string, or NULL if no error
 *
 * Note: Message is valid until next lock operation on same thread.
 */
const char *loopyFlockGetError(void);

#ifdef __cplusplus
}
#endif
