/* timer_example.c - Timer examples using loopy
 *
 * This example demonstrates:
 * - One-shot timers (fire once)
 * - Periodic timers (fire repeatedly)
 * - Periodic timers with initial delay
 * - Cancelling timers
 * - Timer user data
 *
 * Build (from build directory):
 *   Already built as part of loopy: ./examples/timer_example
 *
 * Run:
 *   ./examples/timer_example
 *   ./examples/timer_example --test    # Test mode: runs quick timer sequence
 */

#include "../src/loopy.h"
#include "../src/loopySignal.h"
#include "../src/loopyTimer.h"

#include "../deps/datakit/src/datakit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Global state */
static loopyLoop *g_loop = NULL;
static int tickCount = 0;
static time_t startTime;
static int g_testMode = 0;
static int g_testResult = 0;
static int g_oneShotFired = 0;
static int g_periodicFired = 0;

/* ====================================================================
 * Timer Callbacks
 * ==================================================================== */

/* One-shot timer - fires once then automatically cleaned up */
static void oneShotCallback(loopyLoop *loop, loopyTimer *timer,
                            void *userData) {
    (void)loop;
    (void)timer;

    const char *label = (const char *)userData;
    time_t elapsed = time(NULL) - startTime;

    printf("[%3lds] One-shot timer fired: %s\n", elapsed, label);
    g_oneShotFired++;
}

/* Periodic timer - fires every interval until cancelled */
static void periodicCallback(loopyLoop *loop, loopyTimer *timer,
                             void *userData) {
    (void)loop;

    const char *label = (const char *)userData;
    time_t elapsed = time(NULL) - startTime;

    tickCount++;
    g_periodicFired++;
    printf("[%3lds] Periodic timer #%d: %s\n", elapsed, tickCount, label);

    /* In test mode, stop after fewer ticks */
    int maxTicks = g_testMode ? 3 : 10;
    if (tickCount >= maxTicks) {
        printf("         -> Reached %d ticks, cancelling periodic timer\n",
               maxTicks);
        loopyTimerCancel(timer);
    }
}

/* Countdown timer - demonstrates modifying behavior based on state */
typedef struct {
    int remaining;
    const char *message;
} CountdownData;

static void countdownCallback(loopyLoop *loop, loopyTimer *timer,
                              void *userData) {
    CountdownData *data = (CountdownData *)userData;
    time_t elapsed = time(NULL) - startTime;

    data->remaining--;

    if (data->remaining > 0) {
        printf("[%3lds] Countdown: %d...\n", elapsed, data->remaining);
    } else {
        printf("[%3lds] Countdown: %s\n", elapsed, data->message);
        loopyTimerCancel(timer);

        /* Stop the event loop after countdown completes */
        printf("\nAll timers complete, stopping event loop.\n");
        loopyStop(loop);

        /* Free countdown data */
        zfree(data);
    }
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
    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--test") == 0) {
            g_testMode = 1;
        }
    }

    if (g_testMode) {
        printf("[TEST] Running timer test mode\n");
    }

    printf("Loopy Timer Example\n");
    printf("===================\n\n");

    /* Create event loop with capacity for 64 file descriptors.
     *
     * For a timer-only application like this example, the capacity is less
     * critical since timers don't consume file descriptor slots - they use
     * an internal timer wheel. We use 64 as a minimal value that still allows
     * adding a few sockets later if needed. For apps that only use timers,
     * even smaller values (16-32) would work fine.
     */
    loopyLoop *loop = loopyNew(64);
    if (!loop) {
        fprintf(stderr, "Failed to create event loop\n");
        return 1;
    }
    g_loop = loop;
    startTime = time(NULL);

    /* Set up signal handler */
    loopySignalHandler *sigHandler = loopySignalNew(loop);
    if (sigHandler) {
        loopySignalRegister(sigHandler, SIGINT, onSignal, NULL);
    }

    printf("Setting up timers...\n\n");

    loopyTimer *oneshot1 = NULL;
    loopyTimer *oneshot2 = NULL;
    loopyTimer *periodic = NULL;
    loopyTimer *delayed = NULL;
    CountdownData *countdown = NULL;

    if (g_testMode) {
        /* Test mode: use fast timers (milliseconds) for quick test */

        /* One-shot timer: fires after 50ms */
        oneshot1 =
            loopyTimerOneShotMs(loop, 50, oneShotCallback, "50ms delayed");
        if (!oneshot1) {
            fprintf(stderr, "Failed to create one-shot timer\n");
        } else {
            printf("Created: One-shot timer (50ms)\n");
        }

        /* Periodic timer: fires every 100ms, stops after 3 ticks */
        periodic =
            loopyTimerPeriodicMs(loop, 100, periodicCallback, "Every 100ms");
        if (!periodic) {
            fprintf(stderr, "Failed to create periodic timer\n");
        } else {
            printf("Created: Periodic timer (every 100ms, stops after 3)\n");
        }

        /* Countdown timer: counts down from 4 at 100ms intervals */
        countdown = zmalloc(sizeof(CountdownData));
        if (countdown) {
            countdown->remaining = 5; /* Will fire 4 times then done */
            countdown->message = "DONE!";

            loopyTimer *countdownTimer =
                loopyTimerPeriodicMs(loop, 100, countdownCallback, countdown);
            if (!countdownTimer) {
                fprintf(stderr, "Failed to create countdown timer\n");
                zfree(countdown);
                countdown = NULL;
            } else {
                printf("Created: Countdown timer (4 ticks at 100ms)\n");
            }
        }
    } else {
        /* Normal mode: use longer intervals for demonstration */

        /* One-shot timer: fires after 2 seconds */
        oneshot1 = loopyTimerOneShotSeconds(loop, 2, oneShotCallback,
                                            "2-second delayed");
        if (!oneshot1) {
            fprintf(stderr, "Failed to create one-shot timer\n");
        } else {
            printf("Created: One-shot timer (2 seconds)\n");
        }

        /* One-shot timer: fires after 5 seconds */
        oneshot2 = loopyTimerOneShotMs(loop, 5000, oneShotCallback,
                                       "5-second delayed");
        if (!oneshot2) {
            fprintf(stderr, "Failed to create one-shot timer\n");
        } else {
            printf("Created: One-shot timer (5 seconds)\n");
        }

        /* Periodic timer: fires every 1 second */
        periodic = loopyTimerPeriodicSeconds(loop, 1, periodicCallback,
                                             "Every second");
        if (!periodic) {
            fprintf(stderr, "Failed to create periodic timer\n");
        } else {
            printf(
                "Created: Periodic timer (every 1 second, stops after 10)\n");
        }

        /* Periodic timer with delay: waits 3 seconds, then fires every 2
         * seconds */
        delayed = loopyTimerPeriodicDelayed(
            loop, 3 * 1000000, /* 3 second initial delay (microseconds) */
            2 * 1000000,       /* 2 second interval (microseconds) */
            periodicCallback, "Delayed periodic");
        if (!delayed) {
            fprintf(stderr, "Failed to create delayed periodic timer\n");
        } else {
            printf(
                "Created: Delayed periodic timer (3s delay, then every 2s)\n");
        }

        /* Countdown timer: counts down from 16 */
        countdown = zmalloc(sizeof(CountdownData));
        if (countdown) {
            countdown->remaining =
                16; /* Will fire 15 times (15, 14, ..., 1, done) */
            countdown->message = "DONE!";

            loopyTimer *countdownTimer = loopyTimerPeriodicSeconds(
                loop, 1, countdownCallback, countdown);
            if (!countdownTimer) {
                fprintf(stderr, "Failed to create countdown timer\n");
                zfree(countdown);
                countdown = NULL;
            } else {
                printf("Created: Countdown timer (15 seconds)\n");
            }
        }
    }

    printf("\nStarting event loop... (press Ctrl+C to stop early)\n\n");

    /* Run the event loop */
    loopyMain(loop);

    /* Cleanup */
    printf("Cleaning up...\n");

    /* Cancel any timers that might still be active */
    /* Note: One-shot timers auto-cleanup after firing */
    /* Note: We manually cancelled periodic and countdown in callbacks */
    if (delayed && loopyTimerIsActive(delayed)) {
        loopyTimerCancel(delayed);
    }
    if (periodic && loopyTimerIsActive(periodic)) {
        loopyTimerCancel(periodic);
    }

    if (sigHandler) {
        loopySignalFree(sigHandler);
    }
    loopyDelete(loop);

    /* Test mode result */
    if (g_testMode) {
        /* Verify timers fired as expected */
        if (g_oneShotFired >= 1 && g_periodicFired >= 3) {
            printf("[TEST] PASS: Timers fired correctly (oneshot=%d, "
                   "periodic=%d)\n",
                   g_oneShotFired, g_periodicFired);
            g_testResult = 0;
        } else {
            fprintf(
                stderr,
                "[TEST] FAIL: Expected oneshot>=1, periodic>=3, got %d, %d\n",
                g_oneShotFired, g_periodicFired);
            g_testResult = 1;
        }
        return g_testResult;
    }

    printf("Goodbye!\n");
    return 0;
}
