/* loopyFSPoll - Stat-based file change detection for loopy event loop
 *
 * Provides portable file change detection using periodic stat() calls.
 * This is useful as a fallback for network filesystems where kernel-based
 * notification (inotify, kqueue) may not work reliably.
 *
 * Key differences from loopyWatch:
 * - Uses polling instead of kernel events
 * - Works reliably on network filesystems (NFS, SMB, etc.)
 * - Lower performance but higher reliability
 * - Configurable poll interval
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

#include "loopy.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>

/* ====================================================================
 * Types
 * ==================================================================== */

/**
 * Opaque FS poll handle.
 */
typedef struct loopyFSPoll loopyFSPoll;

/**
 * Unique identifier for a polled file.
 */
typedef uint64_t loopyFSPollId;

/**
 * Change events detected (can be combined).
 */
typedef enum loopyFSPollEvent {
    LOOPY_FSPOLL_NONE = 0x00,        /* No change */
    LOOPY_FSPOLL_MODIFIED = 0x01,    /* File modified (mtime changed) */
    LOOPY_FSPOLL_SIZE = 0x02,        /* File size changed */
    LOOPY_FSPOLL_DELETED = 0x04,     /* File was deleted */
    LOOPY_FSPOLL_CREATED = 0x08,     /* File was created (was missing) */
    LOOPY_FSPOLL_PERMISSIONS = 0x10, /* Permissions changed */
    LOOPY_FSPOLL_OWNER = 0x20,       /* Owner/group changed */
    LOOPY_FSPOLL_TYPE = 0x40,        /* File type changed (rare) */
    LOOPY_FSPOLL_ALL = 0x7F          /* All events */
} loopyFSPollEvent;

/**
 * Information about a detected change.
 */
typedef struct loopyFSPollInfo {
    loopyFSPollId pollId;        /* Poll ID */
    loopyFSPollEvent events;     /* Which changes were detected */
    const char *path;            /* Path being monitored */
    const struct stat *prevStat; /* Previous stat (NULL if was missing) */
    const struct stat *currStat; /* Current stat (NULL if now missing) */
    void *userData;              /* User data */
} loopyFSPollInfo;

/**
 * Callback invoked when file changes are detected.
 */
typedef void loopyFSPollCallback(loopyFSPoll *fsp, const loopyFSPollInfo *info);

/**
 * Configuration options.
 */
typedef struct loopyFSPollConfig {
    uint64_t intervalMs; /* Poll interval in milliseconds (default: 1000) */
    bool followSymlinks; /* Use stat() vs lstat() (default: true) */
} loopyFSPollConfig;

/**
 * Default configuration.
 */
#define LOOPY_FSPOLL_CONFIG_DEFAULT                                            \
    (loopyFSPollConfig) {                                                      \
        .intervalMs = 1000, .followSymlinks = true                             \
    }

/**
 * Initialize an FS poll config with default values.
 *
 * Sets the configuration to default values: 1000ms interval, follow symlinks
 * enabled. Use this before modifying individual fields.
 *
 * @param config Config struct to initialize (must not be NULL)
 */
void loopyFSPollConfigInit(loopyFSPollConfig *config);

/* ====================================================================
 * Lifecycle
 * ==================================================================== */

/**
 * Create a new FS poll handle.
 *
 * Creates a new file system poll monitor that uses stat() calls to detect
 * changes to files. This is portable and works reliably on network filesystems
 * where inotify/kqueue may not work.
 *
 * @param loop   Event loop (must not be NULL)
 * @param config Configuration (NULL for defaults: 1000ms interval)
 * @return New handle, or NULL on error
 *
 * Thread Safety: Must be called from the event loop thread.
 *
 * Example:
 * @code
 * loopyFSPollConfig config = LOOPY_FSPOLL_CONFIG_DEFAULT;
 * config.intervalMs = 500;  // Poll every 500ms
 * loopyFSPoll *fsp = loopyFSPollNew(loop, &config);
 * @endcode
 */
loopyFSPoll *loopyFSPollNew(loopyLoop *loop, const loopyFSPollConfig *config);

/**
 * Free an FS poll handle and all watches.
 *
 * @param fsp Handle to free, or NULL
 */
void loopyFSPollFree(loopyFSPoll *fsp);

/**
 * Get the event loop for this handle.
 *
 * @param fsp FS poll handle
 * @return The associated event loop, or NULL if fsp is NULL
 */
loopyLoop *loopyFSPollGetLoop(const loopyFSPoll *fsp);

/**
 * Get user data from FS poll handle.
 *
 * @param fsp FS poll handle
 * @return User data pointer previously set via loopyFSPollSetData(), or NULL
 */
void *loopyFSPollGetData(const loopyFSPoll *fsp);

/**
 * Set user data on FS poll handle.
 *
 * Stores an opaque user data pointer for retrieval via loopyFSPollGetData().
 * Useful for associating the poll monitor with application context.
 *
 * @param fsp  FS poll handle
 * @param data User data pointer (may be NULL)
 *
 * Thread Safety: Not thread-safe. Must be synchronized by the caller.
 */
void loopyFSPollSetData(loopyFSPoll *fsp, void *data);

/* ====================================================================
 * File Monitoring
 * ==================================================================== */

/**
 * Start monitoring a file for changes.
 *
 * The initial stat is performed immediately. If the file doesn't exist,
 * the callback will be invoked with LOOPY_FSPOLL_CREATED when it appears.
 *
 * @param fsp      FS poll handle
 * @param path     Path to monitor (must remain valid while monitoring)
 * @param events   Events to watch for (bitwise OR of loopyFSPollEvent)
 * @param cb       Callback for changes (must not be NULL)
 * @param userData User data passed to callback
 * @return Poll ID > 0 on success, 0 on error
 *
 * Example:
 * @code
 * void onChange(loopyFSPoll *fsp, const loopyFSPollInfo *info) {
 *     if (info->events & LOOPY_FSPOLL_MODIFIED) {
 *         printf("File %s was modified\n", info->path);
 *     }
 * }
 *
 * loopyFSPollId id = loopyFSPollStart(fsp, "/etc/config.json",
 *                                     LOOPY_FSPOLL_MODIFIED, onChange, NULL);
 * @endcode
 */
loopyFSPollId loopyFSPollStart(loopyFSPoll *fsp, const char *path,
                               loopyFSPollEvent events, loopyFSPollCallback *cb,
                               void *userData);

/**
 * Stop monitoring a file.
 *
 * @param fsp    FS poll handle
 * @param pollId Poll ID from loopyFSPollStart
 * @return true if stopped, false if not found
 *
 * @note The polling timer is automatically stopped when all monitors are
 *       removed, reducing CPU overhead when no files are being watched.
 */
bool loopyFSPollStop(loopyFSPoll *fsp, loopyFSPollId pollId);

/**
 * Stop monitoring all files.
 *
 * Removes all active file monitors and stops the polling timer.
 *
 * @param fsp FS poll handle
 *
 * @note The polling timer is stopped immediately after all entries are
 *       removed, so no callbacks will fire until loopyFSPollStart() is
 *       called again.
 */
void loopyFSPollStopAll(loopyFSPoll *fsp);

/**
 * Change the poll interval.
 *
 * @param fsp        FS poll handle
 * @param intervalMs New interval in milliseconds (minimum 10ms)
 */
void loopyFSPollSetInterval(loopyFSPoll *fsp, uint64_t intervalMs);

/**
 * Get the current poll interval.
 *
 * @param fsp FS poll handle
 * @return Interval in milliseconds
 */
uint64_t loopyFSPollGetInterval(const loopyFSPoll *fsp);

/* ====================================================================
 * Status
 * ==================================================================== */

/**
 * Get number of files being monitored.
 */
size_t loopyFSPollCount(const loopyFSPoll *fsp);

/**
 * Get name of an event type.
 */
const char *loopyFSPollEventName(loopyFSPollEvent event);

/**
 * Get stat for a monitored file.
 *
 * @param fsp    FS poll handle
 * @param pollId Poll ID
 * @param st     Output stat buffer
 * @return true if file exists and stat was filled, false otherwise
 */
bool loopyFSPollGetStat(const loopyFSPoll *fsp, loopyFSPollId pollId,
                        struct stat *st);

/**
 * Get the path for a poll ID.
 *
 * @param fsp    FS poll handle
 * @param pollId Poll ID
 * @return Path string, or NULL if not found
 */
const char *loopyFSPollGetPath(const loopyFSPoll *fsp, loopyFSPollId pollId);
