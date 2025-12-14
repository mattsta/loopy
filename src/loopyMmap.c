/* loopyMmap - Memory-mapped file I/O implementation
 *
 * High-performance file access via memory mapping. Provides zero-copy I/O
 * by mapping files directly into process memory.
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

#include "loopyMmap.h"

#include "../deps/datakit/src/datakit.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* ====================================================================
 * Internal Structure
 * ==================================================================== */

/**
 * Memory mapping handle (opaque to users).
 *
 * Tracks the mapped region and its metadata.
 */
struct loopyMmap {
    void *addr;    /* Base address of mapping */
    size_t length; /* Size of mapping in bytes */
    bool valid;    /* Is this mapping currently valid? */
};

/* ====================================================================
 * Utility Functions
 * ==================================================================== */

size_t loopyMmapPageSize(void) {
    static size_t pageSize = 0;
    if (pageSize == 0) {
        pageSize = (size_t)sysconf(_SC_PAGESIZE);
    }
    return pageSize;
}

size_t loopyMmapAlignSize(size_t size) {
    size_t pageSize = loopyMmapPageSize();
    return (size + pageSize - 1) & ~(pageSize - 1);
}

/* ====================================================================
 * Protection and Flag Conversion
 * ==================================================================== */

/**
 * Convert loopyMmapProt to system mmap prot flags.
 */
static int convertProtFlags(loopyMmapProt prot) {
    int sysProt = 0;

    if (prot & LOOPY_MMAP_PROT_READ) {
        sysProt |= PROT_READ;
    }
    if (prot & LOOPY_MMAP_PROT_WRITE) {
        sysProt |= PROT_WRITE;
    }
    if (prot & LOOPY_MMAP_PROT_EXEC) {
        sysProt |= PROT_EXEC;
    }
    if (prot == LOOPY_MMAP_PROT_NONE) {
        sysProt = PROT_NONE;
    }

    return sysProt;
}

/**
 * Convert loopyMmapFlags to system mmap flags.
 */
static int convertMapFlags(loopyMmapFlags flags) {
    int sysFlags = 0;

    if (flags & LOOPY_MMAP_SHARED) {
        sysFlags |= MAP_SHARED;
    }
    if (flags & LOOPY_MMAP_PRIVATE) {
        sysFlags |= MAP_PRIVATE;
    }
    if (flags & LOOPY_MMAP_FIXED) {
        sysFlags |= MAP_FIXED;
    }
    if (flags & LOOPY_MMAP_ANONYMOUS) {
#ifdef MAP_ANONYMOUS
        sysFlags |= MAP_ANONYMOUS;
#else
        sysFlags |= MAP_ANON; /* BSD/macOS */
#endif
    }

    /* Optional optimization flags (not available on all platforms) */
#ifdef MAP_POPULATE
    if (flags & LOOPY_MMAP_POPULATE) {
        sysFlags |= MAP_POPULATE;
    }
#endif

#ifdef MAP_HUGETLB
    if (flags & LOOPY_MMAP_HUGETLB) {
        sysFlags |= MAP_HUGETLB;
    }
#endif

    return sysFlags;
}

/**
 * Convert loopyMmapSyncFlags to system msync flags.
 */
static int convertSyncFlags(loopyMmapSyncFlags flags) {
    int sysFlags = 0;

    if (flags & LOOPY_MMAP_SYNC_ASYNC) {
        sysFlags |= MS_ASYNC;
    }
    if (flags & LOOPY_MMAP_SYNC_SYNC) {
        sysFlags |= MS_SYNC;
    }
    if (flags & LOOPY_MMAP_SYNC_INVALIDATE) {
        sysFlags |= MS_INVALIDATE;
    }

    return sysFlags;
}

/**
 * Convert loopyMmapAdvice to system madvise advice.
 */
static int convertAdviceFlags(loopyMmapAdvice advice) {
    switch (advice) {
    case LOOPY_MMAP_ADVICE_NORMAL:
        return MADV_NORMAL;
    case LOOPY_MMAP_ADVICE_RANDOM:
        return MADV_RANDOM;
    case LOOPY_MMAP_ADVICE_SEQUENTIAL:
        return MADV_SEQUENTIAL;
    case LOOPY_MMAP_ADVICE_WILLNEED:
        return MADV_WILLNEED;
    case LOOPY_MMAP_ADVICE_DONTNEED:
        return MADV_DONTNEED;
    default:
        return MADV_NORMAL;
    }
}

/* ====================================================================
 * File Mapping Operations
 * ==================================================================== */

loopyMmap *loopyMmapFile(int fd, size_t offset, size_t length,
                         loopyMmapProt prot, loopyMmapFlags flags) {
    if (fd < 0) {
        return NULL;
    }

    /* If length is 0, map entire file */
    if (length == 0) {
        struct stat st;
        if (fstat(fd, &st) != 0) {
            return NULL;
        }
        if (st.st_size == 0) {
            return NULL; /* Empty file */
        }
        length = (size_t)st.st_size;

        /* Adjust length if offset is specified */
        if (offset > 0) {
            if (offset >= length) {
                return NULL; /* Offset beyond file size */
            }
            length -= offset;
        }
    }

    /* Verify offset is page-aligned */
    size_t pageSize = loopyMmapPageSize();
    if (offset % pageSize != 0) {
        return NULL; /* Offset must be page-aligned */
    }

    /* Convert flags */
    int sysProt = convertProtFlags(prot);
    int sysFlags = convertMapFlags(flags);

    /* Ensure either SHARED or PRIVATE is set */
    if (!(sysFlags & (MAP_SHARED | MAP_PRIVATE))) {
        sysFlags |= MAP_PRIVATE; /* Default to private */
    }

    /* Perform mmap */
    void *addr = mmap(NULL, length, sysProt, sysFlags, fd, (off_t)offset);
    if (addr == MAP_FAILED) {
        return NULL;
    }

    /* Allocate handle */
    loopyMmap *m = zmalloc(sizeof(loopyMmap));
    if (!m) {
        munmap(addr, length);
        return NULL;
    }

    m->addr = addr;
    m->length = length;
    m->valid = true;

    return m;
}

loopyMmap *loopyMmapAnon(size_t length, loopyMmapProt prot,
                         loopyMmapFlags flags) {
    if (length == 0) {
        return NULL;
    }

    /* Convert flags and add MAP_ANONYMOUS */
    int sysProt = convertProtFlags(prot);
    int sysFlags = convertMapFlags(flags);

#ifdef MAP_ANONYMOUS
    sysFlags |= MAP_ANONYMOUS;
#else
    sysFlags |= MAP_ANON;
#endif

    /* Ensure either SHARED or PRIVATE is set */
    if (!(sysFlags & (MAP_SHARED | MAP_PRIVATE))) {
        sysFlags |= MAP_PRIVATE; /* Default to private */
    }

    /* Perform mmap with fd = -1 for anonymous mapping */
    void *addr = mmap(NULL, length, sysProt, sysFlags, -1, 0);
    if (addr == MAP_FAILED) {
        return NULL;
    }

    /* Allocate handle */
    loopyMmap *m = zmalloc(sizeof(loopyMmap));
    if (!m) {
        munmap(addr, length);
        return NULL;
    }

    m->addr = addr;
    m->length = length;
    m->valid = true;

    return m;
}

void loopyMmapUnmap(loopyMmap *m) {
    if (!m) {
        return;
    }

    if (m->valid && m->addr != NULL) {
        munmap(m->addr, m->length);
    }

    m->addr = NULL;
    m->length = 0;
    m->valid = false;

    zfree(m);
}

/* ====================================================================
 * Mapping Introspection
 * ==================================================================== */

void *loopyMmapAddress(const loopyMmap *m) {
    if (!m || !m->valid) {
        return NULL;
    }
    return m->addr;
}

size_t loopyMmapSize(const loopyMmap *m) {
    if (!m || !m->valid) {
        return 0;
    }
    return m->length;
}

bool loopyMmapIsValid(const loopyMmap *m) {
    return m != NULL && m->valid;
}

/* ====================================================================
 * Synchronization Operations
 * ==================================================================== */

bool loopyMmapSync(loopyMmap *m, size_t offset, size_t length,
                   loopyMmapSyncFlags flags) {
    if (!m || !m->valid) {
        return false;
    }

    /* If length is 0, sync entire mapping */
    if (length == 0) {
        length = m->length - offset;
    }

    /* Validate range */
    if (offset >= m->length || offset + length > m->length) {
        return false;
    }

    /* Calculate sync address (must be page-aligned) */
    size_t pageSize = loopyMmapPageSize();
    size_t alignedOffset = offset & ~(pageSize - 1);
    size_t alignedLength = length + (offset - alignedOffset);

    void *syncAddr = (char *)m->addr + alignedOffset;

    int sysFlags = convertSyncFlags(flags);

    return msync(syncAddr, alignedLength, sysFlags) == 0;
}

bool loopyMmapAdvise(loopyMmap *m, size_t offset, size_t length,
                     loopyMmapAdvice advice) {
    if (!m || !m->valid) {
        return false;
    }

    /* If length is 0, advise entire mapping */
    if (length == 0) {
        length = m->length - offset;
    }

    /* Validate range */
    if (offset >= m->length || offset + length > m->length) {
        return false;
    }

    void *adviseAddr = (char *)m->addr + offset;
    int sysAdvice = convertAdviceFlags(advice);

    return madvise(adviseAddr, length, sysAdvice) == 0;
}

bool loopyMmapLock(loopyMmap *m, size_t offset, size_t length) {
    if (!m || !m->valid) {
        return false;
    }

    /* If length is 0, lock entire mapping */
    if (length == 0) {
        length = m->length - offset;
    }

    /* Validate range */
    if (offset >= m->length || offset + length > m->length) {
        return false;
    }

    void *lockAddr = (char *)m->addr + offset;

    return mlock(lockAddr, length) == 0;
}

bool loopyMmapUnlock(loopyMmap *m, size_t offset, size_t length) {
    if (!m || !m->valid) {
        return false;
    }

    /* If length is 0, unlock entire mapping */
    if (length == 0) {
        length = m->length - offset;
    }

    /* Validate range */
    if (offset >= m->length || offset + length > m->length) {
        return false;
    }

    void *unlockAddr = (char *)m->addr + offset;

    return munlock(unlockAddr, length) == 0;
}
