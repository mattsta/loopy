/* loopyTimer - High-level Timer Abstraction
 *
 * Provides a clean, user-friendly timer API built on top of timerWheel.
 * Supports both one-shot and periodic timers with automatic cleanup.
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
#include <stdint.h>

/* Forward declaration */
struct loopyLoop;

/* ====================================================================
 * Type Definitions
 * ==================================================================== */

/**
 * Opaque timer handle.
 *
 * Represents a single timer (one-shot or periodic). The timer is automatically
 * managed and will be cleaned up when cancelled or when it expires (for
 * one-shot timers).
 */
typedef struct loopyTimer loopyTimer;

/**
 * Timer callback function.
 *
 * Called when the timer expires. For periodic timers, this is called on each
 * interval. For one-shot timers, this is called once and the timer is
 * automatically cleaned up afterward.
 *
 * @param loop Event loop
 * @param timer Timer handle (valid during callback, may be invalid after)
 * @param userData User-provided context data
 */
typedef void loopyTimerCallback(struct loopyLoop *loop, loopyTimer *timer,
                                void *userData);

/* ====================================================================
 * One-Shot Timers
 * ==================================================================== */

/**
 * Create a one-shot timer that fires once after a delay.
 *
 * The timer fires once after the specified delay and is automatically
 * cleaned up afterward. No manual cleanup is required.
 *
 * Example:
 * @code
 *   void timeout_handler(loopyLoop *l, loopyTimer *t, void *data) {
 *       printf("Timer fired!\n");
 *       // Timer is automatically cleaned up after this callback
 *   }
 *
 *   loopyTimer *t = loopyTimerOneShot(loop, 5000000, timeout_handler, NULL);
 *   if (t == NULL) {
 *       fprintf(stderr, "Timer creation failed: %s\n", loopyTimerGetError());
 *       return;
 *   }
 *   // Timer fires in 5 seconds, then self-destructs
 * @endcode
 *
 * @param loop Event loop (must not be NULL)
 * @param delayUs Delay in microseconds before firing (0 fires as soon as
 * possible)
 * @param callback Function to call when timer fires (must not be NULL)
 * @param userData User context passed to callback
 * @return Timer handle on success, NULL on error (check loopyTimerGetError())
 *
 * @note The callback is invoked within the event loop's run context. The timer
 *       handle becomes invalid immediately after the callback returns - do not
 *       attempt to use it afterward.
 *
 * @see loopyTimerOneShotMs()
 * @see loopyTimerOneShotSeconds()
 * @see loopyTimerGetError()
 */
loopyTimer *loopyTimerOneShot(struct loopyLoop *loop, uint64_t delayUs,
                              loopyTimerCallback *cb, void *userData);

/**
 * Create a one-shot timer from milliseconds.
 *
 * Convenience wrapper around loopyTimerOneShot() that accepts milliseconds
 * instead of microseconds. Equivalent to loopyTimerOneShot(loop, delayMs *
 * 1000, ...).
 *
 * @param loop Event loop (must not be NULL)
 * @param delayMs Delay in milliseconds before firing
 * @param callback Function to call when timer fires (must not be NULL)
 * @param userData User context passed to callback
 * @return Timer handle on success, NULL on error (check loopyTimerGetError())
 *
 * @note The callback is invoked within the event loop's run context. The timer
 *       is automatically freed after the callback completes.
 *
 * @see loopyTimerOneShot()
 * @see loopyTimerOneShotSeconds()
 */
loopyTimer *loopyTimerOneShotMs(struct loopyLoop *loop, uint64_t delayMs,
                                loopyTimerCallback *cb, void *userData);

/**
 * Create a one-shot timer from seconds.
 *
 * Convenience wrapper around loopyTimerOneShot() that accepts seconds
 * instead of microseconds. Equivalent to loopyTimerOneShot(loop, delaySeconds *
 * 1000000, ...).
 *
 * @param loop Event loop (must not be NULL)
 * @param delaySeconds Delay in seconds before firing
 * @param callback Function to call when timer fires (must not be NULL)
 * @param userData User context passed to callback
 * @return Timer handle on success, NULL on error (check loopyTimerGetError())
 *
 * @note The callback is invoked within the event loop's run context. The timer
 *       is automatically freed after the callback completes.
 *
 * @see loopyTimerOneShot()
 * @see loopyTimerOneShotMs()
 */
loopyTimer *loopyTimerOneShotSeconds(struct loopyLoop *loop,
                                     uint64_t delaySeconds,
                                     loopyTimerCallback *cb, void *userData);

/* ====================================================================
 * Periodic Timers
 * ==================================================================== */

/**
 * Create a periodic timer that fires repeatedly at fixed intervals.
 *
 * The timer fires repeatedly every intervalUs microseconds until cancelled.
 * Use loopyTimerCancel() to stop the timer. The first fire occurs after
 * intervalUs.
 *
 * Example:
 * @code
 *   void periodic_handler(loopyLoop *l, loopyTimer *t, void *data) {
 *       printf("Periodic tick!\n");
 *   }
 *
 *   loopyTimer *t = loopyTimerPeriodic(loop, 1000000, periodic_handler, NULL);
 *   if (!t) {
 *       fprintf(stderr, "Timer creation failed: %s\n", loopyTimerGetError());
 *       return;
 *   }
 *   // Timer fires every 1 second until cancelled
 *   // ... later ...
 *   loopyTimerCancel(t);  // Stop the timer
 * @endcode
 *
 * @param loop Event loop (must not be NULL)
 * @param intervalUs Interval in microseconds between firings (must be > 0)
 * @param callback Function to call on each interval (must not be NULL)
 * @param userData User context passed to callback
 * @return Timer handle on success, NULL on error (check loopyTimerGetError())
 *
 * @note The callback is invoked within the event loop's run context. Callbacks
 *       are thread-safe: each timer is tied to a single event loop and
 * callbacks execute sequentially within that loop. Multiple timers on the same
 * loop are also sequential (one callback at a time).
 *
 * @warning If the callback takes longer than the interval, the next interval
 *          will start immediately after the callback returns, not at the
 *          scheduled time. Plan callback duration accordingly.
 *
 * @see loopyTimerPeriodicMs()
 * @see loopyTimerPeriodicSeconds()
 * @see loopyTimerPeriodicDelayed()
 * @see loopyTimerCancel()
 */
loopyTimer *loopyTimerPeriodic(struct loopyLoop *loop, uint64_t intervalUs,
                               loopyTimerCallback *cb, void *userData);

/**
 * Create a periodic timer with initial delay.
 *
 * Like loopyTimerPeriodic(), but allows specifying a different initial delay
 * before the first firing. Subsequent firings occur at intervalUs intervals.
 * Useful for staggered startup or delayed execution patterns.
 *
 * Example:
 * @code
 *   // Wait 5 seconds, then fire every 1 second
 *   loopyTimer *t = loopyTimerPeriodicDelayed(loop, 5000000, 1000000,
 *                                              handler, NULL);
 *   if (!t) {
 *       fprintf(stderr, "Failed to create timer: %s\n", loopyTimerGetError());
 *       return;
 *   }
 * @endcode
 *
 * @param loop Event loop (must not be NULL)
 * @param initialDelayUs Initial delay in microseconds before first firing
 * @param intervalUs Interval in microseconds between subsequent firings (must
 * be > 0)
 * @param callback Function to call on each interval (must not be NULL)
 * @param userData User context passed to callback
 * @return Timer handle on success, NULL on error (check loopyTimerGetError())
 *
 * @note The callback is invoked within the event loop's run context. After
 *       initialDelayUs, the first callback is invoked, then callbacks repeat
 *       at intervalUs intervals. All callbacks execute sequentially within
 *       the event loop.
 *
 * @see loopyTimerPeriodic()
 * @see loopyTimerCancel()
 */
loopyTimer *loopyTimerPeriodicDelayed(struct loopyLoop *loop,
                                      uint64_t initialDelayUs,
                                      uint64_t intervalUs,
                                      loopyTimerCallback *cb, void *userData);

/**
 * Create a periodic timer from milliseconds.
 *
 * Convenience wrapper around loopyTimerPeriodic() that accepts milliseconds.
 * Equivalent to loopyTimerPeriodic(loop, intervalMs * 1000, ...).
 *
 * @param loop Event loop (must not be NULL)
 * @param intervalMs Interval in milliseconds between firings (must be > 0)
 * @param callback Function to call on each interval (must not be NULL)
 * @param userData User context passed to callback
 * @return Timer handle on success, NULL on error (check loopyTimerGetError())
 *
 * @note The callback is invoked within the event loop's run context. Callbacks
 *       execute sequentially within the event loop's thread.
 *
 * @see loopyTimerPeriodic()
 * @see loopyTimerPeriodicSeconds()
 */
loopyTimer *loopyTimerPeriodicMs(struct loopyLoop *loop, uint64_t intervalMs,
                                 loopyTimerCallback *cb, void *userData);

/**
 * Create a periodic timer from seconds.
 *
 * Convenience wrapper around loopyTimerPeriodic() that accepts seconds.
 * Equivalent to loopyTimerPeriodic(loop, intervalSeconds * 1000000, ...).
 *
 * @param loop Event loop (must not be NULL)
 * @param intervalSeconds Interval in seconds between firings (must be > 0)
 * @param callback Function to call on each interval (must not be NULL)
 * @param userData User context passed to callback
 * @return Timer handle on success, NULL on error (check loopyTimerGetError())
 *
 * @note The callback is invoked within the event loop's run context. Callbacks
 *       execute sequentially within the event loop's thread.
 *
 * @see loopyTimerPeriodic()
 * @see loopyTimerPeriodicMs()
 */
loopyTimer *loopyTimerPeriodicSeconds(struct loopyLoop *loop,
                                      uint64_t intervalSeconds,
                                      loopyTimerCallback *cb, void *userData);

/* ====================================================================
 * Timer Management
 * ==================================================================== */

/**
 * Cancel a timer.
 *
 * Cancels and frees a timer. Safe to call on NULL pointers and already-freed
 * timers. After calling this, the timer handle is invalid and must not be used.
 *
 * Note: One-shot timers are automatically cancelled after firing, so you only
 * need to call this if you want to cancel them before they fire. Safe to call
 * from within the timer's callback.
 *
 * Example:
 * @code
 *   loopyTimer *t = loopyTimerPeriodic(loop, 1000000, handler, NULL);
 *   // ... later ...
 *   loopyTimerCancel(t);  // Stop and free the timer
 *   t = NULL;  // Good practice
 *
 *   // Safe to call multiple times or on NULL:
 *   loopyTimerCancel(NULL);  // No-op
 *   loopyTimerCancel(t);     // No-op if already cancelled
 * @endcode
 *
 * @param timer Timer to cancel (may be NULL, already-cancelled, or freed)
 *
 * @note This function is idempotent and safe to call from callbacks. If called
 *       from within the timer's callback, the timer is marked for deletion and
 *       freed after the callback returns.
 *
 * @see loopyTimerIsActive()
 */
void loopyTimerCancel(loopyTimer *timer);

/**
 * Check if a timer is still active.
 *
 * Returns true if the timer is active and will fire in the future.
 * Returns false if the timer is NULL, has been cancelled, or has already
 * fired (for one-shot timers).
 *
 * @param timer Timer to check (may be NULL)
 * @return true if timer is active and will fire, false otherwise
 *
 * @note Useful for checking if a timer was successfully created or has
 *       already been cancelled. Safe to call on NULL.
 *
 * @see loopyTimerCancel()
 */
bool loopyTimerIsActive(const loopyTimer *timer);

/**
 * Get the event loop associated with this timer.
 *
 * @param timer Timer handle (may be NULL)
 * @return Event loop pointer that owns this timer, or NULL if timer is NULL
 *
 * @note Useful for registering other event handlers on the same loop or
 *       verifying timer ownership.
 *
 * @see loopyTimerGetData()
 * @see loopyTimerSetData()
 */
struct loopyLoop *loopyTimerGetLoop(const loopyTimer *timer);

/**
 * Get the user data associated with a timer.
 *
 * Returns the userData pointer that was passed when the timer was created.
 * This is useful for storing context (e.g., a struct containing state).
 *
 * @param timer Timer handle (may be NULL)
 * @return User data pointer that was set on creation, or NULL if timer is NULL
 *         or if userData was NULL when created
 *
 * @note The returned pointer points to the same data passed to the creation
 *       function (loopyTimerOneShot, loopyTimerPeriodic, etc.).
 *
 * @see loopyTimerSetData()
 * @see loopyTimerGetLoop()
 */
void *loopyTimerGetData(const loopyTimer *timer);

/**
 * Update the user data for a timer.
 *
 * Changes the userData pointer for an active timer. The new userData will
 * be passed to future timer callbacks. Useful for updating context during
 * the timer's lifetime.
 *
 * @param timer Timer handle (may be NULL)
 * @param userData New user data pointer (may be NULL)
 * @return true on success, false if timer is NULL, inactive, or cancelled
 *
 * @note Safe to call from within the timer's callback. Changes take effect
 *       for the next callback invocation (not for any currently executing
 *       callback).
 *
 * @see loopyTimerGetData()
 * @see loopyTimerIsActive()
 */
bool loopyTimerSetData(loopyTimer *timer, void *userData);

/* ====================================================================
 * Error Handling
 * ==================================================================== */

/**
 * Get the last error message.
 *
 * Returns a human-readable description of the last error that occurred in
 * any loopyTimer function on this thread. Returns NULL if no error has
 * occurred. Useful for debugging failed timer creation.
 *
 * The error message is stored in thread-local storage and is valid until
 * the next loopyTimer function call on this thread. This function itself
 * does not modify the error state.
 *
 * Example:
 * @code
 *   loopyTimer *t = loopyTimerOneShot(loop, 5000000, cb, NULL);
 *   if (!t) {
 *       const char *err = loopyTimerGetError();
 *       if (err) {
 *           fprintf(stderr, "Timer creation failed: %s\n", err);
 *       } else {
 *           fprintf(stderr, "Timer creation failed (no error details)\n");
 *       }
 *   }
 * @endcode
 *
 * @return Error message string, or NULL if no error has occurred
 *
 * @note The error message is thread-local, so each thread has its own
 *       independent error state. The message persists across timer operations
 *       until the next error occurs.
 *
 * @see loopyTimerOneShot()
 * @see loopyTimerPeriodic()
 */
const char *loopyTimerGetError(void);
