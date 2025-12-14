# Loopy C Event Loop Library - API Conventions

This document describes the consistent API design patterns used throughout the loopy event loop library. Following these conventions makes the library intuitive and predictable for users.

## Table of Contents

1. [Naming Conventions](#naming-conventions)
2. [Callback Naming](#callback-naming)
3. [Parameter Ordering](#parameter-ordering)
4. [Return Value Patterns](#return-value-patterns)
5. [Memory Ownership Rules](#memory-ownership-rules)
6. [Thread Safety Annotations](#thread-safety-annotations)
7. [Configuration Pattern](#configuration-pattern)
8. [Error Handling](#error-handling)

---

## Naming Conventions

### Function Categories

#### Allocators: `loopy<Module>New()` or `loopy<Module>New<Variant>()`

Functions that create and return new instances follow the pattern `loopy<Module>New()`. For modules with multiple creation paths, variants include a suffix describing the type.

**Examples:**

```c
/* Basic allocators */
loopyTimer *loopyTimerOneShot(loopyLoop *loop, uint64_t delayUs,
                              loopyTimerCallback *callback, void *userData);
loopyTimer *loopyTimerPeriodic(loopyLoop *loop, uint64_t intervalUs,
                               loopyTimerCallback *callback, void *userData);

/* Stream allocators with variants */
loopyStream *loopyStreamNewTcp(loopyLoop *loop);
loopyStream *loopyStreamNewPipe(loopyLoop *loop);
loopyStream *loopyStreamFromFd(loopyLoop *loop, int fd, loopyStreamType type);

/* Channel allocators */
loopyChannel *loopyChannelNew(loopyLoop *loop, const loopyChannelConfig *config);

/* DNS resolver allocators */
loopyDNS *loopyDNSNew(loopyLoop *loop, const loopyDNSConfig *config);
```

**Key characteristics:**

- Return pointer to the allocated object
- Return NULL on allocation failure
- Parameter order: event loop first, then configuration/parameters

#### Deallocators: `loopy<Module>Free()`

Functions that deallocate and free resources follow the pattern `loopy<Module>Free()`.

**Examples:**

```c
void loopyTimerCancel(loopyTimer *timer);        /* Frees timer */
void loopyChannelFree(loopyChannel *ch);          /* Frees channel */
void loopyDNSFree(loopyDNS *dns);                 /* Frees DNS resolver */
void loopyStreamClose(loopyStream *stream, loopyStreamCloseCallback *cb,
                      void *userData);            /* Closes and frees stream */
```

**Key characteristics:**

- Always return `void`
- Safe to call on NULL pointers (no-op)
- May accept optional callback for deferred cleanup
- After calling, the handle is invalid and must not be used

#### Getters: `loopy<Module>Get<Property>()`

Functions that retrieve data without modification follow the pattern `loopy<Module>Get<Property>()`.

**Examples:**

```c
/* Simple getters returning values */
void *loopyTimerGetData(const loopyTimer *timer);
int loopyStreamGetFd(const loopyStream *stream);
size_t loopyChannelLen(const loopyChannel *ch);
loopyStreamType loopyStreamGetType(const loopyStream *stream);

/* Getters with output parameters */
bool loopyStreamGetSockName(loopyStream *stream, char *addr, size_t addrLen,
                            int *port);
bool loopyStreamGetPeerName(loopyStream *stream, char *addr, size_t addrLen,
                            int *port);
```

**Key characteristics:**

- Take `const` handle when not modifying state
- Return value directly when possible
- Use output parameters for complex data
- Return `bool` or status code for operations that can fail

#### Setters: `loopy<Module>Set<Property>()`

Functions that modify state without side effects follow the pattern `loopy<Module>Set<Property>()`.

**Examples:**

```c
bool loopyTimerSetData(loopyTimer *timer, void *userData);
void loopyStreamSetData(loopyStream *stream, void *data);
void loopyChannelSetData(loopyChannel *ch, void *data);
void loopyDNSSetData(loopyDNS *dns, void *data);
```

**Key characteristics:**

- Return `bool` if the operation can fail
- Return `void` if guaranteed to succeed
- Modify the handle's state but don't deallocate

#### Predicates: `loopy<Module>Is<Condition>()`

Functions that test boolean conditions follow the pattern `loopy<Module>Is<Condition>()`.

**Examples:**

```c
bool loopyTimerIsActive(const loopyTimer *timer);
bool loopyStreamIsReading(const loopyStream *stream);
bool loopyStreamIsReadable(const loopyStream *stream);
bool loopyStreamIsWritable(const loopyStream *stream);
bool loopyChannelIsClosed(const loopyChannel *ch);
bool loopyChannelIsFull(const loopyChannel *ch);
bool loopyChannelIsEmpty(const loopyChannel *ch);
bool loopyStreamCanPassFd(const loopyStream *stream);
```

**Key characteristics:**

- Always return `bool`
- Never take parameters beyond the handle
- Efficient checks on handle state
- Take `const` handle

#### Actions: `loopy<Module><Verb>()`

Functions that perform actions follow the pattern `loopy<Module><Verb>()` where `<Verb>` is in imperative form: Start, Stop, Send, Cancel, Connect, Bind, Listen, etc.

**Examples:**

```c
/* Start/Stop operations */
bool loopyStreamReadStart(loopyStream *stream, loopyStreamAllocCallback *alloc,
                          loopyStreamReadCallback *read, void *userData);
void loopyStreamReadStop(loopyStream *stream);

/* I/O operations */
bool loopyStreamWrite(loopyStream *stream, const void *data, size_t len,
                      loopyStreamWriteCallback *cb, void *userData);
ssize_t loopyStreamTryWrite(loopyStream *stream, const void *data, size_t len);

/* Connection operations */
bool loopyStreamConnect(loopyStream *stream, const char *addr, int port,
                        loopyStreamWriteCallback *cb, void *userData);
bool loopyStreamBind(loopyStream *stream, const char *addr, int port);
bool loopyStreamListen(loopyStream *stream, int backlog,
                       loopyStreamConnectionCallback *cb, void *userData);
loopyStream *loopyStreamAccept(loopyStream *server);

/* Channel operations */
loopyChannelStatus loopyChannelSend(loopyChannel *ch, const void *data,
                                    size_t len);
ssize_t loopyChannelRecv(loopyChannel *ch, void *buf, size_t bufLen);

/* Async versions */
bool loopyChannelSendAsync(loopyChannel *ch, const void *data, size_t len,
                           loopyChannelSendCallback *cb, void *userData);
```

**Key characteristics:**

- Reflect the operation being performed
- Return appropriate status (bool for simple success/failure, status enum for complex results)
- Can be synchronous or asynchronous
- May accept callbacks for async operations

---

## Callback Naming

### Callback Suffix Conventions

#### `*Callback` Suffix for Async Event/Completion Notifications

Callbacks that are invoked asynchronously when an event occurs or a long-running operation completes use the `*Callback` suffix.

**Examples:**

```c
/* Timer callback - called when timer expires */
typedef void loopyTimerCallback(struct loopyLoop *loop, loopyTimer *timer,
                                void *userData);

/* Stream callbacks - called when I/O operations complete */
typedef void loopyStreamReadCallback(loopyStream *stream, ssize_t nread,
                                     const void *buf, void *userData);
typedef void loopyStreamWriteCallback(loopyStream *stream, int status,
                                      void *userData);
typedef void loopyStreamConnectionCallback(loopyStream *server, int status,
                                           void *userData);
typedef void loopyStreamShutdownCallback(loopyStream *stream, int status,
                                         void *userData);
typedef void loopyStreamCloseCallback(loopyStream *stream, void *userData);

/* Channel callbacks - called when async operations complete */
typedef void loopyChannelSendCallback(loopyChannel *ch,
                                      loopyChannelStatus status,
                                      void *userData);
typedef void loopyChannelRecvCallback(loopyChannel *ch, const void *data,
                                      size_t len, loopyChannelStatus status,
                                      void *userData);

/* DNS callback - called when query completes */
typedef void loopyDNSCallback(loopyDNS *dns, const loopyDNSResult *result);
```

**Key characteristics:**

- Called asynchronously after an event or operation
- Called on the event loop thread
- Parameters include: handle, result data, and userData
- Return type is always `void`

#### `*Fn` Suffix for Synchronous Iterators and Worker Functions

Callback functions used for synchronous iteration or worker thread patterns use the `*Fn` suffix.

**Examples:**

```c
/* Allocation callback - called before each read to allocate buffer */
typedef void loopyStreamAllocCallback(loopyStream *stream, size_t suggested,
                                      void **buf, size_t *bufLen,
                                      void *userData);

/* File descriptor passing callback */
typedef void loopyStreamReadFdCallback(loopyStream *stream, ssize_t nread,
                                       const void *buf, const int *fds,
                                       int nfds, void *userData);
```

**Key characteristics:**

- Used for synchronous operations or pre-processing
- Called during the operation flow, not asynchronously
- May set output parameters
- Return type is always `void`

### Callback Signature Pattern: `loopy<Module><Event>Callback`

Callback types follow the pattern `loopy<Module><Event>Callback` where `<Event>` describes when the callback is invoked.

**Pattern:** `loopy<Module><Event>Callback`

**Examples from the pattern:**

```c
loopyTimerCallback           /* Called when timer expires */
loopyStreamReadCallback      /* Called when data is readable */
loopyStreamWriteCallback     /* Called when write completes */
loopyStreamConnectionCallback /* Called when connection arrives */
loopyChannelSendCallback     /* Called when send completes */
loopyChannelRecvCallback     /* Called when recv completes */
loopyDNSCallback             /* Called when DNS query completes */
```

### Callback Parameter Order

Callback parameters follow a strict order for consistency:

1. **Handle/Context** (first parameter): The object that triggered the callback
   - `loopyStream *stream`, `loopyTimer *timer`, `loopyChannel *ch`, `loopyDNS *dns`
2. **Result/Status/Data** (middle parameters): The actual result of the operation
   - `ssize_t nread`, `int status`, `const void *buf`, `const loopyDNSResult *result`
3. **User Data** (last parameter): Context pointer passed when registering the callback
   - `void *userData`

**Examples:**

```c
/* Pattern: Handle, result/data, userData */
typedef void loopyTimerCallback(loopyLoop *loop, loopyTimer *timer,
                                void *userData);

typedef void loopyStreamReadCallback(loopyStream *stream, ssize_t nread,
                                     const void *buf, void *userData);

typedef void loopyChannelRecvCallback(loopyChannel *ch, const void *data,
                                      size_t len, loopyChannelStatus status,
                                      void *userData);

typedef void loopyDNSCallback(loopyDNS *dns, const loopyDNSResult *result);
                              /* Note: userData is embedded in result */
```

---

## Parameter Ordering

All functions follow a consistent parameter order to make APIs predictable and reduce cognitive load.

### Standard Parameter Order

1. **Handle/Context** (first)
   - The primary object being operated on
   - Examples: `loopyStream *stream`, `loopyTimer *timer`, `loopyChannel *ch`

2. **Required Input Parameters**
   - Parameters essential to the operation
   - Example: `const char *addr, int port` for bind operations

3. **Optional Input Parameters**
   - Configuration parameters that have sensible defaults
   - Example: `bool ipv6Only` for IPv6 bind

4. **Callback Function Pointer** (if async)
   - The callback to invoke on completion
   - Example: `loopyStreamWriteCallback *cb`

5. **User Data Pointer** (if callback exists)
   - Context to pass to callback
   - Always `void *userData`

6. **Output Parameters** (for sync functions only)
   - Pointers where results are written
   - Example: `int *port` for address queries

### Examples of Parameter Order

```c
/* Read start: handle, allocator callback, read callback, userData */
bool loopyStreamReadStart(loopyStream *stream, loopyStreamAllocCallback *alloc,
                          loopyStreamReadCallback *read, void *userData);

/* Write: handle, data, size, callback, userData */
bool loopyStreamWrite(loopyStream *stream, const void *data, size_t len,
                      loopyStreamWriteCallback *cb, void *userData);

/* Connect: handle, address, port, callback, userData */
bool loopyStreamConnect(loopyStream *stream, const char *addr, int port,
                        loopyStreamWriteCallback *cb, void *userData);

/* Get sockname: handle, output buffer, buffer size, output port */
bool loopyStreamGetSockName(loopyStream *stream, char *addr, size_t addrLen,
                            int *port);

/* Channel send: handle, data, size */
loopyChannelStatus loopyChannelSend(loopyChannel *ch, const void *data,
                                    size_t len);

/* Channel send async: handle, data, size, callback, userData */
bool loopyChannelSendAsync(loopyChannel *ch, const void *data, size_t len,
                           loopyChannelSendCallback *cb, void *userData);
```

---

## Return Value Patterns

Return values are chosen based on the type of operation and what information the caller needs.

### Allocators (New): Return Pointer

Functions that create new objects return a pointer to the allocated object.

```c
loopyTimer *loopyTimerOneShot(loopyLoop *loop, uint64_t delayUs,
                              loopyTimerCallback *callback, void *userData);
/* Returns: pointer to loopyTimer on success, NULL on failure */

loopyStream *loopyStreamNewTcp(loopyLoop *loop);
/* Returns: pointer to loopyStream on success, NULL on failure */

loopyChannel *loopyChannelNew(loopyLoop *loop,
                              const loopyChannelConfig *config);
/* Returns: pointer to loopyChannel on success, NULL on failure */
```

**Pattern:**

- Return object pointer on success
- Return NULL on failure
- Set thread-local error message for diagnostics

### Simple Operations: Return bool

Functions that perform straightforward operations return `bool` (true = success, false = failure).

```c
bool loopyStreamBind(loopyStream *stream, const char *addr, int port);
/* Returns: true on success, false on failure */

bool loopyStreamListen(loopyStream *stream, int backlog,
                       loopyStreamConnectionCallback *cb, void *userData);
/* Returns: true on success, false on failure */

bool loopyStreamReadStart(loopyStream *stream, loopyStreamAllocCallback *alloc,
                          loopyStreamReadCallback *read, void *userData);
/* Returns: true on success, false on failure */

bool loopyTimerSetData(loopyTimer *timer, void *userData);
/* Returns: true on success (timer still active), false if timer is NULL/invalid */
```

**Pattern:**

- Return `true` when operation succeeds
- Return `false` when operation fails
- Simple, predictable semantics

### Complex Operations: Return Module-Specific Status Enum

Functions with multiple failure modes return a status enum defining specific failure types.

```c
/* Channel operations with detailed status codes */
typedef enum loopyChannelStatus {
    LOOPY_CHANNEL_OK = LOOPY_OK,
    LOOPY_CHANNEL_ERROR = LOOPY_ERROR,
    LOOPY_CHANNEL_INVALID = LOOPY_INVALID,
    LOOPY_CHANNEL_TIMEOUT = LOOPY_TIMEOUT,
    LOOPY_CHANNEL_CLOSED = LOOPY_CLOSED,
    LOOPY_CHANNEL_FULL = -100,        /* Specific to channels */
    LOOPY_CHANNEL_EMPTY = -101,       /* Specific to channels */
} loopyChannelStatus;

loopyChannelStatus loopyChannelSend(loopyChannel *ch, const void *data,
                                    size_t len);
/* Returns: LOOPY_CHANNEL_OK, LOOPY_CHANNEL_FULL, LOOPY_CHANNEL_CLOSED, etc. */

loopyChannelStatus loopyChannelTrySend(loopyChannel *ch, const void *data,
                                       size_t len);
/* Returns: LOOPY_CHANNEL_OK if sent, LOOPY_CHANNEL_FULL if buffer full */

/* DNS operations with detailed status codes */
typedef enum loopyDNSStatus {
    LOOPY_DNS_OK = LOOPY_OK,
    LOOPY_DNS_ERROR = LOOPY_ERROR,
    LOOPY_DNS_TIMEOUT = LOOPY_TIMEOUT,
    LOOPY_DNS_CANCELLED = LOOPY_CANCELLED,
    LOOPY_DNS_NXDOMAIN = -200,        /* Specific to DNS */
    LOOPY_DNS_SERVFAIL = -201,        /* Specific to DNS */
} loopyDNSStatus;

/* Status is returned in the result structure */
typedef struct loopyDNSResult {
    loopyDNSStatus status;
    /* ... */
} loopyDNSResult;
```

**Pattern:**

- Use enum with success and specific failure codes
- Base codes (LOOPY_OK, LOOPY_ERROR) are shared across modules
- Module-specific codes use ranges (-100 for channel, -200 for DNS, etc.)
- Often embedded in result structures for async callbacks

### Getters: Return Value Directly

Functions that retrieve simple values return them directly.

```c
void *loopyTimerGetData(const loopyTimer *timer);
/* Returns: user data pointer directly */

int loopyStreamGetFd(const loopyStream *stream);
/* Returns: file descriptor directly */

size_t loopyChannelLen(const loopyChannel *ch);
/* Returns: number of elements in channel */

size_t loopyChannelCap(const loopyChannel *ch);
/* Returns: channel capacity */

loopyStreamType loopyStreamGetType(const loopyStream *stream);
/* Returns: stream type enum */
```

**Pattern:**

- Return the value directly
- No error indication (getter must succeed or return sentinel)
- Take `const` handle

### Receive Operations: Return ssize_t

Functions that read variable-length data return `ssize_t` (bytes transferred, 0 for EOF, negative for error).

```c
ssize_t loopyChannelRecv(loopyChannel *ch, void *buf, size_t bufLen);
/* Returns: bytes received on success, negative status code on error */

ssize_t loopyChannelTryRecv(loopyChannel *ch, void *buf, size_t bufLen);
/* Returns: bytes received, LOOPY_CHANNEL_EMPTY if no data, other errors */

ssize_t loopyStreamTryWrite(loopyStream *stream, const void *data, size_t len);
/* Returns: bytes written (>= 0), -1 on error (EAGAIN means use async write) */
```

**Pattern:**

- Returns bytes transferred on success
- Returns 0 for EOF (streams only)
- Returns negative value for error
- Error code can be checked with status enum mapping

### Void: Guaranteed-Success Operations

Functions that cannot fail return `void` to avoid unnecessary error checking.

```c
void loopyTimerCancel(loopyTimer *timer);
/* Cancels timer, safe to call on NULL, always succeeds */

void loopyStreamReadStop(loopyStream *stream);
/* Stops reading, cannot fail */

void loopyChannelClose(loopyChannel *ch);
/* Closes channel, cannot fail */

void loopyDNSCancelAll(loopyDNS *dns);
/* Cancels all queries, cannot fail */

void loopyChannelConfigInit(loopyChannelConfig *config);
/* Initializes config struct, cannot fail */
```

**Pattern:**

- Used for operations that cannot reasonably fail
- Examples: freeing/closing resources, initializing structs, stopping operations
- Reduces cognitive load by eliminating unnecessary error checking
- Still safe to call with NULL pointers where applicable

---

## Memory Ownership Rules

Clear memory ownership prevents leaks and use-after-free bugs.

### Allocators: Caller Takes Ownership

When you call a `loopy<Module>New()` function, you take ownership of the returned pointer.

```c
loopyTimer *timer = loopyTimerOneShot(loop, 5000000, callback, NULL);
if (timer == NULL) {
    /* Handle allocation failure */
}
/* You own timer - you must cancel it or let it expire */
loopyTimerCancel(timer);  /* You are responsible for cleanup */
```

**Rules:**

- Caller is responsible for freeing/cancelling the handle
- Failure to free results in resource leak
- Use-after-free if you use the handle after freeing

### Deallocators: Library Takes Ownership of Argument

When you call a `loopy<Module>Free()` function, the library takes ownership of the pointer.

```c
loopyChannel *ch = loopyChannelNew(loop, &config);
loopyChannelFree(ch);  /* Library now owns ch, you must not use it */
ch = NULL;             /* Good practice to avoid use-after-free */
```

**Rules:**

- Pass the handle to the deallocator
- Handle becomes invalid after deallocator returns
- Do not access the handle after deallocation
- Safe to call with NULL (no-op)

### Callbacks: Library Retains Ownership of Input Pointers

In callbacks, pointers provided as input parameters are owned by the library.

```c
typedef void loopyStreamReadCallback(loopyStream *stream, ssize_t nread,
                                     const void *buf, void *userData);

void my_read_callback(loopyStream *stream, ssize_t nread,
                      const void *buf, void *userData) {
    /* buf is owned by the library - only valid during callback */
    if (nread > 0) {
        /* Copy buf contents if you need to use it later */
        memcpy(my_buffer, buf, nread);
    }
    /* Do not free buf - library will handle it */
}
```

**Rules:**

- Input pointers are valid only during callback execution
- Copy data if you need to use it after callback returns
- Do not free input pointers - library owns them
- Output parameters (userData, result structures) are caller-managed

### Result Structures: Check Who Owns Cleanup

Some result structures need cleanup, others don't.

```c
/* DNS results need cleanup */
typedef struct loopyDNSResult {
    loopyDNSStatus status;
    char *hostname;             /* Owned by result, freed by loopyDNSResultFree */
    loopyDNSAddress *addresses; /* Owned by result, freed by loopyDNSResultFree */
    void *userData;             /* User data - you own this */
} loopyDNSResult;

void my_dns_callback(loopyDNS *dns, const loopyDNSResult *result) {
    /* Result is owned by library - only valid during callback */
    /* But result->userData is owned by you if you allocated it */

    if (result->status == LOOPY_DNS_OK) {
        process_addresses(result->addresses, result->addressCount);
    }

    /* Do not free result - DNS resolver will handle it */
    /* But if you allocated result->userData, free it after callback */
}
```

**Rules:**

- Check documentation for each result type
- Most results are owned by the library and freed after callback
- userData fields are typically caller-owned
- Some modules provide explicit free functions for copies

### File Descriptors: Stream Takes Ownership

When creating a stream from a file descriptor, the stream takes ownership.

```c
int fd = open("/tmp/file", O_RDONLY);
loopyStream *stream = loopyStreamFromFd(loop, fd, LOOPY_STREAM_PIPE);
/* Stream now owns fd - do not close it manually */
loopyStreamClose(stream, NULL, NULL);
/* fd is automatically closed by the library */
```

**Rules:**

- Stream takes ownership of the file descriptor
- Library closes the fd when stream is freed
- Do not close the fd manually after creating a stream from it
- If creating stream fails, you still own the fd

### File Descriptor Passing: Recipient Takes Ownership

When sending file descriptors over a stream, the recipient takes ownership.

```c
int clientFd = accept(serverFd, NULL, NULL);
loopyStreamWriteWithFd(workerPipe, "c", 1, &clientFd, 1, NULL, NULL);
close(clientFd);  /* Local copy is closed, but worker now owns the fd */
```

**Rules:**

- Sender: You retain ownership until send completes
- Recipient: Takes ownership in the callback
- Recipient must close received fds when done
- Both sender and recipient can close their copies

---

## Thread Safety Annotations

Thread safety is crucial for event loop code. Functions should document their thread safety requirements.

### Thread-Safe Everywhere

Some functions are safe to call from any thread without synchronization.

```c
/* Thread-safe: Any thread can call these */
void loopyTimerCancel(loopyTimer *timer);
bool loopyChannelSendAsync(loopyChannel *ch, const void *data, size_t len,
                           loopyChannelSendCallback *cb, void *userData);
bool loopyChannelTrySend(loopyChannel *ch, const void *data, size_t len);
loopyChannelStatus loopyChannelSend(loopyChannel *ch, const void *data,
                                    size_t len);
bool loopyStreamCanPassFd(const loopyStream *stream);
```

**Characteristics:**

- Use atomic operations or mutexes internally
- Do not require external synchronization
- Safe from concurrent calls
- Examples: async sends, try operations, predicates

### Event Loop Thread Only

Some functions must be called only from the event loop thread.

```c
/* Event loop thread only: These require event loop thread context */
loopyChannel *loopyChannelNew(loopyLoop *loop,
                              const loopyChannelConfig *config);
bool loopyChannelRecvAsync(loopyChannel *ch, loopyChannelRecvCallback *cb,
                           void *userData);
bool loopyStreamReadStart(loopyStream *stream, loopyStreamAllocCallback *alloc,
                          loopyStreamReadCallback *read, void *userData);
```

**Characteristics:**

- Must be called from the event loop's thread
- Thread affinity to event loop thread
- Non-blocking operations that may queue work
- Safe from concurrent calls on same thread

### Type-Dependent Thread Safety

Channel types have different thread safety requirements.

```c
/* SPSC Channels */
typedef enum loopyChannelType {
    LOOPY_CHANNEL_SPSC = 0,  /* Single producer, single consumer */
    LOOPY_CHANNEL_MPSC = 1,  /* Multi-producer, single consumer */
    LOOPY_CHANNEL_MPMC = 2,  /* Multi-producer, multi-consumer */
} loopyChannelType;

loopyChannelStatus loopyChannelSend(loopyChannel *ch, const void *data,
                                    size_t len);
/* Thread safety depends on channel type:
 * - SPSC: Only producer thread can send
 * - MPSC/MPMC: Any thread can send
 * - MPSC: Only consumer thread can receive
 * - MPMC: Any thread can receive
 */
```

**Characteristics:**

- Safety guarantees vary by configuration
- Documentation must specify requirements per type
- Users must respect channel type semantics

### Callbacks: Always Called on Event Loop Thread

All callbacks are invoked on the event loop thread.

```c
typedef void loopyTimerCallback(struct loopyLoop *loop, loopyTimer *timer,
                                void *userData);
/* Called on event loop thread */

typedef void loopyStreamReadCallback(loopyStream *stream, ssize_t nread,
                                     const void *buf, void *userData);
/* Called on event loop thread */

typedef void loopyChannelRecvCallback(loopyChannel *ch, const void *data,
                                      size_t len, loopyChannelStatus status,
                                      void *userData);
/* Called on event loop thread */
```

**Characteristics:**

- All callbacks are synchronized to event loop thread
- No other threads will call callbacks concurrently
- Allows non-thread-safe code in callbacks
- Callbacks must not block the event loop

### Getting Data: Mostly Safe, Some Caveats

Getter functions are generally safe but might return approximate values under contention.

```c
size_t loopyChannelLen(const loopyChannel *ch);
/* Safe from any thread, but may be approximate for MPMC under contention */

bool loopyChannelIsFull(const loopyChannel *ch);
/* Safe from any thread, but may be approximate for MPMC */

bool loopyChannelIsEmpty(const loopyChannel *ch);
/* Safe from any thread, but may be approximate for MPMC */
```

**Characteristics:**

- Generally safe from any thread
- Non-blocking operations
- May not reflect the absolute current state in concurrent scenarios
- Suitable for diagnostics and metrics, not for precise synchronization

---

## Configuration Pattern

Loopy modules use a consistent configuration pattern allowing both macro-based and function-based initialization.

### Configuration Struct

Each module defines a configuration struct with sensible defaults.

```c
typedef struct loopyChannelConfig {
    loopyChannelType type;      /* Channel type (default: LOOPY_CHANNEL_SPSC) */
    size_t capacity;            /* Buffer capacity (default: 1024) */
    size_t elementSize;         /* Size of each element (required) */
    bool blocking;              /* Block on full/empty (default: false) */
    uint64_t sendTimeoutUs;     /* Send timeout in microseconds */
    uint64_t recvTimeoutUs;     /* Receive timeout in microseconds */
} loopyChannelConfig;

typedef struct loopyDNSConfig {
    size_t maxConcurrent;       /* Max concurrent queries (default: 16) */
    uint64_t timeoutMs;         /* Query timeout in ms (default: 5000) */
    size_t workerThreads;       /* Number of worker threads (default: 2) */
} loopyDNSConfig;
```

**Key characteristics:**

- All fields have sensible defaults
- Some fields are required and must be set by caller
- Struct is passed by const pointer to allocation functions

### Default Macro: `LOOPY_<MODULE>_CONFIG_DEFAULT`

For simple use cases, a macro provides default configuration.

```c
#define LOOPY_DNS_CONFIG_DEFAULT \
    (loopyDNSConfig){ \
        .maxConcurrent = 16, \
        .timeoutMs = 5000, \
        .workerThreads = 2 \
    }

/* Usage */
loopyDNSConfig config = LOOPY_DNS_CONFIG_DEFAULT;
loopyDNS *dns = loopyDNSNew(loop, &config);
```

**Benefits:**

- Compile-time initialization
- No function call overhead
- Works in all contexts (global, local, etc.)

### Init Function: `void loopy<Module>ConfigInit()`

For explicit initialization with full flexibility.

```c
void loopyChannelConfigInit(loopyChannelConfig *config);
/* Sets all fields to defaults */

/* Usage */
loopyChannelConfig config;
loopyChannelConfigInit(&config);
config.elementSize = sizeof(struct Message);  /* Required field */
config.capacity = 256;                         /* Optional override */
loopyChannel *ch = loopyChannelNew(loop, &config);
```

**Benefits:**

- Always returns `void` (guaranteed to succeed)
- Can modify config after initialization
- Self-documenting code
- Easier refactoring if defaults change

### When to Use Each Approach

**Use macro for:**

- Static/global configuration (compile-time constant)
- Simple use cases with all defaults
- Inline initialization at allocation point

```c
loopyDNS *dns = loopyDNSNew(loop, &LOOPY_DNS_CONFIG_DEFAULT);
```

**Use init function for:**

- Dynamic configuration from runtime values
- Modifications after initialization
- Complex setup logic
- Clarity about which fields have been set

```c
loopyChannelConfig config;
loopyChannelConfigInit(&config);
config.elementSize = sizeof(MyMessage);
config.type = getChannelType();
loopyChannel *ch = loopyChannelNew(loop, &config);
```

---

## Error Handling

Error reporting follows consistent patterns across the library.

### Thread-Local Error Messages

Most functions set a thread-local error message on failure.

```c
loopyTimer *timer = loopyTimerOneShot(loop, 5000000, callback, NULL);
if (timer == NULL) {
    fprintf(stderr, "Timer creation failed: %s\n", loopyTimerGetError());
}

loopyStream *stream = loopyStreamNewTcp(loop);
if (stream == NULL) {
    fprintf(stderr, "Stream creation failed: %s\n", loopyStreamGetError());
}

loopyChannel *ch = loopyChannelNew(loop, &config);
if (ch == NULL) {
    fprintf(stderr, "Channel creation failed: %s\n",
            loopyChannelGetError());  /* If this function exists */
}
```

**Characteristics:**

- Set by allocation and configuration functions
- Retrieved via module-specific `Get*Error()` function
- Stored in thread-local storage
- Valid until next loopy function call on that thread
- Human-readable description of the failure

### Status Enums for Complex Operations

Operations with multiple failure modes return status enums.

```c
loopyChannelStatus status = loopyChannelSend(ch, &msg, sizeof(msg));
switch (status) {
    case LOOPY_CHANNEL_OK:
        /* Success */
        break;
    case LOOPY_CHANNEL_CLOSED:
        fprintf(stderr, "Channel is closed\n");
        break;
    case LOOPY_CHANNEL_FULL:
        fprintf(stderr, "Channel buffer is full\n");
        break;
    case LOOPY_CHANNEL_INVALID:
        fprintf(stderr, "Invalid argument\n");
        break;
    default:
        fprintf(stderr, "Unknown error: %d\n", status);
        break;
}
```

**Characteristics:**

- Defined as enums with clear names
- Base codes (OK, ERROR, INVALID, TIMEOUT, CLOSED) shared across modules
- Module-specific codes for domain-specific failures
- Helper functions to convert codes to strings

### Status Code Organization

Status codes follow a consistent numbering scheme.

```c
/* Base codes (global) */
#define LOOPY_OK         0      /* Operation succeeded */
#define LOOPY_ERROR     -1      /* Generic error */
#define LOOPY_INVALID   -2      /* Invalid argument */
#define LOOPY_TIMEOUT   -3      /* Operation timed out */
#define LOOPY_CLOSED    -4      /* Resource has been closed */
#define LOOPY_CANCELLED -5      /* Operation was cancelled */

/* Module-specific codes use ranges */
/* DNS: -200 to -299 */
#define LOOPY_DNS_NXDOMAIN -200  /* Domain does not exist */
#define LOOPY_DNS_SERVFAIL -201  /* Server failure */

/* Channel: -100 to -199 */
#define LOOPY_CHANNEL_FULL  -100  /* Channel is full */
#define LOOPY_CHANNEL_EMPTY -101  /* Channel is empty */
```

**Pattern:**

- Negative values indicate errors
- 0 indicates success
- Global codes are reused by all modules
- Modules reserve ranges for specific codes
- String conversion functions for diagnostics

### Helper Functions for Status Codes

Modules provide functions to convert status codes to strings.

```c
const char *loopyChannelStatusName(loopyChannelStatus status);
const char *loopyDNSStatusString(loopyDNSStatus status);

/* Usage */
loopyChannelStatus status = loopyChannelSend(ch, data, len);
if (status != LOOPY_CHANNEL_OK) {
    fprintf(stderr, "Send failed: %s\n",
            loopyChannelStatusName(status));
}
```

**Benefits:**

- Human-readable error messages
- Easier debugging and logging
- Consistent error reporting across application

---

## Summary

These conventions create a predictable, intuitive API for the loopy event loop library:

1. **Naming is self-documenting**: Function names clearly indicate their purpose and category
2. **Consistency reduces learning curve**: Once you understand one module, others follow the same patterns
3. **Parameter order is predictable**: Handle, inputs, callbacks, userData, outputs always in the same order
4. **Return values are appropriate**: Each operation returns the minimum necessary information
5. **Memory ownership is clear**: Functions and documentation specify who owns each pointer
6. **Thread safety is explicit**: Documentation clearly states requirements for each function
7. **Configuration is flexible**: Both macro and function approaches supported
8. **Error handling is consistent**: Thread-local errors + status enums across modules

By following these conventions, all functions in the loopy library are intuitive and consistent, making it easier to learn and use effectively.
