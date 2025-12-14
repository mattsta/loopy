/* loopySignal - Safe signal handling for loopy event loop
 *
 * Provides async-signal-safe signal handling by capturing signals
 * and delivering them to callbacks in the main event loop context.
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
#include <signal.h>
#include <stdbool.h>

/* Forward declarations */
typedef struct loopySignalHandler loopySignalHandler;

/**
 * Signal handler callback function.
 *
 * Invoked in the context of the event loop thread when a registered signal
 * is received. The callback is guaranteed to be called from the event loop's
 * thread, making it safe to call loopy APIs without additional synchronization.
 *
 * @param l The event loop that triggered the callback
 * @param signum The signal number that was received
 * @param userData User-provided data passed during signal registration
 *
 * @note The callback is invoked from the event loop thread context, NOT from
 *       the signal handler itself. This allows for safe access to shared data
 *       and loopy APIs without async-signal-safety constraints.
 *
 * @see loopySignalRegister()
 * @see loopySignalRegisterOneshot()
 */
typedef void loopySignalCallback(loopyLoop *l, int signum, void *userData);

/* ====================================================================
 * Lifecycle
 * ==================================================================== */

/**
 * Create a signal handler attached to an event loop.
 *
 * Initializes a signal handler that safely delivers signals to the event loop.
 * Uses signalfd() on Linux for efficiency, and the self-pipe trick on other
 * platforms (BSD, macOS). Only one signal handler per process is supported
 * due to global signal handler registration constraints.
 *
 * @param loop The event loop to attach this signal handler to (must not be
 * NULL)
 * @return A new signal handler, or NULL on failure (out of memory, loop is
 * NULL, or a signal handler already exists)
 *
 * @note Only one signal handler may be created per process. Attempting to
 *       create a second one will fail.
 *
 * @note On Linux, uses signalfd() for efficiency. On other platforms, uses
 *       a self-pipe to safely deliver signals to the event loop context.
 *
 * @see loopySignalFree()
 * @see loopySignalRegister()
 *
 * @code
 * // Create a signal handler for graceful shutdown
 * loopySignalHandler *sh = loopySignalNew(loop);
 * if (!sh) {
 *     fprintf(stderr, "Failed to create signal handler\n");
 *     return -1;
 * }
 * @endcode
 */
loopySignalHandler *loopySignalNew(loopyLoop *loop);

/**
 * Free a signal handler and restore default signal dispositions.
 *
 * Unregisters all registered signals, restores the previous signal handlers,
 * and frees all associated resources. After this call, the signal handler
 * pointer is invalid and must not be used.
 *
 * @param sh The signal handler to free (may be NULL, in which case this is a
 * no-op)
 *
 * @note All registered signals are unregistered and their handlers restored to
 *       their previous states before this function returns.
 *
 * @note If this is the only/global signal handler, the process signal state
 *       is returned to what it was before loopySignalNew() was called.
 *
 * @see loopySignalNew()
 *
 * @code
 * loopySignalFree(sh);
 * sh = NULL;  // Good practice to avoid use-after-free
 * @endcode
 */
void loopySignalFree(loopySignalHandler *sh);

/* ====================================================================
 * Signal Registration
 * ==================================================================== */

/**
 * Register a callback for a signal.
 *
 * Registers a signal handler that will invoke the callback when the signal
 * is received. The callback is invoked in the event loop thread context,
 * making it safe to use loopy APIs without special synchronization.
 *
 * Only one callback per signal is allowed. Attempting to register the same
 * signal twice will fail.
 *
 * @param sh The signal handler (must not be NULL)
 * @param signum The signal number to register (e.g., SIGTERM, SIGUSR1)
 * @param cb The callback function to invoke (must not be NULL)
 * @param userData User data to pass to the callback
 * @return true on success, false on failure
 *
 * @retval false if sh is NULL, cb is NULL, signum is invalid, or the signal
 *         is already registered
 * @retval true if the signal was successfully registered
 *
 * @note The callback will be invoked from the event loop thread, not from
 *       the signal handler. This means you can safely call loopy APIs and
 *       access shared data structures from the callback.
 *
 * @note Signal handlers are process-wide: registering a signal affects all
 *       threads. However, delivery is safe due to the event loop mediation.
 *
 * @see loopySignalRegisterOneshot()
 * @see loopySignalUnregister()
 * @see loopySignalCallback
 *
 * @code
 * void handle_sigterm(loopyLoop *l, int signum, void *data) {
 *     AppState *app = data;
 *     printf("SIGTERM received, shutting down...\n");
 *     loopyLoopBreak(app->loop);
 * }
 *
 * loopySignalRegister(sh, SIGTERM, handle_sigterm, app);
 * @endcode
 */
bool loopySignalRegister(loopySignalHandler *sh, int signum,
                         loopySignalCallback *cb, void *userData);

/**
 * Register a one-shot callback for a signal.
 *
 * Like loopySignalRegister(), but the callback fires once and then
 * automatically unregisters itself. This is useful for signals that should
 * only be handled once, or for temporary signal handlers.
 *
 * @param sh The signal handler (must not be NULL)
 * @param signum The signal number to register
 * @param cb The callback function to invoke (must not be NULL)
 * @param userData User data to pass to the callback
 * @return true on success, false on failure
 *
 * @retval false if sh is NULL, cb is NULL, signum is invalid, or the signal
 *         is already registered
 * @retval true if the signal was successfully registered
 *
 * @note After the callback fires, the signal is automatically unregistered
 *       and the signal handler restored to its previous state. If the signal
 *       is received again, it will be handled by whatever was in place before
 *       the registration.
 *
 * @note The callback executes in the event loop thread, so you can safely
 *       call loopy APIs.
 *
 * @see loopySignalRegister()
 * @see loopySignalCallback
 *
 * @code
 * void handle_sighup_once(loopyLoop *l, int signum, void *data) {
 *     printf("SIGHUP received once, reloading config...\n");
 *     // reload config
 * }
 *
 * loopySignalRegisterOneshot(sh, SIGHUP, handle_sighup_once, config);
 * @endcode
 */
bool loopySignalRegisterOneshot(loopySignalHandler *sh, int signum,
                                loopySignalCallback *cb, void *userData);

/**
 * Unregister a signal callback and restore the previous handler.
 *
 * Removes the registered callback for a signal and restores the previous
 * signal handler (as it was before loopySignalRegister was called).
 *
 * @param sh The signal handler (must not be NULL)
 * @param signum The signal number to unregister
 * @return true on success, false on failure
 *
 * @retval false if sh is NULL, signum is invalid, or the signal is not
 *         currently registered
 * @retval true if the signal was successfully unregistered
 *
 * @note The previous signal handler is restored. If this was the default
 *       handler, the default behavior is restored.
 *
 * @see loopySignalRegister()
 * @see loopySignalRegisterOneshot()
 *
 * @code
 * // Unregister and restore default SIGUSR1 handler
 * loopySignalUnregister(sh, SIGUSR1);
 * @endcode
 */
bool loopySignalUnregister(loopySignalHandler *sh, int signum);

/* ====================================================================
 * Utility
 * ==================================================================== */

/**
 * Get the name of a signal.
 *
 * Returns a human-readable string name for a signal number.
 *
 * @param signum The signal number (e.g., SIGTERM, SIGUSR1)
 * @return A string like "SIGTERM", "SIGUSR1", etc., or "UNKNOWN" for
 * unrecognized signals
 *
 * @note The returned string is a static constant and should not be freed.
 *
 * @see LOOPY_SIGTERM, LOOPY_SIGINT, LOOPY_SIGHUP, etc.
 *
 * @code
 * const char *name = loopySignalName(SIGTERM);
 * printf("Received signal: %s\n", name);  // Prints: "Received signal:
 * SIGTERM\n"
 * @endcode
 */
const char *loopySignalName(int signum);

/* ====================================================================
 * Handle Accessors
 * ==================================================================== */

/**
 * Get the event loop associated with this signal handler.
 *
 * @param sh The signal handler
 * @return The event loop passed to loopySignalNew(), or NULL if sh is NULL
 *
 * @see loopySignalNew()
 */
loopyLoop *loopySignalGetLoop(const loopySignalHandler *sh);

/**
 * Get user data from the signal handler.
 *
 * Retrieves user-provided data that was set with loopySignalSetData().
 *
 * @param sh The signal handler
 * @return The user data previously set, or NULL if no data was set or sh is
 * NULL
 *
 * @see loopySignalSetData()
 *
 * @code
 * void *data = loopySignalGetData(sh);
 * if (data) {
 *     AppState *app = data;
 *     // Use app state
 * }
 * @endcode
 */
void *loopySignalGetData(const loopySignalHandler *sh);

/**
 * Set user data on the signal handler.
 *
 * Associates arbitrary user data with this signal handler for later retrieval.
 *
 * @param sh The signal handler (if NULL, this is a no-op)
 * @param data User data pointer (may be NULL)
 *
 * @see loopySignalGetData()
 *
 * @code
 * AppState *app = malloc(sizeof(AppState));
 * loopySignalSetData(sh, app);
 * @endcode
 */
void loopySignalSetData(loopySignalHandler *sh, void *data);

/* ====================================================================
 * Common Signals
 * ==================================================================== */

/* Common signals for convenience */
#define LOOPY_SIGTERM SIGTERM
#define LOOPY_SIGINT SIGINT
#define LOOPY_SIGHUP SIGHUP
#define LOOPY_SIGUSR1 SIGUSR1
#define LOOPY_SIGUSR2 SIGUSR2
#define LOOPY_SIGPIPE SIGPIPE
#define LOOPY_SIGCHLD SIGCHLD
