/* loopyPubSub - Topic-based publish/subscribe with rax routing
 *
 * Provides high-performance pub/sub messaging with wildcard pattern matching:
 *   - '*' matches exactly one segment
 *   - '#' matches zero or more segments
 *
 * Example patterns:
 *   "stock.*.price"     matches "stock.AAPL.price", "stock.GOOG.price"
 *   "stock.#"           matches "stock", "stock.AAPL", "stock.AAPL.price"
 *   "*.weather.#"       matches "us.weather", "uk.weather.london.rain"
 *
 * Features:
 * - Pattern-based subscriptions with AMQP-style wildcards
 * - Efficient rax radix tree routing for fast message delivery
 * - Per-subscriber message queues with backpressure
 * - Async message delivery integrating with loopy event loop
 * - Statistics and introspection
 *
 * Copyright 2024 Matt Stancliff <matt@genges.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 */

#pragma once

#include "loopyPlatform.h"

#include "loopy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ====================================================================
 * Types
 * ==================================================================== */

/**
 * Opaque pub/sub hub - manages subscriptions and message routing.
 */
typedef struct loopyPubSub loopyPubSub;

/**
 * Opaque subscription handle.
 */
typedef struct loopySubscription loopySubscription;

/**
 * Message structure passed to subscribers.
 */
typedef struct loopyMessage {
    const char *topic;   /* Topic the message was published to */
    const void *data;    /* Message payload */
    size_t len;          /* Payload length */
    uint64_t timestamp;  /* Publication timestamp (monotonic ns) */
    uint64_t sequence;   /* Global sequence number */
    void *publisherData; /* Optional publisher-provided user data */
} loopyMessage;

/**
 * Subscription delivery mode.
 */
typedef enum loopyDeliveryMode {
    LOOPY_DELIVER_SYNC = 0,  /* Synchronous callback (may block) */
    LOOPY_DELIVER_ASYNC = 1, /* Async via event loop */
    LOOPY_DELIVER_QUEUE = 2, /* Queue for manual retrieval */
} loopyDeliveryMode;

/**
 * Message acknowledgment mode.
 */
typedef enum loopyAckMode {
    LOOPY_ACK_NONE = 0,   /* No acknowledgment required */
    LOOPY_ACK_AUTO = 1,   /* Auto-ack after callback returns */
    LOOPY_ACK_MANUAL = 2, /* Manual ack required */
} loopyAckMode;

/**
 * Subscription configuration.
 */
typedef struct loopySubscriptionConfig {
    loopyDeliveryMode deliveryMode; /* How messages are delivered */
    loopyAckMode ackMode;           /* Acknowledgment mode */
    size_t queueSize;               /* Max pending messages (0 = unlimited) */
    bool dropOnFull;                /* Drop messages when queue full vs block */
    void *userData;                 /* User data passed to callbacks */
} loopySubscriptionConfig;

/**
 * Pub/sub configuration.
 */
typedef struct loopyPubSubConfig {
    char separator;          /* Topic segment separator (default: '.') */
    char starWildcard;       /* Single-segment wildcard (default: '*') */
    char hashWildcard;       /* Multi-segment wildcard (default: '#') */
    size_t maxSubscriptions; /* Max subscriptions (0 = unlimited) */
    bool enableStats;        /* Enable statistics collection */
} loopyPubSubConfig;

/**
 * Pub/sub statistics.
 */
typedef struct loopyPubSubStats {
    uint64_t messagesPublished; /* Total messages published */
    uint64_t messagesDelivered; /* Total messages delivered */
    uint64_t messagesDropped;   /* Messages dropped due to full queues */
    uint64_t bytesPublished;    /* Total bytes published */
    uint64_t bytesDelivered;    /* Total bytes delivered */
    size_t subscriptionCount;   /* Current subscription count */
    size_t patternCount;        /* Unique patterns registered */
} loopyPubSubStats;

/* ====================================================================
 * Callbacks
 * ==================================================================== */

/**
 * Message delivery callback.
 *
 * @param sub       The subscription receiving the message
 * @param msg       The message (valid only during callback)
 * @param userData  User-provided data from subscription config
 *
 * For LOOPY_ACK_MANUAL, return true to acknowledge, false to nack/requeue.
 * For other ack modes, return value is ignored.
 */
typedef bool loopyMessageCallback(loopySubscription *sub,
                                  const loopyMessage *msg, void *userData);

/**
 * Subscription event callback (for state changes).
 *
 * @param sub       The subscription
 * @param event     Event type (see event enum)
 * @param userData  User-provided data
 */
typedef void loopySubscriptionEventCallback(loopySubscription *sub, int event,
                                            void *userData);

/**
 * Subscription events.
 */
typedef enum loopySubscriptionEvent {
    LOOPY_SUB_SUBSCRIBED = 0,   /* Successfully subscribed */
    LOOPY_SUB_UNSUBSCRIBED = 1, /* Unsubscribed */
    LOOPY_SUB_QUEUE_FULL = 2,   /* Queue full, messages being dropped */
    LOOPY_SUB_QUEUE_EMPTY = 3,  /* Queue drained */
    LOOPY_SUB_ERROR = 4,        /* Error occurred */
} loopySubscriptionEvent;

/* ====================================================================
 * Hub Lifecycle
 * ==================================================================== */

/**
 * Initialize configuration with defaults.
 *
 * @param config  Configuration to initialize
 */
void loopyPubSubConfigInit(loopyPubSubConfig *config);

/**
 * Initialize subscription configuration with defaults.
 *
 * @param config  Configuration to initialize
 */
void loopySubscriptionConfigInit(loopySubscriptionConfig *config);

/**
 * Create a new pub/sub hub.
 *
 * @param loop    Event loop for async operations (NULL for sync-only)
 * @param config  Configuration (NULL for defaults)
 * @return New hub, or NULL on error
 */
loopyPubSub *loopyPubSubNew(loopyLoop *loop, const loopyPubSubConfig *config);

/**
 * Destroy a pub/sub hub.
 *
 * All subscriptions are automatically unsubscribed.
 *
 * @param ps  The hub to destroy
 */
void loopyPubSubFree(loopyPubSub *ps);

/* ====================================================================
 * Subscription Management
 * ==================================================================== */

/**
 * Subscribe to a topic pattern.
 *
 * @param ps       The pub/sub hub
 * @param pattern  Topic pattern (may include wildcards)
 * @param cb       Message callback
 * @param config   Subscription configuration (NULL for defaults)
 * @return Subscription handle, or NULL on error
 */
loopySubscription *loopySubscribe(loopyPubSub *ps, const char *pattern,
                                  loopyMessageCallback *cb,
                                  const loopySubscriptionConfig *config);

/**
 * Unsubscribe from a topic.
 *
 * @param sub  The subscription to cancel
 * @return true on success
 */
bool loopyUnsubscribe(loopySubscription *sub);

/**
 * Get the pattern a subscription is bound to.
 *
 * @param sub  The subscription
 * @return Pattern string (do not modify)
 */
const char *loopySubscriptionPattern(const loopySubscription *sub);

/**
 * Get pending message count in subscription queue.
 *
 * @param sub  The subscription
 * @return Number of pending messages
 */
size_t loopySubscriptionPending(const loopySubscription *sub);

/**
 * Manually receive next message from LOOPY_DELIVER_QUEUE subscription.
 *
 * @param sub  The subscription
 * @param msg  Output message structure (caller must not free)
 * @return true if message available, false if queue empty
 */
bool loopySubscriptionReceive(loopySubscription *sub, loopyMessage *msg);

/**
 * Acknowledge a message (for LOOPY_ACK_MANUAL mode).
 *
 * @param sub       The subscription
 * @param sequence  Message sequence number to ack
 * @return true on success
 */
bool loopySubscriptionAck(loopySubscription *sub, uint64_t sequence);

/**
 * Negative acknowledge (request redelivery).
 *
 * @param sub       The subscription
 * @param sequence  Message sequence number to nack
 * @return true on success
 */
bool loopySubscriptionNack(loopySubscription *sub, uint64_t sequence);

/**
 * Set event callback for subscription state changes.
 *
 * @param sub       The subscription
 * @param cb        Event callback
 * @param userData  User data for callback
 */
void loopySubscriptionOnEvent(loopySubscription *sub,
                              loopySubscriptionEventCallback *cb,
                              void *userData);

/* ====================================================================
 * Publishing
 * ==================================================================== */

/**
 * Publish a message to a topic.
 *
 * @param ps     The pub/sub hub
 * @param topic  Topic to publish to (must not contain wildcards)
 * @param data   Message payload
 * @param len    Payload length
 * @return Number of subscribers that received the message
 */
size_t loopyPublish(loopyPubSub *ps, const char *topic, const void *data,
                    size_t len);

/**
 * Publish with additional options.
 *
 * @param ps            The pub/sub hub
 * @param topic         Topic to publish to
 * @param data          Message payload
 * @param len           Payload length
 * @param publisherData Optional user data included in message
 * @return Number of subscribers that received the message
 */
size_t loopyPublishEx(loopyPubSub *ps, const char *topic, const void *data,
                      size_t len, void *publisherData);

/**
 * Async publish with completion callback.
 *
 * @param ps       The pub/sub hub
 * @param topic    Topic to publish to
 * @param data     Message payload (must remain valid until callback)
 * @param len      Payload length
 * @param cb       Completion callback (receives delivery count)
 * @param userData User data for callback
 * @return true if publish was queued
 */
typedef void loopyPublishCallback(size_t delivered, void *userData);

bool loopyPublishAsync(loopyPubSub *ps, const char *topic, const void *data,
                       size_t len, loopyPublishCallback *cb, void *userData);

/* ====================================================================
 * Querying
 * ==================================================================== */

/**
 * Check if a pattern has any subscriptions.
 *
 * @param ps       The pub/sub hub
 * @param pattern  Pattern to check
 * @return true if pattern is subscribed
 */
bool loopyHasSubscribers(const loopyPubSub *ps, const char *pattern);

/**
 * Count subscriptions matching a pattern.
 *
 * @param ps       The pub/sub hub
 * @param pattern  Pattern to count
 * @return Number of matching subscriptions
 */
size_t loopyCountSubscribers(const loopyPubSub *ps, const char *pattern);

/**
 * Count how many subscriptions would match a topic.
 *
 * @param ps     The pub/sub hub
 * @param topic  Topic to match
 * @return Number of subscriptions that would receive a message
 */
size_t loopyMatchCount(const loopyPubSub *ps, const char *topic);

/**
 * Subscription iterator callback.
 *
 * @param pattern  Subscription pattern
 * @param sub      Subscription handle
 * @param arg      User argument
 * @return 0 to continue, non-zero to stop
 */
typedef int loopySubscriptionIterFn(const char *pattern, loopySubscription *sub,
                                    void *arg);

/**
 * Iterate all subscriptions.
 *
 * @param ps       The pub/sub hub
 * @param fn       Iterator callback
 * @param arg      User argument
 * @return Number of subscriptions visited
 */
size_t loopyIterateSubscriptions(const loopyPubSub *ps,
                                 loopySubscriptionIterFn fn, void *arg);

/* ====================================================================
 * Statistics
 * ==================================================================== */

/**
 * Get pub/sub statistics.
 *
 * @param ps     The pub/sub hub
 * @param stats  Output statistics
 */
void loopyPubSubGetStats(const loopyPubSub *ps, loopyPubSubStats *stats);

/**
 * Reset statistics counters.
 *
 * @param ps  The pub/sub hub
 */
void loopyPubSubResetStats(loopyPubSub *ps);

/* ====================================================================
 * Utility
 * ==================================================================== */

/**
 * Validate a topic string (no wildcards allowed).
 *
 * @param ps     The pub/sub hub
 * @param topic  Topic to validate
 * @return true if valid
 */
bool loopyValidateTopic(const loopyPubSub *ps, const char *topic);

/**
 * Validate a pattern string (wildcards allowed).
 *
 * @param ps       The pub/sub hub
 * @param pattern  Pattern to validate
 * @return true if valid
 */
bool loopyValidatePattern(const loopyPubSub *ps, const char *pattern);

/**
 * Get event name string.
 *
 * @param event  Event type
 * @return Static string
 */
const char *loopySubscriptionEventName(loopySubscriptionEvent event);

/**
 * Get delivery mode name string.
 *
 * @param mode  Delivery mode
 * @return Static string
 */
const char *loopyDeliveryModeName(loopyDeliveryMode mode);

/**
 * Get ack mode name string.
 *
 * @param mode  Ack mode
 * @return Static string
 */
const char *loopyAckModeName(loopyAckMode mode);
