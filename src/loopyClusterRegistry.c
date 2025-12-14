/* loopyClusterRegistry - Implementation
 *
 * Copyright 2024 Matt Stancliff <matt@genges.com>
 * Licensed under Apache License 2.0
 */

#include "loopyPlatform.h"

#include "loopyClusterRegistry.h"

#include "../deps/datakit/src/datakit.h"
#include "../deps/rax/src/rax.h"

#include <pthread.h>
#include <string.h>
#include <sys/time.h>

/* ====================================================================
 * Internal Structures
 * ==================================================================== */

/* Configuration entry stored in config rax */
typedef struct configEntry {
    void *value;
    size_t valueLen;
    uint64_t version;
    uint64_t expireAt; /* 0 = no expiration */
} configEntry;

/* Node structure */
struct loopyClusterNode {
    loopyClusterRegistry *registry;
    char *nodeId;
    char *address;
    loopyNodeState state;
    uint32_t roles;
    uint64_t joinTime;
    uint64_t lastSeen;
    void *userData;
};

/* Registry structure */
struct loopyClusterRegistry {
    char *localNodeId;
    loopyClusterConfig config;
    rax *nodes;      /* nodeId -> loopyClusterNode* */
    rax *configData; /* key -> configEntry* */
    uint64_t configVersion;
    pthread_rwlock_t lock;
};

/* ====================================================================
 * Helpers
 * ==================================================================== */

static char *registryStrdup(const char *s) {
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

static uint64_t currentTimeMs(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

static void freeNode(loopyClusterNode *node) {
    if (!node) {
        return;
    }
    zfree(node->nodeId);
    zfree(node->address);
    zfree(node);
}

static void freeConfigEntry(configEntry *entry) {
    if (!entry) {
        return;
    }
    zfree(entry->value);
    zfree(entry);
}

static void emitEvent(loopyClusterRegistry *registry,
                      loopyClusterEventType event, const loopyNodeInfo *node,
                      const loopyConfigValue *config) {
    if (registry->config.eventCallback) {
        registry->config.eventCallback(registry, event, node, config,
                                       registry->config.eventUserData);
    }
}

static void nodeToInfo(const loopyClusterNode *node, loopyNodeInfo *info) {
    info->nodeId = node->nodeId;
    info->address = node->address;
    info->state = node->state;
    info->roles = node->roles;
    info->joinTime = node->joinTime;
    info->lastSeen = node->lastSeen;
    info->userData = node->userData;
}

/* ====================================================================
 * Configuration
 * ==================================================================== */

void loopyClusterConfigInit(loopyClusterConfig *config) {
    if (!config) {
        return;
    }

    memset(config, 0, sizeof(*config));
    config->heartbeatIntervalMs = 1000;
    config->failureTimeoutMs = 5000;
    config->autoHeartbeat = false;
}

/* ====================================================================
 * Registry Lifecycle
 * ==================================================================== */

loopyClusterRegistry *
loopyClusterRegistryNew(const loopyClusterConfig *config) {
    if (!config || !config->localNodeId) {
        return NULL;
    }

    loopyClusterRegistry *registry = zcalloc(1, sizeof(loopyClusterRegistry));
    if (!registry) {
        return NULL;
    }

    registry->localNodeId = registryStrdup(config->localNodeId);
    if (!registry->localNodeId) {
        zfree(registry);
        return NULL;
    }

    registry->nodes = raxNew();
    if (!registry->nodes) {
        zfree(registry->localNodeId);
        zfree(registry);
        return NULL;
    }

    registry->configData = raxNew();
    if (!registry->configData) {
        raxFree(registry->nodes);
        zfree(registry->localNodeId);
        zfree(registry);
        return NULL;
    }

    if (pthread_rwlock_init(&registry->lock, NULL) != 0) {
        raxFree(registry->configData);
        raxFree(registry->nodes);
        zfree(registry->localNodeId);
        zfree(registry);
        return NULL;
    }

    registry->config = *config;
    registry->config.localNodeId = registry->localNodeId;
    registry->configVersion = 0;

    /* Register local node */
    loopyClusterRegisterNode(registry, config->localNodeId,
                             config->localAddress, config->localRoles);

    return registry;
}

void loopyClusterRegistryFree(loopyClusterRegistry *registry) {
    if (!registry) {
        return;
    }

    pthread_rwlock_wrlock(&registry->lock);

    /* Free all nodes */
    raxIterator iter;
    raxStart(&iter, registry->nodes);
    raxSeek(&iter, "^", NULL, 0);
    while (raxNext(&iter)) {
        loopyClusterNode *node = iter.data;
        freeNode(node);
    }
    raxStop(&iter);
    raxFree(registry->nodes);

    /* Free all config entries */
    raxStart(&iter, registry->configData);
    raxSeek(&iter, "^", NULL, 0);
    while (raxNext(&iter)) {
        configEntry *entry = iter.data;
        freeConfigEntry(entry);
    }
    raxStop(&iter);
    raxFree(registry->configData);

    pthread_rwlock_unlock(&registry->lock);
    pthread_rwlock_destroy(&registry->lock);

    zfree(registry->localNodeId);
    zfree(registry);
}

const char *loopyClusterLocalNodeId(const loopyClusterRegistry *registry) {
    return registry ? registry->localNodeId : NULL;
}

/* ====================================================================
 * Node Management
 * ==================================================================== */

loopyClusterNode *loopyClusterRegisterNode(loopyClusterRegistry *registry,
                                           const char *nodeId,
                                           const char *address,
                                           uint32_t roles) {
    if (!registry || !nodeId) {
        return NULL;
    }

    pthread_rwlock_wrlock(&registry->lock);

    /* Check if node already exists */
    loopyClusterNode *existing =
        raxFind(registry->nodes, (unsigned char *)nodeId, strlen(nodeId));
    if (existing != RAX_NOT_FOUND) {
        /* Update existing node */
        existing->lastSeen = currentTimeMs();
        if (address && !existing->address) {
            existing->address = registryStrdup(address);
        }
        existing->roles = roles;
        pthread_rwlock_unlock(&registry->lock);
        return existing;
    }

    /* Create new node */
    loopyClusterNode *node = zcalloc(1, sizeof(loopyClusterNode));
    if (!node) {
        pthread_rwlock_unlock(&registry->lock);
        return NULL;
    }

    node->registry = registry;
    node->nodeId = registryStrdup(nodeId);
    node->address = registryStrdup(address);
    node->state = LOOPY_NODE_JOINING;
    node->roles = roles;
    node->joinTime = currentTimeMs();
    node->lastSeen = node->joinTime;

    if (!node->nodeId) {
        freeNode(node);
        pthread_rwlock_unlock(&registry->lock);
        return NULL;
    }

    /* Insert into nodes */
    int ret = raxTryInsert(registry->nodes, (unsigned char *)nodeId,
                           strlen(nodeId), node, NULL);
    if (ret == 0) {
        /* Key already exists (race condition) */
        freeNode(node);
        pthread_rwlock_unlock(&registry->lock);
        return raxFind(registry->nodes, (unsigned char *)nodeId,
                       strlen(nodeId));
    }

    /* Move to active state immediately for local node */
    if (strcmp(nodeId, registry->localNodeId) == 0) {
        node->state = LOOPY_NODE_ACTIVE;
    }

    pthread_rwlock_unlock(&registry->lock);

    /* Emit event */
    loopyNodeInfo info;
    nodeToInfo(node, &info);
    emitEvent(registry, LOOPY_CLUSTER_NODE_JOINED, &info, NULL);

    return node;
}

bool loopyClusterUnregisterNode(loopyClusterRegistry *registry,
                                const char *nodeId) {
    if (!registry || !nodeId) {
        return false;
    }

    /* Don't allow unregistering local node */
    if (strcmp(nodeId, registry->localNodeId) == 0) {
        return false;
    }

    pthread_rwlock_wrlock(&registry->lock);

    loopyClusterNode *node =
        raxFind(registry->nodes, (unsigned char *)nodeId, strlen(nodeId));
    if (node == RAX_NOT_FOUND) {
        pthread_rwlock_unlock(&registry->lock);
        return false;
    }

    /* Get info before removing */
    loopyNodeInfo info;
    nodeToInfo(node, &info);

    raxRemove(registry->nodes, (unsigned char *)nodeId, strlen(nodeId), NULL);

    pthread_rwlock_unlock(&registry->lock);

    /* Emit event */
    emitEvent(registry, LOOPY_CLUSTER_NODE_LEFT, &info, NULL);

    freeNode(node);
    return true;
}

loopyClusterNode *loopyClusterGetNode(loopyClusterRegistry *registry,
                                      const char *nodeId) {
    if (!registry || !nodeId) {
        return NULL;
    }

    pthread_rwlock_rdlock(&registry->lock);

    loopyClusterNode *node =
        raxFind(registry->nodes, (unsigned char *)nodeId, strlen(nodeId));
    if (node == RAX_NOT_FOUND) {
        node = NULL;
    }

    pthread_rwlock_unlock(&registry->lock);

    return node;
}

bool loopyClusterNodeSetState(loopyClusterNode *node, loopyNodeState state) {
    if (!node) {
        return false;
    }

    loopyNodeState oldState = node->state;
    node->state = state;
    node->lastSeen = currentTimeMs();

    if (oldState != state) {
        loopyNodeInfo info;
        nodeToInfo(node, &info);
        emitEvent(node->registry, LOOPY_CLUSTER_NODE_STATE, &info, NULL);
    }

    return true;
}

bool loopyClusterNodeSetRoles(loopyClusterNode *node, uint32_t roles) {
    if (!node) {
        return false;
    }

    uint32_t oldRoles = node->roles;
    node->roles = roles;

    /* Check for leader change */
    bool wasLeader = (oldRoles & LOOPY_ROLE_LEADER) != 0;
    bool isLeader = (roles & LOOPY_ROLE_LEADER) != 0;

    if (wasLeader != isLeader) {
        loopyNodeInfo info;
        nodeToInfo(node, &info);
        emitEvent(node->registry, LOOPY_CLUSTER_LEADER_CHANGE, &info, NULL);
    }

    return true;
}

bool loopyClusterNodeHeartbeat(loopyClusterNode *node) {
    if (!node) {
        return false;
    }

    node->lastSeen = currentTimeMs();

    /* Transition joining nodes to active */
    if (node->state == LOOPY_NODE_JOINING) {
        loopyClusterNodeSetState(node, LOOPY_NODE_ACTIVE);
    }

    return true;
}

void loopyClusterNodeGetInfo(const loopyClusterNode *node,
                             loopyNodeInfo *info) {
    if (!node || !info) {
        return;
    }
    nodeToInfo(node, info);
}

void loopyClusterNodeSetUserData(loopyClusterNode *node, void *userData) {
    if (node) {
        node->userData = userData;
    }
}

void *loopyClusterNodeGetUserData(const loopyClusterNode *node) {
    return node ? node->userData : NULL;
}

/* ====================================================================
 * Node Querying
 * ==================================================================== */

size_t loopyClusterNodeCount(const loopyClusterRegistry *registry) {
    if (!registry) {
        return 0;
    }
    return raxSize(registry->nodes);
}

size_t loopyClusterActiveNodeCount(const loopyClusterRegistry *registry) {
    if (!registry) {
        return 0;
    }

    size_t count = 0;
    pthread_rwlock_rdlock((pthread_rwlock_t *)&registry->lock);

    raxIterator iter;
    raxStart(&iter, registry->nodes);
    raxSeek(&iter, "^", NULL, 0);
    while (raxNext(&iter)) {
        loopyClusterNode *node = iter.data;
        if (node->state == LOOPY_NODE_ACTIVE) {
            count++;
        }
    }
    raxStop(&iter);

    pthread_rwlock_unlock((pthread_rwlock_t *)&registry->lock);

    return count;
}

loopyClusterNode *loopyClusterGetLeader(loopyClusterRegistry *registry) {
    if (!registry) {
        return NULL;
    }

    loopyClusterNode *leader = NULL;

    pthread_rwlock_rdlock(&registry->lock);

    raxIterator iter;
    raxStart(&iter, registry->nodes);
    raxSeek(&iter, "^", NULL, 0);
    while (raxNext(&iter)) {
        loopyClusterNode *node = iter.data;
        if (node->roles & LOOPY_ROLE_LEADER) {
            leader = node;
            break;
        }
    }
    raxStop(&iter);

    pthread_rwlock_unlock(&registry->lock);

    return leader;
}

size_t loopyClusterIterateNodes(loopyClusterRegistry *registry,
                                loopyClusterNodeIterFn *fn, void *userData) {
    if (!registry || !fn) {
        return 0;
    }

    size_t count = 0;

    pthread_rwlock_rdlock(&registry->lock);

    raxIterator iter;
    raxStart(&iter, registry->nodes);
    raxSeek(&iter, "^", NULL, 0);
    while (raxNext(&iter)) {
        loopyClusterNode *node = iter.data;
        loopyNodeInfo info;
        nodeToInfo(node, &info);

        count++;
        if (fn(node, &info, userData) != 0) {
            break;
        }
    }
    raxStop(&iter);

    pthread_rwlock_unlock(&registry->lock);

    return count;
}

size_t loopyClusterIterateNodesByRole(loopyClusterRegistry *registry,
                                      loopyNodeRole role,
                                      loopyClusterNodeIterFn *fn,
                                      void *userData) {
    if (!registry || !fn) {
        return 0;
    }

    size_t count = 0;

    pthread_rwlock_rdlock(&registry->lock);

    raxIterator iter;
    raxStart(&iter, registry->nodes);
    raxSeek(&iter, "^", NULL, 0);
    while (raxNext(&iter)) {
        loopyClusterNode *node = iter.data;
        if (node->roles & role) {
            loopyNodeInfo info;
            nodeToInfo(node, &info);

            count++;
            if (fn(node, &info, userData) != 0) {
                break;
            }
        }
    }
    raxStop(&iter);

    pthread_rwlock_unlock(&registry->lock);

    return count;
}

/* ====================================================================
 * Configuration Management
 * ==================================================================== */

uint64_t loopyClusterConfigSet(loopyClusterRegistry *registry, const char *key,
                               const void *value, size_t valueLen,
                               uint64_t ttlMs) {
    if (!registry || !key || !value || valueLen == 0) {
        return 0;
    }

    pthread_rwlock_wrlock(&registry->lock);

    /* Check for existing entry */
    configEntry *existing =
        raxFind(registry->configData, (unsigned char *)key, strlen(key));
    if (existing != RAX_NOT_FOUND) {
        /* Update existing */
        void *newValue = zmalloc(valueLen);
        if (!newValue) {
            pthread_rwlock_unlock(&registry->lock);
            return 0;
        }
        memcpy(newValue, value, valueLen);

        zfree(existing->value);
        existing->value = newValue;
        existing->valueLen = valueLen;
        existing->version = ++registry->configVersion;
        existing->expireAt = ttlMs > 0 ? currentTimeMs() + ttlMs : 0;

        pthread_rwlock_unlock(&registry->lock);

        /* Emit event */
        loopyConfigValue cv = {
            .key = key,
            .value = existing->value,
            .valueLen = existing->valueLen,
            .version = existing->version,
            .expireAt = existing->expireAt,
        };
        emitEvent(registry, LOOPY_CLUSTER_CONFIG_CHANGE, NULL, &cv);

        return existing->version;
    }

    /* Create new entry */
    configEntry *entry = zcalloc(1, sizeof(configEntry));
    if (!entry) {
        pthread_rwlock_unlock(&registry->lock);
        return 0;
    }

    entry->value = zmalloc(valueLen);
    if (!entry->value) {
        zfree(entry);
        pthread_rwlock_unlock(&registry->lock);
        return 0;
    }
    memcpy(entry->value, value, valueLen);
    entry->valueLen = valueLen;
    entry->version = ++registry->configVersion;
    entry->expireAt = ttlMs > 0 ? currentTimeMs() + ttlMs : 0;

    int ret = raxInsert(registry->configData, (unsigned char *)key, strlen(key),
                        entry, NULL);
    if (ret == 0) {
        freeConfigEntry(entry);
        pthread_rwlock_unlock(&registry->lock);
        return 0;
    }

    uint64_t version = entry->version;

    pthread_rwlock_unlock(&registry->lock);

    /* Emit event */
    loopyConfigValue cv = {
        .key = key,
        .value = entry->value,
        .valueLen = entry->valueLen,
        .version = entry->version,
        .expireAt = entry->expireAt,
    };
    emitEvent(registry, LOOPY_CLUSTER_CONFIG_CHANGE, NULL, &cv);

    return version;
}

uint64_t loopyClusterConfigSetString(loopyClusterRegistry *registry,
                                     const char *key, const char *value,
                                     uint64_t ttlMs) {
    if (!value) {
        return 0;
    }
    return loopyClusterConfigSet(registry, key, value, strlen(value) + 1,
                                 ttlMs);
}

const void *loopyClusterConfigGet(loopyClusterRegistry *registry,
                                  const char *key, size_t *valueLen) {
    if (!registry || !key) {
        return NULL;
    }

    pthread_rwlock_rdlock(&registry->lock);

    configEntry *entry =
        raxFind(registry->configData, (unsigned char *)key, strlen(key));
    if (entry == RAX_NOT_FOUND) {
        pthread_rwlock_unlock(&registry->lock);
        return NULL;
    }

    /* Check expiration */
    if (entry->expireAt > 0 && currentTimeMs() >= entry->expireAt) {
        pthread_rwlock_unlock(&registry->lock);
        return NULL;
    }

    if (valueLen) {
        *valueLen = entry->valueLen;
    }
    const void *value = entry->value;

    pthread_rwlock_unlock(&registry->lock);

    return value;
}

const char *loopyClusterConfigGetString(loopyClusterRegistry *registry,
                                        const char *key) {
    size_t len;
    const void *value = loopyClusterConfigGet(registry, key, &len);
    return (const char *)value;
}

bool loopyClusterConfigDelete(loopyClusterRegistry *registry, const char *key) {
    if (!registry || !key) {
        return false;
    }

    pthread_rwlock_wrlock(&registry->lock);

    void *old = NULL;
    int ret = raxRemove(registry->configData, (unsigned char *)key, strlen(key),
                        &old);

    pthread_rwlock_unlock(&registry->lock);

    if (ret && old) {
        configEntry *entry = old;

        /* Emit event */
        loopyConfigValue cv = {
            .key = key,
            .value = entry->value,
            .valueLen = entry->valueLen,
            .version = entry->version,
            .expireAt = 0,
        };
        emitEvent(registry, LOOPY_CLUSTER_CONFIG_CHANGE, NULL, &cv);

        freeConfigEntry(entry);
        return true;
    }

    return false;
}

bool loopyClusterConfigExists(loopyClusterRegistry *registry, const char *key) {
    if (!registry || !key) {
        return false;
    }

    pthread_rwlock_rdlock(&registry->lock);

    configEntry *entry =
        raxFind(registry->configData, (unsigned char *)key, strlen(key));
    if (entry == RAX_NOT_FOUND) {
        pthread_rwlock_unlock(&registry->lock);
        return false;
    }

    /* Check expiration */
    bool exists = entry->expireAt == 0 || currentTimeMs() < entry->expireAt;

    pthread_rwlock_unlock(&registry->lock);

    return exists;
}

uint64_t loopyClusterConfigVersion(loopyClusterRegistry *registry,
                                   const char *key) {
    if (!registry || !key) {
        return 0;
    }

    pthread_rwlock_rdlock(&registry->lock);

    configEntry *entry =
        raxFind(registry->configData, (unsigned char *)key, strlen(key));
    if (entry == RAX_NOT_FOUND) {
        pthread_rwlock_unlock(&registry->lock);
        return 0;
    }

    uint64_t version = entry->version;

    pthread_rwlock_unlock(&registry->lock);

    return version;
}

size_t loopyClusterConfigIterate(loopyClusterRegistry *registry,
                                 loopyClusterConfigIterFn *fn, void *userData) {
    if (!registry || !fn) {
        return 0;
    }

    size_t count = 0;
    uint64_t now = currentTimeMs();

    pthread_rwlock_rdlock(&registry->lock);

    raxIterator iter;
    raxStart(&iter, registry->configData);
    raxSeek(&iter, "^", NULL, 0);
    while (raxNext(&iter)) {
        configEntry *entry = iter.data;

        /* Skip expired entries */
        if (entry->expireAt > 0 && now >= entry->expireAt) {
            continue;
        }

        /* Build key string (null-terminate) */
        char key[256];
        size_t keyLen = iter.keyLen < 255 ? iter.keyLen : 255;
        memcpy(key, iter.key, keyLen);
        key[keyLen] = '\0';

        loopyConfigValue cv = {
            .key = key,
            .value = entry->value,
            .valueLen = entry->valueLen,
            .version = entry->version,
            .expireAt = entry->expireAt,
        };

        count++;
        if (fn(key, &cv, userData) != 0) {
            break;
        }
    }
    raxStop(&iter);

    pthread_rwlock_unlock(&registry->lock);

    return count;
}

size_t loopyClusterConfigIteratePrefix(loopyClusterRegistry *registry,
                                       const char *prefix,
                                       loopyClusterConfigIterFn *fn,
                                       void *userData) {
    if (!registry || !prefix || !fn) {
        return 0;
    }

    size_t count = 0;
    size_t prefixLen = strlen(prefix);
    uint64_t now = currentTimeMs();

    pthread_rwlock_rdlock(&registry->lock);

    raxIterator iter;
    raxStart(&iter, registry->configData);
    raxSeek(&iter, ">=", (unsigned char *)prefix, prefixLen);

    while (raxNext(&iter)) {
        /* Check if still matches prefix */
        if (iter.keyLen < prefixLen ||
            memcmp(iter.key, prefix, prefixLen) != 0) {
            break;
        }

        configEntry *entry = iter.data;

        /* Skip expired entries */
        if (entry->expireAt > 0 && now >= entry->expireAt) {
            continue;
        }

        /* Build key string */
        char key[256];
        size_t keyLen = iter.keyLen < 255 ? iter.keyLen : 255;
        memcpy(key, iter.key, keyLen);
        key[keyLen] = '\0';

        loopyConfigValue cv = {
            .key = key,
            .value = entry->value,
            .valueLen = entry->valueLen,
            .version = entry->version,
            .expireAt = entry->expireAt,
        };

        count++;
        if (fn(key, &cv, userData) != 0) {
            break;
        }
    }
    raxStop(&iter);

    pthread_rwlock_unlock(&registry->lock);

    return count;
}

/* ====================================================================
 * Cluster Operations
 * ==================================================================== */

size_t loopyClusterProcess(loopyClusterRegistry *registry) {
    if (!registry) {
        return 0;
    }

    size_t events = 0;
    uint64_t now = currentTimeMs();
    uint64_t failureTimeout = registry->config.failureTimeoutMs;

    pthread_rwlock_wrlock(&registry->lock);

    /* Check for expired config entries */
    raxIterator iter;
    raxStart(&iter, registry->configData);
    raxSeek(&iter, "^", NULL, 0);

    /* Collect expired keys (can't modify during iteration) */
    char *expiredKeys[64];
    size_t expiredCount = 0;

    while (raxNext(&iter) && expiredCount < 64) {
        configEntry *entry = iter.data;
        if (entry->expireAt > 0 && now >= entry->expireAt) {
            char *key = zmalloc(iter.keyLen + 1);
            if (key) {
                memcpy(key, iter.key, iter.keyLen);
                key[iter.keyLen] = '\0';
                expiredKeys[expiredCount++] = key;
            }
        }
    }
    raxStop(&iter);

    /* Remove expired entries */
    for (size_t i = 0; i < expiredCount; i++) {
        void *old = NULL;
        raxRemove(registry->configData, (unsigned char *)expiredKeys[i],
                  strlen(expiredKeys[i]), &old);
        if (old) {
            configEntry *entry = old;

            loopyConfigValue cv = {
                .key = expiredKeys[i],
                .value = entry->value,
                .valueLen = entry->valueLen,
                .version = entry->version,
                .expireAt = entry->expireAt,
            };

            /* Emit outside lock would be safer, but we're in a simple
             * implementation */
            if (registry->config.eventCallback) {
                registry->config.eventCallback(
                    registry, LOOPY_CLUSTER_CONFIG_CHANGE, NULL, &cv,
                    registry->config.eventUserData);
            }

            freeConfigEntry(entry);
            events++;
        }
        zfree(expiredKeys[i]);
    }

    /* Check for failed nodes */
    raxStart(&iter, registry->nodes);
    raxSeek(&iter, "^", NULL, 0);

    char *failedNodeIds[64];
    size_t failedCount = 0;

    while (raxNext(&iter) && failedCount < 64) {
        loopyClusterNode *node = iter.data;

        /* Skip local node */
        if (strcmp(node->nodeId, registry->localNodeId) == 0) {
            continue;
        }

        /* Skip already failed nodes */
        if (node->state == LOOPY_NODE_FAILED) {
            continue;
        }

        /* Check for timeout */
        if (now - node->lastSeen > failureTimeout) {
            char *nodeId = registryStrdup(node->nodeId);
            if (nodeId) {
                failedNodeIds[failedCount++] = nodeId;
            }
        }
    }
    raxStop(&iter);

    /* Mark failed nodes */
    for (size_t i = 0; i < failedCount; i++) {
        loopyClusterNode *node =
            raxFind(registry->nodes, (unsigned char *)failedNodeIds[i],
                    strlen(failedNodeIds[i]));
        if (node != RAX_NOT_FOUND) {
            node->state = LOOPY_NODE_FAILED;

            loopyNodeInfo info;
            nodeToInfo(node, &info);

            if (registry->config.eventCallback) {
                registry->config.eventCallback(
                    registry, LOOPY_CLUSTER_NODE_FAILED, &info, NULL,
                    registry->config.eventUserData);
            }
            events++;
        }
        zfree(failedNodeIds[i]);
    }

    pthread_rwlock_unlock(&registry->lock);

    return events;
}

uint64_t loopyClusterNow(const loopyClusterRegistry *registry) {
    (void)registry;
    return currentTimeMs();
}

/* ====================================================================
 * Utility
 * ==================================================================== */

const char *loopyNodeStateName(loopyNodeState state) {
    switch (state) {
    case LOOPY_NODE_UNKNOWN:
        return "UNKNOWN";
    case LOOPY_NODE_JOINING:
        return "JOINING";
    case LOOPY_NODE_ACTIVE:
        return "ACTIVE";
    case LOOPY_NODE_DRAINING:
        return "DRAINING";
    case LOOPY_NODE_LEAVING:
        return "LEAVING";
    case LOOPY_NODE_FAILED:
        return "FAILED";
    default:
        return "INVALID";
    }
}

const char *loopyClusterEventName(loopyClusterEventType event) {
    switch (event) {
    case LOOPY_CLUSTER_NODE_JOINED:
        return "NODE_JOINED";
    case LOOPY_CLUSTER_NODE_LEFT:
        return "NODE_LEFT";
    case LOOPY_CLUSTER_NODE_FAILED:
        return "NODE_FAILED";
    case LOOPY_CLUSTER_NODE_STATE:
        return "NODE_STATE";
    case LOOPY_CLUSTER_CONFIG_CHANGE:
        return "CONFIG_CHANGE";
    case LOOPY_CLUSTER_LEADER_CHANGE:
        return "LEADER_CHANGE";
    default:
        return "INVALID";
    }
}
