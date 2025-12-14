/* loopyPipe - Pipe Creation and Management Implementation
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

#include "loopy.h"
#include "loopyPipe.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../deps/datakit/src/datakit.h"

/* ====================================================================
 * Internal Structure
 * ==================================================================== */

/**
 * Pipe handle internal structure.
 */
struct loopyPipe {
    int readFd;      /* Read end file descriptor (-1 if closed) */
    int writeFd;     /* Write end file descriptor (-1 if closed) */
    loopyLoop *loop; /* Event loop (NULL if not registered) */
    void *userData;  /* User data for handle accessors */

    /* Async read state */
    loopyPipeReadCallback *readCallback;
    void *readUserData;
    void *readBuffer;
    size_t readBufferSize;

    /* Async write state */
    loopyPipeWriteCallback *writeCallback;
    void *writeUserData;
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

const char *loopyPipeGetError(void) {
    return lastError[0] ? lastError : NULL;
}

/* ====================================================================
 * Internal Helper Functions
 * ==================================================================== */

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

static void closeFd(int *fd) {
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

/* ====================================================================
 * Pipe Creation
 * ==================================================================== */

loopyPipe *loopyPipeCreate(loopyPipeFlags flags) {
    int fds[2];

#ifdef __linux__
    /* Use pipe2 on Linux for atomic flag setting */
    int pipeFlags = 0;
    if (flags & LOOPY_PIPE_NONBLOCK) {
        pipeFlags |= O_NONBLOCK;
    }
    if (flags & LOOPY_PIPE_CLOEXEC) {
        pipeFlags |= O_CLOEXEC;
    }

    if (pipe2(fds, pipeFlags) == -1) {
        setErrorErrno("pipe2");
        return NULL;
    }
#else
    /* Use pipe + fcntl on other platforms */
    if (pipe(fds) == -1) {
        setErrorErrno("pipe");
        return NULL;
    }

    if (flags & LOOPY_PIPE_NONBLOCK) {
        if (!setNonBlocking(fds[0]) || !setNonBlocking(fds[1])) {
            setErrorErrno("fcntl O_NONBLOCK");
            close(fds[0]);
            close(fds[1]);
            return NULL;
        }
    }

    if (flags & LOOPY_PIPE_CLOEXEC) {
        if (!setCloseOnExec(fds[0]) || !setCloseOnExec(fds[1])) {
            setErrorErrno("fcntl FD_CLOEXEC");
            close(fds[0]);
            close(fds[1]);
            return NULL;
        }
    }
#endif

    loopyPipe *pipe = zcalloc(1, sizeof(loopyPipe));
    if (!pipe) {
        setError("Memory allocation failed");
        close(fds[0]);
        close(fds[1]);
        return NULL;
    }

    pipe->readFd = fds[0];
    pipe->writeFd = fds[1];
    pipe->loop = NULL;
    pipe->readCallback = NULL;
    pipe->writeCallback = NULL;

    lastError[0] = '\0';
    return pipe;
}

loopyPipe *loopyPipeFromFds(int readFd, int writeFd) {
    if (readFd < 0 || writeFd < 0) {
        setError("Invalid file descriptors");
        return NULL;
    }

    loopyPipe *pipe = zcalloc(1, sizeof(loopyPipe));
    if (!pipe) {
        setError("Memory allocation failed");
        return NULL;
    }

    pipe->readFd = readFd;
    pipe->writeFd = writeFd;
    pipe->loop = NULL;
    pipe->readCallback = NULL;
    pipe->writeCallback = NULL;

    lastError[0] = '\0';
    return pipe;
}

bool loopyPipeMakeFifo(const char *path, mode_t mode) {
    if (!path) {
        setError("NULL path");
        return false;
    }

    if (mkfifo(path, mode) == -1) {
        if (errno != EEXIST) {
            setErrorErrno("mkfifo");
            return false;
        }
    }

    lastError[0] = '\0';
    return true;
}

loopyPipe *loopyPipeOpenFifo(const char *path, bool write, bool nonblock) {
    if (!path) {
        setError("NULL path");
        return NULL;
    }

    int flags = write ? O_WRONLY : O_RDONLY;
    if (nonblock) {
        flags |= O_NONBLOCK;
    }

    int fd = open(path, flags);
    if (fd == -1) {
        setErrorErrno("open");
        return NULL;
    }

    loopyPipe *pipe = zcalloc(1, sizeof(loopyPipe));
    if (!pipe) {
        setError("Memory allocation failed");
        close(fd);
        return NULL;
    }

    if (write) {
        pipe->readFd = -1;
        pipe->writeFd = fd;
    } else {
        pipe->readFd = fd;
        pipe->writeFd = -1;
    }

    pipe->loop = NULL;
    pipe->readCallback = NULL;
    pipe->writeCallback = NULL;

    lastError[0] = '\0';
    return pipe;
}

bool loopyPipeRemoveFifo(const char *path) {
    if (!path) {
        setError("NULL path");
        return false;
    }

    if (unlink(path) == -1) {
        setErrorErrno("unlink");
        return false;
    }

    lastError[0] = '\0';
    return true;
}

/* ====================================================================
 * Pipe Operations
 * ==================================================================== */

int loopyPipeGetReadFd(const loopyPipe *pipe) {
    return pipe ? pipe->readFd : -1;
}

int loopyPipeGetWriteFd(const loopyPipe *pipe) {
    return pipe ? pipe->writeFd : -1;
}

ssize_t loopyPipeRead(loopyPipe *pipe, void *buf, size_t count) {
    if (!pipe || pipe->readFd < 0) {
        setError("Invalid pipe or read end closed");
        errno = EBADF;
        return -1;
    }

    if (!buf) {
        setError("NULL buffer");
        errno = EINVAL;
        return -1;
    }

    ssize_t nread;
    do {
        nread = read(pipe->readFd, buf, count);
    } while (nread == -1 && errno == EINTR);

    if (nread == -1) {
        setErrorErrno("read");
    } else {
        lastError[0] = '\0';
    }

    return nread;
}

ssize_t loopyPipeWrite(loopyPipe *pipe, const void *buf, size_t count) {
    if (!pipe || pipe->writeFd < 0) {
        setError("Invalid pipe or write end closed");
        errno = EBADF;
        return -1;
    }

    if (!buf) {
        setError("NULL buffer");
        errno = EINVAL;
        return -1;
    }

    ssize_t nwritten;
    do {
        nwritten = write(pipe->writeFd, buf, count);
    } while (nwritten == -1 && errno == EINTR);

    if (nwritten == -1) {
        setErrorErrno("write");
    } else {
        lastError[0] = '\0';
    }

    return nwritten;
}

void loopyPipeCloseRead(loopyPipe *pipe) {
    if (!pipe) {
        return;
    }

    /* Unregister from event loop if registered */
    if (pipe->loop && pipe->readFd >= 0) {
        loopyUnregisterReadWrite(pipe->loop, pipe->readFd);
    }

    closeFd(&pipe->readFd);

    if (pipe->readBuffer) {
        zfree(pipe->readBuffer);
        pipe->readBuffer = NULL;
        pipe->readBufferSize = 0;
    }

    pipe->readCallback = NULL;
    pipe->readUserData = NULL;
}

void loopyPipeCloseWrite(loopyPipe *pipe) {
    if (!pipe) {
        return;
    }

    /* Unregister from event loop if registered */
    if (pipe->loop && pipe->writeFd >= 0) {
        loopyUnregisterReadWrite(pipe->loop, pipe->writeFd);
    }

    closeFd(&pipe->writeFd);

    pipe->writeCallback = NULL;
    pipe->writeUserData = NULL;
}

void loopyPipeClose(loopyPipe *pipe) {
    if (!pipe) {
        return;
    }

    loopyPipeCloseRead(pipe);
    loopyPipeCloseWrite(pipe);
    zfree(pipe);
}

/* ====================================================================
 * Event Loop Integration
 * ==================================================================== */

/* Forward declaration of read event callback */
static void pipeReadEventCallback(loopyLoop *l, int fd, void *clientData,
                                  loopyAction mask);

bool loopyPipeReadStart(loopyPipe *pipe, struct loopyLoop *loop,
                        loopyPipeReadCallback *cb, void *userData) {
    if (!pipe || !loop || !cb) {
        setError("NULL pipe, loop, or callback");
        return false;
    }

    if (pipe->readFd < 0) {
        setError("Read end is closed");
        return false;
    }

    /* Allocate read buffer (default 4KB) */
    if (!pipe->readBuffer) {
        pipe->readBufferSize = 4096;
        pipe->readBuffer = zmalloc(pipe->readBufferSize);
        if (!pipe->readBuffer) {
            setError("Failed to allocate read buffer");
            return false;
        }
    }

    pipe->loop = loop;
    pipe->readCallback = cb;
    pipe->readUserData = userData;

    if (!loopyRegisterRead(loop, pipe->readFd, pipeReadEventCallback, pipe)) {
        setError("Failed to register with event loop");
        zfree(pipe->readBuffer);
        pipe->readBuffer = NULL;
        pipe->readBufferSize = 0;
        pipe->loop = NULL;
        pipe->readCallback = NULL;
        pipe->readUserData = NULL;
        return false;
    }

    lastError[0] = '\0';
    return true;
}

void loopyPipeReadStop(loopyPipe *pipe) {
    if (!pipe || !pipe->loop || pipe->readFd < 0) {
        return;
    }

    loopyUnregisterReadWrite(pipe->loop, pipe->readFd);

    if (pipe->readBuffer) {
        zfree(pipe->readBuffer);
        pipe->readBuffer = NULL;
        pipe->readBufferSize = 0;
    }

    pipe->loop = NULL;
    pipe->readCallback = NULL;
    pipe->readUserData = NULL;
}

static void pipeReadEventCallback(loopyLoop *l, int fd, void *clientData,
                                  loopyAction mask) {
    (void)l;
    (void)mask;

    loopyPipe *pipe = (loopyPipe *)clientData;
    if (!pipe || !pipe->readCallback || fd != pipe->readFd) {
        return;
    }

    ssize_t nread;
    do {
        nread = read(pipe->readFd, pipe->readBuffer, pipe->readBufferSize);
    } while (nread == -1 && errno == EINTR);

    /* Call user callback */
    pipe->readCallback(pipe, nread, pipe->readBuffer, pipe->readUserData);
}

/* Forward declaration of write event callback */
static void pipeWriteEventCallback(loopyLoop *l, int fd, void *clientData,
                                   loopyAction mask);

bool loopyPipeWriteAsync(loopyPipe *pipe, struct loopyLoop *loop,
                         const void *buf, size_t count,
                         loopyPipeWriteCallback *cb, void *userData) {
    if (!pipe || !loop || !buf) {
        setError("NULL pipe, loop, or buffer");
        return false;
    }

    if (pipe->writeFd < 0) {
        setError("Write end is closed");
        return false;
    }

    /* Try immediate write first */
    ssize_t nwritten;
    do {
        nwritten = write(pipe->writeFd, buf, count);
    } while (nwritten == -1 && errno == EINTR);

    if (nwritten == (ssize_t)count) {
        /* Write completed immediately */
        if (cb) {
            cb(pipe, 0, userData);
        }
        lastError[0] = '\0';
        return true;
    }

    if (nwritten == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
        /* Error */
        setErrorErrno("write");
        if (cb) {
            cb(pipe, -1, userData);
        }
        return false;
    }

    /* Would block - register for write events */
    pipe->loop = loop;
    pipe->writeCallback = cb;
    pipe->writeUserData = userData;

    if (!loopyRegisterWrite(loop, pipe->writeFd, pipeWriteEventCallback,
                            pipe)) {
        setError("Failed to register with event loop");
        pipe->loop = NULL;
        pipe->writeCallback = NULL;
        pipe->writeUserData = NULL;
        return false;
    }

    lastError[0] = '\0';
    return true;
}

static void pipeWriteEventCallback(loopyLoop *l, int fd, void *clientData,
                                   loopyAction mask) {
    (void)l;
    (void)fd;
    (void)mask;

    loopyPipe *pipe = (loopyPipe *)clientData;
    if (!pipe) {
        return;
    }

    /* Unregister write events */
    if (pipe->loop && pipe->writeFd >= 0) {
        loopyUnregisterWrite(pipe->loop, pipe->writeFd);
    }

    /* Call user callback */
    if (pipe->writeCallback) {
        pipe->writeCallback(pipe, 0, pipe->writeUserData);
    }

    pipe->writeCallback = NULL;
    pipe->writeUserData = NULL;
}

/* ====================================================================
 * Pipe Information
 * ==================================================================== */

bool loopyPipeIsReadable(const loopyPipe *pipe) {
    return pipe && pipe->readFd >= 0;
}

bool loopyPipeIsWritable(const loopyPipe *pipe) {
    return pipe && pipe->writeFd >= 0;
}

ssize_t loopyPipeGetBufferSize(const loopyPipe *pipe) {
    if (!pipe || (pipe->readFd < 0 && pipe->writeFd < 0)) {
        setError("Invalid pipe");
        return -1;
    }

#ifdef __linux__
    int fd = pipe->readFd >= 0 ? pipe->readFd : pipe->writeFd;
    int size = fcntl(fd, F_GETPIPE_SZ);
    if (size == -1) {
        setErrorErrno("fcntl F_GETPIPE_SZ");
        return -1;
    }
    lastError[0] = '\0';
    return size;
#else
    /* Default pipe buffer size on most systems */
    lastError[0] = '\0';
    return 65536;
#endif
}

bool loopyPipeSetBufferSize(loopyPipe *pipe, size_t size) {
    if (!pipe || (pipe->readFd < 0 && pipe->writeFd < 0)) {
        setError("Invalid pipe");
        return false;
    }

#ifdef __linux__
    int fd = pipe->readFd >= 0 ? pipe->readFd : pipe->writeFd;
    if (fcntl(fd, F_SETPIPE_SZ, (int)size) == -1) {
        setErrorErrno("fcntl F_SETPIPE_SZ");
        return false;
    }
    lastError[0] = '\0';
    return true;
#else
    /* Not supported on non-Linux */
    (void)size;
    setError("Pipe buffer size modification not supported");
    return false;
#endif
}

/* ====================================================================
 * Handle Accessors
 * ==================================================================== */

loopyLoop *loopyPipeGetLoop(const loopyPipe *pipe) {
    return pipe ? pipe->loop : NULL;
}

void *loopyPipeGetData(const loopyPipe *pipe) {
    return pipe ? pipe->userData : NULL;
}

void loopyPipeSetData(loopyPipe *pipe, void *data) {
    if (pipe) {
        pipe->userData = data;
    }
}

bool loopyPipeIsActive(const loopyPipe *pipe) {
    return pipe && pipe->loop && pipe->readCallback;
}
