/* dns_lookup.c - Async DNS resolution example using loopy
 *
 * This example demonstrates:
 * - Async DNS resolution with loopyDNS
 * - Handling multiple concurrent queries
 * - Both forward (hostname->IP) and reverse (IP->hostname) lookups
 * - Timeout and error handling
 *
 * Build (from build directory):
 *   Already built as part of loopy: ./examples/dns_lookup
 *
 * Run:
 *   ./examples/dns_lookup google.com github.com 8.8.8.8
 *   ./examples/dns_lookup --test    # Test mode: resolves localhost
 */

#include "loopy.h"
#include "loopyDNS.h"
#include "loopyTimer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Track pending queries for clean shutdown */
static int pendingQueries = 0;
static loopyLoop *g_loop = NULL;
static int g_testMode = 0;
static int g_testResult = 0;
static int g_successfulQueries = 0;

/* ====================================================================
 * Callbacks
 * ==================================================================== */

/* Called when forward DNS lookup completes */
static void onResolve(loopyDNS *dns, const loopyDNSResult *result) {
    (void)dns;

    printf("\n--- Forward lookup: %s ---\n", result->hostname);

    if (result->status != LOOPY_DNS_OK) {
        printf("  Status: %s\n", loopyDNSStatusString(result->status));
        if (result->gaierrno != 0) {
            printf("  Error: %s\n", gai_strerror(result->gaierrno));
        }
    } else {
        printf("  Found %zu address(es):\n", result->addressCount);
        for (size_t i = 0; i < result->addressCount; i++) {
            const loopyDNSAddress *addr = &result->addresses[i];
            printf("    [%zu] %s (%s)\n", i + 1, addr->str,
                   addr->family == AF_INET ? "IPv4" : "IPv6");
        }
        g_successfulQueries++;
    }

    pendingQueries--;
    if (pendingQueries == 0) {
        printf("\nAll queries complete.\n");
        loopyStop(g_loop);
    }
}

/* Called when reverse DNS lookup completes */
static void onReverseLookup(loopyDNS *dns,
                            const loopyDNSReverseResult *result) {
    (void)dns;

    printf("\n--- Reverse lookup: %s ---\n", result->addrStr);

    if (result->status != LOOPY_DNS_OK) {
        printf("  Status: %s\n", loopyDNSStatusString(result->status));
        if (result->gaierrno != 0) {
            printf("  Error: %s\n", gai_strerror(result->gaierrno));
        }
    } else {
        printf("  Hostname: %s\n", result->hostname);
        if (result->service[0] != '\0') {
            printf("  Service: %s\n", result->service);
        }
    }

    pendingQueries--;
    if (pendingQueries == 0) {
        printf("\nAll queries complete.\n");
        loopyStop(g_loop);
    }
}

/* Timeout callback - stop if queries are taking too long */
static void onTimeout(loopyLoop *loop, loopyTimer *timer, void *userData) {
    (void)timer;
    (void)userData;

    fprintf(stderr, "\nTimeout! %d queries still pending.\n", pendingQueries);
    loopyStop(loop);
}

/* Helper to check if string looks like an IP address */
static bool isIpAddress(const char *str) {
    /* Simple check: contains only digits, dots, and colons */
    for (const char *p = str; *p; p++) {
        if (*p != '.' && *p != ':' && (*p < '0' || *p > '9') &&
            (*p < 'a' || *p > 'f') && (*p < 'A' || *p > 'F')) {
            return false;
        }
    }
    /* Must contain at least one dot or colon */
    return strchr(str, '.') != NULL || strchr(str, ':') != NULL;
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
        printf("Usage: %s <hostname|ip> [hostname|ip] ...\n", argv[0]);
        printf("\nExamples:\n");
        printf("  %s google.com\n", argv[0]);
        printf("  %s github.com cloudflare.com\n", argv[0]);
        printf("  %s 8.8.8.8              # reverse lookup\n", argv[0]);
        printf("  %s google.com 8.8.4.4   # mixed\n", argv[0]);
        printf("  %s --test              # test mode\n", argv[0]);
        return 1;
    }

    if (g_testMode) {
        printf("[TEST] Running DNS lookup test mode\n");
    }

    printf("Loopy DNS Lookup Example\n");
    printf("========================\n");

    /* Create event loop with capacity for 64 file descriptors.
     *
     * DNS resolution uses a thread pool internally, not one FD per query.
     * The main event loop only needs FDs for:
     * - Internal async notification pipe (for thread->main communication)
     * - Signal handler (optional)
     *
     * 64 is more than enough for a DNS client. The DNS subsystem handles
     * thousands of concurrent queries efficiently regardless of this value.
     */
    loopyLoop *loop = loopyNew(64);
    if (!loop) {
        fprintf(stderr, "Failed to create event loop\n");
        return 1;
    }
    g_loop = loop;

    /* Create DNS resolver with custom configuration.
     *
     * DNS resolution is done via a thread pool (getaddrinfo is blocking).
     * These config values control resource usage and behavior:
     */
    loopyDNSConfig config;
    loopyDNSConfigInit(&config);
    config.timeoutMs = 10000; /* 10 seconds max wait per query before failure */
    config.maxConcurrent =
        32; /* Max queries in-flight at once (queued if exceeded) */
    config.workerThreads =
        4; /* Thread pool size for parallel getaddrinfo calls */

    loopyDNS *dns = loopyDNSNew(loop, &config);
    if (!dns) {
        fprintf(stderr, "Failed to create DNS resolver\n");
        loopyDelete(loop);
        return 1;
    }

    printf("DNS resolver created with %zu max concurrent queries\n",
           config.maxConcurrent);

    /* In test mode, just resolve localhost */
    if (g_testMode) {
        printf("Submitting test query for: localhost\n");
        loopyDNSQueryId id =
            loopyDNSResolve(dns, "localhost", LOOPY_DNS_ANY, onResolve, NULL);
        if (id == 0) {
            fprintf(stderr, "Failed to submit query for localhost\n");
            g_testResult = 1;
        } else {
            pendingQueries++;
        }
    } else {
        /* Submit queries for each argument */
        for (int i = 1; i < argc; i++) {
            const char *arg = argv[i];
            if (strcmp(arg, "--test") == 0) {
                continue;
            }

            loopyDNSQueryId id;

            if (isIpAddress(arg)) {
                /* Looks like an IP - do reverse lookup */
                printf("Submitting reverse lookup for: %s\n", arg);
                id = loopyDNSReverseLookup(dns, arg, 0, onReverseLookup, NULL);
            } else {
                /* Hostname - do forward lookup */
                printf("Submitting forward lookup for: %s\n", arg);
                id = loopyDNSResolve(dns, arg, LOOPY_DNS_ANY, onResolve, NULL);
            }

            if (id == 0) {
                fprintf(stderr, "Failed to submit query for: %s\n", arg);
            } else {
                pendingQueries++;
            }
        }
    }

    if (pendingQueries == 0) {
        fprintf(stderr, "No queries submitted\n");
        loopyDNSFree(dns);
        loopyDelete(loop);
        return 1;
    }

    /* Set overall timeout */
    loopyTimer *timeout = loopyTimerOneShotSeconds(loop, 30, onTimeout, NULL);
    if (!timeout) {
        fprintf(stderr, "Warning: Failed to set timeout timer\n");
    }

    printf("\nWaiting for %d queries...\n", pendingQueries);

    /* Run event loop until all queries complete or timeout */
    loopyMain(loop);

    /* Cleanup */
    if (timeout) {
        loopyTimerCancel(timeout);
    }
    loopyDNSFree(dns);
    loopyDelete(loop);

    if (g_testMode) {
        if (g_successfulQueries > 0) {
            printf("[TEST] PASS: DNS resolution successful (%d queries)\n",
                   g_successfulQueries);
            return 0;
        } else {
            fprintf(stderr, "[TEST] FAIL: No successful DNS queries\n");
            return 1;
        }
    }

    return g_testResult;
}
