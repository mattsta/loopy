/* pubsub_network.c - Networked Peer-to-Peer Pub/Sub System
 *
 * This comprehensive example demonstrates loopy's full feature space by
 * implementing a distributed pub/sub messaging system where multiple instances
 * can connect as peers and route messages across the network.
 *
 * ============================================================================
 * FEATURES DEMONSTRATED
 * ============================================================================
 *
 * 1. EVENT LOOP (loopy.h)
 *    - Main event loop with loopyMain()
 *    - File descriptor monitoring
 *    - Clean shutdown
 *
 * 2. TCP NETWORKING (loopyStream.h)
 *    - Server: Accept incoming peer connections
 *    - Client: Connect to remote peers
 *    - Non-blocking async I/O
 *    - Graceful connection handling
 *
 * 3. PUB/SUB MESSAGING (loopyPubSub.h)
 *    - Topic-based subscriptions with wildcards (* and #)
 *    - Local message delivery
 *    - Network message routing
 *
 * 4. TIMERS (loopyTimer.h)
 *    - Heartbeat/keepalive timers
 *    - Reconnection backoff
 *
 * 5. SIGNALS (loopySignal.h)
 *    - Graceful SIGINT handling
 *
 * ============================================================================
 * PROTOCOL
 * ============================================================================
 *
 * Simple line-based protocol for easy debugging with netcat:
 *
 *   HELLO <name> <addr:port> - Identify self on connect with listen address
 *   CLUSTER <name> <addr>    - Announce cluster member (sent for each known
 * peer) SUB <pattern>            - Subscribe to pattern (e.g., "weather.#")
 *   UNSUB <pattern>          - Unsubscribe from pattern
 *   PUB <topic> <message>    - Publish message to topic
 *   MSG <topic> <message>    - Forwarded message from another peer
 *   PING                     - Keepalive
 *   PONG                     - Keepalive response
 *
 * Cluster Discovery:
 *   When connecting to any peer, you receive CLUSTER messages with all known
 *   members. With --auto-connect, the node automatically connects to new peers.
 *
 * ============================================================================
 * USAGE
 * ============================================================================
 *
 * Start first peer (server):
 *   ./pubsub_network --port 9000 --name peer1
 *
 * Connect second peer:
 *   ./pubsub_network --port 9001 --name peer2 --connect localhost:9000
 *
 * Connect third peer to both:
 *   ./pubsub_network --port 9002 --name peer3 \
 *       --connect localhost:9000 --connect localhost:9001
 *
 * Interactive commands:
 *   sub <pattern>           Subscribe to topic pattern
 *   unsub <pattern>         Unsubscribe from pattern
 *   pub <topic> <msg>       Publish message
 *   connect <host:port>     Connect to peer
 *   peers                   List connected peers
 *   cluster                 Show cluster membership (all discovered nodes)
 *   subs                    List local subscriptions
 *   stats                   Show statistics
 *   help                    Show help
 *   quit                    Exit
 *
 * Cluster auto-discovery:
 *   ./pubsub_network --port 9002 --auto-connect --connect localhost:9000
 *   (Automatically connects to all peers discovered through the cluster)
 *
 * Test mode:
 *   ./pubsub_network --test
 *
 * Copyright 2024 Matt Stancliff <matt@genges.com>
 * Licensed under the Apache License, Version 2.0
 */

#include "loopy.h"
#include "loopyClusterRegistry.h"
#include "loopyNet.h"
#include "loopyPubSub.h"
#include "loopySignal.h"
#include "loopyStream.h"
#include "loopyTimer.h"

#include "../deps/datakit/src/datakit.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ============================================================================
 * Configuration
 * ============================================================================
 */

#define MAX_PEERS 32
#define MAX_SUBS 64
#define MAX_LINE 4096
#define DEFAULT_PORT 9000
#define HEARTBEAT_INTERVAL_MS 5000
#define RECONNECT_DELAY_MS 3000
#define PROMPT "net-pubsub> "

/* ============================================================================
 * Types
 * ============================================================================
 */

/* Forward declaration */
struct PeerConn;

/* Peer connection state */
typedef enum { PEER_CONNECTING, PEER_CONNECTED, PEER_CLOSING } PeerState;

/* Peer connection */
typedef struct PeerConn {
    loopyStream *stream;
    char name[64];
    char addr[64];
    int port;
    PeerState state;
    char readBuf[MAX_LINE];
    size_t readBufUsed;
    uint64_t msgSent;
    uint64_t msgRecv;
    bool isInbound; /* true if peer connected to us */
} PeerConn;

/* Network subscription tracking (for routing) */
typedef struct {
    char pattern[128];
    loopySubscription *localSub;
    bool isLocal;     /* true if local user subscribed */
    int peerRefCount; /* how many peers have this subscription */
} NetSub;

/* Global state */
static struct {
    loopyLoop *loop;
    loopyPubSub *pubsub;
    loopyStream *server;
    loopySignalHandler *sigHandler;
    loopyTimer *heartbeat;
    loopyClusterRegistry *cluster;

    char myName[64];
    char myAddr[128];
    int myPort;

    PeerConn peers[MAX_PEERS];
    int peerCount;

    NetSub subs[MAX_SUBS];
    int subCount;

    bool testMode;
    int testResult;
    int testMsgCount;
    bool autoConnect; /* Auto-connect to discovered peers */
} G;

/* ============================================================================
 * Utility Functions
 * ============================================================================
 */

static void trimNewline(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[--len] = '\0';
    }
}

static PeerConn *findPeerByStream(loopyStream *stream) {
    for (int i = 0; i < G.peerCount; i++) {
        if (G.peers[i].stream == stream) {
            return &G.peers[i];
        }
    }
    return NULL;
}

static PeerConn *findPeerByName(const char *name) {
    for (int i = 0; i < G.peerCount; i++) {
        if (strcmp(G.peers[i].name, name) == 0) {
            return &G.peers[i];
        }
    }
    return NULL;
}

static PeerConn *findPeerByAddress(const char *addr, int port) {
    for (int i = 0; i < G.peerCount; i++) {
        if (strcmp(G.peers[i].addr, addr) == 0 && G.peers[i].port == port) {
            return &G.peers[i];
        }
    }
    return NULL;
}

/* Check if we're already connected or connecting to an address */
static bool isConnectedTo(const char *addr, int port) {
    /* Check if it's our own address */
    if (port == G.myPort) {
        /* Simple check - could be more sophisticated with hostname resolution
         */
        if (strcmp(addr, "localhost") == 0 || strcmp(addr, "127.0.0.1") == 0 ||
            strcmp(addr, G.myAddr) == 0) {
            return true;
        }
    }
    return findPeerByAddress(addr, port) != NULL;
}

static void removePeer(PeerConn *peer) {
    if (!peer) {
        return;
    }

    printf("[Network] Peer disconnected: %s (%s:%d)\n", peer->name, peer->addr,
           peer->port);

    /* Update cluster registry - mark node as failed */
    if (G.cluster && peer->name[0]) {
        loopyClusterNode *node = loopyClusterGetNode(G.cluster, peer->name);
        if (node) {
            loopyClusterNodeSetState(node, LOOPY_NODE_FAILED);
        }
    }

    /* Find and remove from array */
    int idx = peer - G.peers;
    if (idx >= 0 && idx < G.peerCount) {
        for (int i = idx; i < G.peerCount - 1; i++) {
            G.peers[i] = G.peers[i + 1];
        }
        G.peerCount--;
    }
}

/* ============================================================================
 * Protocol - Send Messages
 * ============================================================================
 */

static void sendToPeer(PeerConn *peer, const char *fmt, ...) {
    if (!peer || !peer->stream || peer->state != PEER_CONNECTED) {
        return;
    }

    char buf[MAX_LINE];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf) - 2, fmt, args);
    va_end(args);

    if (len > 0) {
        buf[len++] = '\n';
        buf[len] = '\0';

        char *copy = zmalloc(len + 1);
        if (copy) {
            memcpy(copy, buf, len + 1);
            loopyStreamWrite(peer->stream, copy, len, NULL, copy);
            peer->msgSent++;
        }
    }
}

static void broadcastToPeers(const char *fmt, ...) {
    char buf[MAX_LINE];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    for (int i = 0; i < G.peerCount; i++) {
        if (G.peers[i].state == PEER_CONNECTED) {
            sendToPeer(&G.peers[i], "%s", buf);
        }
    }
}

/* Forward declaration for auto-connect */
static void cmdConnect(const char *hostport);

/* Cluster node iterator for sending CLUSTER messages */
static int sendClusterMemberIter(loopyClusterNode *node,
                                 const loopyNodeInfo *info, void *userData) {
    PeerConn *peer = (PeerConn *)userData;

    /* Don't send info about the peer we're sending to */
    if (strcmp(info->nodeId, peer->name) == 0) {
        return 0;
    }

    /* Send CLUSTER message with node info */
    sendToPeer(peer, "CLUSTER %s %s", info->nodeId,
               info->address ? info->address : "unknown");
    return 0;
}

/* Send full cluster membership to a peer */
static void sendClusterMembership(PeerConn *peer) {
    if (!G.cluster) {
        return;
    }

    /* Send our own info first */
    sendToPeer(peer, "CLUSTER %s %s", G.myName, G.myAddr);

    /* Send all known cluster members */
    loopyClusterIterateNodes(G.cluster, sendClusterMemberIter, peer);
}

/* Handle discovered cluster member - optionally auto-connect */
static void handleDiscoveredPeer(const char *name, const char *address) {
    /* Skip if it's ourselves */
    if (strcmp(name, G.myName) == 0) {
        return;
    }

    /* Parse address (supports IPv4 and IPv6) */
    char host[256] = {0};
    int port = DEFAULT_PORT;

    if (!loopyNetParseAddr(address, host, sizeof(host), &port, DEFAULT_PORT)) {
        if (!G.testMode) {
            printf("[Cluster] Warning: Invalid address format: %s\n", address);
        }
        return;
    }

    /* Register in cluster registry if not already known */
    if (G.cluster) {
        loopyClusterNode *node = loopyClusterGetNode(G.cluster, name);
        if (!node) {
            node = loopyClusterRegisterNode(G.cluster, name, address,
                                            LOOPY_ROLE_WORKER);
            if (node) {
                loopyClusterNodeSetState(node, LOOPY_NODE_ACTIVE);
            }
            if (!G.testMode) {
                printf("[Cluster] Discovered: %s at %s\n", name, address);
            }
        }
    }

    /* Auto-connect if enabled and not already connected */
    if (G.autoConnect && !isConnectedTo(host, port)) {
        if (!G.testMode) {
            printf("[Cluster] Auto-connecting to %s (%s)\n", name, address);
        }
        /* Pass the original address to preserve IPv6 brackets */
        cmdConnect(address);
    }
}

/* ============================================================================
 * Network Subscriptions
 * ============================================================================
 */

static bool localMessageCallback(loopySubscription *sub,
                                 const loopyMessage *msg, void *userData);

static NetSub *findNetSub(const char *pattern) {
    for (int i = 0; i < G.subCount; i++) {
        if (strcmp(G.subs[i].pattern, pattern) == 0) {
            return &G.subs[i];
        }
    }
    return NULL;
}

static NetSub *addNetSub(const char *pattern, bool isLocal) {
    if (G.subCount >= MAX_SUBS) {
        return NULL;
    }

    NetSub *ns = findNetSub(pattern);
    if (ns) {
        if (isLocal) {
            ns->isLocal = true;
        } else {
            ns->peerRefCount++;
        }
        return ns;
    }

    ns = &G.subs[G.subCount++];
    strncpy(ns->pattern, pattern, sizeof(ns->pattern) - 1);
    ns->pattern[sizeof(ns->pattern) - 1] = '\0';
    ns->isLocal = isLocal;
    ns->peerRefCount = isLocal ? 0 : 1;

    /* Create local pub/sub subscription to receive messages */
    ns->localSub =
        loopySubscribe(G.pubsub, pattern, localMessageCallback,
                       &(loopySubscriptionConfig){
                           .userData = ns, .deliveryMode = LOOPY_DELIVER_SYNC});

    return ns;
}

static void removeNetSub(const char *pattern, bool isLocal) {
    for (int i = 0; i < G.subCount; i++) {
        if (strcmp(G.subs[i].pattern, pattern) == 0) {
            if (isLocal) {
                G.subs[i].isLocal = false;
            } else {
                G.subs[i].peerRefCount--;
            }

            /* Remove if no references */
            if (!G.subs[i].isLocal && G.subs[i].peerRefCount <= 0) {
                if (G.subs[i].localSub) {
                    loopyUnsubscribe(G.subs[i].localSub);
                }
                for (int j = i; j < G.subCount - 1; j++) {
                    G.subs[j] = G.subs[j + 1];
                }
                G.subCount--;
            }
            return;
        }
    }
}

/* ============================================================================
 * Message Routing
 * ============================================================================
 */

/* Called when local pub/sub receives a message */
static bool localMessageCallback(loopySubscription *sub,
                                 const loopyMessage *msg, void *userData) {
    NetSub *ns = (NetSub *)userData;
    (void)sub;

    if (G.testMode) {
        G.testMsgCount++;
        printf("  [%s] Received: topic='%s' data='%.*s'\n", ns->pattern,
               msg->topic, (int)msg->len, (const char *)msg->data);
        return true;
    }

    /* Display locally */
    printf("\n");
    printf("  +-- MESSAGE --+\n");
    printf("  | Pattern: %s\n", ns->pattern);
    printf("  | Topic:   %s\n", msg->topic);
    printf("  | Data:    %.*s\n", (int)msg->len, (const char *)msg->data);
    printf("  +------------+\n");
    printf(PROMPT);
    fflush(stdout);

    return true;
}

/* Forward a published message to all peers */
static void forwardToNetwork(const char *topic, const char *data) {
    broadcastToPeers("MSG %s %s", topic, data);
}

/* ============================================================================
 * Protocol - Handle Incoming Messages
 * ============================================================================
 */

static void handlePeerMessage(PeerConn *peer, char *line) {
    peer->msgRecv++;
    trimNewline(line);

    if (strlen(line) == 0) {
        return;
    }

    char cmd[32] = {0};
    char arg1[256] = {0};
    char arg2[MAX_LINE] = {0};

    sscanf(line, "%31s %255s %[^\n]", cmd, arg1, arg2);

    if (strcmp(cmd, "HELLO") == 0) {
        strncpy(peer->name, arg1, sizeof(peer->name) - 1);
        peer->name[sizeof(peer->name) - 1] = '\0';
        peer->state = PEER_CONNECTED;

        /* arg2 may contain the peer's listen address */
        char peerListenAddr[128] = {0};
        if (arg2[0]) {
            strncpy(peerListenAddr, arg2, sizeof(peerListenAddr) - 1);
        } else {
            /* Fall back to peer's connection address */
            snprintf(peerListenAddr, sizeof(peerListenAddr), "%s:%d",
                     peer->addr, peer->port);
        }

        printf("[Network] Peer identified: %s (listen: %s)\n", peer->name,
               peerListenAddr);

        /* Register in cluster registry */
        if (G.cluster) {
            loopyClusterNode *node = loopyClusterGetNode(G.cluster, peer->name);
            if (!node) {
                node = loopyClusterRegisterNode(
                    G.cluster, peer->name, peerListenAddr, LOOPY_ROLE_WORKER);
            }
            if (node) {
                loopyClusterNodeSetState(node, LOOPY_NODE_ACTIVE);
                loopyClusterNodeHeartbeat(node);
            }
        }

        /* Send cluster membership to the new peer */
        sendClusterMembership(peer);

        /* Send our subscriptions to the new peer */
        for (int i = 0; i < G.subCount; i++) {
            if (G.subs[i].isLocal) {
                sendToPeer(peer, "SUB %s", G.subs[i].pattern);
            }
        }

    } else if (strcmp(cmd, "CLUSTER") == 0) {
        /* Cluster member announcement - arg1 is name, arg2 is address */
        handleDiscoveredPeer(arg1, arg2);

    } else if (strcmp(cmd, "SUB") == 0) {
        /* Peer is subscribing - track it for routing */
        addNetSub(arg1, false);
        if (!G.testMode) {
            printf("[%s] Subscribed to: %s\n", peer->name, arg1);
        }

    } else if (strcmp(cmd, "UNSUB") == 0) {
        removeNetSub(arg1, false);
        if (!G.testMode) {
            printf("[%s] Unsubscribed from: %s\n", peer->name, arg1);
        }

    } else if (strcmp(cmd, "PUB") == 0) {
        /* Peer published - deliver locally and forward to other peers */
        loopyPublish(G.pubsub, arg1, arg2, strlen(arg2));

        /* Forward to other peers (excluding sender) */
        for (int i = 0; i < G.peerCount; i++) {
            if (&G.peers[i] != peer && G.peers[i].state == PEER_CONNECTED) {
                sendToPeer(&G.peers[i], "MSG %s %s", arg1, arg2);
            }
        }

    } else if (strcmp(cmd, "MSG") == 0) {
        /* Forwarded message - deliver locally only (don't re-forward) */
        loopyPublish(G.pubsub, arg1, arg2, strlen(arg2));

    } else if (strcmp(cmd, "PING") == 0) {
        sendToPeer(peer, "PONG");

    } else if (strcmp(cmd, "PONG") == 0) {
        /* Keepalive response - peer is alive */
    }
}

/* ============================================================================
 * Network Callbacks
 * ============================================================================
 */

static void onPeerClose(loopyStream *stream, void *userData) {
    (void)userData;
    PeerConn *peer = findPeerByStream(stream);
    if (peer) {
        removePeer(peer);
    }
}

static void onPeerAlloc(loopyStream *stream, size_t suggested, void **buf,
                        size_t *bufLen, void *userData) {
    (void)stream;
    (void)suggested;
    PeerConn *peer = (PeerConn *)userData;

    /* Provide remaining space in read buffer */
    *buf = peer->readBuf + peer->readBufUsed;
    *bufLen = sizeof(peer->readBuf) - peer->readBufUsed - 1;
}

static void onPeerRead(loopyStream *stream, ssize_t nread, const void *buf,
                       void *userData) {
    PeerConn *peer = (PeerConn *)userData;
    (void)buf;

    if (nread <= 0) {
        /* EOF or error */
        loopyStreamClose(stream, onPeerClose, peer);
        return;
    }

    peer->readBufUsed += nread;
    peer->readBuf[peer->readBufUsed] = '\0';

    /* Process complete lines */
    char *line = peer->readBuf;
    char *newline;
    while ((newline = strchr(line, '\n')) != NULL) {
        *newline = '\0';
        handlePeerMessage(peer, line);
        line = newline + 1;
    }

    /* Move remaining partial line to start of buffer */
    if (line != peer->readBuf) {
        size_t remaining = peer->readBufUsed - (line - peer->readBuf);
        memmove(peer->readBuf, line, remaining);
        peer->readBufUsed = remaining;
    }
}

static void initPeerConnection(PeerConn *peer) {
    /* Start reading */
    if (!loopyStreamReadStart(peer->stream, onPeerAlloc, onPeerRead, peer)) {
        loopyStreamClose(peer->stream, onPeerClose, peer);
        return;
    }

    /* Send HELLO with our listen address for cluster discovery */
    peer->state = PEER_CONNECTED;
    sendToPeer(peer, "HELLO %s %s", G.myName, G.myAddr);
}

static void onConnectComplete(loopyStream *stream, int status, void *userData) {
    PeerConn *peer = (PeerConn *)userData;

    if (status < 0) {
        printf("[Network] Failed to connect to %s:%d\n", peer->addr,
               peer->port);
        loopyStreamClose(stream, NULL, NULL);
        removePeer(peer);
        return;
    }

    printf("[Network] Connected to %s:%d\n", peer->addr, peer->port);
    initPeerConnection(peer);
}

static void onNewConnection(loopyStream *server, int status, void *userData) {
    (void)userData;

    if (status < 0) {
        fprintf(stderr, "[Network] Connection error\n");
        return;
    }

    if (G.peerCount >= MAX_PEERS) {
        fprintf(stderr, "[Network] Max peers reached, rejecting\n");
        loopyStream *temp = loopyStreamAccept(server);
        if (temp) {
            loopyStreamClose(temp, NULL, NULL);
        }
        return;
    }

    loopyStream *client = loopyStreamAccept(server);
    if (!client) {
        return;
    }

    PeerConn *peer = &G.peers[G.peerCount++];
    memset(peer, 0, sizeof(*peer));
    peer->stream = client;
    peer->state = PEER_CONNECTING;
    peer->isInbound = true;
    snprintf(peer->name, sizeof(peer->name), "peer%d", G.peerCount);

    loopyStreamGetPeerName(client, peer->addr, sizeof(peer->addr), &peer->port);
    loopyStreamSetData(client, peer);

    printf("[Network] Incoming connection from %s:%d\n", peer->addr,
           peer->port);
    initPeerConnection(peer);
}

/* ============================================================================
 * Commands
 * ============================================================================
 */

static void cmdConnect(const char *hostport) {
    if (!hostport || !*hostport) {
        printf("Usage: connect <host:port>\n");
        printf("  IPv4:  connect 192.168.1.1:9000\n");
        printf("  IPv6:  connect [::1]:9000\n");
        return;
    }

    char host[256] = {0};
    int port = DEFAULT_PORT;

    if (!loopyNetParseAddr(hostport, host, sizeof(host), &port, DEFAULT_PORT)) {
        printf("Error: Invalid address format: %s\n", hostport);
        printf("  IPv4 example: 192.168.1.1:9000\n");
        printf("  IPv6 example: [::1]:9000\n");
        return;
    }

    if (G.peerCount >= MAX_PEERS) {
        printf("Error: Maximum peers (%d) reached\n", MAX_PEERS);
        return;
    }

    /* Check if already connected */
    if (isConnectedTo(host, port)) {
        printf("Already connected to %s:%d\n", host, port);
        return;
    }

    loopyStream *stream = loopyStreamNewTcp(G.loop);
    if (!stream) {
        printf("Error: Failed to create socket\n");
        return;
    }

    PeerConn *peer = &G.peers[G.peerCount++];
    memset(peer, 0, sizeof(*peer));
    peer->stream = stream;
    peer->state = PEER_CONNECTING;
    peer->isInbound = false;
    peer->port = port;
    strncpy(peer->addr, host, sizeof(peer->addr) - 1);
    loopyNetFormatAddr(peer->name, sizeof(peer->name), host, port);
    loopyStreamSetData(stream, peer);

    printf("[Network] Connecting to %s:%d...\n", host, port);

    if (!loopyStreamConnect(stream, host, port, onConnectComplete, peer)) {
        printf("Error: Connect failed\n");
        G.peerCount--;
        loopyStreamClose(stream, NULL, NULL);
    }
}

static void cmdSubscribe(const char *pattern) {
    if (!pattern || !*pattern) {
        printf("Usage: sub <pattern>\n");
        printf("  Example: sub weather.#\n");
        return;
    }

    if (!loopyValidatePattern(G.pubsub, pattern)) {
        printf("Error: Invalid pattern '%s'\n", pattern);
        return;
    }

    NetSub *ns = addNetSub(pattern, true);
    if (!ns) {
        printf("Error: Maximum subscriptions reached\n");
        return;
    }

    printf("Subscribed to: %s\n", pattern);

    /* Notify all peers */
    broadcastToPeers("SUB %s", pattern);
}

static void cmdUnsubscribe(const char *pattern) {
    if (!pattern || !*pattern) {
        printf("Usage: unsub <pattern>\n");
        return;
    }

    NetSub *ns = findNetSub(pattern);
    if (!ns || !ns->isLocal) {
        printf("Error: Not subscribed to '%s'\n", pattern);
        return;
    }

    removeNetSub(pattern, true);
    printf("Unsubscribed from: %s\n", pattern);

    /* Notify all peers */
    broadcastToPeers("UNSUB %s", pattern);
}

static void cmdPublish(const char *topic, const char *message) {
    if (!topic || !*topic) {
        printf("Usage: pub <topic> <message>\n");
        printf("  Example: pub weather.us.ca Sunny 72F\n");
        return;
    }

    if (!loopyValidateTopic(G.pubsub, topic)) {
        printf("Error: Invalid topic '%s' (wildcards not allowed)\n", topic);
        return;
    }

    const char *msg = message ? message : "";

    /* Deliver locally */
    size_t delivered = loopyPublish(G.pubsub, topic, msg, strlen(msg));

    /* Forward to network */
    broadcastToPeers("PUB %s %s", topic, msg);

    printf("Published to '%s': %s (delivered to %zu local)\n", topic, msg,
           delivered);
}

static void cmdPeers(void) {
    printf("\n");
    printf("Connected Peers (%d):\n", G.peerCount);
    printf("------------------------------------------------------------\n");
    if (G.peerCount == 0) {
        printf("  (none)\n");
    } else {
        printf(
            "  Name              Address              Sent     Recv   Dir\n");
        printf(
            "  ----------------  -------------------  -------  -----  ----\n");
        for (int i = 0; i < G.peerCount; i++) {
            PeerConn *p = &G.peers[i];
            printf("  %-16s  %-15s:%-4d  %-7" PRIu64 "  %-5" PRIu64 "  %s\n",
                   p->name, p->addr, p->port, p->msgSent, p->msgRecv,
                   p->isInbound ? "IN" : "OUT");
        }
    }
    printf("------------------------------------------------------------\n\n");
}

static void cmdSubs(void) {
    printf("\n");
    printf("Subscriptions (%d):\n", G.subCount);
    printf("------------------------------------------------------------\n");
    if (G.subCount == 0) {
        printf("  (none)\n");
    } else {
        printf("  Pattern                       Local  Peer Refs\n");
        printf("  ----------------------------  -----  ---------\n");
        for (int i = 0; i < G.subCount; i++) {
            printf("  %-28s  %-5s  %d\n", G.subs[i].pattern,
                   G.subs[i].isLocal ? "yes" : "no", G.subs[i].peerRefCount);
        }
    }
    printf("------------------------------------------------------------\n\n");
}

static void cmdStats(void) {
    loopyPubSubStats stats;
    loopyPubSubGetStats(G.pubsub, &stats);

    uint64_t totalSent = 0, totalRecv = 0;
    for (int i = 0; i < G.peerCount; i++) {
        totalSent += G.peers[i].msgSent;
        totalRecv += G.peers[i].msgRecv;
    }

    printf("\n");
    printf("Statistics:\n");
    printf("------------------------------------------------------------\n");
    printf("  Node name:           %s\n", G.myName);
    printf("  Listen address:      %s\n", G.myAddr);
    printf("  Connected peers:     %d\n", G.peerCount);
    printf("  Active subscriptions:%d\n", G.subCount);
    printf("  Auto-connect:        %s\n",
           G.autoConnect ? "enabled" : "disabled");
    printf("  ---\n");
    printf("  Local pub/sub:\n");
    printf("    Messages published:  %" PRIu64 "\n", stats.messagesPublished);
    printf("    Messages delivered:  %" PRIu64 "\n", stats.messagesDelivered);
    printf("  Network:\n");
    printf("    Messages sent:       %" PRIu64 "\n", totalSent);
    printf("    Messages received:   %" PRIu64 "\n", totalRecv);
    printf("------------------------------------------------------------\n\n");
}

/* Iterator callback for printing cluster nodes */
static int printClusterNodeIter(loopyClusterNode *node,
                                const loopyNodeInfo *info, void *userData) {
    (void)node;
    (void)userData;

    const char *stateStr = loopyNodeStateName(info->state);
    bool isLocal = (strcmp(info->nodeId, G.myName) == 0);
    bool isConnected = (findPeerByName(info->nodeId) != NULL);

    printf("  %-16s  %-24s  %-8s  %s\n", info->nodeId,
           info->address ? info->address : "(unknown)", stateStr,
           isLocal ? "(self)" : (isConnected ? "connected" : ""));
    return 0;
}

static void cmdCluster(void) {
    printf("\n");
    printf("Cluster Membership:\n");
    printf("------------------------------------------------------------\n");

    if (!G.cluster) {
        printf("  (cluster registry not initialized)\n");
    } else {
        size_t nodeCount = loopyClusterNodeCount(G.cluster);
        size_t activeCount = loopyClusterActiveNodeCount(G.cluster);

        printf("  Total nodes: %zu (active: %zu)\n", nodeCount, activeCount);
        printf("  Auto-connect: %s\n\n",
               G.autoConnect ? "enabled" : "disabled");

        if (nodeCount > 0) {
            printf("  Node             Address                   State     "
                   "Status\n");
            printf("  ---------------  ------------------------  --------  "
                   "--------\n");
            loopyClusterIterateNodes(G.cluster, printClusterNodeIter, NULL);
        }
    }
    printf("------------------------------------------------------------\n");
    printf("  Tip: Use --auto-connect to automatically connect to\n");
    printf("       discovered peers in the cluster.\n");
    printf("------------------------------------------------------------\n\n");
}

static void cmdHelp(void) {
    printf("\n");
    printf("=== NETWORKED PUB/SUB WITH CLUSTER DISCOVERY ===\n");
    printf("\n");
    printf("Network:\n");
    printf("  connect <host:port>   Connect to a peer\n");
    printf("  peers                 List connected peers\n");
    printf("  cluster               Show all discovered cluster members\n");
    printf("\n");
    printf("Pub/Sub:\n");
    printf("  sub <pattern>         Subscribe to pattern (e.g., weather.#)\n");
    printf("  unsub <pattern>       Unsubscribe from pattern\n");
    printf("  pub <topic> <msg>     Publish message to topic\n");
    printf("  subs                  List subscriptions\n");
    printf("\n");
    printf("Other:\n");
    printf("  stats                 Show statistics\n");
    printf("  help                  Show this help\n");
    printf("  quit                  Exit\n");
    printf("\n");
    printf("Wildcard Patterns:\n");
    printf("  *  matches exactly ONE segment\n");
    printf("  #  matches ZERO or more segments\n");
    printf("\n");
    printf("Cluster Discovery:\n");
    printf("  When connecting to any peer, you automatically learn about\n");
    printf("  all other members in the cluster. Use --auto-connect to\n");
    printf("  automatically connect to discovered peers.\n");
    printf("\n");
}

/* ============================================================================
 * Input Handling
 * ============================================================================
 */

static void processCommand(char *input) {
    while (*input && isspace(*input)) {
        input++;
    }
    if (!*input) {
        return;
    }

    char cmd[32] = {0};
    char arg1[256] = {0};
    char arg2[MAX_LINE] = {0};

    int n = sscanf(input, "%31s %255s %[^\n]", cmd, arg1, arg2);

    if (strcmp(cmd, "connect") == 0) {
        cmdConnect(arg1);
    } else if (strcmp(cmd, "sub") == 0) {
        cmdSubscribe(arg1);
    } else if (strcmp(cmd, "unsub") == 0) {
        cmdUnsubscribe(arg1);
    } else if (strcmp(cmd, "pub") == 0) {
        cmdPublish(arg1, n >= 3 ? arg2 : "");
    } else if (strcmp(cmd, "peers") == 0) {
        cmdPeers();
    } else if (strcmp(cmd, "cluster") == 0) {
        cmdCluster();
    } else if (strcmp(cmd, "subs") == 0) {
        cmdSubs();
    } else if (strcmp(cmd, "stats") == 0) {
        cmdStats();
    } else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        cmdHelp();
    } else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
        loopyStop(G.loop);
    } else {
        printf("Unknown command: %s (type 'help' for commands)\n", cmd);
    }
}

static void onStdinReady(loopyLoop *loop, int fd, void *userData,
                         loopyAction mask) {
    (void)loop;
    (void)fd;
    (void)userData;
    (void)mask;

    char input[MAX_LINE];
    if (fgets(input, sizeof(input), stdin) == NULL) {
        printf("\n");
        loopyStop(G.loop);
        return;
    }

    trimNewline(input);
    processCommand(input);

    if (!loopyIsStopped(G.loop)) {
        printf(PROMPT);
        fflush(stdout);
    }
}

static void onSignal(loopyLoop *loop, int signum, void *userData) {
    (void)signum;
    (void)userData;
    printf("\nReceived signal, shutting down...\n");
    loopyStop(loop);
}

static void onHeartbeat(loopyLoop *loop, loopyTimer *timer, void *userData) {
    (void)loop;
    (void)timer;
    (void)userData;

    /* Send PING to all connected peers */
    for (int i = 0; i < G.peerCount; i++) {
        if (G.peers[i].state == PEER_CONNECTED) {
            sendToPeer(&G.peers[i], "PING");
        }
    }
}

/* ============================================================================
 * Test Mode
 * ============================================================================
 */

static void testTimeout(loopyLoop *loop, loopyTimer *timer, void *userData) {
    (void)timer;
    (void)userData;
    printf("[TEST] Timeout\n");
    loopyStop(loop);
}

/* Test helper for pubsub_network */
#define NET_TEST(name, topic, msg, expected)                                   \
    do {                                                                       \
        G.testMsgCount = 0;                                                    \
        cmdPublish(topic, msg);                                                \
        if (G.testMsgCount == expected) {                                      \
            printf("  [PASS] %s\n", name);                                     \
            passed++;                                                          \
        } else {                                                               \
            printf("  [FAIL] %s - expected %d, got %d\n", name, expected,      \
                   G.testMsgCount);                                            \
            failed++;                                                          \
        }                                                                      \
    } while (0)

static void clearAllSubs(void) {
    for (int i = G.subCount - 1; i >= 0; i--) {
        if (G.subs[i].localSub) {
            loopyUnsubscribe(G.subs[i].localSub);
        }
    }
    G.subCount = 0;
}

static int runTestMode(void) {
    printf("[TEST] Comprehensive Networked Pub/Sub Pattern Tests\n");
    printf("=====================================================\n\n");

    G.testMode = true;
    G.testResult = 0;
    int passed = 0;
    int failed = 0;

    /* ================================================================
     * SECTION 1: Distributed Event Routing Patterns
     * ================================================================ */
    printf("=== SECTION 1: Distributed Event Routing ===\n\n");

    clearAllSubs();
    cmdSubscribe("cluster.*.node.*.event");
    cmdSubscribe("cluster.*.#");
    cmdSubscribe("cluster.#.heartbeat");
    cmdSubscribe("#");

    printf("Patterns: cluster.*.node.*.event, cluster.*.#, "
           "cluster.#.heartbeat, #\n\n");

    NET_TEST("Node event in cluster", "cluster.prod.node.web1.event", "started",
             3);
    NET_TEST("Deep cluster path", "cluster.prod.rack.42.node.db.metrics",
             "cpu=50", 2);
    NET_TEST("Cluster heartbeat", "cluster.prod.heartbeat", "alive", 3);
    NET_TEST("Deep heartbeat path", "cluster.staging.region.us.heartbeat", "ok",
             3);

    /* ================================================================
     * SECTION 2: Service Mesh / Microservices Patterns
     * ================================================================ */
    printf("\n=== SECTION 2: Service Mesh Patterns ===\n\n");

    clearAllSubs();
    cmdSubscribe("svc.*.rpc.*.request");
    cmdSubscribe("svc.*.rpc.*.response");
    cmdSubscribe("svc.gateway.#");
    cmdSubscribe("svc.*.health.#");
    cmdSubscribe("#.error");
    cmdSubscribe("#");

    printf("Patterns: svc.*.rpc.*.request/response, svc.gateway.#, "
           "svc.*.health.#, #.error\n\n");

    NET_TEST("RPC request", "svc.users.rpc.getUser.request", "{id:123}", 2);
    NET_TEST("RPC response", "svc.users.rpc.getUser.response", "{name:'bob'}",
             2);
    NET_TEST("Gateway routing", "svc.gateway.route.api.v2", "/users", 2);
    NET_TEST("Health check", "svc.payments.health.liveness", "ok", 2);
    NET_TEST("Service error", "svc.inventory.db.connection.error", "timeout",
             2);

    /* ================================================================
     * SECTION 3: IoT / Sensor Network Patterns
     * ================================================================ */
    printf("\n=== SECTION 3: IoT / Sensor Network ===\n\n");

    clearAllSubs();
    cmdSubscribe("sensor.*.*.temperature");
    cmdSubscribe("sensor.*.*.humidity");
    cmdSubscribe("sensor.building1.#");
    cmdSubscribe("sensor.#.alert");
    cmdSubscribe("actuator.*.*.command");
    cmdSubscribe("#");

    printf("Patterns: sensor.*.*.temp/humidity, sensor.building1.#, "
           "sensor.#.alert, actuator.*.*.command\n\n");

    NET_TEST("Temperature reading", "sensor.building1.floor3.temperature",
             "72.5", 3);
    NET_TEST("Humidity from building2", "sensor.building2.floor1.humidity",
             "45%", 2);
    NET_TEST("Building1 motion sensor", "sensor.building1.lobby.motion",
             "detected", 2);
    NET_TEST("Deep sensor alert", "sensor.warehouse.zone5.rack12.smoke.alert",
             "triggered", 2);
    NET_TEST("HVAC command", "actuator.building1.hvac.command", "cool", 2);

    /* ================================================================
     * SECTION 4: Financial / Trading Patterns
     * ================================================================ */
    printf("\n=== SECTION 4: Financial / Trading ===\n\n");

    clearAllSubs();
    cmdSubscribe("quote.*.*.bid");
    cmdSubscribe("quote.*.*.ask");
    cmdSubscribe("quote.nyse.#");
    cmdSubscribe("order.*.*.filled");
    cmdSubscribe("order.#.rejected");
    cmdSubscribe("risk.*.breach.#");
    cmdSubscribe("#");

    printf("Patterns: quote.*.*.bid/ask, quote.nyse.#, order patterns, "
           "risk.*.breach.#\n\n");

    NET_TEST("NYSE bid quote", "quote.nyse.AAPL.bid", "142.50", 3);
    NET_TEST("NASDAQ ask", "quote.nasdaq.GOOG.ask", "141.25", 2);
    NET_TEST("NYSE market data", "quote.nyse.market.summary", "up 0.5%", 2);
    NET_TEST("Order filled", "order.account1.12345.filled", "100 shares", 2);
    NET_TEST("Order rejected", "order.account2.12346.margin.rejected",
             "insufficient", 2);
    NET_TEST("Risk breach alert", "risk.portfolio1.breach.var.limit",
             "exceeded", 2);

    /* ================================================================
     * SECTION 5: Log Aggregation / Observability
     * ================================================================ */
    printf("\n=== SECTION 5: Log Aggregation / Observability ===\n\n");

    clearAllSubs();
    cmdSubscribe("log.*.error");
    cmdSubscribe("log.*.warn");
    cmdSubscribe("log.#.fatal");
    cmdSubscribe("log.*.*.#");
    cmdSubscribe("metric.*.*.p99");
    cmdSubscribe("trace.*.span.*.start");
    cmdSubscribe("#");

    printf("Patterns: log.*.error/warn, log.#.fatal, metric.*.*.p99, "
           "trace.*.span.*.start\n\n");

    NET_TEST("Service error log", "log.api.error", "connection refused", 3);
    NET_TEST("Nested fatal", "log.worker.job.cleanup.fatal", "OOM", 3);
    NET_TEST("Deep log path", "log.api.handler.user.create", "user created", 2);
    NET_TEST("P99 latency", "metric.api.latency.p99", "45ms", 2);
    NET_TEST("Trace span start", "trace.req123.span.db_query.start",
             "SELECT...", 2);

    /* ================================================================
     * SECTION 6: Complex Multi-Wildcard Patterns
     * ================================================================ */
    printf("\n=== SECTION 6: Complex Multi-Wildcard ===\n\n");

    clearAllSubs();
    cmdSubscribe("*.*.*.*.leaf");
    cmdSubscribe("#.middle.#");
    cmdSubscribe("a.*.b.*.c");
    cmdSubscribe("*.#.*");
    cmdSubscribe("#");

    printf("Patterns: *.*.*.*.leaf, #.middle.#, a.*.b.*.c, *.#.*\n\n");

    NET_TEST("Exact 4 segments to leaf", "a.b.c.d.leaf", "data",
             3); /* *.*.*.*.leaf, *.#.*, # */
    NET_TEST("5 segments (fails *.*.*.*.leaf)", "a.b.c.d.e.leaf", "data",
             2); /* *.#.*, # */
    NET_TEST("Middle at start", "middle.after", "data", 3);
    NET_TEST("Middle in middle", "before.middle.after", "data", 3);
    NET_TEST("Alternating pattern", "a.X.b.Y.c", "data", 3);
    NET_TEST("Deep path with star-hash-star", "x.a.b.c.d.e.f.y", "data", 2);

    /* ================================================================
     * SECTION 7: Edge Cases
     * ================================================================ */
    printf("\n=== SECTION 7: Edge Cases ===\n\n");

    clearAllSubs();
    cmdSubscribe("#");
    cmdSubscribe("*");
    cmdSubscribe("*.#");
    cmdSubscribe("exact.match");

    printf("Patterns: #, *, *.#, exact.match\n\n");

    NET_TEST("Single segment", "single", "data", 3);
    NET_TEST("Exact match", "exact.match", "data", 3);
    NET_TEST("Two segments", "two.segments", "data", 2);
    NET_TEST("Deep path (10 levels)", "a.b.c.d.e.f.g.h.i.j", "data", 2);

    /* ================================================================
     * SUMMARY
     * ================================================================ */
    printf("\n=====================================================\n");
    printf("[TEST] RESULTS: %d passed, %d failed\n", passed, failed);
    printf("=====================================================\n");

    if (failed == 0) {
        printf("\nNetworked pub/sub pattern matching: ALL TESTS PASSED\n");
        printf("Validated patterns for:\n");
        printf("  - Distributed cluster events\n");
        printf("  - Service mesh / microservices\n");
        printf("  - IoT sensor networks\n");
        printf("  - Financial trading systems\n");
        printf("  - Log aggregation / observability\n");
        printf("  - Complex multi-wildcard combinations\n");
        printf("  - Edge cases and boundaries\n");
    }

    return failed > 0 ? 1 : 0;
}

/* ============================================================================
 * Main
 * ============================================================================
 */

int main(int argc, char **argv) {
    /* Defaults */
    G.myPort = DEFAULT_PORT;
    snprintf(G.myName, sizeof(G.myName), "node%d", getpid() % 10000);

    /* Track connections to make */
    char *connectTo[MAX_PEERS] = {0};
    int connectCount = 0;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--test") == 0) {
            G.testMode = true;
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            G.myPort = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            strncpy(G.myName, argv[++i], sizeof(G.myName) - 1);
        } else if (strcmp(argv[i], "--connect") == 0 && i + 1 < argc) {
            if (connectCount < MAX_PEERS) {
                connectTo[connectCount++] = argv[++i];
            }
        } else if (strcmp(argv[i], "--auto-connect") == 0) {
            G.autoConnect = true;
        } else if (strcmp(argv[i], "--bind") == 0 && i + 1 < argc) {
            /* Explicit bind address for cluster announcements */
            strncpy(G.myAddr, argv[++i], sizeof(G.myAddr) - 1);
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [options]\n", argv[0]);
            printf("\n");
            printf("Options:\n");
            printf("  --port <port>        Listen port (default: %d)\n",
                   DEFAULT_PORT);
            printf("  --name <name>        Node name (default: auto)\n");
            printf("  --bind <addr>        Bind/announce address (default: "
                   "0.0.0.0)\n");
            printf("  --connect <h:p>      Connect to peer on startup\n");
            printf("  --auto-connect       Auto-connect to discovered cluster "
                   "peers\n");
            printf("  --test               Run automated tests\n");
            printf("  --help               Show this help\n");
            printf("\n");
            printf("Address formats (IPv4 and IPv6 supported):\n");
            printf("  IPv4:  192.168.1.1:9000\n");
            printf("  IPv6:  [::1]:9000 or [2001:db8::1]:9000\n");
            printf("\n");
            printf("Example:\n");
            printf("  %s --port 9000 --name peer1\n", argv[0]);
            printf("  %s --port 9001 --name peer2 --connect localhost:9000\n",
                   argv[0]);
            printf("  %s --port 9002 --auto-connect --connect localhost:9000\n",
                   argv[0]);
            return 0;
        }
    }

    /* Set default listen address if not specified */
    if (!G.myAddr[0]) {
        loopyNetFormatAddr(G.myAddr, sizeof(G.myAddr), "0.0.0.0", G.myPort);
    }

    /* Create event loop */
    G.loop = loopyNew(1024);
    if (!G.loop) {
        fprintf(stderr, "Failed to create event loop\n");
        return 1;
    }

    /* Signal handler */
    G.sigHandler = loopySignalNew(G.loop);
    if (G.sigHandler) {
        loopySignalRegister(G.sigHandler, SIGINT, onSignal, NULL);
    }

    /* Create pub/sub */
    loopyPubSubConfig psConfig;
    loopyPubSubConfigInit(&psConfig);
    psConfig.enableStats = true;

    G.pubsub = loopyPubSubNew(G.loop, &psConfig);
    if (!G.pubsub) {
        fprintf(stderr, "Failed to create pub/sub\n");
        goto cleanup;
    }

    /* Create cluster registry for node discovery */
    loopyClusterConfig clusterConfig;
    loopyClusterConfigInit(&clusterConfig);
    clusterConfig.localNodeId = G.myName;
    clusterConfig.localAddress = G.myAddr;
    clusterConfig.localRoles = LOOPY_ROLE_WORKER;

    G.cluster = loopyClusterRegistryNew(&clusterConfig);
    if (!G.cluster) {
        fprintf(stderr, "Warning: Failed to create cluster registry\n");
        /* Continue without cluster support */
    }

    /* Test mode */
    if (G.testMode) {
        int result = runTestMode();
        if (G.cluster) {
            loopyClusterRegistryFree(G.cluster);
        }
        loopyPubSubFree(G.pubsub);
        if (G.sigHandler) {
            loopySignalFree(G.sigHandler);
        }
        loopyDelete(G.loop);
        return result;
    }

    /* Create server */
    G.server = loopyStreamNewTcp(G.loop);
    if (!G.server) {
        fprintf(stderr, "Failed to create server\n");
        goto cleanup;
    }

    if (!loopyStreamBind(G.server, "0.0.0.0", G.myPort)) {
        fprintf(stderr, "Failed to bind to port %d\n", G.myPort);
        goto cleanup;
    }

    if (!loopyStreamListen(G.server, 128, onNewConnection, NULL)) {
        fprintf(stderr, "Failed to listen\n");
        goto cleanup;
    }

    /* Print banner */
    printf("\n");
    printf(
        "================================================================\n");
    printf("  LOOPY NETWORKED PUB/SUB WITH CLUSTER DISCOVERY\n");
    printf(
        "================================================================\n");
    printf("\n");
    printf("  Node:         %s\n", G.myName);
    printf("  Address:      %s\n", G.myAddr);
    printf("  Auto-connect: %s\n", G.autoConnect ? "enabled" : "disabled");
    printf("  Backend:      %s\n", loopyAdapterName());
    printf("\n");
    printf("  Type 'help' for commands, 'quit' to exit.\n");
    printf("  Connect other nodes with: --connect localhost:%d\n", G.myPort);
    if (!G.autoConnect) {
        printf("  Use --auto-connect for automatic cluster discovery.\n");
    }
    printf("\n");

    /* Connect to initial peers */
    for (int i = 0; i < connectCount; i++) {
        cmdConnect(connectTo[i]);
    }

    /* Start heartbeat timer */
    G.heartbeat =
        loopyTimerPeriodicMs(G.loop, HEARTBEAT_INTERVAL_MS, onHeartbeat, NULL);

    /* Register stdin */
    if (!loopyRegisterRead(G.loop, STDIN_FILENO, onStdinReady, NULL)) {
        fprintf(stderr, "Failed to register stdin\n");
        goto cleanup;
    }

    printf(PROMPT);
    fflush(stdout);

    /* Run */
    loopyMain(G.loop);

cleanup:
    printf("\nShutting down...\n");

    /* Close peer connections */
    for (int i = 0; i < G.peerCount; i++) {
        if (G.peers[i].stream) {
            loopyStreamClose(G.peers[i].stream, NULL, NULL);
        }
    }

    /* Cleanup subscriptions */
    for (int i = 0; i < G.subCount; i++) {
        if (G.subs[i].localSub) {
            loopyUnsubscribe(G.subs[i].localSub);
        }
    }

    if (G.heartbeat) {
        loopyTimerCancel(G.heartbeat);
    }
    if (G.server) {
        loopyStreamClose(G.server, NULL, NULL);
    }
    if (G.cluster) {
        loopyClusterRegistryFree(G.cluster);
    }
    if (G.pubsub) {
        loopyPubSubFree(G.pubsub);
    }
    if (G.sigHandler) {
        loopySignalFree(G.sigHandler);
    }
    loopyDelete(G.loop);

    printf("Goodbye!\n");
    return G.testResult;
}
