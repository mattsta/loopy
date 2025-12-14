# loopy - High-Performance Event Loop Library for C

A cross-platform, production-grade event loop library for C with support for multiple I/O backends and comprehensive async abstractions. Similar to libuv but with distinct design choices and a rich feature set optimized for network applications, filesystem operations, and complex asynchronous workflows.

## Project Philosophy

loopy is designed with these core principles:

- **Performance-First**: Zero-copy I/O abstractions, efficient event multiplexing with io_uring on Linux and kqueue on BSD/macOS
- **Cross-Platform**: Seamless abstractions across Linux, macOS, FreeBSD, with platform-optimal I/O backends
- **Composable**: Build complex async patterns from simple, single-purpose primitives
- **Consistent API**: Uniform naming conventions, error handling, and resource management across all modules
- **Production Ready**: Comprehensive error handling, memory safety, and debugging support

## Key Features

### Core Event Loop

- **Efficient multiplexing** with platform-optimal backends:
  - Linux: epoll (default) or io_uring (opt-in) for superior performance
  - macOS/FreeBSD: kqueue for file and network events
- **Flexible event model**: File descriptor readiness, timers, or hybrid processing
- **Before/after sleep callbacks** for custom event processing
- **Configurable concurrency**: Resize max file descriptors at runtime

### Networking

- **TCP streams** with full duplex I/O, connect, bind, listen, and accept
- **UDP sockets** for connectionless datagram communication
- **TLS/SSL support** with automatic encryption/decryption
- **DNS resolution** with async address lookups
- **Connection pooling** for efficient resource reuse
- **Low-level network utilities** with helper functions for TCP/Unix sockets
- **Full IPv4/IPv6 support** with unified address parsing and formatting
- **File descriptor passing** for advanced IPC patterns

### Timers

- **One-shot timers** that fire once and self-cleanup
- **Periodic timers** with flexible interval control
- **High-resolution timing** in microseconds, milliseconds, or seconds
- **Efficient timer wheel** for minimal overhead with many concurrent timers

### Filesystem Operations

- **Non-blocking file I/O** via thread pool integration
- **File watching** with change notifications
- **Memory mapping** with async page loading
- **File locking** with blocking and non-blocking modes
- **File polling** for kqueue/epoll-based monitoring
- **Comprehensive operations**: open, close, read, write, stat, mkdir, rmdir, unlink, chmod, and more

### Processes & Pipes

- **Process spawning** and lifecycle management
- **Pipe creation** for inter-process communication
- **Stream pipes** with full async I/O integration
- **Bidirectional data transfer** between processes

### Pub/Sub & Clustering

- **Pub/Sub system** for intra-process event distribution
- **Cluster registry** for distributed coordination

### Threading & Concurrency

- **Thread-safe async wake-up** from any thread or signal handler
- **Work queue** for offloading CPU-intensive tasks
- **Concurrency pool** for managing worker threads
- **Thread-safe async callbacks** with proper event loop integration

### Utilities & Monitoring

- **Signal handling** with async-signal-safe callbacks
- **Idle detection** for low-activity optimization
- **TTY operations** for terminal control
- **Random number generation** with cryptographic and performance variants
- **Rate limiting** for traffic control
- **Metrics collection** for performance monitoring
- **Process priority control** (nice) for resource management

### Advanced Features

- **User data pointers** for easy callback context management
- **Stack allocation** option for embedded systems or performance-critical code
- **Error propagation** with human-readable error messages
- **Type-safe opaque handles** to prevent misuse

## Quick Start

### Basic Event Loop

```c
#include "loopy.h"

int main() {
    /* Create event loop with initial capacity for 1024 file descriptors.
     *
     * The capacity parameter sets the initial size of the internal event
     * tracking array. This determines how many file descriptors (sockets,
     * files, pipes, etc.) can be monitored simultaneously without reallocation.
     *
     * Guidelines for choosing capacity:
     *   - Small utilities/clients: 64-256 (minimal memory footprint)
     *   - Typical servers: 1024 (good default for most applications)
     *   - High-concurrency servers: 10000+ (for thousands of connections)
     *
     * The array grows automatically if more FDs are needed, but starting
     * with the right size avoids reallocation overhead during operation.
     */
    loopyLoop *loop = loopyNew(1024);
    if (!loop) {
        fprintf(stderr, "Failed to create event loop\n");
        return 1;
    }

    // Register timers, file descriptors, etc.
    // ...

    // Run event loop
    loopyMain(loop);

    // Cleanup
    loopyDelete(loop);
    return 0;
}
```

### TCP Echo Server

Here's a complete example of a simple TCP echo server that demonstrates the event loop and stream APIs:

```c
#include "loopy.h"
#include "loopyStream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    loopyStream *stream;
} ClientContext;

void onRead(loopyStream *stream, ssize_t nread, const void *buf, void *userData) {
    ClientContext *ctx = (ClientContext *)userData;

    if (nread < 0) {
        fprintf(stderr, "Read error: %ld\n", nread);
        loopyStreamClose(stream, NULL, NULL);
        free(ctx);
        return;
    }

    if (nread == 0) {
        // EOF - peer closed connection
        printf("Client disconnected\n");
        loopyStreamClose(stream, NULL, NULL);
        free(ctx);
        return;
    }

    // Echo the data back
    loopyStreamWrite(stream, buf, nread, NULL, NULL);
}

void onAlloc(loopyStream *stream, size_t suggested, void **buf,
             size_t *bufLen, void *userData) {
    /* Buffer allocation callback - called before each read to get a buffer.
     * Using a 4KB static buffer is simple but NOT thread-safe/concurrent-safe.
     * For production: allocate per-connection buffers via userData context. */
    static char buffer[4096];
    *buf = buffer;
    *bufLen = sizeof(buffer);
}

void onConnection(loopyStream *server, int status, void *userData) {
    if (status < 0) {
        fprintf(stderr, "Connection error: %d\n", status);
        return;
    }

    // Accept the new connection
    loopyStream *client = loopyStreamAccept(server);
    if (!client) {
        fprintf(stderr, "Failed to accept connection\n");
        return;
    }

    printf("New client connected\n");

    // Create context for this client
    ClientContext *ctx = malloc(sizeof(ClientContext));
    if (!ctx) {
        loopyStreamClose(client, NULL, NULL);
        return;
    }

    ctx->stream = client;
    loopyStreamSetData(client, ctx);

    // Start reading from the client
    loopyStreamReadStart(client, onAlloc, onRead, ctx);
}

int main() {
    /* Create event loop with capacity for 1024 concurrent file descriptors.
     * For a TCP server, this means we can handle ~1000 simultaneous client
     * connections plus a few FDs for the server socket and internal use. */
    loopyLoop *loop = loopyNew(1024);
    if (!loop) {
        fprintf(stderr, "Failed to create event loop\n");
        return 1;
    }

    // Create server stream
    loopyStream *server = loopyStreamNewTcp(loop);
    if (!server) {
        fprintf(stderr, "Failed to create server\n");
        loopyDelete(loop);
        return 1;
    }

    // Bind and listen
    if (!loopyStreamBind(server, "0.0.0.0", 8888)) {
        fprintf(stderr, "Failed to bind: %s\n", loopyStreamGetError(server));
        loopyStreamClose(server, NULL, NULL);
        loopyDelete(loop);
        return 1;
    }

    /* Start listening with a backlog of 128 pending connections.
     * The backlog is how many connections can queue while waiting for accept().
     * Common values: 128 (typical), 511 (nginx default), SOMAXCONN (system max). */
    if (!loopyStreamListen(server, 128, onConnection, NULL)) {
        fprintf(stderr, "Failed to listen: %s\n", loopyStreamGetError(server));
        loopyStreamClose(server, NULL, NULL);
        loopyDelete(loop);
        return 1;
    }

    printf("Echo server listening on 127.0.0.1:8888\n");
    printf("Connect with: nc localhost 8888\n");

    // Run event loop
    loopyMain(loop);

    // Cleanup
    loopyStreamClose(server, NULL, NULL);
    loopyDelete(loop);
    return 0;
}
```

### Timers Example

```c
#include "loopy.h"
#include "loopyTimer.h"
#include <stdio.h>

void timerCallback(loopyLoop *loop, loopyTimer *timer, void *userData) {
    printf("Timer fired: %s\n", (const char *)userData);
}

int main() {
    /* Create event loop. Capacity of 1024 is generous for a timer-only app
     * (timers don't consume FD slots), but allows room for adding sockets. */
    loopyLoop *loop = loopyNew(1024);

    /* One-shot timer: fires once after 5 seconds (5,000,000 microseconds).
     * The timer is automatically freed after firing. */
    loopyTimerOneShot(loop, 5000000, timerCallback, "one-shot");

    /* Periodic timer: fires every 1000ms (1 second) until cancelled.
     * Must be manually cancelled with loopyTimerCancel() when done. */
    loopyTimerPeriodicMs(loop, 1000, timerCallback, "periodic");

    loopyMain(loop);
    loopyDelete(loop);
    return 0;
}
```

## Build Instructions

### Build Steps

```bash
# Clone the repository
git clone https://github.com/mattsta/loopy.git
cd loopy

# Create build directory
mkdir build && cd build

# Configure with CMake
cmake ..

# Build
make -j8

# Optional: Run tests (run tests from build/src to skip running tests of dependencies)
cd src/
ctest -j8
```

### Build Options

```bash
# Build with io_uring support on Linux (requires kernel 5.1+)
cmake -DUSE_IOURING=ON ..

# Build with TLS support
cmake -DUSE_TLS=ON ..

# Debug build with symbols
cmake -DCMAKE_BUILD_TYPE=Debug ..

# Release build with optimizations
cmake -DCMAKE_BUILD_TYPE=Release ..
```

### Linking

```bash
# Link against loopy
gcc myapp.c -o myapp -lloopy -lm

# With pkg-config
gcc myapp.c -o myapp $(pkg-config --cflags --libs loopy)
```

## Platform Support

| Platform         | Adapter  | Features                               | Status            |
| ---------------- | -------- | -------------------------------------- | ----------------- |
| Linux (epoll)    | epoll    | File descriptors, timers               | Stable            |
| Linux (io_uring) | io_uring | Native async I/O, superior performance | Stable (optional) |
| macOS            | kqueue   | File descriptors, timers               | Stable            |
| FreeBSD          | kqueue   | File descriptors, timers               | Stable            |

### Platform-Specific Details

**Linux with io_uring**: When io_uring is available and enabled, loopy automatically uses it for superior I/O performance. If initialization fails, it falls back to epoll. Check at runtime with `loopyUsingIoUring()`.

**macOS/FreeBSD with kqueue**: Provides efficient event multiplexing and timer support with native kernel integration.

## API Design Philosophy

loopy follows consistent patterns across all modules:

### Naming Conventions

- **Creation**: `loopyXxxNew()` returns opaque handle
- **Destruction**: `loopyXxxFree()` or `loopyXxxDelete()` for cleanup
- **Queries**: `loopyXxxIs*()`, `loopyXxxGet*()` for properties
- **Actions**: `loopyXxxStart()`, `loopyXxxStop()` for state changes

### Opaque Handles

All public data structures use opaque pointers (e.g., `loopyStream *`, `loopyTimer *`). This enables:

- ABI stability across versions
- Implementation flexibility
- Prevention of accidental field misuse

### Error Handling

All operations return status codes or NULL on failure:

```c
typedef enum loopyStatus {
    LOOPY_OK = 0,
    LOOPY_ERROR = -1,
    LOOPY_INVALID = -2,
    LOOPY_TIMEOUT = -3,
    LOOPY_CANCELLED = -4,
    LOOPY_CLOSED = -5,
    LOOPY_WOULD_BLOCK = -6,
    LOOPY_NOMEM = -7,
    LOOPY_NOT_FOUND = -8,
    LOOPY_BUSY = -9,
    LOOPY_AGAIN = -10,
    LOOPY_EOF = -11,
} loopyStatus;
```

Use `loopyStatusString()` for human-readable error messages.

### Configuration Patterns

Complex structures use builder patterns or init functions:

```c
loopyStream *stream = loopyStreamNewTcp(loop);
loopyStreamBind(stream, "0.0.0.0", 8888);
loopyStreamListen(stream, 128, onConnection, NULL);
```

### User Data

All handles support `loopyXxxGetData()` and `loopyXxxSetData()` for callback context:

```c
loopyStreamSetData(stream, myContext);
// Later in callback:
MyContext *ctx = loopyStreamGetData(stream);
```

## Module Reference

### Core Modules

- **loopy.h** - Event loop creation, file descriptor registration, timers
- **loopyTimer.h** - High-level timer abstraction (one-shot and periodic)

### Networking

- **loopyStream.h** - Unified TCP/pipe stream abstraction with full async I/O
- **loopyNet.h** - Low-level networking utilities and helpers
- **loopyUDP.h** - UDP socket support for datagram communication
- **loopyTLS.h** - TLS/SSL encryption and secure connections
- **loopyDNS.h** - Asynchronous DNS resolution
- **loopyConnPool.h** - Connection pooling and reuse

### Filesystem

- **loopyFS.h** - Async file operations (open, read, write, stat, etc.)
- **loopyFSPoll.h** - File system polling with change notifications
- **loopyWatch.h** - Recursive directory watching
- **loopyMmap.h** - Memory-mapped file operations
- **loopyFlock.h** - File locking (blocking and non-blocking)

### IPC & Processes

- **loopyPipe.h** - Pipe creation and management
- **loopyProcess.h** - Process spawning and lifecycle
- **loopyChannel.h** - Typed message passing between processes
- **loopyPubSub.h** - Publish/subscribe for intra-process events

### Async & Threading

- **loopyAsync.h** - Thread-safe event loop wake-up from any thread
- **loopyWork.h** - Offload CPU-intensive work to thread pool
- **loopyConcurrencyPool.h** - Manage worker thread pools

### Utilities

- **loopySignal.h** - Async-signal-safe signal handling
- **loopyIdle.h** - Idle event detection
- **loopyTTY.h** - Terminal control operations
- **loopyRandom.h** - Random number generation
- **loopyRateLimit.h** - Traffic rate limiting
- **loopyMetrics.h** - Performance metrics collection
- **loopyNice.h** - Process priority control

### Advanced

- **loopyClusterRegistry.h** - Distributed coordination
- **loopyIoUringNet.h** - Direct io_uring networking (Linux only)
- **loopyIoUringFS.h** - Direct io_uring filesystem (Linux only)
- **loopySys.h** - System-level utilities

## Architecture

### Event Loop Design

```
loopyMain(loop)
  ├─ Calculate sleep time (minimum timer deadline)
  ├─ Call beforeSleep callback (custom event processing)
  ├─ Poll for events (epoll/kqueue/io_uring)
  ├─ Call afterSleep callback
  ├─ Fire timers that expired
  └─ Call file descriptor callbacks for readable/writable fds
```

### Timer Wheel

loopy uses an efficient timer wheel implementation supporting:

- O(1) insertion and deletion
- Batched timer processing
- Microsecond precision
- No timer skew accumulation

### Buffer Management

Streams use a configurable buffer allocation strategy:

- `loopyStreamReadStart()` callback allocates buffers
- Zero-copy forwarding where possible
- Efficient backpressure handling with queue sizes

## Performance Characteristics

### I/O Multiplexing

| Backend  | Best For             | Limit             |
| -------- | -------------------- | ----------------- |
| epoll    | Moderate concurrency | ~100k connections |
| io_uring | High throughput      | System memory     |
| kqueue   | Network protocols    | ~100k connections |

### Memory Usage

- Event loop: ~8KB base + per-connection overhead
- Timer: ~80 bytes each
- Stream: ~256 bytes + buffers

### Latency

- Typical event latency: <1ms
- Timer precision: 1-10ms (system dependent)
- With io_uring: Sub-microsecond overhead

## Thread Safety

- **Event loop** is single-threaded (not thread-safe)
- **Async handle** (`loopyAsync`) is thread-safe for signaling
- Use `loopyAsyncSend()` to wake loop from worker threads
- All callbacks execute on event loop thread

Safe multi-threaded pattern:

```c
// Worker thread
void *worker(void *arg) {
    loopyAsync *async = (loopyAsync *)arg;
    // Do work...
    loopyAsyncSend(async);  // Thread-safe wake-up
    return NULL;
}

// Event loop thread
void onAsyncDone(loopyLoop *l, loopyAsync *async, void *data) {
    // Handle completion in event loop context
}
```

## Error Handling Best Practices

```c
// Check function return values
if (!loopyStreamBind(stream, "127.0.0.1", 8888)) {
    const char *err = loopyStreamGetError(stream);
    fprintf(stderr, "Bind failed: %s\n", err);
    return false;
}

// Check NULL for allocations
loopyStream *stream = loopyStreamNewTcp(loop);
if (!stream) {
    fprintf(stderr, "Stream allocation failed\n");
    return false;
}

// Use loopyStatusString for common errors
loopyStatus status = someOperation();
if (status != LOOPY_OK) {
    fprintf(stderr, "Operation failed: %s\n", loopyStatusString(status));
}
```

## Debugging

### Enable Debug Logging

```bash
# Compile with debug symbols
cmake -DCMAKE_BUILD_TYPE=Debug ..
make

# Run with GDB
gdb ./myapp
lldb ./myapp
```

### Memory Leak Detection

```bash
# Use Valgrind with included suppressions
valgrind --suppressions=tools/valgrind.supp ./myapp
```

### Event Loop Inspection

```c
// Get event loop statistics
int maxFd = loopyGetMaxFd(loop);
int setSize = loopyGetSetSize(loop);
bool using_iouring = loopyUsingIoUring(loop);

printf("Max FDs: %d, Set Size: %d, io_uring: %s\n",
       maxFd, setSize, using_iouring ? "yes" : "no");
```

## Examples

See the `/examples` directory for complete, runnable examples:

- `echo_server.c` - TCP echo server demonstrating stream I/O
- `dns_lookup.c` - Async DNS resolution with hostname lookup
- `timer_example.c` - One-shot and periodic timers
- `file_watcher.c` - Filesystem change monitoring
- `pubsub_chat.c` - Interactive pub/sub messaging system
- `pubsub_network.c` - Networked P2P pub/sub with cluster discovery (IPv4/IPv6)

All examples support `--test` flag for automated testing with clean return values.
