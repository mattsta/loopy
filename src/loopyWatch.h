/* loopyWatch - File and directory watching for loopy event loop
 *
 * Provides cross-platform file system event notification using
 * inotify (Linux), kqueue EVFILT_VNODE (BSD/macOS), or polling fallback.
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
#include <stdint.h>

/* Forward declarations */
typedef struct loopyWatch loopyWatch;
typedef uint64_t loopyWatchId;

/**
 * Watch event types.
 *
 * Represents different file system events that can occur on watched files
 * or directories. Multiple events can be combined using bitwise OR.
 *
 * Platform compatibility:
 * - MODIFY, DELETE, RENAME, ATTRIB: Supported on both Linux (inotify) and
 * BSD/macOS (kqueue)
 * - CREATE: Supported on Linux (inotify). On kqueue, requires monitoring the
 * parent directory.
 *
 * @see loopyWatchAdd()
 */
typedef enum loopyWatchEvent {
    LOOPY_WATCH_MODIFY = 0x01, /* File was modified (content/size changed) */
    LOOPY_WATCH_CREATE =
        0x02, /* File was created in watched directory (inotify only) */
    LOOPY_WATCH_DELETE = 0x04, /* File was deleted */
    LOOPY_WATCH_RENAME = 0x08, /* File was renamed or moved */
    LOOPY_WATCH_ATTRIB =
        0x10, /* Attributes changed (permissions, owner, timestamps) */
    LOOPY_WATCH_ALL = 0x1F, /* All events (bitwise OR of above) */
} loopyWatchEvent;

/**
 * Information about a file system event.
 *
 * Contains details about a file system event that occurred on a watched file or
 * directory. Passed to the watch callback when an event occurs.
 */
typedef struct loopyWatchInfo {
    loopyWatchId watchId;   /* ID of the watch that triggered this event */
    loopyWatchEvent events; /* Bitmask of events that occurred */
    const char *
        path; /* Path being watched (e.g., "/tmp/myfile.txt" or "/tmp/mydir") */
    const char *filename; /* Name of file involved (dir watches only, NULL for
                             file watches) */
    void *userData;       /* User data passed when registering the watch */
} loopyWatchInfo;

/**
 * File watcher callback function.
 *
 * Invoked when a watched file or directory changes. The callback is invoked
 * from the event loop thread, making it safe to call loopy APIs.
 *
 * @param w The watch handle
 * @param info Information about the event that occurred
 *
 * @note Callback is always invoked from the event loop thread.
 *
 * @see loopyWatchAdd()
 */
typedef void loopyWatchCallback(loopyWatch *w, const loopyWatchInfo *info);

/* ====================================================================
 * Lifecycle
 * ==================================================================== */

/**
 * Create a new file watcher attached to an event loop.
 *
 * Initializes a file system event watcher using the appropriate backend
 * for the platform:
 * - Linux: Uses inotify for efficient file system event notification
 * - BSD/macOS: Uses kqueue with EVFILT_VNODE for file event monitoring
 *
 * @param loop The event loop to attach this watcher to (must not be NULL)
 * @return A new watch handle, or NULL on failure (out of memory, loop is NULL,
 *         or platform has no supported file watching backend)
 *
 * @note On platforms without inotify or kqueue support, this function returns
 * NULL.
 *
 * @see loopyWatchFree()
 * @see loopyWatchAdd()
 *
 * @code
 * loopyWatch *w = loopyWatchNew(loop);
 * if (!w) {
 *     fprintf(stderr, "Failed to create watch\n");
 *     return -1;
 * }
 * @endcode
 */
loopyWatch *loopyWatchNew(loopyLoop *loop);

/**
 * Free a file watcher and remove all watches.
 *
 * Removes all active watches, closes underlying file descriptors, and frees
 * all associated resources. After this call, the watch pointer is invalid and
 * must not be used.
 *
 * @param w The watch handle to free (may be NULL, in which case this is a
 * no-op)
 *
 * @note All watches are removed before resources are freed, including cleanup
 *       of any associated file descriptors (inotify fd or kqueue fd).
 *
 * @see loopyWatchNew()
 * @see loopyWatchRemoveAll()
 *
 * @code
 * loopyWatchFree(w);
 * w = NULL;  // Good practice to avoid use-after-free
 * @endcode
 */
void loopyWatchFree(loopyWatch *w);

/* ====================================================================
 * Watch Management
 * ==================================================================== */

/**
 * Add a watch on a file or directory.
 *
 * Registers a watch on a file or directory with specified event mask.
 * When any of the specified events occur, the callback is invoked.
 *
 * Can watch either individual files or directories. When watching a directory:
 * - On inotify (Linux): Receives events for files created/deleted in the
 * directory
 * - On kqueue (BSD/macOS): Receives events about the directory itself
 *
 * @param w The watch handle (must not be NULL)
 * @param path The file or directory path to watch (must not be NULL)
 * @param events Bitwise OR of loopyWatchEvent values specifying which events to
 * monitor
 * @param cb The callback to invoke when an event occurs (must not be NULL)
 * @param userData User data to pass to the callback
 * @return A watch ID (non-zero) on success, 0 on failure
 *
 * @retval 0 on failure: w is NULL, path is NULL, cb is NULL, path doesn't
 * exist, maximum watches reached, or underlying watch system call failed
 * @retval non-zero watch ID on success
 *
 * @note The returned watch ID can be used to remove the watch later with
 *       loopyWatchRemove().
 *
 * @note Multiple watches can be added for the same path with different event
 * masks.
 *
 * @note Platform notes:
 *       - LOOPY_WATCH_CREATE is only available on inotify (Linux)
 *       - kqueue does not provide filename details for directory events
 * (filename will be NULL)
 *       - inotify provides filename in directory events
 *
 * @see loopyWatchRemove()
 * @see loopyWatchInfo
 * @see loopyWatchCallback
 *
 * @code
 * // Watch a log file for modifications
 * void log_changed(loopyWatch *w, const loopyWatchInfo *info) {
 *     if (info->events & LOOPY_WATCH_MODIFY) {
 *         printf("Log file modified: %s\n", info->path);
 *     }
 * }
 *
 * loopyWatchId wid = loopyWatchAdd(w, "/var/log/app.log",
 *                                   LOOPY_WATCH_MODIFY,
 *                                   log_changed, NULL);
 * if (wid == 0) {
 *     fprintf(stderr, "Failed to add watch\n");
 * }
 *
 * // Watch a directory for new files
 * void dir_changed(loopyWatch *w, const loopyWatchInfo *info) {
 *     if (info->events & LOOPY_WATCH_CREATE) {
 *         printf("File created in %s: %s\n", info->path, info->filename);
 *     }
 * }
 *
 * loopyWatchAdd(w, "/tmp/incoming",
 *              LOOPY_WATCH_CREATE | LOOPY_WATCH_DELETE,
 *              dir_changed, NULL);
 * @endcode
 */
loopyWatchId loopyWatchAdd(loopyWatch *w, const char *path,
                           loopyWatchEvent events, loopyWatchCallback *cb,
                           void *userData);

/**
 * Remove a watch.
 *
 * Stops monitoring a file or directory and frees associated resources.
 *
 * @param w The watch handle (must not be NULL)
 * @param watchId The watch ID returned by loopyWatchAdd()
 * @return true on success, false on failure
 *
 * @retval false if w is NULL, watchId is 0, or the watch ID doesn't exist
 * @retval true if the watch was successfully removed
 *
 * @see loopyWatchAdd()
 * @see loopyWatchRemoveAll()
 *
 * @code
 * loopyWatchId wid = loopyWatchAdd(w, "/tmp/file.txt", LOOPY_WATCH_MODIFY, cb,
 * NULL); if (wid) {
 *     // ... later ...
 *     loopyWatchRemove(w, wid);
 * }
 * @endcode
 */
bool loopyWatchRemove(loopyWatch *w, loopyWatchId watchId);

/**
 * Remove all watches.
 *
 * Stops monitoring all files and directories and frees all associated
 * resources.
 *
 * @param w The watch handle (if NULL, this is a no-op)
 *
 * @note This is often called as part of cleanup during loopyWatchFree().
 *
 * @see loopyWatchRemove()
 * @see loopyWatchFree()
 *
 * @code
 * loopyWatchRemoveAll(w);  // Clean slate for new watches
 * @endcode
 */
void loopyWatchRemoveAll(loopyWatch *w);

/* ====================================================================
 * Status
 * ==================================================================== */

/**
 * Get the number of active watches.
 *
 * @param w The watch handle
 * @return Number of active watches, or 0 if w is NULL
 *
 * @see loopyWatchAdd()
 * @see loopyWatchRemove()
 *
 * @code
 * size_t active = loopyWatchCount(w);
 * printf("Currently watching %zu paths\n", active);
 * @endcode
 */
size_t loopyWatchCount(const loopyWatch *w);

/**
 * Get the string name of a watch event.
 *
 * @param event A loopyWatchEvent value
 * @return A string like "MODIFY", "CREATE", "DELETE", etc., or "UNKNOWN"
 *
 * @note The returned string is a static constant and should not be freed.
 *
 * @see loopyWatchEvent
 *
 * @code
 * printf("Event type: %s\n", loopyWatchEventName(LOOPY_WATCH_MODIFY));
 * @endcode
 */
const char *loopyWatchEventName(loopyWatchEvent event);

/**
 * Get the name of the file watching backend.
 *
 * Returns a string identifying which file watching mechanism is in use.
 *
 * @return One of: "inotify" (Linux), "kqueue" (BSD/macOS), or "none"
 *         if no supported backend is available
 *
 * @note The returned string is a static constant and should not be freed.
 *
 * @code
 * const char *backend = loopyWatchBackendName();
 * printf("Using %s for file watching\n", backend);
 * @endcode
 */
const char *loopyWatchBackendName(void);

/* ====================================================================
 * Handle Accessors
 * ==================================================================== */

/**
 * Get the event loop associated with this watch handle.
 *
 * @param w The watch handle
 * @return The event loop passed to loopyWatchNew(), or NULL if w is NULL
 *
 * @see loopyWatchNew()
 */
struct loopyLoop *loopyWatchGetLoop(const loopyWatch *w);

/**
 * Get user data associated with this watch handle.
 *
 * Retrieves user-provided data that was set with loopyWatchSetData().
 *
 * @param w The watch handle
 * @return The user data previously set, or NULL if no data was set or w is NULL
 *
 * @see loopyWatchSetData()
 *
 * @code
 * void *data = loopyWatchGetData(w);
 * if (data) {
 *     FileWatchState *state = data;
 *     // Use state
 * }
 * @endcode
 */
void *loopyWatchGetData(const loopyWatch *w);

/**
 * Set user data associated with this watch handle.
 *
 * Associates arbitrary user data with this watch handle for later retrieval.
 *
 * @param w The watch handle (if NULL, this is a no-op)
 * @param data User data pointer (may be NULL)
 *
 * @see loopyWatchGetData()
 *
 * @code
 * FileWatchState *state = malloc(sizeof(FileWatchState));
 * loopyWatchSetData(w, state);
 * @endcode
 */
void loopyWatchSetData(loopyWatch *w, void *data);
