# Loopy Event Loop - Usage Guide

Loopy is a high-performance event loop library for C that provides efficient I/O multiplexing, timer management, signal handling, and more. This guide covers the core concepts and common usage patterns.

## Table of Contents

1. [Core Concepts](#core-concepts)
2. [Basic Event Loop Setup](#basic-event-loop-setup)
3. [Timer Usage](#timer-usage)
4. [Signal Handling](#signal-handling)
5. [Idle, Prepare, and Check Handles](#idle-prepare-and-check-handles)

---

## Core Concepts

### Event Loop (loopyLoop)

The event loop is the central component of loopy. It manages all registered handles (timers, file descriptors, signals) and dispatches callbacks when events occur.

**Key Functions:**

- `loopyLoop *loopyNew(int setSize)` - Create a new event loop on the heap
- `void loopyMain(loopyLoop *l)` - Run the event loop until stopped
- `void loopyStop(loopyLoop *l)` - Stop the event loop
- `void loopyDelete(loopyLoop *l)` - Clean up and free the event loop

**Key Accessors:**

- `void *loopyGetUserData(const loopyLoop *l)` - Get user data attached to the loop
- `void loopySetUserData(loopyLoop *l, void *data)` - Set user data
- `int loopyGetSetSize(const loopyLoop *l)` - Get max file descriptors
- `bool loopyIsStopped(const loopyLoop *l)` - Check if loop is stopped
- `const char *loopyAdapterName(void)` - Get name of I/O adapter (epoll, kqueue, io_uring, etc.)
- `bool loopyUsingIoUring(const loopyLoop *l)` - Check if using io_uring backend

**Parameters:**

- `setSize` - Initial capacity for file descriptor tracking. This determines how many
  file descriptors (sockets, pipes, files, etc.) can be monitored simultaneously
  without internal reallocation.

  **Choosing the right capacity:**
  - Small utilities or clients: 64-256 (minimal memory footprint)
  - Typical servers: 1024 (good default for most applications)
  - High-concurrency servers: 10000+ (for thousands of simultaneous connections)
  - Maximum practical limit: 65535 (or system `ulimit -n`)

  The event loop will automatically grow if more FDs are needed, but starting
  with an appropriate size avoids reallocation overhead during operation.
  Note: Timers do NOT consume file descriptor slots - they use an efficient
  internal timer wheel.

### Handles

Handles are opaque pointers that represent registered resources. Common handle types include:

- **Timer Handles** (`loopyTimer *`) - Represent one-shot or periodic timers
- **Signal Handles** (`loopySignalHandler *`) - Represent signal handling
- **Idle Handles** (`loopyIdleHandle *`) - Execute callbacks when loop is idle
- **Prepare Handles** (`loopyPrepareHandle *`) - Execute callbacks before I/O polling
- **Check Handles** (`loopyCheckHandle *`) - Execute callbacks after I/O polling

Each handle is created with an associated callback and optional user data context.

### Callbacks

Callbacks are C functions that are invoked when events occur. Different handle types use different callback signatures:

**Timer Callback:**

```c
void timer_callback(loopyLoop *loop, loopyTimer *timer, void *userData) {
    // Called when timer fires
}
```

**Signal Callback:**

```c
void signal_callback(loopyLoop *loop, int signum, void *userData) {
    // Called when signal is received
}
```

**Idle Callback:**

```c
bool idle_callback(loopyLoop *loop, loopyIdleHandle *handle, void *userData) {
    // Return true to continue, false to auto-stop
}
```

**Prepare/Check Callbacks:**

```c
void prepare_callback(loopyLoop *loop, loopyPrepareHandle *handle, void *userData) {
    // Called before I/O poll
}

void check_callback(loopyLoop *loop, loopyCheckHandle *handle, void *userData) {
    // Called after I/O poll
}
```

### User Data

Each handle can have user data (a `void *` pointer) attached to it for context. This allows you to associate application state with handles:

```c
// Create a timer with user data
typedef struct {
    int request_id;
    char *url;
} RequestContext;

RequestContext *ctx = malloc(sizeof(RequestContext));
ctx->request_id = 42;
ctx->url = "https://example.com";

loopyTimer *timer = loopyTimerOneShotMs(loop, 5000, timer_cb, ctx);

// In callback, access the user data
void timer_cb(loopyLoop *l, loopyTimer *t, void *userData) {
    RequestContext *ctx = (RequestContext *)userData;
    printf("Request %d timed out\n", ctx->request_id);
}
```

---

## Basic Event Loop Setup

### Creating and Running an Event Loop

```c
#include <loopy.h>

int main() {
    /* Create event loop with capacity for 1024 file descriptors.
     *
     * This capacity determines how many FDs can be monitored simultaneously.
     * Each socket, pipe, or file being watched consumes one slot.
     * 1024 is a good default for most servers - adjust based on expected
     * concurrent connections (see "Choosing the right capacity" above).
     */
    loopyLoop *loop = loopyNew(1024);
    if (!loop) {
        perror("Failed to create event loop");
        return 1;
    }

    // Register your handles (timers, signals, etc.)
    // See sections below for examples

    // Run the event loop (blocks until loopyStop is called)
    loopyMain(loop);

    // Clean up
    loopyDelete(loop);
    return 0;
}
```

### Stack Allocation

For advanced use cases, you can allocate loopyLoop on the stack:

```c
#include <loopy.h>
#include <string.h>

int main() {
    // Allocate on stack
    loopyLoop loop;
    memset(&loop, 0, loopySize());

    if (!loopyInit(&loop, 1024)) {
        perror("Failed to init event loop");
        return 1;
    }

    // Use the loop
    loopyMain(&loop);

    // Cleanup
    loopyDeinit(&loop);
    return 0;
}
```

### Event Loop Control

```c
// Stop the event loop from within a callback
void stop_handler(loopyLoop *l, loopyTimer *t, void *data) {
    printf("Stopping event loop...\n");
    loopyStop(l);  // Signal the loop to stop
}

// You can also check if the loop is stopped
if (loopyIsStopped(loop)) {
    printf("Loop has stopped\n");
}
```

### Event Loop Callbacks

You can register callbacks that execute before and after I/O polling:

```c
void before_sleep(loopyLoop *l, void *userData) {
    // Called before the event loop blocks waiting for I/O
    printf("About to sleep...\n");
}

void after_sleep(loopyLoop *l, void *userData) {
    // Called after the event loop returns from I/O
    printf("Woke up from sleep\n");
}

loopySetBeforeSleepCallback(loop, before_sleep, NULL);
loopySetAfterSleepCallback(loop, after_sleep, NULL);
```

---

## Timer Usage

Loopy provides two main timer types: **one-shot timers** (fire once) and **periodic timers** (fire repeatedly).

### One-Shot Timers

A one-shot timer fires once after a specified delay and is automatically cleaned up.

**Functions:**

- `loopyTimer *loopyTimerOneShot(loopyLoop *l, uint64_t delayUs, loopyTimerCallback *cb, void *userData)` - Create with microsecond delay
- `loopyTimer *loopyTimerOneShotMs(loopyLoop *l, uint64_t delayMs, loopyTimerCallback *cb, void *userData)` - Create with millisecond delay
- `loopyTimer *loopyTimerOneShotSeconds(loopyLoop *l, uint64_t delaySeconds, loopyTimerCallback *cb, void *userData)` - Create with second delay

**Example: Basic One-Shot Timer**

```c
void timeout_handler(loopyLoop *l, loopyTimer *t, void *data) {
    printf("Timeout fired after 5 seconds!\n");
    loopyStop(l);
}

loopyTimer *timer = loopyTimerOneShotMs(loop, 5000, timeout_handler, NULL);
if (!timer) {
    fprintf(stderr, "Failed to create timer: %s\n", loopyTimerGetError());
}
```

**Example: One-Shot Timer with User Data**

```c
typedef struct {
    int request_id;
    char *request_name;
} TimerData;

void request_timeout(loopyLoop *l, loopyTimer *t, void *data) {
    TimerData *ctx = (TimerData *)data;
    printf("Request %d (%s) timed out\n", ctx->request_id, ctx->request_name);
    free(ctx->request_name);
    free(ctx);
    // Timer is automatically freed after callback returns
}

TimerData *ctx = malloc(sizeof(TimerData));
ctx->request_id = 123;
ctx->request_name = strdup("fetch_user_profile");

loopyTimer *timer = loopyTimerOneShotMs(loop, 10000, request_timeout, ctx);
```

### Periodic Timers

Periodic timers fire repeatedly at fixed intervals until explicitly cancelled.

**Functions:**

- `loopyTimer *loopyTimerPeriodic(loopyLoop *l, uint64_t intervalUs, loopyTimerCallback *cb, void *userData)` - Create with microsecond interval
- `loopyTimer *loopyTimerPeriodicMs(loopyLoop *l, uint64_t intervalMs, loopyTimerCallback *cb, void *userData)` - Create with millisecond interval
- `loopyTimer *loopyTimerPeriodicSeconds(loopyLoop *l, uint64_t intervalSeconds, loopyTimerCallback *cb, void *userData)` - Create with second interval
- `loopyTimer *loopyTimerPeriodicDelayed(loopyLoop *l, uint64_t initialDelayUs, uint64_t intervalUs, loopyTimerCallback *cb, void *userData)` - Create with initial delay before first firing

**Example: Periodic Timer**

```c
typedef struct {
    int tick_count;
} TickData;

void heartbeat(loopyLoop *l, loopyTimer *t, void *data) {
    TickData *ctx = (TickData *)data;
    ctx->tick_count++;
    printf("Heartbeat #%d\n", ctx->tick_count);

    // Stop after 10 beats
    if (ctx->tick_count >= 10) {
        loopyTimerCancel(t);
    }
}

TickData *ctx = malloc(sizeof(TickData));
ctx->tick_count = 0;

// Fire every 1 second
loopyTimer *timer = loopyTimerPeriodicMs(loop, 1000, heartbeat, ctx);
```

**Example: Periodic Timer with Initial Delay**

```c
void delayed_periodic(loopyLoop *l, loopyTimer *t, void *data) {
    printf("Firing...\n");
}

// Wait 5 seconds, then fire every 2 seconds
loopyTimer *timer = loopyTimerPeriodicDelayed(loop, 5000000, 2000000,
                                              delayed_periodic, NULL);
```

### Timer Management

**Cancel a Timer:**

```c
void cancel_handler(loopyLoop *l, loopyTimer *t, void *data) {
    printf("Cancelling timer...\n");
}

loopyTimer *periodic = loopyTimerPeriodicMs(loop, 1000, cancel_handler, NULL);

// Later, cancel it
loopyTimerCancel(periodic);
periodic = NULL;  // Good practice: clear the pointer after cancellation
```

**Check if Timer is Active:**

```c
if (loopyTimerIsActive(timer)) {
    printf("Timer is still active\n");
} else {
    printf("Timer has been cancelled or fired\n");
}
```

**Access Timer Data:**

```c
// Get user data
void *data = loopyTimerGetData(timer);

// Update user data
loopyTimerSetData(timer, new_data);

// Get the event loop
loopyLoop *l = loopyTimerGetLoop(timer);
```

**Error Handling:**

```c
loopyTimer *timer = loopyTimerOneShotMs(loop, 1000, callback, NULL);
if (!timer) {
    const char *error = loopyTimerGetError();
    fprintf(stderr, "Timer error: %s\n", error);
}
```

---

## Signal Handling

Loopy provides safe signal handling by capturing signals and delivering them to callbacks within the main event loop context. This avoids issues with async-signal-safe restrictions.

### Creating a Signal Handler

```c
#include <loopy.h>
#include <signal.h>

// Create signal handler
loopySignalHandler *sh = loopySignalNew(loop);
if (!sh) {
    perror("Failed to create signal handler");
    return 1;
}

// ... register signals ...

// Clean up (restores default signal dispositions)
loopySignalFree(sh);
```

### Registering Signal Callbacks

**Register a Persistent Signal Handler:**

```c
void sigterm_handler(loopyLoop *l, int signum, void *userData) {
    printf("Received SIGTERM, shutting down...\n");
    loopyStop(l);
}

loopySignalHandler *sh = loopySignalNew(loop);
if (!loopySignalRegister(sh, SIGTERM, sigterm_handler, NULL)) {
    fprintf(stderr, "Failed to register SIGTERM\n");
    return 1;
}
```

**Register a One-Shot Signal Handler:**

```c
void sigusr1_handler(loopyLoop *l, int signum, void *userData) {
    printf("Received SIGUSR1 (will only fire once)\n");
}

// This callback fires once, then automatically unregisters
if (!loopySignalRegisterOneshot(sh, SIGUSR1, sigusr1_handler, NULL)) {
    fprintf(stderr, "Failed to register SIGUSR1\n");
}
```

### Signal Handling with User Data

```c
typedef struct {
    int shutdown_count;
} ShutdownContext;

void sigterm_with_context(loopyLoop *l, int signum, void *userData) {
    ShutdownContext *ctx = (ShutdownContext *)userData;
    ctx->shutdown_count++;

    printf("Shutdown requested (count: %d)\n", ctx->shutdown_count);

    // Allow graceful shutdown with multiple signals
    if (ctx->shutdown_count >= 3) {
        printf("Force shutting down\n");
        loopyStop(l);
    }
}

ShutdownContext *ctx = malloc(sizeof(ShutdownContext));
ctx->shutdown_count = 0;

loopySignalHandler *sh = loopySignalNew(loop);
loopySignalRegister(sh, SIGTERM, sigterm_with_context, ctx);
loopySignalRegister(sh, SIGINT, sigterm_with_context, ctx);
```

### Unregistering Signal Handlers

```c
// Unregister a signal (restores default disposition)
loopySignalUnregister(sh, SIGTERM);
```

### Signal Handler Utilities

**Get Signal Name:**

```c
const char *name = loopySignalName(SIGTERM);
printf("Signal name: %s\n", name);  // Output: "SIGTERM"
```

**Access Signal Handler Data:**

```c
// Get user data
void *data = loopySignalGetData(sh);

// Set/update user data
loopySignalSetData(sh, new_data);

// Get associated event loop
loopyLoop *l = loopySignalGetLoop(sh);
```

### Common Signal Examples

Loopy provides constants for common signals:

```c
#define LOOPY_SIGTERM SIGTERM   // Termination
#define LOOPY_SIGINT  SIGINT    // Interrupt (Ctrl+C)
#define LOOPY_SIGHUP  SIGHUP    // Hangup
#define LOOPY_SIGUSR1 SIGUSR1   // User-defined 1
#define LOOPY_SIGUSR2 SIGUSR2   // User-defined 2
#define LOOPY_SIGPIPE SIGPIPE   // Broken pipe
#define LOOPY_SIGCHLD SIGCHLD   // Child process change
```

**Example: Graceful Shutdown with Multiple Signals**

```c
void graceful_shutdown(loopyLoop *l, int signum, void *userData) {
    const char *sig_name = loopySignalName(signum);
    printf("Received %s, initiating graceful shutdown...\n", sig_name);
    loopyStop(l);
}

loopySignalHandler *sh = loopySignalNew(loop);
loopySignalRegister(sh, SIGTERM, graceful_shutdown, NULL);
loopySignalRegister(sh, SIGINT, graceful_shutdown, NULL);
loopySignalRegister(sh, SIGHUP, graceful_shutdown, NULL);
```

---

## Idle, Prepare, and Check Handles

These handle types allow you to hook into specific phases of the event loop iteration.

### Event Loop Phases

The event loop iteration follows this sequence:

1. **Prepare Phase** - Callbacks run before I/O polling
2. **Poll Phase** - Wait for I/O events (blocking or non-blocking)
3. **Check Phase** - Callbacks run after I/O polling
4. **Idle Phase** - Callbacks run if no events are pending

### Idle Handles

Idle handles execute callbacks when the event loop has no other work to do. While any idle handle is active, the loop uses a zero-timeout poll, effectively spinning the CPU.

**Use Cases:**

- Run work until completion (e.g., processing a queue)
- Busy-waiting for specific conditions
- Integration with external libraries that require polling

**Warning:** Idle handles cause high CPU usage. Stop them as soon as possible.

**Functions:**

- `loopyIdleHandle *loopyIdleStart(loopyLoop *l, loopyIdleCallback *cb, void *userData)` - Start idle handle
- `void loopyIdleStop(loopyIdleHandle *handle)` - Stop (pause) the handle
- `bool loopyIdleRestart(loopyIdleHandle *handle)` - Restart a stopped handle
- `void loopyIdleFree(loopyIdleHandle *handle)` - Free the handle
- `bool loopyIdleIsActive(const loopyIdleHandle *handle)` - Check if active

**Example: Processing a Work Queue**

```c
typedef struct {
    char **items;
    int count;
    int index;
} WorkQueue;

bool process_queue(loopyLoop *l, loopyIdleHandle *handle, void *userData) {
    WorkQueue *queue = (WorkQueue *)userData;

    // Process one item
    if (queue->index < queue->count) {
        printf("Processing: %s\n", queue->items[queue->index]);
        queue->index++;
        return true;  // Continue processing
    }

    // Queue is empty, stop the idle handle
    printf("Queue processing complete\n");
    return false;  // Auto-stop this idle handle
}

WorkQueue *queue = malloc(sizeof(WorkQueue));
queue->items = (char *[]){"task1", "task2", "task3"};
queue->count = 3;
queue->index = 0;

loopyIdleHandle *idle = loopyIdleStart(loop, process_queue, queue);
```

**Example: Manual Idle Control**

```c
loopyIdleHandle *idle = loopyIdleStart(loop, idle_callback, NULL);

// ... later ...

// Stop the idle handle (pause)
loopyIdleStop(idle);

// Check if it's active
if (!loopyIdleIsActive(idle)) {
    printf("Idle handle is stopped\n");
}

// Restart it
if (loopyIdleRestart(idle)) {
    printf("Idle handle restarted\n");
}

// Finally, free it
loopyIdleFree(idle);
```

### Prepare Handles

Prepare handles execute callbacks before the I/O poll operation. They're useful for pre-polling setup.

**Use Cases:**

- Flush pending writes before blocking
- Setup state for poll operation
- Prepare data structures before I/O

**Functions:**

- `loopyPrepareHandle *loopyPrepareStart(loopyLoop *l, loopyPrepareCallback *cb, void *userData)` - Start prepare handle
- `void loopyPrepareStop(loopyPrepareHandle *handle)` - Stop the handle
- `bool loopyPrepareRestart(loopyPrepareHandle *handle)` - Restart a stopped handle
- `void loopyPrepareFree(loopyPrepareHandle *handle)` - Free the handle
- `bool loopyPrepareIsActive(const loopyPrepareHandle *handle)` - Check if active

**Example: Flushing Before Poll**

```c
typedef struct {
    int pending_writes;
} WriteBuffer;

void prepare_flush(loopyLoop *l, loopyPrepareHandle *handle, void *userData) {
    WriteBuffer *buf = (WriteBuffer *)userData;

    if (buf->pending_writes > 0) {
        printf("Flushing %d pending writes before poll...\n", buf->pending_writes);
        // Perform flush operation
        buf->pending_writes = 0;
    }
}

WriteBuffer *buf = malloc(sizeof(WriteBuffer));
buf->pending_writes = 0;

loopyPrepareHandle *prep = loopyPrepareStart(loop, prepare_flush, buf);
```

### Check Handles

Check handles execute callbacks after the I/O poll operation. They're useful for processing poll results or performing work after blocking returns.

**Use Cases:**

- Process results from I/O operations
- Perform work after blocking
- Integrate with external libraries

**Functions:**

- `loopyCheckHandle *loopyCheckStart(loopyLoop *l, loopyCheckCallback *cb, void *userData)` - Start check handle
- `void loopyCheckStop(loopyCheckHandle *handle)` - Stop the handle
- `bool loopyCheckRestart(loopyCheckHandle *handle)` - Restart a stopped handle
- `void loopyCheckFree(loopyCheckHandle *handle)` - Free the handle
- `bool loopyCheckIsActive(const loopyCheckHandle *handle)` - Check if active

**Example: Processing Poll Results**

```c
typedef struct {
    int events_processed;
} EventCounter;

void check_events(loopyLoop *l, loopyCheckHandle *handle, void *userData) {
    EventCounter *counter = (EventCounter *)userData;
    counter->events_processed++;
    printf("Poll completed, events processed: %d\n", counter->events_processed);
}

EventCounter *counter = malloc(sizeof(EventCounter));
counter->events_processed = 0;

loopyCheckHandle *check = loopyCheckStart(loop, check_events, counter);
```

### Querying Active Handles

```c
#include <loopyIdle.h>

// Count active handles
size_t idle_count = loopyIdleCount(loop);
size_t prep_count = loopyPrepareCount(loop);
size_t check_count = loopyCheckCount(loop);

printf("Active handles: idle=%zu, prepare=%zu, check=%zu\n",
       idle_count, prep_count, check_count);

// Check if any idle handles are active (useful for detecting CPU spin)
if (loopyHasActiveIdle(loop)) {
    printf("Warning: Idle handles active, loop may be spinning\n");
}
```

### Handle Lifecycle

```c
// Start
loopyIdleHandle *idle = loopyIdleStart(loop, callback, userData);

// Stop (pause)
loopyIdleStop(idle);

// Check status
if (loopyIdleIsActive(idle)) {
    printf("Active\n");
}

// Restart
loopyIdleRestart(idle);

// Access data
void *data = loopyIdleGetData(idle);
loopyIdleSetData(idle, new_data);

// Get event loop
loopyLoop *l = loopyIdleGetLoop(idle);

// Free
loopyIdleFree(idle);
idle = NULL;
```

---

## Complete Example: Server with Timers and Signals

Here's a complete example combining multiple loopy features:

```c
#include <loopy.h>
#include <loopyTimer.h>
#include <loopySignal.h>
#include <loopyIdle.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    loopyLoop *loop;
    int requests_processed;
    loopyTimer *heartbeat;
} ServerContext;

void heartbeat_callback(loopyLoop *l, loopyTimer *t, void *userData) {
    ServerContext *ctx = (ServerContext *)userData;
    printf("Heartbeat - Requests processed: %d\n", ctx->requests_processed);
}

void shutdown_callback(loopyLoop *l, int signum, void *userData) {
    ServerContext *ctx = (ServerContext *)userData;
    printf("Received signal %d, shutting down gracefully...\n", signum);
    loopyStop(l);
}

bool idle_callback(loopyLoop *l, loopyIdleHandle *handle, void *userData) {
    ServerContext *ctx = (ServerContext *)userData;

    // Simulate processing a request
    ctx->requests_processed++;

    // Stop after 10 requests
    if (ctx->requests_processed >= 10) {
        printf("Processed 10 requests, stopping\n");
        return false;
    }

    return true;
}

int main() {
    /* Create event loop with capacity for 1024 concurrent FDs.
     * This example server uses FDs for signal handling and could add
     * network sockets. 1024 allows plenty of headroom for expansion. */
    loopyLoop *loop = loopyNew(1024);
    if (!loop) {
        perror("Failed to create event loop");
        return 1;
    }

    // Setup context
    ServerContext ctx = {
        .loop = loop,
        .requests_processed = 0,
        .heartbeat = NULL
    };

    // Setup signals
    loopySignalHandler *sh = loopySignalNew(loop);
    if (!sh) {
        perror("Failed to create signal handler");
        loopyDelete(loop);
        return 1;
    }

    loopySignalRegister(sh, SIGTERM, shutdown_callback, &ctx);
    loopySignalRegister(sh, SIGINT, shutdown_callback, &ctx);

    // Setup heartbeat timer (every 2 seconds)
    ctx.heartbeat = loopyTimerPeriodicMs(loop, 2000, heartbeat_callback, &ctx);
    if (!ctx.heartbeat) {
        fprintf(stderr, "Failed to create heartbeat timer\n");
        loopySignalFree(sh);
        loopyDelete(loop);
        return 1;
    }

    // Setup idle work
    loopyIdleHandle *idle = loopyIdleStart(loop, idle_callback, &ctx);
    if (!idle) {
        fprintf(stderr, "Failed to create idle handle\n");
        loopyTimerCancel(ctx.heartbeat);
        loopySignalFree(sh);
        loopyDelete(loop);
        return 1;
    }

    printf("Server starting (max 10 requests)...\n");
    printf("Send SIGTERM or SIGINT to shut down gracefully\n\n");

    // Run event loop
    loopyMain(loop);

    printf("\nShutdown complete. Final stats:\n");
    printf("  Requests processed: %d\n", ctx.requests_processed);

    // Cleanup
    loopyIdleFree(idle);
    loopySignalFree(sh);
    loopyDelete(loop);

    return 0;
}
```

Compile with:

```bash
gcc -o server example.c -lloopy
```

Run and send signals:

```bash
./server &
kill -SIGTERM $!
# or
kill -SIGINT $!
```

---

## API Reference Quick Summary

### Core Loop Functions

- `loopyLoop *loopyNew(int setSize)` - Create event loop with specified FD capacity
- `void loopyMain(loopyLoop *l)` - Run event loop
- `void loopyStop(loopyLoop *l)` - Stop event loop
- `void loopyDelete(loopyLoop *l)` - Free event loop

### Timer Functions

- `loopyTimer *loopyTimerOneShot(loopyLoop *l, uint64_t delayUs, loopyTimerCallback *cb, void *userData)`
- `loopyTimer *loopyTimerOneShotMs(loopyLoop *l, uint64_t delayMs, loopyTimerCallback *cb, void *userData)`
- `loopyTimer *loopyTimerPeriodic(loopyLoop *l, uint64_t intervalUs, loopyTimerCallback *cb, void *userData)`
- `loopyTimer *loopyTimerPeriodicMs(loopyLoop *l, uint64_t intervalMs, loopyTimerCallback *cb, void *userData)`
- `void loopyTimerCancel(loopyTimer *timer)`
- `bool loopyTimerIsActive(const loopyTimer *timer)`

### Signal Functions

- `loopySignalHandler *loopySignalNew(loopyLoop *loop)`
- `void loopySignalFree(loopySignalHandler *sh)`
- `bool loopySignalRegister(loopySignalHandler *sh, int signum, loopySignalCallback *cb, void *userData)`
- `bool loopySignalUnregister(loopySignalHandler *sh, int signum)`
- `const char *loopySignalName(int signum)`

### Idle/Prepare/Check Functions

- `loopyIdleHandle *loopyIdleStart(loopyLoop *loop, loopyIdleCallback *cb, void *userData)`
- `void loopyIdleStop(loopyIdleHandle *handle)`
- `void loopyIdleFree(loopyIdleHandle *handle)`
- `loopyPrepareHandle *loopyPrepareStart(loopyLoop *loop, loopyPrepareCallback *cb, void *userData)`
- `void loopyPrepareStop(loopyPrepareHandle *handle)`
- `void loopyPrepareFree(loopyPrepareHandle *handle)`
- `loopyCheckHandle *loopyCheckStart(loopyLoop *loop, loopyCheckCallback *cb, void *userData)`
- `void loopyCheckStop(loopyCheckHandle *handle)`
- `void loopyCheckFree(loopyCheckHandle *handle)`

For more details, consult the header files in `src/`:

- `loopy.h` - Core event loop API
- `loopyTimer.h` - Timer API
- `loopySignal.h` - Signal handling API
- `loopyIdle.h` - Idle/Prepare/Check API
