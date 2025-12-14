/* loopyAsync - Thread-safe event loop wake-up for loopy
 *
 * Provides a mechanism to safely wake the event loop from another thread
 * or signal handler. Essential for worker thread completion notifications
 * and external event injection.
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

#include "loopy.h"

#include <stdbool.h>

/* ====================================================================
 * Types
 * ==================================================================== */

/**
 * Opaque async handle structure.
 * Created with loopyAsyncNew(), freed with loopyAsyncFree().
 */
typedef struct loopyAsync loopyAsync;

/**
 * Callback invoked when the async handle is signaled.
 *
 * @param l       The event loop
 * @param async   The async handle that was signaled
 * @param userData User-provided data from loopyAsyncNew()
 *
 * Thread Safety: Always called on the event loop thread.
 *
 * Note: Multiple loopyAsyncSend() calls may be coalesced into a single
 * callback invocation. If you need to count events, use external atomics.
 */
typedef void loopyAsyncCallback(loopyLoop *l, loopyAsync *async,
                                void *userData);

/* ====================================================================
 * Lifecycle
 * ==================================================================== */

/**
 * Create a new async handle.
 *
 * The handle is immediately active and registered with the event loop.
 * Use loopyAsyncSend() to trigger the callback from any thread.
 *
 * @param loop     The event loop to register with (must not be NULL)
 * @param cb       Callback to invoke when signaled (must not be NULL)
 * @param userData User data passed to callback (may be NULL)
 * @return New async handle, or NULL on error
 *
 * Thread Safety: Must be called from the event loop thread.
 *
 * Memory: Allocates ~32-40 bytes plus 1-2 file descriptors.
 *
 * Example:
 * @code
 * void onAsync(loopyLoop *l, loopyAsync *async, void *data) {
 *     printf("Async event received!\n");
 * }
 *
 * loopyAsync *async = loopyAsyncNew(loop, onAsync, NULL);
 * @endcode
 */
loopyAsync *loopyAsyncNew(loopyLoop *loop, loopyAsyncCallback *cb,
                          void *userData);

/**
 * Free an async handle.
 *
 * Unregisters from the event loop and releases all resources.
 * Safe to call with NULL.
 *
 * @param async The handle to free, or NULL
 *
 * Thread Safety: Must be called from the event loop thread.
 */
void loopyAsyncFree(loopyAsync *async);

/* ====================================================================
 * Operations
 * ==================================================================== */

/**
 * Signal the async handle, waking the event loop.
 *
 * This is the primary mechanism for cross-thread communication with
 * the event loop. The callback will be invoked on the next event loop
 * iteration.
 *
 * @param async The handle to signal (must not be NULL)
 *
 * Thread Safety: SAFE to call from any thread or signal handler.
 * This function is async-signal-safe.
 *
 * Coalescing: Multiple calls to loopyAsyncSend() before the callback
 * runs will result in only ONE callback invocation. If you need to
 * count events, maintain your own atomic counter.
 *
 * Example:
 * @code
 * // From worker thread:
 * void *worker(void *arg) {
 *     loopyAsync *async = arg;
 *     // ... do work ...
 *     loopyAsyncSend(async);  // Wake the event loop
 *     return NULL;
 * }
 * @endcode
 */
void loopyAsyncSend(loopyAsync *async);

/**
 * Check if there are pending notifications not yet delivered.
 *
 * @param async The handle to check (must not be NULL)
 * @return true if there are pending notifications
 *
 * Thread Safety: Safe to call from any thread.
 */
bool loopyAsyncPending(const loopyAsync *async);

/* ====================================================================
 * Information
 * ==================================================================== */

/**
 * Get the event loop associated with this async handle.
 *
 * @param async The async handle
 * @return The event loop, or NULL if async is NULL
 */
loopyLoop *loopyAsyncGetLoop(const loopyAsync *async);

/**
 * Get the user data associated with this async handle.
 *
 * @param async The async handle
 * @return The user data, or NULL if async is NULL
 */
void *loopyAsyncGetData(const loopyAsync *async);

/**
 * Set new user data for this async handle.
 *
 * @param async    The async handle
 * @param userData New user data (may be NULL)
 */
void loopyAsyncSetData(loopyAsync *async, void *userData);

/**
 * Get the name of the backend implementation.
 *
 * @return "eventfd" on Linux, "pipe" on BSD/macOS, "none" if unsupported
 */
const char *loopyAsyncBackendName(void);
