/* loopyDNS - Async DNS resolution for loopy event loop
 *
 * Implementation uses a small thread pool to perform blocking getaddrinfo()
 * calls, with results signaled back to the event loop via pipe/eventfd.
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
#include "loopyDNS.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Simple string duplication using zcalloc */
static inline char *dnsStrdup(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t len = strlen(s) + 1;
    char *dup = zcalloc(1, len);
    if (dup) {
        memcpy(dup, s, len);
    }
    return dup;
}

/* Use eventfd on Linux for efficiency, pipe elsewhere */
#if __linux__
#include <sys/eventfd.h>
#define USE_EVENTFD 1
#endif

/* ====================================================================
 * Internal data structures
 * ==================================================================== */

/* Query types */
typedef enum {
    DNS_QUERY_FORWARD = 0,
    DNS_QUERY_REVERSE = 1,
} loopyDNSQueryKind;

typedef struct loopyDNSQuery {
    loopyDNSQueryId id;
    loopyDNSQueryKind kind;
    char *hostname;                 /* For forward: hostname to resolve */
    char addrStr[INET6_ADDRSTRLEN]; /* For reverse: IP address string */
    int port;                       /* For reverse: port number */
    loopyDNSQueryType type;         /* For forward: A, AAAA, ANY */
    union {
        loopyDNSCallback *forward;
        loopyDNSReverseCallback *reverse;
    } callback;
    void *userData;
    bool cancelled;
    struct loopyDNSQuery *next; /* For linked list */
} loopyDNSQuery;

typedef struct loopyDNSCompletedQuery {
    loopyDNSQueryKind kind;
    union {
        loopyDNSResult forward;
        loopyDNSReverseResult reverse;
    } result;
    union {
        loopyDNSCallback *forward;
        loopyDNSReverseCallback *reverse;
    } callback;
    struct loopyDNSCompletedQuery *next;
} loopyDNSCompletedQuery;

struct loopyDNS {
    loopyLoop *loop;
    void *userData; /* User data for handle accessors */
    loopyDNSConfig config;

    /* Thread pool */
    pthread_t *workers;
    size_t workerCount;
    bool shutdown;

    /* Query management */
    loopyDNSQuery *pendingHead;
    loopyDNSQuery *pendingTail;
    size_t pendingCount;
    loopyDNSQueryId nextQueryId;

    /* Completed results queue */
    loopyDNSCompletedQuery *completedHead;
    loopyDNSCompletedQuery *completedTail;

    /* Synchronization */
    pthread_mutex_t mutex;
    pthread_cond_t workAvailable;

    /* Event loop notification */
#if USE_EVENTFD
    int eventFd;
#else
    int notifyPipe[2]; /* [0] = read, [1] = write */
#endif
};

/* ====================================================================
 * Forward declarations
 * ==================================================================== */

static void *dnsWorkerThread(void *arg);
static void dnsNotifyCallback(loopyLoop *l, int fd, void *data,
                              loopyAction mask);
static void dnsProcessCompleted(loopyDNS *dns);
static loopyDNSQuery *dnsDequeueQuery(loopyDNS *dns);
static void dnsEnqueueCompleted(loopyDNS *dns,
                                loopyDNSCompletedQuery *completed);
static void dnsNotifyLoop(loopyDNS *dns);

/* ====================================================================
 * Configuration
 * ==================================================================== */

void loopyDNSConfigInit(loopyDNSConfig *config) {
    if (config) {
        *config = LOOPY_DNS_CONFIG_DEFAULT;
    }
}

/* ====================================================================
 * Lifecycle
 * ==================================================================== */

loopyDNS *loopyDNSNew(loopyLoop *loop, const loopyDNSConfig *config) {
    if (!loop) {
        return NULL;
    }

    loopyDNS *dns = zcalloc(1, sizeof(*dns));
    if (!dns) {
        return NULL;
    }

    dns->loop = loop;
    dns->config = config ? *config : LOOPY_DNS_CONFIG_DEFAULT;
    dns->nextQueryId = 1;

    /* Initialize synchronization */
    if (pthread_mutex_init(&dns->mutex, NULL) != 0) {
        zfree(dns);
        return NULL;
    }

    if (pthread_cond_init(&dns->workAvailable, NULL) != 0) {
        pthread_mutex_destroy(&dns->mutex);
        zfree(dns);
        return NULL;
    }

    /* Create notification mechanism */
#if USE_EVENTFD
    dns->eventFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (dns->eventFd == -1) {
        pthread_cond_destroy(&dns->workAvailable);
        pthread_mutex_destroy(&dns->mutex);
        zfree(dns);
        return NULL;
    }
    int notifyFd = dns->eventFd;
#else
    if (pipe(dns->notifyPipe) == -1) {
        pthread_cond_destroy(&dns->workAvailable);
        pthread_mutex_destroy(&dns->mutex);
        zfree(dns);
        return NULL;
    }
    /* Set non-blocking */
    fcntl(dns->notifyPipe[0], F_SETFL, O_NONBLOCK);
    fcntl(dns->notifyPipe[1], F_SETFL, O_NONBLOCK);
    int notifyFd = dns->notifyPipe[0];
#endif

    /* Register notification fd with event loop */
    if (!loopyRegisterRead(loop, notifyFd, dnsNotifyCallback, dns)) {
#if USE_EVENTFD
        close(dns->eventFd);
#else
        close(dns->notifyPipe[0]);
        close(dns->notifyPipe[1]);
#endif
        pthread_cond_destroy(&dns->workAvailable);
        pthread_mutex_destroy(&dns->mutex);
        zfree(dns);
        return NULL;
    }

    /* Create worker threads */
    dns->workerCount = dns->config.workerThreads;
    dns->workers = zcalloc(dns->workerCount, sizeof(pthread_t));
    if (!dns->workers) {
        loopyUnregisterReadWrite(loop, notifyFd);
#if USE_EVENTFD
        close(dns->eventFd);
#else
        close(dns->notifyPipe[0]);
        close(dns->notifyPipe[1]);
#endif
        pthread_cond_destroy(&dns->workAvailable);
        pthread_mutex_destroy(&dns->mutex);
        zfree(dns);
        return NULL;
    }

    for (size_t i = 0; i < dns->workerCount; i++) {
        if (pthread_create(&dns->workers[i], NULL, dnsWorkerThread, dns) != 0) {
            /* Shutdown already-created threads */
            dns->shutdown = true;
            pthread_cond_broadcast(&dns->workAvailable);
            for (size_t j = 0; j < i; j++) {
                pthread_join(dns->workers[j], NULL);
            }
            loopyUnregisterReadWrite(loop, notifyFd);
#if USE_EVENTFD
            close(dns->eventFd);
#else
            close(dns->notifyPipe[0]);
            close(dns->notifyPipe[1]);
#endif
            pthread_cond_destroy(&dns->workAvailable);
            pthread_mutex_destroy(&dns->mutex);
            zfree(dns->workers);
            zfree(dns);
            return NULL;
        }
    }

    return dns;
}

void loopyDNSFree(loopyDNS *dns) {
    if (!dns) {
        return;
    }

    /* Signal shutdown to workers */
    pthread_mutex_lock(&dns->mutex);
    dns->shutdown = true;
    pthread_cond_broadcast(&dns->workAvailable);
    pthread_mutex_unlock(&dns->mutex);

    /* Wait for workers to finish */
    for (size_t i = 0; i < dns->workerCount; i++) {
        pthread_join(dns->workers[i], NULL);
    }

    /* Unregister from event loop */
#if USE_EVENTFD
    loopyUnregisterReadWrite(dns->loop, dns->eventFd);
    close(dns->eventFd);
#else
    loopyUnregisterReadWrite(dns->loop, dns->notifyPipe[0]);
    close(dns->notifyPipe[0]);
    close(dns->notifyPipe[1]);
#endif

    /* Free pending queries */
    loopyDNSQuery *q = dns->pendingHead;
    while (q) {
        loopyDNSQuery *next = q->next;
        zfree(q->hostname);
        zfree(q);
        q = next;
    }

    /* Free completed queries */
    loopyDNSCompletedQuery *c = dns->completedHead;
    while (c) {
        loopyDNSCompletedQuery *next = c->next;
        if (c->kind == DNS_QUERY_FORWARD) {
            loopyDNSResultFree(&c->result.forward);
        }
        /* Reverse results don't need special cleanup */
        zfree(c);
        c = next;
    }

    pthread_cond_destroy(&dns->workAvailable);
    pthread_mutex_destroy(&dns->mutex);
    zfree(dns->workers);
    zfree(dns);
}

/* ====================================================================
 * Queries
 * ==================================================================== */

loopyDNSQueryId loopyDNSResolve(loopyDNS *dns, const char *hostname,
                                loopyDNSQueryType type, loopyDNSCallback *cb,
                                void *userData) {
    if (!dns || !hostname || !cb) {
        return 0;
    }

    /* Check if at capacity */
    pthread_mutex_lock(&dns->mutex);
    if (dns->pendingCount >= dns->config.maxConcurrent) {
        pthread_mutex_unlock(&dns->mutex);
        return 0;
    }

    /* Create query */
    loopyDNSQuery *query = zcalloc(1, sizeof(*query));
    if (!query) {
        pthread_mutex_unlock(&dns->mutex);
        return 0;
    }

    query->id = dns->nextQueryId++;
    query->kind = DNS_QUERY_FORWARD;
    query->hostname = dnsStrdup(hostname);
    if (!query->hostname) {
        zfree(query);
        pthread_mutex_unlock(&dns->mutex);
        return 0;
    }
    query->type = type;
    query->callback.forward = cb;
    query->userData = userData;

    /* Enqueue */
    if (dns->pendingTail) {
        dns->pendingTail->next = query;
    } else {
        dns->pendingHead = query;
    }
    dns->pendingTail = query;
    dns->pendingCount++;

    loopyDNSQueryId id = query->id;

    /* Wake up a worker */
    pthread_cond_signal(&dns->workAvailable);
    pthread_mutex_unlock(&dns->mutex);

    return id;
}

bool loopyDNSCancel(loopyDNS *dns, loopyDNSQueryId queryId) {
    if (!dns || queryId == 0) {
        return false;
    }

    pthread_mutex_lock(&dns->mutex);

    /* Find and mark as cancelled */
    loopyDNSQuery *q = dns->pendingHead;
    while (q) {
        if (q->id == queryId) {
            q->cancelled = true;
            pthread_mutex_unlock(&dns->mutex);
            return true;
        }
        q = q->next;
    }

    pthread_mutex_unlock(&dns->mutex);
    return false;
}

void loopyDNSCancelAll(loopyDNS *dns) {
    if (!dns) {
        return;
    }

    pthread_mutex_lock(&dns->mutex);
    loopyDNSQuery *q = dns->pendingHead;
    while (q) {
        q->cancelled = true;
        q = q->next;
    }
    pthread_mutex_unlock(&dns->mutex);
}

loopyDNSQueryId loopyDNSReverseLookup(loopyDNS *dns, const char *addr, int port,
                                      loopyDNSReverseCallback *cb,
                                      void *userData) {
    if (!dns || !addr || !cb) {
        return 0;
    }

    /* Validate address format */
    struct in_addr v4;
    struct in6_addr v6;
    int family = 0;
    if (inet_pton(AF_INET, addr, &v4) == 1) {
        family = AF_INET;
    } else if (inet_pton(AF_INET6, addr, &v6) == 1) {
        family = AF_INET6;
    } else {
        return 0; /* Invalid address format */
    }

    /* Check if at capacity */
    pthread_mutex_lock(&dns->mutex);
    if (dns->pendingCount >= dns->config.maxConcurrent) {
        pthread_mutex_unlock(&dns->mutex);
        return 0;
    }

    /* Create query */
    loopyDNSQuery *query = zcalloc(1, sizeof(*query));
    if (!query) {
        pthread_mutex_unlock(&dns->mutex);
        return 0;
    }

    query->id = dns->nextQueryId++;
    query->kind = DNS_QUERY_REVERSE;
    strncpy(query->addrStr, addr, sizeof(query->addrStr) - 1);
    query->addrStr[sizeof(query->addrStr) - 1] = '\0';
    query->port = port;
    query->type = (family == AF_INET) ? LOOPY_DNS_A : LOOPY_DNS_AAAA;
    query->callback.reverse = cb;
    query->userData = userData;

    /* Enqueue */
    if (dns->pendingTail) {
        dns->pendingTail->next = query;
    } else {
        dns->pendingHead = query;
    }
    dns->pendingTail = query;
    dns->pendingCount++;

    loopyDNSQueryId id = query->id;

    /* Wake up a worker */
    pthread_cond_signal(&dns->workAvailable);
    pthread_mutex_unlock(&dns->mutex);

    return id;
}

/* ====================================================================
 * Status
 * ==================================================================== */

size_t loopyDNSPendingCount(const loopyDNS *dns) {
    if (!dns) {
        return 0;
    }
    /* Note: This is a racy read, but acceptable for status queries */
    return dns->pendingCount;
}

const char *loopyDNSStatusString(loopyDNSStatus status) {
    switch (status) {
    case LOOPY_DNS_OK:
        return "OK";
    case LOOPY_DNS_NXDOMAIN:
        return "NXDOMAIN";
    case LOOPY_DNS_SERVFAIL:
        return "SERVFAIL";
    case LOOPY_DNS_TIMEOUT:
        return "TIMEOUT";
    case LOOPY_DNS_CANCELLED:
        return "CANCELLED";
    case LOOPY_DNS_ERROR:
    default:
        return "ERROR";
    }
}

/* ====================================================================
 * Result helpers
 * ==================================================================== */

void loopyDNSResultFree(loopyDNSResult *result) {
    if (!result) {
        return;
    }
    zfree(result->hostname);
    zfree(result->addresses);
    result->hostname = NULL;
    result->addresses = NULL;
    result->addressCount = 0;
}

/* ====================================================================
 * Internal: Worker thread
 * ==================================================================== */

static loopyDNSQuery *dnsDequeueQuery(loopyDNS *dns) {
    /* Must be called with mutex held */
    if (!dns->pendingHead) {
        return NULL;
    }

    loopyDNSQuery *query = dns->pendingHead;
    dns->pendingHead = query->next;
    if (!dns->pendingHead) {
        dns->pendingTail = NULL;
    }
    dns->pendingCount--;
    query->next = NULL;
    return query;
}

static void dnsProcessForwardQuery(loopyDNS *dns, loopyDNSQuery *query,
                                   loopyDNSCompletedQuery *completed) {
    (void)dns;
    completed->kind = DNS_QUERY_FORWARD;
    completed->result.forward.queryId = query->id;
    completed->result.forward.userData = query->userData;
    completed->result.forward.hostname =
        query->hostname; /* Transfer ownership */
    completed->callback.forward = query->callback.forward;

    /* Check if cancelled */
    if (query->cancelled) {
        completed->result.forward.status = LOOPY_DNS_CANCELLED;
        return;
    }

    /* Perform resolution */
    struct addrinfo hints = {0};
    hints.ai_socktype = SOCK_STREAM;
    switch (query->type) {
    case LOOPY_DNS_A:
        hints.ai_family = AF_INET;
        break;
    case LOOPY_DNS_AAAA:
        hints.ai_family = AF_INET6;
        break;
    case LOOPY_DNS_ANY:
    default:
        hints.ai_family = AF_UNSPEC;
        break;
    }

    struct addrinfo *result = NULL;
    int gai_err = getaddrinfo(query->hostname, NULL, &hints, &result);

    if (gai_err != 0) {
        completed->result.forward.gaierrno = gai_err;
        switch (gai_err) {
        case EAI_NONAME:
#ifdef EAI_NODATA
        case EAI_NODATA:
#endif
            completed->result.forward.status = LOOPY_DNS_NXDOMAIN;
            break;
        case EAI_AGAIN:
        case EAI_FAIL:
            completed->result.forward.status = LOOPY_DNS_SERVFAIL;
            break;
        default:
            completed->result.forward.status = LOOPY_DNS_ERROR;
            break;
        }
    } else {
        /* Count addresses */
        size_t count = 0;
        for (struct addrinfo *p = result; p; p = p->ai_next) {
            if (p->ai_family == AF_INET || p->ai_family == AF_INET6) {
                count++;
            }
        }

        if (count > 0) {
            completed->result.forward.addresses =
                zcalloc(count, sizeof(loopyDNSAddress));
            if (completed->result.forward.addresses) {
                size_t i = 0;
                for (struct addrinfo *p = result; p && i < count;
                     p = p->ai_next) {
                    if (p->ai_family == AF_INET) {
                        struct sockaddr_in *sa =
                            (struct sockaddr_in *)p->ai_addr;
                        completed->result.forward.addresses[i].family = AF_INET;
                        completed->result.forward.addresses[i].addr.v4 =
                            sa->sin_addr;
                        inet_ntop(
                            AF_INET, &sa->sin_addr,
                            completed->result.forward.addresses[i].str,
                            sizeof(completed->result.forward.addresses[i].str));
                        i++;
                    } else if (p->ai_family == AF_INET6) {
                        struct sockaddr_in6 *sa =
                            (struct sockaddr_in6 *)p->ai_addr;
                        completed->result.forward.addresses[i].family =
                            AF_INET6;
                        completed->result.forward.addresses[i].addr.v6 =
                            sa->sin6_addr;
                        inet_ntop(
                            AF_INET6, &sa->sin6_addr,
                            completed->result.forward.addresses[i].str,
                            sizeof(completed->result.forward.addresses[i].str));
                        i++;
                    }
                }
                completed->result.forward.addressCount = i;
            }
        }

        freeaddrinfo(result);
        completed->result.forward.status = LOOPY_DNS_OK;
    }
}

static void dnsProcessReverseQuery(loopyDNS *dns, loopyDNSQuery *query,
                                   loopyDNSCompletedQuery *completed) {
    (void)dns;

    completed->kind = DNS_QUERY_REVERSE;
    completed->result.reverse.queryId = query->id;
    completed->result.reverse.userData = query->userData;
    strncpy(completed->result.reverse.addrStr, query->addrStr,
            sizeof(completed->result.reverse.addrStr) - 1);
    completed->callback.reverse = query->callback.reverse;

    /* Check if cancelled */
    if (query->cancelled) {
        completed->result.reverse.status = LOOPY_DNS_CANCELLED;
        return;
    }

    /* Determine address family and build sockaddr */
    struct sockaddr_storage ss;
    socklen_t sslen;
    memset(&ss, 0, sizeof(ss));

    if (query->type == LOOPY_DNS_A) {
        struct sockaddr_in *sa4 = (struct sockaddr_in *)&ss;
        sa4->sin_family = AF_INET;
        sa4->sin_port = htons(query->port);
        inet_pton(AF_INET, query->addrStr, &sa4->sin_addr);
        sslen = sizeof(struct sockaddr_in);
        completed->result.reverse.family = AF_INET;
    } else {
        struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)&ss;
        sa6->sin6_family = AF_INET6;
        sa6->sin6_port = htons(query->port);
        inet_pton(AF_INET6, query->addrStr, &sa6->sin6_addr);
        sslen = sizeof(struct sockaddr_in6);
        completed->result.reverse.family = AF_INET6;
    }

    /* Perform reverse lookup */
    int flags = 0;
    if (query->port == 0) {
        flags |= NI_NUMERICSERV; /* Don't look up service name if port is 0 */
    }

    int gai_err = getnameinfo((struct sockaddr *)&ss, sslen,
                              completed->result.reverse.hostname,
                              sizeof(completed->result.reverse.hostname),
                              completed->result.reverse.service,
                              sizeof(completed->result.reverse.service), flags);

    if (gai_err != 0) {
        completed->result.reverse.gaierrno = gai_err;
        switch (gai_err) {
        case EAI_NONAME:
#ifdef EAI_NODATA
        case EAI_NODATA:
#endif
            completed->result.reverse.status = LOOPY_DNS_NXDOMAIN;
            break;
        case EAI_AGAIN:
        case EAI_FAIL:
            completed->result.reverse.status = LOOPY_DNS_SERVFAIL;
            break;
        default:
            completed->result.reverse.status = LOOPY_DNS_ERROR;
            break;
        }
    } else {
        completed->result.reverse.status = LOOPY_DNS_OK;
    }
}

static void *dnsWorkerThread(void *arg) {
    loopyDNS *dns = arg;

    while (true) {
        pthread_mutex_lock(&dns->mutex);

        /* Wait for work or shutdown */
        while (!dns->pendingHead && !dns->shutdown) {
            pthread_cond_wait(&dns->workAvailable, &dns->mutex);
        }

        if (dns->shutdown) {
            pthread_mutex_unlock(&dns->mutex);
            break;
        }

        /* Dequeue a query */
        loopyDNSQuery *query = dnsDequeueQuery(dns);
        pthread_mutex_unlock(&dns->mutex);

        if (!query) {
            continue;
        }

        /* Allocate result */
        loopyDNSCompletedQuery *completed = zcalloc(1, sizeof(*completed));
        if (!completed) {
            zfree(query->hostname);
            zfree(query);
            continue;
        }

        /* Process based on query type */
        if (query->kind == DNS_QUERY_FORWARD) {
            dnsProcessForwardQuery(dns, query, completed);
        } else {
            dnsProcessReverseQuery(dns, query, completed);
        }

        /* Enqueue result and notify */
        dnsEnqueueCompleted(dns, completed);
        dnsNotifyLoop(dns);

        zfree(query);
    }

    return NULL;
}

/* ====================================================================
 * Internal: Result queue and notification
 * ==================================================================== */

static void dnsEnqueueCompleted(loopyDNS *dns,
                                loopyDNSCompletedQuery *completed) {
    pthread_mutex_lock(&dns->mutex);

    if (dns->completedTail) {
        dns->completedTail->next = completed;
    } else {
        dns->completedHead = completed;
    }
    dns->completedTail = completed;

    pthread_mutex_unlock(&dns->mutex);
}

static void dnsNotifyLoop(loopyDNS *dns) {
#if USE_EVENTFD
    uint64_t val = 1;
    ssize_t unused = write(dns->eventFd, &val, sizeof(val));
    (void)unused;
#else
    char byte = 1;
    ssize_t unused = write(dns->notifyPipe[1], &byte, 1);
    (void)unused;
#endif
}

static void dnsNotifyCallback(loopyLoop *l, int fd, void *data,
                              loopyAction mask) {
    (void)l;
    (void)mask;

    loopyDNS *dns = data;

    /* Drain the notification */
#if USE_EVENTFD
    uint64_t val;
    ssize_t unused = read(fd, &val, sizeof(val));
    (void)unused;
#else
    char buf[64];
    while (read(fd, buf, sizeof(buf)) > 0) {
        /* Drain */
    }
#endif

    dnsProcessCompleted(dns);
}

static void dnsProcessCompleted(loopyDNS *dns) {
    while (true) {
        pthread_mutex_lock(&dns->mutex);
        loopyDNSCompletedQuery *completed = dns->completedHead;
        if (completed) {
            dns->completedHead = completed->next;
            if (!dns->completedHead) {
                dns->completedTail = NULL;
            }
        }
        pthread_mutex_unlock(&dns->mutex);

        if (!completed) {
            break;
        }

        /* Invoke callback based on query kind */
        if (completed->kind == DNS_QUERY_FORWARD) {
            if (completed->callback.forward) {
                completed->callback.forward(dns, &completed->result.forward);
            }
            /* Cleanup forward result */
            loopyDNSResultFree(&completed->result.forward);
        } else {
            if (completed->callback.reverse) {
                completed->callback.reverse(dns, &completed->result.reverse);
            }
            /* Reverse result doesn't need special cleanup (no allocated memory)
             */
        }

        zfree(completed);
    }
}

/* ====================================================================
 * Handle Accessors
 * ==================================================================== */

loopyLoop *loopyDNSGetLoop(const loopyDNS *dns) {
    return dns ? dns->loop : NULL;
}

void *loopyDNSGetData(const loopyDNS *dns) {
    return dns ? dns->userData : NULL;
}

void loopyDNSSetData(loopyDNS *dns, void *data) {
    if (dns) {
        dns->userData = data;
    }
}
