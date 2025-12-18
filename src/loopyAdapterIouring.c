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

#include <linux/futex.h> /* For struct futex_waitv */
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
#define IORING_OP_SYMLINKAT 38
#define IORING_OP_LINKAT 39
#define IORING_OP_MSG_RING 40
#define IORING_OP_FSETXATTR 41
#define IORING_OP_SETXATTR 42
#define IORING_OP_FGETXATTR 43
#define IORING_OP_GETXATTR 44
#define IORING_OP_SOCKET 45
#define IORING_OP_URING_CMD 46
#define IORING_OP_SEND_ZC 47
#define IORING_OP_SENDMSG_ZC 48
#define IORING_OP_READ_MULTISHOT 49
#define IORING_OP_WAITID 50
#define IORING_OP_FUTEX_WAIT 51
#define IORING_OP_FUTEX_WAKE 52
#define IORING_OP_FUTEX_WAITV 53
#define IORING_OP_FTRUNCATE 54
#define IORING_OP_BIND 55
#define IORING_OP_LISTEN 56
#define IORING_OP_RECV_ZC 57
#define IORING_OP_FIXED_FD_INSTALL 57
#define IORING_OP_FSETXATTR2 58
#define IORING_OP_SETXATTR2 59
#define IORING_OP_FGETXATTR2 60
#define IORING_OP_GETXATTR2 61
#define IORING_OP_GETDENTS 62
#define IORING_OP_SEND_BUNDLE 63
#define IORING_OP_EPOLL_WAIT 68

#ifndef IORING_OP_FIXED_FD_INSTALL
#define IORING_OP_FIXED_FD_INSTALL 54
#endif

/* Vector operations with fixed buffers */
#ifndef IORING_OP_READ_FIXED
#define IORING_OP_READ_FIXED 4
#endif
#ifndef IORING_OP_WRITE_FIXED
#define IORING_OP_WRITE_FIXED 5
#endif
#ifndef IORING_OP_READV_FIXED
/* READV with registered buffers (Linux 5.10+) */
#define IORING_OP_READV_FIXED 60
#endif
#ifndef IORING_OP_WRITEV_FIXED
/* WRITEV with registered buffers (Linux 5.10+) */
#define IORING_OP_WRITEV_FIXED 61
#endif

/* Poll events (same as EPOLL) */
#define IORING_POLL_IN EPOLLIN
#define IORING_POLL_OUT EPOLLOUT
#define IORING_POLL_ERR EPOLLERR
#define IORING_POLL_HUP EPOLLHUP

/* Poll update flags (for POLL_REMOVE with update) */
#ifndef IORING_POLL_UPDATE_EVENTS
#define IORING_POLL_UPDATE_EVENTS (1U << 0) /* Update poll mask */
#endif
#ifndef IORING_POLL_UPDATE_USER_DATA
#define IORING_POLL_UPDATE_USER_DATA (1U << 1) /* Update user_data */
#endif

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
#define IORING_TIMEOUT_UPDATE (1U << 1)

/* SQE flags */
#define IOSQE_FIXED_FILE (1U << 0)  /* Use fixed file index instead of fd */
#define IOSQE_IO_DRAIN (1U << 1)    /* Drain previously submitted operations */
#define IOSQE_IO_LINK (1U << 2)     /* Link next SQE (fails -> cancel chain) */
#define IOSQE_IO_HARDLINK (1U << 3) /* Like LINK but continues on error */
#ifndef IOSQE_BUFFER_SELECT
#define IOSQE_BUFFER_SELECT (1U << 6) /* Select buffer from provided pool */
#endif

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
        uint32_t rename_flags;
        uint32_t unlink_flags;
        uint32_t xattr_flags;
        uint32_t msg_ring_flags;
        uint32_t futex_flags;
        uint32_t install_fd_flags;
        uint32_t pipe_flags;
    };
    uint64_t user_data;
    union {
        uint16_t buf_index; /* For READ_FIXED/WRITE_FIXED operations */
        uint16_t buf_group; /* For PROVIDE_BUFFERS and buffer selection */
    } __attribute__((packed));
    uint16_t personality;
    union {
        int32_t splice_fd_in;
        uint32_t file_index;
        struct {
            uint16_t addr_len;
            uint16_t __pad3[1];
        };
    };
    union {
        struct {
            uint64_t addr3;
            uint64_t __pad2[1];
        };
        uint64_t optval;
    };
};

/* Compile-time size verification */
_Static_assert(sizeof(struct io_uring_sqe) == 64,
               "io_uring_sqe must be exactly 64 bytes");

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
 * Buffer pool callback type (receives buffer info on completion).
 */
typedef void loopyIoUringBufferCallback(void *userData, int32_t result,
                                        uint16_t bufferId, void *bufferAddr,
                                        size_t bufferSize);

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
    /* Buffer pool support */
    loopyIoUringBufferCallback *bufferCb; /* Buffer-aware callback (or NULL) */
    uint16_t bufferGroup; /* Buffer group ID for buffer-selected ops */
} loopyIoUringFileOp;

/**
 * File operation tracking state.
 */
typedef struct loopyIoUringFileState {
    loopyIoUringFileOp *ops; /* Array of pending operations */
    size_t opsSize;          /* Size of ops array */
    uint64_t nextId;         /* Next operation ID */
} loopyIoUringFileState;

/* Buffer pool structure (needed for CQE processing) */
struct loopyIoUringBufferPool {
    loopyLoop *loop;
    void *buffers;
    uint32_t bufferSize;
    uint32_t bufferCount;
    uint16_t groupId;
    bool registered;
};
typedef struct loopyIoUringBufferPool loopyIoUringBufferPool;

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

    /* Buffer pool tracking (PROVIDE_BUFFERS / Linux 5.7+) */
    void **bufferPoolGroups;    /* Array of buffer pools indexed by group ID */
    uint32_t *bufferPoolCounts; /* Number of buffers per group */
    uint32_t *bufferPoolSizes;  /* Size of each buffer per group */
    uint32_t maxBufferGroups;   /* Size of tracking arrays (typically 256) */
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

    /* Initialize buffer pool tracking (support up to 256 groups) */
    u->maxBufferGroups = 256;
    u->bufferPoolGroups = zcalloc(u->maxBufferGroups, sizeof(void *));
    u->bufferPoolCounts = zcalloc(u->maxBufferGroups, sizeof(uint32_t));
    u->bufferPoolSizes = zcalloc(u->maxBufferGroups, sizeof(uint32_t));
    if (!u->bufferPoolGroups || !u->bufferPoolCounts || !u->bufferPoolSizes) {
        zfree(u->bufferPoolGroups);
        zfree(u->bufferPoolCounts);
        zfree(u->bufferPoolSizes);
        zfree(u->fileState.ops);
        zfree(state->pollMasks);
        munmap(u->sqes, u->sqesSize);
        if (!singleMmap) {
            munmap(u->cqRing, u->cqRingSize);
        }
        munmap(u->sqRing, u->sqRingSize);
        close(ringFd);
        return false;
    }

    state->backend = BACKEND_IOURING;
    return true;
}

static void ioUringFree(loopyInternalState *state) {
    if (!state) {
        return;
    }

    IoUringState *u = &state->uring;

    /* Free buffer pools */
    if (u->bufferPoolGroups) {
        for (uint32_t i = 0; i < u->maxBufferGroups; i++) {
            if (u->bufferPoolGroups[i]) {
                zfree(u->bufferPoolGroups[i]);
            }
        }
        zfree(u->bufferPoolGroups);
        u->bufferPoolGroups = NULL;
    }
    if (u->bufferPoolCounts) {
        zfree(u->bufferPoolCounts);
        u->bufferPoolCounts = NULL;
    }
    if (u->bufferPoolSizes) {
        zfree(u->bufferPoolSizes);
        u->bufferPoolSizes = NULL;
    }

    /* Free file operation tracking */
    if (u->fileState.ops) {
        zfree(u->fileState.ops);
        u->fileState.ops = NULL; /* Clear pointer to prevent double-free */
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

/* Forward declaration for buffer-aware operation allocation */
static uint64_t ioUringBufferOpAllocate(IoUringState *u,
                                        loopyIoUringBufferCallback *bufferCb,
                                        void *userData, uint16_t groupId);

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
            u->fileState.ops[i].bufferCb = NULL; /* Clear any stale buffer cb */
            u->fileState.ops[i].userData = userData;
            u->fileState.ops[i].active = true;
            u->fileState.ops[i].multishot = false;
            u->fileState.ops[i].bufferGroup = 0;
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
    /* Strip bit 63 if present - callers may pass id with or without the marker
     * bit */
    const uint64_t idToFind = id & ~(1ULL << 63);

    for (size_t i = 0; i < u->fileState.opsSize; i++) {
        if (u->fileState.ops[i].active && u->fileState.ops[i].id == idToFind) {
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

    if (op->bufferCb) {
        /* Buffer-aware callback (final completion without buffer data) */
        op->bufferCb(op->userData, result, 0, NULL, 0);
    } else if (op->cb) {
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
 * Submit a CLOSE_DIRECT operation to io_uring (close file in fixed file table).
 */
static uint64_t ioUringSubmitCloseDirect(IoUringState *u, uint32_t fileIndex,
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
    sqe->fd = 0; /* Not used for direct close */
    sqe->file_index =
        fileIndex + 1; /* 0 = no fixed file, so encode as index + 1 */
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

/**
 * Submit a RECVMSG_MULTISHOT operation to io_uring.
 *
 * Similar to RECVMSG but fires callback multiple times for successive messages
 * until error or cancellation. Requires buffer pool for automatic buffer
 * selection.
 *
 * @param u            io_uring state
 * @param sockfd       Socket file descriptor
 * @param msg          Message header (scatter/gather, ancillary data)
 * @param flags        Receive flags
 * @param buffer_group Buffer group ID for buffer selection
 * @param cb           Completion callback (invoked multiple times)
 * @param userData     User data for callback
 * @return Operation ID on success, 0 on failure
 */
static uint64_t ioUringSubmitRecvmsgMultishot(IoUringState *u, int sockfd,
                                              struct msghdr *msg, int flags,
                                              uint16_t buffer_group,
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
    sqe->len = 1; /* Number of messages */
    sqe->msg_flags = (uint32_t)flags;
    sqe->buf_group = buffer_group;    /* Buffer pool for multishot */
    sqe->flags = IOSQE_BUFFER_SELECT; /* Enable buffer selection */
    sqe->user_data = id;

    /* Mark as multishot operation */
    loopyIoUringFileOp *op = ioUringFileOpFind(u, id);
    if (op) {
        op->multishot = true;
    }

    ioUringSubmitSqe(u);

    if (io_uring_enter(u->ringFd, 1, 0, 0, NULL) < 0) {
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
    /* Use fd directly for poll operations (bit 63 = 0 for poll, 1 for file ops)
     */
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
    sqe->fd = -1;             /* Not used for POLL_REMOVE */
    sqe->addr = (uint64_t)fd; /* Match the user_data of the poll to remove */
    sqe->user_data = 0;       /* Not meaningful for POLL_REMOVE */

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
    l->state = NULL; /* Clear state pointer to prevent double-free */
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

    /* Get completions from kernel - submit any pending + retrieve completions
     */
    uint32_t pending =
        *u->sqTail - __atomic_load_n(u->sqHead, __ATOMIC_ACQUIRE);
    int ret =
        io_uring_enter(u->ringFd, pending, 0, IORING_ENTER_GETEVENTS, NULL);
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

        /* Check bit 63 to distinguish file operations (bit 63=1) from poll (bit
         * 63=0) */
        bool isFileOp = (user_data & (1ULL << 63)) != 0;

        if (isFileOp) {
            /* File operation completion - mask off bit 63 to get actual ID */
            uint64_t fileOpId = user_data & ~(1ULL << 63);
            loopyIoUringFileOp *fileOp = ioUringFileOpFind(u, fileOpId);
            if (fileOp) {
                /* File operation completion */
                bool hasMore = (cqe->flags & IORING_CQE_F_MORE) != 0;
                bool hasBuffer = (cqe->flags & IORING_CQE_F_BUFFER) != 0;

                /* Check if this is a buffer-selected operation */
                if (hasBuffer) {
                    /* Extract buffer ID from CQE flags (bits 16-31) */
                    uint16_t bufferId = (uint16_t)(cqe->flags >> 16);

                    /* If we have a buffer-aware callback, use it with buffer
                     * info */
                    if (fileOp->bufferCb) {
                        /* Look up buffer pool info */
                        void *bufferAddr = NULL;
                        size_t bufferSize = 0;
                        uint16_t groupId = fileOp->bufferGroup;
                        if (groupId < u->maxBufferGroups &&
                            u->bufferPoolGroups[groupId]) {
                            /* bufferPoolGroups stores loopyIoUringBufferPool*
                             */
                            loopyIoUringBufferPool *pool =
                                (loopyIoUringBufferPool *)
                                    u->bufferPoolGroups[groupId];
                            bufferSize = pool->bufferSize;
                            bufferAddr = (char *)pool->buffers +
                                         ((size_t)bufferId * bufferSize);
                        }
                        fileOp->bufferCb(fileOp->userData, cqe->res, bufferId,
                                         bufferAddr, bufferSize);
                    } else if (fileOp->cb) {
                        /* Fallback to standard callback */
                        fileOp->cb(fileOp->userData, cqe->res);
                    }

                    /* Don't free the operation if multishot */
                    if (!fileOp->multishot || !hasMore) {
                        fileOp->active = false;
                    }
                } else if (fileOp->multishot && hasMore) {
                    /* Multishot operation with more completions coming.
                     * Invoke callback but DON'T free the operation - it will
                     * complete again for the next connection/event. */
                    if (fileOp->bufferCb) {
                        /* Buffer-aware callback without buffer (error/EOF) */
                        fileOp->bufferCb(fileOp->userData, cqe->res, 0, NULL,
                                         0);
                    } else if (fileOp->cb) {
                        fileOp->cb(fileOp->userData, cqe->res);
                    }
                    /* Operation stays active and will complete again */
                } else {
                    /* Final completion (no CQE_F_MORE) or non-multishot
                     * operation. This is the standard path and will free the
                     * operation. */
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
 * Close a file in the fixed file table via io_uring.
 *
 * @param l         Event loop
 * @param fileIndex Index of file in fixed file table (0-based)
 * @param cb        Completion callback
 * @param userData  User data
 * @return Operation ID on success, 0 on failure
 */
uint64_t loopyIoUringCloseDirect(loopyLoop *l, uint32_t fileIndex,
                                 loopyIoUringFileCallback *cb, void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitCloseDirect(&state->uring, fileIndex, cb, userData);
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
 * File System Metadata & Directory Operations
 * ==================================================================== */

static uint64_t ioUringSubmitStatx(IoUringState *u, int dirfd,
                                   const char *pathname, int flags,
                                   uint32_t mask, struct statx *statxbuf,
                                   loopyIoUringFileCallback *cb,
                                   void *userData) {
    if (!pathname || !statxbuf) {
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

    sqe->opcode = IORING_OP_STATX;
    sqe->fd = dirfd;
    sqe->addr = (uint64_t)(uintptr_t)pathname;
    sqe->len = mask;
    sqe->off = (uint64_t)(uintptr_t)statxbuf;
    sqe->statx_flags = (uint32_t)flags;
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

uint64_t loopyIoUringStatx(loopyLoop *l, int dirfd, const char *pathname,
                           int flags, uint32_t mask, struct statx *statxbuf,
                           loopyIoUringFileCallback *cb, void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitStatx(&state->uring, dirfd, pathname, flags, mask,
                              statxbuf, cb, userData);
}

static uint64_t ioUringSubmitRenameat(IoUringState *u, int olddirfd,
                                      const char *oldpath, int newdirfd,
                                      const char *newpath,
                                      loopyIoUringFileCallback *cb,
                                      void *userData) {
    if (!oldpath || !newpath) {
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

    sqe->opcode = IORING_OP_RENAMEAT;
    sqe->fd = olddirfd;
    sqe->addr = (uint64_t)(uintptr_t)oldpath;
    sqe->len = (uint32_t)newdirfd;
    sqe->addr2 = (uint64_t)(uintptr_t)newpath;
    sqe->rename_flags = 0;
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

uint64_t loopyIoUringRenameat(loopyLoop *l, int olddirfd, const char *oldpath,
                              int newdirfd, const char *newpath,
                              loopyIoUringFileCallback *cb, void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitRenameat(&state->uring, olddirfd, oldpath, newdirfd,
                                 newpath, cb, userData);
}

static uint64_t ioUringSubmitUnlinkat(IoUringState *u, int dirfd,
                                      const char *pathname, int flags,
                                      loopyIoUringFileCallback *cb,
                                      void *userData) {
    if (!pathname) {
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

    sqe->opcode = IORING_OP_UNLINKAT;
    sqe->fd = dirfd;
    sqe->addr = (uint64_t)(uintptr_t)pathname;
    sqe->unlink_flags = (uint32_t)flags;
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

uint64_t loopyIoUringUnlinkat(loopyLoop *l, int dirfd, const char *pathname,
                              int flags, loopyIoUringFileCallback *cb,
                              void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitUnlinkat(&state->uring, dirfd, pathname, flags, cb,
                                 userData);
}

static uint64_t ioUringSubmitMkdirat(IoUringState *u, int dirfd,
                                     const char *pathname, mode_t mode,
                                     loopyIoUringFileCallback *cb,
                                     void *userData) {
    if (!pathname) {
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

    sqe->opcode = IORING_OP_MKDIRAT;
    sqe->fd = dirfd;
    sqe->addr = (uint64_t)(uintptr_t)pathname;
    sqe->len = (uint32_t)mode;
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

uint64_t loopyIoUringMkdirat(loopyLoop *l, int dirfd, const char *pathname,
                             mode_t mode, loopyIoUringFileCallback *cb,
                             void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitMkdirat(&state->uring, dirfd, pathname, mode, cb,
                                userData);
}

static uint64_t ioUringSubmitSymlinkat(IoUringState *u, const char *target,
                                       int newdirfd, const char *linkpath,
                                       loopyIoUringFileCallback *cb,
                                       void *userData) {
    if (!target || !linkpath) {
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

    sqe->opcode = IORING_OP_SYMLINKAT;
    sqe->fd = newdirfd;
    sqe->addr = (uint64_t)(uintptr_t)target;
    sqe->len = 0;
    sqe->off = (uint64_t)(uintptr_t)linkpath;
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

uint64_t loopyIoUringSymlinkat(loopyLoop *l, const char *target, int newdirfd,
                               const char *linkpath,
                               loopyIoUringFileCallback *cb, void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitSymlinkat(&state->uring, target, newdirfd, linkpath, cb,
                                  userData);
}

static uint64_t ioUringSubmitLinkat(IoUringState *u, int olddirfd,
                                    const char *oldpath, int newdirfd,
                                    const char *newpath, int flags,
                                    loopyIoUringFileCallback *cb,
                                    void *userData) {
    if (!oldpath || !newpath) {
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

    sqe->opcode = IORING_OP_LINKAT;
    sqe->fd = olddirfd;
    sqe->addr = (uint64_t)(uintptr_t)oldpath;
    sqe->len = (uint32_t)newdirfd;
    sqe->addr2 = (uint64_t)(uintptr_t)newpath;
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

uint64_t loopyIoUringLinkat(loopyLoop *l, int olddirfd, const char *oldpath,
                            int newdirfd, const char *newpath, int flags,
                            loopyIoUringFileCallback *cb, void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitLinkat(&state->uring, olddirfd, oldpath, newdirfd,
                               newpath, flags, cb, userData);
}

static uint64_t ioUringSubmitFallocate(IoUringState *u, int fd, int mode,
                                       off_t offset, off_t len,
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

    sqe->opcode = IORING_OP_FALLOCATE;
    sqe->fd = fd;
    sqe->off = (uint64_t)offset;
    sqe->addr = (uint64_t)len;
    sqe->len = (uint32_t)mode;
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

uint64_t loopyIoUringFallocate(loopyLoop *l, int fd, int mode, off_t offset,
                               off_t len, loopyIoUringFileCallback *cb,
                               void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitFallocate(&state->uring, fd, mode, offset, len, cb,
                                  userData);
}

static uint64_t ioUringSubmitFadvise(IoUringState *u, int fd, off_t offset,
                                     off_t len, int advice,
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

    sqe->opcode = IORING_OP_FADVISE;
    sqe->fd = fd;
    sqe->off = (uint64_t)offset;
    sqe->len = (uint32_t)len;
    sqe->fadvise_advice = (uint32_t)advice;
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

uint64_t loopyIoUringFadvise(loopyLoop *l, int fd, off_t offset, off_t len,
                             int advice, loopyIoUringFileCallback *cb,
                             void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitFadvise(&state->uring, fd, offset, len, advice, cb,
                                userData);
}

static uint64_t ioUringSubmitSyncFileRange(IoUringState *u, int fd,
                                           off_t offset, off_t len,
                                           uint32_t flags,
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

    sqe->opcode = IORING_OP_SYNC_FILE_RANGE;
    sqe->fd = fd;
    sqe->off = (uint64_t)offset;
    sqe->len = (uint32_t)len;
    sqe->sync_range_flags = flags;
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

uint64_t loopyIoUringSyncFileRange(loopyLoop *l, int fd, off_t offset,
                                   off_t len, uint32_t flags,
                                   loopyIoUringFileCallback *cb,
                                   void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitSyncFileRange(&state->uring, fd, offset, len, flags, cb,
                                      userData);
}

static uint64_t ioUringSubmitFtruncate(IoUringState *u, int fd, off_t length,
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

    sqe->opcode = IORING_OP_FTRUNCATE;
    sqe->fd = fd;
    sqe->addr = 0;
    sqe->len = 0;
    sqe->off = (uint64_t)length;
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

uint64_t loopyIoUringFtruncate(loopyLoop *l, int fd, off_t length,
                               loopyIoUringFileCallback *cb, void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitFtruncate(&state->uring, fd, length, cb, userData);
}

/* Phase 5: Data Movement Operations */

static uint64_t ioUringSubmitSplice(IoUringState *u, int fd_in, off_t off_in,
                                    int fd_out, off_t off_out, size_t len,
                                    uint32_t flags,
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

    sqe->opcode = IORING_OP_SPLICE;
    sqe->fd = fd_out;
    sqe->off = (uint64_t)off_out;
    sqe->addr = 0;
    sqe->len = (uint32_t)len;
    sqe->splice_off_in = (uint64_t)off_in;
    sqe->splice_fd_in = fd_in;
    sqe->splice_flags = flags;
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

uint64_t loopyIoUringSplice(loopyLoop *l, int fd_in, off_t off_in, int fd_out,
                            off_t off_out, size_t len, uint32_t flags,
                            loopyIoUringFileCallback *cb, void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitSplice(&state->uring, fd_in, off_in, fd_out, off_out,
                               len, flags, cb, userData);
}

static uint64_t ioUringSubmitTee(IoUringState *u, int fd_in, int fd_out,
                                 size_t len, uint32_t flags,
                                 loopyIoUringFileCallback *cb, void *userData) {
    struct io_uring_sqe *sqe = ioUringGetSqe(u);
    if (!sqe) {
        return 0;
    }

    uint64_t id = ioUringFileOpAllocate(u, cb, userData);
    if (!id) {
        return 0;
    }

    sqe->opcode = IORING_OP_TEE;
    sqe->fd = fd_out;
    sqe->len = (uint32_t)len;
    sqe->splice_fd_in = fd_in;
    sqe->splice_flags = flags;
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

uint64_t loopyIoUringTee(loopyLoop *l, int fd_in, int fd_out, size_t len,
                         uint32_t flags, loopyIoUringFileCallback *cb,
                         void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitTee(&state->uring, fd_in, fd_out, len, flags, cb,
                            userData);
}

static uint64_t ioUringSubmitReadv(IoUringState *u, int fd,
                                   const struct iovec *iov, int iovcnt,
                                   off_t offset, loopyIoUringFileCallback *cb,
                                   void *userData) {
    if (!iov || iovcnt <= 0) {
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

    sqe->opcode = IORING_OP_READV;
    sqe->fd = fd;
    sqe->addr = (uint64_t)(uintptr_t)iov;
    sqe->len = (uint32_t)iovcnt;
    sqe->off = (uint64_t)offset;
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

uint64_t loopyIoUringReadv(loopyLoop *l, int fd, const struct iovec *iov,
                           int iovcnt, off_t offset,
                           loopyIoUringFileCallback *cb, void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitReadv(&state->uring, fd, iov, iovcnt, offset, cb,
                              userData);
}

static uint64_t ioUringSubmitWritev(IoUringState *u, int fd,
                                    const struct iovec *iov, int iovcnt,
                                    off_t offset, loopyIoUringFileCallback *cb,
                                    void *userData) {
    if (!iov || iovcnt <= 0) {
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

    sqe->opcode = IORING_OP_WRITEV;
    sqe->fd = fd;
    sqe->addr = (uint64_t)(uintptr_t)iov;
    sqe->len = (uint32_t)iovcnt;
    sqe->off = (uint64_t)offset;
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

uint64_t loopyIoUringWritev(loopyLoop *l, int fd, const struct iovec *iov,
                            int iovcnt, off_t offset,
                            loopyIoUringFileCallback *cb, void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitWritev(&state->uring, fd, iov, iovcnt, offset, cb,
                               userData);
}

/* Vector operations with fixed (registered) buffers */

static uint64_t ioUringSubmitReadvFixed(IoUringState *u, int fd,
                                        const struct iovec *iov, int iovcnt,
                                        off_t offset, uint32_t *buf_indices,
                                        loopyIoUringFileCallback *cb,
                                        void *userData) {
    if (!iov || iovcnt <= 0 || !buf_indices) {
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

    sqe->opcode = IORING_OP_READV_FIXED;
    sqe->fd = fd;
    sqe->addr = (uint64_t)(uintptr_t)iov;
    sqe->len = (uint32_t)iovcnt;
    sqe->off = (uint64_t)offset;
    sqe->buf_index = buf_indices[0]; /* First buffer index */
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

uint64_t loopyIoUringReadvFixed(loopyLoop *l, int fd, const struct iovec *iov,
                                int iovcnt, off_t offset, uint32_t *buf_indices,
                                loopyIoUringFileCallback *cb, void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitReadvFixed(&state->uring, fd, iov, iovcnt, offset,
                                   buf_indices, cb, userData);
}

static uint64_t ioUringSubmitWritevFixed(IoUringState *u, int fd,
                                         const struct iovec *iov, int iovcnt,
                                         off_t offset, uint32_t *buf_indices,
                                         loopyIoUringFileCallback *cb,
                                         void *userData) {
    if (!iov || iovcnt <= 0 || !buf_indices) {
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

    sqe->opcode = IORING_OP_WRITEV_FIXED;
    sqe->fd = fd;
    sqe->addr = (uint64_t)(uintptr_t)iov;
    sqe->len = (uint32_t)iovcnt;
    sqe->off = (uint64_t)offset;
    sqe->buf_index = buf_indices[0]; /* First buffer index */
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

uint64_t loopyIoUringWritevFixed(loopyLoop *l, int fd, const struct iovec *iov,
                                 int iovcnt, off_t offset,
                                 uint32_t *buf_indices,
                                 loopyIoUringFileCallback *cb, void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitWritevFixed(&state->uring, fd, iov, iovcnt, offset,
                                    buf_indices, cb, userData);
}

/* XATTR operations */

static uint64_t ioUringSubmitSetxattr(IoUringState *u, const char *path,
                                      const char *name, const void *value,
                                      size_t size, int flags,
                                      loopyIoUringFileCallback *cb,
                                      void *userData) {
    if (!path || !name) {
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

    sqe->opcode = IORING_OP_SETXATTR;
    sqe->addr = (uint64_t)(uintptr_t)name;
    sqe->addr2 = (uint64_t)(uintptr_t)value;
    sqe->len = (uint32_t)size;
    sqe->splice_off_in = (uint64_t)(uintptr_t)path;
    sqe->xattr_flags = (uint32_t)flags;
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

uint64_t loopyIoUringSetxattr(loopyLoop *l, const char *path, const char *name,
                              const void *value, size_t size, int flags,
                              loopyIoUringFileCallback *cb, void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitSetxattr(&state->uring, path, name, value, size, flags,
                                 cb, userData);
}

static uint64_t ioUringSubmitFsetxattr(IoUringState *u, int fd,
                                       const char *name, const void *value,
                                       size_t size, int flags,
                                       loopyIoUringFileCallback *cb,
                                       void *userData) {
    if (!name) {
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

    sqe->opcode = IORING_OP_FSETXATTR;
    sqe->fd = fd;
    sqe->addr = (uint64_t)(uintptr_t)name;
    sqe->addr2 = (uint64_t)(uintptr_t)value;
    sqe->len = (uint32_t)size;
    sqe->xattr_flags = (uint32_t)flags;
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

uint64_t loopyIoUringFsetxattr(loopyLoop *l, int fd, const char *name,
                               const void *value, size_t size, int flags,
                               loopyIoUringFileCallback *cb, void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitFsetxattr(&state->uring, fd, name, value, size, flags,
                                  cb, userData);
}

static uint64_t ioUringSubmitGetxattr(IoUringState *u, const char *path,
                                      const char *name, void *value,
                                      size_t size, loopyIoUringFileCallback *cb,
                                      void *userData) {
    if (!path || !name) {
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

    sqe->opcode = IORING_OP_GETXATTR;
    sqe->addr = (uint64_t)(uintptr_t)name;
    sqe->addr2 = (uint64_t)(uintptr_t)value;
    sqe->len = (uint32_t)size;
    sqe->splice_off_in = (uint64_t)(uintptr_t)path;
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

uint64_t loopyIoUringGetxattr(loopyLoop *l, const char *path, const char *name,
                              void *value, size_t size,
                              loopyIoUringFileCallback *cb, void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitGetxattr(&state->uring, path, name, value, size, cb,
                                 userData);
}

static uint64_t ioUringSubmitFgetxattr(IoUringState *u, int fd,
                                       const char *name, void *value,
                                       size_t size,
                                       loopyIoUringFileCallback *cb,
                                       void *userData) {
    if (!name) {
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

    sqe->opcode = IORING_OP_FGETXATTR;
    sqe->fd = fd;
    sqe->addr = (uint64_t)(uintptr_t)name;
    sqe->addr2 = (uint64_t)(uintptr_t)value;
    sqe->len = (uint32_t)size;
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

uint64_t loopyIoUringFgetxattr(loopyLoop *l, int fd, const char *name,
                               void *value, size_t size,
                               loopyIoUringFileCallback *cb, void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitFgetxattr(&state->uring, fd, name, value, size, cb,
                                  userData);
}

static uint64_t ioUringSubmitMadvise(IoUringState *u, void *addr, size_t length,
                                     int advice, loopyIoUringFileCallback *cb,
                                     void *userData) {
    struct io_uring_sqe *sqe = ioUringGetSqe(u);
    if (!sqe) {
        return 0;
    }

    uint64_t id = ioUringFileOpAllocate(u, cb, userData);
    if (!id) {
        return 0;
    }

    sqe->opcode = IORING_OP_MADVISE;
    sqe->addr = (uint64_t)(uintptr_t)addr;
    sqe->len = (uint32_t)length;
    sqe->fadvise_advice = (uint32_t)advice;
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

uint64_t loopyIoUringMadvise(loopyLoop *l, void *addr, size_t length,
                             int advice, loopyIoUringFileCallback *cb,
                             void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitMadvise(&state->uring, addr, length, advice, cb,
                                userData);
}

/* Phase 4: Timeout Operations */

struct io_uring_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

static uint64_t ioUringSubmitTimeout(IoUringState *u,
                                     struct io_uring_timespec *ts,
                                     uint32_t count, uint32_t flags,
                                     loopyIoUringFileCallback *cb,
                                     void *userData) {
    if (!ts) {
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

    sqe->opcode = IORING_OP_TIMEOUT;
    sqe->addr = (uint64_t)(uintptr_t)ts;
    sqe->len = 1;
    sqe->off = count;
    sqe->timeout_flags = flags;
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

uint64_t loopyIoUringTimeout(loopyLoop *l, uint64_t nanoseconds, bool absolute,
                             loopyIoUringFileCallback *cb, void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }

    static struct io_uring_timespec
        ts; /* Static to keep alive during async op */
    ts.tv_sec = (int64_t)(nanoseconds / 1000000000ULL);
    ts.tv_nsec = (int64_t)(nanoseconds % 1000000000ULL);
    uint32_t flags = absolute ? 1 : 0;

    return ioUringSubmitTimeout(&state->uring, &ts, 0, flags, cb, userData);
}

static uint64_t ioUringSubmitTimeoutRemove(IoUringState *u, uint64_t timeoutId,
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

    sqe->opcode = IORING_OP_TIMEOUT_REMOVE;
    sqe->addr = timeoutId;
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

typedef struct {
    bool done;
    int32_t result;
} TimeoutRemoveCtx;

static void timeoutRemoveCallback(void *userData, int32_t result) {
    TimeoutRemoveCtx *c = userData;
    c->result = result;
    c->done = true;
}

bool loopyIoUringTimeoutRemove(loopyLoop *l, uint64_t timeoutOpId) {
    if (!l || !l->state) {
        return false;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return false;
    }

    TimeoutRemoveCtx ctx = {false, 0};
    ioUringSubmitTimeoutRemove(&state->uring, timeoutOpId,
                               timeoutRemoveCallback, &ctx);

    int iterations = 0;
    while (!ctx.done && iterations < 100) {
        struct timeval tv = {0, 1000};
        loopyInternalPoll(l, &tv);
        iterations++;
    }

    return ctx.done && ctx.result == 0;
}

static uint64_t ioUringSubmitTimeoutUpdate(IoUringState *u, uint64_t timeoutId,
                                           struct io_uring_timespec *ts,
                                           uint32_t flags,
                                           loopyIoUringFileCallback *cb,
                                           void *userData) {
    if (!ts) {
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

    sqe->opcode = IORING_OP_TIMEOUT_REMOVE;
    sqe->addr = timeoutId;
    sqe->addr2 = (uint64_t)(uintptr_t)ts;
    sqe->timeout_flags = flags | IORING_TIMEOUT_UPDATE;
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

typedef struct {
    bool done;
    int32_t result;
} TimeoutUpdateCtx;

static void timeoutUpdateCallback(void *userData, int32_t result) {
    TimeoutUpdateCtx *c = userData;
    c->result = result;
    c->done = true;
}

uint64_t loopyIoUringTimeoutUpdate(loopyLoop *l, uint64_t timeoutOpId,
                                   uint64_t nanoseconds, bool absolute) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }

    static struct io_uring_timespec
        ts; /* Static to keep alive during async op */
    ts.tv_sec = (int64_t)(nanoseconds / 1000000000ULL);
    ts.tv_nsec = (int64_t)(nanoseconds % 1000000000ULL);
    uint32_t flags = absolute ? IORING_TIMEOUT_ABS : 0;

    TimeoutUpdateCtx ctx = {false, 0};
    ioUringSubmitTimeoutUpdate(&state->uring, timeoutOpId, &ts, flags,
                               timeoutUpdateCallback, &ctx);

    /* Wait for completion with timeout */
    int iterations = 0;
    while (!ctx.done && iterations < 100) {
        struct timeval tv = {0, 1000};
        loopyInternalPoll(l, &tv);
        iterations++;
    }

    return (ctx.done && ctx.result == 0) ? timeoutOpId : 0;
}

/* Phase 3: Network Zero-Copy Operations */

#define IORING_OP_SEND_ZC 47
#define IORING_OP_SENDMSG_ZC 48
#define IORING_OP_SOCKET 45
#define IORING_OP_BIND 56
#define IORING_OP_LISTEN 57

static uint64_t ioUringSubmitSocket(IoUringState *u, int domain, int type,
                                    int protocol, loopyIoUringFileCallback *cb,
                                    void *userData) {
    struct io_uring_sqe *sqe = ioUringGetSqe(u);
    if (!sqe) {
        return 0;
    }

    uint64_t id = ioUringFileOpAllocate(u, cb, userData);
    if (!id) {
        return 0;
    }

    sqe->opcode = IORING_OP_SOCKET;
    sqe->fd = domain;
    sqe->off = type;
    sqe->len = protocol;
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

uint64_t loopyIoUringSocket(loopyLoop *l, int domain, int type, int protocol,
                            loopyIoUringFileCallback *cb, void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitSocket(&state->uring, domain, type, protocol, cb,
                               userData);
}

static uint64_t ioUringSubmitBind(IoUringState *u, int sockfd,
                                  const struct sockaddr *addr,
                                  socklen_t addrlen,
                                  loopyIoUringFileCallback *cb,
                                  void *userData) {
    if (!addr) {
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

    sqe->opcode = IORING_OP_BIND;
    sqe->fd = sockfd;
    sqe->addr = (uint64_t)(uintptr_t)addr;
    sqe->off = addrlen;
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

uint64_t loopyIoUringBind(loopyLoop *l, int sockfd, const struct sockaddr *addr,
                          socklen_t addrlen, loopyIoUringFileCallback *cb,
                          void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitBind(&state->uring, sockfd, addr, addrlen, cb,
                             userData);
}

static uint64_t ioUringSubmitListen(IoUringState *u, int sockfd, int backlog,
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

    sqe->opcode = IORING_OP_LISTEN;
    sqe->fd = sockfd;
    sqe->off = backlog;
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

uint64_t loopyIoUringListen(loopyLoop *l, int sockfd, int backlog,
                            loopyIoUringFileCallback *cb, void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitListen(&state->uring, sockfd, backlog, cb, userData);
}

/* ====================================================================
 * Direct socket operations (ACCEPT_DIRECT, SOCKET_DIRECT) - Linux 5.19+
 * Use fixed file table for zero-syscall socket management
 * ==================================================================== */

/**
 * Submit an ACCEPT_DIRECT operation to io_uring.
 *
 * Accepts a new connection and installs it directly in the fixed file table
 * at the specified index, avoiding the need to return the fd via callback.
 *
 * Requires io_uring with fixed file table support (Linux 5.19+).
 *
 * @param u         io_uring state
 * @param sockfd    Listening socket file descriptor
 * @param addr      OUT: Client address (optional)
 * @param addrlen   IN/OUT: Address buffer size / actual size (optional)
 * @param flags     Accept flags (SOCK_NONBLOCK, SOCK_CLOEXEC, etc.)
 * @param file_index Index in fixed file table where fd will be installed
 * @param cb        Completion callback (receives 0 on success, -errno on error)
 * @param userData  User data for callback
 * @return Operation ID on success, 0 on failure
 */
static uint64_t
ioUringSubmitAcceptDirect(IoUringState *u, int sockfd, struct sockaddr *addr,
                          socklen_t *addrlen, int flags, uint32_t file_index,
                          loopyIoUringFileCallback *cb, void *userData) {
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
    sqe->flags = IOSQE_FIXED_FILE; /* Use fixed file table */
    sqe->fd = sockfd;
    sqe->addr = (uint64_t)(uintptr_t)addr;
    sqe->addr2 = (uint64_t)(uintptr_t)addrlen;
    sqe->accept_flags = (uint32_t)flags;
    sqe->file_index = file_index; /* Installation index in fixed file table */
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
 * Public wrapper for ACCEPT_DIRECT operation.
 *
 * Accepts a new connection and installs it in the fixed file table.
 *
 * @param l         Event loop
 * @param sockfd    Listening socket file descriptor
 * @param addr      OUT: Client address (optional, can be NULL)
 * @param addrlen   IN/OUT: Address buffer size / actual size (optional)
 * @param flags     Accept flags (SOCK_NONBLOCK, SOCK_CLOEXEC, etc.)
 * @param file_index Index in fixed file table where fd will be installed
 * @param cb        Completion callback
 * @param userData  User data for callback
 * @return Operation ID on success, 0 on failure or if io_uring not available
 */
uint64_t loopyIoUringAcceptDirect(loopyLoop *l, int sockfd,
                                  struct sockaddr *addr, socklen_t *addrlen,
                                  int flags, uint32_t file_index,
                                  loopyIoUringNetCallback *cb, void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitAcceptDirect(&state->uring, sockfd, addr, addrlen,
                                     flags, file_index,
                                     (loopyIoUringFileCallback *)cb, userData);
}

/**
 * Submit a SOCKET_DIRECT operation to io_uring.
 *
 * Creates a socket and installs it directly in the fixed file table
 * at the specified index, avoiding the need to return the fd via callback.
 *
 * Requires io_uring with fixed file table support (Linux 5.19+).
 *
 * @param u         io_uring state
 * @param domain    Address family (AF_INET, AF_INET6, AF_UNIX, etc.)
 * @param type      Socket type (SOCK_STREAM, SOCK_DGRAM, etc.)
 * @param protocol  Protocol (usually 0)
 * @param file_index Index in fixed file table where fd will be installed
 * @param cb        Completion callback (receives 0 on success, -errno on error)
 * @param userData  User data for callback
 * @return Operation ID on success, 0 on failure
 */
static uint64_t ioUringSubmitSocketDirect(IoUringState *u, int domain, int type,
                                          int protocol, uint32_t file_index,
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

    sqe->opcode = IORING_OP_SOCKET;
    sqe->flags = IOSQE_FIXED_FILE; /* Use fixed file table */
    sqe->fd = domain;
    sqe->off = type;
    sqe->len = protocol;
    sqe->file_index = file_index; /* Installation index in fixed file table */
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
 * Public wrapper for SOCKET_DIRECT operation.
 *
 * Creates a socket and installs it directly in the fixed file table.
 *
 * @param l         Event loop
 * @param domain    Address family (AF_INET, AF_INET6, AF_UNIX, etc.)
 * @param type      Socket type (SOCK_STREAM, SOCK_DGRAM, etc.)
 * @param protocol  Protocol (usually 0)
 * @param file_index Index in fixed file table where fd will be installed
 * @param cb        Completion callback
 * @param userData  User data for callback
 * @return Operation ID on success, 0 on failure or if io_uring not available
 */
uint64_t loopyIoUringSocketDirect(loopyLoop *l, int domain, int type,
                                  int protocol, uint32_t file_index,
                                  loopyIoUringNetCallback *cb, void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitSocketDirect(&state->uring, domain, type, protocol,
                                     file_index, (loopyIoUringFileCallback *)cb,
                                     userData);
}

/* Multishot network operations */
#define IORING_RECV_MULTISHOT (1U << 1)
#define IORING_OP_POLL_MULTISHOT 7

static uint64_t ioUringSubmitRecvMultishot(IoUringState *u, int sockfd,
                                           void *buf, size_t len, int flags,
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

    sqe->opcode = IORING_OP_RECV;
    sqe->fd = sockfd;
    sqe->addr = (uint64_t)(uintptr_t)buf;
    sqe->len = (uint32_t)len;
    sqe->msg_flags = flags;
    sqe->ioprio = IORING_RECV_MULTISHOT;
    sqe->user_data = id;

    loopyIoUringFileOp *op = ioUringFileOpFind(u, id);
    if (op) {
        op->multishot = true;
    }

    ioUringSubmitSqe(u);
    if (io_uring_enter(u->ringFd, 1, 0, 0, NULL) < 0) {
        if (op) {
            op->active = false;
        }
        return 0;
    }
    return id;
}

uint64_t loopyIoUringRecvMultishot(loopyLoop *l, int sockfd, void *buf,
                                   size_t len, int flags,
                                   loopyIoUringFileCallback *cb,
                                   void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitRecvMultishot(&state->uring, sockfd, buf, len, flags,
                                      cb, userData);
}

/* Multishot file read operation - requires buffer pool */
static uint64_t ioUringSubmitReadMultishot(IoUringState *u, int fd, size_t len,
                                           off_t offset, uint16_t buffer_group,
                                           loopyIoUringBufferCallback *cb,
                                           void *userData) {
    struct io_uring_sqe *sqe = ioUringGetSqe(u);
    if (!sqe) {
        return 0;
    }

    uint64_t id = ioUringBufferOpAllocate(u, cb, userData, buffer_group);
    if (!id) {
        return 0;
    }

    sqe->opcode = IORING_OP_READ_MULTISHOT;
    sqe->fd = fd;
    sqe->addr = 0; /* NULL - uses buffer pool, not user buffer */
    sqe->len = (uint32_t)len;
    sqe->off = (uint64_t)offset;
    sqe->buf_group = buffer_group;    /* Buffer pool for multishot */
    sqe->flags = IOSQE_BUFFER_SELECT; /* Enable buffer selection */
    sqe->user_data = id;

    loopyIoUringFileOp *op = ioUringFileOpFind(u, id);
    if (op) {
        op->multishot = true;
    }

    ioUringSubmitSqe(u);
    if (io_uring_enter(u->ringFd, 1, 0, 0, NULL) < 0) {
        if (op) {
            op->active = false;
        }
        return 0;
    }
    return id;
}

uint64_t loopyIoUringReadMultishot(loopyLoop *l, int fd, size_t len,
                                   off_t offset, uint16_t buffer_group,
                                   loopyIoUringBufferCallback *cb,
                                   void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitReadMultishot(&state->uring, fd, len, offset,
                                      buffer_group, cb, userData);
}

/* Direct file operations */
#define IORING_OP_OPENAT_DIRECT 18
#define IORING_FILE_INDEX_ALLOC (~0U)

static uint64_t ioUringSubmitOpenatDirect(IoUringState *u, int dirfd,
                                          const char *pathname, int flags,
                                          mode_t mode, uint32_t file_index,
                                          loopyIoUringFileCallback *cb,
                                          void *userData) {
    if (!pathname) {
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

    sqe->opcode = IORING_OP_OPENAT;
    sqe->fd = dirfd;
    sqe->addr = (uint64_t)(uintptr_t)pathname;
    sqe->len = mode;
    sqe->open_flags = (uint32_t)flags;
    /* file_index is 1-indexed for direct opens (0 means no direct),
     * except for IORING_FILE_INDEX_ALLOC which is passed as-is */
    if (file_index == IORING_FILE_INDEX_ALLOC) {
        sqe->file_index = file_index;
    } else {
        sqe->file_index = file_index + 1;
    }
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

uint64_t loopyIoUringOpenatDirect(loopyLoop *l, int dirfd, const char *pathname,
                                  int flags, mode_t mode, uint32_t file_index,
                                  loopyIoUringFileCallback *cb,
                                  void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitOpenatDirect(&state->uring, dirfd, pathname, flags,
                                     mode, file_index, cb, userData);
}

/* Phase 7: Process/IPC Operations */

#define IORING_OP_WAITID 50
#define IORING_OP_FUTEX_WAIT 51
#define IORING_OP_FUTEX_WAKE 52
#define IORING_OP_PIPE 62

static uint64_t ioUringSubmitPipe(IoUringState *u, int *pipefds, int flags,
                                  loopyIoUringFileCallback *cb,
                                  void *userData) {
    if (!pipefds) {
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

    sqe->opcode = IORING_OP_PIPE;
    sqe->fd = 0;
    sqe->off = 0;
    sqe->addr = (uint64_t)(uintptr_t)pipefds;
    sqe->len = 0;
    sqe->pipe_flags = (uint32_t)flags;
    sqe->flags = 0;
    sqe->ioprio = 0;
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

uint64_t loopyIoUringPipe(loopyLoop *l, int *pipefds, int flags,
                          loopyIoUringFileCallback *cb, void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitPipe(&state->uring, pipefds, flags, cb, userData);
}

static uint64_t ioUringSubmitMsgRing(IoUringState *u, int target_fd,
                                     uint32_t len, uint64_t data,
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

    sqe->opcode = IORING_OP_MSG_RING;
    sqe->fd = target_fd;
    sqe->len = len;
    sqe->off = data;
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

uint64_t loopyIoUringMsgRing(loopyLoop *l, int target_fd, uint32_t len,
                             uint64_t data, loopyIoUringFileCallback *cb,
                             void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitMsgRing(&state->uring, target_fd, len, data, cb,
                                userData);
}

static uint64_t ioUringSubmitMsgRingFd(IoUringState *u, int target_fd,
                                       int source_fd, int target_fixed,
                                       uint64_t data,
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

    sqe->opcode = IORING_OP_MSG_RING;
    sqe->fd = target_fd;
    sqe->addr = 1; /* IORING_MSG_SEND_FD */
    sqe->len = 0;
    sqe->off = data;
    sqe->addr3 = source_fd;
    sqe->file_index = target_fixed + 1; /* Encode as index + 1 */
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

uint64_t loopyIoUringMsgRingFd(loopyLoop *l, int target_fd, int source_fd,
                               int target_fixed, uint64_t data,
                               loopyIoUringFileCallback *cb, void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitMsgRingFd(&state->uring, target_fd, source_fd,
                                  target_fixed, data, cb, userData);
}

uint64_t loopyIoUringMsgRingFdAlloc(loopyLoop *l, int target_fd, int source_fd,
                                    uint64_t data, loopyIoUringFileCallback *cb,
                                    void *userData) {
    return loopyIoUringMsgRingFd(l, target_fd, source_fd, 0xFFFFFFFE, data, cb,
                                 userData);
}

static uint64_t ioUringSubmitMsgRingCqeFlags(IoUringState *u, int target_fd,
                                             uint32_t len, uint64_t data,
                                             uint32_t cqe_flags,
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

    sqe->opcode = IORING_OP_MSG_RING;
    sqe->fd = target_fd;
    sqe->len = len;
    sqe->off = data;
    sqe->msg_ring_flags = 1; /* IORING_MSG_RING_FLAGS_PASS */
    sqe->file_index = cqe_flags;
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

uint64_t loopyIoUringMsgRingCqeFlags(loopyLoop *l, int target_fd, uint32_t len,
                                     uint64_t data, uint32_t cqe_flags,
                                     loopyIoUringFileCallback *cb,
                                     void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitMsgRingCqeFlags(&state->uring, target_fd, len, data,
                                        cqe_flags, cb, userData);
}

/* Zero-copy network operations */
static uint64_t ioUringSubmitSendZC(IoUringState *u, int sockfd,
                                    const void *buf, size_t len, int flags,
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

    sqe->opcode = IORING_OP_SEND_ZC;
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

uint64_t loopyIoUringSendZeroCopy(loopyLoop *l, int sockfd, const void *buf,
                                  size_t len, int flags,
                                  loopyIoUringFileCallback *cb,
                                  void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitSendZC(&state->uring, sockfd, buf, len, flags, cb,
                               userData);
}

/**
 * Submit a RECV_ZC (zero-copy receive) operation to io_uring.
 *
 * Performs a zero-copy receive on a socket, avoiding the kernel→userspace copy
 * when possible. Requires Linux 6.0+ kernel support for io_uring RECV_ZC.
 *
 * @param u        io_uring state
 * @param sockfd   Socket file descriptor (must be connected)
 * @param buf      Buffer to receive into (must remain valid until completion)
 * @param len      Buffer size
 * @param flags    Receive flags (MSG_DONTWAIT, MSG_PEEK, etc.)
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure
 */
static uint64_t ioUringSubmitRecvZC(IoUringState *u, int sockfd, void *buf,
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

    sqe->opcode = IORING_OP_RECV_ZC;
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
 * Submit a SENDMSG_ZC (zero-copy sendmsg) operation to io_uring.
 *
 * Sends data with control messages (scatter/gather I/O, ancillary data)
 * without copying from user space to kernel space, reducing CPU overhead.
 * Requires Linux 6.1+ kernel support for io_uring SENDMSG_ZC.
 *
 * @param u        io_uring state
 * @param sockfd   Socket file descriptor
 * @param msg      Message header (must remain valid until notification CQE)
 * @param flags    Send flags (MSG_DONTWAIT, MSG_NOSIGNAL, etc.)
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure
 */
static uint64_t ioUringSubmitSendmsgZC(IoUringState *u, int sockfd,
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

    sqe->opcode = IORING_OP_SENDMSG_ZC;
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
 * Submit a SEND_BUNDLE operation to io_uring.
 *
 * Allows bundling multiple send operations in a single SQE for efficiency.
 * This reduces the overhead of issuing multiple individual send operations
 * when sending to the same socket.
 *
 * Requires Linux 6.1+ kernel support for io_uring SEND_BUNDLE.
 *
 * @param u        io_uring state
 * @param sockfd   Socket file descriptor
 * @param bufs     Array of buffers to send (const void **)
 * @param lens     Array of buffer lengths (size_t *)
 * @param count    Number of buffers in the bundle
 * @param flags    Send flags (MSG_DONTWAIT, MSG_NOSIGNAL, etc.)
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure
 */
static uint64_t ioUringSubmitSendBundle(IoUringState *u, int sockfd,
                                        const void *const *bufs,
                                        const size_t *lens, uint32_t count,
                                        int flags, loopyIoUringFileCallback *cb,
                                        void *userData) {
    struct io_uring_sqe *sqe = ioUringGetSqe(u);
    if (!sqe) {
        return 0;
    }

    /* Validate parameters before allocating operation resources */
    if (!bufs || !lens || count == 0 || count > 65535) {
        return 0;
    }

    uint64_t id = ioUringFileOpAllocate(u, cb, userData);
    if (!id) {
        return 0;
    }

    sqe->opcode = IORING_OP_SEND_BUNDLE;
    sqe->fd = sockfd;
    sqe->addr = (uint64_t)(uintptr_t)bufs;
    sqe->addr2 = (uint64_t)(uintptr_t)lens;
    sqe->len = count; /* Number of buffers in the bundle */
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

/* Additional multishot operations */
#define IORING_OP_POLL_MULTISHOT_ADD 7
#define IORING_OP_RECVMSG_MULTISHOT 27

static uint64_t ioUringSubmitPollMultishot(IoUringState *u, int fd,
                                           uint32_t poll_mask,
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

    sqe->opcode = IORING_OP_POLL_ADD;
    sqe->fd = fd;
    sqe->poll32_events = poll_mask;
    sqe->len = (1U << 0); /* IORING_POLL_ADD_MULTI */
    sqe->user_data = id;

    loopyIoUringFileOp *op = ioUringFileOpFind(u, id);
    if (op) {
        op->multishot = true;
    }

    ioUringSubmitSqe(u);
    if (io_uring_enter(u->ringFd, 1, 0, 0, NULL) < 0) {
        if (op) {
            op->active = false;
        }
        return 0;
    }
    return id;
}

uint64_t loopyIoUringPollMultishot(loopyLoop *l, int fd, uint32_t poll_mask,
                                   loopyIoUringFileCallback *cb,
                                   void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitPollMultishot(&state->uring, fd, poll_mask, cb,
                                      userData);
}

/* Poll update operation (IORING_OP_POLL_REMOVE with update flags) */
static uint64_t ioUringSubmitPollUpdate(IoUringState *u, uint64_t oldOpId,
                                        uint32_t poll_mask,
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

    /* POLL_UPDATE uses POLL_REMOVE opcode with update flags in len field */
    sqe->opcode = IORING_OP_POLL_REMOVE;
    sqe->fd = -1;                   /* Not used for POLL_REMOVE */
    sqe->addr = oldOpId;            /* Old operation ID to update */
    sqe->poll32_events = poll_mask; /* New poll mask */
    sqe->len =
        IORING_POLL_UPDATE_EVENTS; /* Flag indicating this is an update */
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

uint64_t loopyIoUringPollUpdate(loopyLoop *l, uint64_t oldOpId,
                                uint32_t poll_mask,
                                loopyIoUringFileCallback *cb, void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitPollUpdate(&state->uring, oldOpId, poll_mask, cb,
                                   userData);
}

/* Process operations */
static uint64_t ioUringSubmitWaitid(IoUringState *u, int which, int upid,
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

    sqe->opcode = IORING_OP_WAITID;
    sqe->fd = which;
    sqe->off = upid;
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

uint64_t loopyIoUringWaitid(loopyLoop *l, int which, int upid,
                            loopyIoUringFileCallback *cb, void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitWaitid(&state->uring, which, upid, cb, userData);
}

static uint64_t ioUringSubmitFutexWait(IoUringState *u, uint32_t *futex,
                                       uint64_t val, uint64_t mask,
                                       uint32_t futex_flags,
                                       loopyIoUringFileCallback *cb,
                                       void *userData) {
    if (!futex) {
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

    sqe->opcode = IORING_OP_FUTEX_WAIT;
    sqe->addr = (uint64_t)(uintptr_t)futex;
    sqe->len = val;
    sqe->addr2 = mask;
    sqe->futex_flags = futex_flags;
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

uint64_t loopyIoUringFutexWait(loopyLoop *l, uint32_t *futex, uint64_t val,
                               uint64_t mask, uint32_t futex_flags,
                               loopyIoUringFileCallback *cb, void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitFutexWait(&state->uring, futex, val, mask, futex_flags,
                                  cb, userData);
}

static uint64_t ioUringSubmitFutexWake(IoUringState *u, uint32_t *futex,
                                       uint64_t val, uint64_t mask,
                                       uint32_t futex_flags,
                                       loopyIoUringFileCallback *cb,
                                       void *userData) {
    if (!futex) {
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

    sqe->opcode = IORING_OP_FUTEX_WAKE;
    sqe->addr = (uint64_t)(uintptr_t)futex;
    sqe->len = val;
    sqe->addr2 = mask;
    sqe->futex_flags = futex_flags;
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

uint64_t loopyIoUringFutexWake(loopyLoop *l, uint32_t *futex, uint64_t val,
                               uint64_t mask, uint32_t futex_flags,
                               loopyIoUringFileCallback *cb, void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitFutexWake(&state->uring, futex, val, mask, futex_flags,
                                  cb, userData);
}

#define IORING_OP_EPOLL_CTL 29

static uint64_t ioUringSubmitEpollCtl(IoUringState *u, int epfd, int fd, int op,
                                      struct epoll_event *ev,
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

    sqe->opcode = IORING_OP_EPOLL_CTL;
    sqe->fd = epfd;
    sqe->addr = (uint64_t)(uintptr_t)ev;
    sqe->len = op;
    sqe->off = fd;
    sqe->user_data = id;

    ioUringSubmitSqe(u);
    if (io_uring_enter(u->ringFd, 1, 0, 0, NULL) < 0) {
        loopyIoUringFileOp *op_ptr = ioUringFileOpFind(u, id);
        if (op_ptr) {
            op_ptr->active = false;
        }
        return 0;
    }
    return id;
}

uint64_t loopyIoUringEpollCtl(loopyLoop *l, int epfd, int fd, int op,
                              struct epoll_event *ev,
                              loopyIoUringFileCallback *cb, void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitEpollCtl(&state->uring, epfd, fd, op, ev, cb, userData);
}

static uint64_t ioUringSubmitEpollWait(IoUringState *u, int epfd,
                                       struct epoll_event *events,
                                       int maxevents, int timeout_ms,
                                       loopyIoUringFileCallback *cb,
                                       void *userData) {
    if (!events || maxevents <= 0) {
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

    sqe->opcode = IORING_OP_EPOLL_WAIT;
    sqe->fd = epfd;
    sqe->addr = (uint64_t)(uintptr_t)events;
    sqe->len = maxevents;
    sqe->off = timeout_ms;
    sqe->user_data = id;

    ioUringSubmitSqe(u);
    if (io_uring_enter(u->ringFd, 1, 0, 0, NULL) < 0) {
        loopyIoUringFileOp *op_ptr = ioUringFileOpFind(u, id);
        if (op_ptr) {
            op_ptr->active = false;
        }
        return 0;
    }
    return id;
}

uint64_t loopyIoUringEpollWait(loopyLoop *l, int epfd,
                               struct epoll_event *events, int maxevents,
                               int timeout_ms, loopyIoUringFileCallback *cb,
                               void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitEpollWait(&state->uring, epfd, events, maxevents,
                                  timeout_ms, cb, userData);
}

static uint64_t ioUringSubmitFutexWaitv(IoUringState *u,
                                        const struct futex_waitv *futexes,
                                        uint32_t nfutexes, uint32_t flags,
                                        loopyIoUringFileCallback *cb,
                                        void *userData) {
    if (!futexes || nfutexes == 0) {
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

    sqe->opcode = IORING_OP_FUTEX_WAITV;
    sqe->addr = (uint64_t)(uintptr_t)futexes;
    sqe->len = nfutexes;
    sqe->futex_flags = flags;
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

uint64_t loopyIoUringFutexWaitv(loopyLoop *l, const struct futex_waitv *futexes,
                                uint32_t nfutexes, uint32_t flags,
                                loopyIoUringFileCallback *cb, void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitFutexWaitv(&state->uring, futexes, nfutexes, flags, cb,
                                   userData);
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

/* ====================================================================
 * Buffer Pool Management (PROVIDE_BUFFERS / Linux 5.7+)
 * ==================================================================== */

/**
 * Submit a PROVIDE_BUFFERS operation to add buffers to a kernel-managed pool.
 *
 * The kernel maintains pools of buffers grouped by ID. Operations can then
 * use IOSQE_BUFFER_SELECT to automatically select an available buffer from
 * the pool, eliminating manual buffer management.
 *
 * @param u        io_uring state
 * @param buffers  Array of buffers to provide
 * @param count    Number of buffers to provide
 * @param groupId  Buffer group ID (0-65535)
 * @param bufId    Starting buffer ID (increments for each buffer)
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure
 */
static uint64_t ioUringSubmitProvideBuffers(IoUringState *u, void *buffers,
                                            uint32_t bufSize, uint32_t count,
                                            uint16_t groupId, uint16_t bufId,
                                            loopyIoUringFileCallback *cb,
                                            void *userData) {
    if (!buffers || count == 0 || bufSize == 0) {
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

    sqe->opcode = IORING_OP_PROVIDE_BUFFERS;
    sqe->fd = (int32_t)count; /* Number of buffers */
    sqe->addr = (uint64_t)(uintptr_t)buffers;
    sqe->len = bufSize;       /* Size of each buffer */
    sqe->off = bufId;         /* Starting buffer ID */
    sqe->buf_group = groupId; /* Buffer group ID */
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
 * Submit a REMOVE_BUFFERS operation to remove buffers from a pool.
 *
 * Removes all buffers from the specified group. Buffers currently in use
 * by pending operations will complete normally.
 *
 * @param u        io_uring state
 * @param count    Number of buffers to remove
 * @param groupId  Buffer group ID
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure
 */
static uint64_t ioUringSubmitRemoveBuffers(IoUringState *u, uint32_t count,
                                           uint16_t groupId,
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

    sqe->opcode = IORING_OP_REMOVE_BUFFERS;
    sqe->fd = (int32_t)count; /* Number of buffers to remove */
    sqe->buf_group = groupId; /* Buffer group ID */
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

static uint64_t ioUringSubmitFixedFdInstall(IoUringState *u, int fd,
                                            uint32_t flags,
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

    sqe->opcode = IORING_OP_FIXED_FD_INSTALL;
    sqe->fd = fd;
    sqe->flags = IOSQE_FIXED_FILE;
    sqe->install_fd_flags = flags;
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
 * Install FD into fixed file table.
 *
 * @param l        Event loop
 * @param fd       File descriptor to install
 * @param flags    Installation flags
 * @param cb       Completion callback (result = index in table)
 * @param userData User data
 * @return Operation ID on success, 0 on failure
 */
uint64_t loopyIoUringFixedFdInstall(loopyLoop *l, int fd, uint32_t flags,
                                    loopyIoUringFileCallback *cb,
                                    void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitFixedFdInstall(&state->uring, fd, flags, cb, userData);
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

static uint64_t ioUringSubmitSendto(IoUringState *u, int sockfd,
                                    const void *buf, size_t len, int flags,
                                    const struct sockaddr *dest_addr,
                                    socklen_t addrlen,
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

    sqe->opcode = IORING_OP_SEND;
    sqe->fd = sockfd;
    sqe->addr = (uint64_t)(uintptr_t)buf;
    sqe->len = (uint32_t)len;
    sqe->msg_flags = (uint32_t)flags;
    /* UDP address fields */
    sqe->addr2 = (uint64_t)(uintptr_t)dest_addr;
    sqe->addr_len = addrlen;
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
 * Send to specific address (UDP/unconnected sockets).
 */
uint64_t loopyIoUringSendto(loopyLoop *l, int sockfd, const void *buf,
                            size_t len, int flags,
                            const struct sockaddr *dest_addr, socklen_t addrlen,
                            loopyIoUringNetCallback *cb, void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitSendto(&state->uring, sockfd, buf, len, flags,
                               dest_addr, addrlen,
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
 * Submit a zero-copy RECV operation (RECV_ZC) via io_uring (if available).
 *
 * Performs a zero-copy receive on a connected socket, avoiding the
 * kernel→userspace copy when possible. This provides better CPU efficiency
 * for high-throughput networking workloads.
 *
 * Requires Linux 6.0+ kernel support for io_uring RECV_ZC operations.
 *
 * The operation completes asynchronously and the callback is invoked when done.
 *
 * @param l        Event loop
 * @param sockfd   Socket file descriptor (must be connected)
 * @param buf      Buffer to receive into (must remain valid until completion)
 * @param len      Buffer size
 * @param flags    Receive flags (MSG_DONTWAIT, MSG_PEEK, etc.)
 * @param cb       Completion callback
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure or if io_uring not available
 *
 * Note: Zero-copy receives may have special notification CQE handling.
 *       The callback's result parameter contains bytes received or -errno on
 * error.
 */
uint64_t loopyIoUringRecvZeroCopy(loopyLoop *l, int sockfd, void *buf,
                                  size_t len, int flags,
                                  loopyIoUringNetCallback *cb, void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitRecvZC(&state->uring, sockfd, buf, len, flags,
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
 * Submit a SENDMSG_ZC (zero-copy sendmsg) operation via io_uring (if
 * available).
 *
 * Sends data with control messages (scatter/gather I/O, ancillary data) without
 * copying from user space to kernel space, reducing CPU overhead for large
 * transfers.
 *
 * Requires:
 *  - Linux kernel 6.1+ for io_uring SENDMSG_ZC support
 *  - io_uring backend active
 *
 * Benefits:
 *  - Avoids userspace-to-kernel copy overhead
 *  - Useful for large message transfers
 *  - Supports scatter/gather I/O and ancillary data like regular SENDMSG
 *
 * The operation completes asynchronously and the callback is invoked when done.
 * May generate additional notification CQEs similar to SEND_ZC.
 *
 * @param l        Event loop
 * @param sockfd   Socket file descriptor
 * @param msg      Message header (scatter/gather, ancillary data)
 *                 Must remain valid until completion callback is invoked
 * @param flags    Send flags (MSG_DONTWAIT, MSG_NOSIGNAL, etc.)
 * @param cb       Completion callback (receives bytes sent in result)
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure or if io_uring not available
 *
 * Example:
 *   struct iovec iov[3];
 *   iov[0].iov_base = buffer1; iov[0].iov_len = len1;
 *   iov[1].iov_base = buffer2; iov[1].iov_len = len2;
 *   iov[2].iov_base = buffer3; iov[2].iov_len = len3;
 *
 *   struct msghdr msg = {0};
 *   msg.msg_iov = iov;
 *   msg.msg_iovlen = 3;
 *
 *   uint64_t opId = loopyIoUringSendmsgZeroCopy(loop, sockfd, &msg,
 *                                               MSG_NOSIGNAL, onSend, NULL);
 */
uint64_t loopyIoUringSendmsgZeroCopy(loopyLoop *l, int sockfd,
                                     const struct msghdr *msg, int flags,
                                     loopyIoUringNetCallback *cb,
                                     void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitSendmsgZC(&state->uring, sockfd, msg, flags,
                                  (loopyIoUringFileCallback *)cb, userData);
}

/**
 * Submit a SEND_BUNDLE operation via io_uring (if available).
 *
 * Bundles multiple send operations into a single SQE for improved efficiency.
 * Instead of submitting multiple individual SEND operations, this allows
 * sending multiple buffers with a single syscall, reducing overhead.
 *
 * Requires:
 *  - Linux kernel 6.1+ for io_uring SEND_BUNDLE support
 *  - io_uring backend active
 *
 * Benefits:
 *  - Reduces syscall overhead when sending multiple buffers to same socket
 *  - Batches multiple sends into a single operation
 *  - Useful for headers, data chunks, trailers patterns
 *
 * The operation completes asynchronously and the callback is invoked when done.
 * The result contains the total number of bytes sent from all buffers.
 *
 * Note:
 *  - All buffers are sent to the same socket
 *  - The operation is atomic - either all buffers are sent or none
 *  - If partial send occurs, the kernel may retry or return partial count
 *  - Buffers must remain valid until completion callback is invoked
 *
 * @param l        Event loop
 * @param sockfd   Socket file descriptor (connected, or target address set via
 * msg)
 * @param bufs     Array of buffer pointers (const void *[])
 *                 Must remain valid until completion
 * @param lens     Array of buffer lengths (size_t[])
 *                 Must remain valid until completion
 * @param count    Number of buffers in the bundle (1-65535)
 * @param flags    Send flags (MSG_DONTWAIT, MSG_NOSIGNAL, etc.)
 * @param cb       Completion callback (receives total bytes sent in result)
 * @param userData User data for callback
 * @return Operation ID on success, 0 on failure or if io_uring not available
 *
 * Example (header + data pattern):
 *   char header[10] = "HEADER:";
 *   char data[100] = "...actual data...";
 *   char trailer[20] = "\r\n";
 *
 *   const void *bufs[] = { header, data, trailer };
 *   size_t lens[] = { 7, 94, 2 };
 *
 *   uint64_t opId = loopyIoUringSendBundle(loop, sockfd, bufs, lens, 3,
 *                                          MSG_NOSIGNAL, onSend, NULL);
 *
 *   void onSend(void *userData, int32_t result) {
 *       if (result < 0) {
 *           printf("Send failed: %s\\n", strerror(-result));
 *       } else {
 *           printf("Sent %d bytes total\\n", result);
 *       }
 *   }
 */
uint64_t loopyIoUringSendBundle(loopyLoop *l, int sockfd,
                                const void *const *bufs, const size_t *lens,
                                uint32_t count, int flags,
                                loopyIoUringNetCallback *cb, void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitSendBundle(&state->uring, sockfd, bufs, lens, count,
                                   flags, (loopyIoUringFileCallback *)cb,
                                   userData);
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
 * Submit a multishot RECVMSG operation via io_uring (if available).
 *
 * Receives multiple messages with callback firing for each message until
 * error or cancellation. Requires buffer pool for automatic buffer selection.
 */
uint64_t loopyIoUringRecvmsgMultishot(loopyLoop *l, int sockfd,
                                      struct msghdr *msg, int flags,
                                      uint16_t buffer_group,
                                      loopyIoUringNetCallback *cb,
                                      void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitRecvmsgMultishot(
        &state->uring, sockfd, msg, flags, buffer_group,
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

/**
 * Cancel all in-flight operations on a specific file descriptor.
 *
 * @param l  Event loop
 * @param fd File descriptor
 * @return true if cancellation request was submitted, false on error
 */
bool loopyIoUringCancelFd(loopyLoop *l, int fd) {
    if (!l || !l->state) {
        return false;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return false;
    }

    IoUringState *u = &state->uring;

    /* Get an SQE for the cancel operation */
    struct io_uring_sqe *sqe = ioUringGetSqe(u);
    if (!sqe) {
        return false;
    }

    /* Submit IORING_OP_ASYNC_CANCEL with fd
     * IORING_ASYNC_CANCEL_FD flag tells kernel to cancel all ops on this FD */
    sqe->opcode = IORING_OP_ASYNC_CANCEL;
    sqe->fd = fd;
    sqe->cancel_flags = (1U << 1); /* IORING_ASYNC_CANCEL_FD = (1U << 1) */
    sqe->user_data = 0;            /* Don't track the cancel operation itself */

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
    LINK_OP_FSYNC,
    LINK_OP_TIMEOUT
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

        struct {
            struct io_uring_timespec *ts;
            int count;
        } timeout;
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
 * Add a LINK_TIMEOUT operation to the link chain.
 *
 * LINK_TIMEOUT sets a timeout for the next operation in the chain. If the
 * next operation does not complete within the specified time, it will be
 * canceled and receive -ETIME in its completion callback.
 */
loopyIoUringLinkChain *
loopyIoUringLinkChainTimeout(loopyIoUringLinkChain *chain, uint64_t timeoutUs,
                             loopyIoUringFileCallback *cb, void *userData) {
    if (!chain || !linkChainEnsureCapacity(chain)) {
        return NULL;
    }

    LinkOp *op = &chain->ops[chain->count++];
    op->type = LINK_OP_TIMEOUT;
    op->cb = cb;
    op->userData = userData;

    /* Allocate timespec structure */
    struct io_uring_timespec *ts = zmalloc(sizeof(struct io_uring_timespec));
    if (!ts) {
        chain->count--; /* Undo the increment */
        return NULL;
    }

    /* Convert microseconds to struct io_uring_timespec (seconds and
     * nanoseconds) */
    uint64_t seconds = timeoutUs / 1000000;
    uint64_t remainingUs = timeoutUs % 1000000;
    ts->tv_sec = (int64_t)seconds;
    ts->tv_nsec = (int64_t)(remainingUs * 1000);

    op->params.timeout.ts = ts;

    /* count=-1 means the timeout applies to the next linked operation */
    op->params.timeout.count = -1;

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

        case LINK_OP_TIMEOUT:
            sqe->opcode = IORING_OP_LINK_TIMEOUT;
            sqe->addr = (uint64_t)(uintptr_t)op->params.timeout.ts;
            sqe->len = (uint32_t)op->params.timeout.count;
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

    /* Free any allocated timespec structures */
    if (chain->ops) {
        for (uint32_t i = 0; i < chain->count; i++) {
            if (chain->ops[i].type == LINK_OP_TIMEOUT) {
                zfree(chain->ops[i].params.timeout.ts);
            }
        }
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

/* ====================================================================
 * Buffer Pool Public API Implementation
 * ==================================================================== */

/* loopyIoUringBufferPool struct defined earlier in file */

typedef struct {
    bool completed;
    int32_t result;
} BufferPoolOpContext;

static void bufferPoolOpCallback(void *userData, int32_t result) {
    BufferPoolOpContext *ctx = (BufferPoolOpContext *)userData;
    ctx->result = result;
    ctx->completed = true;
}

loopyIoUringBufferPool *loopyIoUringBufferPoolNew(loopyLoop *l,
                                                  uint32_t bufferSize,
                                                  uint32_t bufferCount,
                                                  uint16_t groupId) {
    if (!l || !l->state || bufferSize == 0 || bufferCount == 0) {
        return NULL;
    }

    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return NULL;
    }

    IoUringState *u = &state->uring;

    if (groupId < u->maxBufferGroups && u->bufferPoolGroups[groupId]) {
        return NULL;
    }

    loopyIoUringBufferPool *pool = zmalloc(sizeof(loopyIoUringBufferPool));
    if (!pool) {
        return NULL;
    }

    pool->buffers = zmalloc((size_t)bufferSize * bufferCount);
    if (!pool->buffers) {
        zfree(pool);
        return NULL;
    }

    pool->loop = l;
    pool->bufferSize = bufferSize;
    pool->bufferCount = bufferCount;
    pool->groupId = groupId;
    pool->registered = false;

    BufferPoolOpContext ctx = {false, 0};
    uint64_t opId =
        ioUringSubmitProvideBuffers(u, pool->buffers, bufferSize, bufferCount,
                                    groupId, 0, bufferPoolOpCallback, &ctx);
    if (!opId) {
        zfree(pool->buffers);
        zfree(pool);
        return NULL;
    }

    int iterations = 0;
    while (!ctx.completed && iterations < 1000) {
        struct timeval tv = {0, 1000};
        loopyInternalPoll(l, &tv);
        iterations++;
    }

    if (!ctx.completed || ctx.result < 0) {
        zfree(pool->buffers);
        zfree(pool);
        return NULL;
    }

    pool->registered = true;
    if (groupId < u->maxBufferGroups) {
        u->bufferPoolGroups[groupId] = pool;
        u->bufferPoolCounts[groupId] = bufferCount;
        u->bufferPoolSizes[groupId] = bufferSize;
    }

    return pool;
}

void loopyIoUringBufferPoolFree(loopyIoUringBufferPool *pool) {
    if (!pool) {
        return;
    }

    if (pool->loop && pool->loop->state) {
        loopyInternalState *state = pool->loop->state;
        if (state->backend == BACKEND_IOURING) {
            IoUringState *u = &state->uring;

            if (pool->registered) {
                BufferPoolOpContext ctx = {false, 0};
                ioUringSubmitRemoveBuffers(u, pool->bufferCount, pool->groupId,
                                           bufferPoolOpCallback, &ctx);
                int iterations = 0;
                while (!ctx.completed && iterations < 100) {
                    struct timeval tv = {0, 1000};
                    loopyInternalPoll(pool->loop, &tv);
                    iterations++;
                }
            }

            if (pool->groupId < u->maxBufferGroups) {
                u->bufferPoolGroups[pool->groupId] = NULL;
                u->bufferPoolCounts[pool->groupId] = 0;
                u->bufferPoolSizes[pool->groupId] = 0;
            }
        }
    }

    zfree(pool->buffers);
    zfree(pool);
}

bool loopyIoUringBufferPoolStats(const loopyIoUringBufferPool *pool,
                                 uint32_t *totalBuffers, uint32_t *inUse) {
    if (!pool) {
        return false;
    }
    if (totalBuffers) {
        *totalBuffers = pool->bufferCount;
    }
    if (inUse) {
        *inUse = 0;
    }
    return true;
}

bool loopyIoUringBufferPoolsAvailable(loopyLoop *l) {
    if (!l || !l->state) {
        return false;
    }
    loopyInternalState *state = l->state;
    return state->backend == BACKEND_IOURING;
}

/* ====================================================================
 * Buffer Pool Operations (RecvPooled, ReadPooled)
 * ==================================================================== */

/**
 * Allocate a file operation slot for buffer-selected operations.
 *
 * Similar to ioUringFileOpAllocate but stores the buffer callback
 * and group ID for use when the CQE arrives.
 */
static uint64_t ioUringBufferOpAllocate(IoUringState *u,
                                        loopyIoUringBufferCallback *bufferCb,
                                        void *userData, uint16_t groupId) {
    loopyIoUringFileState *fs = &u->fileState;

    /* Find free slot */
    for (size_t i = 0; i < fs->opsSize; i++) {
        if (!fs->ops[i].active) {
            uint64_t id = fs->nextId++;
            fs->ops[i].id =
                id; /* Store WITHOUT bit 63 (like ioUringFileOpAllocate) */
            fs->ops[i].cb = NULL;           /* No standard callback */
            fs->ops[i].bufferCb = bufferCb; /* Buffer-aware callback */
            fs->ops[i].userData = userData;
            fs->ops[i].active = true;
            fs->ops[i].multishot = false;
            fs->ops[i].bufferGroup = groupId;
            return id | (1ULL << 63); /* Return WITH bit 63 for user_data */
        }
    }

    /* Expand array if full */
    size_t newSize = fs->opsSize ? fs->opsSize * 2 : 16;
    loopyIoUringFileOp *newOps =
        zrealloc(fs->ops, newSize * sizeof(loopyIoUringFileOp));
    if (!newOps) {
        return 0;
    }

    /* Initialize new slots */
    for (size_t i = fs->opsSize; i < newSize; i++) {
        newOps[i].active = false;
        newOps[i].cb = NULL;
        newOps[i].bufferCb = NULL;
        newOps[i].bufferGroup = 0;
    }

    fs->ops = newOps;
    size_t idx = fs->opsSize;
    fs->opsSize = newSize;

    uint64_t id = fs->nextId++;
    fs->ops[idx].id = id; /* Store WITHOUT bit 63 */
    fs->ops[idx].cb = NULL;
    fs->ops[idx].bufferCb = bufferCb;
    fs->ops[idx].userData = userData;
    fs->ops[idx].active = true;
    fs->ops[idx].multishot = false;
    fs->ops[idx].bufferGroup = groupId;
    return id | (1ULL << 63); /* Return WITH bit 63 */
}

/**
 * Submit a RECV operation with automatic buffer selection from pool.
 */
static uint64_t ioUringSubmitRecvPooled(IoUringState *u, int sockfd,
                                        uint16_t groupId, int flags,
                                        loopyIoUringBufferCallback *cb,
                                        void *userData) {
    struct io_uring_sqe *sqe = ioUringGetSqe(u);
    if (!sqe) {
        return 0;
    }

    /* Verify buffer group exists */
    if (groupId >= u->maxBufferGroups || !u->bufferPoolGroups[groupId]) {
        return 0;
    }

    /* Get buffer size from pool */
    loopyIoUringBufferPool *pool =
        (loopyIoUringBufferPool *)u->bufferPoolGroups[groupId];

    uint64_t id = ioUringBufferOpAllocate(u, cb, userData, groupId);
    if (!id) {
        return 0;
    }

    sqe->opcode = IORING_OP_RECV;
    sqe->fd = sockfd;
    sqe->addr = 0; /* No buffer - kernel selects from pool */
    sqe->len =
        pool->bufferSize; /* Must specify length even with buffer select */
    sqe->msg_flags = flags;
    sqe->flags = IOSQE_BUFFER_SELECT;
    sqe->buf_group = groupId;
    sqe->user_data = id;

    ioUringSubmitSqe(u);

    int ret = io_uring_enter(u->ringFd, 1, 0, 0, NULL);
    if (ret < 0) {
        loopyIoUringFileOp *op = ioUringFileOpFind(u, id);
        if (op) {
            op->active = false;
        }
        return 0;
    }

    return id;
}

/**
 * Submit a READ operation with automatic buffer selection from pool.
 */
static uint64_t ioUringSubmitReadPooled(IoUringState *u, int fd,
                                        uint16_t groupId, off_t offset,
                                        loopyIoUringBufferCallback *cb,
                                        void *userData) {
    struct io_uring_sqe *sqe = ioUringGetSqe(u);
    if (!sqe) {
        return 0;
    }

    /* Verify buffer group exists */
    if (groupId >= u->maxBufferGroups || !u->bufferPoolGroups[groupId]) {
        return 0;
    }

    /* Get buffer size from pool */
    loopyIoUringBufferPool *pool =
        (loopyIoUringBufferPool *)u->bufferPoolGroups[groupId];

    uint64_t id = ioUringBufferOpAllocate(u, cb, userData, groupId);
    if (!id) {
        return 0;
    }

    sqe->opcode = IORING_OP_READ;
    sqe->fd = fd;
    sqe->addr = 0; /* No buffer - kernel selects from pool */
    sqe->len =
        pool->bufferSize; /* Must specify length even with buffer select */
    sqe->off = offset;
    sqe->flags = IOSQE_BUFFER_SELECT;
    sqe->buf_group = groupId;
    sqe->user_data = id;

    ioUringSubmitSqe(u);

    int ret = io_uring_enter(u->ringFd, 1, 0, 0, NULL);
    if (ret < 0) {
        loopyIoUringFileOp *op = ioUringFileOpFind(u, id);
        if (op) {
            op->active = false;
        }
        return 0;
    }

    return id;
}

/**
 * Provide buffers to io_uring for dynamic buffer selection.
 *
 * This is an advanced API for fine-grained buffer pool management.
 * Most users should use loopyIoUringBufferPoolNew instead.
 *
 * @param l        Event loop
 * @param buffers  Pointer to contiguous buffer memory
 * @param bufSize  Size of each buffer
 * @param count    Number of buffers
 * @param groupId  Buffer group ID (0-65535)
 * @param bufId    Starting buffer ID
 * @param cb       Completion callback
 * @param userData User data
 * @return Operation ID on success, 0 on failure
 */
uint64_t loopyIoUringProvideBuffers(loopyLoop *l, void *buffers, size_t bufSize,
                                    uint32_t count, uint16_t groupId,
                                    uint16_t bufId,
                                    loopyIoUringFileCallback *cb,
                                    void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitProvideBuffers(&state->uring, buffers, bufSize, count,
                                       groupId, bufId, cb, userData);
}

/**
 * Remove buffers from io_uring buffer pool.
 *
 * @param l        Event loop
 * @param count    Number of buffers to remove
 * @param groupId  Buffer group ID
 * @param cb       Completion callback
 * @param userData User data
 * @return Operation ID on success, 0 on failure
 */
uint64_t loopyIoUringRemoveBuffers(loopyLoop *l, uint32_t count,
                                   uint16_t groupId,
                                   loopyIoUringFileCallback *cb,
                                   void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitRemoveBuffers(&state->uring, count, groupId, cb,
                                      userData);
}

/**
 * Receive data using automatic buffer selection from pool.
 */
uint64_t loopyIoUringRecvPooled(loopyLoop *l, int sockfd, uint16_t groupId,
                                int flags, loopyIoUringBufferCallback *cb,
                                void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitRecvPooled(&state->uring, sockfd, groupId, flags, cb,
                                   userData);
}

/**
 * Read data using automatic buffer selection from pool.
 */
uint64_t loopyIoUringReadPooled(loopyLoop *l, int fd, uint16_t groupId,
                                off_t offset, loopyIoUringBufferCallback *cb,
                                void *userData) {
    if (!l || !l->state) {
        return 0;
    }
    loopyInternalState *state = l->state;
    if (state->backend != BACKEND_IOURING) {
        return 0;
    }
    return ioUringSubmitReadPooled(&state->uring, fd, groupId, offset, cb,
                                   userData);
}

#else /* !__linux__ */

#endif /* __linux__ */
