# Loopy Event Loop - Architecture Guide

## Overview

Loopy is an efficient, space-time aware event loop library written in C. It implements a unified event-driven programming model that abstracts away platform-specific I/O multiplexing details while remaining optimized for high-performance applications.

The architecture is built on a **pluggable adapter pattern** that selects the best available I/O multiplexing mechanism at compile-time or runtime:

- **Linux**: io_uring (5.1+) with epoll fallback
- **macOS/FreeBSD**: kqueue
- **Solaris**: event ports
- **Fallback**: POSIX select()

---

## Table of Contents

1. [Module Hierarchy](#module-hierarchy)
2. [Adapter Pattern](#adapter-pattern)
3. [Event Loop Model](#event-loop-model)
4. [Handle Types](#handle-types)
5. [Memory Management](#memory-management)
6. [Threading Model](#threading-model)
7. [Core Structures](#core-structures)

---

## Module Hierarchy

The loopy library is organized as a layered architecture where higher-level modules build on lower-level primitives:

### Core Layer (loopy.c / loopy.h)

The foundation of the entire library:

- **loopyLoop**: Opaque main event loop handle
- **File Events**: Direct fd I/O registration (`loopyRegisterRead`, `loopyRegisterWrite`)
- **Timers**: Registration via integrated timer wheel (`loopyRegisterTimer`)
- **Main Loops**: `loopyMain()` (all events) and `loopyMainFdOnly()` (file events only)

The core layer manages:

- FD storage with automatic resizing (power-of-2 growth)
- Max-FD tracking via embedded max-heap for O(1) bounds checking
- Event coalescing and firing
- Before/after sleep callbacks for custom hooks

**Key Data Structures:**

- `loopyFileEvent`: Stores read/write callbacks and masks for each fd
- `loopyFiredEvent`: Temporary array of events fired in current poll cycle
- `loopyMaxHeap`: Embedded heap for O(1) max-fd lookup

### I/O Adapters (Pluggable Backends)

Each adapter implements the same interface and is compiled in as the sole event mechanism:

```
loopy.c (core)
├── loopyAdapterLinux.c    (epoll with io_uring detection)
├── loopyAdapterIouring.c  (io_uring with epoll fallback)
├── loopyAdapterBSD.c      (kqueue)
├── loopyAdapterSolaris.c  (event ports)
└── loopyAdapterGeneric.c  (select fallback)
```

Each adapter defines:

- `loopyInternalNew()`: Create adapter-specific state
- `loopyInternalResize()`: Handle set size changes
- `loopyInternalFree()`: Clean up state
- `loopyInternalAddEvent()`: Register fd with OS mechanism
- `loopyInternalDelEvent()`: Unregister fd with OS mechanism
- `loopyInternalPoll()`: Wait for events and populate `l->fired[]`
- `loopyInternalName()`: Return adapter name string

### Async Layer (loopyAsync.c / loopyAsync.h)

Cross-thread and signal-handler-safe event loop wake-up:

- **On Linux**: Uses eventfd for efficient signaling
- **On BSD/macOS**: Uses pipe pair
- Enables worker threads to notify the main event loop
- Essential for `loopyWork` and external notifications

### Timer Abstraction (loopyTimer.c / loopyTimer.h)

User-friendly timer API built on the embedded timer wheel:

- One-shot timers: Fire once then auto-cleanup
- Periodic timers: Fire repeatedly at interval
- Automatic ID management and error handling

### Communication Modules

**loopyChannel** (loopyChannel.c / loopyChannel.h):

- Thread-safe inter-thread communication channels
- Three modes: SPSC (lock-free), MPSC, MPMC
- Bounded ring buffers with backpressure handling
- Event loop integration for async operations

**loopyAsync** (loopyAsync.c / loopyAsync.h):

- Simple one-way async notifications
- Safe to call from signal handlers
- Used internally by loopyWork

### Work Queue (loopyWork.c / loopyWork.h)

Thread pool for non-blocking work offloading:

- `loopyWork`: Opaque work queue handle
- Work callbacks run on worker threads
- After-work callbacks run on event loop thread
- Configurable thread count and queue limits

### I/O Modules

High-level abstractions for common I/O patterns:

- **loopyStream**: Unified duplex I/O (TCP, pipes)
- **loopyNet**: Networking utilities and connection pooling
- **loopyFS**: File system operations (read, write, stat, poll)
- **loopyPipe**: Pipe creation and management
- **loopyUDP**: UDP socket helpers
- **loopyDNS**: Asynchronous DNS resolution
- **loopyTLS**: TLS/SSL wrapper for streams
- **loopyProcess**: Subprocess spawning and management
- **loopySignal**: Signal handling
- **loopyTTY**: Terminal operations

### Utility Modules

- **loopyPubSub**: Publish-subscribe messaging
- **loopyIdleWork**: Idle-time work scheduling
- **loopyWatch**: File system change notifications
- **loopyFlock**: File locking
- **loopyFSPoll**: File system polling
- **loopyRandom**: Cryptographically secure random numbers
- **loopyNice**: Process priority management
- **loopyMmap**: Memory mapping utilities
- **loopyMetrics**: Performance metrics collection
- **loopyMaxHeap**: Embedded max-heap data structure
- **loopyConcurrencyPool**: Thread pool implementation
- **loopyConnPool**: Connection pooling
- **loopyClusterRegistry**: Cluster node registration

---

## Adapter Pattern

The adapter pattern is the key architectural decision that allows loopy to work efficiently across all major UNIX-like platforms. Each platform compiles in exactly one adapter.

### Design Principles

1. **Compile-time Selection**: The build system selects the appropriate adapter
2. **Unified Interface**: All adapters implement identical function signatures
3. **Transparent to Users**: Library users never interact with adapters directly
4. **Runtime Detection**: Some adapters (io_uring) can detect availability at runtime

### Adapter Responsibilities

All adapters must implement these functions, which are called by the core event loop:

```c
/* Creation and teardown */
static bool loopyInternalNew(loopyLoop *l);
static bool loopyInternalResize(loopyLoop *l, size_t setSize);
static void loopyInternalFree(loopyLoop *l);

/* Event registration */
static bool loopyInternalAddEvent(loopyLoop *l, int fd, loopyAction mask);
static void loopyInternalDelEvent(loopyLoop *l, int fd, loopyAction mask);

/* Main poll mechanism */
static int loopyInternalPoll(loopyLoop *l, const struct timeval *tvp);

/* Metadata */
static char *loopyInternalName(void);
```

### Platform-Specific Details

#### loopyAdapterLinux.c (epoll)

Implements epoll-based I/O multiplexing:

```c
typedef struct loopyInternalState {
    struct epoll_event *events;  // Buffer for epoll results
    int epollFd;                 // epoll file descriptor
} loopyInternalState;
```

**Event Registration**: Uses `EPOLL_CTL_ADD` for new registrations and `EPOLL_CTL_MOD` for updates.

**Polling**: Calls `epoll_wait()` which returns events in the `events` array, which are translated to `loopyAction` flags.

**Notes**:

- Planned feature: timerfd integration for sub-millisecond timeouts
- Currently supports 1ms timeout resolution

#### loopyAdapterIouring.c (io_uring)

Modern asynchronous I/O submission-completion queue model for Linux 5.1+:

```c
/* Includes epoll fallback for systems without io_uring */
#include "loopyIoUringFS.h"
#include "loopyIoUringNet.h"
```

**Key Features**:

- Unified interface for both network and filesystem I/O
- Submission Queue (SQ) and Completion Queue (CQ) with shared ring buffers
- Automatic mmap management
- Transparent epoll fallback if io_uring unavailable

**Polling**: Uses `io_uring_enter()` syscall with configurable timeout.

**Performance**: Dramatically reduces syscall overhead, especially for high-throughput applications.

#### loopyAdapterBSD.c (kqueue)

BSD/macOS kernel queue mechanism:

```c
typedef struct loopyInternalState {
    int kqfd;              // kqueue file descriptor
    struct kevent *events; // Buffer for kevent results
} loopyInternalState;
```

**Event Registration**: Batches read/write filters into single `kevent()` syscall:

```c
// Register both READ and WRITE in one syscall
EV_SET(&changes[0], fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
EV_SET(&changes[1], fd, EVFILT_WRITE, EV_ADD, 0, 0, NULL);
kevent(kqfd, changes, 2, NULL, 0, NULL);
```

**Polling**: Calls `kevent()` to wait for changes.

**Advantage**: Filters automatically unarmed after firing, preventing event storms.

#### loopyAdapterSolaris.c (event ports)

Solaris/illumos event ports:

```c
typedef struct loopyInternalState {
    int portfd;              // Event port file descriptor
    int pending_fds[512];    // Recently returned fds
    int pending_masks[512];  // Their masks for re-association
} loopyInternalState;
```

**Special Handling**: Event ports auto-disassociate fds after returning them (to prevent re-firing on level-triggered events). The adapter re-associates them before the next poll.

**Polling**: Uses `port_get()` to retrieve events.

#### loopyAdapterGeneric.c (select)

POSIX select() fallback for maximum portability:

```c
typedef struct loopyInternalState {
    fd_set rfds, wfds;    // Read and write sets
    fd_set _rfds, _wfds;  // Copies (select modifies in-place)
} loopyInternalState;
```

**Limitations**:

- O(n) scanning of fds instead of O(1) lookup
- Limited by `FD_SETSIZE` (usually 1024)
- But guaranteed to exist everywhere

**Polling**: Calls `select()` and manually scans the bit sets.

---

## Event Loop Model

The event loop is a **single-threaded reactor** that processes both file and timer events in a structured sequence.

### Main Loop Structure

```c
void loopyMain(loopyLoop *l) {
    l->stop = false;
    while (!l->stop) {
        loopyMetricsIncrementIterations(l);

        // User hook: before processing events
        if (l->sleep.before.cb) {
            l->sleep.before.cb(l, l->sleep.before.clientData);
            if (l->stop) break;
        }

        // Process all pending events
        loopyProcessEvents(l, LOOPY_EVENTS_ALL);

        // User hook: after processing events
        if (l->sleep.after.cb && !l->stop) {
            l->sleep.after.cb(l, l->sleep.after.clientData);
        }
    }
}
```

### Event Processing Phase

```c
loopyProcessEvents(loopyLoop *l, loopyEvents flags)
```

This function:

1. **Determines Wait Duration**: If timers are registered, calculates timeout until next timer
2. **Polls**: Calls adapter's `loopyInternalPoll()` with timeout
3. **Invokes Callbacks**: For each fired event:
   - Check if event still valid (may have been unregistered)
   - Invoke read callback if applicable
   - Invoke write callback if applicable
   - Handles reallocation of events array during callback
4. **Processes Timers**: After all file events, invokes timer wheel callbacks

### Event Flags

The `loopyEvents` parameter controls what gets processed:

```c
typedef enum loopyEvents {
    LOOPY_EVENTS_FILE = 0x01,    // Process file events only
    LOOPY_EVENTS_TIME = 0x02,    // Process timers only
    LOOPY_EVENTS_ALL = 0x03,     // Process both
    LOOPY_EVENTS_NOWAIT = 0x04,  // Don't block (poll immediately)
} loopyEvents;
```

### FD Registration Details

```c
// Register for read events
bool loopyRegisterRead(loopyLoop *l, int fd, loopyFileCallback *cb,
                       void *clientData);

// Register for write events
bool loopyRegisterWrite(loopyLoop *l, int fd, loopyFileCallback *cb,
                        void *clientData);

// Register write only if no write handler exists
bool loopyRegisterWriteIfNoneExists(loopyLoop *l, int fd, loopyFileCallback *cb,
                                    void *clientData);

// Unregister
void loopyUnregisterRead(loopyLoop *l, int fd);
void loopyUnregisterWrite(loopyLoop *l, int fd);
void loopyUnregisterReadWrite(loopyLoop *l, int fd);
```

### Callback Signature

```c
typedef void loopyFileCallback(loopyLoop *l, int fd, void *clientData,
                               loopyAction mask);
```

The callback receives:

- `l`: Event loop reference
- `fd`: File descriptor that's ready
- `clientData`: User-provided context
- `mask`: Which action(s) fired (`LOOPY_ACTION_READ`, `LOOPY_ACTION_WRITE`, or both)

### Timer Registration

Timers use an efficient timer wheel for O(1) operations:

```c
uint64_t loopyRegisterTimer(loopyLoop *l, uint64_t startAfterMicroseconds,
                            uint64_t repeatEveryMicroseconds,
                            timerWheelCallback *cb, void *clientData);

bool loopyUnregisterTimer(loopyLoop *l, timerWheelId id);
```

### Automatic Resizing

The event storage automatically grows when needed:

```
// Smart growth: O(1) amortized with minimal reallocations
if (fd >= l->setSize) {
    newSize = nextPowerOf2(fd);  // E.g., fd=100000 -> newSize=131072
    fdStorageResize(l, newSize);
}
```

The adapter's `loopyInternalResize()` is called to adapt its buffers.

---

## Handle Types

Loopy uses opaque handle pointers throughout to maintain encapsulation and allow internal flexibility. All handles follow the **New/Free pattern**.

### Core Handles

**loopyLoop** - Event loop instance

```c
loopyLoop *loopyNew(int setSize);              // Heap allocation
bool loopyInit(loopyLoop *l, int setSize);    // Stack initialization
void loopyDelete(loopyLoop *l);                // Heap cleanup
void loopyDeinit(loopyLoop *l);                // Stack cleanup
size_t loopySize(void);                        // Get struct size for stack alloc
```

### I/O Handles

**loopyStream** - Duplex I/O abstraction

```c
loopyStream *loopyStreamNew(...);
void loopyStreamFree(loopyStream *s);
loopyStatus loopyStreamRead(loopyStream *s, loopyStreamAllocCallback *alloc,
                            loopyStreamReadCallback *cb, void *userData);
loopyStatus loopyStreamWrite(loopyStream *s, const void *buf, size_t len,
                             loopyStreamWriteCallback *cb, void *userData);
```

**loopyChannel** - Inter-thread communication

```c
loopyChannel *loopyChannelNew(const loopyChannelConfig *config);
void loopyChannelFree(loopyChannel *ch);
loopyChannelStatus loopyChannelSend(loopyChannel *ch, const void *data);
loopyChannelStatus loopyChannelRecv(loopyChannel *ch, void *data);
```

**loopyAsync** - Thread-safe async notification

```c
loopyAsync *loopyAsyncNew(loopyLoop *loop, loopyAsyncCallback *cb,
                          void *userData);
void loopyAsyncFree(loopyAsync *async);
void loopyAsyncSend(loopyAsync *async);  // Thread-safe, signal-safe
```

**loopyWork** - Thread pool work queue

```c
loopyWork *loopyWorkNew(loopyLoop *l, const loopyWorkConfig *config);
void loopyWorkFree(loopyWork *work);
loopyWorkId loopyWorkQueue(loopyWork *work, loopyWorkCallback *cb,
                          loopyAfterWorkCallback *afterCb, void *userData);
```

**loopyTimer** - High-level timer API

```c
loopyTimer *loopyTimerOneShot(loopyLoop *loop, uint64_t delayUs,
                              loopyTimerCallback *cb, void *userData);
loopyTimer *loopyTimerPeriodic(loopyLoop *loop, uint64_t intervalUs,
                               loopyTimerCallback *cb, void *userData);
void loopyTimerCancel(loopyTimer *timer);
```

### Property Access

Most handles provide accessor functions:

```c
// loopyAsync
loopyLoop *loopyAsyncGetLoop(const loopyAsync *async);
void *loopyAsyncGetData(const loopyAsync *async);
void loopyAsyncSetData(loopyAsync *async, void *userData);

// loopyChannel
loopyChannelId loopyChannelGetId(const loopyChannel *ch);
size_t loopyChannelSize(const loopyChannel *ch);
bool loopyChannelIsClosed(const loopyChannel *ch);
```

### Handle Lifecycle Patterns

All handles follow consistent patterns:

**Allocation Pattern:**

1. Create with `loopyXxxNew(...)` → returns opaque pointer or NULL
2. Use handle for operations
3. Free with `loopyXxxFree(handle)` → safe to pass NULL

**Stack Allocation Pattern** (for loopyLoop only):

1. Allocate storage: `byte buf[loopySize()];`
2. Initialize: `loopyInit((loopyLoop*)buf, setSize);`
3. Use normally
4. Cleanup: `loopyDeinit((loopyLoop*)buf);`

---

## Memory Management

Loopy uses straightforward malloc/free patterns (aliased as `zcalloc`/`zfree` for safety and debugging):

### Allocation Strategies

#### Heap Allocation (Recommended)

```c
/* The setSize parameter (1024 here) is the initial capacity for tracking
 * file descriptors. This affects the size of internal arrays for event
 * registration. The array grows automatically if needed. */
loopyLoop *l = loopyNew(1024);  // Auto-allocates and initializes
// ... use l ...
loopyDelete(l);  // Frees everything
```

#### Stack Allocation (Advanced)

For performance-critical applications that want to avoid dynamic allocation:

```c
byte storage[loopySize()];
loopyLoop *l = (loopyLoop *)storage;
/* Same setSize semantics as loopyNew - initial FD tracking capacity */
loopyInit(l, 1024);
// ... use l ...
loopyDeinit(l);
// No separate free needed
```

### Growth Semantics

Data structures grow automatically without copies of existing data:

- **FD Event Storage**: Power-of-2 growth using `realloc()`
- **Max-Heap**: Automatic capacity reservation
- **Timer Wheel**: Allocated on first timer registration

### Cleanup Requirements

All components must be properly freed:

```c
loopyDelete(l);  // Internally calls:
// - loopyInternalFree(l)    [adapter cleanup]
// - loopyDeinit(l)          [general cleanup]
// - timerWheelFree()        [if timers used]
// - loopyMaxHeapDeinit()    [fd heap cleanup]
// - zfree(l->events)
// - zfree(l->fired)
// - zfree(l)                [if heap-allocated]
```

### Configuration Structs

Higher-level modules use configuration structs with sensible defaults:

```c
// Channel configuration
typedef struct loopyChannelConfig {
    loopyChannelType type;  // SPSC, MPSC, or MPMC
    size_t capacity;        // Ring buffer size
    size_t elementSize;     // Size of each message
    bool blocking;          // Block on full?
} loopyChannelConfig;

// Work queue configuration
typedef struct loopyWorkConfig {
    size_t minThreads;      // Minimum worker threads
    size_t maxThreads;      // Maximum worker threads
    size_t maxQueueSize;    // Max pending work items
} loopyWorkConfig;
```

Most configs are initialized with defaults before calling `loopyXxxNew()`.

---

## Threading Model

Loopy is fundamentally **single-threaded**: the event loop runs on one thread and all callbacks execute on that same thread.

### Thread Safety Guarantees

- **Event loop callbacks**: Not thread-safe. Never call loopy functions from event callbacks unless documented otherwise.
- **loopyAsync**: Explicitly designed for cross-thread notifications
- **loopyChannel**: Thread-safe for send/recv across threads
- **loopyWork**: Thread-safe work submission

### Cross-Thread Communication: loopyAsync

For worker threads to notify the event loop, use `loopyAsync`:

```c
// On event loop thread
void onAsyncEvent(loopyLoop *l, loopyAsync *async, void *data) {
    printf("Worker thread wants attention!\n");
}

loopyAsync *async = loopyAsyncNew(loop, onAsyncEvent, NULL);

// On worker thread
void *workerThread(void *arg) {
    loopyAsync *async = (loopyAsync *)arg;
    // ... do work ...
    loopyAsyncSend(async);  // Signal event loop (signal-safe!)
    return NULL;
}
```

**Why loopyAsync?**

- Uses eventfd on Linux (1 syscall)
- Uses pipe on BSD/macOS (1 write)
- Signal-safe (can call from signal handlers)
- Async-safe (can call from any thread)

### Cross-Thread Communication: loopyChannel

For structured message passing between threads:

```c
// Configuration
loopyChannelConfig cfg = {
    .type = LOOPY_CHANNEL_SPSC,
    .elementSize = sizeof(int),
    .capacity = 100,
};

loopyChannel *ch = loopyChannelNew(&cfg);

// Producer thread
int msg = 42;
loopyChannelSend(ch, &msg);  // Thread-safe

// Consumer (event loop thread)
void onChannelReady(loopyLoop *l, loopyChannel *ch, void *data) {
    int msg;
    loopyChannelRecv(ch, &msg);
    printf("Received: %d\n", msg);
}
```

**Channel Modes:**

- **SPSC** (Single-Producer/Single-Consumer): Lock-free using atomics
- **MPSC** (Multi-Producer/Single-Consumer): Mutex on producer side
- **MPMC** (Multi-Producer/Multi-Consumer): Full mutex synchronization

### Thread Pool: loopyWork

Offload CPU-intensive or blocking I/O to worker threads:

```c
// Work that runs on a worker thread
void computeWork(loopyWork *work, loopyWorkId id, void *data) {
    int *result = (int *)data;
    *result = expensive_computation();
    // DO NOT access event loop here!
}

// Called on event loop thread when work completes
void onWorkComplete(loopyLoop *l, loopyWork *work, loopyWorkId id,
                    loopyWorkStatus status, void *data) {
    int *result = (int *)data;
    printf("Result: %d\n", *result);
}

// Queue work
int result;
loopyWorkId id = loopyWorkQueue(workQueue, computeWork,
                                onWorkComplete, &result);
```

**Work Lifecycle:**

1. Submit work on event loop thread
2. Worker thread executes callback
3. `loopyAsync` signals event loop
4. Event loop invokes after-work callback
5. After-work callback can safely access result

---

## Core Structures

### loopyLoop (Opaque)

The main event loop, defined in `loopyInternal.h`:

```c
struct loopyLoop {
    void *state;              // Adapter-specific state
    loopyFileEvent *events;   // FD → callbacks/mask mapping
    loopyFiredEvent *fired;   // Temporary array of events
    loopyMaxHeap fdHeap;      // Max-heap for O(1) maxfd tracking

    struct {
        struct {
            loopyCallback *cb;     // Before poll
            void *clientData;
        } before;
        struct {
            loopyCallback *cb;     // After processing
            void *clientData;
        } after;
    } sleep;

    timerWheel *timer;        // Timer wheel (lazy-init)
    loopyMetricsInternal *metrics;  // Optional metrics
    void *userData;           // User pointer via loopyGetUserData()

    int setSize;              // Allocated size of events/fired
    int maxfd;                // Highest registered fd (-1 if none)

    struct {
        int writeFd;          // For loopyAsync signaling
        int readFd;
    } managementPipe;

    bool stop;                // loopyStop() sets this
    bool allocated;           // True if heap-allocated
};
```

### loopyFileEvent

One per registered file descriptor:

```c
typedef struct loopyFileEvent {
    loopyFileCallback *readCallback;   // Handler for readable
    loopyFileCallback *writeCallback;  // Handler for writable
    void *clientData;                  // Passed to both callbacks
    loopyAction mask;                  // LOOPY_ACTION_READ | LOOPY_ACTION_WRITE
} loopyFileEvent;
```

### loopyFiredEvent

Temporary array populated by adapter during poll:

```c
typedef struct loopyFiredEvent {
    int fd;                // Which fd fired
    uint32_t mask;         // Which event(s) fired
} loopyFiredEvent;
```

### loopyMaxHeap

Embedded max-heap for O(1) maxfd tracking:

```c
typedef struct loopyMaxHeap {
    loopyHeapValue *data;      // Heap array (1-indexed)
    loopyHeapPosEntry *posMap; // Position map for O(1) lookup
    size_t size;               // Current elements
    size_t capacity;           // Array capacity
    size_t posMapSize;         // Position map size
    bool allocated;            // Heap-allocated?
} loopyMaxHeap;
```

### Metrics (Optional)

Performance introspection:

```c
// Enable metrics
loopyMetricsEnable(l);

// Query results
loopyMetricsGetIterations(l);       // Loop iterations
loopyMetricsGetEventsProcessed(l);  // Total events fired
loopyMetricsGetPollTime(l);         // Time in poll syscalls
loopyMetricsGetCallbackTime(l);     // Time in callbacks

// Disable and get final counts
loopyMetricsDisable(l);
```

---

## Status Codes

Loopy uses a unified status code system:

```c
typedef enum loopyStatus {
    LOOPY_OK = 0,              // Success
    LOOPY_ERROR = -1,          // Generic error
    LOOPY_INVALID = -2,        // Invalid argument
    LOOPY_TIMEOUT = -3,        // Operation timed out
    LOOPY_CANCELLED = -4,      // Cancelled
    LOOPY_CLOSED = -5,         // Resource closed
    LOOPY_WOULD_BLOCK = -6,    // Would block (non-blocking op)
    LOOPY_NOMEM = -7,          // Out of memory
    LOOPY_NOT_FOUND = -8,      // Not found
    LOOPY_BUSY = -9,           // Busy
    LOOPY_AGAIN = -10,         // Transient, try again
    LOOPY_EOF = -11,           // End of file
    // Module-specific: -100 to -999
} loopyStatus;

// Get string representation
const char *loopyStatusString(loopyStatus status);
```

---

## Design Patterns

### Opaque Handles

All public types are opaque pointers. Users can't see internals:

```c
// Public header
typedef struct loopyStream loopyStream;  // Just a forward declaration

// Private implementation
struct loopyStream {
    // ... actual fields ...
};
```

**Benefit**: Implementation can change between versions without breaking API.

### New/Free Lifecycle

Consistent allocation pattern across all modules:

```c
// Create with New
Thing *t = loopyThingNew(config);
if (!t) { /* handle error */ }

// Use the thing
loopyThingDoWork(t, arg);

// Cleanup with Free (NULL-safe)
loopyThingFree(t);
```

### Embedded Structures

For zero-copy efficiency, some structures embed others:

```c
// loopyLoop embeds loopyMaxHeap
struct loopyLoop {
    // ...
    loopyMaxHeap fdHeap;  // Not a pointer!
    // ...
};

// Init via loopyInit
loopyMaxHeapInit(&l->fdHeap, setSize);

// No separate allocation/freeing needed
```

### Callback Registration

Consistent pattern for registering callbacks:

```c
bool loopyRegisterRead(loopyLoop *l, int fd, loopyFileCallback *cb,
                       void *clientData);
```

Three parameters always present:

1. The handle/loop being registered with
2. The callback function pointer
3. The user context data

---

## Performance Characteristics

### Time Complexity

| Operation              | Complexity     | Notes                                   |
| ---------------------- | -------------- | --------------------------------------- |
| Register fd for I/O    | O(1)           | Most adapters (epoll, kqueue, io_uring) |
| Unregister fd          | O(log n)       | Max-heap removal                        |
| Find max fd            | O(1)           | Heap peek                               |
| Poll (one event cycle) | O(1) to O(n)   | Depends on events available             |
| Register timer         | O(1) amortized | Timer wheel                             |
| Fire timer             | O(1)           | Timer wheel efficient                   |

### Space Complexity

| Component      | Space      | Notes                 |
| -------------- | ---------- | --------------------- |
| loopyLoop base | ~100 bytes | Constant overhead     |
| Per-fd storage | 32 bytes   | loopyFileEvent        |
| Adapter state  | Variable   | Adapter-dependent     |
| Max-heap       | O(n)       | n = max fd + 1        |
| Event buffer   | O(setSize) | loopyFiredEvent array |

### Optimization Techniques

1. **Power-of-2 Growth**: Amortized O(1) allocation
2. **Max-Heap**: O(1) maxfd lookup vs O(n) scan
3. **Adapter Batching**: e.g., kqueue batches read+write in one syscall
4. **Timer Wheel**: O(1) operations with minimal memory overhead
5. **Event Coalescing**: Multiple loopyAsyncSend() → one callback
6. **Lock-Free Channels**: SPSC uses atomics, no locks

---

## Building and Selecting Adapters

The CMake build system selects the appropriate adapter:

```cmake
# CMakeLists.txt detects platform and sets:
# - On Linux: loopyAdapterLinux.c or loopyAdapterIouring.c
# - On BSD/macOS: loopyAdapterBSD.c
# - On Solaris: loopyAdapterSolaris.c
# - Fallback: loopyAdapterGeneric.c

if(LINUX)
    target_sources(loopy PRIVATE src/loopyAdapterLinux.c)
    # Or with io_uring: src/loopyAdapterIouring.c
elseif(APPLE OR BSD)
    target_sources(loopy PRIVATE src/loopyAdapterBSD.c)
# ...
endif()
```

Query the selected adapter at runtime:

```c
const char *name = loopyAdapterName();  // "epoll", "kqueue", "select", etc.

// Check if using io_uring specifically (Linux only)
if (loopyUsingIoUring(loop)) {
    printf("Using io_uring\n");
}
```

---

## Summary

Loopy's architecture achieves high performance and portability through:

1. **Adapter Pattern**: Abstract I/O multiplexing for platform independence
2. **Single-Threaded Reactor**: Simple, predictable event handling
3. **Opaque Handles**: Stable API despite internal changes
4. **Efficient Data Structures**: Max-heap, timer wheel, lockfree channels
5. **Clear Separation**: Core loop, adapters, high-level modules, utilities
6. **Thread Communication**: loopyAsync and loopyChannel for worker integration

The result is a robust, efficient event loop suitable for building high-performance servers, clients, and system tools across all major UNIX platforms.
