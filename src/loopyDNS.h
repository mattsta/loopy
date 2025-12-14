/* loopyDNS - Async DNS resolution for loopy event loop
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
#include <arpa/inet.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdint.h>

/* Forward declarations */
typedef struct loopyDNS loopyDNS;
typedef struct loopyDNSResult loopyDNSResult;
typedef uint64_t loopyDNSQueryId;

/* DNS query types */
typedef enum loopyDNSQueryType {
    LOOPY_DNS_A = 0x01,                           /* IPv4 address */
    LOOPY_DNS_AAAA = 0x02,                        /* IPv6 address */
    LOOPY_DNS_ANY = LOOPY_DNS_A | LOOPY_DNS_AAAA, /* Both A and AAAA */
} loopyDNSQueryType;

/**
 * DNS query result status codes.
 *
 * Indicates the outcome of a DNS query. Common codes reference base loopyStatus
 * values directly. Module-specific codes use the -200 range.
 *
 * @see loopyDNSStatusString() to convert to human-readable string
 */
typedef enum loopyDNSStatus {
    LOOPY_DNS_OK = LOOPY_OK,       /* Query succeeded, addresses resolved */
    LOOPY_DNS_ERROR = LOOPY_ERROR, /* Generic error (check gaierrno) */
    LOOPY_DNS_TIMEOUT =
        LOOPY_TIMEOUT, /* Query timed out (exceeds configured timeout) */
    LOOPY_DNS_CANCELLED =
        LOOPY_CANCELLED, /* Query was cancelled before completion */
    /* Module-specific codes */
    LOOPY_DNS_NXDOMAIN = -200, /* Domain name does not exist (NXDOMAIN) */
    LOOPY_DNS_SERVFAIL = -201, /* DNS server error (SERVFAIL) */
} loopyDNSStatus;

/**
 * A single resolved DNS address.
 *
 * Represents one IP address result from a DNS forward lookup.
 */
typedef struct loopyDNSAddress {
    int family; /* AF_INET for IPv4, AF_INET6 for IPv6 */
    union {
        struct in_addr v4;  /* IPv4 address (when family == AF_INET) */
        struct in6_addr v6; /* IPv6 address (when family == AF_INET6) */
    } addr;
    char str[INET6_ADDRSTRLEN]; /* String representation (e.g., "192.0.2.1" or
                                   "2001:db8::1") */
} loopyDNSAddress;

/**
 * DNS forward lookup result.
 *
 * Contains the result of a forward DNS resolution (hostname -> IP addresses).
 * Passed to the callback after the query completes.
 *
 * Memory ownership: The hostname and addresses arrays are owned by this result
 * and are automatically freed after the callback returns. If you need to keep
 * them, make copies in your callback.
 */
struct loopyDNSResult {
    loopyDNSStatus status;      /* Query result status code */
    loopyDNSQueryId queryId;    /* Query ID that produced this result */
    char *hostname;             /* Original hostname that was queried */
    loopyDNSAddress *addresses; /* Array of resolved IP addresses (owned) */
    size_t addressCount;        /* Number of addresses in the array */
    int gaierrno;   /* getaddrinfo() error code if status is ERROR (see
                       gai_strerror) */
    void *userData; /* User data passed during loopyDNSResolve() */
};

/**
 * Forward DNS resolution callback function.
 *
 * Invoked when a forward DNS resolution (hostname to IP) completes. The
 * callback is always invoked from the event loop thread, making it safe to call
 * loopy APIs.
 *
 * @param dns The DNS resolver handle
 * @param result The query result (addresses and status). Owned by the resolver
 *               and automatically freed after callback returns.
 *
 * @note The callback is always invoked from the event loop thread, never from
 *       a worker thread.
 *
 * @note Memory ownership: hostname and addresses pointers in result are valid
 *       only during the callback. Do NOT save pointers to them. Make copies if
 * needed.
 *
 * @see loopyDNSResolve()
 * @see loopyDNSResult
 */
typedef void loopyDNSCallback(loopyDNS *dns, const loopyDNSResult *result);

/**
 * Configuration for DNS resolver.
 *
 * Controls behavior of the DNS resolver including concurrency limits,
 * timeouts, and thread pool size.
 */
typedef struct loopyDNSConfig {
    size_t maxConcurrent; /* Max concurrent queries (default: 16). Queries
                             beyond this limit fail. */
    uint64_t timeoutMs;   /* Query timeout in ms (default: 5000). Not currently
                             enforced by implementation. */
    size_t workerThreads; /* Number of worker threads (default: 2). Threads
                             perform blocking getaddrinfo(). */
} loopyDNSConfig;

/** Default configuration: 16 concurrent, 5 second timeout, 2 worker threads */
#define LOOPY_DNS_CONFIG_DEFAULT                                               \
    (loopyDNSConfig){.maxConcurrent = 16, .timeoutMs = 5000, .workerThreads = 2}

/**
 * Initialize a DNS config with default values.
 *
 * Convenience function to populate a config struct with sensible defaults.
 *
 * @param config Config struct to initialize (must not be NULL)
 *
 * @note Can also just use LOOPY_DNS_CONFIG_DEFAULT or {0} to let loopyDNSNew()
 *       apply defaults.
 *
 * @code
 * loopyDNSConfig config;
 * loopyDNSConfigInit(&config);
 * config.workerThreads = 4;  // Customize one field
 * loopyDNS *dns = loopyDNSNew(loop, &config);
 * @endcode
 */
void loopyDNSConfigInit(loopyDNSConfig *config);

/* ====================================================================
 * Lifecycle
 * ==================================================================== */

/**
 * Create a new async DNS resolver attached to an event loop.
 *
 * Initializes a DNS resolver with a worker thread pool. The resolver uses
 * blocking getaddrinfo() calls in worker threads to avoid blocking the
 * event loop. Results are delivered to callbacks on the event loop thread.
 *
 * @param loop The event loop to attach to (must not be NULL)
 * @param config Configuration for resolver behavior (use NULL for defaults).
 *               If NULL, LOOPY_DNS_CONFIG_DEFAULT is used.
 * @return A new DNS resolver, or NULL on failure (out of memory, loop is NULL,
 *         thread creation failed)
 *
 * @note The resolver creates a thread pool (default 2 threads) that persist
 *       until loopyDNSFree() is called. Worker threads wake on new queries.
 *
 * @note Thread-safe: Multiple threads can call loopyDNSResolve() and
 *       loopyDNSCancel() on the same resolver.
 *
 * @see loopyDNSFree()
 * @see loopyDNSResolve()
 * @see loopyDNSConfig
 *
 * @code
 * loopyDNSConfig config = LOOPY_DNS_CONFIG_DEFAULT;
 * config.workerThreads = 4;  // Use 4 workers
 * loopyDNS *dns = loopyDNSNew(loop, &config);
 * if (!dns) {
 *     fprintf(stderr, "Failed to create DNS resolver\n");
 *     return -1;
 * }
 * @endcode
 */
loopyDNS *loopyDNSNew(loopyLoop *loop, const loopyDNSConfig *config);

/**
 * Free the DNS resolver and cancel any pending queries.
 *
 * Shuts down worker threads, cancels all pending queries, and frees all
 * associated resources. After this call, the resolver pointer is invalid
 * and must not be used.
 *
 * @param dns The DNS resolver to free (may be NULL, in which case this is a
 * no-op)
 *
 * @note All pending queries are cancelled without invoking their callbacks.
 *
 * @note Worker threads are signaled to shut down and this function waits for
 *       them to exit before returning. This may take a few milliseconds.
 *
 * @see loopyDNSNew()
 * @see loopyDNSCancel()
 *
 * @code
 * loopyDNSFree(dns);
 * dns = NULL;  // Good practice to avoid use-after-free
 * @endcode
 */
void loopyDNSFree(loopyDNS *dns);

/* ====================================================================
 * Queries
 * ==================================================================== */

/**
 * Submit an asynchronous forward DNS query (hostname to IP address).
 *
 * Queues a DNS resolution request. The query is processed by a worker thread
 * using the system's getaddrinfo() function. When complete, the callback
 * is invoked on the event loop thread with the resolved addresses.
 *
 * @param dns The DNS resolver (must not be NULL)
 * @param hostname The hostname to resolve (e.g., "example.com", must not be
 * NULL)
 * @param type Query type: LOOPY_DNS_A (IPv4), LOOPY_DNS_AAAA (IPv6), or
 * LOOPY_DNS_ANY (both)
 * @param cb The callback to invoke when resolution completes (must not be NULL)
 * @param userData User data to pass to callback
 * @return A query ID (non-zero) on success, 0 on failure
 *
 * @retval 0 on failure: dns is NULL, hostname is NULL, cb is NULL, resolver
 *         is at max concurrent queries, or memory allocation failed
 * @retval non-zero query ID on success
 *
 * @note The callback is always invoked on the event loop thread, never from
 *       a worker thread. This makes it safe to call loopy APIs from the
 * callback.
 *
 * @note If a query fails (e.g., due to network issues), the callback is still
 *       invoked with result->status set to indicate the error. Check gaierrno
 *       for the system error code.
 *
 * @note Memory ownership: The hostname and addresses passed to the callback
 *       are valid only during the callback. Make copies if you need to keep
 * them.
 *
 * @note Thread-safe: Can be called from any thread.
 *
 * @see loopyDNSCallback
 * @see loopyDNSResult
 * @see loopyDNSCancel()
 *
 * @code
 * void on_resolved(loopyDNS *dns, const loopyDNSResult *result) {
 *     if (result->status == LOOPY_DNS_OK) {
 *         printf("Resolved %s to:\n", result->hostname);
 *         for (size_t i = 0; i < result->addressCount; i++) {
 *             printf("  %s\n", result->addresses[i].str);
 *         }
 *     } else {
 *         printf("Failed: %s\n", loopyDNSStatusString(result->status));
 *     }
 * }
 *
 * loopyDNSQueryId qid = loopyDNSResolve(dns, "example.com", LOOPY_DNS_ANY,
 *                                       on_resolved, NULL);
 * @endcode
 */
loopyDNSQueryId loopyDNSResolve(loopyDNS *dns, const char *hostname,
                                loopyDNSQueryType type, loopyDNSCallback *cb,
                                void *userData);

/**
 * DNS reverse lookup result.
 *
 * Contains the result of a reverse DNS resolution (IP address -> hostname).
 * Passed to the callback after the query completes.
 */
typedef struct loopyDNSReverseResult {
    loopyDNSStatus status;     /* Query result status code */
    loopyDNSQueryId queryId;   /* Query ID that produced this result */
    char hostname[NI_MAXHOST]; /* Resolved hostname (or IP if reverse lookup
                                  failed) */
    char service[NI_MAXSERV]; /* Resolved service name (if port was provided) */
    int family;               /* AF_INET or AF_INET6 */
    char addrStr[INET6_ADDRSTRLEN]; /* Original IP address string that was
                                       queried */
    int gaierrno;   /* getnameinfo() error code if status is ERROR */
    void *userData; /* User data passed during loopyDNSReverseLookup() */
} loopyDNSReverseResult;

/**
 * Reverse DNS resolution callback function.
 *
 * Invoked when a reverse DNS resolution (IP to hostname) completes. The
 * callback is always invoked from the event loop thread, making it safe to
 * call loopy APIs.
 *
 * @param dns The DNS resolver handle
 * @param result The query result (hostname and status)
 *
 * @note The callback is always invoked from the event loop thread, never from
 *       a worker thread.
 *
 * @see loopyDNSReverseLookup()
 * @see loopyDNSReverseResult
 */
typedef void loopyDNSReverseCallback(loopyDNS *dns,
                                     const loopyDNSReverseResult *result);

/**
 * Submit an asynchronous reverse DNS query (IP address to hostname).
 *
 * Queues a reverse DNS lookup. The query is processed by a worker thread using
 * the system's getnameinfo() function. When complete, the callback is invoked
 * on the event loop thread with the resolved hostname.
 *
 * @param dns The DNS resolver (must not be NULL)
 * @param addr IP address string to look up (e.g., "192.0.2.1" or "2001:db8::1",
 *             must be a valid IPv4 or IPv6 address)
 * @param port Port number to look up service for (0 to skip service lookup)
 * @param cb The callback to invoke when lookup completes (must not be NULL)
 * @param userData User data to pass to callback
 * @return A query ID (non-zero) on success, 0 on failure
 *
 * @retval 0 on failure: dns is NULL, addr is NULL or invalid format, cb is
 * NULL, resolver is at max concurrent queries, or memory allocation failed
 * @retval non-zero query ID on success
 *
 * @note The callback is always invoked on the event loop thread. Safe to call
 *       loopy APIs from the callback.
 *
 * @note If the lookup fails (e.g., no reverse DNS record), the callback is
 * still invoked with result->status set to indicate the error. Check gaierrno
 *       for the system error code.
 *
 * @note Thread-safe: Can be called from any thread.
 *
 * @note The returned hostname in the callback is valid only during the
 * callback. Make copies if you need to keep them.
 *
 * @see loopyDNSReverseCallback
 * @see loopyDNSReverseResult
 * @see loopyDNSCancel()
 *
 * @code
 * void on_reverse_lookup(loopyDNS *dns, const loopyDNSReverseResult *result) {
 *     if (result->status == LOOPY_DNS_OK) {
 *         printf("IP %s resolves to %s\n", result->addrStr, result->hostname);
 *         if (result->service[0]) {
 *             printf("Service: %s\n", result->service);
 *         }
 *     } else {
 *         printf("Reverse lookup failed: %s\n",
 * loopyDNSStatusString(result->status));
 *     }
 * }
 *
 * loopyDNSReverseLookup(dns, "192.0.2.1", 80, on_reverse_lookup, NULL);
 * @endcode
 */
loopyDNSQueryId loopyDNSReverseLookup(loopyDNS *dns, const char *addr, int port,
                                      loopyDNSReverseCallback *cb,
                                      void *userData);

/**
 * Cancel a pending DNS query.
 *
 * Marks a query for cancellation. If the query is still pending, it will be
 * cancelled and the callback will not be invoked. If the query is already
 * being processed, the cancellation may fail.
 *
 * @param dns The DNS resolver (must not be NULL)
 * @param queryId The query ID returned by loopyDNSResolve() or
 * loopyDNSReverseLookup()
 * @return true if the query was cancelled, false if it doesn't exist or
 * couldn't be cancelled
 *
 * @note Thread-safe: Can be called from any thread.
 *
 * @note Once cancelled, the query ID becomes invalid and should not be used
 * again.
 *
 * @see loopyDNSResolve()
 * @see loopyDNSReverseLookup()
 *
 * @code
 * loopyDNSQueryId qid = loopyDNSResolve(dns, "example.com", LOOPY_DNS_ANY, cb,
 * NULL);
 * // ... later ...
 * if (still_needed == false) {
 *     loopyDNSCancel(dns, qid);
 * }
 * @endcode
 */
bool loopyDNSCancel(loopyDNS *dns, loopyDNSQueryId queryId);

/**
 * Cancel all pending DNS queries.
 *
 * Marks all pending queries for cancellation. Queries being processed by
 * worker threads will continue to completion, but their callbacks will not
 * be invoked.
 *
 * @param dns The DNS resolver (if NULL, this is a no-op)
 *
 * @note Thread-safe: Can be called from any thread.
 *
 * @note After calling this, loopyDNSPendingCount() will return 0 and no
 *       more callbacks will be invoked from previously pending queries.
 *
 * @see loopyDNSCancel()
 * @see loopyDNSPendingCount()
 *
 * @code
 * loopyDNSCancelAll(dns);  // Stop all pending lookups
 * @endcode
 */
void loopyDNSCancelAll(loopyDNS *dns);

/* ====================================================================
 * Status
 * ==================================================================== */

/**
 * Get the number of pending DNS queries.
 *
 * Returns the count of queries that are currently pending (queued or
 * being processed). Note this is a racy read in multi-threaded environments
 * but acceptable for status monitoring.
 *
 * @param dns The DNS resolver
 * @return Number of pending queries, or 0 if dns is NULL
 *
 * @note This is an approximate count suitable for monitoring but not for
 *       strict synchronization.
 *
 * @code
 * size_t pending = loopyDNSPendingCount(dns);
 * printf("Queries in progress: %zu\n", pending);
 * @endcode
 */
size_t loopyDNSPendingCount(const loopyDNS *dns);

/**
 * Get the human-readable name of a DNS status code.
 *
 * Converts a status code to a descriptive string for logging or error messages.
 *
 * @param status A loopyDNSStatus value
 * @return A string like "OK", "NXDOMAIN", "SERVFAIL", "ERROR", etc.
 *
 * @note The returned string is a static constant and should not be freed.
 *
 * @see loopyDNSStatus
 *
 * @code
 * const char *status_str = loopyDNSStatusString(result->status);
 * printf("Query status: %s\n", status_str);
 * @endcode
 */
const char *loopyDNSStatusString(loopyDNSStatus status);

/* ====================================================================
 * Handle Accessors
 * ==================================================================== */

/**
 * Get the event loop associated with this DNS resolver.
 *
 * @param dns The DNS resolver
 * @return The event loop passed to loopyDNSNew(), or NULL if dns is NULL
 *
 * @see loopyDNSNew()
 */
loopyLoop *loopyDNSGetLoop(const loopyDNS *dns);

/**
 * Get user data from the DNS resolver.
 *
 * Retrieves user-provided data that was set with loopyDNSSetData().
 *
 * @param dns The DNS resolver
 * @return The user data previously set, or NULL if no data was set or dns is
 * NULL
 *
 * @see loopyDNSSetData()
 *
 * @code
 * void *data = loopyDNSGetData(dns);
 * if (data) {
 *     AppConfig *cfg = data;
 *     // Use config
 * }
 * @endcode
 */
void *loopyDNSGetData(const loopyDNS *dns);

/**
 * Set user data on the DNS resolver.
 *
 * Associates arbitrary user data with this resolver for later retrieval.
 *
 * @param dns The DNS resolver (if NULL, this is a no-op)
 * @param data User data pointer (may be NULL)
 *
 * @see loopyDNSGetData()
 *
 * @code
 * AppConfig *cfg = malloc(sizeof(AppConfig));
 * loopyDNSSetData(dns, cfg);
 * @endcode
 */
void loopyDNSSetData(loopyDNS *dns, void *data);

/* ====================================================================
 * Result helpers
 * ==================================================================== */

/**
 * Free a forward DNS result.
 *
 * Deallocates memory associated with a loopyDNSResult. This is normally
 * called automatically after the callback returns, but is available for
 * cases where you've made a copy of the result or need manual cleanup.
 *
 * @param result The result to free (may be NULL, in which case this is a no-op)
 *
 * @note Calling this on results returned by callbacks is NOT necessary, as
 *       the resolver automatically cleans up after the callback. Only call
 *       this if you've explicitly allocated or copied a result.
 *
 * @note After calling this, the hostname and addresses pointers are freed
 *       and must not be accessed.
 *
 * @see loopyDNSResult
 * @see loopyDNSCallback
 *
 * @code
 * // Only needed if you allocated or copied the result yourself
 * loopyDNSResult *copy = malloc(sizeof(loopyDNSResult));
 * // ... populate copy ...
 * loopyDNSResultFree(copy);
 * free(copy);
 * @endcode
 */
void loopyDNSResultFree(loopyDNSResult *result);
