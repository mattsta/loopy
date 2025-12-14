/* io_uring(7) based loopy.c module with epoll fallback
 *
 * This adapter uses io_uring for high-performance I/O on Linux 5.1+,
 * with automatic runtime fallback to epoll if io_uring is unavailable.
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

#include "loopy.c"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__

#include <poll.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>

/* io_uring public API headers */
#include "loopyIoUringFS.h"
#include "loopyIoUringNet.h"

/* ====================================================================
 * io_uring definitions (avoid liburing dependency)
 * ==================================================================== */

/* io_uring syscall numbers */
#ifndef __NR_io_uring_setup
#define __NR_io_uring_setup 425
#endif
#ifndef __NR_io_uring_enter
#define __NR_io_uring_enter 426
#endif
#ifndef __NR_io_uring_register
#define __NR_io_uring_register 427
#endif

/* io_uring_setup flags */
#define IORING_SETUP_CQSIZE (1U << 3)

/* io_uring_enter flags */
#define IORING_ENTER_GETEVENTS (1U << 0)

/* Submission queue entry opcodes */
#define IORING_OP_NOP 0
#define IORING_OP_READV 1
#define IORING_OP_WRITEV 2
#define IORING_OP_FSYNC 3
#define IORING_OP_READ_FIXED 4
#define IORING_OP_WRITE_FIXED 5
#define IORING_OP_POLL_ADD 6
#define IORING_OP_POLL_REMOVE 7
#define IORING_OP_SYNC_FILE_RANGE 8
#define IORING_OP_SENDMSG 9
#define IORING_OP_RECVMSG 10
#define IORING_OP_TIMEOUT 11
#define IORING_OP_TIMEOUT_REMOVE 12
#define IORING_OP_ACCEPT 13
#define IORING_OP_ASYNC_CANCEL 14
#define IORING_OP_LINK_TIMEOUT 15
#define IORING_OP_CONNECT 16
#define IORING_OP_FALLOCATE 17
#define IORING_OP_OPENAT 18
#define IORING_OP_CLOSE 19
#define IORING_OP_FILES_UPDATE 20
#define IORING_OP_STATX 21
#define IORING_OP_READ 22
#define IORING_OP_WRITE 23
#define IORING_OP_FADVISE 24
#define IORING_OP_MADVISE 25
#define IORING_OP_SEND 26
#define IORING_OP_RECV 27
#define IORING_OP_OPENAT2 28
#define IORING_OP_EPOLL_CTL 29
#define IORING_OP_SPLICE 30
#define IORING_OP_PROVIDE_BUFFERS 31
#define IORING_OP_REMOVE_BUFFERS 32
#define IORING_OP_TEE 33
#define IORING_OP_SHUTDOWN 34
#define IORING_OP_RENAMEAT 35
#define IORING_OP_UNLINKAT 36
#define IORING_OP_MKDIRAT 37

/* Poll events (same as EPOLL) */
#define IORING_POLL_IN EPOLLIN
#define IORING_POLL_OUT EPOLLOUT
#define IORING_POLL_ERR EPOLLERR
#define IORING_POLL_HUP EPOLLHUP

/* Feature flags */
#define IORING_FEAT_SINGLE_MMAP (1U << 0)
#define IORING_FEAT_NODROP (1U << 1)
#define IORING_FEAT_SUBMIT_STABLE (1U << 2)
#define IORING_FEAT_RW_CUR_POS (1U << 3)
#define IORING_FEAT_CUR_PERSONALITY (1U << 4)
#define IORING_FEAT_FAST_POLL (1U << 5)
#define IORING_FEAT_POLL_32BITS (1U << 6)

/* Multishot accept flag (Linux 5.19+) */
#ifndef IORING_ACCEPT_MULTISHOT
#define IORING_ACCEPT_MULTISHOT (1U << 0)
#endif

/* CQE (Completion Queue Entry) flags */
#ifndef IORING_CQE_F_BUFFER
#define IORING_CQE_F_BUFFER (1U << 0) /* Buffer ID is valid */
#endif
#ifndef IORING_CQE_F_MORE
#define IORING_CQE_F_MORE (1U << 1) /* More completions will follow */
#endif

/* FSYNC flags */
#define IORING_FSYNC_DATASYNC (1U << 0)

/* Timeout flags */
#define IORING_TIMEOUT_ABS (1U << 0)

/* SQE flags */
#define IOSQE_FIXED_FILE (1U << 0)  /* Use fixed file index instead of fd */
#define IOSQE_IO_DRAIN (1U << 1)    /* Drain previously submitted operations */
#define IOSQE_IO_LINK (1U << 2)     /* Link next SQE (fails -> cancel chain) */
#define IOSQE_IO_HARDLINK (1U << 3) /* Like LINK but continues on error */

/* io_uring_register opcodes */
#define IORING_REGISTER_BUFFERS 0
#define IORING_UNREGISTER_BUFFERS 1
#define IORING_REGISTER_FILES 2
#define IORING_UNREGISTER_FILES 3
#define IORING_REGISTER_EVENTFD 4
#define IORING_UNREGISTER_EVENTFD 5
#define IORING_REGISTER_FILES_UPDATE 6
#define IORING_REGISTER_EVENTFD_ASYNC 7
#define IORING_REGISTER_PROBE 8
#define IORING_REGISTER_PERSONALITY 9
#define IORING_UNREGISTER_PERSONALITY 10

/* Statx flags - need for STATX operation */
#ifndef AT_FDCWD
#define AT_FDCWD -100
#endif

#ifndef AT_STATX_SYNC_AS_STAT
#define AT_STATX_SYNC_AS_STAT 0x0000
#endif

/* Offsets for mmap */
#define IORING_OFF_SQ_RING 0ULL
#define IORING_OFF_CQ_RING 0x8000000ULL
#define IORING_OFF_SQES 0x10000000ULL

/* io_uring structures */
struct io_uring_sqe {
    uint8_t opcode;
    uint8_t flags;
    uint16_t ioprio;
    int32_t fd;
    union {
        uint64_t off;
        uint64_t addr2;
    };
    union {
        uint64_t addr;
        uint64_t splice_off_in;
    };
    uint32_t len;
    union {
        uint32_t rw_flags;
        uint32_t fsync_flags;
        uint32_t poll_events;
        uint32_t poll32_events;
        uint32_t sync_range_flags;
        uint32_t msg_flags;
        uint32_t timeout_flags;
        uint32_t accept_flags;
        uint32_t cancel_flags;
        uint32_t open_flags;
        uint32_t statx_flags;
        uint32_t fadvise_advice;
        uint32_t splice_flags;
    };
    uint64_t user_data;
    union {
        struct {
            uint16_t buf_index;
            uint16_t buf_group;
        };
        uint64_t optval;
    };
    uint16_t personality;
    union {
        int32_t splice_fd_in;
        uint32_t file_index;
    };
    uint64_t __pad2[2];
};

struct io_uring_cqe {
    uint64_t user_data;
    int32_t res;
    uint32_t flags;
};

struct io_sqring_offsets {
    uint32_t head;
    uint32_t tail;
    uint32_t ring_mask;
    uint32_t ring_entries;
    uint32_t flags;
    uint32_t dropped;
    uint32_t array;
    uint32_t resv1;
    uint64_t resv2;
};

struct io_cqring_offsets {
    uint32_t head;
    uint32_t tail;
    uint32_t ring_mask;
    uint32_t ring_entries;
    uint32_t overflow;
    uint32_t cqes;
    uint32_t flags;
    uint32_t resv1;
    uint64_t resv2;
};

struct io_uring_params {
    uint32_t sq_entries;
    uint32_t cq_entries;
    uint32_t flags;
    uint32_t sq_thread_cpu;
    uint32_t sq_thread_idle;
    uint32_t features;
    uint32_t wq_fd;
    uint32_t resv[3];
    struct io_sqring_offsets sq_off;
    struct io_cqring_offsets cq_off;
};

/* ====================================================================
 * io_uring wrapper functions (syscall-based)
 * ==================================================================== */

static int io_uring_setup(unsigned entries, struct io_uring_params *p) {
    return (int)syscall(__NR_io_uring_setup, entries, p);
}

static int io_uring_enter(int fd, unsigned to_submit, unsigned min_complete,
                          unsigned flags, void *sig) {
    return (int)syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags,
                        sig, 0);
}

static int io_uring_register(int fd, unsigned opcode, void *arg,
                             unsigned nr_args) {
    return (int)syscall(__NR_io_uring_register, fd, opcode, arg, nr_args);
}

/* ====================================================================
 * Internal state
 * ==================================================================== */

typedef enum { BACKEND_EPOLL, BACKEND_IOURING } BackendType;

/* ====================================================================
 * File operation support
 * ==================================================================== */

/**
 * Callback for file operation completion.
 *
 * @param userData User data passed during submission
 * @param result   Operation result (bytes transferred, fd, or -errno on error)
 */
typedef void loopyIoUringFileCallback(void *userData, int32_t result);

/**
 * Pending file operation tracking.
 *
 * Each submitted file operation is tracked so we can invoke the callback
 * when the completion arrives.
 */
typedef struct loopyIoUringFileOp {
    uint64_t id;                  /* Unique operation ID (user_data) */
    loopyIoUringFileCallback *cb; /* Completion callback */
    void *userData;               /* User data for callback */
    bool active;                  /* Is this slot active? */
    bool multishot;               /* Is this a multishot operation? */
} loopyIoUringFileOp;

/**
 * File operation tracking state.
 */
typedef struct loopyIoUringFileState {
    loopyIoUringFileOp *ops; /* Array of pending operations */
    size_t opsSize;          /* Size of ops array */
    uint64_t nextId;         /* Next operation ID */
} loopyIoUringFileState;

/* io_uring ring state */
typedef struct {
    int ringFd;
    void *sqRing;
    void *cqRing;
    struct io_uring_sqe *sqes;
    size_t sqRingSize;
    size_t cqRingSize;
    size_t sqesSize;

    /* Ring pointers */
    uint32_t *sqHead;
    uint32_t *sqTail;
    uint32_t *sqRingMask;
    uint32_t *sqArray;

    uint32_t *cqHead;
    uint32_t *cqTail;
    uint32_t *cqRingMask;
    struct io_uring_cqe *cqes;

    /* File operation tracking */
    loopyIoUringFileState fileState;

    /* Feature detection */
    uint32_t features; /* Features from io_uring_setup */

    /* Fixed buffer registration */
    struct iovec
        *registeredBuffers;        /* Array of registered buffer descriptors */
    uint32_t numRegisteredBuffers; /* Number of registered buffers */
    bool buffersRegistered;        /* Are buffers currently registered? */

    /* Fixed file registration */
    int *registeredFiles;        /* Array of registered file descriptors */
    uint32_t numRegisteredFiles; /* Number of registered files */
    bool filesRegistered;        /* Are files currently registered? */
} IoUringState;

/* Epoll state (fallback) */
typedef struct {
    int epollFd;
    struct epoll_event *events;
} EpollState;

/* Combined internal state */
typedef struct loopyInternalState {
    BackendType backend;
    union {
        IoUringState uring;
        EpollState epoll;
    };
    /* Track which fds are registered for poll (for io_uring) */
    uint32_t *pollMasks; /* Per-fd poll mask tracking */
    size_t pollMasksSize;
} loopyInternalState;

/* ====================================================================
 * io_uring implementation
 * ==================================================================== */

static bool ioUringInit(loopyInternalState *state, size_t setSize) {
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));

    /* Try to create io_uring with reasonable queue depth */
    int ringFd = io_uring_setup(256, &params);
    if (ringFd < 0) {
        return false;
    }

    IoUringState *u = &state->uring;
    u->ringFd = ringFd;
    u->features = params.features;

    /* Initialize fixed buffer tracking */
    u->registeredBuffers = NULL;
    u->numRegisteredBuffers = 0;
    u->buffersRegistered = false;

    /* Initialize fixed file tracking */
    u->registeredFiles = NULL;
    u->numRegisteredFiles = 0;
    u->filesRegistered = false;

    /* Calculate mmap sizes */
    u->sqRingSize = params.sq_off.array + params.sq_entries * sizeof(uint32_t);
    u->cqRingSize =
        params.cq_off.cqes + params.cq_entries * sizeof(struct io_uring_cqe);
    u->sqesSize = params.sq_entries * sizeof(struct io_uring_sqe);

    /* Check if kernel supports single mmap for both rings */
    bool singleMmap = (params.features & IORING_FEAT_SINGLE_MMAP) != 0;
    if (singleMmap && u->cqRingSize > u->sqRingSize) {
        u->sqRingSize = u->cqRingSize;
    }

    /* Map submission queue ring */
    u->sqRing = mmap(NULL, u->sqRingSize, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_POPULATE, ringFd, IORING_OFF_SQ_RING);
    if (u->sqRing == MAP_FAILED) {
        close(ringFd);
        return false;
    }

    /* Map completion queue ring (may be same as sq for newer kernels) */
    if (singleMmap) {
        u->cqRing = u->sqRing;
    } else {
        u->cqRing = mmap(NULL, u->cqRingSize, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_POPULATE, ringFd, IORING_OFF_CQ_RING);
        if (u->cqRing == MAP_FAILED) {
            munmap(u->sqRing, u->sqRingSize);
            close(ringFd);
            return false;
        }
    }

    /* Map submission queue entries */
    u->sqes = mmap(NULL, u->sqesSize, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_POPULATE, ringFd, IORING_OFF_SQES);
    if (u->sqes == MAP_FAILED) {
        if (!singleMmap) {
            munmap(u->cqRing, u->cqRingSize);
        }
        munmap(u->sqRing, u->sqRingSize);
        close(ringFd);
        return false;
    }

    /* Set up ring pointers */
    u->sqHead = (uint32_t *)((char *)u->sqRing + params.sq_off.head);
    u->sqTail = (uint32_t *)((char *)u->sqRing + params.sq_off.tail);
    u->sqRingMask = (uint32_t *)((char *)u->sqRing + params.sq_off.ring_mask);
    u->sqArray = (uint32_t *)((char *)u->sqRing + params.sq_off.array);

    u->cqHead = (uint32_t *)((char *)u->cqRing + params.cq_off.head);
    u->cqTail = (uint32_t *)((char *)u->cqRing + params.cq_off.tail);
    u->cqRingMask = (uint32_t *)((char *)u->cqRing + params.cq_off.ring_mask);
    u->cqes = (struct io_uring_cqe *)((char *)u->cqRing + params.cq_off.cqes);

    /* Allocate poll mask tracking */
    state->pollMasks = zcalloc(setSize, sizeof(*state->pollMasks));
    if (!state->pollMasks) {
        munmap(u->sqes, u->sqesSize);
        if (!singleMmap) {
            munmap(u->cqRing, u->cqRingSize);
        }
        munmap(u->sqRing, u->sqRingSize);
        close(ringFd);
        return false;
    }
    state->pollMasksSize = setSize;

    /* Initialize file operation tracking */
    u->fileState.opsSize = 256; /* Initial capacity for file ops */
    u->fileState.ops =
        zcalloc(u->fileState.opsSize, sizeof(loopyIoUringFileOp));
    if (!u->fileState.ops) {
        zfree(state->pollMasks);
        munmap(u->sqes, u->sqesSize);
        if (!singleMmap) {
            munmap(u->cqRing, u->cqRingSize);
        }
        munmap(u->sqRing, u->sqRingSize);
        close(ringFd);
        return false;
    }
    u->fileState.nextId = 1; /* Start at 1, 0 reserved for poll operations */

    state->backend = BACKEND_IOURING;
    return true;
}

static void ioUringFree(loopyInternalState *state) {
    if (!state) {
        return;
    }

    IoUringState *u = &state->uring;

    /* Free file operation tracking */
    if (u->fileState.ops) {
        zfree(u->fileState.ops);
        u->fileState.ops = NULL;  /* Clear pointer to prevent double-free */
    }

    /* Unmap memory regions safely */
    if (u->sqes) {
        munmap(u->sqes, u->sqesSize);
        u->sqes = NULL;
    }

    /* Check if cqRing is separate from sqRing */
    if (u->cqRing && u->cqRing != u->sqRing) {
        munmap(u->cqRing, u->cqRingSize);
        u->cqRing = NULL;
    }

    if (u->sqRing) {
        munmap(u->sqRing, u->sqRingSize);
        u->sqRing = NULL;
    }

    /* Close ring file descriptor */
    if (u->ringFd >= 0) {
        close(u->ringFd);
        u->ringFd = -1;
    }

    /* Free poll masks */
    if (state->pollMasks) {
        zfree(state->pollMasks);
        state->pollMasks = NULL;
    }
}

static struct io_uring_sqe *ioUringGetSqe(IoUringState *u) {
    uint32_t head = __atomic_load_n(u->sqHead, __ATOMIC_ACQUIRE);
    uint32_t tail = *u->sqTail;

    if (tail - head >= *u->sqRingMask + 1) {
        return NULL; /* Queue full */
    }

    struct io_uring_sqe *sqe = &u->sqes[tail & *u->sqRingMask];
    memset(sqe, 0, sizeof(*sqe));
    return sqe;
}

static void ioUringSubmitSqe(IoUringState *u) {
    uint32_t tail = *u->sqTail;
    u->sqArray[tail & *u->sqRingMask] = tail & *u->sqRingMask;
    __atomic_store_n(u->sqTail, tail + 1, __ATOMIC_RELEASE);
}

/* ====================================================================
 * File operation helpers
 * ==================================================================== */

/**
 * Allocate a new file operation ID and register the callback.
 *
 * @param u        io_uring state
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return Operation ID, or 0 on failure
 */
static uint64_t ioUringFileOpAllocate(IoUringState *u,
                                      loopyIoUringFileCallback *cb,
                                      void *userData) {
    /* Validate callback is not NULL */
    if (!cb) {
        return 0;
    }

    /* Find free slot */
    for (size_t i = 0; i < u->fileState.opsSize; i++) {
        if (!u->fileState.ops[i].active) {
            uint64_t id = u->fileState.nextId++;
            u->fileState.ops[i].id = id;
            u->fileState.ops[i].cb = cb;
            u->fileState.ops[i].userData = userData;
            u->fileState.ops[i].active = true;
            /* Set bit 63 to mark as file operation (vs poll with bit 63 = 0) */
            return id | (1ULL << 63);
        }
    }

    /* No free slots - need to grow array */
    size_t newSize = u->fileState.opsSize * 2;
    loopyIoUringFileOp *newOps =
        zrealloc(u->fileState.ops, newSize * sizeof(loopyIoUringFileOp));
    if (!newOps) {
        return 0;
    }

    /* Zero new entries */
    memset(newOps + u->fileState.opsSize, 0,
           (newSize - u->fileState.opsSize) * sizeof(loopyIoUringFileOp));

    u->fileState.ops = newOps;
    u->fileState.opsSize = newSize;

    /* Use first new slot */
    uint64_t id = u->fileState.nextId++;
    u->fileState.ops[u->fileState.opsSize / 2].id = id;
    u->fileState.ops[u->fileState.opsSize / 2].cb = cb;
    u->fileState.ops[u->fileState.opsSize / 2].userData = userData;
    u->fileState.ops[u->fileState.opsSize / 2].active = true;

    /* Set bit 63 to mark as file operation */
    return id | (1ULL << 63);
}

/**
 * Find file operation by ID.
 *
 * @param u  io_uring state
 * @param id Operation ID
 * @return Pointer to operation, or NULL if not found
 */
static loopyIoUringFileOp *ioUringFileOpFind(IoUringState *u, uint64_t id) {
    for (size_t i = 0; i < u->fileState.opsSize; i++) {
        if (u->fileState.ops[i].active && u->fileState.ops[i].id == id) {
            return &u->fileState.ops[i];
        }
    }
    return NULL;
}

/**
 * Complete and free a file operation.
 *
 * @param op     File operation
 * @param result Completion result
 */
static void ioUringFileOpComplete(loopyIoUringFileOp *op, int32_t result) {
    if (!op || !op->active) {
        return;
    }

    if (op->cb) {
        op->cb(op->userData, result);
    }

    op->active = false;
}

/**
 * Submit a READ operation to io_uring.
 *
 * @param u        io_uring state
 * @param fd       File descriptor to read from
 * @param buf      Buffer to read into
 * @param len      Number of bytes to read
 * @param offset   File offset (or -1 for current position if supported)
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure
 */
static uint64_t ioUringSubmitRead(IoUringState *u, int fd, void *buf,
                                  size_t len, off_t offset,
                                  loopyIoUringFileCallback *cb,
                                  void *userData) {
    struct io_uring_sqe *sqe = ioUringGetSqe(u);
    if (!sqe) {
        return 0;
    }

    /* Validate parameters before allocating operation resources */
    if (!buf) {
        return 0;
    }

    uint64_t id = ioUringFileOpAllocate(u, cb, userData);
    if (!id) {
        return 0;
    }

    sqe->opcode = IORING_OP_READ;
    sqe->fd = fd;
    sqe->addr = (uint64_t)(uintptr_t)buf;
    sqe->len = (uint32_t)len;
    sqe->off = (uint64_t)offset;
    sqe->user_data = id;

    ioUringSubmitSqe(u);

    /* Submit to kernel */
    if (io_uring_enter(u->ringFd, 1, 0, 0, NULL) < 0) {
        /* Failed to submit - mark operation as failed */
        loopyIoUringFileOp *op = ioUringFileOpFind(u, id);
        if (op) {
            op->active = false;
        }
        return 0;
    }

    return id;
}

/**
 * Submit a WRITE operation to io_uring.
 *
 * @param u        io_uring state
 * @param fd       File descriptor to write to
 * @param buf      Buffer to write from
 * @param len      Number of bytes to write
 * @param offset   File offset (or -1 for current position if supported)
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure
 */
static uint64_t ioUringSubmitWrite(IoUringState *u, int fd, const void *buf,
                                   size_t len, off_t offset,
                                   loopyIoUringFileCallback *cb,
                                   void *userData) {
    struct io_uring_sqe *sqe = ioUringGetSqe(u);
    if (!sqe) {
        return 0;
    }

    /* Validate parameters before allocating operation resources */
    if (!buf) {
        return 0;
    }

    uint64_t id = ioUringFileOpAllocate(u, cb, userData);
    if (!id) {
        return 0;
    }

    sqe->opcode = IORING_OP_WRITE;
    sqe->fd = fd;
    sqe->addr = (uint64_t)(uintptr_t)buf;
    sqe->len = (uint32_t)len;
    sqe->off = (uint64_t)offset;
    sqe->user_data = id;

    ioUringSubmitSqe(u);

    /* Submit to kernel */
    if (io_uring_enter(u->ringFd, 1, 0, 0, NULL) < 0) {
        loopyIoUringFileOp *op = ioUringFileOpFind(u, id);
        if (op) {
            op->active = false;
        }
        return 0;
    }

    return id;
}

/**
 * Submit a READ_FIXED operation to io_uring using a registered buffer.
 *
 * @param u         io_uring state
 * @param fd        File descriptor to read from
 * @param bufIndex  Index of registered buffer
 * @param len       Number of bytes to read
 * @param offset    File offset (or -1 for current position if supported)
 * @param cb        Completion callback
 * @param userData  User data for callback
 * @return Operation ID on success, 0 on failure
 */
static uint64_t ioUringSubmitReadFixed(IoUringState *u, int fd,
                                       uint32_t bufIndex, size_t len,
                                       off_t offset,
                                       loopyIoUringFileCallback *cb,
                                       void *userData) {
    /* Validate buffer index */
    if (!u->buffersRegistered || bufIndex >= u->numRegisteredBuffers) {
        return 0;
    }

    /* Get buffer address from registered buffers */
    void *buf = u->registeredBuffers[bufIndex].iov_base;
    size_t bufSize = u->registeredBuffers[bufIndex].iov_len;

    /* Validate length fits in buffer */
    if (len > bufSize) {
        return 0;
    }

    struct io_uring_sqe *sqe = ioUringGetSqe(u);
    if (!sqe) {
        return 0;
    }

    uint64_t id = ioUringFileOpAllocate(u, cb, userData);
    if (!id) {
        return 0;
    }

    sqe->opcode = IORING_OP_READ_FIXED;
    sqe->fd = fd;
    sqe->addr = (uint64_t)(uintptr_t)buf;
    sqe->len = (uint32_t)len;
    sqe->off = (uint64_t)offset;
    sqe->buf_index = (uint16_t)bufIndex;
    sqe->user_data = id;

    ioUringSubmitSqe(u);

    /* Submit to kernel */
    if (io_uring_enter(u->ringFd, 1, 0, 0, NULL) < 0) {
        loopyIoUringFileOp *op = ioUringFileOpFind(u, id);
        if (op) {
            op->active = false;
        }
        return 0;
    }

    return id;
}

/**
 * Submit a WRITE_FIXED operation to io_uring using a registered buffer.
 *
 * @param u         io_uring state
 * @param fd        File descriptor to write to
 * @param bufIndex  Index of registered buffer
 * @param len       Number of bytes to write
 * @param offset    File offset (or -1 for current position if supported)
 * @param cb        Completion callback
 * @param userData  User data for callback
 * @return Operation ID on success, 0 on failure
 */
static uint64_t ioUringSubmitWriteFixed(IoUringState *u, int fd,
                                        uint32_t bufIndex, size_t len,
                                        off_t offset,
                                        loopyIoUringFileCallback *cb,
                                        void *userData) {
    /* Validate buffer index */
    if (!u->buffersRegistered || bufIndex >= u->numRegisteredBuffers) {
        return 0;
    }

    /* Get buffer address from registered buffers */
    const void *buf = u->registeredBuffers[bufIndex].iov_base;
    size_t bufSize = u->registeredBuffers[bufIndex].iov_len;

    /* Validate length fits in buffer */
    if (len > bufSize) {
        return 0;
    }

    struct io_uring_sqe *sqe = ioUringGetSqe(u);
    if (!sqe) {
        return 0;
    }

    uint64_t id = ioUringFileOpAllocate(u, cb, userData);
    if (!id) {
        return 0;
    }

    sqe->opcode = IORING_OP_WRITE_FIXED;
    sqe->fd = fd;
    sqe->addr = (uint64_t)(uintptr_t)buf;
    sqe->len = (uint32_t)len;
    sqe->off = (uint64_t)offset;
    sqe->buf_index = (uint16_t)bufIndex;
    sqe->user_data = id;

    ioUringSubmitSqe(u);

    /* Submit to kernel */
    if (io_uring_enter(u->ringFd, 1, 0, 0, NULL) < 0) {
        loopyIoUringFileOp *op = ioUringFileOpFind(u, id);
        if (op) {
            op->active = false;
        }
        return 0;
    }

    return id;
}

/**
 * Submit a READ operation using a registered fixed file (by index).
 *
 * Uses a pre-registered file descriptor by index, eliminating file descriptor
 * lookup overhead on each I/O operation.
 *
 * @param u         io_uring state
 * @param fileIndex Index of registered file (0 to count-1)
 * @param buf       Buffer to read into
 * @param len       Number of bytes to read
 * @param offset    File offset (or -1 for current position)
 * @param cb        Completion callback
 * @param userData  User data for callback
 * @return Operation ID on success, 0 on failure
 */
static uint64_t ioUringSubmitReadFixedFile(IoUringState *u, uint32_t fileIndex,
                                           void *buf, size_t len, off_t offset,
                                           loopyIoUringFileCallback *cb,
                                           void *userData) {
    /* Validate file index */
    if (!u->filesRegistered || fileIndex >= u->numRegisteredFiles) {
        return 0;
    }

    struct io_uring_sqe *sqe = ioUringGetSqe(u);
    if (!sqe) {
        return 0;
    }

    uint64_t id = ioUringFileOpAllocate(u, cb, userData);
    if (!id) {
        return 0;
    }

    sqe->opcode = IORING_OP_READ;
    sqe->flags = IOSQE_FIXED_FILE; /* Use fixed file by index */
    sqe->fd = (int)fileIndex;      /* File index, not actual fd */
    sqe->addr = (uint64_t)(uintptr_t)buf;
    sqe->len = (uint32_t)len;
    sqe->off = (uint64_t)offset;
    sqe->user_data = id;

    ioUringSubmitSqe(u);

    /* Submit to kernel */
    if (io_uring_enter(u->ringFd, 1, 0, 0, NULL) < 0) {
        loopyIoUringFileOp *op = ioUringFileOpFind(u, id);
        if (op) {
            op->active = false;
        }
        return 0;
    }

    return id;
}

/**
 * Submit a WRITE operation using a registered fixed file (by index).
 *
 * Uses a pre-registered file descriptor by index, eliminating file descriptor
 * lookup overhead on each I/O operation.
 *
 * @param u         io_uring state
 * @param fileIndex Index of registered file (0 to count-1)
 * @param buf       Buffer to write from
 * @param len       Number of bytes to write
 * @param offset    File offset (or -1 for current position)
 * @param cb        Completion callback
 * @param userData  User data for callback
 * @return Operation ID on success, 0 on failure
 */
static uint64_t ioUringSubmitWriteFixedFile(IoUringState *u, uint32_t fileIndex,
                                            const void *buf, size_t len,
                                            off_t offset,
                                            loopyIoUringFileCallback *cb,
                                            void *userData) {
    /* Validate file index */
    if (!u->filesRegistered || fileIndex >= u->numRegisteredFiles) {
        return 0;
    }

    struct io_uring_sqe *sqe = ioUringGetSqe(u);
    if (!sqe) {
        return 0;
    }

    uint64_t id = ioUringFileOpAllocate(u, cb, userData);
    if (!id) {
        return 0;
    }

    sqe->opcode = IORING_OP_WRITE;
    sqe->flags = IOSQE_FIXED_FILE; /* Use fixed file by index */
    sqe->fd = (int)fileIndex;      /* File index, not actual fd */
    sqe->addr = (uint64_t)(uintptr_t)buf;
    sqe->len = (uint32_t)len;
    sqe->off = (uint64_t)offset;
    sqe->user_data = id;

    ioUringSubmitSqe(u);

    /* Submit to kernel */
    if (io_uring_enter(u->ringFd, 1, 0, 0, NULL) < 0) {
        loopyIoUringFileOp *op = ioUringFileOpFind(u, id);
        if (op) {
            op->active = false;
        }
        return 0;
    }

    return id;
}

/**
 * Submit an OPENAT operation to io_uring.
 *
 * @param u        io_uring state
 * @param dirfd    Directory fd (AT_FDCWD for cwd)
 * @param path     Path to open
 * @param flags    Open flags
 * @param mode     File mode (for O_CREAT)
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure
 */
static uint64_t ioUringSubmitOpenat(IoUringState *u, int dirfd,
                                    const char *path, int flags, mode_t mode,
                                    loopyIoUringFileCallback *cb,
                                    void *userData) {
    struct io_uring_sqe *sqe = ioUringGetSqe(u);
    if (!sqe) {
        return 0;
    }

    /* Validate parameters before allocating operation resources */
    if (!path) {
        return 0;
    }

    uint64_t id = ioUringFileOpAllocate(u, cb, userData);
    if (!id) {
        return 0;
    }

    sqe->opcode = IORING_OP_OPENAT;
    sqe->fd = dirfd;
    sqe->addr = (uint64_t)(uintptr_t)path;
    sqe->len = mode;
    sqe->open_flags = (uint32_t)flags;
    sqe->user_data = id;

    ioUringSubmitSqe(u);

    if (io_uring_enter(u->ringFd, 1, 0, 0, NULL) < 0) {
        loopyIoUringFileOp *op = ioUringFileOpFind(u, id);
        if (op) {
            op->active = false;
        }
        return 0;
    }

    return id;
}

/**
 * Submit a CLOSE operation to io_uring.
 *
 * @param u        io_uring state
 * @param fd       File descriptor to close
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure
 */
static uint64_t ioUringSubmitClose(IoUringState *u, int fd,
                                   loopyIoUringFileCallback *cb,
                                   void *userData) {
    struct io_uring_sqe *sqe = ioUringGetSqe(u);
    if (!sqe) {
        return 0;
    }

    uint64_t id = ioUringFileOpAllocate(u, cb, userData);
    if (!id) {
        return 0;
    }

    sqe->opcode = IORING_OP_CLOSE;
    sqe->fd = fd;
    sqe->user_data = id;

    ioUringSubmitSqe(u);

    if (io_uring_enter(u->ringFd, 1, 0, 0, NULL) < 0) {
        loopyIoUringFileOp *op = ioUringFileOpFind(u, id);
        if (op) {
            op->active = false;
        }
        return 0;
    }

    return id;
}

/**
 * Submit an FSYNC operation to io_uring.
 *
 * @param u         io_uring state
 * @param fd        File descriptor to sync
 * @param datasync  true for fdatasync, false for fsync
 * @param cb        Completion callback
 * @param userData  User data for callback
 * @return Operation ID on success, 0 on failure
 */
static uint64_t ioUringSubmitFsync(IoUringState *u, int fd, bool datasync,
                                   loopyIoUringFileCallback *cb,
                                   void *userData) {
    struct io_uring_sqe *sqe = ioUringGetSqe(u);
    if (!sqe) {
        return 0;
    }

    uint64_t id = ioUringFileOpAllocate(u, cb, userData);
    if (!id) {
        return 0;
    }

    sqe->opcode = IORING_OP_FSYNC;
    sqe->fd = fd;
    sqe->fsync_flags = datasync ? IORING_FSYNC_DATASYNC : 0;
    sqe->user_data = id;

    ioUringSubmitSqe(u);

    if (io_uring_enter(u->ringFd, 1, 0, 0, NULL) < 0) {
        loopyIoUringFileOp *op = ioUringFileOpFind(u, id);
        if (op) {
            op->active = false;
        }
        return 0;
    }

    return id;
}

/* ====================================================================
 * Network operation helpers
 * ==================================================================== */

/**
 * Submit a SEND operation to io_uring.
 *
 * @param u        io_uring state
 * @param sockfd   Socket file descriptor (must be connected)
 * @param buf      Buffer to send from
 * @param len      Number of bytes to send
 * @param flags    Send flags (MSG_DONTWAIT, MSG_NOSIGNAL, etc.)
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure
 */
static uint64_t ioUringSubmitSend(IoUringState *u, int sockfd, const void *buf,
                                  size_t len, int flags,
                                  loopyIoUringFileCallback *cb,
                                  void *userData) {
    struct io_uring_sqe *sqe = ioUringGetSqe(u);
    if (!sqe) {
        return 0;
    }

    /* Validate parameters before allocating operation resources */
    if (!buf) {
        return 0;
    }

    uint64_t id = ioUringFileOpAllocate(u, cb, userData);
    if (!id) {
        return 0;
    }

    sqe->opcode = IORING_OP_SEND;
    sqe->fd = sockfd;
    sqe->addr = (uint64_t)(uintptr_t)buf;
    sqe->len = (uint32_t)len;
    sqe->msg_flags = (uint32_t)flags;
    sqe->user_data = id;

    ioUringSubmitSqe(u);

    if (io_uring_enter(u->ringFd, 1, 0, 0, NULL) < 0) {
        loopyIoUringFileOp *op = ioUringFileOpFind(u, id);
        if (op) {
            op->active = false;
        }
        return 0;
    }

    return id;
}

/**
 * Submit a RECV operation to io_uring.
 *
 * @param u        io_uring state
 * @param sockfd   Socket file descriptor (must be connected)
 * @param buf      Buffer to receive into
 * @param len      Buffer size
 * @param flags    Receive flags (MSG_DONTWAIT, MSG_PEEK, etc.)
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure
 */
static uint64_t ioUringSubmitRecv(IoUringState *u, int sockfd, void *buf,
                                  size_t len, int flags,
                                  loopyIoUringFileCallback *cb,
                                  void *userData) {
    struct io_uring_sqe *sqe = ioUringGetSqe(u);
    if (!sqe) {
        return 0;
    }

    /* Validate parameters before allocating operation resources */
    if (!buf) {
        return 0;
    }

    uint64_t id = ioUringFileOpAllocate(u, cb, userData);
    if (!id) {
        return 0;
    }

    sqe->opcode = IORING_OP_RECV;
    sqe->fd = sockfd;
    sqe->addr = (uint64_t)(uintptr_t)buf;
    sqe->len = (uint32_t)len;
    sqe->msg_flags = (uint32_t)flags;
    sqe->user_data = id;

    ioUringSubmitSqe(u);

    if (io_uring_enter(u->ringFd, 1, 0, 0, NULL) < 0) {
        loopyIoUringFileOp *op = ioUringFileOpFind(u, id);
        if (op) {
            op->active = false;
        }
        return 0;
    }

    return id;
}

/**
 * Submit an ACCEPT operation to io_uring.
 *
 * @param u        io_uring state
 * @param sockfd   Listening socket file descriptor
 * @param addr     OUT: Client address (optional)
 * @param addrlen  IN/OUT: Address buffer size / actual size (optional)
 * @param flags    Accept flags (SOCK_NONBLOCK, SOCK_CLOEXEC, etc.)
 * @param cb       Completion callback (receives new socket fd in result)
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure
 */
static uint64_t ioUringSubmitAccept(IoUringState *u, int sockfd,
                                    struct sockaddr *addr, socklen_t *addrlen,
                                    int flags, loopyIoUringFileCallback *cb,
                                    void *userData) {
    struct io_uring_sqe *sqe = ioUringGetSqe(u);
    if (!sqe) {
        return 0;
    }

    /* Validate parameters before allocating operation resources */
    if (!addr && addrlen) {
        /* If user wants address back but provides NULL, that's an error */
        return 0;
    }

    uint64_t id = ioUringFileOpAllocate(u, cb, userData);
    if (!id) {
        return 0;
    }

    sqe->opcode = IORING_OP_ACCEPT;
    sqe->fd = sockfd;
    sqe->addr = (uint64_t)(uintptr_t)addr;
    sqe->addr2 = (uint64_t)(uintptr_t)addrlen;
    sqe->accept_flags = (uint32_t)flags;
    sqe->user_data = id;

    ioUringSubmitSqe(u);

    if (io_uring_enter(u->ringFd, 1, 0, 0, NULL) < 0) {
        loopyIoUringFileOp *op = ioUringFileOpFind(u, id);
        if (op) {
            op->active = false;
        }
        return 0;
    }

    return id;
}

/**
 * Submit a multishot ACCEPT operation to io_uring.
 *
 * Multishot accept (Linux 5.19+) is a high-performance mode where a single
 * IORING_OP_ACCEPT operation stays active and completes multiple times - once
 * for each new connection. This eliminates accept() syscalls and achieves
 * 10x throughput improvement (500K+ connections/sec vs ~50K).
 *
 * The operation remains active until:
 * - An error occurs (result < 0)
 * - The socket is closed
 * - The operation is explicitly cancelled
 *
 * Each completion has CQE_F_MORE flag set (except the final one), indicating
 * the operation will complete again for the next connection.
 *
 * Note: Multishot accept does not provide client address information (addr is
 * NULL).
 *
 * @param u        io_uring state
 * @param sockfd   Listening socket file descriptor
 * @param flags    Accept flags (SOCK_NONBLOCK, SOCK_CLOEXEC, etc.)
 * @param cb       Completion callback (receives new socket fd in result, called
 *                 multiple times - once per connection)
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure
 */
static uint64_t ioUringSubmitAcceptMultishot(IoUringState *u, int sockfd,
                                             int flags,
                                             loopyIoUringFileCallback *cb,
                                             void *userData) {
    struct io_uring_sqe *sqe = ioUringGetSqe(u);
    if (!sqe) {
        return 0;
    }

    uint64_t id = ioUringFileOpAllocate(u, cb, userData);
    if (!id) {
        return 0;
    }

    /* Mark operation as multishot */
    loopyIoUringFileOp *op = ioUringFileOpFind(u, id);
    if (op) {
        op->multishot = true;
    }

    sqe->opcode = IORING_OP_ACCEPT;
    sqe->fd = sockfd;
    sqe->addr = 0;  /* NULL for multishot */
    sqe->addr2 = 0; /* NULL for multishot */
    sqe->accept_flags = (uint32_t)flags;
    sqe->ioprio = IORING_ACCEPT_MULTISHOT; /* Enable multishot mode */
    sqe->user_data = id;

    ioUringSubmitSqe(u);

    if (io_uring_enter(u->ringFd, 1, 0, 0, NULL) < 0) {
        op = ioUringFileOpFind(u, id);
        if (op) {
            op->active = false;
        }
        return 0;
    }

    return id;
}

/**
 * Submit a CONNECT operation to io_uring.
 *
 * @param u        io_uring state
 * @param sockfd   Socket file descriptor (must not be connected)
 * @param addr     Remote address to connect to
 * @param addrlen  Address structure size
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure
 */
static uint64_t ioUringSubmitConnect(IoUringState *u, int sockfd,
                                     const struct sockaddr *addr,
                                     socklen_t addrlen,
                                     loopyIoUringFileCallback *cb,
                                     void *userData) {
    struct io_uring_sqe *sqe = ioUringGetSqe(u);
    if (!sqe) {
        return 0;
    }

    /* Validate parameters before allocating operation resources */
    if (addrlen > 0 && !addr) {
        return 0;
    }

    uint64_t id = ioUringFileOpAllocate(u, cb, userData);
    if (!id) {
        return 0;
    }

    sqe->opcode = IORING_OP_CONNECT;
    sqe->fd = sockfd;
    sqe->addr = (uint64_t)(uintptr_t)addr;
    sqe->off = (uint64_t)addrlen;
    sqe->user_data = id;

    ioUringSubmitSqe(u);

    if (io_uring_enter(u->ringFd, 1, 0, 0, NULL) < 0) {
        loopyIoUringFileOp *op = ioUringFileOpFind(u, id);
        if (op) {
            op->active = false;
        }
        return 0;
    }

    return id;
}

/**
 * Submit a SHUTDOWN operation to io_uring.
 *
 * @param u        io_uring state
 * @param sockfd   Socket file descriptor
 * @param how      SHUT_RD, SHUT_WR, or SHUT_RDWR
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure
 */
static uint64_t ioUringSubmitShutdown(IoUringState *u, int sockfd, int how,
                                      loopyIoUringFileCallback *cb,
                                      void *userData) {
    struct io_uring_sqe *sqe = ioUringGetSqe(u);
    if (!sqe) {
        return 0;
    }

    uint64_t id = ioUringFileOpAllocate(u, cb, userData);
    if (!id) {
        return 0;
    }

    sqe->opcode = IORING_OP_SHUTDOWN;
    sqe->fd = sockfd;
    sqe->len = (uint32_t)how;
    sqe->user_data = id;

    ioUringSubmitSqe(u);

    if (io_uring_enter(u->ringFd, 1, 0, 0, NULL) < 0) {
        loopyIoUringFileOp *op = ioUringFileOpFind(u, id);
        if (op) {
            op->active = false;
        }
        return 0;
    }

    return id;
}

/**
 * Submit a SENDMSG operation to io_uring.
 *
 * @param u        io_uring state
 * @param sockfd   Socket file descriptor
 * @param msg      Message header (scatter/gather, ancillary data)
 * @param flags    Send flags
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure
 */
static uint64_t ioUringSubmitSendmsg(IoUringState *u, int sockfd,
                                     const struct msghdr *msg, int flags,
                                     loopyIoUringFileCallback *cb,
                                     void *userData) {
    struct io_uring_sqe *sqe = ioUringGetSqe(u);
    if (!sqe) {
        return 0;
    }

    /* Validate parameters before allocating operation resources */
    if (!msg) {
        return 0;
    }

    uint64_t id = ioUringFileOpAllocate(u, cb, userData);
    if (!id) {
        return 0;
    }

    sqe->opcode = IORING_OP_SENDMSG;
    sqe->fd = sockfd;
    sqe->addr = (uint64_t)(uintptr_t)msg;
    sqe->len = 1; /* Number of messages (always 1 for sendmsg) */
    sqe->msg_flags = (uint32_t)flags;
    sqe->user_data = id;

    ioUringSubmitSqe(u);

    if (io_uring_enter(u->ringFd, 1, 0, 0, NULL) < 0) {
        loopyIoUringFileOp *op = ioUringFileOpFind(u, id);
        if (op) {
            op->active = false;
        }
        return 0;
    }

    return id;
}

/**
 * Submit a RECVMSG operation to io_uring.
 *
 * @param u        io_uring state
 * @param sockfd   Socket file descriptor
 * @param msg      Message header (scatter/gather, ancillary data)
 * @param flags    Receive flags
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure
 */
static uint64_t ioUringSubmitRecvmsg(IoUringState *u, int sockfd,
                                     struct msghdr *msg, int flags,
                                     loopyIoUringFileCallback *cb,
                                     void *userData) {
    struct io_uring_sqe *sqe = ioUringGetSqe(u);
    if (!sqe) {
        return 0;
    }

    /* Validate parameters before allocating operation resources */
    if (!msg) {
        return 0;
    }

    uint64_t id = ioUringFileOpAllocate(u, cb, userData);
    if (!id) {
        return 0;
    }

    sqe->opcode = IORING_OP_RECVMSG;
    sqe->fd = sockfd;
    sqe->addr = (uint64_t)(uintptr_t)msg;
    sqe->len = 1; /* Number of messages (always 1 for recvmsg) */
    sqe->msg_flags = (uint32_t)flags;
    sqe->user_data = id;

    ioUringSubmitSqe(u);

    if (io_uring_enter(u->ringFd, 1, 0, 0, NULL) < 0) {
        loopyIoUringFileOp *op = ioUringFileOpFind(u, id);
        if (op) {
            op->active = false;
        }
        return 0;
    }

    return id;
}

static bool ioUringAddPoll(loopyInternalState *state, int fd, uint32_t events) {
    IoUringState *u = &state->uring;

    struct io_uring_sqe *sqe = ioUringGetSqe(u);
    if (!sqe) {
        return false;
    }

    sqe->opcode = IORING_OP_POLL_ADD;
    sqe->fd = fd;
    sqe->poll_events = events;
    /* Use fd directly for poll operations (bit 63 = 0 for poll, 1 for file ops) */
    sqe->user_data = (uint64_t)fd;

    ioUringSubmitSqe(u);

    /* Submit immediately */
    if (io_uring_enter(u->ringFd, 1, 0, 0, NULL) < 0) {
        return false;
    }

    return true;
}

static void ioUringRemovePoll(loopyInternalState *state, int fd) {
    IoUringState *u = &state->uring;

    struct io_uring_sqe *sqe = ioUringGetSqe(u);
    if (!sqe) {
        return;
    }

    sqe->opcode = IORING_OP_POLL_REMOVE;
    sqe->fd = -1;  /* Not used for POLL_REMOVE */
    sqe->addr = (uint64_t)fd;  /* Match the user_data of the poll to remove */
    sqe->user_data = 0;  /* Not meaningful for POLL_REMOVE */

    ioUringSubmitSqe(u);
    io_uring_enter(u->ringFd, 1, 0, 0, NULL);
}

/* ====================================================================
 * epoll implementation (fallback)
 * ==================================================================== */

static bool epollInit(loopyInternalState *state, size_t setSize) {
    EpollState *e = &state->epoll;

    e->events = zcalloc(setSize, sizeof(*e->events));
    if (!e->events) {
        return false;
    }

    e->epollFd = epoll_create(1024);
    if (e->epollFd == -1) {
        zfree(e->events);
        return false;
    }

    state->backend = BACKEND_EPOLL;
    return true;
}

static void epollFree(loopyInternalState *state) {
    EpollState *e = &state->epoll;

    if (e->epollFd >= 0) {
        close(e->epollFd);
    }

    zfree(e->events);
}

/* ====================================================================
 * Adapter interface implementation
 * ==================================================================== */

static bool loopyInternalNew(loopyLoop *l) {
    loopyInternalState *state = zcalloc(1, sizeof(*state));
    if (!state) {
        return false;
    }

    /* Try io_uring first, fall back to epoll */
#ifndef LOOPY_DISABLE_IOURING
    if (ioUringInit(state, l->setSize)) {
        l->state = state;
        return true;
    }
#endif

    /* io_uring failed or disabled, use epoll */
    if (epollInit(state, l->setSize)) {
        l->state = state;
        return true;
    }

    zfree(state);
    return false;
}

static bool loopyInternalResize(loopyLoop *l, size_t setSize) {
    loopyInternalState *state = l->state;

    if (state->backend == BACKEND_EPOLL) {
        state->epoll.events = zrealloc(state->epoll.events,
                                       sizeof(*state->epoll.events) * setSize);
        return true;
    }

    /* For io_uring, resize poll mask tracking */
    if (setSize > state->pollMasksSize) {
        state->pollMasks =
            zrealloc(state->pollMasks, sizeof(*state->pollMasks) * setSize);
        /* Zero out new entries */
        memset(state->pollMasks + state->pollMasksSize, 0,
               (setSize - state->pollMasksSize) * sizeof(*state->pollMasks));
        state->pollMasksSize = setSize;
    }

    return true;
}

static void loopyInternalFree(loopyLoop *l) {
    if (!l) {
        return;
    }

    if (!l->state) {
        return;
    }

    loopyInternalState *state = l->state;

    if (state->backend == BACKEND_IOURING) {
        ioUringFree(state);
    } else {
        epollFree(state);
    }

    zfree(state);
    l->state = NULL;  /* Clear state pointer to prevent double-free */
}

static bool loopyInternalAddEvent(loopyLoop *l, int fd, loopyAction mask) {
    loopyInternalState *state = l->state;

    if (state->backend == BACKEND_EPOLL) {
        EpollState *e = &state->epoll;
        struct epoll_event ee = {0};

        int op = l->events[fd].mask == LOOPY_ACTION_NONE ? EPOLL_CTL_ADD
                                                         : EPOLL_CTL_MOD;

        ee.events = 0;
        loopyAction fullMask = mask | l->events[fd].mask;
        if (loopyActionIsRead(fullMask)) {
            ee.events |= EPOLLIN;
        }
        if (loopyActionIsWrite(fullMask)) {
            ee.events |= EPOLLOUT;
        }
        ee.data.fd = fd;

        return epoll_ctl(e->epollFd, op, fd, &ee) != -1;
    }

    /* io_uring backend */
    uint32_t events = 0;
    loopyAction fullMask = mask | l->events[fd].mask;
    if (loopyActionIsRead(fullMask)) {
        events |= IORING_POLL_IN;
    }
    if (loopyActionIsWrite(fullMask)) {
        events |= IORING_POLL_OUT;
    }

    /* If already monitoring this fd, remove old poll first */
    if ((size_t)fd < state->pollMasksSize && state->pollMasks[fd]) {
        ioUringRemovePoll(state, fd);
    }

    if (!ioUringAddPoll(state, fd, events)) {
        return false;
    }

    if ((size_t)fd < state->pollMasksSize) {
        state->pollMasks[fd] = events;
    }

    return true;
}

static void loopyInternalDelEvent(loopyLoop *l, int fd, loopyAction delmask) {
    loopyInternalState *state = l->state;

    if (state->backend == BACKEND_EPOLL) {
        EpollState *e = &state->epoll;
        struct epoll_event ee = {0};
        loopyAction mask = l->events[fd].mask & (~delmask);

        ee.events = 0;
        if (loopyActionIsRead(mask)) {
            ee.events |= EPOLLIN;
        }
        if (loopyActionIsWrite(mask)) {
            ee.events |= EPOLLOUT;
        }
        ee.data.fd = fd;

        if (mask != LOOPY_ACTION_NONE) {
            epoll_ctl(e->epollFd, EPOLL_CTL_MOD, fd, &ee);
        } else {
            epoll_ctl(e->epollFd, EPOLL_CTL_DEL, fd, &ee);
        }
        return;
    }

    /* io_uring backend */
    loopyAction mask = l->events[fd].mask & (~delmask);

    /* Remove current poll */
    ioUringRemovePoll(state, fd);

    if (mask != LOOPY_ACTION_NONE) {
        /* Re-add with remaining events */
        uint32_t events = 0;
        if (loopyActionIsRead(mask)) {
            events |= IORING_POLL_IN;
        }
        if (loopyActionIsWrite(mask)) {
            events |= IORING_POLL_OUT;
        }
        ioUringAddPoll(state, fd, events);

        if ((size_t)fd < state->pollMasksSize) {
            state->pollMasks[fd] = events;
        }
    } else {
        if ((size_t)fd < state->pollMasksSize) {
            state->pollMasks[fd] = 0;
        }
    }
}

static int loopyInternalPoll(loopyLoop *l, const struct timeval *tvp) {
    loopyInternalState *state = l->state;

    if (state->backend == BACKEND_EPOLL) {
        EpollState *e = &state->epoll;
        int timeoutMs =
            tvp ? ((tvp->tv_sec * 1000) + (tvp->tv_usec / 1000)) : -1;

        int retval = epoll_wait(e->epollFd, e->events, l->setSize, timeoutMs);
        if (retval <= 0) {
            return 0;
        }

        for (int j = 0; j < retval; j++) {
            loopyAction mask = LOOPY_ACTION_NONE;
            struct epoll_event *ev = &e->events[j];

            if (ev->events & EPOLLIN) {
                mask |= LOOPY_ACTION_READ;
            }
            if (ev->events & EPOLLOUT) {
                mask |= LOOPY_ACTION_WRITE;
            }
            if (ev->events & EPOLLERR) {
                mask |= LOOPY_ACTION_WRITE;
            }
            if (ev->events & EPOLLHUP) {
                mask |= LOOPY_ACTION_WRITE;
            }

            l->fired[j].fd = ev->data.fd;
            l->fired[j].mask = mask;
        }

        return retval;
    }

    /* io_uring backend */
    IoUringState *u = &state->uring;

    /* Calculate timeout */
    int timeoutMs = tvp ? ((tvp->tv_sec * 1000) + (tvp->tv_usec / 1000)) : -1;

    /* First check if completions are already available (avoid syscall) */
    uint32_t head = __atomic_load_n(u->cqHead, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(u->cqTail, __ATOMIC_ACQUIRE);

    /* If no completions and we need to wait, use ppoll on ring fd */
    if (head == tail && timeoutMs != 0) {
        struct timespec ts;
        struct timespec *tsp = NULL;

        if (timeoutMs > 0) {
            ts.tv_sec = timeoutMs / 1000;
            ts.tv_nsec = (timeoutMs % 1000) * 1000000;
            tsp = &ts;
        }

        struct pollfd pfd = {
            .fd = u->ringFd,
            .events = POLLIN,
        };
        ppoll(&pfd, 1, tsp, NULL);
    }

    /* Get completions from kernel - submit any pending + retrieve completions */
    uint32_t pending = *u->sqTail - __atomic_load_n(u->sqHead, __ATOMIC_ACQUIRE);
    int ret = io_uring_enter(u->ringFd, pending, 0, IORING_ENTER_GETEVENTS, NULL);
    if (ret < 0 && errno != EINTR && errno != EAGAIN) {
        return 0;
    }

    /* Process completion queue */
    int numevents = 0;
    head = __atomic_load_n(u->cqHead, __ATOMIC_ACQUIRE);
    tail = __atomic_load_n(u->cqTail, __ATOMIC_ACQUIRE);

    while (head != tail && numevents < (int)l->setSize) {
        const struct io_uring_cqe *cqe = &u->cqes[head & *u->cqRingMask];
        uint64_t user_data = cqe->user_data;

        /* Check bit 63 to distinguish file operations (bit 63=1) from poll (bit 63=0) */
        bool isFileOp = (user_data & (1ULL << 63)) != 0;

        if (isFileOp) {
            /* File operation completion - mask off bit 63 to get actual ID */
            uint64_t fileOpId = user_data & ~(1ULL << 63);
            loopyIoUringFileOp *fileOp = ioUringFileOpFind(u, fileOpId);
            if (fileOp) {
                /* File operation completion */
                bool hasMore = (cqe->flags & IORING_CQE_F_MORE) != 0;

                if (fileOp->multishot && hasMore) {
                    /* Multishot operation with more completions coming.
                     * Invoke callback but DON'T free the operation - it will
                     * complete again for the next connection/event. */
                    if (fileOp->cb) {
                        fileOp->cb(fileOp->userData, cqe->res);
                    }
                    /* Operation stays active and will complete again */
                } else {
                    /* Final completion (no CQE_F_MORE) or non-multishot operation.
                     * This is the standard path and will free the operation. */
                    ioUringFileOpComplete(fileOp, cqe->res);
                }
            }
        } else {
            /* Poll operation completion */
            int fd = (int)user_data;

            if (cqe->res >= 0 || cqe->res == -ECANCELED) {
                loopyAction mask = LOOPY_ACTION_NONE;
                int events = cqe->res;

                /* For poll completion, res contains the events */
                if (events & IORING_POLL_IN) {
                    mask |= LOOPY_ACTION_READ;
                }
                if (events & IORING_POLL_OUT) {
                    mask |= LOOPY_ACTION_WRITE;
                }
                if (events & IORING_POLL_ERR) {
                    mask |= LOOPY_ACTION_WRITE;
                }
                if (events & IORING_POLL_HUP) {
                    mask |= LOOPY_ACTION_WRITE;
                }

                if (mask != LOOPY_ACTION_NONE) {
                    l->fired[numevents].fd = fd;
                    l->fired[numevents].mask = mask;
                    numevents++;

                    /* Re-arm the poll for this fd (io_uring poll is one-shot)
                     */
                    uint32_t pollEvents = 0;
                    if ((size_t)fd < state->pollMasksSize) {
                        pollEvents = state->pollMasks[fd];
                    }
                    if (pollEvents) {
                        ioUringAddPoll(state, fd, pollEvents);
                    }
                }
            }
        }

        head++;
    }

    /* Update completion queue head */
    __atomic_store_n(u->cqHead, head, __ATOMIC_RELEASE);

    return numevents;
}

static char *loopyInternalName(void) {
    /* Note: This is called before we know which backend we're using,
     * so we return a generic name. Use loopyAdapterBackend() for runtime check.
     */
    return "io_uring/epoll";
}

/* ====================================================================
 * Public API for backend detection
 * ==================================================================== */

/* Check if io_uring is being used (can be called after loop init) */
bool loopyUsingIoUring(const loopyLoop *l) {
    if (!l || !l->state) {
        return false;
    }
    const loopyInternalState *state = l->state;
    return state->backend == BACKEND_IOURING;
}

/* ====================================================================
 * Public API for io_uring file operations
 * ==================================================================== */

/**
 * Submit a READ operation via io_uring (if available).
 *
 * @param l        Event loop
 * @param fd       File descriptor
 * @param buf      Buffer to read into
 * @param len      Bytes to read
 * @param offset   File offset (-1 for current)
 * @param cb       Completion callback
 * @param userData User data
 * @return Operation ID on success, 0 on failure or if io_uring not available
 */
uint64_t loopyIoUringRead(loopyLoop *l, int fd, void *buf, size_t len,
                          off_t offset, loopyIoUringFileCallback *cb,
                          void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitRead(&state->uring, fd, buf, len, offset, cb, userData);
}

/**
 * Submit a WRITE operation via io_uring (if available).
 *
 * @param l        Event loop
 * @param fd       File descriptor
 * @param buf      Buffer to write from
 * @param len      Bytes to write
 * @param offset   File offset (-1 for current)
 * @param cb       Completion callback
 * @param userData User data
 * @return Operation ID on success, 0 on failure or if io_uring not available
 */
uint64_t loopyIoUringWrite(loopyLoop *l, int fd, const void *buf, size_t len,
                           off_t offset, loopyIoUringFileCallback *cb,
                           void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitWrite(&state->uring, fd, buf, len, offset, cb,
                              userData);
}

/**
 * Submit an OPENAT operation via io_uring (if available).
 *
 * @param l        Event loop
 * @param dirfd    Directory fd (AT_FDCWD for cwd)
 * @param path     Path to open
 * @param flags    Open flags
 * @param mode     File mode
 * @param cb       Completion callback
 * @param userData User data
 * @return Operation ID on success, 0 on failure or if io_uring not available
 */
uint64_t loopyIoUringOpenat(loopyLoop *l, int dirfd, const char *path,
                            int flags, mode_t mode,
                            loopyIoUringFileCallback *cb, void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitOpenat(&state->uring, dirfd, path, flags, mode, cb,
                               userData);
}

/**
 * Submit a CLOSE operation via io_uring (if available).
 *
 * @param l        Event loop
 * @param fd       File descriptor to close
 * @param cb       Completion callback
 * @param userData User data
 * @return Operation ID on success, 0 on failure or if io_uring not available
 */
uint64_t loopyIoUringClose(loopyLoop *l, int fd, loopyIoUringFileCallback *cb,
                           void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitClose(&state->uring, fd, cb, userData);
}

/**
 * Submit an FSYNC operation via io_uring (if available).
 *
 * @param l        Event loop
 * @param fd       File descriptor
 * @param datasync true for fdatasync, false for fsync
 * @param cb       Completion callback
 * @param userData User data
 * @return Operation ID on success, 0 on failure or if io_uring not available
 */
uint64_t loopyIoUringFsync(loopyLoop *l, int fd, bool datasync,
                           loopyIoUringFileCallback *cb, void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitFsync(&state->uring, fd, datasync, cb, userData);
}

/* ====================================================================
 * Fixed Buffer Registration API
 * ==================================================================== */

/**
 * Register fixed buffers with io_uring for zero-copy operations.
 *
 * @param l       Event loop
 * @param buffers Array of iovec structures describing buffers
 * @param count   Number of buffers (max 1024)
 * @return true on success, false on failure or if io_uring not available
 */
bool loopyIoUringRegisterBuffers(loopyLoop *l, struct iovec *buffers,
                                 uint32_t count) {
    if (!l || !l->state || !buffers || count == 0 || count > 1024) {
        return false;
    }

    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return false;
    }

    IoUringState *u = &state->uring;

    /* Check if buffers already registered */
    if (u->buffersRegistered) {
        return false; /* Must unregister first */
    }

    /* Register buffers with kernel */
    int ret =
        io_uring_register(u->ringFd, IORING_REGISTER_BUFFERS, buffers, count);
    if (ret < 0) {
        return false;
    }

    /* Store registration info */
    u->registeredBuffers = buffers;
    u->numRegisteredBuffers = count;
    u->buffersRegistered = true;

    return true;
}

/**
 * Unregister previously registered fixed buffers.
 *
 * @param l Event loop
 * @return true on success, false on failure or if no buffers registered
 */
bool loopyIoUringUnregisterBuffers(loopyLoop *l) {
    if (!l || !l->state) {
        return false;
    }

    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return false;
    }

    IoUringState *u = &state->uring;

    if (!u->buffersRegistered) {
        return false; /* No buffers to unregister */
    }

    /* Unregister with kernel */
    int ret = io_uring_register(u->ringFd, IORING_UNREGISTER_BUFFERS, NULL, 0);
    if (ret < 0) {
        return false;
    }

    /* Clear registration info */
    u->registeredBuffers = NULL;
    u->numRegisteredBuffers = 0;
    u->buffersRegistered = false;

    return true;
}

/**
 * Check if fixed buffers are currently registered.
 *
 * @param l Event loop
 * @return true if buffers are registered, false otherwise
 */
bool loopyIoUringHasFixedBuffers(loopyLoop *l) {
    if (!l || !l->state) {
        return false;
    }

    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return false;
    }

    return state->uring.buffersRegistered;
}

/**
 * Submit a READ operation using a fixed buffer (by index).
 *
 * @param l         Event loop
 * @param fd        File descriptor to read from
 * @param bufIndex  Index of registered buffer
 * @param len       Number of bytes to read
 * @param offset    File offset to read from (-1 for current position)
 * @param cb        Completion callback
 * @param userData  User data for callback
 * @return Operation ID on success, 0 on failure or if buffers not registered
 */
uint64_t loopyIoUringReadFixed(loopyLoop *l, int fd, uint32_t bufIndex,
                               size_t len, off_t offset,
                               loopyIoUringFileCallback *cb, void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitReadFixed(&state->uring, fd, bufIndex, len, offset, cb,
                                  userData);
}

/**
 * Submit a WRITE operation using a fixed buffer (by index).
 *
 * @param l         Event loop
 * @param fd        File descriptor to write to
 * @param bufIndex  Index of registered buffer
 * @param len       Number of bytes to write
 * @param offset    File offset to write to (-1 for current position)
 * @param cb        Completion callback
 * @param userData  User data for callback
 * @return Operation ID on success, 0 on failure or if buffers not registered
 */
uint64_t loopyIoUringWriteFixed(loopyLoop *l, int fd, uint32_t bufIndex,
                                size_t len, off_t offset,
                                loopyIoUringFileCallback *cb, void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitWriteFixed(&state->uring, fd, bufIndex, len, offset, cb,
                                   userData);
}

/* ====================================================================
 * Fixed File Registration API
 * ==================================================================== */

/**
 * Register file descriptors with io_uring for zero-overhead operations.
 *
 * Fixed files are registered once and then referenced by index in subsequent
 * I/O operations, eliminating file descriptor lookup overhead.
 *
 * @param l      Event loop
 * @param files  Array of file descriptors to register
 * @param count  Number of file descriptors in array (max 1024)
 * @return true on success, false on failure or if io_uring not available
 */
bool loopyIoUringRegisterFiles(loopyLoop *l, int *files, uint32_t count) {
    if (!l || !l->state) {
        return false;
    }

    /* Validate input */
    if (!files || count == 0 || count > 1024) {
        return false; /* Invalid array or count exceeds kernel limit */
    }

    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return false;
    }

    IoUringState *u = &state->uring;

    if (u->filesRegistered) {
        return false; /* Already registered; must unregister first */
    }

    /* Register with kernel */
    int ret = io_uring_register(u->ringFd, IORING_REGISTER_FILES, files, count);
    if (ret < 0) {
        return false;
    }

    /* Store registration info */
    u->registeredFiles = files;
    u->numRegisteredFiles = count;
    u->filesRegistered = true;

    return true;
}

/**
 * Unregister previously registered file descriptors.
 *
 * @param l Event loop
 * @return true on success, false on failure or if no files registered
 */
bool loopyIoUringUnregisterFiles(loopyLoop *l) {
    if (!l || !l->state) {
        return false;
    }

    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return false;
    }

    IoUringState *u = &state->uring;

    if (!u->filesRegistered) {
        return false; /* No files to unregister */
    }

    /* Unregister with kernel */
    int ret = io_uring_register(u->ringFd, IORING_UNREGISTER_FILES, NULL, 0);
    if (ret < 0) {
        return false;
    }

    /* Clear registration info */
    u->registeredFiles = NULL;
    u->numRegisteredFiles = 0;
    u->filesRegistered = false;

    return true;
}

/**
 * Check if fixed files are currently registered.
 *
 * @param l Event loop
 * @return true if files are registered, false otherwise
 */
bool loopyIoUringHasFixedFiles(loopyLoop *l) {
    if (!l || !l->state) {
        return false;
    }

    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return false;
    }

    return state->uring.filesRegistered;
}

/**
 * Submit a READ operation using a registered fixed file (by index).
 *
 * @param l         Event loop
 * @param fileIndex Index of registered file (0 to count-1)
 * @param buf       Buffer to read into (must remain valid until completion)
 * @param len       Number of bytes to read
 * @param offset    File offset to read from (-1 for current position)
 * @param cb        Completion callback
 * @param userData  User data for callback
 * @return Operation ID on success, 0 on failure or if files not registered
 */
uint64_t loopyIoUringReadFixedFile(loopyLoop *l, uint32_t fileIndex, void *buf,
                                   size_t len, off_t offset,
                                   loopyIoUringFileCallback *cb,
                                   void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitReadFixedFile(&state->uring, fileIndex, buf, len,
                                      offset, cb, userData);
}

/**
 * Submit a WRITE operation using a registered fixed file (by index).
 *
 * @param l         Event loop
 * @param fileIndex Index of registered file (0 to count-1)
 * @param buf       Buffer to write from (must remain valid until completion)
 * @param len       Number of bytes to write
 * @param offset    File offset to write to (-1 for current position)
 * @param cb        Completion callback
 * @param userData  User data for callback
 * @return Operation ID on success, 0 on failure or if files not registered
 */
uint64_t loopyIoUringWriteFixedFile(loopyLoop *l, uint32_t fileIndex,
                                    const void *buf, size_t len, off_t offset,
                                    loopyIoUringFileCallback *cb,
                                    void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitWriteFixedFile(&state->uring, fileIndex, buf, len,
                                       offset, cb, userData);
}

/* ====================================================================
 * Public API for io_uring network operations
 * ==================================================================== */

/**
 * Submit a SEND operation via io_uring (if available).
 */
uint64_t loopyIoUringSend(loopyLoop *l, int sockfd, const void *buf, size_t len,
                          int flags, loopyIoUringNetCallback *cb,
                          void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    /* Cast between compatible callback types */
    return ioUringSubmitSend(&state->uring, sockfd, buf, len, flags,
                             (loopyIoUringFileCallback *)cb, userData);
}

/**
 * Submit a RECV operation via io_uring (if available).
 */
uint64_t loopyIoUringRecv(loopyLoop *l, int sockfd, void *buf, size_t len,
                          int flags, loopyIoUringNetCallback *cb,
                          void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitRecv(&state->uring, sockfd, buf, len, flags,
                             (loopyIoUringFileCallback *)cb, userData);
}

/**
 * Submit an ACCEPT operation via io_uring (if available).
 */
uint64_t loopyIoUringAccept(loopyLoop *l, int sockfd, struct sockaddr *addr,
                            socklen_t *addrlen, int flags,
                            loopyIoUringNetCallback *cb, void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitAccept(&state->uring, sockfd, addr, addrlen, flags,
                               (loopyIoUringFileCallback *)cb, userData);
}

/**
 * Submit a CONNECT operation via io_uring (if available).
 */
uint64_t loopyIoUringConnect(loopyLoop *l, int sockfd,
                             const struct sockaddr *addr, socklen_t addrlen,
                             loopyIoUringNetCallback *cb, void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitConnect(&state->uring, sockfd, addr, addrlen,
                                (loopyIoUringFileCallback *)cb, userData);
}

/**
 * Submit a SHUTDOWN operation via io_uring (if available).
 */
uint64_t loopyIoUringShutdown(loopyLoop *l, int sockfd, int how,
                              loopyIoUringNetCallback *cb, void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitShutdown(&state->uring, sockfd, how,
                                 (loopyIoUringFileCallback *)cb, userData);
}

/**
 * Submit a SENDMSG operation via io_uring (if available).
 */
uint64_t loopyIoUringSendmsg(loopyLoop *l, int sockfd, const struct msghdr *msg,
                             int flags, loopyIoUringNetCallback *cb,
                             void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitSendmsg(&state->uring, sockfd, msg, flags,
                                (loopyIoUringFileCallback *)cb, userData);
}

/**
 * Submit a RECVMSG operation via io_uring (if available).
 */
uint64_t loopyIoUringRecvmsg(loopyLoop *l, int sockfd, struct msghdr *msg,
                             int flags, loopyIoUringNetCallback *cb,
                             void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitRecvmsg(&state->uring, sockfd, msg, flags,
                                (loopyIoUringFileCallback *)cb, userData);
}

/**
 * Check if io_uring network operations are available.
 */
bool loopyIoUringNetAvailable(const loopyLoop *l) {
    return loopyUsingIoUring(l);
}

/**
 * Check if multishot accept is available.
 *
 * Multishot accept requires Linux 5.19+ and io_uring support.
 * We assume multishot is available if io_uring is working - if submission
 * fails on older kernels, the operation returns 0 (graceful degradation).
 *
 * @param l Event loop
 * @return true if io_uring is active (multishot may be available)
 */
bool loopyIoUringNetHasMultishot(const loopyLoop *l) {
    if (!l || !l->state) {
        return false;
    }
    const loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return false;
    }

    /* Multishot accept (IORING_ACCEPT_MULTISHOT) requires:
     * 1. io_uring backend active
     * 2. Linux kernel 5.19+ (can't easily detect, assume if io_uring works)
     *
     * We return true if io_uring is available. If the kernel doesn't support
     * multishot, the submission will fail gracefully (returns 0). */
    return true;
}

/**
 * Submit a multishot ACCEPT operation via io_uring.
 *
 * Multishot accept (Linux 5.19+) is a high-performance mode where a single
 * IORING_OP_ACCEPT operation stays active and completes multiple times - once
 * for each new connection. This eliminates accept() syscalls entirely and can
 * achieve 10x throughput improvement (500K+ connections/sec vs ~50K).
 *
 * The callback will be invoked repeatedly, once for each new connection.
 * Each invocation receives the new client socket fd in the result parameter.
 *
 * The operation continues until:
 *  - An error occurs (result < 0)
 *  - The listening socket is closed
 *  - The operation is explicitly cancelled via loopyIoUringNetCancel()
 *
 * @param l        Event loop
 * @param sockfd   Listening socket file descriptor
 * @param flags    Accept flags (SOCK_NONBLOCK, SOCK_CLOEXEC, etc.)
 * @param cb       Completion callback (called for each new connection)
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure or if not available
 */
uint64_t loopyIoUringAcceptMultishot(loopyLoop *l, int sockfd, int flags,
                                     loopyIoUringNetCallback *cb,
                                     void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }

    IoUringState *u = &state->uring;
    return ioUringSubmitAcceptMultishot(u, sockfd, flags, cb, userData);
}

/**
 * Cancel a pending io_uring network operation.
 *
 * Submits an IORING_OP_ASYNC_CANCEL request to cancel an in-flight operation.
 * This is a best-effort operation - the target operation may complete normally
 * if it finishes before the cancellation is processed.
 *
 * When cancellation succeeds, the target operation's callback will be invoked
 * with result = -ECANCELED.
 *
 * @param l    Event loop
 * @param opId Operation ID to cancel (returned from submit function)
 * @return true if cancellation request was submitted, false on error
 */
bool loopyIoUringNetCancel(loopyLoop *l, uint64_t opId) {
    if (!l || !l->state) {
        return false;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return false;
    }

    IoUringState *u = &state->uring;

    /* Find the operation to ensure it exists and is active */
    const loopyIoUringFileOp *op = ioUringFileOpFind(u, opId);
    if (!op) {
        /* Operation not found or already completed */
        return false;
    }

    /* Get an SQE for the cancel operation */
    struct io_uring_sqe *sqe = ioUringGetSqe(u);
    if (!sqe) {
        return false;
    }

    /* Submit IORING_OP_ASYNC_CANCEL
     * sqe->addr contains the user_data of the operation to cancel */
    sqe->opcode = IORING_OP_ASYNC_CANCEL;
    sqe->addr = opId;   /* user_data of operation to cancel */
    sqe->user_data = 0; /* Don't track the cancel operation itself */

    ioUringSubmitSqe(u);

    /* Submit the cancel request to the kernel */
    if (io_uring_enter(u->ringFd, 1, 0, 0, NULL) < 0) {
        return false;
    }

    return true;
}

/* ====================================================================
 * Linked Operations (Operation Chaining)
 * ==================================================================== */

/**
 * Operation type for link chain
 */
typedef enum {
    LINK_OP_READ = 0,
    LINK_OP_WRITE,
    LINK_OP_OPENAT,
    LINK_OP_CLOSE,
    LINK_OP_FSYNC
} LinkOpType;

/**
 * Single operation in a link chain
 */
typedef struct {
    LinkOpType type;
    loopyIoUringFileCallback *cb;
    void *userData;

    /* Operation-specific parameters */
    union {
        struct {
            int fd;
            void *buf;
            size_t len;
            off_t offset;
        } read;

        struct {
            int fd;
            const void *buf;
            size_t len;
            off_t offset;
        } write;

        struct {
            int dirfd;
            const char *path;
            int flags;
            mode_t mode;
        } openat;

        struct {
            int fd;
        } close;

        struct {
            int fd;
            bool datasync;
        } fsync;
    } params;
} LinkOp;

/**
 * Link chain structure
 */
struct loopyIoUringLinkChain {
    loopyLoop *loop;           /* Event loop */
    loopyIoUringLinkMode mode; /* Link mode (soft/hard) */
    LinkOp *ops;               /* Array of operations */
    uint32_t count;            /* Number of operations */
    uint32_t capacity;         /* Allocated capacity */
};

/**
 * Create a new linked operation chain.
 */
loopyIoUringLinkChain *loopyIoUringLinkChainNew(loopyLoop *l,
                                                loopyIoUringLinkMode mode) {
    if (!l || !l->state) {
        return NULL;
    }

    const loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return NULL;
    }

    loopyIoUringLinkChain *chain = zcalloc(1, sizeof(loopyIoUringLinkChain));
    if (!chain) {
        return NULL;
    }

    chain->loop = l;
    chain->mode = mode;
    chain->count = 0;
    chain->capacity = 8; /* Initial capacity */

    chain->ops = zcalloc(chain->capacity, sizeof(LinkOp));
    if (!chain->ops) {
        zfree(chain);
        return NULL;
    }

    return chain;
}

/**
 * Ensure capacity for one more operation
 */
static bool linkChainEnsureCapacity(loopyIoUringLinkChain *chain) {
    if (chain->count < chain->capacity) {
        return true;
    }

    uint32_t newCapacity = chain->capacity * 2;
    LinkOp *newOps = zrealloc(chain->ops, newCapacity * sizeof(LinkOp));
    if (!newOps) {
        return false;
    }

    chain->ops = newOps;
    chain->capacity = newCapacity;
    return true;
}

/**
 * Add a READ operation to the link chain.
 */
loopyIoUringLinkChain *loopyIoUringLinkChainRead(loopyIoUringLinkChain *chain,
                                                 int fd, void *buf, size_t len,
                                                 off_t offset,
                                                 loopyIoUringFileCallback *cb,
                                                 void *userData) {
    if (!chain || !linkChainEnsureCapacity(chain)) {
        return NULL;
    }

    LinkOp *op = &chain->ops[chain->count++];
    op->type = LINK_OP_READ;
    op->cb = cb;
    op->userData = userData;
    op->params.read.fd = fd;
    op->params.read.buf = buf;
    op->params.read.len = len;
    op->params.read.offset = offset;

    return chain;
}

/**
 * Add a WRITE operation to the link chain.
 */
loopyIoUringLinkChain *loopyIoUringLinkChainWrite(loopyIoUringLinkChain *chain,
                                                  int fd, const void *buf,
                                                  size_t len, off_t offset,
                                                  loopyIoUringFileCallback *cb,
                                                  void *userData) {
    if (!chain || !linkChainEnsureCapacity(chain)) {
        return NULL;
    }

    LinkOp *op = &chain->ops[chain->count++];
    op->type = LINK_OP_WRITE;
    op->cb = cb;
    op->userData = userData;
    op->params.write.fd = fd;
    op->params.write.buf = buf;
    op->params.write.len = len;
    op->params.write.offset = offset;

    return chain;
}

/**
 * Add an OPENAT operation to the link chain.
 */
loopyIoUringLinkChain *loopyIoUringLinkChainOpenat(loopyIoUringLinkChain *chain,
                                                   int dirfd, const char *path,
                                                   int flags, mode_t mode,
                                                   loopyIoUringFileCallback *cb,
                                                   void *userData) {
    if (!chain || !linkChainEnsureCapacity(chain)) {
        return NULL;
    }

    LinkOp *op = &chain->ops[chain->count++];
    op->type = LINK_OP_OPENAT;
    op->cb = cb;
    op->userData = userData;
    op->params.openat.dirfd = dirfd;
    op->params.openat.path = path;
    op->params.openat.flags = flags;
    op->params.openat.mode = mode;

    return chain;
}

/**
 * Add a CLOSE operation to the link chain.
 */
loopyIoUringLinkChain *loopyIoUringLinkChainClose(loopyIoUringLinkChain *chain,
                                                  int fd,
                                                  loopyIoUringFileCallback *cb,
                                                  void *userData) {
    if (!chain || !linkChainEnsureCapacity(chain)) {
        return NULL;
    }

    LinkOp *op = &chain->ops[chain->count++];
    op->type = LINK_OP_CLOSE;
    op->cb = cb;
    op->userData = userData;
    op->params.close.fd = fd;

    return chain;
}

/**
 * Add an FSYNC operation to the link chain.
 */
loopyIoUringLinkChain *loopyIoUringLinkChainFsync(loopyIoUringLinkChain *chain,
                                                  int fd, bool datasync,
                                                  loopyIoUringFileCallback *cb,
                                                  void *userData) {
    if (!chain || !linkChainEnsureCapacity(chain)) {
        return NULL;
    }

    LinkOp *op = &chain->ops[chain->count++];
    op->type = LINK_OP_FSYNC;
    op->cb = cb;
    op->userData = userData;
    op->params.fsync.fd = fd;
    op->params.fsync.datasync = datasync;

    return chain;
}

/**
 * Submit all operations in the link chain atomically.
 */
bool loopyIoUringLinkChainSubmit(loopyIoUringLinkChain *chain) {
    if (!chain || chain->count == 0) {
        if (chain) {
            loopyIoUringLinkChainFree(chain);
        }
        return false;
    }

    loopyInternalState *state = chain->loop->state;
    IoUringState *u = &state->uring;

    /* Determine the link flag based on mode */
    uint8_t linkFlag = (chain->mode == LOOPY_IOURING_LINK_HARD)
                           ? IOSQE_IO_HARDLINK
                           : IOSQE_IO_LINK;

    /* Submit each operation in the chain */
    for (uint32_t i = 0; i < chain->count; i++) {
        LinkOp *op = &chain->ops[i];
        struct io_uring_sqe *sqe = ioUringGetSqe(u);

        if (!sqe) {
            /* SQ full - clean up and fail */
            loopyIoUringLinkChainFree(chain);
            return false;
        }

        /* Allocate an operation ID for tracking */
        uint64_t opId = ioUringFileOpAllocate(u, op->cb, op->userData);
        if (!opId) {
            loopyIoUringLinkChainFree(chain);
            return false;
        }

        /* Fill the SQE based on operation type */
        switch (op->type) {
        case LINK_OP_READ:
            sqe->opcode = IORING_OP_READ;
            sqe->fd = op->params.read.fd;
            sqe->addr = (uint64_t)(uintptr_t)op->params.read.buf;
            sqe->len = (uint32_t)op->params.read.len;
            sqe->off = (uint64_t)op->params.read.offset;
            break;

        case LINK_OP_WRITE:
            sqe->opcode = IORING_OP_WRITE;
            sqe->fd = op->params.write.fd;
            sqe->addr = (uint64_t)(uintptr_t)op->params.write.buf;
            sqe->len = (uint32_t)op->params.write.len;
            sqe->off = (uint64_t)op->params.write.offset;
            break;

        case LINK_OP_OPENAT:
            sqe->opcode = IORING_OP_OPENAT;
            sqe->fd = op->params.openat.dirfd;
            sqe->addr = (uint64_t)(uintptr_t)op->params.openat.path;
            sqe->len = (uint32_t)op->params.openat.mode;
            sqe->open_flags = (uint32_t)op->params.openat.flags;
            break;

        case LINK_OP_CLOSE:
            sqe->opcode = IORING_OP_CLOSE;
            sqe->fd = op->params.close.fd;
            break;

        case LINK_OP_FSYNC:
            sqe->opcode = IORING_OP_FSYNC;
            sqe->fd = op->params.fsync.fd;
            sqe->fsync_flags =
                op->params.fsync.datasync ? IORING_FSYNC_DATASYNC : 0;
            break;
        }

        sqe->user_data = opId;

        /* Set link flag on all operations except the last one */
        if (i < chain->count - 1) {
            sqe->flags = linkFlag;
        } else {
            sqe->flags = 0;
        }

        ioUringSubmitSqe(u);
    }

    /* Submit all operations to the kernel in one syscall */
    int ret = io_uring_enter(u->ringFd, chain->count, 0, 0, NULL);

    /* Free the chain regardless of success */
    loopyIoUringLinkChainFree(chain);

    return ret >= 0;
}

/**
 * Discard a link chain without submitting.
 */
void loopyIoUringLinkChainFree(loopyIoUringLinkChain *chain) {
    if (!chain) {
        return;
    }

    zfree(chain->ops);
    zfree(chain);
}

/**
 * Get the number of operations currently in the link chain.
 */
uint32_t loopyIoUringLinkChainLength(const loopyIoUringLinkChain *chain) {
    return chain ? chain->count : 0;
}

#else /* !__linux__ */

/* Non-Linux stub - this file should not be compiled on non-Linux */
#error "loopyAdapterIouring.c should only be compiled on Linux"

#endif /* __linux__ */
