/* loopyTTY - TTY support for loopy event loop
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
#include "loopyTTY.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/* ====================================================================
 * Internal data structures
 * ==================================================================== */

struct loopyTTY {
    loopyLoop *loop;
    void *userData; /* User data for handle accessors */
    int fd;
    loopyStream *stream;
    loopyTTYMode mode;
    struct termios origTermios;
    bool origSaved;
    bool ownsStream;
    struct loopyTTY *next; /* For global tracking */
};

/* Global list of TTYs for reset-all functionality */
static loopyTTY *ttyList = NULL;

/* ====================================================================
 * Internal helpers
 * ==================================================================== */

static void ttyAddToList(loopyTTY *tty) {
    tty->next = ttyList;
    ttyList = tty;
}

static void ttyRemoveFromList(loopyTTY *tty) {
    if (ttyList == tty) {
        ttyList = tty->next;
        return;
    }

    loopyTTY *prev = ttyList;
    while (prev && prev->next != tty) {
        prev = prev->next;
    }
    if (prev) {
        prev->next = tty->next;
    }
}

static bool ttySaveOriginalMode(loopyTTY *tty) {
    if (tty->origSaved) {
        return true;
    }

    if (tcgetattr(tty->fd, &tty->origTermios) < 0) {
        return false;
    }

    tty->origSaved = true;
    return true;
}

/* ====================================================================
 * Lifecycle
 * ==================================================================== */

loopyTTY *loopyTTYNew(loopyLoop *loop, int fd) {
    if (!loop || fd < 0) {
        return NULL;
    }

    if (!isatty(fd)) {
        return NULL;
    }

    loopyTTY *tty = zcalloc(1, sizeof(*tty));
    if (!tty) {
        return NULL;
    }

    tty->loop = loop;
    tty->fd = fd;
    tty->mode = LOOPY_TTY_MODE_NORMAL;

    /* Save original terminal settings */
    if (!ttySaveOriginalMode(tty)) {
        zfree(tty);
        return NULL;
    }

    /* Create stream for the TTY */
    tty->stream = loopyStreamFromFd(loop, fd, LOOPY_STREAM_PIPE);
    if (!tty->stream) {
        zfree(tty);
        return NULL;
    }
    tty->ownsStream = true;

    ttyAddToList(tty);
    return tty;
}

loopyTTY *loopyTTYStdin(loopyLoop *loop) {
    return loopyTTYNew(loop, STDIN_FILENO);
}

loopyTTY *loopyTTYStdout(loopyLoop *loop) {
    return loopyTTYNew(loop, STDOUT_FILENO);
}

loopyTTY *loopyTTYStderr(loopyLoop *loop) {
    return loopyTTYNew(loop, STDERR_FILENO);
}

void loopyTTYFree(loopyTTY *tty) {
    if (!tty) {
        return;
    }

    /* Restore original terminal mode */
    if (tty->origSaved) {
        tcsetattr(tty->fd, TCSANOW, &tty->origTermios);
    }

    ttyRemoveFromList(tty);

    /* Close stream but don't close the fd for stdin/stdout/stderr */
    if (tty->ownsStream && tty->stream) {
        /* Note: Stream will close the fd, which is usually what we want
         * for non-standard fds. For stdin/stdout/stderr, we should
         * reconsider ownership. */
        loopyStreamClose(tty->stream, NULL, NULL);
    }

    zfree(tty);
}

/* ====================================================================
 * Mode Control
 * ==================================================================== */

bool loopyTTYSetMode(loopyTTY *tty, loopyTTYMode mode) {
    if (!tty) {
        return false;
    }

    if (mode == tty->mode) {
        return true;
    }

    struct termios t;
    if (tcgetattr(tty->fd, &t) < 0) {
        return false;
    }

    switch (mode) {
    case LOOPY_TTY_MODE_NORMAL:
        /* Restore original settings */
        if (tty->origSaved) {
            t = tty->origTermios;
        }
        break;

    case LOOPY_TTY_MODE_RAW:
        /* Raw mode: no echo, no canonical processing, no signals */
        t.c_iflag &=
            ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
        t.c_oflag &= ~OPOST;
        t.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
        t.c_cflag &= ~(CSIZE | PARENB);
        t.c_cflag |= CS8;
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
        break;

    case LOOPY_TTY_MODE_IO:
        /* IO mode: like raw but more aggressive for binary transfer */
        t.c_iflag = 0;
        t.c_oflag = 0;
        t.c_lflag = 0;
        t.c_cflag &= ~(CSIZE | PARENB);
        t.c_cflag |= CS8 | CREAD | CLOCAL;
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
        break;
    }

    if (tcsetattr(tty->fd, TCSAFLUSH, &t) < 0) {
        return false;
    }

    tty->mode = mode;
    return true;
}

loopyTTYMode loopyTTYGetMode(const loopyTTY *tty) {
    if (!tty) {
        return LOOPY_TTY_MODE_NORMAL;
    }
    return tty->mode;
}

bool loopyTTYResetMode(loopyTTY *tty) {
    if (!tty || !tty->origSaved) {
        return false;
    }

    if (tcsetattr(tty->fd, TCSANOW, &tty->origTermios) < 0) {
        return false;
    }

    tty->mode = LOOPY_TTY_MODE_NORMAL;
    return true;
}

void loopyTTYResetAll(void) {
    loopyTTY *tty = ttyList;
    while (tty) {
        if (tty->origSaved) {
            tcsetattr(tty->fd, TCSANOW, &tty->origTermios);
            tty->mode = LOOPY_TTY_MODE_NORMAL;
        }
        tty = tty->next;
    }
}

/* ====================================================================
 * Window Size
 * ==================================================================== */

bool loopyTTYGetWinSize(loopyTTY *tty, int *width, int *height) {
    if (!tty) {
        return false;
    }

    struct winsize ws;
    if (ioctl(tty->fd, TIOCGWINSZ, &ws) < 0) {
        return false;
    }

    if (width) {
        *width = ws.ws_col;
    }
    if (height) {
        *height = ws.ws_row;
    }

    return true;
}

/* ====================================================================
 * Stream Integration
 * ==================================================================== */

loopyStream *loopyTTYAsStream(const loopyTTY *tty) {
    if (!tty) {
        return NULL;
    }
    return tty->stream;
}

/* ====================================================================
 * Utility Functions
 * ==================================================================== */

bool loopyTTYIsTTY(int fd) {
    return isatty(fd) != 0;
}

int loopyTTYGetFd(const loopyTTY *tty) {
    if (!tty) {
        return -1;
    }
    return tty->fd;
}

loopyLoop *loopyTTYGetLoop(const loopyTTY *tty) {
    if (!tty) {
        return NULL;
    }
    return tty->loop;
}

void *loopyTTYGetData(const loopyTTY *tty) {
    return tty ? tty->userData : NULL;
}

void loopyTTYSetData(loopyTTY *tty, void *data) {
    if (tty) {
        tty->userData = data;
    }
}
