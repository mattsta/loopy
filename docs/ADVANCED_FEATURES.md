# Loopy Advanced Features

This guide covers the advanced modules of the loopy event loop library, enabling you to build sophisticated, high-performance applications with async I/O, inter-process communication, resource management, and monitoring capabilities.

---

## Table of Contents

1. [Filesystem Operations](#filesystem-operations)
2. [File Watching](#file-watching)
3. [File Locking](#file-locking)
4. [Memory Mapping](#memory-mapping)
5. [Process Management](#process-management)
6. [Channels and Pub/Sub](#channels-and-pubsub)
7. [Rate Limiting](#rate-limiting)
8. [Concurrency Management](#concurrency-management)
9. [Metrics Collection](#metrics-collection)
10. [Thread Pools](#thread-pools)
11. [Async Cross-Thread Notifications](#async-cross-thread-notifications)

---

## Filesystem Operations

### Overview

`loopyFS` provides non-blocking filesystem operations using the thread pool to prevent blocking the event loop. It supports a comprehensive set of file operations, directory manipulation, and extended file handling.

### Key Features

- **Non-blocking I/O**: File operations execute on worker threads, never blocking the event loop
- **Both sync and async modes**: Call with callbacks for async, or NULL for synchronous blocking
- **Extensive operation types**: Open, close, read, write, stat, chmod, link, copy, etc.
- **Direct I/O support**: Bypass kernel cache with O_DIRECT flag
- **Directory scanning**: Efficient directory listing with stat information
- **Extended operations**: Symlinks, hard links, file copying, ownership changes

### Core Operations

#### Basic File Operations

```c
// Open a file
loopyFSRequest *req = loopyFSOpen(loop, "data.txt", O_RDONLY, 0,
                                  onFileOpen, userData);

// Read from file
loopyFSRequest *req = loopyFSRead(loop, fd, buffer, size, -1,
                                  onFileRead, userData);

// Write to file
loopyFSRequest *req = loopyFSWrite(loop, fd, buffer, size, -1,
                                   onFileWrite, userData);

// Close file
loopyFSRequest *req = loopyFSClose(loop, fd, onFileClose, userData);

// Sync changes to disk (data + metadata)
loopyFSRequest *req = loopyFSFsync(loop, fd, onFsync, userData);

// Sync data only (no metadata)
loopyFSRequest *req = loopyFSFdatasync(loop, fd, onFdatasync, userData);
```

Callback signature:

```c
void fileCallback(loopyLoop *loop, loopyFSRequest *req, ssize_t result, void *userData) {
    // result: bytes read/written, fd for open, 0 for success, -1 for error
    if (result < 0) {
        printf("Error: %s\n", loopyFSGetError(req));
    }
}
```

#### Directory Operations

```c
// Create directory
loopyFSRequest *req = loopyFSMkdir(loop, "mydir", 0755, onDirCreated, userData);

// Remove directory (must be empty)
loopyFSRequest *req = loopyFSRmdir(loop, "mydir", onDirRemoved, userData);

// Scan directory (efficient batch operation)
loopyFSRequest *req = loopyFSScandir(loop, "mydir", onScanComplete, userData);

// After callback, retrieve entries:
void onScanComplete(loopyLoop *loop, loopyFSRequest *req, ssize_t result, void *userData) {
    size_t count;
    const loopyFSDirent *entries = loopyFSRequestGetDirents(req, &count);
    for (size_t i = 0; i < count; i++) {
        printf("%s (type: %d)\n", entries[i].name, entries[i].type);
    }
    loopyFSRequestFree(req);
}
```

#### File Metadata

```c
struct stat st;

// Get file stats
loopyFSRequest *req = loopyFSStat(loop, "file.txt", &st, onStatComplete, userData);

// Get stats by fd
loopyFSRequest *req = loopyFSFstat(loop, fd, &st, onFstatComplete, userData);

// Get symlink stats (don't follow link)
loopyFSRequest *req = loopyFSLstat(loop, "link", &st, onLstatComplete, userData);

// In callback:
void onStatComplete(loopyLoop *loop, loopyFSRequest *req, ssize_t result, void *userData) {
    const struct stat *st = loopyFSRequestGetStatbuf(req);
    if (st) {
        printf("Size: %ld bytes, Perms: %o\n", st->st_size, st->st_mode);
    }
}
```

#### Extended Operations

```c
// Rename/move file
loopyFSRequest *req = loopyFSRename(loop, "old.txt", "new.txt",
                                    onRenamed, userData);

// Delete file
loopyFSRequest *req = loopyFSUnlink(loop, "file.txt", onDeleted, userData);

// Change permissions
loopyFSRequest *req = loopyFSChmod(loop, "file.txt", 0644, onChmodComplete, userData);

// Change ownership
loopyFSRequest *req = loopyFSChown(loop, "file.txt", uid, gid, onChownComplete, userData);

// Create symlink
loopyFSRequest *req = loopyFSSymlink(loop, "target", "link", onSymlinkCreated, userData);

// Read symlink
char buf[256];
loopyFSRequest *req = loopyFSReadlink(loop, "link", buf, sizeof(buf),
                                      onReadlinkComplete, userData);

// Copy file
loopyFSRequest *req = loopyFSCopyfile(loop, "src.txt", "dst.txt",
                                      LOOPY_FS_COPY_DEFAULT, onCopyComplete, userData);

// Create temp directory
char tpl[] = "/tmp/appXXXXXX";
loopyFSRequest *req = loopyFSMkdtemp(loop, tpl, onTempDirCreated, userData);
// After callback, tpl contains the created directory path

// Create temp file
char tpl[] = "/tmp/appXXXXXX";
loopyFSRequest *req = loopyFSMkstemp(loop, tpl, onTempFileCreated, userData);
// After callback, tpl contains the created file path, result is fd
```

#### Direct I/O (Unbuffered)

Direct I/O bypasses the kernel page cache, useful for applications that manage their own caching:

```c
#define ALIGNMENT 512

// Allocate properly aligned buffer
void *buf = loopyFSAllocAligned(4096, ALIGNMENT);

// Open with O_DIRECT (Linux only)
int fd = open("data.bin", O_RDONLY | O_DIRECT);

// Read must be aligned
loopyFSRequest *req = loopyFSRead(loop, fd, buf, 4096, 0, onRead, userData);

// Cleanup
close(fd);
loopyFSFreeAligned(buf);
```

#### Filesystem Statistics

```c
loopyStatfs statfs_buf;

// Get filesystem statistics
loopyFSRequest *req = loopyFSStatfs(loop, "/", &statfs_buf,
                                    onStatfsComplete, userData);

void onStatfsComplete(loopyLoop *loop, loopyFSRequest *req, ssize_t result, void *userData) {
    const loopyStatfs *fs = loopyFSRequestGetStatfsBuf(req);
    if (fs) {
        printf("Available blocks: %llu\n", fs->bavail);
        printf("Free space: %llu bytes\n", fs->bavail * fs->bsize);
    }
}
```

### Synchronous vs Asynchronous

The same API supports both modes:

```c
// Asynchronous (callback-based)
loopyFSRequest *req = loopyFSOpen(loop, "file.txt", O_RDONLY, 0, onOpen, data);
// loopyAsyncRun() will invoke onOpen when complete

// Synchronous (blocking, NULL callback)
loopyFSRequest *req = loopyFSOpen(NULL, "file.txt", O_RDONLY, 0, NULL, NULL);
int fd = loopyFSRequestGetResult(req);
loopyFSRequestFree(req);
```

### Request Management

```c
// Get operation details
loopyFSType opType = loopyFSRequestGetType(req);
const char *path = loopyFSRequestGetPath(req);
ssize_t result = loopyFSRequestGetResult(req);
const struct stat *st = loopyFSRequestGetStatbuf(req);

// Cancel pending request (must not be started)
if (loopyFSCancel(req)) {
    printf("Request cancelled\n");
    // Callback will be invoked with errno = ECANCELED
}

// Free request
loopyFSRequestFree(req);
```

---

## File Watching

### Overview

`loopyWatch` provides cross-platform file system event monitoring using kernel mechanisms (inotify on Linux, kqueue on BSD/macOS) with optional polling fallback. `loopyFSPoll` provides pure polling-based monitoring for network filesystems.

### loopyWatch - Kernel-Based Monitoring

#### Features

- **Efficient kernel notifications**: inotify on Linux, kqueue on BSD/macOS
- **File and directory watching**: Monitor individual files or directory contents
- **Event filtering**: Subscribe to specific event types
- **Cross-platform**: Automatic backend selection

#### Event Types

```c
LOOPY_WATCH_MODIFY   // File content modified
LOOPY_WATCH_CREATE   // File created (directory only)
LOOPY_WATCH_DELETE   // File deleted
LOOPY_WATCH_RENAME   // File renamed
LOOPY_WATCH_ATTRIB   // Attributes changed (permissions, owner)
LOOPY_WATCH_ALL      // All events
```

#### Usage Example

```c
#include "loopyWatch.h"

void onFileChange(loopyWatch *w, const loopyWatchInfo *info) {
    printf("Watch %lu: %s event on %s\n",
           info->watchId, loopyWatchEventName(info->events), info->path);
    if (info->events & LOOPY_WATCH_MODIFY) {
        printf("  File was modified\n");
    }
    if (info->events & LOOPY_WATCH_CREATE) {
        printf("  File created: %s\n", info->filename);
    }
}

// Create watcher
loopyWatch *watcher = loopyWatchNew(loop);

// Watch a file
loopyWatchId id = loopyWatchAdd(watcher, "/etc/config.json",
                                LOOPY_WATCH_MODIFY, onFileChange, NULL);

// Watch a directory
loopyWatchId dirId = loopyWatchAdd(watcher, "/tmp",
                                   LOOPY_WATCH_CREATE | LOOPY_WATCH_DELETE,
                                   onFileChange, NULL);

// Remove a watch
loopyWatchRemove(watcher, id);

// Remove all watches
loopyWatchRemoveAll(watcher);

// Check backend
printf("Using backend: %s\n", loopyWatchBackendName());

// Cleanup
loopyWatchFree(watcher);
```

#### Watch Management

```c
// Get number of active watches
size_t count = loopyWatchCount(watcher);

// Get event name
const char *name = loopyWatchEventName(LOOPY_WATCH_MODIFY);

// Get backend name (inotify, kqueue, poll)
const char *backend = loopyWatchBackendName();

// Access handle data
void *data = loopyWatchGetData(watcher);
loopyWatchSetData(watcher, myData);
```

### loopyFSPoll - Polling-Based Monitoring

Better for network filesystems where kernel notifications may not work reliably.

#### Features

- **Pure polling approach**: Uses stat() calls periodically
- **Network filesystem friendly**: Works over NFS, SMB, etc.
- **Lower performance**: But higher reliability on network mounts
- **Configurable interval**: Adjust poll frequency

#### Event Types

```c
LOOPY_FSPOLL_MODIFIED    // Modification time changed
LOOPY_FSPOLL_SIZE        // File size changed
LOOPY_FSPOLL_DELETED     // File was deleted
LOOPY_FSPOLL_CREATED     // File appeared (was missing)
LOOPY_FSPOLL_PERMISSIONS // Permissions changed
LOOPY_FSPOLL_OWNER       // Owner/group changed
LOOPY_FSPOLL_TYPE        // File type changed (rare)
```

#### Usage Example

```c
#include "loopyFSPoll.h"

void onFileChange(loopyFSPoll *fsp, const loopyFSPollInfo *info) {
    printf("Poll %lu: Changes on %s\n", info->pollId, info->path);

    if (info->events & LOOPY_FSPOLL_MODIFIED) {
        printf("  File was modified\n");
    }
    if (info->events & LOOPY_FSPOLL_DELETED) {
        printf("  File was deleted\n");
    }
    if (info->prevStat && info->currStat) {
        printf("  Size: %ld -> %ld bytes\n",
               info->prevStat->st_size, info->currStat->st_size);
    }
}

// Create with configuration
loopyFSPollConfig config;
loopyFSPollConfigInit(&config);
config.intervalMs = 5000;  // Poll every 5 seconds
config.followSymlinks = true;

loopyFSPoll *poller = loopyFSPollNew(loop, &config);

// Start monitoring a file
loopyFSPollId id = loopyFSPollStart(poller, "/var/log/app.log",
                                    LOOPY_FSPOLL_MODIFIED | LOOPY_FSPOLL_DELETED,
                                    onFileChange, NULL);

// Change poll interval dynamically
loopyFSPollSetInterval(poller, 2000);  // Poll every 2 seconds

// Stop monitoring
loopyFSPollStop(poller, id);

// Stop all
loopyFSPollStopAll(poller);

// Cleanup
loopyFSPollFree(poller);
```

#### Polling Configuration

```c
loopyFSPollConfig config;
loopyFSPollConfigInit(&config);

config.intervalMs = 1000;      // Poll interval (default: 1000ms)
config.followSymlinks = true;  // Use stat() vs lstat() (default: true)

loopyFSPoll *poller = loopyFSPollNew(loop, &config);
```

#### Query and Status

```c
// Get number of monitored files
size_t count = loopyFSPollCount(poller);

// Get current poll interval
uint64_t intervalMs = loopyFSPollGetInterval(poller);

// Get current stat for a file
struct stat st;
if (loopyFSPollGetStat(poller, pollId, &st)) {
    printf("Current size: %ld bytes\n", st.st_size);
}

// Get path being monitored
const char *path = loopyFSPollGetPath(poller, pollId);

// Get event name
const char *name = loopyFSPollEventName(LOOPY_FSPOLL_MODIFIED);
```

---

## File Locking

### Overview

`loopyFlock` provides cross-platform file locking supporting both BSD-style (`flock()`) whole-file locks and POSIX-style (`fcntl()`) byte-range locks.

### Lock Types

```c
LOOPY_FLOCK_SHARED      // Multiple readers, conflicts with exclusive
LOOPY_FLOCK_EXCLUSIVE   // Single writer, conflicts with all
```

### Lock Modes

```c
LOOPY_FLOCK_BLOCKING    // Wait until lock acquired
LOOPY_FLOCK_NONBLOCKING // Fail immediately if unavailable
```

### Lock Mechanisms

```c
LOOPY_FLOCK_AUTO        // Platform default (flock on BSD/Linux, fcntl elsewhere)
LOOPY_FLOCK_FLOCK       // Use flock() - whole file only
LOOPY_FLOCK_FCNTL       // Use fcntl() - supports byte ranges
```

### Capability Detection

```c
// Check platform support
bool hasFlock = loopyFlockHasFlock();    // true on Linux, macOS, BSD
bool hasFcntl = loopyFlockHasFcntl();    // true on POSIX systems
```

### BSD-Style Whole-File Locks (flock)

#### Basic Operations

```c
// Acquire shared lock
int fd = open("data.txt", O_RDWR);
if (loopyFlockLock(fd, LOOPY_FLOCK_SHARED, LOOPY_FLOCK_BLOCKING)) {
    printf("Shared lock acquired\n");
    // Multiple processes can hold shared locks
}

// Acquire exclusive lock
if (loopyFlockLock(fd, LOOPY_FLOCK_EXCLUSIVE, LOOPY_FLOCK_BLOCKING)) {
    printf("Exclusive lock acquired\n");
    // Only one process can hold exclusive lock
}

// Release lock
loopyFlockUnlock(fd);
close(fd);

// Non-blocking attempt
if (loopyFlockLock(fd, LOOPY_FLOCK_EXCLUSIVE, LOOPY_FLOCK_NONBLOCKING)) {
    printf("Lock acquired immediately\n");
} else {
    printf("Lock unavailable (EAGAIN)\n");
}
```

#### Lock Upgrades/Downgrades

```c
// Acquire shared lock first
loopyFlockLock(fd, LOOPY_FLOCK_SHARED, LOOPY_FLOCK_BLOCKING);

// Upgrade to exclusive (atomic operation)
if (loopyFlockUpgrade(fd, LOOPY_FLOCK_BLOCKING)) {
    printf("Now have exclusive lock\n");
}

// Downgrade back to shared
loopyFlockDowngrade(fd);
```

### POSIX Byte-Range Locks (fcntl)

Byte-range locks allow locking portions of files:

```c
// Lock first 4KB
int fd = open("data.bin", O_RDWR);
if (loopyFlockLockRange(fd, LOOPY_FLOCK_EXCLUSIVE, LOOPY_FLOCK_BLOCKING,
                        0, 4096)) {
    printf("First 4KB locked\n");
}

// Lock entire file (grows with file)
if (loopyFlockLockRange(fd, LOOPY_FLOCK_EXCLUSIVE, LOOPY_FLOCK_BLOCKING,
                        0, 0)) {
    printf("Entire file locked\n");
}

// Unlock specific range
loopyFlockUnlockRange(fd, 0, 4096);

// Release all locks
loopyFlockUnlockRange(fd, 0, 0);
```

#### Test for Locks

Query without locking:

```c
pid_t holder;
// Check if we can lock the range
if (loopyFlockTestRange(fd, LOOPY_FLOCK_EXCLUSIVE, 0, 4096, &holder)) {
    printf("Range is free\n");
} else {
    printf("Range is locked by PID %d\n", holder);
}
```

### High-Level Unified API

The `loopyFlock` structure provides a unified interface:

```c
loopyFlockConfig cfg = LOOPY_FLOCK_CONFIG_DEFAULT;
cfg.type = LOOPY_FLOCK_EXCLUSIVE;
cfg.mode = LOOPY_FLOCK_NONBLOCKING;

int fd = open("data.txt", O_RDWR);
loopyFlock *lock = loopyFlockNew(fd, &cfg);

if (lock) {
    printf("Lock acquired\n");

    // Query lock properties
    loopyFlockType type = loopyFlockGetType(lock);
    loopyFlockMechanism mech = loopyFlockGetMechanism(lock);
    bool isLocked = loopyFlockIsLocked(lock);

    // Release lock
    loopyFlockFree(lock);
}
close(fd);
```

### Lock File Pattern

Single-instance enforcement:

```c
// Create/acquire lock file
loopyFlock *lock = loopyFlockNewLockfile("/var/run/myapp.lock",
                                         LOOPY_FLOCK_NONBLOCKING);

if (!lock) {
    fprintf(stderr, "Another instance is running\n");
    exit(1);
}

printf("Lock acquired, running application...\n");
// ... application runs ...

// Release and remove lock file
loopyFlockFreeLockfile(lock);
```

### Error Handling

```c
const char *err = loopyFlockGetError();
if (err && err[0]) {
    printf("Lock error: %s\n", err);
}
```

---

## Memory Mapping

### Overview

`loopyMmap` provides zero-copy file access through memory mapping. Files are mapped directly into process memory, enabling efficient I/O without read/write syscalls.

### Protection Flags

```c
LOOPY_MMAP_PROT_NONE   // No access
LOOPY_MMAP_PROT_READ   // Read access
LOOPY_MMAP_PROT_WRITE  // Write access
LOOPY_MMAP_PROT_EXEC   // Execute access
```

### Mapping Flags

```c
LOOPY_MMAP_SHARED      // Changes visible to other processes
LOOPY_MMAP_PRIVATE     // Copy-on-write (private copy)
LOOPY_MMAP_FIXED       // Map at exact address (dangerous)
LOOPY_MMAP_ANONYMOUS   // No file backing (like malloc)
LOOPY_MMAP_POPULATE    // Prefault all pages (Linux)
LOOPY_MMAP_HUGETLB     // Use huge pages 2MB/1GB (Linux)
```

### Synchronization Flags

```c
LOOPY_MMAP_SYNC_ASYNC      // Schedule flush, return immediately
LOOPY_MMAP_SYNC_SYNC       // Wait for flush to complete
LOOPY_MMAP_SYNC_INVALIDATE // Invalidate cached pages
```

### Advice Hints

```c
LOOPY_MMAP_ADVICE_NORMAL       // Default balanced behavior
LOOPY_MMAP_ADVICE_RANDOM       // Random access pattern
LOOPY_MMAP_ADVICE_SEQUENTIAL   // Sequential access (aggressive read-ahead)
LOOPY_MMAP_ADVICE_WILLNEED     // Prefetch pages into memory
LOOPY_MMAP_ADVICE_DONTNEED     // Drop pages from cache
```

### File Mapping

#### Basic File Mapping

```c
#include "loopyMmap.h"

// Open file
int fd = open("large_file.dat", O_RDONLY);

// Map entire file (read-only)
loopyMmap *mmap = loopyMmapFile(fd, 0, 0,
                                LOOPY_MMAP_PROT_READ,
                                LOOPY_MMAP_PRIVATE);

if (loopyMmapIsValid(mmap)) {
    // Access mapped data
    const char *data = loopyMmapAddress(mmap);
    size_t size = loopyMmapSize(mmap);

    // Process data...
    printf("File size: %zu bytes\n", size);
}

// Cleanup
loopyMmapUnmap(mmap);
close(fd);
```

#### Partial File Mapping

```c
// Map specific region (must be page-aligned)
size_t offset = loopyMmapPageSize() * 10;  // Start at page 10
size_t length = loopyMmapPageSize() * 100; // Map 100 pages

loopyMmap *mmap = loopyMmapFile(fd, offset, length,
                                LOOPY_MMAP_PROT_READ,
                                LOOPY_MMAP_PRIVATE);
```

#### Read-Write Mapping

```c
// Map for reading and writing
loopyMmap *mmap = loopyMmapFile(fd, 0, 0,
                                LOOPY_MMAP_PROT_READ | LOOPY_MMAP_PROT_WRITE,
                                LOOPY_MMAP_SHARED);  // Changes go to file

// Modify data
char *data = loopyMmapAddress(mmap);
memset(data, 0, loopyMmapSize(mmap));

// Flush changes to disk
loopyMmapSync(mmap, 0, 0, LOOPY_MMAP_SYNC_SYNC);
```

### Anonymous Mapping

Anonymous mappings work like `malloc()` but using `mmap()`:

```c
// Allocate 100MB
size_t size = 100 * 1024 * 1024;
loopyMmap *mmap = loopyMmapAnon(size,
                               LOOPY_MMAP_PROT_READ | LOOPY_MMAP_PROT_WRITE,
                               LOOPY_MMAP_PRIVATE | LOOPY_MMAP_ANONYMOUS);

// Use like malloc
char *buffer = loopyMmapAddress(mmap);
memset(buffer, 0, size);

// Cleanup
loopyMmapUnmap(mmap);
```

### Memory Optimization

#### Prefaulting

Prefault pages to avoid page faults during critical operations:

```c
// Create mapping with prefault
loopyMmap *mmap = loopyMmapFile(fd, 0, 0,
                                LOOPY_MMAP_PROT_READ,
                                LOOPY_MMAP_PRIVATE | LOOPY_MMAP_POPULATE);
// All pages loaded into memory immediately
```

#### Access Hints

```c
// Sequential file scan
loopyMmapAdvise(mmap, 0, 0, LOOPY_MMAP_ADVICE_SEQUENTIAL);

// Random access (disable read-ahead)
loopyMmapAdvise(mmap, 0, 0, LOOPY_MMAP_ADVICE_RANDOM);

// Prefetch upcoming data
loopyMmapAdvise(mmap, 0, loopyMmapSize(mmap), LOOPY_MMAP_ADVICE_WILLNEED);

// Free cached pages
loopyMmapAdvise(mmap, 0, loopyMmapSize(mmap), LOOPY_MMAP_ADVICE_DONTNEED);
```

### Data Synchronization

```c
// Asynchronous flush (schedule and return)
loopyMmapSync(mmap, 0, 0, LOOPY_MMAP_SYNC_ASYNC);

// Synchronous flush (wait for completion)
loopyMmapSync(mmap, 0, 0, LOOPY_MMAP_SYNC_SYNC);

// Flush specific range
loopyMmapSync(mmap, 4096, 8192, LOOPY_MMAP_SYNC_SYNC);

// Flush and invalidate cache
loopyMmapSync(mmap, 0, 0, LOOPY_MMAP_SYNC_SYNC | LOOPY_MMAP_SYNC_INVALIDATE);
```

### Memory Locking

Lock pages in RAM (requires CAP_IPC_LOCK):

```c
// Lock entire mapping
loopyMmapLock(mmap, 0, 0);

// Lock specific range
loopyMmapLock(mmap, 0, 4096);

// Unlock
loopyMmapUnlock(mmap, 0, 0);
```

### Utility Functions

```c
// Get system page size
size_t pageSize = loopyMmapPageSize();  // Typically 4096

// Align size to page boundary
size_t aligned = loopyMmapAlignSize(12345);  // Returns 16384

// Check validity
if (loopyMmapIsValid(mmap)) {
    void *addr = loopyMmapAddress(mmap);
    size_t size = loopyMmapSize(mmap);
}
```

---

## Process Management

### Overview

`loopyProcess` enables spawning and managing child processes with stdio redirection, signal handling, and event loop integration.

### Process Options

```c
loopyProcessOptions opts = {
    .file = "/bin/echo",
    .args = (char *[]){ "echo", "Hello", "World", NULL },
    .env = NULL,           // NULL = inherit parent env
    .cwd = "/tmp",         // Working directory
    .stdio = {
        [0] = { .flags = LOOPY_STDIO_CREATE_PIPE },  // stdin - create pipe
        [1] = { .flags = LOOPY_STDIO_CREATE_PIPE },  // stdout - create pipe
        [2] = { .flags = LOOPY_STDIO_INHERIT }       // stderr - inherit
    },
    .flags = 0,
    .uid = -1,
    .gid = -1,
};
```

### Stdio Flags

```c
LOOPY_STDIO_IGNORE       // Redirect to /dev/null
LOOPY_STDIO_CREATE_PIPE  // Create a pipe (parent can read/write)
LOOPY_STDIO_INHERIT_FD   // Inherit specific fd
LOOPY_STDIO_INHERIT      // Inherit from parent
```

### Process Spawning

```c
void onProcessExit(loopyLoop *loop, loopyProcess *proc,
                   int64_t exitStatus, int termSignal, void *userData) {
    if (termSignal) {
        printf("Process killed by signal %d\n", termSignal);
    } else {
        printf("Process exited with status %ld\n", exitStatus);
    }
    loopyProcessFree(proc);
}

// Spawn process
loopyProcessOptions opts = {
    .file = "/bin/ls",
    .args = (char *[]){ "ls", "-la", "/tmp", NULL },
    .stdio = {
        [0] = { .flags = LOOPY_STDIO_IGNORE },       // stdin -> /dev/null
        [1] = { .flags = LOOPY_STDIO_CREATE_PIPE },  // stdout -> pipe
        [2] = { .flags = LOOPY_STDIO_INHERIT }       // stderr -> inherit
    }
};

loopyProcess *proc = loopyProcessSpawn(loop, &opts, onProcessExit, NULL);
if (proc) {
    printf("Process spawned with PID %d\n", loopyProcessGetPid(proc));
}
```

### Reading Process Output

```c
void onProcessExit(loopyLoop *loop, loopyProcess *proc,
                   int64_t exitStatus, int termSignal, void *userData) {
    // Read output from process
    int stdoutFd = loopyProcessGetStdioPipe(proc, 1);
    if (stdoutFd >= 0) {
        // Use loopyFS or standard read() to get output
        char buf[1024];
        ssize_t n = read(stdoutFd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            printf("Process output:\n%s\n", buf);
        }
        close(stdoutFd);
    }
    loopyProcessFree(proc);
}
```

### Writing to Process Input

```c
// After spawning with stdin pipe
loopyProcess *proc = loopyProcessSpawn(loop, &opts, onExit, NULL);

// Write to process stdin
int stdinFd = loopyProcessGetStdioPipe(proc, 0);
if (stdinFd >= 0) {
    const char *input = "Hello process\n";
    write(stdinFd, input, strlen(input));
    close(stdinFd);  // Signal EOF to process
}
```

### Process Control

```c
// Get process information
pid_t pid = loopyProcessGetPid(proc);
bool exited = loopyProcessExited(proc);
const char *error = loopyProcessGetError(proc);

// Send signal to process
loopyProcessKill(proc, SIGTERM);

// Wait for graceful shutdown
sleep(1);
if (!loopyProcessExited(proc)) {
    // Force kill if still running
    loopyProcessKill(proc, SIGKILL);
}

// Get event loop
loopyLoop *loop = loopyProcessGetLoop(proc);

// User data
void *data = loopyProcessGetData(proc);
loopyProcessSetData(proc, myData);
```

### Environment and Working Directory

```c
// Custom environment
char *env[] = {
    "PATH=/bin:/usr/bin",
    "HOME=/home/user",
    "VAR=value",
    NULL
};

loopyProcessOptions opts = {
    .file = "/bin/sh",
    .args = (char *[]){ "sh", "-c", "echo $VAR", NULL },
    .env = env,
    .cwd = "/tmp"
};

loopyProcess *proc = loopyProcessSpawn(loop, &opts, onExit, NULL);
```

### Privilege Changes

```c
loopyProcessOptions opts = {
    .file = "/bin/whoami",
    .args = (char *[]){ "whoami", NULL },
    .flags = LOOPY_PROCESS_SETUID | LOOPY_PROCESS_SETGID,
    .uid = 1000,  // Run as UID 1000
    .gid = 1000   // Run as GID 1000
};
```

---

## Channels and Pub/Sub

### Channels - Message Passing

`loopyChannel` provides high-performance inter-thread communication with multiple threading models.

#### Channel Types

```c
LOOPY_CHANNEL_SPSC    // Single-producer/consumer - lock-free
LOOPY_CHANNEL_MPSC    // Multi-producer/single-consumer
LOOPY_CHANNEL_MPMC    // Multi-producer/multi-consumer
```

#### Configuration

```c
loopyChannelConfig cfg;
loopyChannelConfigInit(&cfg);

cfg.type = LOOPY_CHANNEL_MPSC;
cfg.elementSize = sizeof(struct Message);
cfg.capacity = 256;
cfg.blocking = false;
cfg.sendTimeoutUs = 0;     // Infinite when blocking
cfg.recvTimeoutUs = 0;     // Infinite when blocking

loopyChannel *ch = loopyChannelNew(loop, &cfg);
```

#### Synchronous Operations

```c
struct Message {
    int type;
    int value;
};

// Send message
struct Message msg = { .type = 1, .value = 42 };
loopyChannelStatus status = loopyChannelSend(ch, &msg, sizeof(msg));

if (status == LOOPY_CHANNEL_OK) {
    printf("Message sent\n");
} else if (status == LOOPY_CHANNEL_FULL) {
    printf("Channel full\n");
} else if (status == LOOPY_CHANNEL_CLOSED) {
    printf("Channel closed\n");
}

// Non-blocking send
status = loopyChannelTrySend(ch, &msg, sizeof(msg));

// Receive message
struct Message rxMsg;
ssize_t n = loopyChannelRecv(ch, &rxMsg, sizeof(rxMsg));

if (n > 0) {
    printf("Received: type=%d value=%d\n", rxMsg.type, rxMsg.value);
}

// Non-blocking receive
n = loopyChannelTryRecv(ch, &rxMsg, sizeof(rxMsg));
```

#### Asynchronous Operations

```c
void onSendComplete(loopyChannel *ch, loopyChannelStatus status, void *userData) {
    if (status == LOOPY_CHANNEL_OK) {
        printf("Send completed\n");
    }
}

void onRecvComplete(loopyChannel *ch, const void *data, size_t len,
                    loopyChannelStatus status, void *userData) {
    if (status == LOOPY_CHANNEL_OK) {
        struct Message *msg = (struct Message *)data;
        printf("Received: type=%d value=%d\n", msg->type, msg->value);
    }
}

// Async send
loopyChannelSendAsync(ch, &msg, sizeof(msg), onSendComplete, NULL);

// Async receive
loopyChannelRecvAsync(ch, onRecvComplete, NULL);
```

#### Select (Multiple Channels)

Wait for any of multiple channels to be ready:

```c
loopyChannelCase cases[2];

struct Message msg = { .type = 1, .value = 42 };
cases[0].ch = inputCh;
cases[0].send = false;  // Receive operation
cases[0].data = &msg;
cases[0].len = sizeof(msg);

cases[1].ch = outputCh;
cases[1].send = true;   // Send operation
cases[1].data = &msg;
cases[1].len = sizeof(msg);

// Wait 1 second for any channel to be ready
int ready = loopyChannelSelect(cases, 2, 1000000);

if (ready >= 0) {
    printf("Channel %d is ready\n", ready);
    printf("Result: %ld\n", cases[ready].result);
}
```

#### Channel Status

```c
// Query state
size_t len = loopyChannelLen(ch);           // Elements in channel
size_t cap = loopyChannelCap(ch);           // Capacity
bool full = loopyChannelIsFull(ch);         // Channel full?
bool empty = loopyChannelIsEmpty(ch);       // Channel empty?
bool closed = loopyChannelIsClosed(ch);     // Channel closed?

// Close channel (no more sends allowed)
loopyChannelClose(ch);

// Cleanup
loopyChannelFree(ch);
```

#### Statistics

```c
loopyChannelStats stats;
loopyChannelGetStats(ch, &stats);

printf("Sent: %llu, Received: %llu\n",
       stats.totalSent, stats.totalReceived);
printf("Send blocks: %llu, Recv blocks: %llu\n",
       stats.sendBlocked, stats.recvBlocked);
printf("Peak usage: %llu elements\n", stats.peakUsage);

loopyChannelResetStats(ch);
```

### Pub/Sub - Topic-Based Messaging

`loopyPubSub` provides pattern-based publish/subscribe with wildcard matching.

#### Wildcard Patterns

```
'*'   matches exactly one segment (between separators)
'#'   matches zero or more segments

Examples:
  "stock.*.price"     matches "stock.AAPL.price", "stock.GOOG.price"
  "stock.#"           matches "stock", "stock.AAPL", "stock.AAPL.price"
  "*.weather.#"       matches "us.weather", "uk.weather.london.rain"
```

#### Configuration

```c
loopyPubSubConfig config;
loopyPubSubConfigInit(&config);

config.separator = '.';        // Topic segment separator
config.starWildcard = '*';     // Single-segment wildcard
config.hashWildcard = '#';     // Multi-segment wildcard
config.maxSubscriptions = 1000;
config.enableStats = true;

loopyPubSub *ps = loopyPubSubNew(loop, &config);
```

#### Subscription

```c
bool onMessage(loopySubscription *sub, const loopyMessage *msg, void *userData) {
    printf("Received on %s: %.*s (seq: %lu)\n",
           msg->topic, (int)msg->len, (char *)msg->data, msg->sequence);
    return true;  // Acknowledge message (if manual ack mode)
}

loopySubscriptionConfig subCfg;
loopySubscriptionConfigInit(&subCfg);

subCfg.deliveryMode = LOOPY_DELIVER_ASYNC;
subCfg.ackMode = LOOPY_ACK_AUTO;
subCfg.queueSize = 100;
subCfg.dropOnFull = false;  // Block publisher if queue full

// Subscribe to pattern
loopySubscription *sub = loopySubscribe(ps, "stock.*.price",
                                        onMessage, &subCfg);

// Get subscription info
const char *pattern = loopySubscriptionPattern(sub);
size_t pending = loopySubscriptionPending(sub);

// Unsubscribe
loopyUnsubscribe(sub);
```

#### Publishing

```c
// Simple publish
const char *msg = "123.45";
size_t delivered = loopyPublish(ps, "stock.AAPL.price", msg, strlen(msg));
printf("Message delivered to %zu subscribers\n", delivered);

// Publish with metadata
size_t delivered = loopyPublishEx(ps, "stock.AAPL.price",
                                  msg, strlen(msg), (void *)1);

// Async publish with completion callback
void onPublishDone(size_t delivered, void *userData) {
    printf("Async publish delivered to %zu subscribers\n", delivered);
}

loopyPublishAsync(ps, "stock.AAPL.price", msg, strlen(msg),
                  onPublishDone, NULL);
```

#### Delivery Modes

```c
LOOPY_DELIVER_SYNC   // Synchronous callback (may block)
LOOPY_DELIVER_ASYNC  // Async via event loop
LOOPY_DELIVER_QUEUE  // Queue for manual retrieval

// Queue-based delivery
void onMessage(loopySubscription *sub, const loopyMessage *msg, void *userData) {
    // Not called in QUEUE mode
}

loopySubscriptionConfig cfg;
loopySubscriptionConfigInit(&cfg);
cfg.deliveryMode = LOOPY_DELIVER_QUEUE;

loopySubscription *sub = loopySubscribe(ps, "events.#", onMessage, &cfg);

// Manually receive messages
loopyMessage msg;
while (loopySubscriptionReceive(sub, &msg)) {
    printf("Message: %s\n", msg.topic);
}
```

#### Acknowledgment Modes

```c
LOOPY_ACK_NONE    // No ack needed
LOOPY_ACK_AUTO    // Auto-ack after callback returns
LOOPY_ACK_MANUAL  // Callback must explicitly ack

// Manual acknowledgment
bool onMessage(loopySubscription *sub, const loopyMessage *msg, void *userData) {
    printf("Processing message %lu\n", msg->sequence);
    // Do async processing...
    // Later: loopySubscriptionAck(sub, msg->sequence);
    return false;  // Don't auto-ack
}

loopySubscriptionConfig cfg;
loopySubscriptionConfigInit(&cfg);
cfg.ackMode = LOOPY_ACK_MANUAL;
loopySubscription *sub = loopySubscribe(ps, "tasks.#", onMessage, &cfg);

// After processing
loopySubscriptionAck(sub, sequence);

// Or request redelivery
loopySubscriptionNack(sub, sequence);
```

#### Querying

```c
// Check for subscribers
bool hasSubscribers = loopyHasSubscribers(ps, "stock.*");

// Count subscribers matching pattern
size_t count = loopyCountSubscribers(ps, "stock.*");

// Count subscribers that would match topic
size_t matches = loopyMatchCount(ps, "stock.AAPL.price");

// Iterate all subscriptions
int iter(const char *pattern, loopySubscription *sub, void *arg) {
    printf("Pattern: %s\n", pattern);
    return 0;  // Continue iterating
}

size_t visited = loopyIterateSubscriptions(ps, iter, NULL);
```

#### Statistics

```c
loopyPubSubStats stats;
loopyPubSubGetStats(ps, &stats);

printf("Published: %llu, Delivered: %llu\n",
       stats.messagesPublished, stats.messagesDelivered);
printf("Dropped: %llu, Subscriptions: %zu\n",
       stats.messagesDropped, stats.subscriptionCount);

loopyPubSubResetStats(ps);
```

#### Validation

```c
// Validate topic (no wildcards)
if (loopyValidateTopic(ps, "stock.AAPL.price")) {
    printf("Valid topic\n");
}

// Validate pattern (wildcards allowed)
if (loopyValidatePattern(ps, "stock.*.price")) {
    printf("Valid pattern\n");
}
```

---

## Rate Limiting

### Overview

`loopyRateLimit` provides multiple rate limiting algorithms for controlling request rates and enforcing quotas.

### Algorithms

```c
LOOPY_RATE_LIMIT_TOKEN_BUCKET    // Token bucket with burst
LOOPY_RATE_LIMIT_SLIDING_WINDOW  // Sliding window log
LOOPY_RATE_LIMIT_LEAKY_BUCKET    // Leaky bucket queue
LOOPY_RATE_LIMIT_FIXED_WINDOW    // Fixed time window
```

### Token Bucket

Smooth rate limiting with burst capacity:

```c
loopyTokenBucketConfig cfg;
loopyTokenBucketConfigInit(&cfg, 1000.0, 100.0);  // 1000 req/s, burst 100

loopyRateLimiterConfig limiterCfg;
limiterCfg.algorithm = LOOPY_RATE_LIMIT_TOKEN_BUCKET;
limiterCfg.params.tokenBucket = cfg;

loopyRateLimiter *limiter = loopyRateLimiterNew(loop, &limiterCfg);

// Check and consume
loopyRateLimitResult result = loopyRateLimitCheck(limiter, 1.0);

if (result == LOOPY_RATE_LIMIT_OK) {
    printf("Request allowed\n");
} else {
    printf("Request denied\n");
}
```

### Sliding Window

Precise per-window request counting:

```c
loopySlidingWindowConfig cfg;
loopySlidingWindowConfigInit(&cfg, 60000, 100);  // 100 req/min

loopyRateLimiterConfig limiterCfg;
limiterCfg.algorithm = LOOPY_RATE_LIMIT_SLIDING_WINDOW;
limiterCfg.params.slidingWindow = cfg;

loopyRateLimiter *limiter = loopyRateLimiterNew(loop, &limiterCfg);
```

### Leaky Bucket

Constant output rate smoothing:

```c
loopyLeakyBucketConfig cfg;
loopyLeakyBucketConfigInit(&cfg, 100.0, 1000);  // 100 req/s, queue 1000

loopyRateLimiterConfig limiterCfg;
limiterCfg.algorithm = LOOPY_RATE_LIMIT_LEAKY_BUCKET;
limiterCfg.params.leakyBucket = cfg;

loopyRateLimiter *limiter = loopyRateLimiterNew(loop, &limiterCfg);

// Async check (queues if not ready)
void onCheckComplete(loopyRateLimiter *limiter, loopyRateLimitResult result,
                     const loopyRateLimitInfo *info, void *userData) {
    if (result == LOOPY_RATE_LIMIT_OK) {
        printf("Request allowed\n");
    }
}

loopyRateLimitCheckAsync(limiter, 1.0, onCheckComplete, NULL);
```

### Fixed Window

Simple time-based counting:

```c
loopyFixedWindowConfig cfg;
loopyFixedWindowConfigInit(&cfg, 60000, 100);  // 100 req/min

loopyRateLimiterConfig limiterCfg;
limiterCfg.algorithm = LOOPY_RATE_LIMIT_FIXED_WINDOW;
limiterCfg.params.fixedWindow = cfg;

loopyRateLimiter *limiter = loopyRateLimiterNew(loop, &limiterCfg);
```

### Per-Key Rate Limiting

Limit different clients separately:

```c
// Check global limit
loopyRateLimitResult result = loopyRateLimitCheck(limiter, 1.0);

// Check per-client limit (IP or API key)
result = loopyRateLimitCheckKey(limiter, "192.168.1.1", 11, 1.0);

// Get info without consuming
loopyRateLimitInfo info;
loopyRateLimitPeek(limiter, &info);

loopyRateLimitPeekKey(limiter, "192.168.1.1", 11, &info);

// Reset limit
loopyRateLimitReset(limiter, "192.168.1.1", 11);
loopyRateLimitReset(limiter, NULL, 0);  // Reset global
```

### Statistics

```c
loopyRateLimitStats stats;
loopyRateLimitGetStats(limiter, &stats);

printf("Allowed: %llu, Denied: %llu\n",
       stats.allowedRequests, stats.deniedRequests);
printf("Peak rate: %llu req/s\n", stats.peakRate);

loopyRateLimitResetStats(limiter);
```

---

## Concurrency Management

### Concurrency Limiter

Limit concurrent operations:

```c
loopyConcurrencyConfig cfg;
loopyConcurrencyConfigInit(&cfg, 10);  // 10 concurrent max

loopyConcurrencyLimiter *limiter = loopyConcurrencyLimiterNew(loop, &cfg);

// Try to acquire slot
if (loopyConcurrencyTryAcquire(limiter)) {
    printf("Slot acquired\n");
    // Do work...
    loopyConcurrencyRelease(limiter);
}

// Block until slot available
if (loopyConcurrencyAcquire(limiter, 5000)) {  // 5 second timeout
    // Do work...
    loopyConcurrencyRelease(limiter);
}
```

### Concurrency Pool

Multi-tenant concurrency management for different users/keys:

```c
loopyConcurrencyPoolConfig cfg;
loopyConcurrencyPoolConfigInit(&cfg);

cfg.globalLimit = 100;           // Max 100 concurrent globally
cfg.defaultUserLimit = 10;       // 10 per user by default
cfg.fairScheduling = true;
cfg.autoCreateUsers = true;

loopyConcurrencyPool *pool = loopyConcurrencyPoolNew(&cfg);

// User 1 tries to acquire 5 slots
loopyConcurrencyResult res = loopyConcurrencyPoolTryAcquire(pool, "user1", 5);

if (res == LOOPY_CONCURRENCY_OK) {
    printf("Acquired 5 slots for user1\n");
    // Do work...
    loopyConcurrencyPoolRelease(pool, "user1", 5);
}

// Per-user configuration
loopyConcurrencyUserConfig userCfg;
loopyConcurrencyUserConfigInit(&userCfg);

userCfg.limit = 20;      // User2 gets 20 slots
userCfg.reserved = 5;    // 5 guaranteed slots
userCfg.priority = 100;  // Higher priority

loopyConcurrencyUser *user2 = loopyConcurrencyPoolAddUser(pool, "user2", &userCfg);
```

#### Query Pool State

```c
// Global state
size_t available = loopyConcurrencyPoolAvailable(pool);
size_t active = loopyConcurrencyPoolActive(pool);

// User state
available = loopyConcurrencyAvailable(pool, "user1");
active = loopyConcurrencyActive(pool, "user1");

// Using user handle
loopyConcurrencyUser *user = loopyConcurrencyPoolGetUser(pool, "user1");
available = loopyConcurrencyUserAvailable(user);
active = loopyConcurrencyUserActive(user);
```

#### Statistics

```c
loopyConcurrencyPoolStats stats;
loopyConcurrencyPoolGetStats(pool, &stats);

printf("Global active: %zu / %zu\n", stats.globalActive, stats.globalLimit);
printf("Users: %zu\n", stats.userCount);
printf("Total acquisitions: %llu\n", stats.totalAcquires);

loopyConcurrencyUserStats userStats;
loopyConcurrencyUserGetStats(user, &userStats);

printf("User active: %zu / %zu\n", userStats.active, userStats.limit);
```

---

## Metrics Collection

### Overview

`loopyMetrics` provides comprehensive performance monitoring for event loops.

### Enabling Metrics

```c
// Enable collection
bool ok = loopyMetricsEnable(loop);

// Later, disable to free resources
loopyMetricsDisable(loop);

// Check status
if (loopyMetricsEnabled(loop)) {
    printf("Metrics enabled\n");
}
```

### Getting Metrics

```c
loopyMetrics metrics;

// Get snapshot (thread-safe)
if (loopyMetricsGet(loop, &metrics)) {
    printf("Iterations: %llu\n", metrics.loopIterations);
    printf("Events: %llu\n", metrics.eventsProcessed);
    printf("Timers: %llu\n", metrics.timersProcessed);
    printf("FDs: %u (peak: %u)\n", metrics.currentFdCount, metrics.peakFdCount);
    printf("Timers: %u (peak: %u)\n", metrics.currentTimerCount, metrics.peakTimerCount);
    printf("Idle: %.1f%%\n", loopyMetricsGetIdleRatio(&metrics) * 100);
    printf("Events/sec: %.1f\n", loopyMetricsGetEventsPerSecond(&metrics));
}
```

### Individual Metrics

```c
// Fast lock-free accessors
uint64_t iterations = loopyMetricsGetIterations(loop);
uint64_t events = loopyMetricsGetEventsProcessed(loop);
uint64_t timers = loopyMetricsGetTimersProcessed(loop);
uint32_t fdCount = loopyMetricsGetFdCount(loop);
uint32_t timerCount = loopyMetricsGetTimerCount(loop);
uint64_t uptimeUs = loopyMetricsGetUptimeUs(loop);
uint64_t pollTimeUs = loopyMetricsPollTime(loop);
uint64_t idleTimeUs = loopyMetricsIdleTime(loop);
```

### Computed Metrics

```c
// Computed from raw metrics
double idleRatio = loopyMetricsGetIdleRatio(loop);           // 0.0-1.0
double eventsPerIter = loopyMetricsGetEventsPerIteration(loop);
double avgPollUs = loopyMetricsGetAvgPollTimeUs(loop);
double eventsPerSec = loopyMetricsGetEventsPerSecond(loop);
```

### Latency Histogram

```c
loopyMetrics metrics;
loopyMetricsGet(loop, &metrics);

// Latency distribution (event processing time)
for (int i = 0; i < LOOPY_LATENCY_BUCKET_COUNT; i++) {
    const char *label = loopyMetricsLatencyBucketName(i);
    uint64_t count = metrics.latencyBuckets[i];
    printf("%s: %llu\n", label, count);
}

// Bucket names:
// LOOPY_LATENCY_UNDER_1US
// LOOPY_LATENCY_UNDER_10US
// LOOPY_LATENCY_UNDER_100US
// LOOPY_LATENCY_UNDER_1MS
// LOOPY_LATENCY_UNDER_10MS
// LOOPY_LATENCY_UNDER_100MS
// LOOPY_LATENCY_UNDER_1S
// LOOPY_LATENCY_OVER_1S
```

### Formatted Output

```c
loopyMetrics metrics;
loopyMetricsGet(loop, &metrics);

// Format to string
char buf[512];
size_t len = loopyMetricsFormat(&metrics, buf, sizeof(buf));
printf("%s\n", buf);

// Example output:
// Uptime: 1h 23m 45s
// Iterations: 1,234,567 | Events: 9,876,543 | Timers: 123,456
// Poll: 1,234,567 calls (99.1% events, 0.9% timeouts)
// FDs: 42 current / 128 peak | Timers: 5 current / 32 peak
// Idle: 87.3% | Avg poll: 1.23ms
```

### Reset Metrics

```c
// Clear accumulated metrics (keeps collection enabled)
loopyMetricsReset(loop);
```

---

## Thread Pools

### Overview

`loopyWork` offloads blocking or CPU-intensive work to background threads without blocking the event loop.

### Configuration

```c
loopyWorkConfig cfg;
loopyWorkConfigInit(&cfg);

cfg.minThreads = 2;      // Min 2 worker threads
cfg.maxThreads = 8;      // Max 8 worker threads
cfg.maxQueueSize = 256;  // Max 256 pending items (0 = unlimited)

loopyWork *work = loopyWorkNew(loop, &cfg);
```

### Work Execution

```c
void doWork(loopyWork *w, loopyWorkId id, void *userData) {
    // Runs on WORKER THREAD
    // Safe: Do CPU-intensive work
    // Unsafe: Don't access event loop or non-thread-safe loopy structures

    int *value = userData;
    printf("Worker processing value: %d\n", *value);

    // Simulate work
    sleep(1);
}

void afterWork(loopyLoop *l, loopyWork *w, loopyWorkId id,
               loopyWorkStatus status, void *userData) {
    // Runs on EVENT LOOP THREAD
    // Safe: Access event loop and loopy structures

    if (status == LOOPY_WORK_OK) {
        printf("Work completed\n");
    } else if (status == LOOPY_WORK_CANCELLED) {
        printf("Work was cancelled\n");
    }
}

// Queue work
int value = 42;
loopyWorkId id = loopyWorkQueue(work, doWork, afterWork, &value);

if (id > 0) {
    printf("Work queued with ID %lu\n", id);
}
```

### Work Management

```c
// Cancel pending work (not yet started)
if (loopyWorkCancel(work, id)) {
    printf("Work cancelled\n");
    // afterWork will be called with LOOPY_WORK_CANCELLED
}

// Cancel all pending work
loopyWorkCancelAll(work);

// Query status
size_t pending = loopyWorkPendingCount(work);  // Waiting to start
size_t running = loopyWorkRunningCount(work);  // Currently executing
size_t threads = loopyWorkThreadCount(work);   // Current worker threads

// Cleanup
loopyWorkFree(work);  // Wait for all work to complete
```

### Thread-Safe Work

```c
// Multiple threads can queue work simultaneously
void *worker(void *arg) {
    loopyWork *work = arg;

    for (int i = 0; i < 100; i++) {
        int *value = malloc(sizeof(int));
        *value = i;
        loopyWorkId id = loopyWorkQueue(work, doWork, afterWork, value);
    }

    return NULL;
}

// Start multiple producer threads
pthread_t threads[4];
for (int i = 0; i < 4; i++) {
    pthread_create(&threads[i], NULL, worker, work);
}

// Wait for producers
for (int i = 0; i < 4; i++) {
    pthread_join(threads[i], NULL);
}

// Work continues on worker threads
```

---

## Async Cross-Thread Notifications

### Overview

`loopyAsync` provides a safe mechanism to wake the event loop from other threads or signal handlers, essential for worker thread completion notifications and external event injection.

### Creating Async Handles

```c
void onAsync(loopyLoop *l, loopyAsync *async, void *data) {
    printf("Async event received!\n");
    // Safe to access loopy structures here
}

loopyAsync *async = loopyAsyncNew(loop, onAsync, NULL);

if (!async) {
    printf("Failed to create async handle\n");
}
```

### Signaling from Other Threads

```c
// From worker thread
void *worker(void *arg) {
    loopyAsync *async = arg;

    printf("Worker thread running\n");
    sleep(1);

    // Signal event loop
    loopyAsyncSend(async);

    return NULL;
}

pthread_t thread;
pthread_create(&thread, NULL, worker, async);

// Event loop will call onAsync() next iteration
```

### Signaling from Signal Handlers

```c
// Async-signal-safe!
void sigHandler(int sig) {
    loopyAsyncSend(async);  // Safe in signal handler
}

signal(SIGUSR1, sigHandler);

// Later, event loop will call onAsync()
```

### Coalescing

Multiple signals are coalesced into one callback:

```c
// Send multiple times
loopyAsyncSend(async);
loopyAsyncSend(async);
loopyAsyncSend(async);

// Result: onAsync() called ONCE, not three times
// If you need to count events, use external atomics:

#include <stdatomic.h>

_Atomic int eventCount = 0;

void sigHandler(int sig) {
    atomic_fetch_add(&eventCount, 1);
    loopyAsyncSend(async);
}

void onAsync(loopyLoop *l, loopyAsync *async, void *data) {
    int count = atomic_exchange(&eventCount, 0);
    printf("Received %d events\n", count);
}
```

### Checking Pending Notifications

```c
// Check if there are pending notifications
if (loopyAsyncPending(async)) {
    printf("There are pending notifications\n");
}
```

### Cleanup

```c
// Free handle (unregister from loop)
loopyAsyncFree(async);

// After this, signaling has no effect
```

### Backend Information

```c
// Get which backend is being used
const char *backend = loopyAsyncBackendName();

// Returns: "eventfd" on Linux, "pipe" on BSD/macOS, "none" if unsupported
printf("Using %s backend\n", backend);
```

---

## Complete Example: Building a File Monitor

This example combines multiple features:

```c
#include <stdio.h>
#include <stdlib.h>
#include "loopy.h"
#include "loopyWatch.h"
#include "loopyFS.h"
#include "loopyMetrics.h"

typedef struct {
    loopyWatch *watcher;
    loopyWork *workPool;
    loopyMetrics lastMetrics;
} AppState;

// Handle file changes
void onFileChange(loopyWatch *w, const loopyWatchInfo *info) {
    printf("File change detected: %s\n", info->path);

    if (info->events & LOOPY_WATCH_MODIFY) {
        printf("  -> File was modified\n");
    }
    if (info->events & LOOPY_WATCH_DELETE) {
        printf("  -> File was deleted\n");
    }
}

// CPU-intensive analysis work
void analyzeFile(loopyWork *w, loopyWorkId id, void *userData) {
    const char *path = userData;
    printf("  Analyzing %s on worker thread...\n", path);
    sleep(1);  // Simulate analysis
    printf("  Analysis complete\n");
}

void afterAnalysis(loopyLoop *loop, loopyWork *w, loopyWorkId id,
                   loopyWorkStatus status, void *userData) {
    if (status == LOOPY_WORK_OK) {
        printf("Analysis callback on event loop thread\n");
    }
}

// Print metrics periodically
void printMetrics(loopyLoop *loop, loopyTimer *timer, void *userData) {
    AppState *state = userData;

    loopyMetrics metrics;
    if (loopyMetricsGet(loop, &metrics)) {
        char buf[512];
        loopyMetricsFormat(&metrics, buf, sizeof(buf));
        printf("\n=== Metrics ===\n%s\n", buf);
    }
}

int main() {
    loopyLoop *loop = loopyLoopNew();
    if (!loop) {
        fprintf(stderr, "Failed to create event loop\n");
        return 1;
    }

    // Setup
    AppState state = {
        .watcher = loopyWatchNew(loop),
        .workPool = loopyWorkNew(loop, NULL)
    };

    // Enable metrics
    loopyMetricsEnable(loop);

    // Watch config directory
    loopyWatchAdd(state.watcher, "/etc/myapp",
                 LOOPY_WATCH_MODIFY | LOOPY_WATCH_CREATE | LOOPY_WATCH_DELETE,
                 onFileChange, NULL);

    // Print metrics every 5 seconds
    loopyTimerId metricsTimer = loopyTimerEvery(loop, 5000, printMetrics, &state);

    // Run event loop
    printf("Monitoring /etc/myapp/ - Press Ctrl+C to exit\n");
    loopyRun(loop);

    // Cleanup
    loopyTimerCancel(loop, metricsTimer);
    loopyWatchFree(state.watcher);
    loopyWorkFree(state.workPool);
    loopyMetricsDisable(loop);
    loopyLoopFree(loop);

    return 0;
}
```

---

## Best Practices

### File Operations

- Use async operations to avoid blocking the event loop
- Always free requests after callbacks
- Use direct I/O carefully; buffers must be properly aligned
- Handle errors by checking return values and error strings

### File Watching

- Use `loopyWatch` for normal filesystems (better performance)
- Use `loopyFSPoll` for network filesystems (better reliability)
- Don't perform heavy work in watch callbacks; queue to work pool

### Channels

- Use SPSC for single producer/consumer (highest performance)
- Use MPSC for worker results aggregation
- Use MPMC for general multi-threaded communication
- Close channels when no more sends will occur

### Pub/Sub

- Use pattern wildcards carefully to avoid too many matches
- Monitor statistics for dropped messages
- Use separate subscriptions for different event types
- Consider queue size limits for slow subscribers

### Rate Limiting

- Use token bucket for smooth limits with bursts
- Use leaky bucket for constant output rate
- Use sliding window for precise per-window limits
- Monitor statistics to detect attacks

### Metrics

- Enable metrics only when needed (small overhead but non-zero)
- Reset metrics periodically to detect changes
- Use computed metrics for better insights
- Export metrics to monitoring systems

### Thread Safety

- Never block the event loop thread on I/O or locks
- Queue work via `loopyWork` for CPU-intensive tasks
- Use channels for thread-safe inter-thread communication
- Use `loopyAsync` to wake event loop from worker threads
