/* loopy - An event loop
 *
 * Copyright 2016-2019 Matt Stancliff <matt@genges.com>
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
#include <stdint.h>

#include "../deps/datakit/src/timerWheel.h"

typedef enum loopyAction {
    LOOPY_ACTION_NONE = 0x0,
    LOOPY_ACTION_READ = 0x01,
    LOOPY_ACTION_WRITE = 0x02,
    LOOPY_ACTION_ALL = LOOPY_ACTION_READ | LOOPY_ACTION_WRITE
} loopyAction;

#define loopyActionIsRead(action) ((action) & LOOPY_ACTION_READ)
#define loopyActionIsWrite(action) ((action) & LOOPY_ACTION_WRITE)

/**
 * Mark functions as deprecated for removal in a future version.
 *
 * Usage:
 *   LOOPY_DEPRECATED("Use loopyNewFunction() instead")
 *   void loopyOldFunction(void);
 *
 * Supported on GCC, Clang, and MSVC. On other compilers, this is a no-op.
 */
#if defined(__GNUC__) || defined(__clang__)
#define LOOPY_DEPRECATED(msg) __attribute__((deprecated(msg)))
#elif defined(_MSC_VER)
#define LOOPY_DEPRECATED(msg) __declspec(deprecated(msg))
#else
#define LOOPY_DEPRECATED(msg)
#endif

typedef enum loopyEvents {
    LOOPY_EVENTS_FILE = 0x01,
    LOOPY_EVENTS_TIME = 0x02,
    LOOPY_EVENTS_ALL = LOOPY_EVENTS_FILE | LOOPY_EVENTS_TIME,
    LOOPY_EVENTS_NOWAIT = 0x04
} loopyEvents;

/**
 * Unified status codes used across all loopy modules.
 *
 * All loopy operations that can fail return these status codes (or module-
 * specific extensions). The pattern is:
 * - LOOPY_OK (0) = success
 * - Negative values = errors
 *
 * Module-specific enums (loopyChannelStatus, loopyDNSStatus, etc.) use these
 * same values for common cases and extend with module-specific codes in the
 * -100 to -999 range.
 */
typedef enum loopyStatus {
    LOOPY_OK = 0,           /* Operation succeeded */
    LOOPY_ERROR = -1,       /* Generic/internal error */
    LOOPY_INVALID = -2,     /* Invalid argument */
    LOOPY_TIMEOUT = -3,     /* Operation timed out */
    LOOPY_CANCELLED = -4,   /* Operation was cancelled */
    LOOPY_CLOSED = -5,      /* Resource is closed */
    LOOPY_WOULD_BLOCK = -6, /* Would block (use with non-blocking ops) */
    LOOPY_NOMEM = -7,       /* Out of memory */
    LOOPY_NOT_FOUND = -8,   /* Resource not found */
    LOOPY_BUSY = -9,        /* Resource is busy */
    LOOPY_AGAIN = -10,      /* Transient failure, try again */
    LOOPY_EOF = -11,        /* End of file/stream */

    /* Module-specific codes use -100 to -999 */
} loopyStatus;

/**
 * Get human-readable string for a status code.
 *
 * Converts a loopyStatus enum value to a human-readable string representation.
 * This function is useful for error reporting and debugging.
 *
 * @param status The status code to convert
 * @return Static string describing the status (never NULL). Returns "Unknown
 * status" for unrecognized codes. The returned string is valid for the lifetime
 *         of the program and should not be freed.
 *
 * @note This function returns a pointer to a static string and is thread-safe.
 *
 * Example:
 * @code
 * loopyStatus result = someOperation();
 * if (result != LOOPY_OK) {
 *     printf("Operation failed: %s\n", loopyStatusString(result));
 * }
 * @endcode
 */
const char *loopyStatusString(loopyStatus status);

/**
 * Platform capabilities that can be queried at runtime.
 *
 * Use loopyHasCapability() to check if a specific capability is available
 * on the current platform/build.
 */
typedef enum loopyCapability {
    /* I/O multiplexing backends */
    LOOPY_CAP_EPOLL,   /* Linux epoll */
    LOOPY_CAP_KQUEUE,  /* BSD/macOS kqueue */
    LOOPY_CAP_IOURING, /* Linux io_uring */
    LOOPY_CAP_DEVPOLL, /* Solaris /dev/poll */
    LOOPY_CAP_SELECT,  /* POSIX select (fallback) */

    /* Optional features */
    LOOPY_CAP_TLS,       /* TLS/SSL support (mbedTLS) */
    LOOPY_CAP_DNS_ASYNC, /* Async DNS resolution */
    LOOPY_CAP_FS_EVENTS, /* Native filesystem events (kqueue/inotify) */
    LOOPY_CAP_PROCESS,   /* Process spawning */
    LOOPY_CAP_SIGNALS,   /* Signal handling */
} loopyCapability;

/**
 * Check if a capability is available.
 *
 * Determines whether a specific feature or I/O backend is available on the
 * current platform and in the current build. This allows for runtime feature
 * detection to ensure code can adapt to platform capabilities.
 *
 * @param cap The capability to check (I/O backend or optional feature)
 * @return true if the capability is available on this platform/build,
 *         false if not supported
 *
 * @note This function performs no I/O and is safe to call at any time. Results
 *       are determined at compile-time and are constant for a given build.
 *       Thread-safe.
 *
 * @see loopyCapability for the list of available capabilities
 *
 * Example:
 * @code
 * if (loopyHasCapability(LOOPY_CAP_IOURING)) {
 *     printf("io_uring is available\n");
 * } else if (loopyHasCapability(LOOPY_CAP_EPOLL)) {
 *     printf("epoll is available (io_uring not found)\n");
 * }
 *
 * if (loopyHasCapability(LOOPY_CAP_TLS)) {
 *     // Safe to use TLS functions
 * }
 * @endcode
 */
bool loopyHasCapability(loopyCapability cap);

/* Opaque event loop handle - use loopyNew() to create */
typedef struct loopyLoop loopyLoop;

/* Callback types */
typedef void loopyFileCallback(loopyLoop *l, int fd, void *clientData,
                               loopyAction mask);
typedef void loopyCallback(loopyLoop *l, void *clientData);

/* Control */

/**
 * @brief Create a new event loop on the heap.
 *
 * Allocates a new loopyLoop structure on the heap and initializes it for use.
 * This is the recommended way to create an event loop for most applications.
 * The returned event loop must be freed with loopyDelete().
 *
 * @param setSize Initial capacity for file descriptor storage. Must be a
 * positive value (typically a power of 2 for efficiency). The event loop will
 * automatically grow if you register file descriptors larger than this value,
 * so it's safe to start with a small number.
 * @return Pointer to newly allocated loopyLoop, or NULL on allocation failure
 *         (out of memory). The returned pointer is never shared and has
 * exclusive ownership by the caller.
 *
 * @note The returned event loop must be explicitly freed with loopyDelete() to
 *       avoid memory leaks. Once created, the loop should only be used by a
 * single thread (not thread-safe). The loop will automatically grow its FD
 * storage as needed, so initial setSize only affects initial memory usage.
 *
 * @see loopyDelete() to free the loop
 * @see loopyInit() for stack-based allocation
 * @see loopySize() to get the size for manual allocation
 *
 * Example:
 * @code
 * loopyLoop *loop = loopyNew(1024);  // Initial capacity for 1024 FDs
 * if (!loop) {
 *     // Handle out of memory
 *     return;
 * }
 *
 * // Use the loop...
 * loopyRegisterRead(loop, fd, myCallback, userData);
 * loopyMain(loop);  // Run event loop
 *
 * // Clean up
 * loopyDelete(loop);
 * @endcode
 */
loopyLoop *loopyNew(int setSize);

/**
 * @brief Initialize an event loop allocated elsewhere (typically on the stack).
 *
 * Initializes an uninitialized loopyLoop structure for use. This is useful when
 * you want to allocate the loopyLoop structure yourself (e.g., on the stack or
 * embedded in another structure). For most cases, loopyNew() is more
 * convenient.
 *
 * The structure should be zeroed before calling this function (can be declared
 * with = {0} initialization).
 *
 * @param l Pointer to loopyLoop structure to initialize. Must not be NULL.
 *          The structure will be modified to be ready for use. Should be zeroed
 *          before first call (e.g., static or stack-allocated with = {0}).
 * @param setSize Initial capacity for file descriptor storage. Must be
 * positive. The event loop will automatically grow if needed.
 * @return true if initialization succeeds, false on error (e.g., out of memory
 *         or I/O backend initialization failure)
 *
 * @note The initialized event loop must be freed with loopyDeinit() to avoid
 *       leaks. Unlike loopyNew(), loopyInit() does not free the loopyLoop
 *       structure itself, only its internal resources. Single-threaded use
 * only.
 *
 * @see loopyDeinit() to clean up internal resources
 * @see loopyNew() for automatic heap allocation
 * @see loopySize() to get the required size for dynamic allocation
 *
 * Example:
 * @code
 * // Stack allocation
 * loopyLoop loop = {0};
 * if (!loopyInit(&loop, 1024)) {
 *     // Handle initialization failure
 *     return;
 * }
 *
 * // Use the loop...
 * loopyMain(&loop);
 *
 * // Clean up internal resources (but not the loop struct itself)
 * loopyDeinit(&loop);
 * @endcode
 */
bool loopyInit(loopyLoop *l, int setSize);

/**
 * Check if a loopyLoop has been initialized.
 *
 * Determines whether a loopyLoop structure has been successfully initialized
 * via loopyInit() or loopyNew().
 *
 * @param l Pointer to loopyLoop to check. Must not be NULL.
 * @return true if the loop has been initialized, false otherwise
 *
 * @note Thread-safe for reading. Safe to call even on partially initialized
 * loops.
 */
bool loopyInited(const loopyLoop *l);

/**
 * @brief Signal the event loop to stop.
 *
 * Sets a flag that will cause the running event loop (in loopyMain() or
 * loopyMainFdOnly()) to exit on the next iteration after completing any
 * currently firing events. This is the safe way to shut down an event loop
 * from a callback or another thread.
 *
 * Calling this multiple times is safe; the loop will still exit cleanly.
 *
 * @param l Pointer to loopyLoop to stop. Must not be NULL and must be
 * initialized.
 *
 * @note This function is safe to call from callbacks, signal handlers, and
 * other threads. The event loop will exit cleanly after the current event
 *       iteration completes. The loopyLoop structure itself remains valid after
 *       stopping and can be reused by calling loopyMain() again (which resets
 *       the stop flag).
 *
 * @see loopyMain() to run the event loop
 * @see loopyIsStopped() to check if the loop has been stopped
 *
 * Example:
 * @code
 * void myCallback(loopyLoop *loop, int fd, void *data, loopyAction mask) {
 *     // Do some work...
 *     if (shouldShutdown) {
 *         loopyStop(loop);  // Safe to call from callbacks
 *     }
 * }
 *
 * // In main:
 * loopyMain(loop);  // Runs until loopyStop() is called
 * @endcode
 */
void loopyStop(loopyLoop *l);

/**
 * Free a heap-allocated event loop.
 *
 * Frees a loopyLoop that was created with loopyNew(). This function cleans up
 * all internal resources and frees the loopyLoop structure itself. Do not use
 * on loopyLoop structures allocated on the stack or embedded in other
 * structures; use loopyDeinit() for those cases.
 *
 * @param l Pointer to loopyLoop to free, or NULL (safe no-op if NULL)
 *
 * @note Only use this for loops created with loopyNew(). For stack-allocated or
 *       embedded loops initialized with loopyInit(), use loopyDeinit() instead.
 *       After calling this function, the pointer is invalid and must not be
 * used. Safe to call multiple times on the same pointer (second call is a
 * no-op).
 *
 * @see loopyNew() to create a heap-allocated loop
 * @see loopyDeinit() for stack-allocated loops
 *
 * Example:
 * @code
 * loopyLoop *loop = loopyNew(1024);
 * // ... use loop ...
 * loopyDelete(loop);  // Cleanup and free
 * loop = NULL;        // Optional: clear pointer to avoid use-after-free
 * @endcode
 */
void loopyDelete(loopyLoop *l);

/**
 * Clean up internal resources of an event loop.
 *
 * Deinitializes a loopyLoop initialized with loopyInit(), freeing all internal
 * resources. This does NOT free the loopyLoop structure itself, making it
 * useful for stack-allocated or embedded loops. For heap-allocated loops
 * created with loopyNew(), use loopyDelete() instead.
 *
 * After this call, the loopyLoop structure is reset to all zeros and can be
 * reinitialized with loopyInit() if needed.
 *
 * @param l Pointer to loopyLoop to deinitialize, or NULL (safe no-op if NULL)
 *
 * @note Only use this for loops initialized with loopyInit() or manually
 *       allocated and initialized. For loops created with loopyNew(), use
 *       loopyDelete() instead. After calling this function, the loop is
 *       deinitialized (zeroed) and must not be used until reinitialized.
 *
 * @see loopyInit() to initialize a loop
 * @see loopyDelete() for heap-allocated loops
 *
 * Example:
 * @code
 * loopyLoop loop = {0};
 * loopyInit(&loop, 1024);
 * // ... use loop ...
 * loopyDeinit(&loop);  // Cleanup (but not the struct)
 * // loop is now all zeros and can be reinitialized or discarded
 * @endcode
 */
void loopyDeinit(loopyLoop *l);

/**
 * Resize the file descriptor storage capacity of an event loop.
 *
 * Changes the internal storage capacity for file descriptors. The event loop
 * can automatically grow when needed, so this is primarily useful for
 * pre-allocating space if you know the maximum FD value in advance.
 *
 * @param l Pointer to loopyLoop to resize. Must not be NULL and must be
 * initialized.
 * @param setSize New capacity for file descriptors. Should be >= 0. The loop
 * will only grow (shrinking would leave registered FDs out of bounds, so that's
 * not allowed).
 * @return true if resize succeeds or was a no-op (same size), false on error
 *
 * @note This function can cause reallocation of internal structures, which may
 * be expensive for large loops. The event loop automatically resizes as needed
 *       when you register new FDs, so explicit resizing is optional. Not
 * recommended to call during event processing.
 *
 * @see loopyGetSetSize() to query current capacity
 */
bool loopyResizeSetSize(loopyLoop *l, int setSize);

/* Accessors */

/**
 * Get the current file descriptor storage capacity of an event loop.
 *
 * Returns the number of file descriptors for which space is currently allocated
 * in the event loop's internal storage. This is the capacity, not the number of
 * currently registered FDs.
 *
 * @param l Pointer to loopyLoop. Must not be NULL.
 * @return Current storage capacity (setSize). Returns 0 if uninitialized.
 *
 * @note This is a constant-time O(1) accessor. Thread-safe for reading.
 *
 * @see loopyResizeSetSize() to change capacity
 * @see loopyGetMaxFd() to get the highest registered FD
 */
int loopyGetSetSize(const loopyLoop *l);

/**
 * Get the highest file descriptor currently registered in the event loop.
 *
 * Returns the largest file descriptor number that is currently registered with
 * the event loop for reading or writing. Useful for understanding the scope of
 * FDs being monitored.
 *
 * @param l Pointer to loopyLoop. Must not be NULL.
 * @return Highest registered FD number, or -1 if no FDs are registered.
 *         The return value is always < loopyGetSetSize().
 *
 * @note This is a constant-time O(1) operation implemented via a max-heap.
 *       Thread-safe for reading.
 *
 * @see loopyGetSetSize() to get total capacity
 */
int loopyGetMaxFd(const loopyLoop *l);

/**
 * Check if the event loop has been signaled to stop.
 *
 * Returns whether loopyStop() has been called on this event loop. This can be
 * useful for external code to detect if the loop is shutting down.
 *
 * @param l Pointer to loopyLoop. Must not be NULL.
 * @return true if loopyStop() has been called, false otherwise
 *
 * @note This is a constant-time O(1) accessor. Thread-safe for reading.
 *       The stop flag is reset when loopyMain() is called again.
 *
 * @see loopyStop() to signal the loop to stop
 */
bool loopyIsStopped(const loopyLoop *l);

/**
 * @brief Get the user-associated data pointer.
 *
 * Retrieves the opaque user data pointer that was previously set with
 * loopySetUserData(). Useful for associating application-specific data
 * with the event loop.
 *
 * @param l Pointer to loopyLoop. Must not be NULL.
 * @return The user data pointer, or NULL if never set (loopyNew() initializes
 *         this to NULL)
 *
 * @note This is a constant-time O(1) accessor. Thread-safe for reading if the
 *       user data pointer itself is thread-safe.
 *
 * @see loopySetUserData() to set the data pointer
 *
 * Example:
 * @code
 * struct AppContext { // Internal fields...
 *     int field1;
 *     char field2;
 * };
 *
 * loopyLoop *loop = loopyNew(1024);
 * struct AppContext *ctx = calloc(1, sizeof(*ctx));
 * loopySetUserData(loop, ctx);
 *
 * // Later in a callback:
 * void myCallback(loopyLoop *loop, int fd, void *clientData, loopyAction mask)
 * { struct AppContext *ctx = (struct AppContext *)loopyGetUserData(loop);
 *     // Use ctx...
 * }
 * @endcode
 */
void *loopyGetUserData(const loopyLoop *l);

/**
 * @brief Set the user-associated data pointer.
 *
 * Associates an arbitrary user data pointer with the event loop. This is a
 * convenient way to pass application context to callbacks without needing
 * separate lookup tables or closures.
 *
 * @param l Pointer to loopyLoop. Must not be NULL.
 * @param data Opaque user data pointer (can be NULL). This pointer is stored
 *             as-is with no interpretation by the event loop. The loop takes
 *             no ownership and will not free this pointer.
 *
 * @note This is a constant-time O(1) operation. Not thread-safe; do not call
 *       while the event loop is running in another thread. Memory ownership
 *       of the data pointer remains with the caller.
 *
 * @see loopyGetUserData() to retrieve the data pointer
 *
 * Example:
 * @code
 * loopyLoop *loop = loopyNew(1024);
 * int *myData = malloc(sizeof(int));
 * *myData = 42;
 * loopySetUserData(loop, myData);
 *
 * // In callbacks, retrieve it:
 * int value = *(int *)loopyGetUserData(loop);
 *
 * // Before cleanup:
 * free(myData);
 * loopyDelete(loop);
 * @endcode
 */
void loopySetUserData(loopyLoop *l, void *data);

/**
 * @brief Get size of loopyLoop struct for stack allocation.
 *
 * Returns the size in bytes of the loopyLoop structure, useful for manual
 * memory allocation when you want to allocate the structure yourself (on the
 * stack, in a buffer, or embedded in another structure). After allocation,
 * pass the allocated memory to loopyInit() to initialize it.
 *
 * For most uses, loopyNew() is simpler and more convenient for heap allocation.
 *
 * @return Size in bytes required for a loopyLoop struct (constant value)
 *
 * @note This is useful mainly for embedded uses or stack allocation patterns.
 *       The returned value is constant and safe to cache.
 *
 * @see loopyInit() to initialize manually allocated memory
 * @see loopyNew() for simpler heap allocation
 *
 * Example:
 * @code
 * // Manual dynamic allocation
 * void *mem = malloc(loopySize());
 * loopyLoop *loop = (loopyLoop *)mem;
 * *loop = (loopyLoop){0};  // Zero-initialize
 * if (!loopyInit(loop, 1024)) {
 *     free(mem);
 *     return;
 * }
 * // Use loop...
 * loopyDeinit(loop);
 * free(mem);
 * @endcode
 */
size_t loopySize(void);

/* Files */

/**
 * Register a file descriptor for read events.
 *
 * Registers the given file descriptor with the event loop to monitor it for
 * readability. When the FD becomes readable, the provided callback will be
 * invoked with LOOPY_ACTION_READ set in the mask.
 *
 * Multiple registrations for the same FD are allowed (for read and write
 * separately). If already registered for reading, this updates the callback.
 *
 * @param l Pointer to loopyLoop. Must not be NULL and must be initialized.
 * @param fd File descriptor to monitor. Must be >= 0 and valid. Can be any
 * valid file descriptor (socket, pipe, regular file with O_NONBLOCK, etc.).
 * @param cb Callback function invoked when fd becomes readable. Must not be
 * NULL. Will be called with the event loop pointer, fd, clientData, and a mask
 * with LOOPY_ACTION_READ (may include LOOPY_ACTION_WRITE if registered for
 * both).
 * @param clientData Opaque user data passed to the callback. Can be NULL.
 *                   The callback receives this unchanged.
 * @return true if registration succeeds, false on error (e.g., out of memory,
 *         or backend registration failure)
 *
 * @note Once registered, the FD will remain registered until explicitly
 *       unregistered with loopyUnregisterRead() or loopyUnregisterReadWrite().
 *       The callback is not thread-safe; callbacks are always invoked from
 *       the thread running loopyMain(). Memory ownership: the loop does not
 *       take ownership of clientData and will not free it.
 *
 * @see loopyUnregisterRead() to stop monitoring
 * @see loopyRegisterWrite() for write monitoring
 * @see loopyGetEvents() to query current registrations
 *
 * Example:
 * @code
 * void onSocketReadable(loopyLoop *l, int fd, void *data, loopyAction mask) {
 *     // fd is ready for reading
 *     char buf[1024];
 *     ssize_t n = read(fd, buf, sizeof(buf));
 *     // Handle data...
 * }
 *
 * int sock = socket(AF_INET, SOCK_STREAM, 0);
 * // ... connect or accept ...
 * loopyRegisterRead(loop, sock, onSocketReadable, NULL);
 * @endcode
 */
bool loopyRegisterRead(loopyLoop *l, int fd, loopyFileCallback *cb,
                       void *clientData);

/**
 * Register a file descriptor for write events.
 *
 * Registers the given file descriptor with the event loop to monitor it for
 * writability. When the FD becomes writable, the provided callback will be
 * invoked with LOOPY_ACTION_WRITE set in the mask.
 *
 * Multiple registrations for the same FD are allowed (for read and write
 * separately). If already registered for writing, this updates the callback.
 *
 * @param l Pointer to loopyLoop. Must not be NULL and must be initialized.
 * @param fd File descriptor to monitor. Must be >= 0 and valid. Can be any
 * valid file descriptor (socket, pipe, etc.).
 * @param cb Callback function invoked when fd becomes writable. Must not be
 * NULL. Will be called with the event loop pointer, fd, clientData, and a mask
 * with LOOPY_ACTION_WRITE (may include LOOPY_ACTION_READ if registered for
 * both).
 * @param clientData Opaque user data passed to the callback. Can be NULL.
 *                   The callback receives this unchanged.
 * @return true if registration succeeds, false on error (e.g., out of memory,
 *         or backend registration failure)
 *
 * @note Once registered, the FD will remain registered until explicitly
 *       unregistered with loopyUnregisterWrite() or loopyUnregisterReadWrite().
 *       The callback is not thread-safe; callbacks are always invoked from
 *       the thread running loopyMain(). Memory ownership: the loop does not
 *       take ownership of clientData and will not free it.
 *
 * @see loopyUnregisterWrite() to stop monitoring
 * @see loopyRegisterRead() for read monitoring
 * @see loopyGetEvents() to query current registrations
 *
 * Example:
 * @code
 * void onSocketWritable(loopyLoop *l, int fd, void *data, loopyAction mask) {
 *     // fd is ready for writing
 *     const char *msg = "Hello";
 *     ssize_t n = write(fd, msg, strlen(msg));
 *     // Handle result...
 * }
 *
 * int sock = socket(AF_INET, SOCK_STREAM, 0);
 * // ... connect ...
 * loopyRegisterWrite(loop, sock, onSocketWritable, NULL);
 * @endcode
 */
bool loopyRegisterWrite(loopyLoop *l, int fd, loopyFileCallback *cb,
                        void *clientData);

/**
 * Conditionally register a file descriptor for write events.
 *
 * Registers the given file descriptor for write monitoring, but only if it
 * is not already registered for writing. This is a convenience function to
 * avoid duplicate write registrations without needing to check first.
 *
 * If already registered for writing, this returns false and does nothing.
 * If not registered for writing, it registers and returns true.
 *
 * @param l Pointer to loopyLoop. Must not be NULL and must be initialized.
 * @param fd File descriptor to conditionally register. Must be >= 0 and valid.
 * @param cb Callback function invoked when fd becomes writable (if registered).
 *           Must not be NULL.
 * @param clientData Opaque user data passed to the callback. Can be NULL.
 *
 * @return true if registration succeeded (was not already registered), false if
 *         already registered for writing or on error
 *
 * @note This is useful for scenarios where multiple code paths might try to
 *       register write notifications on the same FD, and you want to avoid
 *       duplicate registrations while using different callbacks.
 *
 * @see loopyRegisterWrite() for unconditional registration
 * @see loopyGetEvents() to query current registrations
 *
 * Example:
 * @code
 * // Only register for writes if not already done
 * if (loopyRegisterWriteIfNoneExists(loop, sock, writeCallback, data)) {
 *     printf("Write monitoring enabled\n");
 * } else {
 *     printf("Already registered (or error)\n");
 * }
 * @endcode
 */
bool loopyRegisterWriteIfNoneExists(loopyLoop *l, int fd, loopyFileCallback *cb,
                                    void *clientData);

/**
 * Unregister a file descriptor from read monitoring.
 *
 * Stops the event loop from monitoring the given file descriptor for read
 * events. If the FD was also registered for writing, the write monitoring
 * continues unchanged.
 *
 * If the FD is not registered for reading, this is a no-op.
 *
 * @param l Pointer to loopyLoop. Must not be NULL and must be initialized.
 * @param fd File descriptor to unregister from read monitoring. Must be >= 0.
 *           If fd is beyond the current storage bounds, this is a no-op.
 *
 * @note Safe to call on FDs that are not registered or have already been
 *       unregistered. If you need to unregister from both read and write,
 *       use loopyUnregisterReadWrite() instead.
 *
 * @see loopyRegisterRead() to register
 * @see loopyUnregisterWrite() to unregister from writing
 * @see loopyUnregisterReadWrite() to unregister from both
 */
void loopyUnregisterRead(loopyLoop *l, int fd);

/**
 * Unregister a file descriptor from write monitoring.
 *
 * Stops the event loop from monitoring the given file descriptor for write
 * events. If the FD was also registered for reading, the read monitoring
 * continues unchanged.
 *
 * If the FD is not registered for writing, this is a no-op.
 *
 * @param l Pointer to loopyLoop. Must not be NULL and must be initialized.
 * @param fd File descriptor to unregister from write monitoring. Must be >= 0.
 *           If fd is beyond the current storage bounds, this is a no-op.
 *
 * @note Safe to call on FDs that are not registered or have already been
 *       unregistered. If you need to unregister from both read and write,
 *       use loopyUnregisterReadWrite() instead.
 *
 * @see loopyRegisterWrite() to register
 * @see loopyUnregisterRead() to unregister from reading
 * @see loopyUnregisterReadWrite() to unregister from both
 */
void loopyUnregisterWrite(loopyLoop *l, int fd);

/**
 * Unregister a file descriptor from both read and write monitoring.
 *
 * Stops the event loop from monitoring the given file descriptor for both
 * read and write events. This completely deregisters the FD from the loop.
 *
 * If the FD is not registered at all, this is a no-op.
 *
 * @param l Pointer to loopyLoop. Must not be NULL and must be initialized.
 * @param fd File descriptor to unregister completely. Must be >= 0.
 *           If fd is beyond the current storage bounds, this is a no-op.
 *
 * @note Safe to call on FDs that are not registered or have already been
 *       unregistered. After this call, the FD is not monitored in any way.
 *
 * @see loopyRegisterRead() and loopyRegisterWrite() to register
 * @see loopyUnregisterRead() to unregister read only
 * @see loopyUnregisterWrite() to unregister write only
 *
 * Example:
 * @code
 * // Later, clean up the FD
 * loopyUnregisterReadWrite(loop, fd);
 * close(fd);
 * @endcode
 */
void loopyUnregisterReadWrite(loopyLoop *l, int fd);

/**
 * Query the current event registrations for a file descriptor.
 *
 * Returns which event types are currently registered for the given FD.
 * The return value is a loopyAction bitmask that can be checked with
 * loopyActionIsRead() and loopyActionIsWrite() macros.
 *
 * @param l Pointer to loopyLoop. Must not be NULL and must be initialized.
 * @param fd File descriptor to query. Must be >= 0.
 * @return Bitmask of loopyAction values (LOOPY_ACTION_READ, LOOPY_ACTION_WRITE,
 *         or both ORed together). Returns LOOPY_ACTION_NONE (0) if the FD is
 *         not registered or is beyond the storage bounds.
 *
 * @note This is a constant-time O(1) operation. Thread-safe for reading.
 *
 * @see loopyActionIsRead() macro to check for read registration
 * @see loopyActionIsWrite() macro to check for write registration
 *
 * Example:
 * @code
 * loopyAction events = loopyGetEvents(loop, fd);
 * if (loopyActionIsRead(events)) {
 *     printf("FD %d is registered for reading\n", fd);
 * }
 * if (loopyActionIsWrite(events)) {
 *     printf("FD %d is registered for writing\n", fd);
 * }
 * @endcode
 */
loopyAction loopyGetEvents(loopyLoop *l, int fd);

/* Timers - see loopyTimer.h for the high-level timer API */

#if 0
/**
 * @brief Wait for a file descriptor to become readable or writable.
 *
 * Blocks the calling thread until the specified file descriptor becomes
 * readable, writable, or the timeout expires. This is a synchronous,
 * blocking operation useful for simple cases where you don't need a full
 * event loop. For most applications, using loopyMain() with a full event
 * loop is recommended.
 *
 * @param fd File descriptor to wait on. Must be a valid file descriptor (>= 0).
 * @param mask Bitmask specifying which events to wait for. Use LOOPY_ACTION_READ,
 *             LOOPY_ACTION_WRITE, or both (LOOPY_ACTION_ALL) to wait for either.
 * @param milliseconds Timeout in milliseconds. Use 0 for non-blocking check,
 *                     or a large value to wait indefinitely.
 * @return Bitmask of loopyAction indicating which events occurred:
 *         LOOPY_ACTION_READ if FD became readable, LOOPY_ACTION_WRITE if
 *         writable, or both if both became ready. Returns 0 on timeout,
 *         returns negative value on error.
 *
 * @note This function blocks the calling thread. It is not suitable for
 *       multi-event scenarios and does not integrate with the event loop.
 *       For most use cases, register the FD with loopyRegisterRead() or
 *       loopyRegisterWrite() and use loopyMain() instead. This function
 *       is primarily for blocking on a single file descriptor in simple
 *       synchronous code.
 *
 * @see loopyRegisterRead() for non-blocking monitoring
 * @see loopyRegisterWrite() for write event monitoring
 * @see loopyMain() for full event loop processing
 *
 * Example:
 * @code
 * // Wait for socket to become readable with 5-second timeout
 * int result = loopyWait(sock, LOOPY_ACTION_READ, 5000);
 * if (result & LOOPY_ACTION_READ) {
 *     // Socket is readable, safe to read
 *     char buf[1024];
 *     ssize_t n = read(sock, buf, sizeof(buf));
 * } else if (result == 0) {
 *     printf("Timeout waiting for socket\n");
 * } else {
 *     printf("Error: %d\n", result);
 * }
 * @endcode
 */
/* Wait for any generic 'fd' up to 'milliseconds' */
int loopyWait(int fd, loopyAction mask, uint64_t milliseconds);
#endif

/* Start event loop */

/**
 * @brief Run the main event loop, processing file and timer events.
 *
 * Enters the main event processing loop that monitors file descriptors and
 * timers for activity. The loop continues until loopyStop() is called.
 *
 * In each iteration:
 * 1. Calls the "before sleep" callback (if set)
 * 2. Polls for file descriptor events and processes timer events
 * 3. Invokes registered callbacks for fired events
 * 4. Calls the "after sleep" callback (if set)
 *
 * File descriptor callbacks are invoked when their monitored FDs become
 * readable/writable. Timer callbacks are invoked when their timeouts expire.
 * The loop waits indefinitely for the next event (blocks if no immediate
 * events).
 *
 * @param l Pointer to loopyLoop to run. Must not be NULL and must be
 * initialized.
 *
 * @note Single-threaded: this function blocks and must only be called from one
 *       thread. Calling loopyMain() again after loopyStop() resets the stop
 *       flag and runs the loop again. The loop can only be stopped via
 *       loopyStop(), which is safe to call from callbacks, signal handlers, or
 *       other threads. All callbacks are invoked from the thread running
 *       loopyMain(). Do not register/unregister FDs while loopyMain() is
 *       running from a different thread.
 *
 * @see loopyMainFdOnly() to run without timer events
 * @see loopyStop() to signal the loop to exit
 * @see loopySetBeforeSleepCallback() and loopySetAfterSleepCallback()
 *
 * Example:
 * @code
 * loopyLoop *loop = loopyNew(1024);
 *
 * // Register some file descriptors...
 * loopyRegisterRead(loop, sock, onReadable, NULL);
 *
 * // Register some timers...
 * uint64_t timerId = loopyRegisterTimer(loop, 1000000, 0, onTimer, NULL);
 *
 * // Set up shutdown callback...
 * void checkShutdown(loopyLoop *l, void *data) {
 *     if (shouldShutdown) {
 *         loopyStop(l);
 *     }
 * }
 * loopySetBeforeSleepCallback(loop, checkShutdown, NULL);
 *
 * // Run the loop - blocks until loopyStop() is called
 * loopyMain(loop);
 *
 * // Loop has exited, clean up
 * loopyDelete(loop);
 * @endcode
 */
void loopyMain(loopyLoop *l);

/**
 * Run the event loop processing only file descriptor events (no timers).
 *
 * Like loopyMain(), but skips timer event processing. Only file descriptor
 * events are monitored and callbacks invoked. This is useful if you want to
 * process timers separately or don't need timer functionality.
 *
 * @param l Pointer to loopyLoop to run. Must not be NULL and must be
 * initialized.
 *
 * @note Single-threaded: See loopyMain() for threading notes. The before/after
 *       sleep callbacks are still invoked if set.
 *
 * @see loopyMain() for full event loop processing
 * @see loopySetBeforeSleepCallback() and loopySetAfterSleepCallback()
 */
void loopyMainFdOnly(loopyLoop *l);

/**
 * Set a callback to be invoked before the event loop blocks for I/O.
 *
 * Registers a callback function that will be invoked at the start of each
 * event loop iteration, just before the loop blocks waiting for I/O events.
 * This is useful for housekeeping tasks, checking for shutdown conditions,
 * or other periodic work.
 *
 * If the callback calls loopyStop(), the loop will exit cleanly.
 *
 * @param l Pointer to loopyLoop. Must not be NULL and must be initialized.
 * @param cb Callback function to invoke before sleeping, or NULL to clear
 *           any existing callback
 * @param clientData Opaque user data passed to callback. Can be NULL.
 *
 * @note This callback is always invoked from the thread running loopyMain().
 *       If the callback calls loopyStop(), loopyMain() will return. The loop
 *       does not take ownership of clientData and will not free it.
 *
 * @see loopySetAfterSleepCallback() for post-sleep callbacks
 * @see loopyStop() to signal shutdown from callback
 *
 * Example:
 * @code
 * void beforeSleep(loopyLoop *l, void *data) {
 *     // Check if we should shut down
 *     if (*(int *)data > 10) {
 *         loopyStop(l);
 *     }
 * }
 *
 * int counter = 0;
 * loopySetBeforeSleepCallback(loop, beforeSleep, &counter);
 * @endcode
 */
void loopySetBeforeSleepCallback(struct loopyLoop *l, loopyCallback *cb,
                                 void *clientData);

/**
 * Set a callback to be invoked after the event loop returns from blocking.
 *
 * Registers a callback function that will be invoked after the loop returns
 * from blocking on I/O, but before processing the fired events. This is useful
 * for checking for async signals, periodic work, or cleanup.
 *
 * @param l Pointer to loopyLoop. Must not be NULL and must be initialized.
 * @param cb Callback function to invoke after sleeping, or NULL to clear
 *           any existing callback
 * @param clientData Opaque user data passed to callback. Can be NULL.
 *
 * @note This callback is always invoked from the thread running loopyMain(),
 *       but only if the loop has not been stopped (loopyStop() was not called).
 *       The loop does not take ownership of clientData and will not free it.
 *
 * @see loopySetBeforeSleepCallback() for pre-sleep callbacks
 *
 * Example:
 * @code
 * void afterSleep(loopyLoop *l, void *data) {
 *     printf("Event loop woke up\n");
 * }
 *
 * loopySetAfterSleepCallback(loop, afterSleep, NULL);
 * @endcode
 */
void loopySetAfterSleepCallback(struct loopyLoop *l, loopyCallback *cb,
                                void *clientData);

/* Metadata */

/**
 * Get the name of the currently active I/O multiplexing backend.
 *
 * Returns a human-readable string identifying which I/O multiplexing
 * mechanism is being used by the event loop implementation. Common values
 * include "epoll" (Linux), "kqueue" (BSD/macOS), "io_uring" (Linux with
 * io_uring support), "/dev/poll" (Solaris), and "select" (fallback).
 *
 * @return Static string with the adapter name (e.g., "epoll", "kqueue").
 *         Never NULL. The string is valid for the lifetime of the program.
 *
 * @note This function performs no I/O and is thread-safe. The returned
 *       adapter name is fixed for a given build and platform.
 *
 * @see loopyHasCapability() to check if a specific backend is available
 * @see loopyUsingIoUring() to check if io_uring specifically is active
 *
 * Example:
 * @code
 * printf("Using I/O backend: %s\n", loopyAdapterName());
 * // Output might be: Using I/O backend: epoll
 * @endcode
 */
const char *loopyAdapterName(void);

/**
 * @brief Check if io_uring backend is being used.
 *
 * Determines whether the event loop is currently using the io_uring
 * I/O multiplexing backend. This is only relevant on Linux systems with
 * io_uring support. On Linux without io_uring, this will return false
 * (indicating epoll or select fallback is used). On non-Linux platforms,
 * this always returns false.
 *
 * This is useful for code that needs to know whether to use certain
 * io_uring-specific optimizations or has io_uring-specific behavior.
 *
 * @param l Event loop to check. Must not be NULL and must be initialized.
 * @return true if using io_uring backend, false if using epoll, kqueue,
 *         /dev/poll, select, or on non-Linux platforms
 *
 * @note This function performs no I/O and is thread-safe. The result is
 *       fixed for the lifetime of the loopyLoop and depends on the build
 *       configuration and kernel support.
 *
 * @see loopyAdapterName() to get the full backend name
 * @see loopyHasCapability() to check for capability availability
 *
 * Example:
 * @code
 * loopyLoop *loop = loopyNew(1024);
 *
 * if (loopyUsingIoUring(loop)) {
 *     printf("io_uring is active - can use advanced features\n");
 * } else {
 *     printf("Using fallback backend: %s\n", loopyAdapterName());
 * }
 * @endcode
 */
bool loopyUsingIoUring(const loopyLoop *l);

/* Originally, before updating to modern style, standards, and architectures:
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
