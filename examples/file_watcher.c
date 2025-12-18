/* file_watcher.c - File system watching example using loopy
 *
 * This example demonstrates:
 * - Watching files and directories for changes
 * - Handling different event types (modify, create, delete, rename)
 * - Multiple simultaneous watches
 * - Graceful shutdown
 *
 * Build (from build directory):
 *   Already built as part of loopy: ./examples/file_watcher
 *
 * Run:
 *   ./examples/file_watcher /path/to/watch [/another/path] ...
 *   ./examples/file_watcher --test    # Test mode: creates temp dir and
 * verifies watching
 *
 * Test by creating, modifying, or deleting files in watched directories.
 */

#include "../src/loopy.h"
#include "../src/loopySignal.h"
#include "../src/loopyTimer.h"
#include "../src/loopyWatch.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Global state */
static loopyLoop *g_loop = NULL;
static int g_testMode = 0;
static int g_testResult = 0;
static int g_eventsReceived = 0;
static char g_testDir[256] = {0};

/* ====================================================================
 * Helper Functions
 * ==================================================================== */

/* Get string representation of events */
static const char *eventString(loopyWatchEvent events) {
    static char buf[256];
    buf[0] = '\0';

    if (events & LOOPY_WATCH_MODIFY) {
        strcat(buf, "MODIFY ");
    }
    if (events & LOOPY_WATCH_CREATE) {
        strcat(buf, "CREATE ");
    }
    if (events & LOOPY_WATCH_DELETE) {
        strcat(buf, "DELETE ");
    }
    if (events & LOOPY_WATCH_RENAME) {
        strcat(buf, "RENAME ");
    }
    if (events & LOOPY_WATCH_ATTRIB) {
        strcat(buf, "ATTRIB ");
    }

    /* Remove trailing space */
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == ' ') {
        buf[len - 1] = '\0';
    }

    return buf;
}

/* Get current timestamp string */
static const char *timestamp(void) {
    static char buf[32];
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    strftime(buf, sizeof(buf), "%H:%M:%S", tm);
    return buf;
}

/* ====================================================================
 * Callbacks
 * ==================================================================== */

/* Called when a watched file or directory changes */
static void onWatch(loopyWatch *w, const loopyWatchInfo *info) {
    (void)w;

    printf("[%s] ", timestamp());
    printf("Event: %s\n", eventString(info->events));
    printf("         Path: %s\n", info->path);

    if (info->filename && info->filename[0] != '\0') {
        printf("         File: %s\n", info->filename);
    }

    /* Show what type of change occurred */
    if (info->events & LOOPY_WATCH_MODIFY) {
        printf("         -> File content was modified\n");
    }
    if (info->events & LOOPY_WATCH_CREATE) {
        printf("         -> New file/directory was created\n");
    }
    if (info->events & LOOPY_WATCH_DELETE) {
        printf("         -> File/directory was deleted\n");
    }
    if (info->events & LOOPY_WATCH_RENAME) {
        printf("         -> File/directory was renamed\n");
    }
    if (info->events & LOOPY_WATCH_ATTRIB) {
        printf("         -> File attributes changed\n");
    }

    printf("\n");

    /* Track events in test mode */
    if (g_testMode) {
        g_eventsReceived++;
    }
}

/* Test mode: perform file operations to trigger watch events */
static void testCreateFile(loopyLoop *loop, loopyTimer *timer, void *userData) {
    (void)loop;
    (void)timer;
    (void)userData;

    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/testfile.txt", g_testDir);

    printf("[TEST] Creating file: %s\n", filepath);
    int fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, "Hello, World!\n", 14);
        close(fd);
    }
}

static void testModifyFile(loopyLoop *loop, loopyTimer *timer, void *userData) {
    (void)loop;
    (void)timer;
    (void)userData;

    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/testfile.txt", g_testDir);

    printf("[TEST] Modifying file: %s\n", filepath);
    int fd = open(filepath, O_WRONLY | O_APPEND);
    if (fd >= 0) {
        write(fd, "More data!\n", 11);
        close(fd);
    }
}

static void testDeleteFile(loopyLoop *loop, loopyTimer *timer, void *userData) {
    (void)loop;
    (void)timer;
    (void)userData;

    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/testfile.txt", g_testDir);

    printf("[TEST] Deleting file: %s\n", filepath);
    unlink(filepath);
}

static void testComplete(loopyLoop *loop, loopyTimer *timer, void *userData) {
    (void)timer;
    (void)userData;

    printf("[TEST] File operations complete, stopping loop\n");
    loopyStop(loop);
}

/* Signal handler for graceful shutdown */
static void onSignal(loopyLoop *loop, int signum, void *userData) {
    (void)signum;
    (void)userData;

    printf("\nReceived signal, stopping...\n");
    loopyStop(loop);
}

/* ====================================================================
 * Main
 * ==================================================================== */

int main(int argc, char **argv) {
    /* Check for test mode */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--test") == 0) {
            g_testMode = 1;
            break;
        }
    }

    if (!g_testMode && argc < 2) {
        printf("Usage: %s <path> [path] ...\n", argv[0]);
        printf("\nWatch files or directories for changes.\n");
        printf("\nExamples:\n");
        printf("  %s /tmp                    # Watch /tmp directory\n",
               argv[0]);
        printf("  %s /etc/hosts ~/.bashrc    # Watch specific files\n",
               argv[0]);
        printf("  %s . ../other-dir          # Watch multiple directories\n",
               argv[0]);
        printf("  %s --test                  # Test mode\n", argv[0]);
        return 1;
    }

    if (g_testMode) {
        printf("[TEST] Running file watcher test mode\n\n");
    }

    printf("Loopy File Watcher Example\n");
    printf("==========================\n\n");

    /* Create event loop with capacity for 256 file descriptors.
     *
     * File watching can consume multiple FDs depending on the backend:
     * - On Linux (inotify): One FD per watched directory
     * - On macOS/BSD (kqueue): One FD per watched item
     *
     * 256 allows watching many directories simultaneously. Increase this
     * if you plan to watch hundreds of paths, or decrease to 64 if watching
     * just a few files to minimize memory usage.
     */
    loopyLoop *loop = loopyNew(256);
    if (!loop) {
        fprintf(stderr, "Failed to create event loop\n");
        return 1;
    }
    g_loop = loop;

    /* Set up signal handler for graceful shutdown */
    loopySignalHandler *sigHandler = loopySignalNew(loop);
    if (!sigHandler) {
        fprintf(stderr, "Failed to set up signal handler\n");
        loopyDelete(loop);
        return 1;
    }
    loopySignalRegister(sigHandler, SIGINT, onSignal, NULL);

    /* Create file watcher */
    loopyWatch *watcher = loopyWatchNew(loop);
    if (!watcher) {
        fprintf(stderr, "Failed to create file watcher\n");
        loopySignalFree(sigHandler);
        loopyDelete(loop);
        return 1;
    }

    printf("Using %s backend\n", loopyWatchBackendName());
    printf("\n");

    int watchCount = 0;

    if (g_testMode) {
        /* Test mode: create temp directory and watch it */
        snprintf(g_testDir, sizeof(g_testDir), "/tmp/loopy_watch_test_%d",
                 (int)getpid());

        if (mkdir(g_testDir, 0755) != 0) {
            fprintf(stderr, "Failed to create test directory: %s\n", g_testDir);
            loopyWatchFree(watcher);
            loopySignalFree(sigHandler);
            loopyDelete(loop);
            return 1;
        }

        printf("[TEST] Created test directory: %s\n", g_testDir);

        loopyWatchId id =
            loopyWatchAdd(watcher, g_testDir, LOOPY_WATCH_ALL, onWatch, NULL);
        if (id == 0) {
            fprintf(stderr, "Failed to watch test directory\n");
            rmdir(g_testDir);
            loopyWatchFree(watcher);
            loopySignalFree(sigHandler);
            loopyDelete(loop);
            return 1;
        }
        printf("[TEST] Watching test directory [%llu]\n\n",
               (unsigned long long)id);
        watchCount = 1;

        /* Schedule test file operations */
        loopyTimerOneShotMs(loop, 100, testCreateFile, NULL);
        loopyTimerOneShotMs(loop, 300, testModifyFile, NULL);
        loopyTimerOneShotMs(loop, 500, testDeleteFile, NULL);
        loopyTimerOneShotMs(loop, 800, testComplete, NULL);
    } else {
        /* Normal mode: Add watches for each argument */
        for (int i = 1; i < argc; i++) {
            const char *path = argv[i];
            if (strcmp(path, "--test") == 0) {
                continue;
            }

            loopyWatchId id =
                loopyWatchAdd(watcher, path, LOOPY_WATCH_ALL, onWatch, NULL);
            if (id == 0) {
                fprintf(stderr, "Failed to watch: %s\n", path);
            } else {
                printf("Watching [%llu]: %s\n", (unsigned long long)id, path);
                watchCount++;
            }
        }

        if (watchCount == 0) {
            fprintf(stderr, "No paths were successfully watched\n");
            loopyWatchFree(watcher);
            loopySignalFree(sigHandler);
            loopyDelete(loop);
            return 1;
        }

        printf("\n%d path(s) being watched.\n", watchCount);
        printf("Press Ctrl+C to stop.\n\n");
        printf("Try these actions in watched directories:\n");
        printf("  touch newfile.txt    # Create a file\n");
        printf("  echo hi >> file.txt  # Modify a file\n");
        printf("  mv file.txt new.txt  # Rename a file\n");
        printf("  rm file.txt          # Delete a file\n");
        printf("  chmod 644 file.txt   # Change attributes\n");
        printf("\n---\n\n");
    }

    /* Run event loop */
    loopyMain(loop);

    /* Cleanup */
    printf("Cleaning up...\n");
    loopyWatchFree(watcher);
    loopySignalFree(sigHandler);
    loopyDelete(loop);

    /* Test mode: clean up temp directory and check results */
    if (g_testMode) {
        /* Remove any leftover test file */
        char filepath[512];
        snprintf(filepath, sizeof(filepath), "%s/testfile.txt", g_testDir);
        unlink(filepath);

        /* Remove test directory */
        rmdir(g_testDir);
        printf("[TEST] Removed test directory: %s\n", g_testDir);

        /* Verify we received events */
        if (g_eventsReceived >= 2) {
            printf("[TEST] PASS: Received %d file watch events\n",
                   g_eventsReceived);
            g_testResult = 0;
        } else {
            fprintf(stderr, "[TEST] FAIL: Expected at least 2 events, got %d\n",
                    g_eventsReceived);
            g_testResult = 1;
        }
        return g_testResult;
    }

    printf("Goodbye!\n");
    return 0;
}
