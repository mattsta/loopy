/* loopyProcess - Process spawning for loopy event loop
 *
 * Spawn and manage child processes with stdio redirection.
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
#include <stdint.h>
#include <sys/types.h>

/* ====================================================================
 * Types
 * ==================================================================== */

/**
 * Opaque process handle.
 */
typedef struct loopyProcess loopyProcess;

/**
 * Process exit callback.
 *
 * @param loop       The event loop
 * @param process    The process handle
 * @param exitStatus Exit status (if exited normally)
 * @param termSignal Signal number (if terminated by signal, 0 otherwise)
 * @param userData   User data from spawn
 *
 * Thread Safety: Always called on the event loop thread.
 */
typedef void loopyProcessExitCallback(loopyLoop *loop, loopyProcess *process,
                                      int64_t exitStatus, int termSignal,
                                      void *userData);

/**
 * stdio configuration flags.
 */
typedef enum loopyStdioFlags {
    LOOPY_STDIO_IGNORE = 0x00,      /* Redirect to /dev/null */
    LOOPY_STDIO_CREATE_PIPE = 0x01, /* Create a pipe */
    LOOPY_STDIO_INHERIT_FD = 0x02,  /* Inherit specific fd */
    LOOPY_STDIO_INHERIT = 0x04,     /* Inherit from parent */
} loopyStdioFlags;

/**
 * stdio container for each fd.
 */
typedef struct loopyStdioContainer {
    loopyStdioFlags flags;
    int fd; /* For INHERIT_FD: the fd to inherit */
} loopyStdioContainer;

/**
 * Process spawn options.
 */
typedef struct loopyProcessOptions {
    const char *file; /* Executable path (required) */
    char **args;      /* NULL-terminated arguments (args[0] = name) */
    char **env;       /* NULL-terminated env vars (NULL = inherit) */
    const char *cwd;  /* Working directory (NULL = inherit) */
    loopyStdioContainer stdio[3]; /* stdin, stdout, stderr */
    unsigned flags;               /* Process flags (see LOOPY_PROCESS_*) */
    int uid;                      /* User ID (if LOOPY_PROCESS_SETUID) */
    int gid;                      /* Group ID (if LOOPY_PROCESS_SETGID) */
} loopyProcessOptions;

/* Process flags */
#define LOOPY_PROCESS_SETUID 0x01   /* Change user ID */
#define LOOPY_PROCESS_SETGID 0x02   /* Change group ID */
#define LOOPY_PROCESS_DETACHED 0x04 /* Survive parent exit */

/* ====================================================================
 * Lifecycle
 * ==================================================================== */

/**
 * Spawn a new child process.
 *
 * @param loop    The event loop (must not be NULL)
 * @param options Process options (must not be NULL, file must be set)
 * @param exitCb  Exit callback (called when process exits)
 * @param userData User data passed to callback
 * @return New process handle, or NULL on error
 *
 * Thread Safety: Must be called from the event loop thread.
 *
 * The exit callback will be called when the process exits, either
 * normally or due to a signal. Use loopyProcessGetExitStatus() and
 * loopyProcessGetTermSignal() to determine how it exited.
 */
loopyProcess *loopyProcessSpawn(loopyLoop *loop,
                                const loopyProcessOptions *options,
                                loopyProcessExitCallback *exitCb,
                                void *userData);

/**
 * Free a process handle.
 *
 * Releases all resources associated with a process handle, including any
 * open pipes and internal state. If the process is still running when freed,
 * it will continue to run (orphaned from the parent).
 *
 * @param process The handle to free, or NULL (safe no-op)
 *
 * @note Thread Safety: Must be called from the event loop thread.
 *
 * @note If the process is still running and you want to terminate it, call
 *       loopyProcessKill(process, SIGTERM) before freeing.
 *
 * @note Any pipes created for the process (from loopyProcessGetStdioPipe)
 *       are closed when the process is freed. Close them earlier if you
 *       need to maintain separate lifetime.
 *
 * @note After calling this, the process pointer becomes invalid and must
 *       not be accessed.
 *
 * @note It is safe to call with NULL.
 *
 * Example:
 * @code
 *   if (loopyProcessExited(process)) {
 *       loopyProcessFree(process);
 *   } else {
 *       loopyProcessKill(process, SIGTERM);
 *       // Later, in exit callback:
 *       loopyProcessFree(process);
 *   }
 * @endcode
 */
void loopyProcessFree(loopyProcess *process);

/* ====================================================================
 * Control
 * ==================================================================== */

/**
 * Send a signal to the process.
 *
 * Sends a signal (such as SIGTERM, SIGKILL) to the child process.
 *
 * @param process The process handle
 * @param signum  Signal number (e.g., SIGTERM, SIGKILL)
 * @return true on success, false if:
 *         - process is NULL
 *         - process has already exited
 *         - the kill() syscall failed (check errno)
 *
 * @note Common signal values:
 *       - SIGTERM (15): Graceful termination request
 *       - SIGKILL (9): Immediate forceful termination (cannot be caught)
 *       - SIGSTOP (19): Pause the process
 *       - SIGCONT (18): Resume a paused process
 *
 * @note Sending SIGKILL will terminate the process immediately. The process
 *       may not perform cleanup or emit a successful exit status.
 *
 * @note If the process has already exited, this will return false and errno
 *       will be set to ESRCH (no such process).
 *
 * @note The exit callback will still be invoked when the signal causes
 *       process termination.
 *
 * Example (graceful shutdown):
 * @code
 *   if (!loopyProcessExited(process)) {
 *       if (!loopyProcessKill(process, SIGTERM)) {
 *           perror("kill");
 *       }
 *   }
 * @endcode
 *
 * Example (force kill):
 * @code
 *   // After timeout waiting for graceful exit
 *   loopyProcessKill(process, SIGKILL);
 * @endcode
 */
bool loopyProcessKill(loopyProcess *process, int signum);

/* ====================================================================
 * Information
 * ==================================================================== */

/**
 * Get the process ID.
 *
 * Returns the operating system process ID (PID) of the child process.
 *
 * @param process The process handle
 * @return Process ID (>= 1) on success, or -1 if:
 *         - process is NULL
 *         - the process was never successfully created
 *
 * @note The PID is valid immediately after loopyProcessSpawn() returns
 *       a non-NULL handle.
 *
 * @note The PID remains valid even after the process exits, though
 *       you can no longer send signals to it.
 *
 * @note On Unix systems, PID 1 is init/systemd and should never be
 *       returned for child processes.
 *
 * @warning The PID may become invalid (stale) if:
 *          - The process has exited and been reaped
 *          - Another process reuses the same PID number (rare but possible)
 *          Use loopyProcessExited() to check if the process is still alive.
 *
 * Example:
 * @code
 *   loopyProcess *proc = loopyProcessSpawn(loop, &opts, cb, NULL);
 *   if (proc) {
 *       pid_t pid = loopyProcessGetPid(proc);
 *       printf("Spawned child process: %d\n", pid);
 *   }
 * @endcode
 */
pid_t loopyProcessGetPid(const loopyProcess *process);

/**
 * Get a stdio pipe file descriptor.
 *
 * Returns the parent's end of a stdio pipe created for the child process.
 * For stdin (index 0): returns the write end (parent writes to child)
 * For stdout/stderr (index 1, 2): returns the read end (parent reads from
 * child)
 *
 * @param process The process handle
 * @param index   0=stdin, 1=stdout, 2=stderr
 * @return File descriptor (>= 0) if a pipe exists, or -1 if:
 *         - process is NULL
 *         - index is not 0, 1, or 2
 *         - no pipe was created for that stdio (depends on loopyStdioFlags)
 *
 * CRITICAL @note The file descriptor is OWNED by the loopyProcess handle.
 *          The user MUST NOT close this file descriptor directly. It will
 *          be automatically closed when loopyProcessFree() is called.
 *          Closing it early will corrupt the process handle state.
 *
 * @note You can register these file descriptors with the event loop for
 *       non-blocking I/O. The loopy library handles this automatically if
 *       you use loopyStream or other I/O wrappers.
 *
 * @note The file descriptor is set to non-blocking mode after creation.
 *
 * @note For stdin: you write data that the child process reads
 * @note For stdout/stderr: you read data that the child process writes
 *
 * @note Close the reading end of a pipe when you're done to signal EOF
 *       to the child process, but do NOT close the fd itself - let
 *       loopyProcessFree() handle that.
 *
 * Example (writing to stdin):
 * @code
 *   int stdin_fd = loopyProcessGetStdioPipe(process, 0);
 *   if (stdin_fd >= 0) {
 *       write(stdin_fd, "data\n", 5);
 *       // Do NOT close stdin_fd
 *   }
 * @endcode
 *
 * Example (reading from stdout):
 * @code
 *   int stdout_fd = loopyProcessGetStdioPipe(process, 1);
 *   if (stdout_fd >= 0) {
 *       char buf[1024];
 *       ssize_t n = read(stdout_fd, buf, sizeof(buf));
 *       // Do NOT close stdout_fd
 *   }
 * @endcode
 */
int loopyProcessGetStdioPipe(loopyProcess *process, int index);

/**
 * Check if process has exited.
 *
 * Determines whether the child process has exited (normally or due to a
 * signal).
 *
 * @param process The process handle
 * @return true if the process has exited, false if:
 *         - process is NULL (treated as exited)
 *         - the process is still running
 *
 * @note After this returns true, you can safely call
 * loopyProcessGetExitStatus() and loopyProcessGetTermSignal() to determine how
 * it exited.
 *
 * @note This is typically called from the exit callback to verify the
 *       process has exited before accessing exit status.
 *
 * Example:
 * @code
 *   void exit_cb(loopyLoop *loop, loopyProcess *proc,
 *                int64_t exitStatus, int termSignal, void *userData) {
 *       // Called here means the process has exited
 *       if (exitStatus != 0) {
 *           fprintf(stderr, "Process exited with code %ld\n", exitStatus);
 *       }
 *   }
 *
 *   // Can also check explicitly:
 *   if (loopyProcessExited(process)) {
 *       // Safe to access exit status
 *   }
 * @endcode
 */
bool loopyProcessExited(const loopyProcess *process);

/**
 * Get the event loop associated with this process.
 *
 * @param process The process handle
 * @return The event loop, or NULL if process is NULL
 */
loopyLoop *loopyProcessGetLoop(const loopyProcess *process);

/**
 * Get the last error message.
 *
 * Returns a descriptive error message from the most recent operation
 * that failed on this process handle.
 *
 * @param process The process handle
 * @return Error message string (always non-NULL), or empty string "" if:
 *         - process is NULL
 *         - no error has occurred
 *
 * @note The error message is limited to 127 characters.
 * @note The message persists until another operation occurs.
 * @note Call this immediately after a failed function (e.g., loopyProcessKill()
 *       returning false) to get the most relevant error.
 *
 * Example:
 * @code
 *   if (!loopyProcessKill(process, SIGTERM)) {
 *       fprintf(stderr, "Kill failed: %s\n", loopyProcessGetError(process));
 *   }
 * @endcode
 */
const char *loopyProcessGetError(const loopyProcess *process);

/**
 * Get user data from process handle.
 *
 * @param process The process handle
 * @return User data pointer, or NULL
 */
void *loopyProcessGetData(const loopyProcess *process);

/**
 * Set user data on process handle.
 *
 * @param process The process handle
 * @param data User data pointer
 */
void loopyProcessSetData(loopyProcess *process, void *data);
