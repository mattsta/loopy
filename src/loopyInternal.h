/* loopyInternal.h - Internal implementation details for loopy
 *
 * This header is for internal use only and should not be included by
 * library users. The structures defined here may change without notice
 * between versions.
 *
 * Copyright 2016-2024 Matt Stancliff <matt@genges.com>
 *
 * Licensed under the Apache License, Version 2.0
 */

#ifndef LOOPY_INTERNAL_H
#define LOOPY_INTERNAL_H

#include "../deps/datakit/src/timerWheel.h"
#include "loopy.h"
#include "loopyMaxHeap.h"

/* ============================================================================
 * Internal Structures
 * ============================================================================
 */

/* File event structure */
typedef struct loopyFileEvent {
    loopyFileCallback *readCallback;
    loopyFileCallback *writeCallback;
    void *clientData;
    loopyAction mask;
} loopyFileEvent;

/* A fired event */
typedef struct loopyFiredEvent {
    int fd;
    uint32_t mask;
} loopyFiredEvent;

/* Forward declaration for metrics */
struct loopyMetricsInternal;

/* Event loop structure (opaque to users, defined here for internal use) */
struct loopyLoop {
    void *state;            /* internal platform adapter state */
    loopyFileEvent *events; /* array of registered events of length 'setSize' */
    loopyFiredEvent *fired; /* array of fired events of length 'setSize' */
    loopyMaxHeap fdHeap;    /* max-heap for O(1) maxfd tracking (embedded) */
    struct {
        struct {
            loopyCallback *cb;
            void *clientData;
        } before;
        struct {
            loopyCallback *cb;
            void *clientData;
        } after;
    } sleep;
    timerWheel *timer;
    struct loopyMetricsInternal *metrics; /* optional performance metrics */
    void *userData;                       /* user-provided data pointer */
    int setSize; /* max number of file descriptors tracked */
    int maxfd;   /* highest fd registered (bounds checking 'events', 'fired') */
    struct {
        int writeFd;
        int readFd;
    } managementPipe;
    int processingDepth;     /* Nesting level of event processing (deferred deletion) */
    uint8_t stop:1;          /* Event loop should stop */
    uint8_t deleting:1;      /* Loop is being deleted (use-after-free protection) */
    uint8_t pendingDelete:1; /* Deletion requested while processing (deferred) */
    uint8_t allocated:1;     /* Loop was heap-allocated (vs stack) */
};

/* ============================================================================
 * Internal API (for adapters and modules)
 * ============================================================================
 */

/**
 * Process file descriptor events that have fired.
 *
 * Internal function called by I/O adapters to notify the event loop of
 * file descriptor events that occurred. This processes the events and invokes
 * registered callbacks for each affected file descriptor.
 *
 * @param l The event loop
 * @param numevents Number of file descriptor events that have fired
 *
 * @note Internal function - called by event loop adapters only
 * @note Thread Safety: Must be called from the event loop thread
 * @note This is called after a poll/select/epoll/kqueue operation returns
 */
void loopyProcessFiredEvents(loopyLoop *l, int numevents);

/* ============================================================================
 * Internal Timer API
 *
 * Low-level timer registration used by loopyTimer.h implementation.
 * Users should use loopyTimer.h for timer operations.
 * ============================================================================
 */
uint64_t loopyRegisterTimer(loopyLoop *l, uint64_t startAfterMicroseconds,
                            uint64_t repeatEveryMicroseconds,
                            timerWheelCallback *cb, void *clientData);

/**
 * Unregister and cancel a timer.
 *
 * Removes a timer from the event loop's timer wheel and cancels it. If the
 * timer has already fired or been cancelled, this is a no-op. After this call,
 * the timer's callback will not be invoked.
 *
 * @param l The event loop
 * @param id The timer ID returned by loopyRegisterTimer()
 *
 * @return true if the timer was successfully cancelled, false if the timer
 *         was already fired, already cancelled, or the ID is invalid
 *
 * @note Internal function - call loopyTimerCancel() from loopyTimer.h instead
 * @note Thread Safety: Must be called from the event loop thread
 * @note Idempotent - safe to call multiple times with the same ID
 * @note If called while the timer's callback is executing, the return value
 *       is undefined (race condition)
 */
bool loopyUnregisterTimer(loopyLoop *l, timerWheelId id);

#endif /* LOOPY_INTERNAL_H */
