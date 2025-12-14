/* loopyIdle - Idle, Prepare, and Check handles for loopy event loop
 *
 * Execute callbacks at specific phases of the event loop iteration:
 * - Idle: Runs every iteration when active, forces zero-timeout poll
 * - Prepare: Runs before I/O polling
 * - Check: Runs after I/O polling
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
#include <stddef.h>
#include <stdint.h>

/* ====================================================================
 * Types
 * ==================================================================== */

/**
 * Handle types for the three callback phases.
 */
typedef struct loopyIdleHandle loopyIdleHandle;
typedef struct loopyPrepareHandle loopyPrepareHandle;
typedef struct loopyCheckHandle loopyCheckHandle;

/**
 * Idle callback - runs every iteration while idle handle is active.
 *
 * @param loop     The event loop
 * @param handle   The idle handle
 * @param userData User data from start
 * @return true to continue, false to auto-stop this idle handle
 *
 * Note: While any idle handle is active, the event loop will poll with
 * a zero timeout (non-blocking), causing it to spin and use CPU.
 * Use idle handles sparingly and stop them when work is complete.
 */
typedef bool loopyIdleCallback(loopyLoop *loop, loopyIdleHandle *handle,
                               void *userData);

/**
 * Prepare callback - runs before I/O polling.
 *
 * @param loop     The event loop
 * @param handle   The prepare handle
 * @param userData User data from start
 *
 * Use cases:
 * - Flush pending writes before blocking
 * - Setup state before poll
 * - Adjust poll timeout (indirectly via idle handles)
 */
typedef void loopyPrepareCallback(loopyLoop *loop, loopyPrepareHandle *handle,
                                  void *userData);

/**
 * Check callback - runs after I/O polling.
 *
 * @param loop     The event loop
 * @param handle   The check handle
 * @param userData User data from start
 *
 * Use cases:
 * - Process results from I/O operations
 * - Perform work after blocking returns
 * - Integrate with external libraries
 */
typedef void loopyCheckCallback(loopyLoop *loop, loopyCheckHandle *handle,
                                void *userData);

/* ====================================================================
 * Idle Handle API
 * ==================================================================== */

/**
 * Start an idle handle.
 *
 * The callback will be invoked on every event loop iteration while the
 * handle is active. Having active idle handles causes the event loop to
 * use a zero-timeout poll, effectively spinning the CPU.
 *
 * @param loop     The event loop (must not be NULL)
 * @param cb       Callback function (must not be NULL)
 * @param userData User data passed to callback
 * @return New handle, or NULL on error
 *
 * Thread Safety: Must be called from the event loop thread.
 */
loopyIdleHandle *loopyIdleStart(loopyLoop *loop, loopyIdleCallback *cb,
                                void *userData);

/**
 * Stop an idle handle.
 *
 * The handle remains valid and can be restarted with loopyIdleRestart().
 *
 * @param handle The handle to stop, or NULL (no-op)
 */
void loopyIdleStop(loopyIdleHandle *handle);

/**
 * Restart a stopped idle handle.
 *
 * @param handle The handle to restart
 * @return true if restarted, false if already active or invalid
 */
bool loopyIdleRestart(loopyIdleHandle *handle);

/**
 * Free an idle handle.
 *
 * @param handle The handle to free, or NULL (no-op)
 */
void loopyIdleFree(loopyIdleHandle *handle);

/**
 * Check if an idle handle is active.
 *
 * @param handle The handle to check
 * @return true if active (callback will be invoked)
 */
bool loopyIdleIsActive(const loopyIdleHandle *handle);

/**
 * Get the event loop for an idle handle.
 *
 * @param handle The handle
 * @return The event loop, or NULL if handle is NULL
 */
loopyLoop *loopyIdleGetLoop(const loopyIdleHandle *handle);

/**
 * Get user data from idle handle.
 */
void *loopyIdleGetData(const loopyIdleHandle *handle);

/**
 * Set user data on idle handle.
 */
void loopyIdleSetData(loopyIdleHandle *handle, void *data);

/* ====================================================================
 * Prepare Handle API
 * ==================================================================== */

/**
 * Start a prepare handle.
 *
 * The callback will be invoked before every I/O poll operation.
 *
 * @param loop     The event loop (must not be NULL)
 * @param cb       Callback function (must not be NULL)
 * @param userData User data passed to callback
 * @return New handle, or NULL on error
 */
loopyPrepareHandle *loopyPrepareStart(loopyLoop *loop, loopyPrepareCallback *cb,
                                      void *userData);

/**
 * Stop a prepare handle.
 *
 * @param handle The handle to stop, or NULL (no-op)
 */
void loopyPrepareStop(loopyPrepareHandle *handle);

/**
 * Restart a stopped prepare handle.
 *
 * @param handle The handle to restart
 * @return true if restarted, false if already active or invalid
 */
bool loopyPrepareRestart(loopyPrepareHandle *handle);

/**
 * Free a prepare handle.
 *
 * @param handle The handle to free, or NULL (no-op)
 */
void loopyPrepareFree(loopyPrepareHandle *handle);

/**
 * Check if a prepare handle is active.
 *
 * @param handle The handle to check
 * @return true if active
 */
bool loopyPrepareIsActive(const loopyPrepareHandle *handle);

/**
 * Get the event loop for a prepare handle.
 *
 * @param handle The handle
 * @return The event loop, or NULL if handle is NULL
 */
loopyLoop *loopyPrepareGetLoop(const loopyPrepareHandle *handle);

/**
 * Get user data from prepare handle.
 */
void *loopyPrepareGetData(const loopyPrepareHandle *handle);

/**
 * Set user data on prepare handle.
 */
void loopyPrepareSetData(loopyPrepareHandle *handle, void *data);

/* ====================================================================
 * Check Handle API
 * ==================================================================== */

/**
 * Start a check handle.
 *
 * The callback will be invoked after every I/O poll operation.
 *
 * @param loop     The event loop (must not be NULL)
 * @param cb       Callback function (must not be NULL)
 * @param userData User data passed to callback
 * @return New handle, or NULL on error
 */
loopyCheckHandle *loopyCheckStart(loopyLoop *loop, loopyCheckCallback *cb,
                                  void *userData);

/**
 * Stop a check handle.
 *
 * @param handle The handle to stop, or NULL (no-op)
 */
void loopyCheckStop(loopyCheckHandle *handle);

/**
 * Restart a stopped check handle.
 *
 * @param handle The handle to restart
 * @return true if restarted, false if already active or invalid
 */
bool loopyCheckRestart(loopyCheckHandle *handle);

/**
 * Free a check handle.
 *
 * @param handle The handle to free, or NULL (no-op)
 */
void loopyCheckFree(loopyCheckHandle *handle);

/**
 * Check if a check handle is active.
 *
 * @param handle The handle to check
 * @return true if active
 */
bool loopyCheckIsActive(const loopyCheckHandle *handle);

/**
 * Get the event loop for a check handle.
 *
 * @param handle The handle
 * @return The event loop, or NULL if handle is NULL
 */
loopyLoop *loopyCheckGetLoop(const loopyCheckHandle *handle);

/**
 * Get user data from check handle.
 */
void *loopyCheckGetData(const loopyCheckHandle *handle);

/**
 * Set user data on check handle.
 */
void loopyCheckSetData(loopyCheckHandle *handle, void *data);

/* ====================================================================
 * Query Functions
 * ==================================================================== */

/**
 * Get count of active idle handles.
 *
 * @param loop The event loop
 * @return Number of active idle handles
 */
size_t loopyIdleCount(const loopyLoop *loop);

/**
 * Get count of active prepare handles.
 *
 * @param loop The event loop
 * @return Number of active prepare handles
 */
size_t loopyPrepareCount(const loopyLoop *loop);

/**
 * Get count of active check handles.
 *
 * @param loop The event loop
 * @return Number of active check handles
 */
size_t loopyCheckCount(const loopyLoop *loop);

/**
 * Check if any idle handles are active.
 *
 * This is useful for determining if the event loop will spin.
 *
 * @param loop The event loop
 * @return true if any idle handles are active
 */
bool loopyHasActiveIdle(const loopyLoop *loop);
