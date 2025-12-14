# Loopy Platform Support

Loopy is a high-performance, cross-platform event loop library for C that provides the best available I/O multiplexing mechanism for each supported platform. This document describes the platforms supported, their capabilities, and how to build for each one.

## Table of Contents

1. [Supported Platforms](#supported-platforms)
2. [Adapter Selection](#adapter-selection)
3. [Feature Availability Matrix](#feature-availability-matrix)
4. [Building for Each Platform](#building-for-each-platform)
5. [Runtime Capability Detection](#runtime-capability-detection)
6. [Performance Notes](#performance-notes)

## Supported Platforms

Loopy automatically selects the best available event multiplexing mechanism for your platform. The following platforms are fully supported:

### Linux

#### epoll (default for kernel 2.6+)

- **Minimum Kernel Version**: Linux 2.6+
- **Mechanism**: epoll(2) system call
- **Characteristics**: Efficient edge/level-triggered notifications, scales well to thousands of file descriptors
- **Default**: Yes, automatically selected on all Linux systems

#### io_uring (optional, Linux 5.1+)

- **Minimum Kernel Version**: Linux 5.1+
- **Mechanism**: io_uring(7) asynchronous I/O
- **Characteristics**: Higher performance than epoll with lower CPU overhead, supports complex I/O operations
- **Default**: No, but automatically enabled at runtime if available
- **Fallback**: Automatically falls back to epoll if io_uring initialization fails
- **Notes**:
  - Requires kernel support for io_uring system calls
  - Optional liburing dependency can be avoided with built-in syscall wrappers
  - Provides additional capabilities for file operations beyond basic I/O multiplexing

### BSD Variants

#### kqueue (macOS, FreeBSD, OpenBSD, NetBSD)

- **Platforms**: macOS, FreeBSD, OpenBSD, NetBSD
- **Mechanism**: kqueue(2) / kevent(2) system calls
- **Characteristics**:
  - Unified event notification system for I/O, timers, signals, and more
  - Edge-triggered notifications
  - Scales efficiently to thousands of events
  - Lowest CPU overhead of all adapters
- **Performance**: Often fastest adapter due to minimal system overhead

### Solaris / illumos

#### devpoll / event ports

- **Platforms**: Solaris 10+, illumos-based systems
- **Mechanism**: port(3C) event notification interface
- **Characteristics**:
  - Level-triggered notifications
  - Efficient for large numbers of file descriptors
  - Used since Solaris 10

### Generic Fallback

#### select() (all POSIX systems)

- **Mechanism**: select(2) system call
- **Characteristics**:
  - Maximum 1024 file descriptors per process (FD_SETSIZE)
  - Limited scalability
  - Highest CPU overhead of all adapters
- **Usage**: Fallback for unsupported platforms or when other mechanisms fail
- **Note**: Automatically used on systems without platform-specific optimizations

## Adapter Selection

Loopy uses compile-time detection to select the appropriate adapter for your platform:

```
Platform Detection -> Adapter Selection -> Compile-time Linkage
```

### Automatic Selection Process

1. **Compile-time Selection**: The build system detects your platform and automatically compiles the appropriate adapter
2. **Runtime Detection** (Linux only): If compiled with io_uring support, loopy attempts to initialize io_uring at runtime
3. **Graceful Fallback** (Linux): If io_uring initialization fails, automatically falls back to epoll
4. **Single Adapter Per Binary**: Each compiled binary uses exactly one adapter (except Linux with optional io_uring fallback)

### Selection Priority by Platform

**Linux**:

- Primary: io_uring (if kernel 5.1+ and enabled at build time)
- Secondary: epoll (kernel 2.6+)
- Fallback: select()

**macOS/FreeBSD/OpenBSD/NetBSD**:

- Primary: kqueue
- Fallback: select()

**Solaris/illumos**:

- Primary: devpoll (event ports)
- Fallback: select()

**Other POSIX systems**:

- Primary: select()

## Feature Availability Matrix

The following table shows which features are available on each platform and adapter:

| Feature                      | Linux epoll | Linux io_uring | macOS kqueue | FreeBSD kqueue | OpenBSD kqueue | Solaris devpoll | Generic select       |
| ---------------------------- | ----------- | -------------- | ------------ | -------------- | -------------- | --------------- | -------------------- |
| **Core I/O**                 |             |                |              |                |                |                 |                      |
| Read events                  | ✓           | ✓              | ✓            | ✓              | ✓              | ✓               | ✓                    |
| Write events                 | ✓           | ✓              | ✓            | ✓              | ✓              | ✓               | ✓                    |
| Thousands of FDs             | ✓           | ✓              | ✓            | ✓              | ✓              | ✓               | ✗ (limited to ~1024) |
| **File Operations**          |             |                |              |                |                |                 |                      |
| File watching (inotify)      | ✓           | ✓              | ✗            | ✗              | ✗              | ✗               | ✗                    |
| File watching (kqueue)       | ✗           | ✗              | ✓            | ✓              | ✓              | ✗               | ✗                    |
| File watching (generic poll) | ✓           | ✓              | ✓            | ✓              | ✓              | ✓               | ✓                    |
| io_uring FS operations       | ✗           | ✓              | ✗            | ✗              | ✗              | ✗               | ✗                    |
| Async file I/O               | ✗           | ✓              | ✗            | ✗              | ✗              | ✗               | ✗                    |
| **Networking**               |             |                |              |                |                |                 |                      |
| TCP/UDP                      | ✓           | ✓              | ✓            | ✓              | ✓              | ✓               | ✓                    |
| TLS/SSL                      | ✓           | ✓              | ✓            | ✓              | ✓              | ✓               | ✓                    |
| io_uring NET operations      | ✗           | ✓              | ✗            | ✗              | ✗              | ✗               | ✗                    |
| **Process Management**       |             |                |              |                |                |                 |                      |
| Process spawning             | ✓           | ✓              | ✓            | ✓              | ✓              | ✓               | ✓                    |
| Signal handling              | ✓           | ✓              | ✓            | ✓              | ✓              | ✓               | ✓                    |
| Process watching (kqueue)    | ✗           | ✗              | ✓            | ✓              | ✓              | ✗               | ✗                    |
| **Advanced Features**        |             |                |              |                |                |                 |                      |
| Timer wheel                  | ✓           | ✓              | ✓            | ✓              | ✓              | ✓               | ✓                    |
| Metrics collection           | ✓           | ✓              | ✓            | ✓              | ✓              | ✓               | ✓                    |
| Cluster registry             | ✓           | ✓              | ✓            | ✓              | ✓              | ✓               | ✓                    |
| Rate limiting                | ✓           | ✓              | ✓            | ✓              | ✓              | ✓               | ✓                    |

### Feature Details

#### File Watching

**inotify** (Linux): Provides efficient file/directory monitoring via the inotify mechanism. Available through `loopyFSPoll` which automatically uses inotify on Linux.

**kqueue** (BSD): FreeBSD, OpenBSD, NetBSD, and macOS support file watching through EVFILT_VNODE in kqueue.

**Generic Polling** (All): All platforms support file change detection through periodic polling fallback.

#### io_uring Specific Features

When io_uring is available (Linux 5.1+), loopy provides:

- Asynchronous file operations (`loopyIoUringFS.h`)
- Asynchronous network operations (`loopyIoUringNet.h`)
- Zero-copy operations where available
- Batch processing of multiple operations
- Automatic fallback to traditional epoll for basic I/O

#### TLS/SSL Support

TLS/SSL support requires OpenSSL and is available on all platforms where OpenSSL can be compiled. The `loopyTLS` module provides both blocking and non-blocking TLS operations compatible with all event loop adapters.

## Building for Each Platform

### Linux (epoll)

```bash
cd /path/to/loopy
mkdir build && cd build
cmake ..
make
```

This will automatically detect your kernel version and compile with epoll support.

### Linux (with io_uring support)

```bash
cd /path/to/loopy
mkdir build && cd build
cmake .. -DENABLE_IO_URING=ON
make
```

This enables io_uring support if your kernel is 5.1+. The library will automatically attempt to use io_uring at runtime and fall back to epoll if unavailable.

### macOS (kqueue)

```bash
cd /path/to/loopy
mkdir build && cd build
cmake ..
make
```

Kqueue is available by default on all macOS versions.

### FreeBSD (kqueue)

```bash
cd /path/to/loopy
mkdir build && cd build
cmake ..
make
```

Kqueue is available by default on all supported FreeBSD versions.

### OpenBSD / NetBSD (kqueue)

```bash
cd /path/to/loopy
mkdir build && cd build
cmake ..
make
```

Kqueue is available by default on all supported OpenBSD and NetBSD versions.

### Solaris / illumos (devpoll)

```bash
cd /path/to/loopy
mkdir build && cd build
cmake ..
make
```

Event ports are available by default on Solaris 10+ and illumos-based systems.

### Generic Fallback

If your platform is not automatically detected, loopy will compile with the generic select() adapter:

```bash
cd /path/to/loopy
mkdir build && cd build
cmake ..
make
```

This will work on any POSIX system but has performance limitations (max ~1024 file descriptors).

### Building with TLS Support

To build with TLS/SSL support on any platform:

```bash
cd /path/to/loopy
mkdir build && cd build
cmake .. -DENABLE_TLS=ON
make
```

This requires OpenSSL development libraries to be installed:

**Ubuntu/Debian**: `sudo apt-get install libssl-dev`
**macOS**: `brew install openssl`
**FreeBSD**: `sudo pkg install openssl`
**Solaris**: `pkg install library/security/openssl`

## Runtime Capability Detection

### Querying the Active Adapter

At runtime, you can determine which adapter is being used:

```c
#include "loopy.h"

int main() {
    /* Create event loop with capacity for 1024 file descriptors.
     * This is independent of which platform adapter is used. */
    loopyLoop *l = loopyNew(1024);

    // Get the name of the active adapter
    const char *adapter = loopyAdapterName();
    printf("Using event adapter: %s\n", adapter);

    // Check if io_uring is being used (Linux only)
    bool using_io_uring = loopyUsingIoUring(l);
    printf("io_uring enabled: %s\n", using_io_uring ? "yes" : "no");

    loopyDelete(l);
    return 0;
}
```

### Return Values

**`loopyAdapterName()`**: Returns a string with the active adapter name:

- Linux: "epoll" or "io_uring" (if available)
- BSD: "kqueue"
- Solaris: "evport"
- Fallback: "select"

**`loopyUsingIoUring(const loopyLoop *l)`**: Returns true if io_uring is being used, false otherwise. Always returns false on non-Linux platforms.

## Performance Notes

### Adapter Performance Characteristics

#### Linux io_uring

- **Throughput**: Highest on Linux 5.1+
- **Latency**: Lowest with batch processing
- **CPU Usage**: Lowest of all adapters
- **Scalability**: Scales to 100,000+ concurrent connections
- **When to use**: Maximum performance scenarios, high-concurrency applications

#### Linux epoll

- **Throughput**: Very good, well-established
- **Latency**: Predictable, sub-millisecond
- **CPU Usage**: Low with proper event masking
- **Scalability**: Scales to 100,000+ concurrent connections
- **When to use**: Wide compatibility, predictable performance

#### BSD kqueue

- **Throughput**: Very good
- **Latency**: Lowest CPU overhead
- **CPU Usage**: Minimal
- **Scalability**: Scales efficiently to 10,000+ connections
- **When to use**: BSD systems, maximizing power efficiency

#### Solaris devpoll

- **Throughput**: Good
- **Latency**: Predictable
- **CPU Usage**: Low
- **Scalability**: Scales to 10,000+ connections
- **When to use**: Solaris/illumos production systems

#### Generic select()

- **Throughput**: Poor with large numbers of file descriptors
- **Latency**: Variable, can spike with many connections
- **CPU Usage**: Very high (O(n) per poll)
- **Scalability**: Limited to ~1024 file descriptors
- **When to use**: Compatibility only, legacy systems without other options

### Optimization Tips

1. **Use platform-native mechanisms**: io_uring on Linux 5.1+, kqueue on BSD
2. **Monitor with loopyMetrics**: Use the metrics module to track event loop health
3. **Profile your workload**: Different adapters may perform better for different I/O patterns
4. **Tune buffer sizes**: Adjust loopy's internal event buffer size based on your connection count
5. **Use batching**: io_uring's batch operations provide best performance for bulk I/O

### Benchmarking

To benchmark different adapters:

```bash
# Compile the stress test
cd build
make

# Run stress tests
./loopyStressTest --help
```

The stress test provides comparative metrics across different load scenarios.

## Troubleshooting

### io_uring Not Initializing (Linux)

If you compiled with io_uring support but the loop falls back to epoll:

1. **Check kernel version**: `uname -r` must be 5.1+
2. **Check system limits**: Verify `/proc/sys/kernel/io_uring_max_requests`
3. **Verify permissions**: Some containers/VMs restrict io_uring access
4. **Use loopyUsingIoUring()**: At runtime, verify which adapter is active

### File Descriptor Limit

For select() adapter:

- Limited to FD_SETSIZE (~1024)
- Increase with platform-specific methods (not recommended)
- Switch to epoll/kqueue/io_uring instead

For other adapters:

- Increase system limits: `ulimit -n` (per-process)
- Modify `/etc/security/limits.conf` (system-wide on Linux)

### Performance Issues

1. **Use loopyMetrics**: Check event loop metrics with `loopyMetrics` module
2. **Profile**: Use platform-specific profilers (perf on Linux, Instruments on macOS)
3. **Check event distribution**: Unbalanced event patterns affect performance differently per adapter
4. **Review callback efficiency**: Long-running callbacks block the event loop

## Adapter Implementation Details

- **Linux epoll**: `src/loopyAdapterLinux.c`
- **Linux io_uring**: `src/loopyAdapterIouring.c`
- **BSD kqueue**: `src/loopyAdapterBSD.c`
- **Solaris devpoll**: `src/loopyAdapterSolaris.c`
- **Generic select**: `src/loopyAdapterGeneric.c`

## Further Reading

- Linux epoll: `man epoll` or https://man7.org/linux/man-pages/man7/epoll.7.html
- Linux io_uring: https://kernel.dk/io_uring.pdf
- BSD kqueue: `man kqueue` or https://man.freebsd.org/cgi/man.cgi?kqueue
- Solaris event ports: https://docs.oracle.com/cd/E86824_01/html/E54761/port-3c.html
