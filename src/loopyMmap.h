/* loopyMmap - Memory-mapped file I/O interface
 *
 * High-performance file access via memory mapping. Provides zero-copy I/O
 * by mapping files directly into process memory, achieving significant
 * performance improvements for large file operations.
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

/* ====================================================================
 * Types
 * ==================================================================== */

/**
 * Memory mapping handle.
 *
 * Represents a memory-mapped region. Treat as opaque - do not access
 * fields directly.
 */
typedef struct loopyMmap loopyMmap;

/**
 * Memory mapping protection flags.
 *
 * These flags control read/write/execute permissions for the mapped region.
 */
typedef enum {
    LOOPY_MMAP_PROT_NONE = 0x0,  /* No access */
    LOOPY_MMAP_PROT_READ = 0x1,  /* Read access */
    LOOPY_MMAP_PROT_WRITE = 0x2, /* Write access */
    LOOPY_MMAP_PROT_EXEC = 0x4,  /* Execute access */
} loopyMmapProt;

/**
 * Memory mapping flags.
 *
 * Control visibility and behavior of the mapping.
 */
typedef enum {
    LOOPY_MMAP_SHARED = 0x1,      /* Changes are visible to other processes */
    LOOPY_MMAP_PRIVATE = 0x2,     /* Changes are copy-on-write */
    LOOPY_MMAP_FIXED = 0x10,      /* Map at exact address (dangerous) */
    LOOPY_MMAP_ANONYMOUS = 0x20,  /* No file backing (like malloc) */
    LOOPY_MMAP_POPULATE = 0x8000, /* Populate page tables (prefault) */
    LOOPY_MMAP_HUGETLB = 0x40000, /* Use huge pages (2MB/1GB) */
} loopyMmapFlags;

/**
 * Memory sync flags for msync().
 *
 * Control how changes are flushed to disk.
 */
typedef enum {
    LOOPY_MMAP_SYNC_ASYNC = 0x1,      /* Schedule flush, return immediately */
    LOOPY_MMAP_SYNC_SYNC = 0x4,       /* Wait for flush to complete */
    LOOPY_MMAP_SYNC_INVALIDATE = 0x2, /* Invalidate cached pages */
} loopyMmapSyncFlags;

/**
 * Memory advice flags for madvise().
 *
 * Hint kernel about expected access patterns for optimization.
 */
typedef enum {
    LOOPY_MMAP_ADVICE_NORMAL = 0,     /* No special treatment */
    LOOPY_MMAP_ADVICE_RANDOM = 1,     /* Random access pattern */
    LOOPY_MMAP_ADVICE_SEQUENTIAL = 2, /* Sequential access */
    LOOPY_MMAP_ADVICE_WILLNEED = 3,   /* Will need soon (prefetch) */
    LOOPY_MMAP_ADVICE_DONTNEED = 4,   /* Won't need (drop cache) */
} loopyMmapAdvice;

/* ====================================================================
 * File Mapping Operations
 * ==================================================================== */

/**
 * Create a memory mapping for a file.
 *
 * Maps a file (or portion of it) into process memory for zero-copy I/O.
 * The file must be opened with appropriate flags (O_RDONLY, O_RDWR).
 *
 * @param fd       File descriptor (must be valid and opened)
 * @param offset   Offset in file to start mapping (must be page-aligned)
 * @param length   Number of bytes to map (0 = map entire file)
 * @param prot     Protection flags (LOOPY_MMAP_PROT_READ, etc.)
 * @param flags    Mapping flags (LOOPY_MMAP_SHARED, LOOPY_MMAP_PRIVATE)
 * @return Mapping handle on success, NULL on failure
 *
 * Performance notes:
 *  - Zero-copy: No data copied between kernel and userspace
 *  - Lazy loading: Pages loaded on first access (page fault)
 *  - Use LOOPY_MMAP_POPULATE to prefault all pages
 *  - offset must be page-aligned (typically 4KB)
 *
 * Example:
 *   int fd = open("data.bin", O_RDONLY);
 *   loopyMmap *m = loopyMmapFile(fd, 0, 0, LOOPY_MMAP_PROT_READ,
 *                                LOOPY_MMAP_PRIVATE);
 *   const char *data = loopyMmapAddress(m);
 *   // ... use data ...
 *   loopyMmapUnmap(m);
 */
loopyMmap *loopyMmapFile(int fd, size_t offset, size_t length,
                         loopyMmapProt prot, loopyMmapFlags flags);

/**
 * Create an anonymous memory mapping.
 *
 * Allocates memory similar to malloc() but using mmap(). Useful for:
 *  - Large allocations (>malloc threshold)
 *  - Shared memory between processes (with LOOPY_MMAP_SHARED)
 *  - Memory that needs specific alignment or huge pages
 *
 * @param length Number of bytes to allocate
 * @param prot   Protection flags
 * @param flags  Mapping flags (typically LOOPY_MMAP_PRIVATE |
 * LOOPY_MMAP_ANONYMOUS)
 * @return Mapping handle on success, NULL on failure
 *
 * Use cases:
 *  - Shared memory IPC: Use LOOPY_MMAP_SHARED | LOOPY_MMAP_ANONYMOUS
 *  - Large buffers: Better than malloc for multi-MB allocations
 *  - Huge pages: Add LOOPY_MMAP_HUGETLB for 2MB/1GB pages
 *
 * Example:
 *   loopyMmap *m = loopyMmapAnon(1024 * 1024 * 100, // 100MB
 *                                LOOPY_MMAP_PROT_READ | LOOPY_MMAP_PROT_WRITE,
 *                                LOOPY_MMAP_PRIVATE | LOOPY_MMAP_ANONYMOUS);
 */
loopyMmap *loopyMmapAnon(size_t length, loopyMmapProt prot,
                         loopyMmapFlags flags);

/**
 * Unmap and free a memory mapping.
 *
 * Unmaps the region and frees the loopyMmap handle. Any pending changes
 * to SHARED mappings are flushed to disk.
 *
 * @param m Mapping to unmap (can be NULL)
 *
 * Note: Accessing the mapped memory after unmapping causes SIGSEGV.
 */
void loopyMmapUnmap(loopyMmap *m);

/* ====================================================================
 * Mapping Introspection
 * ==================================================================== */

/**
 * Get the mapped memory address.
 *
 * Returns a pointer to the start of the mapped region. This pointer
 * remains valid until loopyMmapUnmap() is called.
 *
 * @param m Mapping handle
 * @return Pointer to mapped memory, or NULL if invalid
 *
 * Note: Cast to appropriate type based on file contents.
 */
void *loopyMmapAddress(const loopyMmap *m);

/**
 * Get the size of the mapped region.
 *
 * @param m Mapping handle
 * @return Size in bytes, or 0 if invalid
 */
size_t loopyMmapSize(const loopyMmap *m);

/**
 * Check if mapping is valid.
 *
 * @param m Mapping handle
 * @return true if valid, false otherwise
 */
bool loopyMmapIsValid(const loopyMmap *m);

/* ====================================================================
 * Synchronization Operations
 * ==================================================================== */

/**
 * Flush changes to disk.
 *
 * For SHARED mappings, write modified pages back to the file. For
 * PRIVATE mappings, this is a no-op.
 *
 * @param m      Mapping handle
 * @param offset Offset within mapping to start flush (0 = start)
 * @param length Number of bytes to flush (0 = entire mapping)
 * @param flags  Sync flags (ASYNC, SYNC, INVALIDATE)
 * @return true on success, false on error
 *
 * Use cases:
 *  - Database commits: LOOPY_MMAP_SYNC_SYNC for durability
 *  - Periodic checkpoints: LOOPY_MMAP_SYNC_ASYNC for background flush
 *  - Clear cache: LOOPY_MMAP_SYNC_INVALIDATE to reload from disk
 *
 * Example:
 *   // Ensure database page is on disk
 *   loopyMmapSync(dbMapping, pageOffset, pageSize, LOOPY_MMAP_SYNC_SYNC);
 */
bool loopyMmapSync(loopyMmap *m, size_t offset, size_t length,
                   loopyMmapSyncFlags flags);

/**
 * Advise kernel about access patterns.
 *
 * Provides hints to the kernel for optimization. Does not affect
 * semantics, only performance.
 *
 * @param m      Mapping handle
 * @param offset Offset within mapping (0 = start)
 * @param length Number of bytes to advise about (0 = entire mapping)
 * @param advice Access pattern hint
 * @return true on success, false on error
 *
 * Advice types:
 *  - NORMAL: Default behavior, balanced
 *  - RANDOM: Disable read-ahead, reduce memory pressure
 *  - SEQUENTIAL: Aggressive read-ahead
 *  - WILLNEED: Prefetch pages into memory now
 *  - DONTNEED: Drop pages from cache (free memory)
 *
 * Example:
 *   // Tell kernel we'll scan file sequentially
 *   loopyMmapAdvise(m, 0, 0, LOOPY_MMAP_ADVICE_SEQUENTIAL);
 */
bool loopyMmapAdvise(loopyMmap *m, size_t offset, size_t length,
                     loopyMmapAdvice advice);

/**
 * Lock pages in memory (prevent swapping).
 *
 * Locks the specified range in RAM, preventing the kernel from
 * swapping it to disk. Useful for real-time applications or
 * security-sensitive data.
 *
 * @param m      Mapping handle
 * @param offset Offset within mapping
 * @param length Number of bytes to lock (0 = entire mapping)
 * @return true on success, false on error
 *
 * Note: May require elevated privileges (CAP_IPC_LOCK on Linux).
 *       Check rlimit RLIMIT_MEMLOCK for maximum locked memory.
 */
bool loopyMmapLock(loopyMmap *m, size_t offset, size_t length);

/**
 * Unlock previously locked pages.
 *
 * @param m      Mapping handle
 * @param offset Offset within mapping
 * @param length Number of bytes to unlock (0 = entire mapping)
 * @return true on success, false on error
 */
bool loopyMmapUnlock(loopyMmap *m, size_t offset, size_t length);

/* ====================================================================
 * Utility Functions
 * ==================================================================== */

/**
 * Get system page size.
 *
 * Returns the system's memory page size (typically 4096 bytes).
 * Use this for offset alignment when creating mappings.
 *
 * @return Page size in bytes
 */
size_t loopyMmapPageSize(void);

/**
 * Align size up to page boundary.
 *
 * Rounds size up to the next multiple of page size.
 *
 * @param size Size to align
 * @return Aligned size
 */
size_t loopyMmapAlignSize(size_t size);
