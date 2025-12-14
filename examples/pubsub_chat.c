/* pubsub_chat.c - Interactive Pub/Sub demonstration
 *
 * This example provides an INTERACTIVE shell for exploring loopy's
 * topic-based publish/subscribe system with wildcard pattern matching.
 *
 * ============================================================================
 * WILDCARD PATTERNS - The Key Concept
 * ============================================================================
 *
 * Topics are hierarchical, separated by dots: "market.stock.AAPL.price"
 *
 * Two wildcard characters enable powerful pattern matching:
 *
 *   '*' (star)  - Matches EXACTLY ONE segment
 *   '#' (hash)  - Matches ZERO OR MORE segments
 *
 * Examples:
 *   Pattern               | Matches                    | Does NOT Match
 *   ----------------------|----------------------------|------------------
 *   market.stock.*.price  | market.stock.AAPL.price    | market.stock.price
 *                         | market.stock.GOOG.price    |
 * market.stock.AAPL.volume
 *   ----------------------|----------------------------|------------------
 *   weather.#             | weather                    | sports.weather
 *                         | weather.us                 |
 *                         | weather.us.ca.sf           |
 *   ----------------------|----------------------------|------------------
 *   *.news.#              | sports.news                | news
 *                         | tech.news.breaking         | sports.news
 *                         | us.news.politics.2024      |
 *   ----------------------|----------------------------|------------------
 *   #                     | (matches everything)       | (nothing)
 *
 * Build (from build directory):
 *   Already built as part of loopy: ./examples/pubsub_chat
 *
 * Run:
 *   ./examples/pubsub_chat           # Interactive mode
 *   ./examples/pubsub_chat --demo    # Guided demonstration
 *   ./examples/pubsub_chat --test    # Automated test mode
 *
 * Copyright 2024 Matt Stancliff <matt@genges.com>
 * Licensed under the Apache License, Version 2.0
 */

#include "loopy.h"
#include "loopyPubSub.h"
#include "loopySignal.h"
#include "loopyTimer.h"

#include "../deps/datakit/src/datakit.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ============================================================================
 * Configuration
 * ============================================================================
 */

#define MAX_SUBSCRIPTIONS 32
#define MAX_INPUT_LEN 512
#define PROMPT "pubsub> "

/* ============================================================================
 * Global State
 * ============================================================================
 */

static loopyLoop *g_loop = NULL;
static loopyPubSub *g_pubsub = NULL;
static loopySignalHandler *g_sigHandler = NULL;

/* Track subscriptions for listing and management */
typedef struct {
    loopySubscription *sub;
    char pattern[128];
    char name[32];
    int id;
    uint64_t msgCount;
} SubInfo;

static SubInfo g_subs[MAX_SUBSCRIPTIONS];
static int g_subCount = 0;
static int g_nextSubId = 1;
static int g_totalMessages = 0;
static int g_testMode = 0;
static int g_demoMode = 0;

/* ============================================================================
 * Message Callback - Shows which subscription matched and why
 * ============================================================================
 */

static bool onMessage(loopySubscription *sub, const loopyMessage *msg,
                      void *userData) {
    SubInfo *info = (SubInfo *)userData;
    info->msgCount++;
    g_totalMessages++;

    /* In test mode, be quieter */
    if (g_testMode) {
        printf("  [%s] '%s' -> matched pattern '%s'\n", info->name, msg->topic,
               info->pattern);
        return true;
    }

    /* Show the match with clear explanation */
    printf("\n");
    printf("  +-- MESSAGE RECEIVED --+\n");
    printf("  | Subscription: [%d] %s\n", info->id, info->name);
    printf("  | Pattern:      %s\n", info->pattern);
    printf("  | Topic:        %s\n", msg->topic);
    printf("  | Data:         %.*s\n", (int)msg->len, (const char *)msg->data);
    printf("  | Sequence:     %" PRIu64 "\n", msg->sequence);
    printf("  +----------------------+\n");

    return true;
}

/* ============================================================================
 * Command Handlers
 * ============================================================================
 */

static void printHelp(void) {
    printf("\n");
    printf("=== LOOPY PUB/SUB INTERACTIVE SHELL ===\n");
    printf("\n");
    printf("Commands:\n");
    printf("  sub <pattern> [name]  Subscribe to a pattern\n");
    printf("  unsub <id>            Unsubscribe by ID\n");
    printf("  pub <topic> <msg>     Publish a message to a topic\n");
    printf("  list                  List active subscriptions\n");
    printf("  stats                 Show pub/sub statistics\n");
    printf("  test <topic>          Test which subscriptions match a topic\n");
    printf("  clear                 Clear all subscriptions\n");
    printf("  demo                  Run interactive demonstration\n");
    printf("  help                  Show this help\n");
    printf("  quit                  Exit\n");
    printf("\n");
    printf("=== WILDCARD PATTERNS ===\n");
    printf("\n");
    printf("  *  Matches EXACTLY ONE segment\n");
    printf("  #  Matches ZERO OR MORE segments\n");
    printf("\n");
    printf("Examples:\n");
    printf("  sub market.stock.*.price    # Matches any stock's price\n");
    printf("  sub weather.#               # Matches all weather topics\n");
    printf("  sub *.alerts.#              # Any source, alerts subtree\n");
    printf("  sub #                       # Catch-all (matches everything)\n");
    printf("\n");
    printf("  pub market.stock.AAPL.price 142.50\n");
    printf("  pub weather.us.ca.sf Sunny, 72F\n");
    printf("\n");
}

static void printPatternGuide(void) {
    printf("\n");
    printf("+============================================================+\n");
    printf("|               WILDCARD PATTERN QUICK REFERENCE             |\n");
    printf("+============================================================+\n");
    printf("|\n");
    printf("|  '*' = Exactly ONE segment    '#' = Zero or MORE segments\n");
    printf("|\n");
    printf("|  Pattern              Matches              Doesn't Match\n");
    printf("|  -----------------   -------------------  ------------------\n");
    printf("|  stock.*.price       stock.AAPL.price     stock.price\n");
    printf("|                      stock.GOOG.price     stock.a.b.price\n");
    printf("|\n");
    printf("|  stock.#             stock                weather.stock\n");
    printf("|                      stock.AAPL           \n");
    printf("|                      stock.AAPL.price     \n");
    printf("|                      stock.a.b.c.d        \n");
    printf("|\n");
    printf("|  *.stock.#           us.stock             stock\n");
    printf("|                      uk.stock.FTSE        global.stock\n");
    printf("|                      jp.stock.deep.path   \n");
    printf("|\n");
    printf(
        "+============================================================+\n\n");
}

static void cmdSubscribe(const char *pattern, const char *name) {
    if (g_subCount >= MAX_SUBSCRIPTIONS) {
        printf("Error: Maximum subscriptions (%d) reached\n",
               MAX_SUBSCRIPTIONS);
        return;
    }

    if (!pattern || !*pattern) {
        printf("Usage: sub <pattern> [name]\n");
        printf("  Example: sub market.stock.*.price StockPrices\n");
        return;
    }

    /* Validate pattern */
    if (!loopyValidatePattern(g_pubsub, pattern)) {
        printf("Error: Invalid pattern '%s'\n", pattern);
        return;
    }

    SubInfo *info = &g_subs[g_subCount];
    info->id = g_nextSubId++;
    info->msgCount = 0;
    strncpy(info->pattern, pattern, sizeof(info->pattern) - 1);
    info->pattern[sizeof(info->pattern) - 1] = '\0';

    if (name && *name) {
        strncpy(info->name, name, sizeof(info->name) - 1);
    } else {
        snprintf(info->name, sizeof(info->name), "Sub%d", info->id);
    }
    info->name[sizeof(info->name) - 1] = '\0';

    info->sub = loopySubscribe(
        g_pubsub, pattern, onMessage,
        &(loopySubscriptionConfig){.userData = info,
                                   .deliveryMode = LOOPY_DELIVER_SYNC});

    if (!info->sub) {
        printf("Error: Failed to subscribe to '%s'\n", pattern);
        return;
    }

    g_subCount++;
    printf("Subscribed [%d] %s -> '%s'\n", info->id, info->name, pattern);

    /* Educational hint */
    if (strchr(pattern, '*') && !strchr(pattern, '#')) {
        printf("  Hint: '*' matches exactly ONE segment\n");
    } else if (strchr(pattern, '#') && !strchr(pattern, '*')) {
        printf("  Hint: '#' matches zero or more segments (greedy)\n");
    } else if (strchr(pattern, '*') && strchr(pattern, '#')) {
        printf("  Hint: Combining '*' and '#' for precise matching\n");
    }
}

static void cmdUnsubscribe(int id) {
    for (int i = 0; i < g_subCount; i++) {
        if (g_subs[i].id == id) {
            printf("Unsubscribed [%d] %s (received %" PRIu64 " messages)\n",
                   g_subs[i].id, g_subs[i].name, g_subs[i].msgCount);
            loopyUnsubscribe(g_subs[i].sub);

            /* Remove from array */
            for (int j = i; j < g_subCount - 1; j++) {
                g_subs[j] = g_subs[j + 1];
            }
            g_subCount--;
            return;
        }
    }
    printf("Error: No subscription with ID %d\n", id);
}

static void cmdPublish(const char *topic, const char *message) {
    if (!topic || !*topic || !message) {
        printf("Usage: pub <topic> <message>\n");
        printf("  Example: pub market.stock.AAPL.price 142.50\n");
        return;
    }

    /* Validate topic (no wildcards allowed) */
    if (!loopyValidateTopic(g_pubsub, topic)) {
        printf("Error: Invalid topic '%s' (wildcards not allowed in topics)\n",
               topic);
        return;
    }

    /* Show what we're about to publish */
    printf("\nPublishing to '%s': %s\n", topic, message);

    /* Show which patterns will match BEFORE publishing */
    printf("Matching subscriptions:\n");
    int matchCount = 0;
    for (int i = 0; i < g_subCount; i++) {
        /* We'll see which ones fire when we publish */
        matchCount++;
    }

    size_t delivered = loopyPublish(g_pubsub, topic, message, strlen(message));

    if (delivered == 0) {
        printf("  (no subscriptions matched this topic)\n");
    } else {
        printf("\nDelivered to %zu subscriber(s)\n", delivered);
    }
}

static void cmdList(void) {
    printf("\n");
    printf("Active Subscriptions (%d):\n", g_subCount);
    printf("------------------------------------------------------\n");
    if (g_subCount == 0) {
        printf("  (none)\n");
    } else {
        printf("  ID   Name            Pattern                   Messages\n");
        printf("  ---  --------------  ------------------------  --------\n");
        for (int i = 0; i < g_subCount; i++) {
            printf("  %-3d  %-14s  %-24s  %" PRIu64 "\n", g_subs[i].id,
                   g_subs[i].name, g_subs[i].pattern, g_subs[i].msgCount);
        }
    }
    printf("------------------------------------------------------\n\n");
}

static void cmdStats(void) {
    loopyPubSubStats stats;
    loopyPubSubGetStats(g_pubsub, &stats);

    printf("\n");
    printf("Pub/Sub Statistics:\n");
    printf("------------------------------------------------------\n");
    printf("  Messages published:  %" PRIu64 "\n", stats.messagesPublished);
    printf("  Messages delivered:  %" PRIu64 "\n", stats.messagesDelivered);
    printf("  Messages dropped:    %" PRIu64 "\n", stats.messagesDropped);
    printf("  Bytes published:     %" PRIu64 "\n", stats.bytesPublished);
    printf("  Subscriptions:       %zu\n", stats.subscriptionCount);
    printf("  Unique patterns:     %zu\n", stats.patternCount);
    printf("------------------------------------------------------\n\n");
}

static void cmdTestMatch(const char *topic) {
    if (!topic || !*topic) {
        printf("Usage: test <topic>\n");
        printf("  Shows which subscriptions would match a topic\n");
        return;
    }

    printf("\nTesting which patterns match topic '%s':\n", topic);
    printf("------------------------------------------------------\n");

    int matchCount = 0;
    for (int i = 0; i < g_subCount; i++) {
        /* Use loopyMatchCount to check if pattern matches */
        size_t matches = loopyMatchCount(g_pubsub, topic);
        /* We need to test each pattern individually - let's just show all */
        printf("  [%d] %-20s  ", g_subs[i].id, g_subs[i].pattern);

        /* Simple pattern matching check for display */
        /* The real match happens in loopyPublish, but we can simulate */
        const char *pat = g_subs[i].pattern;

        /* Check if this is a potential match by analyzing pattern */
        bool couldMatch = true;

        /* '#' alone matches everything */
        if (strcmp(pat, "#") == 0) {
            printf("MATCH (# catches all)\n");
            matchCount++;
            continue;
        }

        /* For patterns ending in #, check prefix */
        size_t patLen = strlen(pat);
        if (patLen >= 2 && pat[patLen - 1] == '#') {
            /* Pattern like "foo.#" - check if topic starts with "foo." or
             * equals "foo" */
            char prefix[128];
            strncpy(prefix, pat, patLen - 1); /* Remove # */
            prefix[patLen - 1] = '\0';
            if (patLen > 2) {
                prefix[patLen - 2] = '\0'; /* Remove trailing . */
            }
            if (strncmp(topic, prefix, strlen(prefix)) == 0) {
                printf("MATCH (# matches remainder)\n");
                matchCount++;
                continue;
            }
        }

        /* For other patterns, we'd need full matching logic */
        /* Just indicate we'd need to check */
        printf("(publish to test)\n");
    }

    if (matchCount == 0 && g_subCount > 0) {
        printf("  Publish to see actual matches\n");
    }
    printf("------------------------------------------------------\n\n");
}

static void cmdClear(void) {
    int count = g_subCount;
    for (int i = g_subCount - 1; i >= 0; i--) {
        loopyUnsubscribe(g_subs[i].sub);
    }
    g_subCount = 0;
    printf("Cleared %d subscription(s)\n", count);
}

/* ============================================================================
 * Interactive Demo Mode
 * ============================================================================
 */

static void runDemo(void) {
    printf("\n");
    printf(
        "================================================================\n");
    printf("       INTERACTIVE PUB/SUB DEMONSTRATION\n");
    printf(
        "================================================================\n");
    printf("\n");
    printf("This demo will walk you through wildcard pattern matching.\n");
    printf("Watch how different patterns match different topics.\n");
    printf("\n");
    printf("Press ENTER to continue through each step...\n");
    printf("\n");

    char buf[16];

    /* Step 1: Setup subscriptions */
    printf(
        "--- STEP 1: Setting up subscriptions with different patterns ---\n\n");

    cmdSubscribe("market.stock.*.price", "AnyStock");
    printf("\n  This pattern uses '*' to match exactly ONE segment.\n");
    printf("  It will match 'market.stock.AAPL.price' but NOT "
           "'market.stock.price'\n");
    printf("  (because * requires something in that position)\n\n");
    fgets(buf, sizeof(buf), stdin);

    cmdSubscribe("weather.#", "AllWeather");
    printf("\n  This pattern uses '#' to match ZERO OR MORE segments.\n");
    printf("  It matches 'weather', 'weather.us', 'weather.us.ca.sf', etc.\n");
    printf("  The '#' is greedy - it consumes everything after 'weather.'\n\n");
    fgets(buf, sizeof(buf), stdin);

    cmdSubscribe("alerts.*.#", "Alerts");
    printf("\n  This combines '*' and '#' for precise control.\n");
    printf(
        "  It requires exactly one segment after 'alerts.', then anything.\n");
    printf("  Matches: 'alerts.system.disk' but NOT 'alerts.disk'\n\n");
    fgets(buf, sizeof(buf), stdin);

    cmdSubscribe("#", "CatchAll");
    printf("\n  The pattern '#' alone matches EVERYTHING.\n");
    printf("  Useful for logging/debugging all messages.\n\n");
    fgets(buf, sizeof(buf), stdin);

    /* Step 2: Show subscriptions */
    printf("--- STEP 2: Current subscriptions ---\n");
    cmdList();
    fgets(buf, sizeof(buf), stdin);

    /* Step 3: Publish and observe */
    printf("--- STEP 3: Publishing messages - observe which patterns match "
           "---\n\n");

    printf("Publishing to 'market.stock.AAPL.price'...\n");
    printf("Expected: AnyStock (*.price matches) and CatchAll (#)\n\n");
    fgets(buf, sizeof(buf), stdin);
    cmdPublish("market.stock.AAPL.price", "142.50");
    printf("\n");
    fgets(buf, sizeof(buf), stdin);

    printf("Publishing to 'market.stock.price' (missing stock symbol)...\n");
    printf("Expected: ONLY CatchAll (AnyStock needs exactly one segment for "
           "*)\n\n");
    fgets(buf, sizeof(buf), stdin);
    cmdPublish("market.stock.price", "N/A");
    printf("\n");
    fgets(buf, sizeof(buf), stdin);

    printf("Publishing to 'weather.us.ca.sf'...\n");
    printf("Expected: AllWeather (weather.# matches) and CatchAll\n\n");
    fgets(buf, sizeof(buf), stdin);
    cmdPublish("weather.us.ca.sf", "Sunny, 72F");
    printf("\n");
    fgets(buf, sizeof(buf), stdin);

    printf("Publishing to 'weather' (just the root)...\n");
    printf(
        "Expected: AllWeather (# matches ZERO segments too!) and CatchAll\n\n");
    fgets(buf, sizeof(buf), stdin);
    cmdPublish("weather", "Global weather system online");
    printf("\n");
    fgets(buf, sizeof(buf), stdin);

    printf("Publishing to 'alerts.system.disk.full'...\n");
    printf("Expected: Alerts (*.# needs one segment after alerts) and "
           "CatchAll\n\n");
    fgets(buf, sizeof(buf), stdin);
    cmdPublish("alerts.system.disk.full", "Disk 90%% full");
    printf("\n");
    fgets(buf, sizeof(buf), stdin);

    printf("Publishing to 'alerts.disk' (no intermediate segment)...\n");
    printf("Expected: ONLY CatchAll (Alerts pattern requires *.# = one segment "
           "then any)\n\n");
    fgets(buf, sizeof(buf), stdin);
    cmdPublish("alerts.disk", "Direct alert");
    printf("\n");
    fgets(buf, sizeof(buf), stdin);

    /* Summary */
    printf(
        "================================================================\n");
    printf("       DEMONSTRATION COMPLETE\n");
    printf(
        "================================================================\n\n");

    cmdStats();

    printf("Key takeaways:\n");
    printf("  1. '*' is STRICT - requires exactly one segment\n");
    printf("  2. '#' is GREEDY - matches zero or more (including nothing)\n");
    printf("  3. Combine them: 'foo.*.#' = one segment then anything\n");
    printf("  4. '#' alone catches everything (great for logging)\n");
    printf("\n");
    printf(
        "Type 'help' for commands, or experiment with your own patterns!\n\n");
}

/* ============================================================================
 * Test Mode - Automated testing for CI
 * ============================================================================
 */

/* Test helper macro */
#define TEST_EXPECT(name, topic, expected)                                     \
    do {                                                                       \
        g_totalMessages = 0;                                                   \
        loopyPublish(g_pubsub, topic, "test", 4);                              \
        if (g_totalMessages == expected) {                                     \
            printf("  [PASS] %s\n", name);                                     \
            passed++;                                                          \
        } else {                                                               \
            printf("  [FAIL] %s - expected %d, got %d\n", name, expected,      \
                   g_totalMessages);                                           \
            failed++;                                                          \
        }                                                                      \
    } while (0)

static int runTestMode(void) {
    printf("[TEST] Comprehensive Pub/Sub Pattern Matching Tests\n");
    printf("=====================================================\n\n");

    int passed = 0;
    int failed = 0;

    /* ================================================================
     * SECTION 1: Basic Wildcard Semantics
     * ================================================================ */
    printf("=== SECTION 1: Basic Wildcard Semantics ===\n\n");

    cmdClear();
    cmdSubscribe("a.*.c", "StarMiddle");
    cmdSubscribe("a.#", "HashSuffix");
    cmdSubscribe("#.z", "HashPrefix");
    cmdSubscribe("#", "CatchAll");

    printf("Testing: a.*.c, a.#, #.z, #\n\n");

    /* Star requires exactly one segment */
    TEST_EXPECT("a.*.c matches a.b.c", "a.b.c",
                3); /* StarMiddle, HashSuffix, CatchAll */
    TEST_EXPECT("a.*.c rejects a.c (missing segment)", "a.c",
                2); /* HashSuffix, CatchAll */
    TEST_EXPECT("a.*.c rejects a.b.x.c (too many)", "a.b.x.c",
                2); /* HashSuffix, CatchAll */

    /* Hash matches zero or more */
    TEST_EXPECT("a.# matches a (zero segments)", "a",
                2); /* HashSuffix, CatchAll */
    TEST_EXPECT("a.# matches a.b.c.d.e (many)", "a.b.c.d.e",
                2); /* HashSuffix, CatchAll */
    TEST_EXPECT("#.z matches z (zero prefix)", "z",
                2); /* HashPrefix, CatchAll */
    TEST_EXPECT("#.z matches a.b.c.z (deep)", "a.b.c.z",
                3); /* HashSuffix, HashPrefix, CatchAll */

    /* ================================================================
     * SECTION 2: Multi-Level Infrastructure Patterns (IoT/Cloud)
     * ================================================================ */
    printf("\n=== SECTION 2: Infrastructure Patterns (IoT/Cloud) ===\n\n");

    cmdClear();
    /* Realistic infrastructure monitoring patterns */
    cmdSubscribe("dc.*.rack.*.server.*.cpu", "SpecificCPU");
    cmdSubscribe("dc.*.rack.*.server.#", "AnyServerMetric");
    cmdSubscribe("dc.*.#", "DatacenterWide");
    cmdSubscribe("dc.#.alert", "AllAlerts");
    cmdSubscribe("#", "CatchAll");

    printf("Testing infrastructure: dc.*.rack.*.server.*.cpu, "
           "dc.*.rack.*.server.#, dc.*.#, dc.#.alert\n\n");

    TEST_EXPECT("CPU metric in specific path",
                "dc.west.rack.42.server.node1.cpu",
                4); /* SpecificCPU, AnyServerMetric, DatacenterWide, CatchAll */
    TEST_EXPECT("Memory metric (not CPU pattern)",
                "dc.west.rack.42.server.node1.memory",
                3); /* AnyServerMetric, DatacenterWide, CatchAll */
    TEST_EXPECT("Server-level event (no metric)",
                "dc.west.rack.42.server.node1",
                3); /* AnyServerMetric, DatacenterWide, CatchAll */
    TEST_EXPECT("Datacenter alert bubbles up",
                "dc.east.rack.1.server.x.disk.alert",
                4); /* AnyServerMetric, DatacenterWide, AllAlerts, CatchAll */
    TEST_EXPECT("Direct datacenter alert", "dc.west.alert",
                3); /* DatacenterWide, AllAlerts, CatchAll */
    TEST_EXPECT("Deep nested alert",
                "dc.west.rack.5.server.db1.disk.full.alert",
                4); /* AnyServerMetric, DatacenterWide, AllAlerts, CatchAll */

    /* ================================================================
     * SECTION 3: Microservices Event Routing
     * ================================================================ */
    printf("\n=== SECTION 3: Microservices Event Routing ===\n\n");

    cmdClear();
    cmdSubscribe("svc.*.event.created", "CreateEvents");
    cmdSubscribe("svc.*.event.*.failed", "FailureEvents");
    cmdSubscribe("svc.auth.#", "AuthService");
    cmdSubscribe("svc.*.event.#", "AllServiceEvents");
    cmdSubscribe("*.*.error.#", "GlobalErrors");
    cmdSubscribe("#", "CatchAll");

    printf("Testing microservices: svc.*.event.created, svc.*.event.*.failed, "
           "svc.auth.#, svc.*.event.#, *.*.error.#\n\n");

    TEST_EXPECT("User created event", "svc.users.event.created",
                3); /* CreateEvents, AllServiceEvents, CatchAll */
    TEST_EXPECT("Payment failed event", "svc.payments.event.charge.failed",
                3); /* FailureEvents, AllServiceEvents, CatchAll */
    TEST_EXPECT("Auth login event", "svc.auth.event.login",
                3); /* AuthService, AllServiceEvents, CatchAll */
    TEST_EXPECT("Auth token refresh", "svc.auth.token.refresh",
                2); /* AuthService, CatchAll */
    TEST_EXPECT("Global error from orders", "orders.api.error.timeout",
                2); /* GlobalErrors, CatchAll */
    TEST_EXPECT("Nested service error", "svc.inventory.error.db.connection",
                2); /* CatchAll (*.*.error needs 2 prefix segments) */

    /* ================================================================
     * SECTION 4: Financial/Trading Patterns
     * ================================================================ */
    printf("\n=== SECTION 4: Financial/Trading Patterns ===\n\n");

    cmdClear();
    cmdSubscribe("market.*.*.bid", "AllBids");
    cmdSubscribe("market.*.*.ask", "AllAsks");
    cmdSubscribe("market.nyse.#", "NYSEFeed");
    cmdSubscribe("market.*.tech.#", "TechStocks");
    cmdSubscribe("trade.*.*.executed", "Executions");
    cmdSubscribe("trade.#.rejected", "Rejections");
    cmdSubscribe("#", "CatchAll");

    printf("Testing financial: market.*.*.bid/ask, market.nyse.#, "
           "market.*.tech.#, trade patterns\n\n");

    TEST_EXPECT("NYSE tech stock bid", "market.nyse.tech.bid",
                4); /* AllBids, NYSEFeed, TechStocks, CatchAll */
    TEST_EXPECT("NASDAQ tech ask", "market.nasdaq.tech.ask",
                3); /* AllAsks, TechStocks, CatchAll */
    TEST_EXPECT("NYSE dividend announcement", "market.nyse.dividends.AAPL",
                2); /* NYSEFeed, CatchAll */
    TEST_EXPECT("Trade executed", "trade.broker1.GOOG.executed",
                2); /* Executions, CatchAll */
    TEST_EXPECT("Deep trade rejection", "trade.broker2.order.limit.rejected",
                2); /* Rejections, CatchAll */
    TEST_EXPECT("Simple rejection", "trade.rejected",
                2); /* Rejections, CatchAll */

    /* ================================================================
     * SECTION 5: Complex Multi-Wildcard Combinations
     * ================================================================ */
    printf("\n=== SECTION 5: Complex Multi-Wildcard Combinations ===\n\n");

    cmdClear();
    cmdSubscribe("*.*.*.*.leaf", "FourStarLeaf");
    cmdSubscribe("#.middle.#", "MiddleAnywhere");
    cmdSubscribe("*.#.*.end", "StarHashStarEnd");
    cmdSubscribe("a.*.b.*.c.*.d", "AlternatingPattern");
    cmdSubscribe("#", "CatchAll");

    printf("Testing complex: *.*.*.*.leaf, #.middle.#, *.#.*.end, "
           "a.*.b.*.c.*.d\n\n");

    TEST_EXPECT("Four segments to leaf", "a.b.c.d.leaf",
                2); /* FourStarLeaf, CatchAll */
    TEST_EXPECT("Five segments (too many for *.*.*.*.leaf)", "a.b.c.d.e.leaf",
                1); /* CatchAll only */
    TEST_EXPECT("Middle anywhere - start", "middle.x.y",
                2); /* MiddleAnywhere, CatchAll */
    TEST_EXPECT("Middle anywhere - middle", "a.b.middle.c.d",
                2); /* MiddleAnywhere, CatchAll */
    TEST_EXPECT("Middle anywhere - end", "x.y.z.middle",
                2); /* MiddleAnywhere, CatchAll */
    TEST_EXPECT("Alternating pattern match", "a.X.b.Y.c.Z.d",
                2); /* AlternatingPattern, CatchAll */
    TEST_EXPECT("Alternating pattern fail (missing segment)", "a.X.b.Y.c.d",
                1); /* CatchAll only */

    /* ================================================================
     * SECTION 6: Edge Cases and Boundary Conditions
     * ================================================================ */
    printf("\n=== SECTION 6: Edge Cases and Boundary Conditions ===\n\n");

    cmdClear();
    cmdSubscribe("#", "Everything");
    cmdSubscribe("*", "SingleSegment");
    cmdSubscribe("*.#", "OneOrMore");
    cmdSubscribe("#.*", "ZeroOrMoreThenOne");
    cmdSubscribe("a", "ExactA");

    printf("Testing edge cases: #, *, *.#, #.*, a\n\n");

    TEST_EXPECT(
        "Single segment 'x'", "x",
        4); /* Everything, SingleSegment, OneOrMore, ZeroOrMoreThenOne */
    TEST_EXPECT("Exact match 'a'", "a",
                5); /* Everything, SingleSegment, OneOrMore, ZeroOrMoreThenOne,
                       ExactA */
    TEST_EXPECT("Two segments 'a.b'", "a.b",
                3); /* Everything, OneOrMore, ZeroOrMoreThenOne */
    TEST_EXPECT("Deep path", "a.b.c.d.e.f.g.h.i.j",
                3); /* Everything, OneOrMore, ZeroOrMoreThenOne */

    /* ================================================================
     * SECTION 7: Real-World Logging/Observability
     * ================================================================ */
    printf("\n=== SECTION 7: Logging/Observability Patterns ===\n\n");

    cmdClear();
    cmdSubscribe("log.*.error", "ServiceErrors");
    cmdSubscribe("log.*.warn", "ServiceWarnings");
    cmdSubscribe("log.*.#", "AllServiceLogs");
    cmdSubscribe("log.#.error", "AnyError");
    cmdSubscribe("log.#.fatal", "AnyFatal");
    cmdSubscribe("metric.*.*.p99", "P99Latencies");
    cmdSubscribe("metric.#", "AllMetrics");
    cmdSubscribe("trace.*.span.#", "AllSpans");
    cmdSubscribe("#", "CatchAll");

    printf("Testing observability: log patterns, metric patterns, trace "
           "patterns\n\n");

    TEST_EXPECT("API error log", "log.api.error",
                4); /* ServiceErrors, AllServiceLogs, AnyError, CatchAll */
    TEST_EXPECT("Nested error log", "log.api.db.connection.error",
                3); /* AllServiceLogs, AnyError, CatchAll */
    TEST_EXPECT("Fatal from deep path", "log.worker.task.42.fatal",
                3); /* AllServiceLogs, AnyFatal, CatchAll */
    TEST_EXPECT("P99 latency metric", "metric.api.latency.p99",
                3); /* P99Latencies, AllMetrics, CatchAll */
    TEST_EXPECT("P50 latency (not p99)", "metric.api.latency.p50",
                2); /* AllMetrics, CatchAll */
    TEST_EXPECT("Trace span data", "trace.request123.span.db.query",
                2); /* AllSpans, CatchAll */

    /* ================================================================
     * SUMMARY
     * ================================================================ */
    printf("\n=====================================================\n");
    printf("[TEST] COMPREHENSIVE RESULTS: %d passed, %d failed\n", passed,
           failed);
    printf("=====================================================\n");

    if (failed == 0) {
        printf("\nAll pattern matching tests PASSED!\n");
        printf("The pub/sub system correctly handles:\n");
        printf("  - Single-segment wildcards (*)\n");
        printf("  - Multi-segment wildcards (#)\n");
        printf("  - Complex nested patterns\n");
        printf("  - Infrastructure/IoT hierarchies\n");
        printf("  - Microservices event routing\n");
        printf("  - Financial data patterns\n");
        printf("  - Observability/logging patterns\n");
        printf("  - Edge cases and boundaries\n");
    }

    return failed > 0 ? 1 : 0;
}

/* ============================================================================
 * Input Processing
 * ============================================================================
 */

static void processCommand(char *input) {
    /* Skip whitespace */
    while (*input && isspace(*input)) {
        input++;
    }

    /* Empty line */
    if (!*input) {
        return;
    }

    /* Parse command */
    char cmd[32] = {0};
    char arg1[256] = {0};
    char arg2[256] = {0};

    /* Try to parse: cmd arg1 arg2 */
    int n = sscanf(input, "%31s %255s %255[^\n]", cmd, arg1, arg2);

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "h") == 0 ||
        strcmp(cmd, "?") == 0) {
        printHelp();
    } else if (strcmp(cmd, "patterns") == 0) {
        printPatternGuide();
    } else if (strcmp(cmd, "sub") == 0 || strcmp(cmd, "subscribe") == 0) {
        cmdSubscribe(arg1, n >= 3 ? arg2 : NULL);
    } else if (strcmp(cmd, "unsub") == 0 || strcmp(cmd, "unsubscribe") == 0) {
        if (n >= 2) {
            cmdUnsubscribe(atoi(arg1));
        } else {
            printf("Usage: unsub <id>\n");
        }
    } else if (strcmp(cmd, "pub") == 0 || strcmp(cmd, "publish") == 0) {
        cmdPublish(arg1, n >= 3 ? arg2 : "");
    } else if (strcmp(cmd, "list") == 0 || strcmp(cmd, "ls") == 0) {
        cmdList();
    } else if (strcmp(cmd, "stats") == 0) {
        cmdStats();
    } else if (strcmp(cmd, "test") == 0) {
        cmdTestMatch(arg1);
    } else if (strcmp(cmd, "clear") == 0) {
        cmdClear();
    } else if (strcmp(cmd, "demo") == 0) {
        runDemo();
    } else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0 ||
               strcmp(cmd, "q") == 0) {
        loopyStop(g_loop);
    } else {
        printf("Unknown command: %s (type 'help' for commands)\n", cmd);
    }
}

/* Stdin callback - called when input is available */
static void onStdinReady(loopyLoop *loop, int fd, void *userData,
                         loopyAction mask) {
    (void)loop;
    (void)userData;
    (void)mask;

    char input[MAX_INPUT_LEN];
    if (fgets(input, sizeof(input), stdin) == NULL) {
        /* EOF - user pressed Ctrl+D */
        printf("\n");
        loopyStop(g_loop);
        return;
    }

    /* Remove trailing newline */
    size_t len = strlen(input);
    if (len > 0 && input[len - 1] == '\n') {
        input[len - 1] = '\0';
    }

    processCommand(input);

    /* Print prompt for next command */
    if (!loopyIsStopped(g_loop)) {
        printf(PROMPT);
        fflush(stdout);
    }
}

/* Signal handler */
static void onSignal(loopyLoop *loop, int signum, void *userData) {
    (void)signum;
    (void)userData;
    printf("\nReceived signal, stopping...\n");
    loopyStop(loop);
}

/* ============================================================================
 * Main
 * ============================================================================
 */

int main(int argc, char **argv) {
    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--test") == 0) {
            g_testMode = 1;
        } else if (strcmp(argv[i], "--demo") == 0) {
            g_demoMode = 1;
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [--demo] [--test] [--help]\n", argv[0]);
            printf("\n");
            printf("Options:\n");
            printf("  --demo   Run guided demonstration\n");
            printf("  --test   Run automated tests (for CI)\n");
            printf("  --help   Show this help\n");
            printf("\n");
            printf("Interactive commands: sub, unsub, pub, list, stats, demo, "
                   "help, quit\n");
            return 0;
        }
    }

    /* Create event loop */
    g_loop = loopyNew(64);
    if (!g_loop) {
        fprintf(stderr, "Failed to create event loop\n");
        return 1;
    }

    /* Set up signal handler */
    g_sigHandler = loopySignalNew(g_loop);
    if (g_sigHandler) {
        loopySignalRegister(g_sigHandler, SIGINT, onSignal, NULL);
    }

    /* Create pub/sub hub */
    loopyPubSubConfig config;
    loopyPubSubConfigInit(&config);
    config.enableStats = true;

    g_pubsub = loopyPubSubNew(g_loop, &config);
    if (!g_pubsub) {
        fprintf(stderr, "Failed to create pub/sub hub\n");
        if (g_sigHandler) {
            loopySignalFree(g_sigHandler);
        }
        loopyDelete(g_loop);
        return 1;
    }

    /* Test mode - run automated tests and exit */
    if (g_testMode) {
        int result = runTestMode();
        loopyPubSubFree(g_pubsub);
        if (g_sigHandler) {
            loopySignalFree(g_sigHandler);
        }
        loopyDelete(g_loop);
        return result;
    }

    /* Print welcome */
    printf("\n");
    printf(
        "================================================================\n");
    printf("  LOOPY PUB/SUB - Interactive Pattern Matching Explorer\n");
    printf(
        "================================================================\n");
    printf("\n");
    printf("Explore topic-based pub/sub with wildcard patterns:\n");
    printf("  '*' matches EXACTLY ONE segment\n");
    printf("  '#' matches ZERO OR MORE segments\n");
    printf("\n");
    printf(
        "Type 'help' for commands, 'demo' for guided tour, 'quit' to exit.\n");
    printf("\n");

    /* Demo mode - run demonstration then continue interactively */
    if (g_demoMode) {
        runDemo();
    }

    /* Register stdin for reading */
    if (!loopyRegisterRead(g_loop, STDIN_FILENO, onStdinReady, NULL)) {
        fprintf(stderr, "Failed to register stdin handler\n");
        loopyPubSubFree(g_pubsub);
        if (g_sigHandler) {
            loopySignalFree(g_sigHandler);
        }
        loopyDelete(g_loop);
        return 1;
    }

    /* Initial prompt */
    printf(PROMPT);
    fflush(stdout);

    /* Run event loop */
    loopyMain(g_loop);

    /* Cleanup */
    printf("\nCleaning up...\n");

    /* Unsubscribe all */
    for (int i = 0; i < g_subCount; i++) {
        loopyUnsubscribe(g_subs[i].sub);
    }

    loopyPubSubFree(g_pubsub);
    if (g_sigHandler) {
        loopySignalFree(g_sigHandler);
    }
    loopyDelete(g_loop);

    printf("Total messages received: %d\n", g_totalMessages);
    printf("Goodbye!\n");

    return 0;
}
