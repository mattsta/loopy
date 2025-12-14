/* loopyStressCLI - Interactive stress testing command-line tool
 *
 * Provides a flexible, runtime-configurable stress testing framework with:
 * - Dynamic producer/consumer/element count ranges
 * - Cross-product test generation
 * - Interactive and batch modes
 * - Configurable test scenarios
 * - Detailed reporting
 *
 * Copyright 2024 Matt Stancliff <matt@genges.com>
 * Licensed under the Apache License, Version 2.0
 */

#include "loopyPlatform.h"
#include <getopt.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../deps/datakit/src/datakit.h"
#include "loopyChannel.h"
#if LOOPY_HAVE_RAX
#include "loopyClusterRegistry.h"
#include "loopyPubSub.h"
#endif
#include "loopyRateLimit.h"
#include "loopyStressTest.h"

/* ====================================================================
 * Configuration Types
 * ==================================================================== */

typedef struct {
    int min;
    int max;
    int step;
} range_t;

typedef struct {
    range_t producers;
    range_t consumers;
    range_t elements;
    range_t channelCapacity;
    range_t messageSize;
    range_t durationMs;

    bool testChannel;
    bool testPubSub;
    bool testRegistry;
    bool testIntegration;

    bool verbose;
    bool csvOutput;
    bool stopOnFail;
    int iterations; /* Run each combo N times */
    uint64_t seed;

    const char *outputFile;
} stress_config_t;

typedef struct {
    int producers;
    int consumers;
    int elements;
    int channelCapacity;
    int messageSize;
    int durationMs;
} test_params_t;

typedef struct {
    test_params_t params;
    const char *testName;
    bool passed;
    uint64_t elapsedMs;
    uint64_t operations;
    uint64_t messagesLost;
    uint64_t checksumErrors;
    double opsPerSecond;
    char errorMsg[256];
} test_result_t;

/* ====================================================================
 * Global State
 * ==================================================================== */

static volatile sig_atomic_t g_interrupted = 0;
static stress_config_t g_config;
static FILE *g_output = NULL;

static void signal_handler(int sig) {
    (void)sig;
    g_interrupted = 1;
}

/* ====================================================================
 * Range Utilities
 * ==================================================================== */

static int range_count(const range_t *r) {
    if (r->max < r->min || r->step <= 0) {
        return 0;
    }
    return ((r->max - r->min) / r->step) + 1;
}

static int range_value(const range_t *r, int idx) {
    return r->min + (idx * r->step);
}

static range_t parse_range(const char *str) {
    range_t r = {1, 1, 1};

    /* Format: "N" or "MIN:MAX" or "MIN:MAX:STEP" */
    char *copy = zmalloc(strlen(str) + 1);
    strcpy(copy, str);
    char *token = strtok(copy, ":");

    if (token) {
        r.min = atoi(token);
        r.max = r.min;

        token = strtok(NULL, ":");
        if (token) {
            r.max = atoi(token);

            token = strtok(NULL, ":");
            if (token) {
                r.step = atoi(token);
            }
        }
    }

    zfree(copy);

    if (r.step <= 0) {
        r.step = 1;
    }
    if (r.max < r.min) {
        r.max = r.min;
    }

    return r;
}

/* ====================================================================
 * Test Result Tracking
 * ==================================================================== */

typedef struct {
    test_result_t *results;
    size_t count;
    size_t capacity;
    size_t passed;
    size_t failed;
} result_tracker_t;

static result_tracker_t g_tracker = {0};

static void tracker_init(size_t capacity) {
    g_tracker.results = zcalloc(capacity, sizeof(test_result_t));
    g_tracker.capacity = capacity;
    g_tracker.count = 0;
    g_tracker.passed = 0;
    g_tracker.failed = 0;
}

static void tracker_add(const test_result_t *result) {
    if (g_tracker.count >= g_tracker.capacity) {
        g_tracker.capacity *= 2;
        g_tracker.results = zrealloc(
            g_tracker.results, g_tracker.capacity * sizeof(test_result_t));
    }

    g_tracker.results[g_tracker.count++] = *result;

    if (result->passed) {
        g_tracker.passed++;
    } else {
        g_tracker.failed++;
    }
}

static void tracker_destroy(void) {
    zfree(g_tracker.results);
    memset(&g_tracker, 0, sizeof(g_tracker));
}

/* ====================================================================
 * Output Formatting
 * ==================================================================== */

static void print_header(void) {
    if (g_config.csvOutput) {
        fprintf(
            g_output,
            "test,producers,consumers,elements,capacity,msg_size,duration_ms,"
            "passed,elapsed_ms,operations,msgs_lost,checksum_errors,ops_per_"
            "sec,error\n");
    } else {
        fprintf(g_output, "\n");
        fprintf(g_output, "%-30s %4s %4s %8s %6s %6s %6s %6s %12s %12s %12s\n",
                "Test", "Prod", "Cons", "Elements", "Cap", "MsgSz", "DurMs",
                "Pass", "Elapsed", "Ops", "Ops/s");
        fprintf(g_output, "%-30s %4s %4s %8s %6s %6s %6s %6s %12s %12s %12s\n",
                "------------------------------", "----", "----", "--------",
                "------", "------", "------", "------", "------------",
                "------------", "------------");
    }
}

static void print_result(const test_result_t *r) {
    if (g_config.csvOutput) {
        fprintf(g_output, "%s,%d,%d,%d,%d,%d,%d,%s,%lu,%lu,%lu,%lu,%.2f,%s\n",
                r->testName, r->params.producers, r->params.consumers,
                r->params.elements, r->params.channelCapacity,
                r->params.messageSize, r->params.durationMs,
                r->passed ? "true" : "false", (unsigned long)r->elapsedMs,
                (unsigned long)r->operations, (unsigned long)r->messagesLost,
                (unsigned long)r->checksumErrors, r->opsPerSecond, r->errorMsg);
    } else {
        fprintf(g_output,
                "%-30s %4d %4d %8d %6d %6d %6d %6s %10lums %12lu %10.0f/s",
                r->testName, r->params.producers, r->params.consumers,
                r->params.elements, r->params.channelCapacity,
                r->params.messageSize, r->params.durationMs,
                r->passed ? "PASS" : "FAIL", (unsigned long)r->elapsedMs,
                (unsigned long)r->operations, r->opsPerSecond);

        if (!r->passed && r->errorMsg[0]) {
            fprintf(g_output, " [%s]", r->errorMsg);
        }
        fprintf(g_output, "\n");
    }

    fflush(g_output);
}

static void print_summary(void) {
    fprintf(g_output, "\n");
    fprintf(g_output, "=== Summary ===\n");
    fprintf(g_output, "Total tests: %zu\n", g_tracker.count);
    fprintf(g_output, "Passed:      %zu (%.1f%%)\n", g_tracker.passed,
            g_tracker.count > 0 ? (100.0 * g_tracker.passed / g_tracker.count)
                                : 0);
    fprintf(g_output, "Failed:      %zu\n", g_tracker.failed);

    if (g_tracker.failed > 0) {
        fprintf(g_output, "\nFailed tests:\n");
        for (size_t i = 0; i < g_tracker.count; i++) {
            if (!g_tracker.results[i].passed) {
                test_result_t *r = &g_tracker.results[i];
                fprintf(g_output, "  - %s [P=%d C=%d E=%d]: %s\n", r->testName,
                        r->params.producers, r->params.consumers,
                        r->params.elements, r->errorMsg);
            }
        }
    }
}

/* ====================================================================
 * Channel Stress Tests
 * ==================================================================== */

typedef struct {
    loopyChannel *channel;
    _Atomic(uint64_t) *producerSeqs;
    _Atomic(uint64_t) totalSent;
    _Atomic(uint64_t) totalRecv;
    _Atomic(uint64_t) checksumErrors;
    _Atomic(bool) producersDone;
    int numProducers;
    int elementsPerProducer;
    int messageSize;
} channel_test_ctx;

typedef struct {
    uint32_t producerId;
    uint64_t sequence;
    uint32_t checksum;
    uint8_t data[]; /* Variable size */
} channel_msg_t;

static uint32_t compute_checksum(const channel_msg_t *msg, size_t dataSize) {
    uint32_t sum = msg->producerId ^ (uint32_t)msg->sequence;
    for (size_t i = 0; i < dataSize; i++) {
        sum = (sum << 1) ^ msg->data[i];
    }
    return sum;
}

static void channel_setup(loopyStressWorker *worker) {
    channel_test_ctx *ctx = worker->sharedContext;
    worker->workerContext = zmalloc(ctx->messageSize);
}

static void channel_teardown(loopyStressWorker *worker) {
    zfree(worker->workerContext);
}

static int channel_producer_worker(loopyStressWorker *worker) {
    channel_test_ctx *ctx = worker->sharedContext;
    size_t dataSize = ctx->messageSize - offsetof(channel_msg_t, data);

    channel_msg_t *msg = (channel_msg_t *)worker->workerContext;
    msg->producerId = worker->workerId;
    msg->sequence = atomic_fetch_add(&ctx->producerSeqs[worker->workerId], 1);

    if ((int)msg->sequence >= ctx->elementsPerProducer) {
        return 0; /* Done */
    }

    /* Fill with deterministic pattern */
    uint64_t seed = msg->sequence ^ ((uint64_t)msg->producerId << 32);
    for (size_t i = 0; i < dataSize; i++) {
        msg->data[i] = (uint8_t)(seed = seed * 6364136223846793005ULL + 1);
    }
    msg->checksum = compute_checksum(msg, dataSize);

    loopyChannelStatus status =
        loopyChannelTrySend(ctx->channel, msg, ctx->messageSize);
    if (status == LOOPY_CHANNEL_OK) {
        atomic_fetch_add(&ctx->totalSent, 1);
        loopyStressStatAdd(worker->harness, STRESS_STAT_MESSAGES_SENT, 1);
    }

    return 0;
}

static int channel_consumer_worker(loopyStressWorker *worker) {
    channel_test_ctx *ctx = worker->sharedContext;
    size_t dataSize = ctx->messageSize - offsetof(channel_msg_t, data);

    channel_msg_t *msg = (channel_msg_t *)worker->workerContext;
    ssize_t n = loopyChannelTryRecv(ctx->channel, msg, ctx->messageSize);

    if (n > 0) {
        uint32_t expected = compute_checksum(msg, dataSize);
        if (msg->checksum != expected) {
            atomic_fetch_add(&ctx->checksumErrors, 1);
        }
        atomic_fetch_add(&ctx->totalRecv, 1);
        loopyStressStatAdd(worker->harness, STRESS_STAT_MESSAGES_RECV, 1);
    }

    return 0;
}

static test_result_t run_channel_mpmc_test(const test_params_t *params) {
    test_result_t result = {0};
    result.params = *params;
    result.testName = "channel_mpmc";

    /* Create channel */
    loopyChannelConfig chConfig;
    loopyChannelConfigInit(&chConfig);
    chConfig.type = LOOPY_CHANNEL_MPMC;
    chConfig.capacity = params->channelCapacity;
    chConfig.elementSize = params->messageSize;

    loopyChannel *ch = loopyChannelNew(NULL, &chConfig);
    if (!ch) {
        result.passed = false;
        snprintf(result.errorMsg, sizeof(result.errorMsg),
                 "failed to create channel");
        return result;
    }

    /* Allocate per-producer sequence counters */
    _Atomic(uint64_t) *producerSeqs =
        zcalloc(params->producers, sizeof(_Atomic(uint64_t)));

    channel_test_ctx ctx = {
        .channel = ch,
        .producerSeqs = producerSeqs,
        .totalSent = 0,
        .totalRecv = 0,
        .checksumErrors = 0,
        .producersDone = false,
        .numProducers = params->producers,
        .elementsPerProducer = params->elements / params->producers,
        .messageSize = params->messageSize,
    };

    uint64_t startTime = loopyStressTimeMs();

    /* Run producers */
    loopyStressConfig prodConfig;
    loopyStressConfigInit(&prodConfig);
    prodConfig.numWorkers = params->producers;
    prodConfig.iterationsPerWorker = ctx.elementsPerProducer;
    prodConfig.durationMs = 0;
    prodConfig.workerFn = channel_producer_worker;
    prodConfig.setupFn = channel_setup;
    prodConfig.teardownFn = channel_teardown;
    prodConfig.sharedContext = &ctx;
    prodConfig.seed = g_config.seed;

    loopyStressHarness *prodHarness = loopyStressHarnessNew(&prodConfig);
    loopyStressResults prodResults;
    loopyStressRun(prodHarness, &prodResults);
    loopyStressHarnessFree(prodHarness);

    atomic_store(&ctx.producersDone, true);

    /* Run consumers until channel drained or timeout */
    loopyStressConfig consConfig;
    loopyStressConfigInit(&consConfig);
    consConfig.numWorkers = params->consumers;
    consConfig.durationMs = params->durationMs;
    consConfig.workerFn = channel_consumer_worker;
    consConfig.setupFn = channel_setup;
    consConfig.teardownFn = channel_teardown;
    consConfig.sharedContext = &ctx;
    consConfig.seed = g_config.seed + 1;

    loopyStressHarness *consHarness = loopyStressHarnessNew(&consConfig);
    loopyStressResults consResults;
    loopyStressRun(consHarness, &consResults);
    loopyStressHarnessFree(consHarness);

    uint64_t endTime = loopyStressTimeMs();

    /* Collect results */
    result.elapsedMs = endTime - startTime;
    result.operations =
        atomic_load(&ctx.totalSent) + atomic_load(&ctx.totalRecv);
    result.messagesLost =
        atomic_load(&ctx.totalSent) - atomic_load(&ctx.totalRecv);
    result.checksumErrors = atomic_load(&ctx.checksumErrors);
    result.opsPerSecond = result.elapsedMs > 0
                              ? (1000.0 * result.operations / result.elapsedMs)
                              : 0;

    result.passed = (result.checksumErrors == 0) && prodResults.passed &&
                    consResults.passed;

    if (result.checksumErrors > 0) {
        snprintf(result.errorMsg, sizeof(result.errorMsg),
                 "%lu checksum errors", (unsigned long)result.checksumErrors);
    }

    zfree(producerSeqs);
    loopyChannelFree(ch);

    return result;
}

static test_result_t run_channel_spsc_test(const test_params_t *params) {
    test_params_t spscParams = *params;
    spscParams.producers = 1;
    spscParams.consumers = 1;

    test_result_t result = run_channel_mpmc_test(&spscParams);
    result.testName = "channel_spsc";
    return result;
}

#if LOOPY_HAVE_RAX
/* ====================================================================
 * PubSub Stress Tests
 * ==================================================================== */

typedef struct {
    loopyPubSub *ps;
    _Atomic(uint64_t) published;
    _Atomic(uint64_t) delivered;
    _Atomic(uint64_t) checksumErrors;
    int numTopics;
    int messageSize;
} pubsub_test_ctx;

static _Atomic(uint64_t) g_pubsub_delivered = 0;

static bool pubsub_test_callback(loopySubscription *sub,
                                 const loopyMessage *msg, void *userData) {
    (void)sub;
    pubsub_test_ctx *ctx = userData;

    if (ctx && msg->len >= sizeof(uint64_t)) {
        atomic_fetch_add(&ctx->delivered, 1);
    }
    atomic_fetch_add(&g_pubsub_delivered, 1);

    return true;
}

static void pubsub_setup(loopyStressWorker *worker) {
    pubsub_test_ctx *ctx = worker->sharedContext;
    worker->workerContext = zmalloc(ctx->messageSize);
}

static void pubsub_teardown(loopyStressWorker *worker) {
    zfree(worker->workerContext);
}

static int pubsub_publisher_worker(loopyStressWorker *worker) {
    pubsub_test_ctx *ctx = worker->sharedContext;

    /* Build topic name */
    int topicIdx = loopyStressWorkerRandRange(worker, ctx->numTopics);
    char topic[64];
    snprintf(topic, sizeof(topic), "stress.topic.%d", topicIdx);

    /* Build message */
    uint8_t *msg_buf = (uint8_t *)worker->workerContext;
    uint64_t seq = loopyStressWorkerRand(worker);
    memcpy(msg_buf, &seq, sizeof(seq));

    loopyPublish(ctx->ps, topic, msg_buf, ctx->messageSize);
    atomic_fetch_add(&ctx->published, 1);
    loopyStressStatAdd(worker->harness, STRESS_STAT_MESSAGES_SENT, 1);

    return 0;
}

static test_result_t run_pubsub_test(const test_params_t *params) {
    test_result_t result = {0};
    result.params = *params;
    result.testName = "pubsub_fanout";

    loopyPubSub *ps = loopyPubSubNew(NULL, NULL);
    if (!ps) {
        result.passed = false;
        snprintf(result.errorMsg, sizeof(result.errorMsg),
                 "failed to create pubsub");
        return result;
    }

    pubsub_test_ctx ctx = {
        .ps = ps,
        .published = 0,
        .delivered = 0,
        .checksumErrors = 0,
        .numTopics = 10,
        .messageSize = params->messageSize,
    };

    /* Create subscribers */
    loopySubscriptionConfig subConfig;
    loopySubscriptionConfigInit(&subConfig);
    subConfig.userData = &ctx;

    for (int i = 0; i < params->consumers; i++) {
        char pattern[64];
        snprintf(pattern, sizeof(pattern), "stress.topic.%d", i % 10);
        loopySubscribe(ps, pattern, pubsub_test_callback, &subConfig);
    }

    /* Wildcard subscriber */
    loopySubscribe(ps, "stress.#", pubsub_test_callback, &subConfig);

    atomic_store(&g_pubsub_delivered, 0);
    uint64_t startTime = loopyStressTimeMs();

    /* Run publishers */
    loopyStressConfig config;
    loopyStressConfigInit(&config);
    config.numWorkers = params->producers;
    config.iterationsPerWorker = params->elements / params->producers;
    config.durationMs = 0;
    config.workerFn = pubsub_publisher_worker;
    config.setupFn = pubsub_setup;
    config.teardownFn = pubsub_teardown;
    config.sharedContext = &ctx;
    config.seed = g_config.seed;

    loopyStressHarness *harness = loopyStressHarnessNew(&config);
    loopyStressResults results;
    loopyStressRun(harness, &results);
    loopyStressHarnessFree(harness);

    uint64_t endTime = loopyStressTimeMs();

    /* Collect results */
    result.elapsedMs = endTime - startTime;
    uint64_t published = atomic_load(&ctx.published);
    uint64_t delivered = atomic_load(&g_pubsub_delivered);
    result.operations = published + delivered;
    result.checksumErrors = atomic_load(&ctx.checksumErrors);
    result.opsPerSecond = result.elapsedMs > 0
                              ? (1000.0 * result.operations / result.elapsedMs)
                              : 0;

    /* PubSub should deliver to at least as many as published (wildcard) */
    result.passed = (published > 0) && (delivered >= published) &&
                    (result.checksumErrors == 0) && results.passed;

    if (!result.passed && published > 0 && delivered < published) {
        snprintf(result.errorMsg, sizeof(result.errorMsg),
                 "delivery mismatch: sent=%lu delivered=%lu",
                 (unsigned long)published, (unsigned long)delivered);
    }

    loopyPubSubFree(ps);
    return result;
}

/* ====================================================================
 * Registry Stress Tests
 * ==================================================================== */

typedef struct {
    loopyClusterRegistry *registry;
    _Atomic(uint64_t) nodesAdded;
    _Atomic(uint64_t) nodesRemoved;
    _Atomic(uint64_t) configSets;
    _Atomic(uint64_t) configGets;
    int numKeys;
} registry_test_ctx;

static int registry_worker(loopyStressWorker *worker) {
    registry_test_ctx *ctx = worker->sharedContext;

    char key[64];
    snprintf(key, sizeof(key), "stress.key.%u.%" PRIu64, worker->workerId,
             loopyStressWorkerRandRange(worker, ctx->numKeys));

    int op = loopyStressWorkerRandRange(worker, 4);

    switch (op) {
    case 0:
    case 1: {
        /* Set config */
        char value[64];
        snprintf(value, sizeof(value), "value_%" PRIu64,
                 loopyStressWorkerRand(worker));
        loopyClusterConfigSetString(ctx->registry, key, value, 0);
        atomic_fetch_add(&ctx->configSets, 1);
        break;
    }
    case 2: {
        /* Get config */
        loopyClusterConfigGetString(ctx->registry, key);
        atomic_fetch_add(&ctx->configGets, 1);
        break;
    }
    case 3: {
        /* Node operations */
        char nodeId[64];
        snprintf(nodeId, sizeof(nodeId), "node_%u_%" PRIu64, worker->workerId,
                 loopyStressWorkerRandRange(worker, 100));

        if (loopyStressWorkerRandRange(worker, 2) == 0) {
            loopyClusterNode *node = loopyClusterRegisterNode(
                ctx->registry, nodeId, NULL, LOOPY_ROLE_WORKER);
            if (node) {
                atomic_fetch_add(&ctx->nodesAdded, 1);
            }
        } else {
            if (loopyClusterUnregisterNode(ctx->registry, nodeId)) {
                atomic_fetch_add(&ctx->nodesRemoved, 1);
            }
        }
        break;
    }
    }

    loopyStressStatAdd(worker->harness, STRESS_STAT_OPERATIONS, 1);
    return 0;
}

static test_result_t run_registry_test(const test_params_t *params) {
    test_result_t result = {0};
    result.params = *params;
    result.testName = "registry_churn";

    loopyClusterConfig clConfig;
    loopyClusterConfigInit(&clConfig);
    clConfig.localNodeId = "stress_test_node";

    loopyClusterRegistry *registry = loopyClusterRegistryNew(&clConfig);
    if (!registry) {
        result.passed = false;
        snprintf(result.errorMsg, sizeof(result.errorMsg),
                 "failed to create registry");
        return result;
    }

    registry_test_ctx ctx = {
        .registry = registry,
        .nodesAdded = 0,
        .nodesRemoved = 0,
        .configSets = 0,
        .configGets = 0,
        .numKeys = 100,
    };

    uint64_t startTime = loopyStressTimeMs();

    loopyStressConfig config;
    loopyStressConfigInit(&config);
    config.numWorkers = params->producers; /* Use producers as worker count */
    config.iterationsPerWorker = params->elements / params->producers;
    config.durationMs = 0;
    config.workerFn = registry_worker;
    config.sharedContext = &ctx;
    config.seed = g_config.seed;

    loopyStressHarness *harness = loopyStressHarnessNew(&config);
    loopyStressResults results;
    loopyStressRun(harness, &results);
    loopyStressHarnessFree(harness);

    uint64_t endTime = loopyStressTimeMs();

    result.elapsedMs = endTime - startTime;
    result.operations =
        atomic_load(&ctx.configSets) + atomic_load(&ctx.configGets) +
        atomic_load(&ctx.nodesAdded) + atomic_load(&ctx.nodesRemoved);
    result.opsPerSecond = result.elapsedMs > 0
                              ? (1000.0 * result.operations / result.elapsedMs)
                              : 0;

    result.passed = results.passed;

    loopyClusterRegistryFree(registry);
    return result;
}

/* ====================================================================
 * Integration Stress Tests
 * ==================================================================== */

typedef struct {
    loopyChannel *channel;
    loopyPubSub *pubsub;
    _Atomic(uint64_t) channelOps;
    _Atomic(uint64_t) pubsubOps;
    _Atomic(bool) running;
    int messageSize;
} integration_test_ctx;

static void integration_setup(loopyStressWorker *worker) {
    integration_test_ctx *ctx = worker->sharedContext;
    worker->workerContext = zmalloc(ctx->messageSize);
}

static void integration_teardown(loopyStressWorker *worker) {
    zfree(worker->workerContext);
}

static int integration_worker(loopyStressWorker *worker) {
    integration_test_ctx *ctx = worker->sharedContext;

    int op = loopyStressWorkerRandRange(worker, 4);

    switch (op) {
    case 0: {
        /* Channel send */
        uint8_t *msg_buf = (uint8_t *)worker->workerContext;
        memset(msg_buf, 0x42, ctx->messageSize);
        loopyChannelTrySend(ctx->channel, msg_buf, ctx->messageSize);
        atomic_fetch_add(&ctx->channelOps, 1);
        break;
    }
    case 1: {
        /* Channel receive */
        uint8_t *msg_buf = (uint8_t *)worker->workerContext;
        loopyChannelTryRecv(ctx->channel, msg_buf, ctx->messageSize);
        atomic_fetch_add(&ctx->channelOps, 1);
        break;
    }
    case 2:
    case 3: {
        /* PubSub */
        char topic[64];
        snprintf(topic, sizeof(topic), "integration.topic.%" PRIu64,
                 loopyStressWorkerRandRange(worker, 10));
        uint8_t data[8] = {0};
        loopyPublish(ctx->pubsub, topic, data, sizeof(data));
        atomic_fetch_add(&ctx->pubsubOps, 1);
        break;
    }
    }

    loopyStressStatAdd(worker->harness, STRESS_STAT_OPERATIONS, 1);
    return 0;
}

static bool integration_sub_callback(loopySubscription *s,
                                     const loopyMessage *m, void *u) {
    (void)s;
    (void)m;
    (void)u;
    return true;
}

static test_result_t run_integration_test(const test_params_t *params) {
    test_result_t result = {0};
    result.params = *params;
    result.testName = "integration";

    /* Create channel */
    loopyChannelConfig chConfig;
    loopyChannelConfigInit(&chConfig);
    chConfig.type = LOOPY_CHANNEL_MPMC;
    chConfig.capacity = params->channelCapacity;
    chConfig.elementSize = params->messageSize;

    loopyChannel *ch = loopyChannelNew(NULL, &chConfig);
    loopyPubSub *ps = loopyPubSubNew(NULL, NULL);

    if (!ch || !ps) {
        result.passed = false;
        snprintf(result.errorMsg, sizeof(result.errorMsg),
                 "failed to create components");
        if (ch) {
            loopyChannelFree(ch);
        }
        if (ps) {
            loopyPubSubFree(ps);
        }
        return result;
    }

    /* Subscribe */
    loopySubscribe(ps, "integration.#", integration_sub_callback, NULL);

    integration_test_ctx ctx = {
        .channel = ch,
        .pubsub = ps,
        .channelOps = 0,
        .pubsubOps = 0,
        .running = true,
        .messageSize = params->messageSize,
    };

    uint64_t startTime = loopyStressTimeMs();

    loopyStressConfig config;
    loopyStressConfigInit(&config);
    config.numWorkers = params->producers + params->consumers;
    config.iterationsPerWorker = params->elements / config.numWorkers;
    config.durationMs = 0;
    config.workerFn = integration_worker;
    config.setupFn = integration_setup;
    config.teardownFn = integration_teardown;
    config.sharedContext = &ctx;
    config.seed = g_config.seed;

    loopyStressHarness *harness = loopyStressHarnessNew(&config);
    loopyStressResults results;
    loopyStressRun(harness, &results);
    loopyStressHarnessFree(harness);

    uint64_t endTime = loopyStressTimeMs();

    result.elapsedMs = endTime - startTime;
    result.operations =
        atomic_load(&ctx.channelOps) + atomic_load(&ctx.pubsubOps);
    result.opsPerSecond = result.elapsedMs > 0
                              ? (1000.0 * result.operations / result.elapsedMs)
                              : 0;

    result.passed = results.passed;

    loopyChannelFree(ch);
    loopyPubSubFree(ps);
    return result;
}
#endif /* LOOPY_HAVE_RAX */

/* ====================================================================
 * Test Runner
 * ==================================================================== */

typedef test_result_t (*test_fn_t)(const test_params_t *params);

typedef struct {
    const char *name;
    test_fn_t fn;
    bool enabled;
} test_def_t;

static test_def_t g_tests[] = {
    {"channel_mpmc", run_channel_mpmc_test, false},
    {"channel_spsc", run_channel_spsc_test, false},
#if LOOPY_HAVE_RAX
    {"pubsub", run_pubsub_test, false},
    {"registry", run_registry_test, false},
    {"integration", run_integration_test, false},
#endif
    {NULL, NULL, false},
};

static void enable_tests(void) {
    for (int i = 0; g_tests[i].name; i++) {
        if (strcmp(g_tests[i].name, "channel_mpmc") == 0 ||
            strcmp(g_tests[i].name, "channel_spsc") == 0) {
            g_tests[i].enabled = g_config.testChannel;
        }
#if LOOPY_HAVE_RAX
        else if (strcmp(g_tests[i].name, "pubsub") == 0) {
            g_tests[i].enabled = g_config.testPubSub;
        } else if (strcmp(g_tests[i].name, "registry") == 0) {
            g_tests[i].enabled = g_config.testRegistry;
        } else if (strcmp(g_tests[i].name, "integration") == 0) {
            g_tests[i].enabled = g_config.testIntegration;
        }
#endif
    }
}

static size_t count_combinations(void) {
    size_t tests = 0;
    for (int i = 0; g_tests[i].name; i++) {
        if (g_tests[i].enabled) {
            tests++;
        }
    }

    return tests * range_count(&g_config.producers) *
           range_count(&g_config.consumers) * range_count(&g_config.elements) *
           range_count(&g_config.channelCapacity) *
           range_count(&g_config.messageSize) * g_config.iterations;
}

static void run_all_tests(void) {
    size_t totalCombos = count_combinations();

    if (!g_config.csvOutput) {
        fprintf(g_output, "Running %zu test combinations...\n", totalCombos);
    }

    tracker_init(totalCombos > 0 ? totalCombos : 64);
    print_header();

    size_t combo = 0;
    for (int ti = 0; g_tests[ti].name && !g_interrupted; ti++) {
        if (!g_tests[ti].enabled) {
            continue;
        }

        for (int pi = 0;
             pi < range_count(&g_config.producers) && !g_interrupted; pi++) {
            for (int ci = 0;
                 ci < range_count(&g_config.consumers) && !g_interrupted;
                 ci++) {
                for (int ei = 0;
                     ei < range_count(&g_config.elements) && !g_interrupted;
                     ei++) {
                    for (int capi = 0;
                         capi < range_count(&g_config.channelCapacity) &&
                         !g_interrupted;
                         capi++) {
                        for (int mi = 0;
                             mi < range_count(&g_config.messageSize) &&
                             !g_interrupted;
                             mi++) {
                            for (int iter = 0;
                                 iter < g_config.iterations && !g_interrupted;
                                 iter++) {
                                test_params_t params = {
                                    .producers =
                                        range_value(&g_config.producers, pi),
                                    .consumers =
                                        range_value(&g_config.consumers, ci),
                                    .elements =
                                        range_value(&g_config.elements, ei),
                                    .channelCapacity = range_value(
                                        &g_config.channelCapacity, capi),
                                    .messageSize =
                                        range_value(&g_config.messageSize, mi),
                                    .durationMs = g_config.durationMs.min,
                                };

                                test_result_t result = g_tests[ti].fn(&params);

                                print_result(&result);
                                tracker_add(&result);

                                combo++;

                                if (!result.passed && g_config.stopOnFail) {
                                    fprintf(g_output,
                                            "\nStopping on first failure.\n");
                                    goto done;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    (void)combo; /* Suppress unused warning */

done:
    print_summary();
    tracker_destroy();
}

/* ====================================================================
 * Interactive Mode
 * ==================================================================== */

static void interactive_help(void) {
    printf("\nInteractive Commands:\n");
    printf("  run [test]              Run test(s) with current settings\n");
    printf("  set <param> <value>     Set parameter (producers, consumers, "
           "elements, etc.)\n");
    printf("  range <param> <spec>    Set range (e.g., 'range producers "
           "1:16:2')\n");
    printf("  show                    Show current configuration\n");
    printf("  tests                   List available tests\n");
    printf("  enable <test>           Enable a test\n");
    printf("  disable <test>          Disable a test\n");
    printf("  help                    Show this help\n");
    printf("  quit                    Exit\n");
    printf("\nRange spec format: MIN:MAX:STEP (e.g., '1:16:2' for "
           "1,3,5,7...15)\n");
}

static void show_config(void) {
    printf("\nCurrent Configuration:\n");
    printf("  Producers:       %d:%d:%d (%d values)\n", g_config.producers.min,
           g_config.producers.max, g_config.producers.step,
           range_count(&g_config.producers));
    printf("  Consumers:       %d:%d:%d (%d values)\n", g_config.consumers.min,
           g_config.consumers.max, g_config.consumers.step,
           range_count(&g_config.consumers));
    printf("  Elements:        %d:%d:%d (%d values)\n", g_config.elements.min,
           g_config.elements.max, g_config.elements.step,
           range_count(&g_config.elements));
    printf("  Channel Cap:     %d:%d:%d (%d values)\n",
           g_config.channelCapacity.min, g_config.channelCapacity.max,
           g_config.channelCapacity.step,
           range_count(&g_config.channelCapacity));
    printf("  Message Size:    %d:%d:%d (%d values)\n",
           g_config.messageSize.min, g_config.messageSize.max,
           g_config.messageSize.step, range_count(&g_config.messageSize));
    printf("  Duration (ms):   %d\n", g_config.durationMs.min);
    printf("  Iterations:      %d\n", g_config.iterations);
    printf("  Seed:            %lu\n", (unsigned long)g_config.seed);
    printf("\n  Enabled tests:\n");
    for (int i = 0; g_tests[i].name; i++) {
        printf("    [%c] %s\n", g_tests[i].enabled ? 'X' : ' ',
               g_tests[i].name);
    }
    printf("\n  Total combinations: %zu\n", count_combinations());
}

static void interactive_mode(void) {
    char line[256];

    printf("loopyStressCLI Interactive Mode\n");
    printf("Type 'help' for commands.\n");

    while (!g_interrupted) {
        printf("\nstress> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            break;
        }

        /* Remove newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
        }

        /* Parse command */
        char cmd[64] = {0};
        char arg1[64] = {0};
        char arg2[64] = {0};
        sscanf(line, "%63s %63s %63s", cmd, arg1, arg2);

        if (strlen(cmd) == 0) {
            continue;
        } else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0 ||
                   strcmp(cmd, "q") == 0) {
            break;
        } else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
            interactive_help();
        } else if (strcmp(cmd, "show") == 0) {
            show_config();
        } else if (strcmp(cmd, "tests") == 0) {
            printf("\nAvailable tests:\n");
            for (int i = 0; g_tests[i].name; i++) {
                printf("  [%c] %s\n", g_tests[i].enabled ? 'X' : ' ',
                       g_tests[i].name);
            }
        } else if (strcmp(cmd, "enable") == 0) {
            for (int i = 0; g_tests[i].name; i++) {
                if (strcmp(g_tests[i].name, arg1) == 0 ||
                    strcmp(arg1, "all") == 0) {
                    g_tests[i].enabled = true;
                    printf("Enabled: %s\n", g_tests[i].name);
                }
            }
        } else if (strcmp(cmd, "disable") == 0) {
            for (int i = 0; g_tests[i].name; i++) {
                if (strcmp(g_tests[i].name, arg1) == 0 ||
                    strcmp(arg1, "all") == 0) {
                    g_tests[i].enabled = false;
                    printf("Disabled: %s\n", g_tests[i].name);
                }
            }
        } else if (strcmp(cmd, "set") == 0 || strcmp(cmd, "range") == 0) {
            range_t r = parse_range(arg2);
            if (strcmp(arg1, "producers") == 0) {
                g_config.producers = r;
            } else if (strcmp(arg1, "consumers") == 0) {
                g_config.consumers = r;
            } else if (strcmp(arg1, "elements") == 0) {
                g_config.elements = r;
            } else if (strcmp(arg1, "capacity") == 0) {
                g_config.channelCapacity = r;
            } else if (strcmp(arg1, "msgsize") == 0) {
                g_config.messageSize = r;
            } else if (strcmp(arg1, "duration") == 0) {
                g_config.durationMs = r;
            } else if (strcmp(arg1, "iterations") == 0) {
                g_config.iterations = atoi(arg2);
            } else if (strcmp(arg1, "seed") == 0) {
                g_config.seed = strtoull(arg2, NULL, 0);
            } else {
                printf("Unknown parameter: %s\n", arg1);
                printf("Valid: producers, consumers, elements, capacity, "
                       "msgsize, duration, iterations, seed\n");
            }
            printf("Total combinations: %zu\n", count_combinations());
        } else if (strcmp(cmd, "run") == 0) {
            if (strlen(arg1) > 0 && strcmp(arg1, "all") != 0) {
                /* Run specific test */
                bool found = false;
                for (int i = 0; g_tests[i].name; i++) {
                    if (strcmp(g_tests[i].name, arg1) == 0) {
                        bool wasEnabled = g_tests[i].enabled;
                        g_tests[i].enabled = true;

                        /* Temporarily disable others */
                        for (int j = 0; g_tests[j].name; j++) {
                            if (i != j) {
                                g_tests[j].enabled = false;
                            }
                        }

                        run_all_tests();

                        /* Restore */
                        for (int j = 0; g_tests[j].name; j++) {
                            g_tests[j].enabled =
                                (i == j) ? wasEnabled : g_tests[j].enabled;
                        }

                        found = true;
                        break;
                    }
                }
                if (!found) {
                    printf("Unknown test: %s\n", arg1);
                }
            } else {
                run_all_tests();
            }
        } else {
            printf("Unknown command: %s (try 'help')\n", cmd);
        }
    }

    printf("\nGoodbye!\n");
}

/* ====================================================================
 * Main
 * ==================================================================== */

static void print_usage(const char *prog) {
    printf("Usage: %s [OPTIONS]\n\n", prog);
    printf("Dynamic stress testing CLI for loopy components.\n\n");
    printf("Options:\n");
    printf("  -p, --producers RANGE   Producer count range (default: 1:8:1)\n");
    printf("  -c, --consumers RANGE   Consumer count range (default: 1:8:1)\n");
    printf("  -e, --elements RANGE    Element count range (default: "
           "1000:10000:1000)\n");
    printf("  -C, --capacity RANGE    Channel capacity range (default: "
           "64:512:64)\n");
    printf(
        "  -m, --msgsize RANGE     Message size range (default: 64:256:64)\n");
    printf("  -d, --duration MS       Max duration per test (default: 5000)\n");
    printf(
        "  -n, --iterations N      Iterations per combination (default: 1)\n");
    printf("  -s, --seed N            Random seed (default: time-based)\n");
    printf("\n");
    printf("  --channel               Enable channel tests\n");
    printf("  --pubsub                Enable pubsub tests\n");
    printf("  --registry              Enable registry tests\n");
    printf("  --integration           Enable integration tests\n");
    printf("  --all                   Enable all tests\n");
    printf("\n");
    printf("  -i, --interactive       Interactive mode\n");
    printf("  --csv                   CSV output format\n");
    printf("  -o, --output FILE       Output file (default: stdout)\n");
    printf("  --stop-on-fail          Stop on first failure\n");
    printf("  -v, --verbose           Verbose output\n");
    printf("  -h, --help              Show this help\n");
    printf("\n");
    printf("Range format: MIN:MAX:STEP (e.g., '1:16:2' for 1,3,5,...,15)\n");
    printf("             or just N for a single value\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s --channel -p 1:8 -c 1:8 -e 10000\n", prog);
    printf("  %s --all -p 4 -c 4 -e 1000:10000:1000 --csv -o results.csv\n",
           prog);
    printf("  %s -i  # Interactive mode\n", prog);
}

int main(int argc, char *argv[]) {
    /* Set up signal handler */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Default configuration */
    g_config = (stress_config_t){
        .producers = {1, 8, 1},
        .consumers = {1, 8, 1},
        .elements = {1000, 10000, 1000},
        .channelCapacity = {64, 512, 64},
        .messageSize = {64, 256, 64},
        .durationMs = {5000, 5000, 1},
        .testChannel = false,
        .testPubSub = false,
        .testRegistry = false,
        .testIntegration = false,
        .verbose = false,
        .csvOutput = false,
        .stopOnFail = false,
        .iterations = 1,
        .seed = (uint64_t)time(NULL),
        .outputFile = NULL,
    };

    bool interactiveMode = false;

    static struct option long_options[] = {
        {"producers", required_argument, 0, 'p'},
        {"consumers", required_argument, 0, 'c'},
        {"elements", required_argument, 0, 'e'},
        {"capacity", required_argument, 0, 'C'},
        {"msgsize", required_argument, 0, 'm'},
        {"duration", required_argument, 0, 'd'},
        {"iterations", required_argument, 0, 'n'},
        {"seed", required_argument, 0, 's'},
        {"channel", no_argument, 0, 1001},
        {"pubsub", no_argument, 0, 1002},
        {"registry", no_argument, 0, 1003},
        {"integration", no_argument, 0, 1004},
        {"all", no_argument, 0, 1005},
        {"interactive", no_argument, 0, 'i'},
        {"csv", no_argument, 0, 1006},
        {"output", required_argument, 0, 'o'},
        {"stop-on-fail", no_argument, 0, 1007},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}};

    int opt;
    while ((opt = getopt_long(argc, argv, "p:c:e:C:m:d:n:s:io:vh", long_options,
                              NULL)) != -1) {
        switch (opt) {
        case 'p':
            g_config.producers = parse_range(optarg);
            break;
        case 'c':
            g_config.consumers = parse_range(optarg);
            break;
        case 'e':
            g_config.elements = parse_range(optarg);
            break;
        case 'C':
            g_config.channelCapacity = parse_range(optarg);
            break;
        case 'm':
            g_config.messageSize = parse_range(optarg);
            break;
        case 'd':
            g_config.durationMs = parse_range(optarg);
            break;
        case 'n':
            g_config.iterations = atoi(optarg);
            break;
        case 's':
            g_config.seed = strtoull(optarg, NULL, 0);
            break;
        case 1001:
            g_config.testChannel = true;
            break;
        case 1002:
            g_config.testPubSub = true;
            break;
        case 1003:
            g_config.testRegistry = true;
            break;
        case 1004:
            g_config.testIntegration = true;
            break;
        case 1005:
            g_config.testChannel = true;
            g_config.testPubSub = true;
            g_config.testRegistry = true;
            g_config.testIntegration = true;
            break;
        case 'i':
            interactiveMode = true;
            break;
        case 1006:
            g_config.csvOutput = true;
            break;
        case 'o':
            g_config.outputFile = optarg;
            break;
        case 1007:
            g_config.stopOnFail = true;
            break;
        case 'v':
            g_config.verbose = true;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    /* Open output file */
    if (g_config.outputFile) {
        g_output = fopen(g_config.outputFile, "w");
        if (!g_output) {
            fprintf(stderr, "Failed to open output file: %s\n",
                    g_config.outputFile);
            return 1;
        }
    } else {
        g_output = stdout;
    }

    enable_tests();

    if (interactiveMode) {
        interactive_mode();
    } else if (count_combinations() > 0) {
        run_all_tests();
    } else {
        fprintf(stderr, "No tests enabled. Use --channel, --pubsub, "
                        "--registry, --integration, or --all\n");
        print_usage(argv[0]);
        return 1;
    }

    if (g_output != stdout) {
        fclose(g_output);
    }

    return (g_tracker.failed > 0) ? 1 : 0;
}
