/* loopyFS - Async file system operations for loopy event loop
 *
 * Non-blocking file operations using the thread pool.
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
#include <sys/stat.h>
#include <sys/types.h>

/* ====================================================================
 * Types
 * ==================================================================== */

/**
 * Opaque file system request handle.
 */
typedef struct loopyFSRequest loopyFSRequest;

/**
 * File system operation types.
 */
typedef enum loopyFSType {
    LOOPY_FS_OPEN,
    LOOPY_FS_CLOSE,
    LOOPY_FS_READ,
    LOOPY_FS_WRITE,
    LOOPY_FS_STAT,
    LOOPY_FS_FSTAT,
    LOOPY_FS_LSTAT,
    LOOPY_FS_UNLINK,
    LOOPY_FS_MKDIR,
    LOOPY_FS_RMDIR,
    LOOPY_FS_RENAME,
    LOOPY_FS_FSYNC,
    LOOPY_FS_FDATASYNC,
    LOOPY_FS_FTRUNCATE,
    LOOPY_FS_CHMOD,
    LOOPY_FS_FCHMOD,
    LOOPY_FS_READDIR,
    /* Extended operations */
    LOOPY_FS_LINK,
    LOOPY_FS_SYMLINK,
    LOOPY_FS_READLINK,
    LOOPY_FS_REALPATH,
    LOOPY_FS_ACCESS,
    LOOPY_FS_SCANDIR,
    LOOPY_FS_CHOWN,
    LOOPY_FS_FCHOWN,
    LOOPY_FS_LCHOWN,
    LOOPY_FS_UTIME,
    LOOPY_FS_FUTIME,
    LOOPY_FS_LUTIME,
    LOOPY_FS_COPYFILE,
    LOOPY_FS_SENDFILE,
    LOOPY_FS_MKDTEMP,
    LOOPY_FS_MKSTEMP,
    LOOPY_FS_STATFS,
    LOOPY_FS_UNKNOWN
} loopyFSType;

/**
 * File system callback - called when operation completes.
 *
 * @param loop     The event loop
 * @param req      The request (may be freed after callback returns)
 * @param result   Operation result (depends on operation type):
 *                 - open: fd on success, -1 on error
 *                 - close: 0 on success, -1 on error
 *                 - read: bytes read, 0 on EOF, -1 on error
 *                 - write: bytes written, -1 on error
 *                 - stat/fstat/lstat: 0 on success, -1 on error
 *                 - unlink/mkdir/rmdir/rename: 0 on success, -1 on error
 * @param userData User data from operation call
 */
typedef void loopyFSCallback(loopyLoop *loop, loopyFSRequest *req,
                             ssize_t result, void *userData);

/* ====================================================================
 * File Operations
 * ==================================================================== */

/**
 * Open a file.
 *
 * @param loop     Event loop (NULL for sync mode)
 * @param path     File path
 * @param flags    Open flags (O_RDONLY, O_WRONLY, O_CREAT, etc.)
 * @param mode     File mode for O_CREAT (e.g., 0644)
 * @param cb       Callback (NULL for sync mode)
 * @param userData User data for callback
 * @return Request handle (async), or result in request (sync), or NULL on error
 *
 * In sync mode (cb == NULL), call loopyFSRequestGetResult() to get the fd.
 */
loopyFSRequest *loopyFSOpen(loopyLoop *loop, const char *path, int flags,
                            int mode, loopyFSCallback *cb, void *userData);

/**
 * Close a file descriptor.
 *
 * @param loop     Event loop (NULL for sync mode)
 * @param fd       File descriptor to close
 * @param cb       Callback (NULL for sync mode)
 * @param userData User data for callback
 * @return Request handle
 */
loopyFSRequest *loopyFSClose(loopyLoop *loop, int fd, loopyFSCallback *cb,
                             void *userData);

/**
 * Read from a file.
 *
 * @param loop     Event loop (NULL for sync mode)
 * @param fd       File descriptor
 * @param buf      Buffer to read into
 * @param len      Buffer length
 * @param offset   File offset (-1 for current position)
 * @param cb       Callback (NULL for sync mode)
 * @param userData User data for callback
 * @return Request handle
 */
loopyFSRequest *loopyFSRead(loopyLoop *loop, int fd, void *buf, size_t len,
                            off_t offset, loopyFSCallback *cb, void *userData);

/**
 * Write to a file.
 *
 * @param loop     Event loop (NULL for sync mode)
 * @param fd       File descriptor
 * @param buf      Buffer to write
 * @param len      Buffer length
 * @param offset   File offset (-1 for current position)
 * @param cb       Callback (NULL for sync mode)
 * @param userData User data for callback
 * @return Request handle
 */
loopyFSRequest *loopyFSWrite(loopyLoop *loop, int fd, const void *buf,
                             size_t len, off_t offset, loopyFSCallback *cb,
                             void *userData);

/**
 * Get file status.
 *
 * @param loop     Event loop (NULL for sync mode)
 * @param path     File path
 * @param statbuf  Buffer for stat result (must remain valid until callback)
 * @param cb       Callback (NULL for sync mode)
 * @param userData User data for callback
 * @return Request handle
 */
loopyFSRequest *loopyFSStat(loopyLoop *loop, const char *path,
                            struct stat *statbuf, loopyFSCallback *cb,
                            void *userData);

/**
 * Get file status by descriptor.
 *
 * @param loop     Event loop (NULL for sync mode)
 * @param fd       File descriptor
 * @param statbuf  Buffer for stat result (must remain valid until callback)
 * @param cb       Callback (NULL for sync mode)
 * @param userData User data for callback
 * @return Request handle
 */
loopyFSRequest *loopyFSFstat(loopyLoop *loop, int fd, struct stat *statbuf,
                             loopyFSCallback *cb, void *userData);

/**
 * Get symbolic link status (doesn't follow symlinks).
 *
 * @param loop     Event loop (NULL for sync mode)
 * @param path     File path
 * @param statbuf  Buffer for stat result (must remain valid until callback)
 * @param cb       Callback (NULL for sync mode)
 * @param userData User data for callback
 * @return Request handle
 */
loopyFSRequest *loopyFSLstat(loopyLoop *loop, const char *path,
                             struct stat *statbuf, loopyFSCallback *cb,
                             void *userData);

/**
 * Delete a file.
 *
 * @param loop     Event loop (NULL for sync mode)
 * @param path     File path
 * @param cb       Callback (NULL for sync mode)
 * @param userData User data for callback
 * @return Request handle
 */
loopyFSRequest *loopyFSUnlink(loopyLoop *loop, const char *path,
                              loopyFSCallback *cb, void *userData);

/**
 * Create a directory.
 *
 * @param loop     Event loop (NULL for sync mode)
 * @param path     Directory path
 * @param mode     Directory mode (e.g., 0755)
 * @param cb       Callback (NULL for sync mode)
 * @param userData User data for callback
 * @return Request handle
 */
loopyFSRequest *loopyFSMkdir(loopyLoop *loop, const char *path, int mode,
                             loopyFSCallback *cb, void *userData);

/**
 * Remove a directory.
 *
 * @param loop     Event loop (NULL for sync mode)
 * @param path     Directory path
 * @param cb       Callback (NULL for sync mode)
 * @param userData User data for callback
 * @return Request handle
 */
loopyFSRequest *loopyFSRmdir(loopyLoop *loop, const char *path,
                             loopyFSCallback *cb, void *userData);

/**
 * Rename a file or directory.
 *
 * @param loop     Event loop (NULL for sync mode)
 * @param path     Current path
 * @param newPath  New path
 * @param cb       Callback (NULL for sync mode)
 * @param userData User data for callback
 * @return Request handle
 */
loopyFSRequest *loopyFSRename(loopyLoop *loop, const char *path,
                              const char *newPath, loopyFSCallback *cb,
                              void *userData);

/**
 * Sync file data and metadata to disk.
 *
 * @param loop     Event loop (NULL for sync mode)
 * @param fd       File descriptor
 * @param cb       Callback (NULL for sync mode)
 * @param userData User data for callback
 * @return Request handle
 */
loopyFSRequest *loopyFSFsync(loopyLoop *loop, int fd, loopyFSCallback *cb,
                             void *userData);

/**
 * Sync file data (not metadata) to disk.
 *
 * @param loop     Event loop (NULL for sync mode)
 * @param fd       File descriptor
 * @param cb       Callback (NULL for sync mode)
 * @param userData User data for callback
 * @return Request handle
 */
loopyFSRequest *loopyFSFdatasync(loopyLoop *loop, int fd, loopyFSCallback *cb,
                                 void *userData);

/**
 * Truncate a file.
 *
 * @param loop     Event loop (NULL for sync mode)
 * @param fd       File descriptor
 * @param length   New file length
 * @param cb       Callback (NULL for sync mode)
 * @param userData User data for callback
 * @return Request handle
 */
loopyFSRequest *loopyFSFtruncate(loopyLoop *loop, int fd, off_t length,
                                 loopyFSCallback *cb, void *userData);

/**
 * Change file mode.
 *
 * @param loop     Event loop (NULL for sync mode)
 * @param path     File path
 * @param mode     New mode
 * @param cb       Callback (NULL for sync mode)
 * @param userData User data for callback
 * @return Request handle
 */
loopyFSRequest *loopyFSChmod(loopyLoop *loop, const char *path, int mode,
                             loopyFSCallback *cb, void *userData);

/**
 * Change file mode by descriptor.
 *
 * @param loop     Event loop (NULL for sync mode)
 * @param fd       File descriptor
 * @param mode     New mode
 * @param cb       Callback (NULL for sync mode)
 * @param userData User data for callback
 * @return Request handle
 */
loopyFSRequest *loopyFSFchmod(loopyLoop *loop, int fd, int mode,
                              loopyFSCallback *cb, void *userData);

/* ====================================================================
 * Extended File Operations
 * ==================================================================== */

/**
 * Create a hard link.
 *
 * @param loop     Event loop (NULL for sync mode)
 * @param path     Existing file path
 * @param newPath  New link path
 * @param cb       Callback (NULL for sync mode)
 * @param userData User data for callback
 * @return Request handle
 */
loopyFSRequest *loopyFSLink(loopyLoop *loop, const char *path,
                            const char *newPath, loopyFSCallback *cb,
                            void *userData);

/**
 * Create a symbolic link.
 *
 * @param loop     Event loop (NULL for sync mode)
 * @param target   Target path (what the symlink points to)
 * @param linkPath Path for the new symlink
 * @param cb       Callback (NULL for sync mode)
 * @param userData User data for callback
 * @return Request handle
 */
loopyFSRequest *loopyFSSymlink(loopyLoop *loop, const char *target,
                               const char *linkPath, loopyFSCallback *cb,
                               void *userData);

/**
 * Read a symbolic link.
 *
 * @param loop     Event loop (NULL for sync mode)
 * @param path     Symlink path
 * @param buf      Buffer to store target path
 * @param bufLen   Buffer size
 * @param cb       Callback (NULL for sync mode)
 * @param userData User data for callback
 * @return Request handle (result is length of target, or -1 on error)
 */
loopyFSRequest *loopyFSReadlink(loopyLoop *loop, const char *path, char *buf,
                                size_t bufLen, loopyFSCallback *cb,
                                void *userData);

/**
 * Resolve canonical absolute path.
 *
 * @param loop     Event loop (NULL for sync mode)
 * @param path     Path to resolve
 * @param buf      Buffer to store resolved path (should be PATH_MAX or larger)
 * @param bufLen   Buffer size
 * @param cb       Callback (NULL for sync mode)
 * @param userData User data for callback
 * @return Request handle (result is 0 on success, -1 on error)
 */
loopyFSRequest *loopyFSRealpath(loopyLoop *loop, const char *path, char *buf,
                                size_t bufLen, loopyFSCallback *cb,
                                void *userData);

/**
 * Check file accessibility.
 *
 * @param loop     Event loop (NULL for sync mode)
 * @param path     File path
 * @param mode     Access mode (R_OK, W_OK, X_OK, F_OK)
 * @param cb       Callback (NULL for sync mode)
 * @param userData User data for callback
 * @return Request handle (result is 0 if accessible, -1 if not)
 */
loopyFSRequest *loopyFSAccess(loopyLoop *loop, const char *path, int mode,
                              loopyFSCallback *cb, void *userData);

/**
 * Directory entry from scandir.
 */
typedef struct loopyFSDirent {
    char *name;         /* Entry name */
    unsigned char type; /* DT_REG, DT_DIR, DT_LNK, etc. */
} loopyFSDirent;

/**
 * Scan a directory.
 *
 * Reads directory entries, automatically skipping "." and ".." entries.
 * Results are cached in the request object and can be retrieved using
 * loopyFSRequestGetDirents().
 *
 * @param loop     Event loop (NULL for sync mode)
 * @param path     Directory path
 * @param cb       Callback (NULL for sync mode)
 * @param userData User data for callback
 * @return Request handle (result is number of entries, or -1 on error)
 *
 * @note After callback (async) or immediately (sync), use
 *       loopyFSRequestGetDirents() to access the directory entries.
 *       Memory for the dirent array is owned by the request and is freed
 *       when loopyFSRequestFree() is called.
 *
 * @note Entries include all files except "." and "..", sorted in undefined
 *       order. The d_type field may be DT_UNKNOWN on some filesystems.
 *
 * Example:
 * @code
 *   loopyFSRequest *req = loopyFSScandir(loop, "/tmp", cb, NULL);
 *   // In callback:
 *   size_t count = 0;
 *   const loopyFSDirent *entries = loopyFSRequestGetDirents(req, &count);
 *   for (size_t i = 0; i < count; i++) {
 *       printf("Entry: %s (type=%u)\n", entries[i].name, entries[i].type);
 *   }
 *   loopyFSRequestFree(req);
 * @endcode
 */
loopyFSRequest *loopyFSScandir(loopyLoop *loop, const char *path,
                               loopyFSCallback *cb, void *userData);

/**
 * Get scandir results.
 *
 * Retrieves the directory entry array from a completed LOOPY_FS_SCANDIR
 * request. This function should only be called after the callback has been
 * invoked or after a synchronous operation completes.
 *
 * @param req      The request (must be LOOPY_FS_SCANDIR)
 * @param count    Output: number of entries (set to 0 if not applicable)
 * @return Array of loopyFSDirent structures, or NULL if:
 *         - req is NULL
 *         - req type is not LOOPY_FS_SCANDIR
 *         - no entries were found (count will be 0)
 *
 * @note The returned pointer is valid until loopyFSRequestFree(req) is called.
 *       Do NOT attempt to free the array directly - it is owned by the
 *       request object.
 *
 * @note Entry names are dynamically allocated and owned by the request.
 *       Each name is null-terminated for convenience.
 *
 * @note This function is safe to call multiple times on the same request.
 *
 * @return Lifecycle: The memory remains valid from callback invocation through
 *                    loopyFSRequestFree(). Afterwards, the pointer becomes
 *                    invalid and must not be accessed.
 */
const loopyFSDirent *loopyFSRequestGetDirents(const loopyFSRequest *req,
                                              size_t *count);

/**
 * Change file ownership.
 *
 * @param loop     Event loop (NULL for sync mode)
 * @param path     File path
 * @param uid      New owner UID (-1 to leave unchanged)
 * @param gid      New group GID (-1 to leave unchanged)
 * @param cb       Callback (NULL for sync mode)
 * @param userData User data for callback
 * @return Request handle
 */
loopyFSRequest *loopyFSChown(loopyLoop *loop, const char *path, uid_t uid,
                             gid_t gid, loopyFSCallback *cb, void *userData);

/**
 * Change file ownership by descriptor.
 *
 * @param loop     Event loop (NULL for sync mode)
 * @param fd       File descriptor
 * @param uid      New owner UID (-1 to leave unchanged)
 * @param gid      New group GID (-1 to leave unchanged)
 * @param cb       Callback (NULL for sync mode)
 * @param userData User data for callback
 * @return Request handle
 */
loopyFSRequest *loopyFSFchown(loopyLoop *loop, int fd, uid_t uid, gid_t gid,
                              loopyFSCallback *cb, void *userData);

/**
 * Change symlink ownership (doesn't follow symlinks).
 *
 * @param loop     Event loop (NULL for sync mode)
 * @param path     Symlink path
 * @param uid      New owner UID (-1 to leave unchanged)
 * @param gid      New group GID (-1 to leave unchanged)
 * @param cb       Callback (NULL for sync mode)
 * @param userData User data for callback
 * @return Request handle
 */
loopyFSRequest *loopyFSLchown(loopyLoop *loop, const char *path, uid_t uid,
                              gid_t gid, loopyFSCallback *cb, void *userData);

/**
 * Time specification for utime operations.
 */
typedef struct loopyFSTimespec {
    int64_t sec;  /* Seconds since epoch */
    int64_t nsec; /* Nanoseconds (0-999999999) */
} loopyFSTimespec;

/**
 * Special time values for utime.
 */
#define LOOPY_FS_UTIME_NOW ((int64_t)-1)  /* Use current time */
#define LOOPY_FS_UTIME_OMIT ((int64_t)-2) /* Leave unchanged */

/**
 * Change file access and modification times.
 *
 * @param loop     Event loop (NULL for sync mode)
 * @param path     File path
 * @param atime    Access time (or LOOPY_FS_UTIME_NOW/OMIT)
 * @param mtime    Modification time (or LOOPY_FS_UTIME_NOW/OMIT)
 * @param cb       Callback (NULL for sync mode)
 * @param userData User data for callback
 * @return Request handle
 */
loopyFSRequest *loopyFSUtime(loopyLoop *loop, const char *path,
                             const loopyFSTimespec *atime,
                             const loopyFSTimespec *mtime, loopyFSCallback *cb,
                             void *userData);

/**
 * Change file times by descriptor.
 *
 * @param loop     Event loop (NULL for sync mode)
 * @param fd       File descriptor
 * @param atime    Access time (or LOOPY_FS_UTIME_NOW/OMIT)
 * @param mtime    Modification time (or LOOPY_FS_UTIME_NOW/OMIT)
 * @param cb       Callback (NULL for sync mode)
 * @param userData User data for callback
 * @return Request handle
 */
loopyFSRequest *loopyFSFutime(loopyLoop *loop, int fd,
                              const loopyFSTimespec *atime,
                              const loopyFSTimespec *mtime, loopyFSCallback *cb,
                              void *userData);

/**
 * Change symlink times (doesn't follow symlinks).
 *
 * @param loop     Event loop (NULL for sync mode)
 * @param path     Symlink path
 * @param atime    Access time (or LOOPY_FS_UTIME_NOW/OMIT)
 * @param mtime    Modification time (or LOOPY_FS_UTIME_NOW/OMIT)
 * @param cb       Callback (NULL for sync mode)
 * @param userData User data for callback
 * @return Request handle
 */
loopyFSRequest *loopyFSLutime(loopyLoop *loop, const char *path,
                              const loopyFSTimespec *atime,
                              const loopyFSTimespec *mtime, loopyFSCallback *cb,
                              void *userData);

/**
 * Flags for copyfile.
 */
typedef enum loopyFSCopyFlags {
    LOOPY_FS_COPY_DEFAULT = 0,      /* Default copy behavior */
    LOOPY_FS_COPY_EXCL = 1,         /* Fail if destination exists */
    LOOPY_FS_COPY_FICLONE = 2,      /* Try copy-on-write clone first */
    LOOPY_FS_COPY_FICLONE_FORCE = 4 /* Use only copy-on-write, fail otherwise */
} loopyFSCopyFlags;

/**
 * Copy a file.
 *
 * @param loop     Event loop (NULL for sync mode)
 * @param srcPath  Source file path
 * @param dstPath  Destination file path
 * @param flags    Copy flags
 * @param cb       Callback (NULL for sync mode)
 * @param userData User data for callback
 * @return Request handle (result is 0 on success)
 */
loopyFSRequest *loopyFSCopyfile(loopyLoop *loop, const char *srcPath,
                                const char *dstPath, loopyFSCopyFlags flags,
                                loopyFSCallback *cb, void *userData);

/**
 * Transfer data between file descriptors.
 *
 * @param loop     Event loop (NULL for sync mode)
 * @param outFd    Destination file descriptor
 * @param inFd     Source file descriptor
 * @param offset   Offset in source file (-1 for current position)
 * @param len      Number of bytes to transfer
 * @param cb       Callback (NULL for sync mode)
 * @param userData User data for callback
 * @return Request handle (result is bytes transferred)
 */
loopyFSRequest *loopyFSSendfile(loopyLoop *loop, int outFd, int inFd,
                                off_t offset, size_t len, loopyFSCallback *cb,
                                void *userData);

/**
 * Create a unique temporary directory.
 *
 * @param loop     Event loop (NULL for sync mode)
 * @param tpl      Template path (must end with XXXXXX, modified in place)
 * @param cb       Callback (NULL for sync mode)
 * @param userData User data for callback
 * @return Request handle (result is 0 on success, path is in template)
 */
loopyFSRequest *loopyFSMkdtemp(loopyLoop *loop, char *tpl, loopyFSCallback *cb,
                               void *userData);

/**
 * Create a unique temporary file.
 *
 * @param loop     Event loop (NULL for sync mode)
 * @param tpl      Template path (must end with XXXXXX, modified in place)
 * @param cb       Callback (NULL for sync mode)
 * @param userData User data for callback
 * @return Request handle (result is fd on success, path is in template)
 */
loopyFSRequest *loopyFSMkstemp(loopyLoop *loop, char *tpl, loopyFSCallback *cb,
                               void *userData);

/* ====================================================================
 * Request Management
 * ==================================================================== */

/**
 * Free a request.
 *
 * Releases all resources associated with a file system request, including
 * any cached results (stat buffers, directory entries, error messages).
 * This must be called exactly once per request returned by a loopyFS*
 * function.
 *
 * @param req Request to free, or NULL (safe no-op)
 *
 * @note For async operations: Call this in or after the callback.
 * @note For sync operations: Call immediately after getting the result.
 * @note After calling this, the request pointer becomes invalid and must
 *       not be accessed.
 * @note Calling with NULL is safe and has no effect.
 *
 * @warning Do NOT double-free. Calling loopyFSRequestFree() twice on the
 *          same request will cause undefined behavior.
 *
 * @note If the request owns any allocated memory (paths for scandir entries,
 *       error strings), all of it will be freed. The stat buffer and other
 *       user-provided buffers are NOT freed, as they are owned by the caller.
 *
 * Example:
 * @code
 *   void my_callback(loopyLoop *loop, loopyFSRequest *req,
 *                    ssize_t result, void *userData) {
 *       if (result >= 0) {
 *           printf("Success: %zd\n", result);
 *       } else {
 *           printf("Error: %s\n", loopyFSGetError(req));
 *       }
 *       loopyFSRequestFree(req);  // Must free here
 *   }
 * @endcode
 */
void loopyFSRequestFree(loopyFSRequest *req);

/**
 * Cancel a pending request.
 *
 * Attempts to cancel an asynchronous file system operation. If the operation
 * has already started executing in the thread pool (or io_uring), cancellation
 * will be marked but the operation may still complete.
 *
 * @param req Request to cancel
 * @return true if cancellation was marked for processing, false if:
 *         - req is NULL
 *         - the request has already completed
 *         - the request is synchronous (not applicable)
 *
 * @note If the operation is cancelled before execution, the callback will be
 *       invoked with result=-1 and errno=ECANCELED.
 *
 * @note If the operation is already running when cancel is called, the cancel
 *       flag is set, but the operation may still complete normally. This is
 *       a race condition inherent to asynchronous cancellation.
 *
 * @note After calling loopyFSCancel(), you must still free the request with
 *       loopyFSRequestFree() when done, whether the cancellation succeeded
 *       or the operation completed.
 *
 * @note On Linux with io_uring, cancelled operations may not invoke the
 *       callback if they are successfully cancelled before submission.
 *
 * @warning Do not assume the callback won't be called after loopyFSCancel()
 *          returns. The callback may be in progress when cancel is called
 *          (on another thread), or may still fire with a result.
 *
 * Example:
 * @code
 *   loopyFSRequest *req = loopyFSRead(loop, fd, buf, len, -1, cb, NULL);
 *   if (some_condition) {
 *       if (loopyFSCancel(req)) {
 *           printf("Cancellation requested\n");
 *       } else {
 *           printf("Operation already completed\n");
 *       }
 *   }
 *   // Later, when callback fires or operation completes:
 *   loopyFSRequestFree(req);
 * @endcode
 */
bool loopyFSCancel(loopyFSRequest *req);

/* ====================================================================
 * Request Properties
 * ==================================================================== */

/**
 * Get the operation type.
 *
 * Returns the file system operation type that was requested. Useful for
 * understanding what operation the request represents, especially when
 * handling results generically.
 *
 * @param req The request
 * @return Operation type (loopyFSType enum), or LOOPY_FS_UNKNOWN if req is NULL
 *
 * Example:
 * @code
 *   loopyFSType type = loopyFSRequestGetType(req);
 *   if (type == LOOPY_FS_READ) {
 *       printf("This was a read operation\n");
 *   }
 * @endcode
 */
loopyFSType loopyFSRequestGetType(const loopyFSRequest *req);

/**
 * Get the path (for path-based operations).
 *
 * Returns the file system path associated with the request, if any.
 * For descriptor-based operations (read, write, fstat, fchown, etc.),
 * this will return NULL as no path is associated.
 *
 * @param req The request
 * @return File system path string, or NULL if:
 *         - req is NULL
 *         - the operation is descriptor-based (no path)
 *
 * @note The returned pointer is valid until loopyFSRequestFree() is called.
 *
 * Example:
 * @code
 *   const char *path = loopyFSRequestGetPath(req);
 *   if (path) {
 *       printf("Operation on path: %s\n", path);
 *   } else {
 *       printf("Operation is descriptor-based\n");
 *   }
 * @endcode
 */
const char *loopyFSRequestGetPath(const loopyFSRequest *req);

/**
 * Get the result (for sync operations or after callback).
 *
 * Retrieves the operation result. The meaning depends on the operation type:
 * - open: file descriptor (>= 0) on success, -1 on error
 * - read: bytes read (>= 0) or -1 on error
 * - write: bytes written (>= 0) or -1 on error
 * - stat/fstat/lstat: 0 on success, -1 on error
 * - mkdir/rmdir/unlink/rename: 0 on success, -1 on error
 * - scandir: number of entries (>= 0) or -1 on error
 * - other: operation-specific, see docs
 *
 * @param req The request
 * @return Operation result (interpretation depends on operation type)
 *         Returns -1 if req is NULL.
 *
 * @note For sync operations (cb == NULL on call), the result is available
 *       immediately after the function returns.
 * @note For async operations, the result is available in/after the callback.
 * @note Negative results may indicate errors. Use loopyFSGetError() for
 *       detailed error messages.
 *
 * Example:
 * @code
 *   ssize_t result = loopyFSRequestGetResult(req);
 *   if (result < 0) {
 *       printf("Error: %s\n", loopyFSGetError(req));
 *   } else if (loopyFSRequestGetType(req) == LOOPY_FS_READ) {
 *       printf("Read %zd bytes\n", result);
 *   }
 * @endcode
 */
ssize_t loopyFSRequestGetResult(const loopyFSRequest *req);

/**
 * Get the event loop.
 *
 * Returns the event loop associated with this request. This is the loop
 * passed to the original loopyFS* function call, or NULL if the request
 * was made in synchronous mode.
 *
 * @param req The request
 * @return Event loop pointer, or NULL if:
 *         - req is NULL
 *         - the operation was synchronous (no loop provided)
 *
 * Example:
 * @code
 *   loopyLoop *loop = loopyFSRequestGetLoop(req);
 *   if (loop) {
 *       printf("Async operation on loop %p\n", (void *)loop);
 *   } else {
 *       printf("Sync operation (no loop)\n");
 *   }
 * @endcode
 */
loopyLoop *loopyFSRequestGetLoop(const loopyFSRequest *req);

/**
 * Get the stat buffer (for stat operations).
 *
 * Retrieves the populated stat buffer from a completed stat, fstat, or
 * lstat operation. The buffer is populated by the operation and should
 * be read only after the operation completes.
 *
 * @param req The request
 * @return Pointer to populated struct stat, or NULL if:
 *         - req is NULL
 *         - the operation is not stat/fstat/lstat
 *         - the operation failed
 *
 * @note The returned pointer remains valid until loopyFSRequestFree(req).
 * @note For async operations, data is populated during callback execution.
 *       Ensure the buffer remains valid until the callback completes.
 * @note The stat buffer must be provided by the user and passed to the
 *       original loopyFSStat/Fstat/Lstat call. This function returns that
 *       same buffer pointer after it has been populated.
 *
 * Example:
 * @code
 *   struct stat sb;
 *   loopyFSRequest *req = loopyFSStat(loop, "/tmp/file", &sb, cb, NULL);
 *   // In callback:
 *   const struct stat *result = loopyFSRequestGetStatbuf(req);
 *   if (result) {
 *       printf("File size: %zu\n", result->st_size);
 *   }
 * @endcode
 */
const struct stat *loopyFSRequestGetStatbuf(const loopyFSRequest *req);

/**
 * Get the error message (after failed operation).
 *
 * Returns a descriptive error message for a failed operation. The message
 * format is typically "operation: error description", combining the operation
 * name with the system error string.
 *
 * @param req The request
 * @return Error message string (always non-NULL), or empty string "" if:
 *         - req is NULL
 *         - the operation succeeded (no error)
 *         - the error message buffer wasn't filled
 *
 * @note The returned pointer remains valid until loopyFSRequestFree(req).
 * @note For operations that succeeded (loopyFSRequestGetResult() >= 0),
 *       this function returns an empty string.
 * @note The error string has a maximum length of 127 characters.
 * @note Use in combination with loopyFSRequestGetResult() to detect errors:
 *       if (result < 0) { error = loopyFSGetError(req); }
 *
 * Example:
 * @code
 *   ssize_t result = loopyFSRequestGetResult(req);
 *   if (result < 0) {
 *       fprintf(stderr, "Operation failed: %s\n", loopyFSGetError(req));
 *   }
 * @endcode
 */
const char *loopyFSGetError(const loopyFSRequest *req);

/* ====================================================================
 * Filesystem Statistics
 * ==================================================================== */

/**
 * Filesystem statistics.
 * Cross-platform subset of statfs/statvfs data.
 */
typedef struct loopyStatfs {
    uint64_t type;     /* Type of filesystem */
    uint64_t bsize;    /* Optimal transfer block size */
    uint64_t blocks;   /* Total data blocks */
    uint64_t bfree;    /* Free blocks */
    uint64_t bavail;   /* Free blocks available to non-root */
    uint64_t files;    /* Total file nodes */
    uint64_t ffree;    /* Free file nodes */
    uint64_t spare[4]; /* Reserved for future use */
} loopyStatfs;

/**
 * Get filesystem statistics.
 *
 * @param loop     Event loop (NULL for sync mode)
 * @param path     Path on the filesystem
 * @param buf      Buffer to store result
 * @param cb       Callback (NULL for sync mode)
 * @param userData User data for callback
 * @return Request handle (result is 0 on success, -1 on error)
 */
loopyFSRequest *loopyFSStatfs(loopyLoop *loop, const char *path,
                              loopyStatfs *buf, loopyFSCallback *cb,
                              void *userData);

/**
 * Get the statfs buffer (for statfs operations).
 *
 * Retrieves the populated filesystem statistics buffer from a completed
 * loopyFSStatfs operation. This provides cross-platform filesystem information.
 *
 * @param req The request (must be from loopyFSStatfs)
 * @return Pointer to populated loopyStatfs structure, or NULL if:
 *         - req is NULL
 *         - the operation is not loopyFSStatfs
 *         - the operation failed
 *
 * @note The returned pointer remains valid until loopyFSRequestFree(req).
 * @note The statfs buffer must be provided by the user and passed to the
 *       original loopyFSStatfs call.
 * @note All sizes are in units of f_bsize (optimal transfer block size),
 *       except for bsize itself which is in bytes.
 * @note The loopyStatfs structure provides a cross-platform subset of
 *       statfs/statvfs data for portability.
 *
 * Lifetime Note: The buffer is valid immediately after async callback
 *                invocation through loopyFSRequestFree(). For sync
 *                operations, it is valid immediately after the call.
 *
 * Example:
 * @code
 *   loopyStatfs buf;
 *   loopyFSRequest *req = loopyFSStatfs(loop, "/", &buf, cb, NULL);
 *   // In callback:
 *   const loopyStatfs *sfs = loopyFSRequestGetStatfsBuf(req);
 *   if (sfs) {
 *       uint64_t free_bytes = sfs->bavail * sfs->bsize;
 *       printf("Free space: %lu bytes\n", free_bytes);
 *   }
 * @endcode
 */
const loopyStatfs *loopyFSRequestGetStatfsBuf(const loopyFSRequest *req);

/* ====================================================================
 * Direct I/O Support
 * ==================================================================== */

/**
 * O_DIRECT flag for unbuffered I/O (Linux-specific).
 *
 * Direct I/O bypasses the kernel page cache, providing:
 *  - No double-buffering (kernel cache + application buffer)
 *  - Predictable I/O performance
 *  - Reduced memory pressure
 *
 * Requirements:
 *  - Buffer must be aligned to sector size (typically 512 bytes)
 *  - File offset must be aligned to sector size
 *  - Transfer size must be multiple of sector size
 *  - Use loopyFSAllocAligned() to allocate aligned buffers
 *
 * Use cases:
 *  - Database systems managing their own cache
 *  - High-performance I/O applications
 *  - Avoiding cache pollution for large sequential I/O
 *
 * Note: On macOS, use F_NOCACHE fcntl() after opening instead.
 *
 * Example:
 *   // Allocate aligned buffer for O_DIRECT
 *   void *buf = loopyFSAllocAligned(4096, 512);
 *   int fd = loopyFSOpen(loop, "data.bin", O_RDWR | O_DIRECT, 0644, cb, data);
 *   loopyFSRead(loop, fd, buf, 4096, 0, cb, data);
 *   loopyFSFreeAligned(buf);
 */
#ifndef O_DIRECT
#define O_DIRECT 0 /* Not supported on this platform */
#endif

/**
 * Allocate aligned memory for direct I/O.
 *
 * Allocates memory aligned to the specified alignment boundary.
 * Required for O_DIRECT file operations.
 *
 * @param size      Size in bytes to allocate
 * @param alignment Alignment requirement (typically 512 or 4096)
 * @return Aligned memory pointer, or NULL on failure
 *
 * Must be freed with loopyFSFreeAligned().
 */
void *loopyFSAllocAligned(size_t size, size_t alignment);

/**
 * Free memory allocated by loopyFSAllocAligned().
 *
 * @param ptr Pointer to free (can be NULL)
 */
void loopyFSFreeAligned(void *ptr);

/**
 * Get the recommended alignment for direct I/O.
 *
 * Returns the sector size for optimal O_DIRECT performance.
 *
 * @return Alignment in bytes (typically 512 or 4096)
 */
size_t loopyFSDirectAlignment(void);
