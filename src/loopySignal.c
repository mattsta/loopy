/* loopySignal - Safe signal handling for loopy event loop
 *
 * Uses signalfd on Linux, self-pipe trick on other platforms.
 * Signals are captured by a signal handler that writes to a pipe/signalfd,
 * which is then read in the event loop context where the user callback runs.
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
#include "loopySignal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Use signalfd on Linux for efficiency */
#if __linux__
#include <sys/signalfd.h>
#define USE_SIGNALFD 1
#endif

/* Maximum number of signals we track (NSIG or reasonable default) */
#ifndef NSIG
#define NSIG 64
#endif
#define MAX_SIGNALS NSIG

/* ====================================================================
 * Internal data structures
 * ==================================================================== */

typedef struct loopySignalEntry {
    loopySignalCallback *callback;
    void *userData;
    struct sigaction oldAction; /* For restoring on unregister */
    bool registered;
    bool oneshot; /* Auto-unregister after first callback */
} loopySignalEntry;

struct loopySignalHandler {
    loopyLoop *loop;
    void *userData; /* User data for handle accessors */
    loopySignalEntry signals[MAX_SIGNALS];

#if USE_SIGNALFD
    int signalFd;
    sigset_t mask;
#else
    int selfPipe[2]; /* [0] = read, [1] = write */
#endif
};

/* Global pointer to signal handler for use in signal handler function */
static loopySignalHandler *g_signalHandler = NULL;

/* ====================================================================
 * Forward declarations
 * ==================================================================== */

static void signalEventCallback(loopyLoop *l, int fd, void *data,
                                loopyAction mask);

#if !USE_SIGNALFD
static void signalCatcher(int signum);
#endif

/* ====================================================================
 * Lifecycle
 * ==================================================================== */

loopySignalHandler *loopySignalNew(loopyLoop *loop) {
    if (!loop) {
        return NULL;
    }

    /* Only one signal handler per process (due to global state) */
    if (g_signalHandler) {
        return NULL;
    }

    loopySignalHandler *sh = zcalloc(1, sizeof(*sh));
    if (!sh) {
        return NULL;
    }

    sh->loop = loop;

#if USE_SIGNALFD
    /* Initialize empty signal set */
    sigemptyset(&sh->mask);

    /* Create signalfd with no signals initially */
    sh->signalFd = signalfd(-1, &sh->mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sh->signalFd == -1) {
        zfree(sh);
        return NULL;
    }

    /* Register with event loop */
    if (!loopyRegisterRead(loop, sh->signalFd, signalEventCallback, sh)) {
        close(sh->signalFd);
        zfree(sh);
        return NULL;
    }
#else
    /* Create self-pipe */
    if (pipe(sh->selfPipe) == -1) {
        zfree(sh);
        return NULL;
    }

    /* Set non-blocking and close-on-exec */
    fcntl(sh->selfPipe[0], F_SETFL, O_NONBLOCK);
    fcntl(sh->selfPipe[1], F_SETFL, O_NONBLOCK);
    fcntl(sh->selfPipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(sh->selfPipe[1], F_SETFD, FD_CLOEXEC);

    /* Register read end with event loop */
    if (!loopyRegisterRead(loop, sh->selfPipe[0], signalEventCallback, sh)) {
        close(sh->selfPipe[0]);
        close(sh->selfPipe[1]);
        zfree(sh);
        return NULL;
    }
#endif

    g_signalHandler = sh;
    return sh;
}

void loopySignalFree(loopySignalHandler *sh) {
    if (!sh) {
        return;
    }

    /* Unregister all signals and restore old handlers */
    for (int i = 0; i < MAX_SIGNALS; i++) {
        if (sh->signals[i].registered) {
            loopySignalUnregister(sh, i);
        }
    }

#if USE_SIGNALFD
    loopyUnregisterReadWrite(sh->loop, sh->signalFd);
    close(sh->signalFd);
#else
    loopyUnregisterReadWrite(sh->loop, sh->selfPipe[0]);
    close(sh->selfPipe[0]);
    close(sh->selfPipe[1]);
#endif

    g_signalHandler = NULL;
    zfree(sh);
}

/* ====================================================================
 * Signal Registration
 * ==================================================================== */

/* Internal helper for registering signals */
static bool signalRegisterInternal(loopySignalHandler *sh, int signum,
                                   loopySignalCallback *cb, void *userData,
                                   bool oneshot) {
    if (!sh || !cb || signum < 0 || signum >= MAX_SIGNALS) {
        return false;
    }

    /* Don't allow re-registering */
    if (sh->signals[signum].registered) {
        return false;
    }

#if USE_SIGNALFD
    /* Add signal to mask */
    sigaddset(&sh->mask, signum);

    /* Block signal from default handling */
    sigset_t blockSet;
    sigemptyset(&blockSet);
    sigaddset(&blockSet, signum);
    if (sigprocmask(SIG_BLOCK, &blockSet, NULL) == -1) {
        sigdelset(&sh->mask, signum);
        return false;
    }

    /* Update signalfd */
    if (signalfd(sh->signalFd, &sh->mask, SFD_NONBLOCK | SFD_CLOEXEC) == -1) {
        sigprocmask(SIG_UNBLOCK, &blockSet, NULL);
        sigdelset(&sh->mask, signum);
        return false;
    }
#else
    /* Install signal handler */
    struct sigaction sa = {0};
    sa.sa_handler = signalCatcher;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);

    if (sigaction(signum, &sa, &sh->signals[signum].oldAction) == -1) {
        return false;
    }
#endif

    sh->signals[signum].callback = cb;
    sh->signals[signum].userData = userData;
    sh->signals[signum].registered = true;
    sh->signals[signum].oneshot = oneshot;

    return true;
}

bool loopySignalRegister(loopySignalHandler *sh, int signum,
                         loopySignalCallback *cb, void *userData) {
    return signalRegisterInternal(sh, signum, cb, userData, false);
}

bool loopySignalRegisterOneshot(loopySignalHandler *sh, int signum,
                                loopySignalCallback *cb, void *userData) {
    return signalRegisterInternal(sh, signum, cb, userData, true);
}

bool loopySignalUnregister(loopySignalHandler *sh, int signum) {
    if (!sh || signum < 0 || signum >= MAX_SIGNALS) {
        return false;
    }

    if (!sh->signals[signum].registered) {
        return false;
    }

#if USE_SIGNALFD
    /* Remove signal from mask */
    sigdelset(&sh->mask, signum);

    /* Unblock signal */
    sigset_t blockSet;
    sigemptyset(&blockSet);
    sigaddset(&blockSet, signum);
    sigprocmask(SIG_UNBLOCK, &blockSet, NULL);

    /* Update signalfd */
    signalfd(sh->signalFd, &sh->mask, SFD_NONBLOCK | SFD_CLOEXEC);
#else
    /* Restore old signal handler */
    sigaction(signum, &sh->signals[signum].oldAction, NULL);
#endif

    sh->signals[signum].callback = NULL;
    sh->signals[signum].userData = NULL;
    sh->signals[signum].registered = false;

    return true;
}

/* ====================================================================
 * Utility
 * ==================================================================== */

const char *loopySignalName(int signum) {
    switch (signum) {
    case SIGABRT:
        return "SIGABRT";
    case SIGALRM:
        return "SIGALRM";
    case SIGBUS:
        return "SIGBUS";
    case SIGCHLD:
        return "SIGCHLD";
    case SIGCONT:
        return "SIGCONT";
    case SIGFPE:
        return "SIGFPE";
    case SIGHUP:
        return "SIGHUP";
    case SIGILL:
        return "SIGILL";
    case SIGINT:
        return "SIGINT";
    case SIGKILL:
        return "SIGKILL";
    case SIGPIPE:
        return "SIGPIPE";
    case SIGQUIT:
        return "SIGQUIT";
    case SIGSEGV:
        return "SIGSEGV";
    case SIGSTOP:
        return "SIGSTOP";
    case SIGTERM:
        return "SIGTERM";
    case SIGTSTP:
        return "SIGTSTP";
    case SIGTTIN:
        return "SIGTTIN";
    case SIGTTOU:
        return "SIGTTOU";
    case SIGUSR1:
        return "SIGUSR1";
    case SIGUSR2:
        return "SIGUSR2";
#ifdef SIGPOLL
    case SIGPOLL:
        return "SIGPOLL";
#endif
    case SIGPROF:
        return "SIGPROF";
    case SIGSYS:
        return "SIGSYS";
    case SIGTRAP:
        return "SIGTRAP";
    case SIGURG:
        return "SIGURG";
    case SIGVTALRM:
        return "SIGVTALRM";
    case SIGXCPU:
        return "SIGXCPU";
    case SIGXFSZ:
        return "SIGXFSZ";
    default:
        return "UNKNOWN";
    }
}

/* ====================================================================
 * Internal: Event callback and signal catcher
 * ==================================================================== */

static void signalEventCallback(loopyLoop *l, int fd, void *data,
                                loopyAction mask) {
    (void)l;
    (void)mask;

    loopySignalHandler *sh = data;

#if USE_SIGNALFD
    struct signalfd_siginfo info;
    while (read(fd, &info, sizeof(info)) == sizeof(info)) {
        int signum = info.ssi_signo;
        if (signum >= 0 && signum < MAX_SIGNALS &&
            sh->signals[signum].registered) {
            /* Save oneshot flag before callback (callback might re-register) */
            bool isOneshot = sh->signals[signum].oneshot;
            sh->signals[signum].callback(sh->loop, signum,
                                         sh->signals[signum].userData);
            /* Auto-unregister oneshot signals */
            if (isOneshot && sh->signals[signum].registered) {
                loopySignalUnregister(sh, signum);
            }
        }
    }
#else
    /* Read signal numbers from pipe */
    uint8_t signum;
    while (read(fd, &signum, sizeof(signum)) == sizeof(signum)) {
        if (signum < MAX_SIGNALS && sh->signals[signum].registered) {
            /* Save oneshot flag before callback (callback might re-register) */
            bool isOneshot = sh->signals[signum].oneshot;
            sh->signals[signum].callback(sh->loop, signum,
                                         sh->signals[signum].userData);
            /* Auto-unregister oneshot signals */
            if (isOneshot && sh->signals[signum].registered) {
                loopySignalUnregister(sh, signum);
            }
        }
    }
#endif
}

#if !USE_SIGNALFD
/* Signal handler that writes to self-pipe (async-signal-safe) */
static void signalCatcher(int signum) {
    if (g_signalHandler && signum < MAX_SIGNALS) {
        uint8_t sig = (uint8_t)signum;
        /* write() is async-signal-safe, ignore errors (best effort) */
        ssize_t unused = write(g_signalHandler->selfPipe[1], &sig, sizeof(sig));
        (void)unused;
    }
}
#endif

/* ====================================================================
 * Handle Accessors
 * ==================================================================== */

loopyLoop *loopySignalGetLoop(const loopySignalHandler *sh) {
    return sh ? sh->loop : NULL;
}

void *loopySignalGetData(const loopySignalHandler *sh) {
    return sh ? sh->userData : NULL;
}

void loopySignalSetData(loopySignalHandler *sh, void *data) {
    if (sh) {
        sh->userData = data;
    }
}
