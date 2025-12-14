/* loopyClusterRegistry - Cluster node registry and configuration management
 *
 * Provides a lightweight cluster registry for:
 * - Node discovery and membership tracking
 * - Cluster hello/handshake protocol
 * - Distributed configuration key-value storage
 * - Event callbacks for cluster state changes
 *
 * This is designed for single-process coordination but can be extended
 * to inter-process/network coordination by implementing the transport
 * callbacks.
 *
 * Use Cases:
 * - Service discovery within a cluster
 * - Shared configuration across components
 * - Leader election participants
 * - Health monitoring targets
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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ====================================================================
 * Types
 * ==================================================================== */

/**
 * Opaque cluster registry handle.
 */
typedef struct loopyClusterRegistry loopyClusterRegistry;

/**
 * Opaque node handle.
 */
typedef struct loopyClusterNode loopyClusterNode;

/**
 * Node state.
 */
typedef enum loopyNodeState {
    LOOPY_NODE_UNKNOWN = 0,  /* Node state is unknown */
    LOOPY_NODE_JOINING = 1,  /* Node is in the process of joining */
    LOOPY_NODE_ACTIVE = 2,   /* Node is active and healthy */
    LOOPY_NODE_DRAINING = 3, /* Node is draining (preparing to leave) */
    LOOPY_NODE_LEAVING = 4,  /* Node is leaving the cluster */
    LOOPY_NODE_FAILED = 5,   /* Node has failed */
} loopyNodeState;

/**
 * Node role flags (can be combined).
 */
typedef enum loopyNodeRole {
    LOOPY_ROLE_NONE = 0,
    LOOPY_ROLE_WORKER = (1 << 0),    /* Can process work */
    LOOPY_ROLE_LEADER = (1 << 1),    /* Current leader */
    LOOPY_ROLE_CANDIDATE = (1 << 2), /* Candidate for leadership */
    LOOPY_ROLE_OBSERVER = (1 << 3),  /* Observer only, doesn't participate */
} loopyNodeRole;

/**
 * Configuration change type.
 */
typedef enum loopyConfigChangeType {
    LOOPY_CONFIG_SET = 0,    /* Key was set/updated */
    LOOPY_CONFIG_DELETE = 1, /* Key was deleted */
    LOOPY_CONFIG_EXPIRE = 2, /* Key expired */
} loopyConfigChangeType;

/**
 * Cluster event type.
 */
typedef enum loopyClusterEventType {
    LOOPY_CLUSTER_NODE_JOINED = 0,   /* New node joined */
    LOOPY_CLUSTER_NODE_LEFT = 1,     /* Node left (gracefully) */
    LOOPY_CLUSTER_NODE_FAILED = 2,   /* Node failed (ungracefully) */
    LOOPY_CLUSTER_NODE_STATE = 3,    /* Node state changed */
    LOOPY_CLUSTER_CONFIG_CHANGE = 4, /* Configuration changed */
    LOOPY_CLUSTER_LEADER_CHANGE = 5, /* Leader changed */
} loopyClusterEventType;

/**
 * Node information for callbacks.
 */
typedef struct loopyNodeInfo {
    const char *nodeId;   /* Node identifier */
    const char *address;  /* Node address (host:port or path) */
    loopyNodeState state; /* Current state */
    uint32_t roles;       /* Role flags */
    uint64_t joinTime;    /* When node joined (epoch ms) */
    uint64_t lastSeen;    /* Last heartbeat (epoch ms) */
    void *userData;       /* User-provided context */
} loopyNodeInfo;

/**
 * Configuration value structure.
 */
typedef struct loopyConfigValue {
    const char *key;   /* Configuration key */
    const void *value; /* Value data */
    size_t valueLen;   /* Value length */
    uint64_t version;  /* Version number */
    uint64_t expireAt; /* Expiration time (0 = never) */
} loopyConfigValue;

/**
 * Cluster event callback.
 *
 * @param registry  The registry
 * @param event     Event type
 * @param node      Node info (for node events) or NULL
 * @param config    Config value (for config events) or NULL
 * @param userData  User-provided context
 */
typedef void loopyClusterEventFn(loopyClusterRegistry *registry,
                                 loopyClusterEventType event,
                                 const loopyNodeInfo *node,
                                 const loopyConfigValue *config,
                                 void *userData);

/**
 * Registry configuration.
 */
typedef struct loopyClusterConfig {
    const char *localNodeId;      /* This node's ID (required) */
    const char *localAddress;     /* This node's address */
    uint32_t localRoles;          /* This node's roles */
    uint32_t heartbeatIntervalMs; /* Heartbeat interval (default 1000) */
    uint32_t
        failureTimeoutMs; /* Time until node is marked failed (default 5000) */
    bool autoHeartbeat;   /* Automatically send heartbeats */
    loopyClusterEventFn *eventCallback; /* Event callback */
    void *eventUserData;                /* User data for callback */
} loopyClusterConfig;

/* ====================================================================
 * Registry Lifecycle
 * ==================================================================== */

/**
 * Initialize configuration with defaults.
 *
 * @param config  Configuration to initialize
 */
void loopyClusterConfigInit(loopyClusterConfig *config);

/**
 * Create a new cluster registry.
 *
 * @param config  Configuration (localNodeId is required)
 * @return New registry, or NULL on error
 */
loopyClusterRegistry *loopyClusterRegistryNew(const loopyClusterConfig *config);

/**
 * Destroy a cluster registry.
 *
 * @param registry  The registry to destroy
 */
void loopyClusterRegistryFree(loopyClusterRegistry *registry);

/**
 * Get this node's ID.
 *
 * @param registry  The registry
 * @return Local node ID
 */
const char *loopyClusterLocalNodeId(const loopyClusterRegistry *registry);

/* ====================================================================
 * Node Management
 * ==================================================================== */

/**
 * Register a new node in the cluster.
 *
 * @param registry  The registry
 * @param nodeId    Unique node identifier
 * @param address   Node address (optional)
 * @param roles     Node roles
 * @return Node handle, or NULL on error
 */
loopyClusterNode *loopyClusterRegisterNode(loopyClusterRegistry *registry,
                                           const char *nodeId,
                                           const char *address, uint32_t roles);

/**
 * Unregister a node from the cluster.
 *
 * @param registry  The registry
 * @param nodeId    Node identifier
 * @return true if node was found and removed
 */
bool loopyClusterUnregisterNode(loopyClusterRegistry *registry,
                                const char *nodeId);

/**
 * Get a node by ID.
 *
 * @param registry  The registry
 * @param nodeId    Node identifier
 * @return Node handle, or NULL if not found
 */
loopyClusterNode *loopyClusterGetNode(loopyClusterRegistry *registry,
                                      const char *nodeId);

/**
 * Update node state.
 *
 * @param node   The node handle
 * @param state  New state
 * @return true on success
 */
bool loopyClusterNodeSetState(loopyClusterNode *node, loopyNodeState state);

/**
 * Update node roles.
 *
 * @param node   The node handle
 * @param roles  New roles (bitmask)
 * @return true on success
 */
bool loopyClusterNodeSetRoles(loopyClusterNode *node, uint32_t roles);

/**
 * Send heartbeat for a node (updates lastSeen).
 *
 * @param node  The node handle
 * @return true on success
 */
bool loopyClusterNodeHeartbeat(loopyClusterNode *node);

/**
 * Get node info.
 *
 * @param node  The node handle
 * @param info  Output info structure
 */
void loopyClusterNodeGetInfo(const loopyClusterNode *node, loopyNodeInfo *info);

/**
 * Set node user data.
 *
 * @param node      The node handle
 * @param userData  User data pointer
 */
void loopyClusterNodeSetUserData(loopyClusterNode *node, void *userData);

/**
 * Get node user data.
 *
 * @param node  The node handle
 * @return User data pointer
 */
void *loopyClusterNodeGetUserData(const loopyClusterNode *node);

/* ====================================================================
 * Node Querying
 * ==================================================================== */

/**
 * Get number of nodes in the cluster.
 *
 * @param registry  The registry
 * @return Number of registered nodes
 */
size_t loopyClusterNodeCount(const loopyClusterRegistry *registry);

/**
 * Get number of active nodes.
 *
 * @param registry  The registry
 * @return Number of nodes in ACTIVE state
 */
size_t loopyClusterActiveNodeCount(const loopyClusterRegistry *registry);

/**
 * Get current leader node.
 *
 * @param registry  The registry
 * @return Leader node, or NULL if no leader
 */
loopyClusterNode *loopyClusterGetLeader(loopyClusterRegistry *registry);

/**
 * Node iteration callback.
 *
 * @param node     The node
 * @param info     Node info
 * @param userData User argument
 * @return 0 to continue, non-zero to stop
 */
typedef int loopyClusterNodeIterFn(loopyClusterNode *node,
                                   const loopyNodeInfo *info, void *userData);

/**
 * Iterate all nodes.
 *
 * @param registry  The registry
 * @param fn        Iterator callback
 * @param userData  User argument
 * @return Number of nodes visited
 */
size_t loopyClusterIterateNodes(loopyClusterRegistry *registry,
                                loopyClusterNodeIterFn *fn, void *userData);

/**
 * Get nodes by role.
 *
 * @param registry  The registry
 * @param role      Role to filter by
 * @param fn        Iterator callback
 * @param userData  User argument
 * @return Number of matching nodes visited
 */
size_t loopyClusterIterateNodesByRole(loopyClusterRegistry *registry,
                                      loopyNodeRole role,
                                      loopyClusterNodeIterFn *fn,
                                      void *userData);

/* ====================================================================
 * Configuration Management
 * ==================================================================== */

/**
 * Set a configuration value.
 *
 * @param registry  The registry
 * @param key       Configuration key
 * @param value     Value data
 * @param valueLen  Value length
 * @param ttlMs     Time-to-live in ms (0 = no expiration)
 * @return Version number, or 0 on error
 */
uint64_t loopyClusterConfigSet(loopyClusterRegistry *registry, const char *key,
                               const void *value, size_t valueLen,
                               uint64_t ttlMs);

/**
 * Set a configuration string value.
 *
 * @param registry  The registry
 * @param key       Configuration key
 * @param value     String value
 * @param ttlMs     Time-to-live in ms (0 = no expiration)
 * @return Version number, or 0 on error
 */
uint64_t loopyClusterConfigSetString(loopyClusterRegistry *registry,
                                     const char *key, const char *value,
                                     uint64_t ttlMs);

/**
 * Get a configuration value.
 *
 * @param registry  The registry
 * @param key       Configuration key
 * @param valueLen  Output value length
 * @return Value data (owned by registry, do not free), or NULL if not found
 */
const void *loopyClusterConfigGet(loopyClusterRegistry *registry,
                                  const char *key, size_t *valueLen);

/**
 * Get a configuration string value.
 *
 * @param registry  The registry
 * @param key       Configuration key
 * @return String value (owned by registry), or NULL if not found
 */
const char *loopyClusterConfigGetString(loopyClusterRegistry *registry,
                                        const char *key);

/**
 * Delete a configuration value.
 *
 * @param registry  The registry
 * @param key       Configuration key
 * @return true if key was found and deleted
 */
bool loopyClusterConfigDelete(loopyClusterRegistry *registry, const char *key);

/**
 * Check if configuration key exists.
 *
 * @param registry  The registry
 * @param key       Configuration key
 * @return true if key exists and is not expired
 */
bool loopyClusterConfigExists(loopyClusterRegistry *registry, const char *key);

/**
 * Get configuration version.
 *
 * @param registry  The registry
 * @param key       Configuration key
 * @return Version number, or 0 if not found
 */
uint64_t loopyClusterConfigVersion(loopyClusterRegistry *registry,
                                   const char *key);

/**
 * Configuration iteration callback.
 *
 * @param key       Configuration key
 * @param value     Configuration value
 * @param userData  User argument
 * @return 0 to continue, non-zero to stop
 */
typedef int loopyClusterConfigIterFn(const char *key,
                                     const loopyConfigValue *value,
                                     void *userData);

/**
 * Iterate all configuration keys.
 *
 * @param registry  The registry
 * @param fn        Iterator callback
 * @param userData  User argument
 * @return Number of keys visited
 */
size_t loopyClusterConfigIterate(loopyClusterRegistry *registry,
                                 loopyClusterConfigIterFn *fn, void *userData);

/**
 * Iterate configuration keys matching a prefix.
 *
 * @param registry  The registry
 * @param prefix    Key prefix to match
 * @param fn        Iterator callback
 * @param userData  User argument
 * @return Number of matching keys visited
 */
size_t loopyClusterConfigIteratePrefix(loopyClusterRegistry *registry,
                                       const char *prefix,
                                       loopyClusterConfigIterFn *fn,
                                       void *userData);

/* ====================================================================
 * Cluster Operations
 * ==================================================================== */

/**
 * Process pending events (expired configs, failed nodes).
 * Call periodically if not using auto-heartbeat.
 *
 * @param registry  The registry
 * @return Number of events processed
 */
size_t loopyClusterProcess(loopyClusterRegistry *registry);

/**
 * Get current time in milliseconds (for testing/mocking).
 *
 * @param registry  The registry
 * @return Current epoch time in milliseconds
 */
uint64_t loopyClusterNow(const loopyClusterRegistry *registry);

/* ====================================================================
 * Utility
 * ==================================================================== */

/**
 * Get state name.
 *
 * @param state  Node state
 * @return Static string
 */
const char *loopyNodeStateName(loopyNodeState state);

/**
 * Get event type name.
 *
 * @param event  Event type
 * @return Static string
 */
const char *loopyClusterEventName(loopyClusterEventType event);
