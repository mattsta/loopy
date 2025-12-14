/* loopyFS - Async file system operations for loopy event loop
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
#include "loopyFS.h"
#include "loopyWork.h"

#ifdef USE_IOURING
#include "loopyIoUringFS.h"
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/mount.h> /* For statfs */
#else
#include <sys/vfs.h> /* For statfs on Linux */
#endif

#ifdef __APPLE__
#include <copyfile.h>
#include <sys/clonefile.h>
#include <sys/socket.h>
#include <sys/uio.h>
#endif

#ifdef __linux__
#include <sys/sendfile.h>
#endif

/* Custom fsStrdup using zcalloc to avoid deprecation warnings */
static inline char *fsStrdup(const char *s) {
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
 * Internal data structures
 * ==================================================================== */

struct loopyFSRequest {
    loopyLoop *loop;
    loopyFSType type;
    loopyFSCallback *cb;
    void *userData;

    /* Operation-specific parameters */
    char *path;
    char *newPath; /* For rename, link, symlink, copyfile */
    int fd;
    int fd2; /* For sendfile (outFd) */
    int flags;
    int mode;
    void *buf;
    size_t len;
    off_t offset;
    struct stat *statbuf;

    /* Extended operation parameters */
    uid_t uid;
    gid_t gid;
    loopyFSTimespec atime;
    loopyFSTimespec mtime;
    loopyFSCopyFlags copyFlags;

    /* Scandir results */
    loopyFSDirent *dirents;
    size_t direntCount;

    /* Statfs result */
    loopyStatfs *statfsbuf;

    /* Result */
    ssize_t result;
    int savedErrno;
    char errorString[128];

    /* Work request for async operations */
    loopyWork *work;
    loopyWorkId workId;
    bool cancelled;
    bool completed;

#ifdef USE_IOURING
    /* io_uring operation tracking */
    uint64_t ioUringOpId;
    bool usingIoUring;
#endif
};

/* ====================================================================
 * Internal helpers
 * ==================================================================== */

static void fsSetError(loopyFSRequest *req, const char *field) {
    snprintf(req->errorString, sizeof(req->errorString), "%s: %s", field,
             strerror(errno));
}

static loopyFSRequest *fsRequestNew(loopyLoop *loop, loopyFSType type,
                                    loopyFSCallback *cb, void *userData) {
    loopyFSRequest *req = zcalloc(1, sizeof(*req));
    if (!req) {
        return NULL;
    }

    req->loop = loop;
    req->type = type;
    req->cb = cb;
    req->userData = userData;
    req->fd = -1;
    req->offset = -1;
    req->result = -1;

    return req;
}

/* ====================================================================
 * Work callbacks
 * ==================================================================== */

static void fsWorkCallback(loopyWork *work, loopyWorkId workId,
                           void *userData) {
    (void)work;
    (void)workId;
    loopyFSRequest *req = userData;

    if (req->cancelled) {
        req->savedErrno = ECANCELED;
        req->result = -1;
        return;
    }

    switch (req->type) {
    case LOOPY_FS_OPEN:
        req->result = open(req->path, req->flags, req->mode);
        if (req->result < 0) {
            req->savedErrno = errno;
            fsSetError(req, "open");
        }
        break;

    case LOOPY_FS_CLOSE:
        req->result = close(req->fd);
        if (req->result < 0) {
            req->savedErrno = errno;
            fsSetError(req, "close");
        }
        break;

    case LOOPY_FS_READ:
        if (req->offset >= 0) {
            req->result = pread(req->fd, req->buf, req->len, req->offset);
        } else {
            req->result = read(req->fd, req->buf, req->len);
        }
        if (req->result < 0) {
            req->savedErrno = errno;
            fsSetError(req, "read");
        }
        break;

    case LOOPY_FS_WRITE:
        if (req->offset >= 0) {
            req->result = pwrite(req->fd, req->buf, req->len, req->offset);
        } else {
            req->result = write(req->fd, req->buf, req->len);
        }
        if (req->result < 0) {
            req->savedErrno = errno;
            fsSetError(req, "write");
        }
        break;

    case LOOPY_FS_STAT:
        req->result = stat(req->path, req->statbuf);
        if (req->result < 0) {
            req->savedErrno = errno;
            fsSetError(req, "stat");
        }
        break;

    case LOOPY_FS_FSTAT:
        req->result = fstat(req->fd, req->statbuf);
        if (req->result < 0) {
            req->savedErrno = errno;
            fsSetError(req, "fstat");
        }
        break;

    case LOOPY_FS_LSTAT:
        req->result = lstat(req->path, req->statbuf);
        if (req->result < 0) {
            req->savedErrno = errno;
            fsSetError(req, "lstat");
        }
        break;

    case LOOPY_FS_UNLINK:
        req->result = unlink(req->path);
        if (req->result < 0) {
            req->savedErrno = errno;
            fsSetError(req, "unlink");
        }
        break;

    case LOOPY_FS_MKDIR:
        req->result = mkdir(req->path, req->mode);
        if (req->result < 0) {
            req->savedErrno = errno;
            fsSetError(req, "mkdir");
        }
        break;

    case LOOPY_FS_RMDIR:
        req->result = rmdir(req->path);
        if (req->result < 0) {
            req->savedErrno = errno;
            fsSetError(req, "rmdir");
        }
        break;

    case LOOPY_FS_RENAME:
        req->result = rename(req->path, req->newPath);
        if (req->result < 0) {
            req->savedErrno = errno;
            fsSetError(req, "rename");
        }
        break;

    case LOOPY_FS_FSYNC:
        req->result = fsync(req->fd);
        if (req->result < 0) {
            req->savedErrno = errno;
            fsSetError(req, "fsync");
        }
        break;

    case LOOPY_FS_FDATASYNC:
#ifdef __APPLE__
        /* macOS doesn't have fdatasync, use fcntl F_FULLFSYNC for similar
         * effect */
        req->result = fcntl(req->fd, F_FULLFSYNC);
        if (req->result < 0) {
            /* Fall back to fsync */
            req->result = fsync(req->fd);
        }
#else
        req->result = fdatasync(req->fd);
#endif
        if (req->result < 0) {
            req->savedErrno = errno;
            fsSetError(req, "fdatasync");
        }
        break;

    case LOOPY_FS_FTRUNCATE:
        req->result = ftruncate(req->fd, req->offset);
        if (req->result < 0) {
            req->savedErrno = errno;
            fsSetError(req, "ftruncate");
        }
        break;

    case LOOPY_FS_CHMOD:
        req->result = chmod(req->path, req->mode);
        if (req->result < 0) {
            req->savedErrno = errno;
            fsSetError(req, "chmod");
        }
        break;

    case LOOPY_FS_FCHMOD:
        req->result = fchmod(req->fd, req->mode);
        if (req->result < 0) {
            req->savedErrno = errno;
            fsSetError(req, "fchmod");
        }
        break;

    case LOOPY_FS_LINK:
        req->result = link(req->path, req->newPath);
        if (req->result < 0) {
            req->savedErrno = errno;
            fsSetError(req, "link");
        }
        break;

    case LOOPY_FS_SYMLINK:
        req->result = symlink(req->path, req->newPath);
        if (req->result < 0) {
            req->savedErrno = errno;
            fsSetError(req, "symlink");
        }
        break;

    case LOOPY_FS_READLINK:
        req->result = readlink(req->path, req->buf, req->len);
        if (req->result < 0) {
            req->savedErrno = errno;
            fsSetError(req, "readlink");
        } else if ((size_t)req->result < req->len) {
            /* Null-terminate the result */
            ((char *)req->buf)[req->result] = '\0';
        }
        break;

    case LOOPY_FS_REALPATH: {
        char *resolved = realpath(req->path, NULL);
        if (resolved) {
            size_t len = strlen(resolved);
            if (len < req->len) {
                memcpy(req->buf, resolved, len + 1);
                req->result = 0;
            } else {
                req->result = -1;
                req->savedErrno = ENAMETOOLONG;
                fsSetError(req, "realpath");
            }
            zfree(resolved);
        } else {
            req->result = -1;
            req->savedErrno = errno;
            fsSetError(req, "realpath");
        }
        break;
    }

    case LOOPY_FS_ACCESS:
        req->result = access(req->path, req->mode);
        if (req->result < 0) {
            req->savedErrno = errno;
            fsSetError(req, "access");
        }
        break;

    case LOOPY_FS_SCANDIR: {
        DIR *dir = opendir(req->path);
        if (!dir) {
            req->result = -1;
            req->savedErrno = errno;
            fsSetError(req, "opendir");
            break;
        }

        /* Count entries first */
        size_t count = 0;
        const struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            /* Skip . and .. */
            if (de->d_name[0] == '.' &&
                (de->d_name[1] == '\0' ||
                 (de->d_name[1] == '.' && de->d_name[2] == '\0'))) {
                continue;
            }
            count++;
        }

        /* Allocate array */
        req->dirents = zcalloc(count, sizeof(loopyFSDirent));
        if (!req->dirents && count > 0) {
            closedir(dir);
            req->result = -1;
            req->savedErrno = ENOMEM;
            fsSetError(req, "scandir");
            break;
        }

        /* Rewind and fill */
        rewinddir(dir);
        size_t i = 0;
        while ((de = readdir(dir)) != NULL && i < count) {
            if (de->d_name[0] == '.' &&
                (de->d_name[1] == '\0' ||
                 (de->d_name[1] == '.' && de->d_name[2] == '\0'))) {
                continue;
            }
            req->dirents[i].name = fsStrdup(de->d_name);
            req->dirents[i].type = de->d_type;
            i++;
        }
        closedir(dir);

        req->direntCount = i;
        req->result = (ssize_t)i;
        break;
    }

    case LOOPY_FS_CHOWN:
        req->result = chown(req->path, req->uid, req->gid);
        if (req->result < 0) {
            req->savedErrno = errno;
            fsSetError(req, "chown");
        }
        break;

    case LOOPY_FS_FCHOWN:
        req->result = fchown(req->fd, req->uid, req->gid);
        if (req->result < 0) {
            req->savedErrno = errno;
            fsSetError(req, "fchown");
        }
        break;

    case LOOPY_FS_LCHOWN:
        req->result = lchown(req->path, req->uid, req->gid);
        if (req->result < 0) {
            req->savedErrno = errno;
            fsSetError(req, "lchown");
        }
        break;

    case LOOPY_FS_UTIME:
    case LOOPY_FS_LUTIME: {
        struct timeval times[2];
        struct timeval *tp = NULL;

        if (req->atime.sec != LOOPY_FS_UTIME_OMIT ||
            req->mtime.sec != LOOPY_FS_UTIME_OMIT) {
            if (req->atime.sec == LOOPY_FS_UTIME_NOW) {
                gettimeofday(&times[0], NULL);
            } else if (req->atime.sec != LOOPY_FS_UTIME_OMIT) {
                times[0].tv_sec = req->atime.sec;
                times[0].tv_usec = req->atime.nsec / 1000;
            }
            if (req->mtime.sec == LOOPY_FS_UTIME_NOW) {
                gettimeofday(&times[1], NULL);
            } else if (req->mtime.sec != LOOPY_FS_UTIME_OMIT) {
                times[1].tv_sec = req->mtime.sec;
                times[1].tv_usec = req->mtime.nsec / 1000;
            }
            tp = times;
        }

        if (req->type == LOOPY_FS_LUTIME) {
            req->result = lutimes(req->path, tp);
        } else {
            req->result = utimes(req->path, tp);
        }
        if (req->result < 0) {
            req->savedErrno = errno;
            fsSetError(req,
                       req->type == LOOPY_FS_LUTIME ? "lutimes" : "utimes");
        }
        break;
    }

    case LOOPY_FS_FUTIME: {
        struct timeval times[2];
        struct timeval *tp = NULL;

        if (req->atime.sec != LOOPY_FS_UTIME_OMIT ||
            req->mtime.sec != LOOPY_FS_UTIME_OMIT) {
            if (req->atime.sec == LOOPY_FS_UTIME_NOW) {
                gettimeofday(&times[0], NULL);
            } else if (req->atime.sec != LOOPY_FS_UTIME_OMIT) {
                times[0].tv_sec = req->atime.sec;
                times[0].tv_usec = req->atime.nsec / 1000;
            }
            if (req->mtime.sec == LOOPY_FS_UTIME_NOW) {
                gettimeofday(&times[1], NULL);
            } else if (req->mtime.sec != LOOPY_FS_UTIME_OMIT) {
                times[1].tv_sec = req->mtime.sec;
                times[1].tv_usec = req->mtime.nsec / 1000;
            }
            tp = times;
        }

        req->result = futimes(req->fd, tp);
        if (req->result < 0) {
            req->savedErrno = errno;
            fsSetError(req, "futimes");
        }
        break;
    }

    case LOOPY_FS_COPYFILE: {
#ifdef __APPLE__
        /* Try clonefile first if requested */
        if (req->copyFlags & LOOPY_FS_COPY_FICLONE) {
            if (clonefile(req->path, req->newPath, 0) == 0) {
                req->result = 0;
                break;
            }
            if (req->copyFlags & LOOPY_FS_COPY_FICLONE_FORCE) {
                req->result = -1;
                req->savedErrno = errno;
                fsSetError(req, "clonefile");
                break;
            }
        }

        /* Use copyfile on macOS */
        copyfile_flags_t flags = COPYFILE_ALL;
        if (req->copyFlags & LOOPY_FS_COPY_EXCL) {
            flags |= COPYFILE_EXCL;
        }
        req->result = copyfile(req->path, req->newPath, NULL, flags);
        if (req->result < 0) {
            req->savedErrno = errno;
            fsSetError(req, "copyfile");
        }
#else
        /* Fallback: read/write copy */
        int srcFd = open(req->path, O_RDONLY);
        if (srcFd < 0) {
            req->result = -1;
            req->savedErrno = errno;
            fsSetError(req, "open source");
            break;
        }

        int dstFlags = O_WRONLY | O_CREAT | O_TRUNC;
        if (req->copyFlags & LOOPY_FS_COPY_EXCL) {
            dstFlags |= O_EXCL;
        }

        struct stat st;
        if (fstat(srcFd, &st) < 0) {
            close(srcFd);
            req->result = -1;
            req->savedErrno = errno;
            fsSetError(req, "fstat source");
            break;
        }

        int dstFd = open(req->newPath, dstFlags, st.st_mode);
        if (dstFd < 0) {
            close(srcFd);
            req->result = -1;
            req->savedErrno = errno;
            fsSetError(req, "open dest");
            break;
        }

        /* Copy data */
        char buf[65536];
        ssize_t n;
        req->result = 0;
        while ((n = read(srcFd, buf, sizeof(buf))) > 0) {
            ssize_t written = write(dstFd, buf, n);
            if (written != n) {
                req->result = -1;
                req->savedErrno = errno;
                fsSetError(req, "write");
                break;
            }
        }
        if (n < 0 && req->result == 0) {
            req->result = -1;
            req->savedErrno = errno;
            fsSetError(req, "read");
        }

        close(srcFd);
        close(dstFd);
#endif
        break;
    }

    case LOOPY_FS_SENDFILE: {
#ifdef __APPLE__
        off_t len = req->len;
        off_t off = req->offset;
        req->result = sendfile(req->fd, req->fd2, off, &len, NULL, 0);
        if (req->result == 0) {
            req->result = len; /* Return bytes transferred */
        } else {
            req->savedErrno = errno;
            fsSetError(req, "sendfile");
        }
#elif defined(__linux__)
        off_t off = req->offset;
        req->result = sendfile(req->fd2, req->fd,
                               req->offset >= 0 ? &off : NULL, req->len);
        if (req->result < 0) {
            req->savedErrno = errno;
            fsSetError(req, "sendfile");
        }
#else
        /* Fallback: read/write */
        if (req->offset >= 0) {
            if (lseek(req->fd, req->offset, SEEK_SET) < 0) {
                req->result = -1;
                req->savedErrno = errno;
                fsSetError(req, "lseek");
                break;
            }
        }

        char buf[65536];
        size_t remaining = req->len;
        ssize_t total = 0;

        while (remaining > 0) {
            size_t toRead = remaining < sizeof(buf) ? remaining : sizeof(buf);
            ssize_t n = read(req->fd, buf, toRead);
            if (n <= 0) {
                if (n < 0) {
                    req->savedErrno = errno;
                    fsSetError(req, "read");
                }
                break;
            }
            ssize_t written = write(req->fd2, buf, n);
            if (written != n) {
                req->savedErrno = errno;
                fsSetError(req, "write");
                break;
            }
            total += written;
            remaining -= n;
        }
        req->result = total;
#endif
        break;
    }

    case LOOPY_FS_MKDTEMP: {
        const char *result = mkdtemp(req->buf);
        if (result) {
            req->result = 0;
        } else {
            req->result = -1;
            req->savedErrno = errno;
            fsSetError(req, "mkdtemp");
        }
        break;
    }

    case LOOPY_FS_MKSTEMP: {
        req->result = mkstemp(req->buf);
        if (req->result < 0) {
            req->savedErrno = errno;
            fsSetError(req, "mkstemp");
        }
        break;
    }

    case LOOPY_FS_STATFS: {
        struct statfs sfs;
        req->result = statfs(req->path, &sfs);
        if (req->result < 0) {
            req->savedErrno = errno;
            fsSetError(req, "statfs");
        } else if (req->statfsbuf) {
            /* Convert to cross-platform structure */
            req->statfsbuf->type = sfs.f_type;
            req->statfsbuf->bsize = sfs.f_bsize;
            req->statfsbuf->blocks = sfs.f_blocks;
            req->statfsbuf->bfree = sfs.f_bfree;
            req->statfsbuf->bavail = sfs.f_bavail;
            req->statfsbuf->files = sfs.f_files;
            req->statfsbuf->ffree = sfs.f_ffree;
            memset(req->statfsbuf->spare, 0, sizeof(req->statfsbuf->spare));
        }
        break;
    }

    default:
        req->result = -1;
        req->savedErrno = EINVAL;
        snprintf(req->errorString, sizeof(req->errorString),
                 "unknown operation type: %d", req->type);
        break;
    }
}

#ifdef USE_IOURING
/* io_uring completion callback */
static void fsIoUringCompletionCallback(void *userData, int32_t result) {
    loopyFSRequest *req = userData;

    req->completed = true;
    req->result = result;

    /* For errors, io_uring returns negative errno values */
    if (result < 0) {
        req->savedErrno = -result;
        errno = req->savedErrno;
    }

    if (req->cb) {
        req->cb(req->loop, req, req->result, req->userData);
    }
}
#endif

static void fsAfterWorkCallback(loopyLoop *loop, loopyWork *work,
                                loopyWorkId workId, loopyWorkStatus status,
                                void *userData) {
    (void)work;
    (void)workId;
    loopyFSRequest *req = userData;

    req->completed = true;

    if (status == LOOPY_WORK_CANCELLED) {
        req->result = -1;
        req->savedErrno = ECANCELED;
    }

    /* Restore errno from worker thread */
    if (req->savedErrno) {
        errno = req->savedErrno;
    }

    if (req->cb) {
        req->cb(loop, req, req->result, req->userData);
    }
}

/* Run operation synchronously */
static void fsRunSync(loopyFSRequest *req) {
    fsWorkCallback(NULL, 0, req);
    req->completed = true;

    if (req->savedErrno) {
        errno = req->savedErrno;
    }
}

/* Run operation asynchronously via work queue */
static bool fsRunAsync(loopyFSRequest *req) {
#ifdef USE_IOURING
    /* Try io_uring first for supported operations */
    if (loopyUsingIoUring(req->loop)) {
        uint64_t opId = 0;

        switch (req->type) {
        case LOOPY_FS_READ:
            opId =
                loopyIoUringRead(req->loop, req->fd, req->buf, req->len,
                                 req->offset, fsIoUringCompletionCallback, req);
            break;

        case LOOPY_FS_WRITE:
            opId = loopyIoUringWrite(req->loop, req->fd, req->buf, req->len,
                                     req->offset, fsIoUringCompletionCallback,
                                     req);
            break;

        case LOOPY_FS_OPEN:
            opId =
                loopyIoUringOpenat(req->loop, AT_FDCWD, req->path, req->flags,
                                   req->mode, fsIoUringCompletionCallback, req);
            break;

        case LOOPY_FS_CLOSE:
            opId = loopyIoUringClose(req->loop, req->fd,
                                     fsIoUringCompletionCallback, req);
            break;

        case LOOPY_FS_FSYNC:
        case LOOPY_FS_FDATASYNC:
            opId = loopyIoUringFsync(req->loop, req->fd,
                                     req->type == LOOPY_FS_FDATASYNC,
                                     fsIoUringCompletionCallback, req);
            break;

        default:
            /* Operation not supported by io_uring, fall through to thread pool
             */
            break;
        }

        if (opId != 0) {
            /* io_uring operation submitted successfully */
            req->ioUringOpId = opId;
            req->usingIoUring = true;
            return true;
        }
        /* If io_uring submission failed, fall through to thread pool */
    }
#endif

    /* Fall back to thread pool for unsupported operations or if io_uring
     * unavailable */
    req->work = loopyWorkNew(req->loop, NULL);
    if (!req->work) {
        return false;
    }

    req->workId =
        loopyWorkQueue(req->work, fsWorkCallback, fsAfterWorkCallback, req);
    if (req->workId == 0) {
        return false;
    }

#ifdef USE_IOURING
    req->usingIoUring = false;
#endif

    return true;
}

/* ====================================================================
 * File Operations Implementation
 * ==================================================================== */

loopyFSRequest *loopyFSOpen(loopyLoop *loop, const char *path, int flags,
                            int mode, loopyFSCallback *cb, void *userData) {
    if (!path) {
        return NULL;
    }

    loopyFSRequest *req = fsRequestNew(loop, LOOPY_FS_OPEN, cb, userData);
    if (!req) {
        return NULL;
    }

    req->path = fsStrdup(path);
    if (!req->path) {
        zfree(req);
        return NULL;
    }
    req->flags = flags;
    req->mode = mode;

    if (cb && loop) {
        if (!fsRunAsync(req)) {
            zfree(req->path);
            zfree(req);
            return NULL;
        }
    } else {
        fsRunSync(req);
    }

    return req;
}

loopyFSRequest *loopyFSClose(loopyLoop *loop, int fd, loopyFSCallback *cb,
                             void *userData) {
    if (fd < 0) {
        return NULL;
    }

    loopyFSRequest *req = fsRequestNew(loop, LOOPY_FS_CLOSE, cb, userData);
    if (!req) {
        return NULL;
    }

    req->fd = fd;

    if (cb && loop) {
        if (!fsRunAsync(req)) {
            zfree(req);
            return NULL;
        }
    } else {
        fsRunSync(req);
    }

    return req;
}

loopyFSRequest *loopyFSRead(loopyLoop *loop, int fd, void *buf, size_t len,
                            off_t offset, loopyFSCallback *cb, void *userData) {
    if (fd < 0 || !buf || len == 0) {
        return NULL;
    }

    loopyFSRequest *req = fsRequestNew(loop, LOOPY_FS_READ, cb, userData);
    if (!req) {
        return NULL;
    }

    req->fd = fd;
    req->buf = buf;
    req->len = len;
    req->offset = offset;

    if (cb && loop) {
        if (!fsRunAsync(req)) {
            zfree(req);
            return NULL;
        }
    } else {
        fsRunSync(req);
    }

    return req;
}

loopyFSRequest *loopyFSWrite(loopyLoop *loop, int fd, const void *buf,
                             size_t len, off_t offset, loopyFSCallback *cb,
                             void *userData) {
    if (fd < 0 || !buf || len == 0) {
        return NULL;
    }

    loopyFSRequest *req = fsRequestNew(loop, LOOPY_FS_WRITE, cb, userData);
    if (!req) {
        return NULL;
    }

    req->fd = fd;
    req->buf = (void *)buf; /* Cast away const for storage */
    req->len = len;
    req->offset = offset;

    if (cb && loop) {
        if (!fsRunAsync(req)) {
            zfree(req);
            return NULL;
        }
    } else {
        fsRunSync(req);
    }

    return req;
}

loopyFSRequest *loopyFSStat(loopyLoop *loop, const char *path,
                            struct stat *statbuf, loopyFSCallback *cb,
                            void *userData) {
    if (!path || !statbuf) {
        return NULL;
    }

    loopyFSRequest *req = fsRequestNew(loop, LOOPY_FS_STAT, cb, userData);
    if (!req) {
        return NULL;
    }

    req->path = fsStrdup(path);
    if (!req->path) {
        zfree(req);
        return NULL;
    }
    req->statbuf = statbuf;

    if (cb && loop) {
        if (!fsRunAsync(req)) {
            zfree(req->path);
            zfree(req);
            return NULL;
        }
    } else {
        fsRunSync(req);
    }

    return req;
}

loopyFSRequest *loopyFSFstat(loopyLoop *loop, int fd, struct stat *statbuf,
                             loopyFSCallback *cb, void *userData) {
    if (fd < 0 || !statbuf) {
        return NULL;
    }

    loopyFSRequest *req = fsRequestNew(loop, LOOPY_FS_FSTAT, cb, userData);
    if (!req) {
        return NULL;
    }

    req->fd = fd;
    req->statbuf = statbuf;

    if (cb && loop) {
        if (!fsRunAsync(req)) {
            zfree(req);
            return NULL;
        }
    } else {
        fsRunSync(req);
    }

    return req;
}

loopyFSRequest *loopyFSLstat(loopyLoop *loop, const char *path,
                             struct stat *statbuf, loopyFSCallback *cb,
                             void *userData) {
    if (!path || !statbuf) {
        return NULL;
    }

    loopyFSRequest *req = fsRequestNew(loop, LOOPY_FS_LSTAT, cb, userData);
    if (!req) {
        return NULL;
    }

    req->path = fsStrdup(path);
    if (!req->path) {
        zfree(req);
        return NULL;
    }
    req->statbuf = statbuf;

    if (cb && loop) {
        if (!fsRunAsync(req)) {
            zfree(req->path);
            zfree(req);
            return NULL;
        }
    } else {
        fsRunSync(req);
    }

    return req;
}

loopyFSRequest *loopyFSUnlink(loopyLoop *loop, const char *path,
                              loopyFSCallback *cb, void *userData) {
    if (!path) {
        return NULL;
    }

    loopyFSRequest *req = fsRequestNew(loop, LOOPY_FS_UNLINK, cb, userData);
    if (!req) {
        return NULL;
    }

    req->path = fsStrdup(path);
    if (!req->path) {
        zfree(req);
        return NULL;
    }

    if (cb && loop) {
        if (!fsRunAsync(req)) {
            zfree(req->path);
            zfree(req);
            return NULL;
        }
    } else {
        fsRunSync(req);
    }

    return req;
}

loopyFSRequest *loopyFSMkdir(loopyLoop *loop, const char *path, int mode,
                             loopyFSCallback *cb, void *userData) {
    if (!path) {
        return NULL;
    }

    loopyFSRequest *req = fsRequestNew(loop, LOOPY_FS_MKDIR, cb, userData);
    if (!req) {
        return NULL;
    }

    req->path = fsStrdup(path);
    if (!req->path) {
        zfree(req);
        return NULL;
    }
    req->mode = mode;

    if (cb && loop) {
        if (!fsRunAsync(req)) {
            zfree(req->path);
            zfree(req);
            return NULL;
        }
    } else {
        fsRunSync(req);
    }

    return req;
}

loopyFSRequest *loopyFSRmdir(loopyLoop *loop, const char *path,
                             loopyFSCallback *cb, void *userData) {
    if (!path) {
        return NULL;
    }

    loopyFSRequest *req = fsRequestNew(loop, LOOPY_FS_RMDIR, cb, userData);
    if (!req) {
        return NULL;
    }

    req->path = fsStrdup(path);
    if (!req->path) {
        zfree(req);
        return NULL;
    }

    if (cb && loop) {
        if (!fsRunAsync(req)) {
            zfree(req->path);
            zfree(req);
            return NULL;
        }
    } else {
        fsRunSync(req);
    }

    return req;
}

loopyFSRequest *loopyFSRename(loopyLoop *loop, const char *path,
                              const char *newPath, loopyFSCallback *cb,
                              void *userData) {
    if (!path || !newPath) {
        return NULL;
    }

    loopyFSRequest *req = fsRequestNew(loop, LOOPY_FS_RENAME, cb, userData);
    if (!req) {
        return NULL;
    }

    req->path = fsStrdup(path);
    req->newPath = fsStrdup(newPath);
    if (!req->path || !req->newPath) {
        zfree(req->path);
        zfree(req->newPath);
        zfree(req);
        return NULL;
    }

    if (cb && loop) {
        if (!fsRunAsync(req)) {
            zfree(req->path);
            zfree(req->newPath);
            zfree(req);
            return NULL;
        }
    } else {
        fsRunSync(req);
    }

    return req;
}

loopyFSRequest *loopyFSFsync(loopyLoop *loop, int fd, loopyFSCallback *cb,
                             void *userData) {
    if (fd < 0) {
        return NULL;
    }

    loopyFSRequest *req = fsRequestNew(loop, LOOPY_FS_FSYNC, cb, userData);
    if (!req) {
        return NULL;
    }

    req->fd = fd;

    if (cb && loop) {
        if (!fsRunAsync(req)) {
            zfree(req);
            return NULL;
        }
    } else {
        fsRunSync(req);
    }

    return req;
}

loopyFSRequest *loopyFSFdatasync(loopyLoop *loop, int fd, loopyFSCallback *cb,
                                 void *userData) {
    if (fd < 0) {
        return NULL;
    }

    loopyFSRequest *req = fsRequestNew(loop, LOOPY_FS_FDATASYNC, cb, userData);
    if (!req) {
        return NULL;
    }

    req->fd = fd;

    if (cb && loop) {
        if (!fsRunAsync(req)) {
            zfree(req);
            return NULL;
        }
    } else {
        fsRunSync(req);
    }

    return req;
}

loopyFSRequest *loopyFSFtruncate(loopyLoop *loop, int fd, off_t length,
                                 loopyFSCallback *cb, void *userData) {
    if (fd < 0) {
        return NULL;
    }

    loopyFSRequest *req = fsRequestNew(loop, LOOPY_FS_FTRUNCATE, cb, userData);
    if (!req) {
        return NULL;
    }

    req->fd = fd;
    req->offset = length; /* Reuse offset field for length */

    if (cb && loop) {
        if (!fsRunAsync(req)) {
            zfree(req);
            return NULL;
        }
    } else {
        fsRunSync(req);
    }

    return req;
}

loopyFSRequest *loopyFSChmod(loopyLoop *loop, const char *path, int mode,
                             loopyFSCallback *cb, void *userData) {
    if (!path) {
        return NULL;
    }

    loopyFSRequest *req = fsRequestNew(loop, LOOPY_FS_CHMOD, cb, userData);
    if (!req) {
        return NULL;
    }

    req->path = fsStrdup(path);
    if (!req->path) {
        zfree(req);
        return NULL;
    }
    req->mode = mode;

    if (cb && loop) {
        if (!fsRunAsync(req)) {
            zfree(req->path);
            zfree(req);
            return NULL;
        }
    } else {
        fsRunSync(req);
    }

    return req;
}

loopyFSRequest *loopyFSFchmod(loopyLoop *loop, int fd, int mode,
                              loopyFSCallback *cb, void *userData) {
    if (fd < 0) {
        return NULL;
    }

    loopyFSRequest *req = fsRequestNew(loop, LOOPY_FS_FCHMOD, cb, userData);
    if (!req) {
        return NULL;
    }

    req->fd = fd;
    req->mode = mode;

    if (cb && loop) {
        if (!fsRunAsync(req)) {
            zfree(req);
            return NULL;
        }
    } else {
        fsRunSync(req);
    }

    return req;
}

/* ====================================================================
 * Extended File Operations Implementation
 * ==================================================================== */

loopyFSRequest *loopyFSLink(loopyLoop *loop, const char *path,
                            const char *newPath, loopyFSCallback *cb,
                            void *userData) {
    if (!path || !newPath) {
        return NULL;
    }

    loopyFSRequest *req = fsRequestNew(loop, LOOPY_FS_LINK, cb, userData);
    if (!req) {
        return NULL;
    }

    req->path = fsStrdup(path);
    req->newPath = fsStrdup(newPath);
    if (!req->path || !req->newPath) {
        zfree(req->path);
        zfree(req->newPath);
        zfree(req);
        return NULL;
    }

    if (cb && loop) {
        if (!fsRunAsync(req)) {
            zfree(req->path);
            zfree(req->newPath);
            zfree(req);
            return NULL;
        }
    } else {
        fsRunSync(req);
    }

    return req;
}

loopyFSRequest *loopyFSSymlink(loopyLoop *loop, const char *target,
                               const char *linkPath, loopyFSCallback *cb,
                               void *userData) {
    if (!target || !linkPath) {
        return NULL;
    }

    loopyFSRequest *req = fsRequestNew(loop, LOOPY_FS_SYMLINK, cb, userData);
    if (!req) {
        return NULL;
    }

    req->path = fsStrdup(target);
    req->newPath = fsStrdup(linkPath);
    if (!req->path || !req->newPath) {
        zfree(req->path);
        zfree(req->newPath);
        zfree(req);
        return NULL;
    }

    if (cb && loop) {
        if (!fsRunAsync(req)) {
            zfree(req->path);
            zfree(req->newPath);
            zfree(req);
            return NULL;
        }
    } else {
        fsRunSync(req);
    }

    return req;
}

loopyFSRequest *loopyFSReadlink(loopyLoop *loop, const char *path, char *buf,
                                size_t bufLen, loopyFSCallback *cb,
                                void *userData) {
    if (!path || !buf || bufLen == 0) {
        return NULL;
    }

    loopyFSRequest *req = fsRequestNew(loop, LOOPY_FS_READLINK, cb, userData);
    if (!req) {
        return NULL;
    }

    req->path = fsStrdup(path);
    if (!req->path) {
        zfree(req);
        return NULL;
    }
    req->buf = buf;
    req->len = bufLen;

    if (cb && loop) {
        if (!fsRunAsync(req)) {
            zfree(req->path);
            zfree(req);
            return NULL;
        }
    } else {
        fsRunSync(req);
    }

    return req;
}

loopyFSRequest *loopyFSRealpath(loopyLoop *loop, const char *path, char *buf,
                                size_t bufLen, loopyFSCallback *cb,
                                void *userData) {
    if (!path || !buf || bufLen == 0) {
        return NULL;
    }

    loopyFSRequest *req = fsRequestNew(loop, LOOPY_FS_REALPATH, cb, userData);
    if (!req) {
        return NULL;
    }

    req->path = fsStrdup(path);
    if (!req->path) {
        zfree(req);
        return NULL;
    }
    req->buf = buf;
    req->len = bufLen;

    if (cb && loop) {
        if (!fsRunAsync(req)) {
            zfree(req->path);
            zfree(req);
            return NULL;
        }
    } else {
        fsRunSync(req);
    }

    return req;
}

loopyFSRequest *loopyFSAccess(loopyLoop *loop, const char *path, int mode,
                              loopyFSCallback *cb, void *userData) {
    if (!path) {
        return NULL;
    }

    loopyFSRequest *req = fsRequestNew(loop, LOOPY_FS_ACCESS, cb, userData);
    if (!req) {
        return NULL;
    }

    req->path = fsStrdup(path);
    if (!req->path) {
        zfree(req);
        return NULL;
    }
    req->mode = mode;

    if (cb && loop) {
        if (!fsRunAsync(req)) {
            zfree(req->path);
            zfree(req);
            return NULL;
        }
    } else {
        fsRunSync(req);
    }

    return req;
}

loopyFSRequest *loopyFSScandir(loopyLoop *loop, const char *path,
                               loopyFSCallback *cb, void *userData) {
    if (!path) {
        return NULL;
    }

    loopyFSRequest *req = fsRequestNew(loop, LOOPY_FS_SCANDIR, cb, userData);
    if (!req) {
        return NULL;
    }

    req->path = fsStrdup(path);
    if (!req->path) {
        zfree(req);
        return NULL;
    }

    if (cb && loop) {
        if (!fsRunAsync(req)) {
            zfree(req->path);
            zfree(req);
            return NULL;
        }
    } else {
        fsRunSync(req);
    }

    return req;
}

loopyFSRequest *loopyFSChown(loopyLoop *loop, const char *path, uid_t uid,
                             gid_t gid, loopyFSCallback *cb, void *userData) {
    if (!path) {
        return NULL;
    }

    loopyFSRequest *req = fsRequestNew(loop, LOOPY_FS_CHOWN, cb, userData);
    if (!req) {
        return NULL;
    }

    req->path = fsStrdup(path);
    if (!req->path) {
        zfree(req);
        return NULL;
    }
    req->uid = uid;
    req->gid = gid;

    if (cb && loop) {
        if (!fsRunAsync(req)) {
            zfree(req->path);
            zfree(req);
            return NULL;
        }
    } else {
        fsRunSync(req);
    }

    return req;
}

loopyFSRequest *loopyFSFchown(loopyLoop *loop, int fd, uid_t uid, gid_t gid,
                              loopyFSCallback *cb, void *userData) {
    if (fd < 0) {
        return NULL;
    }

    loopyFSRequest *req = fsRequestNew(loop, LOOPY_FS_FCHOWN, cb, userData);
    if (!req) {
        return NULL;
    }

    req->fd = fd;
    req->uid = uid;
    req->gid = gid;

    if (cb && loop) {
        if (!fsRunAsync(req)) {
            zfree(req);
            return NULL;
        }
    } else {
        fsRunSync(req);
    }

    return req;
}

loopyFSRequest *loopyFSLchown(loopyLoop *loop, const char *path, uid_t uid,
                              gid_t gid, loopyFSCallback *cb, void *userData) {
    if (!path) {
        return NULL;
    }

    loopyFSRequest *req = fsRequestNew(loop, LOOPY_FS_LCHOWN, cb, userData);
    if (!req) {
        return NULL;
    }

    req->path = fsStrdup(path);
    if (!req->path) {
        zfree(req);
        return NULL;
    }
    req->uid = uid;
    req->gid = gid;

    if (cb && loop) {
        if (!fsRunAsync(req)) {
            zfree(req->path);
            zfree(req);
            return NULL;
        }
    } else {
        fsRunSync(req);
    }

    return req;
}

loopyFSRequest *loopyFSUtime(loopyLoop *loop, const char *path,
                             const loopyFSTimespec *atime,
                             const loopyFSTimespec *mtime, loopyFSCallback *cb,
                             void *userData) {
    if (!path) {
        return NULL;
    }

    loopyFSRequest *req = fsRequestNew(loop, LOOPY_FS_UTIME, cb, userData);
    if (!req) {
        return NULL;
    }

    req->path = fsStrdup(path);
    if (!req->path) {
        zfree(req);
        return NULL;
    }
    if (atime) {
        req->atime = *atime;
    } else {
        req->atime.sec = LOOPY_FS_UTIME_NOW;
    }
    if (mtime) {
        req->mtime = *mtime;
    } else {
        req->mtime.sec = LOOPY_FS_UTIME_NOW;
    }

    if (cb && loop) {
        if (!fsRunAsync(req)) {
            zfree(req->path);
            zfree(req);
            return NULL;
        }
    } else {
        fsRunSync(req);
    }

    return req;
}

loopyFSRequest *loopyFSFutime(loopyLoop *loop, int fd,
                              const loopyFSTimespec *atime,
                              const loopyFSTimespec *mtime, loopyFSCallback *cb,
                              void *userData) {
    if (fd < 0) {
        return NULL;
    }

    loopyFSRequest *req = fsRequestNew(loop, LOOPY_FS_FUTIME, cb, userData);
    if (!req) {
        return NULL;
    }

    req->fd = fd;
    if (atime) {
        req->atime = *atime;
    } else {
        req->atime.sec = LOOPY_FS_UTIME_NOW;
    }
    if (mtime) {
        req->mtime = *mtime;
    } else {
        req->mtime.sec = LOOPY_FS_UTIME_NOW;
    }

    if (cb && loop) {
        if (!fsRunAsync(req)) {
            zfree(req);
            return NULL;
        }
    } else {
        fsRunSync(req);
    }

    return req;
}

loopyFSRequest *loopyFSLutime(loopyLoop *loop, const char *path,
                              const loopyFSTimespec *atime,
                              const loopyFSTimespec *mtime, loopyFSCallback *cb,
                              void *userData) {
    if (!path) {
        return NULL;
    }

    loopyFSRequest *req = fsRequestNew(loop, LOOPY_FS_LUTIME, cb, userData);
    if (!req) {
        return NULL;
    }

    req->path = fsStrdup(path);
    if (!req->path) {
        zfree(req);
        return NULL;
    }
    if (atime) {
        req->atime = *atime;
    } else {
        req->atime.sec = LOOPY_FS_UTIME_NOW;
    }
    if (mtime) {
        req->mtime = *mtime;
    } else {
        req->mtime.sec = LOOPY_FS_UTIME_NOW;
    }

    if (cb && loop) {
        if (!fsRunAsync(req)) {
            zfree(req->path);
            zfree(req);
            return NULL;
        }
    } else {
        fsRunSync(req);
    }

    return req;
}

loopyFSRequest *loopyFSCopyfile(loopyLoop *loop, const char *srcPath,
                                const char *dstPath, loopyFSCopyFlags flags,
                                loopyFSCallback *cb, void *userData) {
    if (!srcPath || !dstPath) {
        return NULL;
    }

    loopyFSRequest *req = fsRequestNew(loop, LOOPY_FS_COPYFILE, cb, userData);
    if (!req) {
        return NULL;
    }

    req->path = fsStrdup(srcPath);
    req->newPath = fsStrdup(dstPath);
    if (!req->path || !req->newPath) {
        zfree(req->path);
        zfree(req->newPath);
        zfree(req);
        return NULL;
    }
    req->copyFlags = flags;

    if (cb && loop) {
        if (!fsRunAsync(req)) {
            zfree(req->path);
            zfree(req->newPath);
            zfree(req);
            return NULL;
        }
    } else {
        fsRunSync(req);
    }

    return req;
}

loopyFSRequest *loopyFSSendfile(loopyLoop *loop, int outFd, int inFd,
                                off_t offset, size_t len, loopyFSCallback *cb,
                                void *userData) {
    if (outFd < 0 || inFd < 0) {
        return NULL;
    }

    loopyFSRequest *req = fsRequestNew(loop, LOOPY_FS_SENDFILE, cb, userData);
    if (!req) {
        return NULL;
    }

    req->fd = inFd;
    req->fd2 = outFd;
    req->offset = offset;
    req->len = len;

    if (cb && loop) {
        if (!fsRunAsync(req)) {
            zfree(req);
            return NULL;
        }
    } else {
        fsRunSync(req);
    }

    return req;
}

loopyFSRequest *loopyFSMkdtemp(loopyLoop *loop, char *tpl, loopyFSCallback *cb,
                               void *userData) {
    if (!tpl) {
        return NULL;
    }

    loopyFSRequest *req = fsRequestNew(loop, LOOPY_FS_MKDTEMP, cb, userData);
    if (!req) {
        return NULL;
    }

    req->buf = tpl;

    if (cb && loop) {
        if (!fsRunAsync(req)) {
            zfree(req);
            return NULL;
        }
    } else {
        fsRunSync(req);
    }

    return req;
}

loopyFSRequest *loopyFSMkstemp(loopyLoop *loop, char *tpl, loopyFSCallback *cb,
                               void *userData) {
    if (!tpl) {
        return NULL;
    }

    loopyFSRequest *req = fsRequestNew(loop, LOOPY_FS_MKSTEMP, cb, userData);
    if (!req) {
        return NULL;
    }

    req->buf = tpl;

    if (cb && loop) {
        if (!fsRunAsync(req)) {
            zfree(req);
            return NULL;
        }
    } else {
        fsRunSync(req);
    }

    return req;
}

/* ====================================================================
 * Request Management
 * ==================================================================== */

void loopyFSRequestFree(loopyFSRequest *req) {
    if (!req) {
        return;
    }

    zfree(req->path);
    zfree(req->newPath);

    /* Free scandir results */
    if (req->dirents) {
        for (size_t i = 0; i < req->direntCount; i++) {
            zfree(req->dirents[i].name);
        }
        zfree(req->dirents);
    }

    zfree(req);
}

bool loopyFSCancel(loopyFSRequest *req) {
    if (!req || req->completed) {
        return false;
    }

    req->cancelled = true;

    if (req->work && req->workId) {
        return loopyWorkCancel(req->work, req->workId);
    }

    return true;
}

/* ====================================================================
 * Request Properties
 * ==================================================================== */

loopyFSType loopyFSRequestGetType(const loopyFSRequest *req) {
    if (!req) {
        return LOOPY_FS_UNKNOWN;
    }
    return req->type;
}

const char *loopyFSRequestGetPath(const loopyFSRequest *req) {
    if (!req) {
        return NULL;
    }
    return req->path;
}

ssize_t loopyFSRequestGetResult(const loopyFSRequest *req) {
    if (!req) {
        return -1;
    }
    return req->result;
}

loopyLoop *loopyFSRequestGetLoop(const loopyFSRequest *req) {
    if (!req) {
        return NULL;
    }
    return req->loop;
}

const struct stat *loopyFSRequestGetStatbuf(const loopyFSRequest *req) {
    if (!req) {
        return NULL;
    }
    return req->statbuf;
}

const loopyFSDirent *loopyFSRequestGetDirents(const loopyFSRequest *req,
                                              size_t *count) {
    if (!req || req->type != LOOPY_FS_SCANDIR) {
        if (count) {
            *count = 0;
        }
        return NULL;
    }
    if (count) {
        *count = req->direntCount;
    }
    return req->dirents;
}

const char *loopyFSGetError(const loopyFSRequest *req) {
    if (!req) {
        return "";
    }
    return req->errorString;
}

/* ====================================================================
 * Filesystem Statistics
 * ==================================================================== */

loopyFSRequest *loopyFSStatfs(loopyLoop *loop, const char *path,
                              loopyStatfs *buf, loopyFSCallback *cb,
                              void *userData) {
    if (!path || !buf) {
        return NULL;
    }

    loopyFSRequest *req = fsRequestNew(loop, LOOPY_FS_STATFS, cb, userData);
    if (!req) {
        return NULL;
    }

    req->path = fsStrdup(path);
    if (!req->path) {
        zfree(req);
        return NULL;
    }
    req->statfsbuf = buf;

    if (cb && loop) {
        if (!fsRunAsync(req)) {
            zfree(req->path);
            zfree(req);
            return NULL;
        }
    } else {
        fsRunSync(req);
    }

    return req;
}

const loopyStatfs *loopyFSRequestGetStatfsBuf(const loopyFSRequest *req) {
    if (!req || req->type != LOOPY_FS_STATFS) {
        return NULL;
    }
    return req->statfsbuf;
}

/* ====================================================================
 * Direct I/O Support
 * ==================================================================== */

void *loopyFSAllocAligned(size_t size, size_t alignment) {
    if (size == 0 || alignment == 0) {
        return NULL;
    }

    void *ptr = NULL;

#ifdef _POSIX_VERSION
    /* Use posix_memalign() for aligned allocation */
    if (posix_memalign(&ptr, alignment, size) != 0) {
        return NULL;
    }
#else
    /* Fallback: allocate extra space and manually align */
    void *raw = zmalloc(size + alignment + sizeof(void *));
    if (!raw) {
        return NULL;
    }

    /* Calculate aligned address */
    uintptr_t addr = (uintptr_t)raw + sizeof(void *);
    uintptr_t aligned = (addr + alignment - 1) & ~(alignment - 1);

    /* Store original pointer before aligned address */
    ptr = (void *)aligned;
    *((void **)(aligned - sizeof(void *))) = raw;
#endif

    return ptr;
}

void loopyFSFreeAligned(void *ptr) {
    if (!ptr) {
        return;
    }

#ifdef _POSIX_VERSION
    /* posix_memalign() memory can be freed with regular free() */
    zfree(ptr);
#else
    /* Retrieve original pointer stored before aligned address */
    void *raw = *((void **)((uintptr_t)ptr - sizeof(void *)));
    zfree(raw);
#endif
}

size_t loopyFSDirectAlignment(void) {
    /* Return typical sector size for O_DIRECT */
    return 512;
}
