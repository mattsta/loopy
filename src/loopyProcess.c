/* loopyProcess - Process spawning for loopy event loop
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

#include "loopyPlatform.h"

#include "../deps/datakit/src/datakit.h"
#include "loopyProcess.h"
#include "loopySignal.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* ====================================================================
 * Internal data structures
 * ==================================================================== */

struct loopyProcess {
    loopyLoop *loop;
    pid_t pid;
    loopyProcessExitCallback *exitCb;
    void *userData;

    /* stdio pipes: [0]=stdin, [1]=stdout, [2]=stderr */
    /* For stdin: parentFd is write end, childFd is read end */
    /* For stdout/stderr: parentFd is read end, childFd is write end */
    int stdioPipes[3][2];

    /* Exit status */
    bool exited;
    int64_t exitStatus;
    int termSignal;

    /* Linked list of processes for this loop */
    struct loopyProcess *next;
    struct loopyProcess *prev;

    /* Error tracking */
    char errorString[128];
};

/* Per-loop process management */
typedef struct loopyProcessManager {
    loopyLoop *loop;
    loopySignalHandler *sigchldHandle;
    loopyProcess *processList;
    struct loopyProcessManager *next;
} loopyProcessManager;

/* Global list of managers (one per loop) */
static loopyProcessManager *g_managers = NULL;

/* ====================================================================
 * Forward declarations
 * ==================================================================== */

static void processSetError(loopyProcess *process, const char *field);
static loopyProcessManager *processGetManager(const loopyLoop *loop);
static loopyProcessManager *processCreateManager(loopyLoop *loop);
static void processDestroyManager(loopyProcessManager *mgr);
static void processSigchldHandler(loopyLoop *loop, int signum, void *userData);
static void processCheckChildren(loopyProcessManager *mgr);
static bool processSetupStdio(loopyProcess *process,
                              const loopyProcessOptions *options);
static void processCloseUnusedPipes(loopyProcess *process, bool inChild);

/* ====================================================================
 * Error Management
 * ==================================================================== */

static void processSetError(loopyProcess *process, const char *field) {
    snprintf(process->errorString, sizeof(process->errorString), "%s: %s",
             field, strerror(errno));
}

/* ====================================================================
 * Manager (handles SIGCHLD per loop)
 * ==================================================================== */

static loopyProcessManager *processGetManager(const loopyLoop *loop) {
    loopyProcessManager *mgr = g_managers;
    while (mgr) {
        if (mgr->loop == loop) {
            return mgr;
        }
        mgr = mgr->next;
    }
    return NULL;
}

static loopyProcessManager *processCreateManager(loopyLoop *loop) {
    loopyProcessManager *mgr = processGetManager(loop);
    if (mgr) {
        return mgr;
    }

    mgr = zcalloc(1, sizeof(*mgr));
    if (!mgr) {
        return NULL;
    }

    mgr->loop = loop;

    /* Create SIGCHLD handler */
    mgr->sigchldHandle = loopySignalNew(loop);
    if (!mgr->sigchldHandle) {
        zfree(mgr);
        return NULL;
    }

    if (!loopySignalRegister(mgr->sigchldHandle, SIGCHLD, processSigchldHandler,
                             mgr)) {
        loopySignalFree(mgr->sigchldHandle);
        zfree(mgr);
        return NULL;
    }

    /* Add to global list */
    mgr->next = g_managers;
    g_managers = mgr;

    return mgr;
}

static void processDestroyManager(loopyProcessManager *mgr) {
    if (!mgr) {
        return;
    }

    /* Remove from global list */
    if (g_managers == mgr) {
        g_managers = mgr->next;
    } else {
        loopyProcessManager *prev = g_managers;
        while (prev && prev->next != mgr) {
            prev = prev->next;
        }
        if (prev) {
            prev->next = mgr->next;
        }
    }

    loopySignalFree(mgr->sigchldHandle);
    zfree(mgr);
}

static void processSigchldHandler(loopyLoop *loop, int signum, void *userData) {
    (void)loop;
    (void)signum;

    loopyProcessManager *mgr = userData;
    processCheckChildren(mgr);
}

static void processCheckChildren(loopyProcessManager *mgr) {
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        /* Find the process */
        loopyProcess *process = mgr->processList;
        while (process) {
            if (process->pid == pid) {
                process->exited = true;

                if (WIFEXITED(status)) {
                    process->exitStatus = WEXITSTATUS(status);
                    process->termSignal = 0;
                } else if (WIFSIGNALED(status)) {
                    process->exitStatus = 0;
                    process->termSignal = WTERMSIG(status);
                }

                /* Invoke callback */
                if (process->exitCb) {
                    process->exitCb(mgr->loop, process, process->exitStatus,
                                    process->termSignal, process->userData);
                }
                break;
            }
            process = process->next;
        }
    }
}

/* ====================================================================
 * stdio setup
 * ==================================================================== */

static bool processSetupStdio(loopyProcess *process,
                              const loopyProcessOptions *options) {
    for (int i = 0; i < 3; i++) {
        process->stdioPipes[i][0] = -1;
        process->stdioPipes[i][1] = -1;
    }

    for (int i = 0; i < 3; i++) {
        loopyStdioFlags flags = options->stdio[i].flags;

        if (flags == LOOPY_STDIO_CREATE_PIPE) {
            if (pipe(process->stdioPipes[i]) < 0) {
                processSetError(process, "pipe");
                return false;
            }

            /* Set non-blocking on parent side */
            int parentIdx =
                (i == 0) ? 1 : 0; /* stdin: write end, others: read end */
            int fl = fcntl(process->stdioPipes[i][parentIdx], F_GETFL);
            if (fl >= 0) {
                fcntl(process->stdioPipes[i][parentIdx], F_SETFL,
                      fl | O_NONBLOCK);
            }
        }
    }

    return true;
}

static void processCloseUnusedPipes(loopyProcess *process, bool inChild) {
    for (int i = 0; i < 3; i++) {
        if (inChild) {
            /* In child: close parent's end */
            int parentIdx = (i == 0) ? 1 : 0;
            if (process->stdioPipes[i][parentIdx] >= 0) {
                close(process->stdioPipes[i][parentIdx]);
            }
        } else {
            /* In parent: close child's end */
            int childIdx = (i == 0) ? 0 : 1;
            if (process->stdioPipes[i][childIdx] >= 0) {
                close(process->stdioPipes[i][childIdx]);
                process->stdioPipes[i][childIdx] = -1;
            }
        }
    }
}

/* ====================================================================
 * Lifecycle
 * ==================================================================== */

loopyProcess *loopyProcessSpawn(loopyLoop *loop,
                                const loopyProcessOptions *options,
                                loopyProcessExitCallback *exitCb,
                                void *userData) {
    if (!loop || !options || !options->file) {
        return NULL;
    }

    /* Get or create manager for this loop */
    loopyProcessManager *mgr = processCreateManager(loop);
    if (!mgr) {
        return NULL;
    }

    loopyProcess *process = zcalloc(1, sizeof(*process));
    if (!process) {
        return NULL;
    }

    process->loop = loop;
    process->exitCb = exitCb;
    process->userData = userData;
    process->pid = -1;

    /* Setup stdio pipes */
    if (!processSetupStdio(process, options)) {
        loopyProcessFree(process);
        return NULL;
    }

    /* Fork */
    pid_t pid = fork();
    if (pid < 0) {
        processSetError(process, "fork");
        loopyProcessFree(process);
        return NULL;
    }

    if (pid == 0) {
        /* ========== CHILD PROCESS ========== */

        /* Setup stdio */
        for (int i = 0; i < 3; i++) {
            loopyStdioFlags flags = options->stdio[i].flags;

            if (flags == LOOPY_STDIO_IGNORE) {
                int devnull = open("/dev/null", (i == 0) ? O_RDONLY : O_WRONLY);
                if (devnull >= 0) {
                    dup2(devnull, i);
                    close(devnull);
                }
            } else if (flags == LOOPY_STDIO_CREATE_PIPE) {
                int childIdx =
                    (i == 0) ? 0 : 1; /* stdin: read end, others: write */
                if (process->stdioPipes[i][childIdx] >= 0) {
                    dup2(process->stdioPipes[i][childIdx], i);
                }
            } else if (flags == LOOPY_STDIO_INHERIT_FD) {
                dup2(options->stdio[i].fd, i);
            }
            /* LOOPY_STDIO_INHERIT: do nothing, already inherited */
        }

        /* Close pipe fds that are no longer needed */
        processCloseUnusedPipes(process, true);
        for (int i = 0; i < 3; i++) {
            int childIdx = (i == 0) ? 0 : 1;
            if (process->stdioPipes[i][childIdx] > 2) {
                close(process->stdioPipes[i][childIdx]);
            }
        }

        /* Change directory if requested */
        if (options->cwd) {
            if (chdir(options->cwd) < 0) {
                _exit(127);
            }
        }

        /* Detached process: create new session */
        if (options->flags & LOOPY_PROCESS_DETACHED) {
            setsid();
        }

        /* Set UID/GID */
        if (options->flags & LOOPY_PROCESS_SETGID) {
            setgid(options->gid);
        }
        if (options->flags & LOOPY_PROCESS_SETUID) {
            setuid(options->uid);
        }

        /* Execute */
        if (options->env) {
            execve(options->file, options->args, options->env);
        } else {
            execv(options->file, options->args);
        }

        /* If we get here, exec failed */
        _exit(127);
    }

    /* ========== PARENT PROCESS ========== */
    process->pid = pid;

    /* Close child's end of pipes */
    processCloseUnusedPipes(process, false);

    /* Add to process list */
    process->next = mgr->processList;
    if (mgr->processList) {
        mgr->processList->prev = process;
    }
    mgr->processList = process;

    return process;
}

void loopyProcessFree(loopyProcess *process) {
    if (!process) {
        return;
    }

    /* Close remaining pipes */
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 2; j++) {
            if (process->stdioPipes[i][j] >= 0) {
                close(process->stdioPipes[i][j]);
            }
        }
    }

    /* Remove from process list */
    loopyProcessManager *mgr = processGetManager(process->loop);
    if (mgr) {
        if (process->prev) {
            process->prev->next = process->next;
        } else {
            mgr->processList = process->next;
        }
        if (process->next) {
            process->next->prev = process->prev;
        }

        /* Destroy manager if no more processes */
        if (!mgr->processList) {
            processDestroyManager(mgr);
        }
    }

    zfree(process);
}

/* ====================================================================
 * Control
 * ==================================================================== */

bool loopyProcessKill(loopyProcess *process, int signum) {
    if (!process || process->pid <= 0 || process->exited) {
        return false;
    }

    if (kill(process->pid, signum) < 0) {
        processSetError(process, "kill");
        return false;
    }

    return true;
}

/* ====================================================================
 * Information
 * ==================================================================== */

pid_t loopyProcessGetPid(const loopyProcess *process) {
    return process ? process->pid : -1;
}

int loopyProcessGetStdioPipe(loopyProcess *process, int index) {
    if (!process || index < 0 || index > 2) {
        return -1;
    }

    /* Return parent's end of the pipe */
    int parentIdx = (index == 0) ? 1 : 0;
    return process->stdioPipes[index][parentIdx];
}

bool loopyProcessExited(const loopyProcess *process) {
    return process ? process->exited : true;
}

loopyLoop *loopyProcessGetLoop(const loopyProcess *process) {
    return process ? process->loop : NULL;
}

const char *loopyProcessGetError(const loopyProcess *process) {
    return process ? process->errorString : "";
}

void *loopyProcessGetData(const loopyProcess *process) {
    return process ? process->userData : NULL;
}

void loopyProcessSetData(loopyProcess *process, void *data) {
    if (process) {
        process->userData = data;
    }
}
