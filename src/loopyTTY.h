/* loopyTTY - TTY support for loopy event loop
 *
 * Terminal handling with mode control and window size detection.
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
#include "loopyStream.h"

#include <stdbool.h>

/* ====================================================================
 * Types
 * ==================================================================== */

/**
 * Opaque TTY handle.
 */
typedef struct loopyTTY loopyTTY;

/**
 * TTY modes.
 */
typedef enum loopyTTYMode {
    LOOPY_TTY_MODE_NORMAL, /* Default line-buffered mode (cooked) */
    LOOPY_TTY_MODE_RAW,    /* Raw input, no echo, no signals */
    LOOPY_TTY_MODE_IO      /* Raw mode optimized for binary I/O */
} loopyTTYMode;

/* ====================================================================
 * Lifecycle
 * ==================================================================== */

/**
 * Create a TTY handle for a file descriptor.
 *
 * @param loop Event loop
 * @param fd   File descriptor (must be a TTY)
 * @return New TTY handle, or NULL on error
 *
 * The fd must be a valid TTY (use loopyTTYIsTTY() to check).
 * The TTY takes ownership of the fd if readable is true.
 */
loopyTTY *loopyTTYNew(loopyLoop *loop, int fd);

/**
 * Create a TTY for stdin.
 *
 * @param loop Event loop
 * @return New TTY handle, or NULL if stdin is not a TTY
 */
loopyTTY *loopyTTYStdin(loopyLoop *loop);

/**
 * Create a TTY for stdout.
 *
 * @param loop Event loop
 * @return New TTY handle, or NULL if stdout is not a TTY
 */
loopyTTY *loopyTTYStdout(loopyLoop *loop);

/**
 * Create a TTY for stderr.
 *
 * @param loop Event loop
 * @return New TTY handle, or NULL if stderr is not a TTY
 */
loopyTTY *loopyTTYStderr(loopyLoop *loop);

/**
 * Free a TTY handle.
 *
 * Restores the original terminal mode before freeing.
 *
 * @param tty TTY handle, or NULL (no-op)
 */
void loopyTTYFree(loopyTTY *tty);

/* ====================================================================
 * Mode Control
 * ==================================================================== */

/**
 * Set the TTY mode.
 *
 * Modes:
 * - NORMAL: Line-buffered input with echo (default terminal mode)
 * - RAW: Character-at-a-time input, no echo, signals disabled
 * - IO: Like RAW but optimized for binary data transfer
 *
 * @param tty  TTY handle
 * @param mode Desired mode
 * @return true on success, false on error
 */
bool loopyTTYSetMode(loopyTTY *tty, loopyTTYMode mode);

/**
 * Get the current TTY mode.
 *
 * @param tty TTY handle
 * @return Current mode
 */
loopyTTYMode loopyTTYGetMode(const loopyTTY *tty);

/**
 * Reset the TTY to its original mode.
 *
 * Called automatically by loopyTTYFree(). Can be called manually
 * to restore terminal state (e.g., before exit).
 *
 * @param tty TTY handle
 * @return true on success, false on error
 */
bool loopyTTYResetMode(loopyTTY *tty);

/**
 * Reset all TTYs to original mode.
 *
 * Global function for emergency cleanup (e.g., signal handlers).
 * Call this before exit to ensure terminal is left in a usable state.
 */
void loopyTTYResetAll(void);

/* ====================================================================
 * Window Size
 * ==================================================================== */

/**
 * Get the terminal window size.
 *
 * @param tty    TTY handle
 * @param width  OUT: columns (optional)
 * @param height OUT: rows (optional)
 * @return true on success, false on error
 */
bool loopyTTYGetWinSize(loopyTTY *tty, int *width, int *height);

/* ====================================================================
 * Stream Integration
 * ==================================================================== */

/**
 * Get the TTY as a stream for read/write operations.
 *
 * The returned stream is owned by the TTY and should NOT be freed
 * separately. Use stream APIs for reading and writing:
 *   - loopyStreamReadStart() / loopyStreamReadStop()
 *   - loopyStreamWrite() / loopyStreamTryWrite()
 *
 * @param tty TTY handle
 * @return Stream handle (owned by TTY), or NULL on error
 */
loopyStream *loopyTTYAsStream(const loopyTTY *tty);

/* ====================================================================
 * Utility Functions
 * ==================================================================== */

/**
 * Check if a file descriptor is a TTY.
 *
 * @param fd File descriptor
 * @return true if fd is a TTY
 */
bool loopyTTYIsTTY(int fd);

/**
 * Get the underlying file descriptor.
 *
 * @param tty TTY handle
 * @return File descriptor, or -1 on error
 */
int loopyTTYGetFd(const loopyTTY *tty);

/**
 * Get the event loop.
 *
 * @param tty TTY handle
 * @return Event loop, or NULL
 */
loopyLoop *loopyTTYGetLoop(const loopyTTY *tty);

/**
 * Get user data from TTY handle.
 *
 * @param tty TTY handle
 * @return User data pointer, or NULL
 */
void *loopyTTYGetData(const loopyTTY *tty);

/**
 * Set user data on TTY handle.
 *
 * @param tty TTY handle
 * @param data User data pointer
 */
void loopyTTYSetData(loopyTTY *tty, void *data);
