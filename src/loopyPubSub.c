/* loopyPubSub - Topic-based publish/subscribe with rax routing
 *
 * Copyright 2024 Matt Stancliff <matt@genges.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 */

#include "loopyPlatform.h"

#include "loopyAsync.h"
#include "loopyPubSub.h"

#include "../deps/datakit/src/datakit.h"
#include "../deps/rax/src/rax_topic.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

/* ====================================================================
 * Internal Structures
 * ==================================================================== */

/* Message queue entry */
typedef struct loopyMessageEntry {
    struct loopyMessageEntry *next;
    char *topic;
    void *data;
    size_t len;
    uint64_t timestamp;
    uint64_t sequence;
    void *publisherData;
    bool acked;
} loopyMessageEntry;

/* Subscription structure */
struct loopySubscription {
    loopyPubSub *ps;
    char *pattern;
    loopyMessageCallback *callback;
    loopySubscriptionEventCallback *eventCallback;
    void *userData;
    void *eventUserData;

    loopyDeliveryMode deliveryMode;
    loopyAckMode ackMode;
    size_t queueSize;
    bool dropOnFull;

    /* Message queue for LOOPY_DELIVER_QUEUE mode */
    loopyMessageEntry *queueHead;
    loopyMessageEntry *queueTail;
    size_t queueLen;
    pthread_mutex_t queueMutex;

    /* Stats */
    uint64_t messagesReceived;
    uint64_t messagesDropped;

    /* Linked list of subscriptions for same pattern */
    struct loopySubscription *nextSamePattern;
};

/* Pattern entry in the rax tree - holds list of subscriptions */
typedef struct loopyPatternEntry {
    char *pattern;
    loopySubscription *subscriptions;
    size_t count;
} loopyPatternEntry;

/* Pub/sub hub structure */
struct loopyPubSub {
    loopyLoop *loop;
    raxTopic *topics;

    /* Configuration */
    loopyPubSubConfig config;

    /* Thread safety */
    pthread_rwlock_t lock;

    /* Global sequence counter */
    atomic_uint_least64_t sequence;

    /* Statistics */
    atomic_uint_least64_t messagesPublished;
    atomic_uint_least64_t messagesDelivered;
    atomic_uint_least64_t messagesDropped;
    atomic_uint_least64_t bytesPublished;
    atomic_uint_least64_t bytesDelivered;
    atomic_size_t subscriptionCount;
    atomic_size_t patternCount;
};

/* ====================================================================
 * Utilities
 * ==================================================================== */

static uint64_t getMonotonicNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static char *pubsubStrdup(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t len = strlen(s) + 1;
    char *copy = zmalloc(len);
    if (copy) {
        memcpy(copy, s, len);
    }
    return copy;
}

static loopyPatternEntry *patternEntryNew(const char *pattern) {
    loopyPatternEntry *entry = zcalloc(1, sizeof(loopyPatternEntry));
    if (!entry) {
        return NULL;
    }

    entry->pattern = pubsubStrdup(pattern);
    if (!entry->pattern) {
        zfree(entry);
        return NULL;
    }

    return entry;
}

static void patternEntryFree(loopyPatternEntry *entry) {
    if (!entry) {
        return;
    }
    zfree(entry->pattern);
    zfree(entry);
}

static void messageEntryFree(loopyMessageEntry *entry) {
    if (!entry) {
        return;
    }
    zfree(entry->topic);
    zfree(entry->data);
    zfree(entry);
}

/* ====================================================================
 * Configuration
 * ==================================================================== */

void loopyPubSubConfigInit(loopyPubSubConfig *config) {
    if (!config) {
        return;
    }

    memset(config, 0, sizeof(*config));
    config->separator = '.';
    config->starWildcard = '*';
    config->hashWildcard = '#';
    config->maxSubscriptions = 0; /* Unlimited */
    config->enableStats = true;
}

void loopySubscriptionConfigInit(loopySubscriptionConfig *config) {
    if (!config) {
        return;
    }

    memset(config, 0, sizeof(*config));
    config->deliveryMode = LOOPY_DELIVER_SYNC;
    config->ackMode = LOOPY_ACK_AUTO;
    config->queueSize = 1024;
    config->dropOnFull = true;
    config->userData = NULL;
}

/* ====================================================================
 * Hub Lifecycle
 * ==================================================================== */

loopyPubSub *loopyPubSubNew(loopyLoop *loop, const loopyPubSubConfig *config) {
    loopyPubSub *ps = zcalloc(1, sizeof(loopyPubSub));
    if (!ps) {
        return NULL;
    }

    ps->loop = loop;

    /* Set configuration */
    if (config) {
        ps->config = *config;
    } else {
        loopyPubSubConfigInit(&ps->config);
    }

    /* Create rax topic matcher with custom config */
    raxTopicConfig raxConfig = {
        .separator = ps->config.separator,
        .starWildcard = ps->config.starWildcard,
        .hashWildcard = ps->config.hashWildcard,
    };
    ps->topics = raxTopicNewWithConfig(&raxConfig);
    if (!ps->topics) {
        zfree(ps);
        return NULL;
    }

    /* Initialize lock */
    if (pthread_rwlock_init(&ps->lock, NULL) != 0) {
        raxTopicFree(ps->topics);
        zfree(ps);
        return NULL;
    }

    atomic_init(&ps->sequence, 0);
    atomic_init(&ps->messagesPublished, 0);
    atomic_init(&ps->messagesDelivered, 0);
    atomic_init(&ps->messagesDropped, 0);
    atomic_init(&ps->bytesPublished, 0);
    atomic_init(&ps->bytesDelivered, 0);
    atomic_init(&ps->subscriptionCount, 0);
    atomic_init(&ps->patternCount, 0);

    return ps;
}

static void freePatternEntry(void *ptr) {
    loopyPatternEntry *entry = ptr;
    if (!entry) {
        return;
    }

    /* Free all subscriptions in the list */
    loopySubscription *sub = entry->subscriptions;
    while (sub) {
        loopySubscription *next = sub->nextSamePattern;

        /* Free subscription queue */
        loopyMessageEntry *msg = sub->queueHead;
        while (msg) {
            loopyMessageEntry *nextMsg = msg->next;
            messageEntryFree(msg);
            msg = nextMsg;
        }

        pthread_mutex_destroy(&sub->queueMutex);
        zfree(sub->pattern);
        zfree(sub);
        sub = next;
    }

    patternEntryFree(entry);
}

void loopyPubSubFree(loopyPubSub *ps) {
    if (!ps) {
        return;
    }

    pthread_rwlock_wrlock(&ps->lock);

    /* Free all pattern entries and their subscriptions */
    raxTopicFreeWithCallback(ps->topics, freePatternEntry);

    pthread_rwlock_unlock(&ps->lock);
    pthread_rwlock_destroy(&ps->lock);

    zfree(ps);
}

/* ====================================================================
 * Subscription Management
 * ==================================================================== */

loopySubscription *loopySubscribe(loopyPubSub *ps, const char *pattern,
                                  loopyMessageCallback *cb,
                                  const loopySubscriptionConfig *config) {
    if (!ps || !pattern || !cb) {
        return NULL;
    }

    /* Check subscription limit */
    if (ps->config.maxSubscriptions > 0 &&
        atomic_load(&ps->subscriptionCount) >= ps->config.maxSubscriptions) {
        errno = EAGAIN;
        return NULL;
    }

    /* Create subscription */
    loopySubscription *sub = zcalloc(1, sizeof(loopySubscription));
    if (!sub) {
        return NULL;
    }

    sub->ps = ps;
    sub->pattern = pubsubStrdup(pattern);
    if (!sub->pattern) {
        zfree(sub);
        return NULL;
    }

    sub->callback = cb;

    if (config) {
        sub->deliveryMode = config->deliveryMode;
        sub->ackMode = config->ackMode;
        sub->queueSize = config->queueSize;
        sub->dropOnFull = config->dropOnFull;
        sub->userData = config->userData;
    } else {
        sub->deliveryMode = LOOPY_DELIVER_SYNC;
        sub->ackMode = LOOPY_ACK_AUTO;
        sub->queueSize = 1024;
        sub->dropOnFull = true;
    }

    if (pthread_mutex_init(&sub->queueMutex, NULL) != 0) {
        zfree(sub->pattern);
        zfree(sub);
        return NULL;
    }

    /* Add to pattern entry in rax */
    pthread_rwlock_wrlock(&ps->lock);

    loopyPatternEntry *entry = raxTopicGet(ps->topics, pattern);
    if (!entry) {
        /* New pattern - create entry */
        entry = patternEntryNew(pattern);
        if (!entry) {
            pthread_rwlock_unlock(&ps->lock);
            pthread_mutex_destroy(&sub->queueMutex);
            zfree(sub->pattern);
            zfree(sub);
            return NULL;
        }

        void *old = NULL;
        if (raxTopicBind(ps->topics, pattern, entry, &old) < 0) {
            pthread_rwlock_unlock(&ps->lock);
            patternEntryFree(entry);
            pthread_mutex_destroy(&sub->queueMutex);
            zfree(sub->pattern);
            zfree(sub);
            return NULL;
        }

        atomic_fetch_add(&ps->patternCount, 1);
    }

    /* Add subscription to front of list */
    sub->nextSamePattern = entry->subscriptions;
    entry->subscriptions = sub;
    entry->count++;

    atomic_fetch_add(&ps->subscriptionCount, 1);

    pthread_rwlock_unlock(&ps->lock);

    /* Fire subscribed event */
    if (sub->eventCallback) {
        sub->eventCallback(sub, LOOPY_SUB_SUBSCRIBED, sub->eventUserData);
    }

    return sub;
}

bool loopyUnsubscribe(loopySubscription *sub) {
    if (!sub || !sub->ps) {
        return false;
    }

    loopyPubSub *ps = sub->ps;

    pthread_rwlock_wrlock(&ps->lock);

    loopyPatternEntry *entry = raxTopicGet(ps->topics, sub->pattern);
    if (!entry) {
        pthread_rwlock_unlock(&ps->lock);
        return false;
    }

    /* Remove from subscription list */
    loopySubscription **pp = &entry->subscriptions;
    while (*pp) {
        if (*pp == sub) {
            *pp = sub->nextSamePattern;
            entry->count--;
            break;
        }
        pp = &(*pp)->nextSamePattern;
    }

    /* If pattern has no more subscriptions, remove it */
    if (entry->count == 0) {
        void *old = NULL;
        raxTopicUnbind(ps->topics, sub->pattern, &old);
        patternEntryFree(entry);
        atomic_fetch_sub(&ps->patternCount, 1);
    }

    atomic_fetch_sub(&ps->subscriptionCount, 1);

    pthread_rwlock_unlock(&ps->lock);

    /* Fire unsubscribed event */
    if (sub->eventCallback) {
        sub->eventCallback(sub, LOOPY_SUB_UNSUBSCRIBED, sub->eventUserData);
    }

    /* Free subscription resources */
    pthread_mutex_lock(&sub->queueMutex);
    loopyMessageEntry *msg = sub->queueHead;
    while (msg) {
        loopyMessageEntry *next = msg->next;
        messageEntryFree(msg);
        msg = next;
    }
    pthread_mutex_unlock(&sub->queueMutex);

    pthread_mutex_destroy(&sub->queueMutex);
    zfree(sub->pattern);
    zfree(sub);

    return true;
}

const char *loopySubscriptionPattern(const loopySubscription *sub) {
    return sub ? sub->pattern : NULL;
}

size_t loopySubscriptionPending(const loopySubscription *sub) {
    if (!sub) {
        return 0;
    }

    pthread_mutex_lock((pthread_mutex_t *)&sub->queueMutex);
    size_t count = sub->queueLen;
    pthread_mutex_unlock((pthread_mutex_t *)&sub->queueMutex);

    return count;
}

bool loopySubscriptionReceive(loopySubscription *sub, loopyMessage *msg) {
    if (!sub || !msg || sub->deliveryMode != LOOPY_DELIVER_QUEUE) {
        return false;
    }

    pthread_mutex_lock(&sub->queueMutex);

    loopyMessageEntry *entry = sub->queueHead;
    if (!entry) {
        pthread_mutex_unlock(&sub->queueMutex);
        return false;
    }

    /* Fill message structure */
    msg->topic = entry->topic;
    msg->data = entry->data;
    msg->len = entry->len;
    msg->timestamp = entry->timestamp;
    msg->sequence = entry->sequence;
    msg->publisherData = entry->publisherData;

    /* Remove from queue if auto-ack */
    if (sub->ackMode != LOOPY_ACK_MANUAL) {
        sub->queueHead = entry->next;
        if (!sub->queueHead) {
            sub->queueTail = NULL;
        }
        sub->queueLen--;
        messageEntryFree(entry);
    }

    pthread_mutex_unlock(&sub->queueMutex);

    return true;
}

bool loopySubscriptionAck(loopySubscription *sub, uint64_t sequence) {
    if (!sub || sub->ackMode != LOOPY_ACK_MANUAL) {
        return false;
    }

    pthread_mutex_lock(&sub->queueMutex);

    /* Find and remove the message with this sequence */
    loopyMessageEntry **pp = &sub->queueHead;
    while (*pp) {
        if ((*pp)->sequence == sequence) {
            loopyMessageEntry *entry = *pp;
            *pp = entry->next;
            if (sub->queueTail == entry) {
                sub->queueTail = NULL;
            }
            sub->queueLen--;
            messageEntryFree(entry);
            pthread_mutex_unlock(&sub->queueMutex);
            return true;
        }
        pp = &(*pp)->next;
    }

    pthread_mutex_unlock(&sub->queueMutex);
    return false;
}

bool loopySubscriptionNack(loopySubscription *sub, uint64_t sequence) {
    /* For now, nack just keeps the message in queue for redelivery */
    (void)sub;
    (void)sequence;
    return true;
}

void loopySubscriptionOnEvent(loopySubscription *sub,
                              loopySubscriptionEventCallback *cb,
                              void *userData) {
    if (!sub) {
        return;
    }
    sub->eventCallback = cb;
    sub->eventUserData = userData;
}

/* ====================================================================
 * Publishing
 * ==================================================================== */

/* Context for match callback during publish */
typedef struct publishContext {
    loopyPubSub *ps;
    const char *topic;
    const void *data;
    size_t len;
    uint64_t timestamp;
    uint64_t sequence;
    void *publisherData;
    size_t delivered;
    size_t dropped;
} publishContext;

static int publishToPattern(const char *pattern, void *value, void *arg) {
    (void)pattern;
    publishContext *ctx = arg;
    loopyPatternEntry *entry = value;

    /* Deliver to all subscriptions for this pattern */
    for (loopySubscription *sub = entry->subscriptions; sub;
         sub = sub->nextSamePattern) {
        /* Create message */
        loopyMessage msg = {
            .topic = ctx->topic,
            .data = ctx->data,
            .len = ctx->len,
            .timestamp = ctx->timestamp,
            .sequence = ctx->sequence,
            .publisherData = ctx->publisherData,
        };

        if (sub->deliveryMode == LOOPY_DELIVER_SYNC) {
            /* Synchronous callback */
            sub->callback(sub, &msg, sub->userData);
            sub->messagesReceived++;
            ctx->delivered++;
        } else if (sub->deliveryMode == LOOPY_DELIVER_QUEUE) {
            /* Queue for later retrieval */
            pthread_mutex_lock(&sub->queueMutex);

            /* Check queue limits */
            if (sub->queueSize > 0 && sub->queueLen >= sub->queueSize) {
                if (sub->dropOnFull) {
                    sub->messagesDropped++;
                    ctx->dropped++;
                    pthread_mutex_unlock(&sub->queueMutex);

                    if (sub->eventCallback) {
                        sub->eventCallback(sub, LOOPY_SUB_QUEUE_FULL,
                                           sub->eventUserData);
                    }
                    continue;
                }
                /* Block would be here for non-drop mode, but we'll just drop
                 * for now */
            }

            /* Create queue entry */
            loopyMessageEntry *queueEntry =
                zcalloc(1, sizeof(loopyMessageEntry));
            if (queueEntry) {
                queueEntry->topic = pubsubStrdup(ctx->topic);
                queueEntry->data = zmalloc(ctx->len);
                if (queueEntry->data) {
                    memcpy(queueEntry->data, ctx->data, ctx->len);
                }
                queueEntry->len = ctx->len;
                queueEntry->timestamp = ctx->timestamp;
                queueEntry->sequence = ctx->sequence;
                queueEntry->publisherData = ctx->publisherData;

                /* Add to queue tail */
                if (sub->queueTail) {
                    sub->queueTail->next = queueEntry;
                } else {
                    sub->queueHead = queueEntry;
                }
                sub->queueTail = queueEntry;
                sub->queueLen++;

                sub->messagesReceived++;
                ctx->delivered++;
            }

            pthread_mutex_unlock(&sub->queueMutex);
        }
        /* LOOPY_DELIVER_ASYNC would need event loop integration */
    }

    return 0; /* Continue matching */
}

size_t loopyPublish(loopyPubSub *ps, const char *topic, const void *data,
                    size_t len) {
    return loopyPublishEx(ps, topic, data, len, NULL);
}

size_t loopyPublishEx(loopyPubSub *ps, const char *topic, const void *data,
                      size_t len, void *publisherData) {
    if (!ps || !topic) {
        return 0;
    }

    /* Get sequence and timestamp */
    uint64_t seq = atomic_fetch_add(&ps->sequence, 1) + 1;
    uint64_t ts = getMonotonicNs();

    publishContext ctx = {
        .ps = ps,
        .topic = topic,
        .data = data,
        .len = len,
        .timestamp = ts,
        .sequence = seq,
        .publisherData = publisherData,
        .delivered = 0,
        .dropped = 0,
    };

    /* Match and deliver */
    pthread_rwlock_rdlock(&ps->lock);
    raxTopicMatch(ps->topics, topic, publishToPattern, &ctx);
    pthread_rwlock_unlock(&ps->lock);

    /* Update stats */
    if (ps->config.enableStats) {
        atomic_fetch_add(&ps->messagesPublished, 1);
        atomic_fetch_add(&ps->messagesDelivered, ctx.delivered);
        atomic_fetch_add(&ps->messagesDropped, ctx.dropped);
        atomic_fetch_add(&ps->bytesPublished, len);
        atomic_fetch_add(&ps->bytesDelivered, len * ctx.delivered);
    }

    return ctx.delivered;
}

bool loopyPublishAsync(loopyPubSub *ps, const char *topic, const void *data,
                       size_t len, loopyPublishCallback *cb, void *userData) {
    /* For now, just do sync publish and callback immediately */
    size_t delivered = loopyPublish(ps, topic, data, len);
    if (cb) {
        cb(delivered, userData);
    }
    return true;
}

/* ====================================================================
 * Querying
 * ==================================================================== */

bool loopyHasSubscribers(const loopyPubSub *ps, const char *pattern) {
    if (!ps || !pattern) {
        return false;
    }

    pthread_rwlock_rdlock((pthread_rwlock_t *)&ps->lock);
    bool exists = raxTopicExists(ps->topics, pattern);
    pthread_rwlock_unlock((pthread_rwlock_t *)&ps->lock);

    return exists;
}

size_t loopyCountSubscribers(const loopyPubSub *ps, const char *pattern) {
    if (!ps || !pattern) {
        return 0;
    }

    pthread_rwlock_rdlock((pthread_rwlock_t *)&ps->lock);
    loopyPatternEntry *entry = raxTopicGet(ps->topics, pattern);
    size_t count = entry ? entry->count : 0;
    pthread_rwlock_unlock((pthread_rwlock_t *)&ps->lock);

    return count;
}

/* Context for match count */
typedef struct matchCountContext {
    size_t total;
} matchCountContext;

static int countMatches(const char *pattern, void *value, void *arg) {
    (void)pattern;
    matchCountContext *ctx = arg;
    loopyPatternEntry *entry = value;
    ctx->total += entry->count;
    return 0;
}

size_t loopyMatchCount(const loopyPubSub *ps, const char *topic) {
    if (!ps || !topic) {
        return 0;
    }

    matchCountContext ctx = {.total = 0};

    pthread_rwlock_rdlock((pthread_rwlock_t *)&ps->lock);
    raxTopicMatch(ps->topics, topic, countMatches, &ctx);
    pthread_rwlock_unlock((pthread_rwlock_t *)&ps->lock);

    return ctx.total;
}

/* Context for iteration */
typedef struct iterContext {
    loopySubscriptionIterFn *fn;
    void *arg;
    size_t count;
} iterContext;

static int iterateEntry(const char *pattern, void *value, void *arg) {
    iterContext *ctx = arg;
    loopyPatternEntry *entry = value;

    for (loopySubscription *sub = entry->subscriptions; sub;
         sub = sub->nextSamePattern) {
        ctx->count++;
        if (ctx->fn(pattern, sub, ctx->arg) != 0) {
            return 1; /* Stop iteration */
        }
    }

    return 0;
}

size_t loopyIterateSubscriptions(const loopyPubSub *ps,
                                 loopySubscriptionIterFn fn, void *arg) {
    if (!ps || !fn) {
        return 0;
    }

    iterContext ctx = {.fn = fn, .arg = arg, .count = 0};

    pthread_rwlock_rdlock((pthread_rwlock_t *)&ps->lock);
    raxTopicIterate(ps->topics, iterateEntry, &ctx);
    pthread_rwlock_unlock((pthread_rwlock_t *)&ps->lock);

    return ctx.count;
}

/* ====================================================================
 * Statistics
 * ==================================================================== */

void loopyPubSubGetStats(const loopyPubSub *ps, loopyPubSubStats *stats) {
    if (!ps || !stats) {
        return;
    }

    memset(stats, 0, sizeof(*stats));
    stats->messagesPublished = atomic_load(&ps->messagesPublished);
    stats->messagesDelivered = atomic_load(&ps->messagesDelivered);
    stats->messagesDropped = atomic_load(&ps->messagesDropped);
    stats->bytesPublished = atomic_load(&ps->bytesPublished);
    stats->bytesDelivered = atomic_load(&ps->bytesDelivered);
    stats->subscriptionCount = atomic_load(&ps->subscriptionCount);
    stats->patternCount = atomic_load(&ps->patternCount);
}

void loopyPubSubResetStats(loopyPubSub *ps) {
    if (!ps) {
        return;
    }

    atomic_store(&ps->messagesPublished, 0);
    atomic_store(&ps->messagesDelivered, 0);
    atomic_store(&ps->messagesDropped, 0);
    atomic_store(&ps->bytesPublished, 0);
    atomic_store(&ps->bytesDelivered, 0);
}

/* ====================================================================
 * Utility
 * ==================================================================== */

bool loopyValidateTopic(const loopyPubSub *ps, const char *topic) {
    if (!ps || !topic || *topic == '\0') {
        return false;
    }

    /* Topics cannot contain wildcards */
    for (const char *p = topic; *p; p++) {
        if (*p == ps->config.starWildcard || *p == ps->config.hashWildcard) {
            return false;
        }
    }

    return true;
}

bool loopyValidatePattern(const loopyPubSub *ps, const char *pattern) {
    if (!ps || !pattern || *pattern == '\0') {
        return false;
    }

    /* Basic validation - could be more thorough */
    /* # must be at end of a segment or the pattern */
    const char *hash = strchr(pattern, ps->config.hashWildcard);
    if (hash) {
        /* # must be followed by nothing or separator */
        if (hash[1] != '\0' && hash[1] != ps->config.separator) {
            return false;
        }
    }

    return true;
}

const char *loopySubscriptionEventName(loopySubscriptionEvent event) {
    switch (event) {
    case LOOPY_SUB_SUBSCRIBED:
        return "SUBSCRIBED";
    case LOOPY_SUB_UNSUBSCRIBED:
        return "UNSUBSCRIBED";
    case LOOPY_SUB_QUEUE_FULL:
        return "QUEUE_FULL";
    case LOOPY_SUB_QUEUE_EMPTY:
        return "QUEUE_EMPTY";
    case LOOPY_SUB_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

const char *loopyDeliveryModeName(loopyDeliveryMode mode) {
    switch (mode) {
    case LOOPY_DELIVER_SYNC:
        return "SYNC";
    case LOOPY_DELIVER_ASYNC:
        return "ASYNC";
    case LOOPY_DELIVER_QUEUE:
        return "QUEUE";
    default:
        return "UNKNOWN";
    }
}

const char *loopyAckModeName(loopyAckMode mode) {
    switch (mode) {
    case LOOPY_ACK_NONE:
        return "NONE";
    case LOOPY_ACK_AUTO:
        return "AUTO";
    case LOOPY_ACK_MANUAL:
        return "MANUAL";
    default:
        return "UNKNOWN";
    }
}
