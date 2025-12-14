# Networking with Loopy

Loopy provides a comprehensive suite of networking primitives for building high-performance event-driven network applications. This guide covers TCP/UDP communication, DNS resolution, TLS encryption, and connection pooling.

## Table of Contents

1. [TCP Client/Server Patterns](#tcp-clientserver-patterns)
2. [UDP Patterns](#udp-patterns)
3. [DNS Resolution](#dns-resolution)
4. [TLS/SSL](#tlsssl)
5. [Connection Pooling](#connection-pooling)
6. [Stream Abstraction](#stream-abstraction)
7. [Advanced Topics](#advanced-topics)

---

## TCP Client/Server Patterns

Loopy provides two levels of TCP networking APIs:

- **Low-level**: `loopyNet` - raw socket operations with error message propagation
- **High-level**: `loopyStream` - buffered, event-driven I/O abstraction

### Low-Level: loopyNet

`loopyNet` provides synchronous socket setup functions with non-blocking support for integration with the event loop.

#### Server Setup Pattern

Create a listening socket using `loopyNet`:

```c
#include "loopyNet.h"

loopyNet server = {0};

// Create and bind IPv4 TCP server
if (!loopyNetTcp4Server(&server, 8080, "0.0.0.0", 128)) {
    fprintf(stderr, "Server setup failed: %s\n", loopyNetGetError(&server));
    return false;
}

// Enable non-blocking mode
if (!loopyNetNonBlockEnable(&server)) {
    fprintf(stderr, "Failed to enable non-blocking: %s\n", loopyNetGetError(&server));
    return false;
}

// Server socket is now in loopyNetSockGet(&server)
int serverFd = loopyNetSockGet(&server);
```

For IPv6:

```c
loopyNet server = {0};

if (!loopyNetTcp6Server(&server, 8080, "::", 128)) {
    fprintf(stderr, "Server setup failed: %s\n", loopyNetGetError(&server));
    return false;
}
```

#### Accept Pattern

Accept incoming connections:

```c
loopyNet server = {0};
char clientIp[INET6_ADDRSTRLEN];
int clientPort;
int clientFd;

// Wait for a connection
if (!loopyNetTcpAcceptNonBlock(&server, clientIp, sizeof(clientIp),
                               &clientPort, &clientFd)) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // No connections waiting
        return;
    }
    fprintf(stderr, "Accept failed: %s\n", loopyNetGetError(&server));
    return;
}

printf("Accepted connection from %s:%d\n", clientIp, clientPort);
// Register clientFd with event loop for reading/writing
```

#### Client Connection Pattern

Connect to a remote server:

```c
loopyNet client = {0};

// Initiate non-blocking connection
if (!loopyNetTcpNonBlockConnect(&client, "example.com", 8080)) {
    fprintf(stderr, "Connect failed: %s\n", loopyNetGetError(&client));
    return false;
}

int clientFd = loopyNetSockGet(&client);

// Register with event loop for write readiness
// When writable, check for connection success:
if (!loopyNetCheckSocketError(&client)) {
    fprintf(stderr, "Connection failed: %s\n", loopyNetGetError(&client));
    return false;
}
printf("Connected!\n");
```

#### Optional: Bind to Specific Interface

When connecting, you can bind to a specific local address:

```c
loopyNet client = {0};

// Connect while binding to 192.168.1.100
if (!loopyNetTcpNonBlockConnectBind(&client, "example.com", 8080,
                                     "192.168.1.100")) {
    fprintf(stderr, "Connect with bind failed: %s\n", loopyNetGetError(&client));
    return false;
}
```

Or use best-effort binding (falls back if binding fails):

```c
// Try to bind to 192.168.1.100, but proceed if it fails
loopyNetTcpNonBlockConnectBindOptional(&client, "example.com", 8080,
                                       "192.168.1.100");
```

### Socket Configuration

Configure socket behavior:

```c
loopyNet sock = {0};

// Enable TCP_NODELAY (disable Nagle's algorithm for low latency)
loopyNetTcpNoDelayEnable(&sock);

// Enable TCP keepalive with 30-second probes
loopyNetKeepAlive(&sock, 30);

// Set send timeout (5 seconds)
loopyNetSendTimeout(&sock, 5000);
```

### High-Level: loopyStream

`loopyStream` provides a buffered, event-driven abstraction that handles read/write callbacks automatically.

#### Stream Server

```c
#include "loopyStream.h"

// TCP server callback
void onNewConnection(loopyStream *server, int status, void *userData) {
    if (status != 0) {
        fprintf(stderr, "Accept failed: %d\n", status);
        return;
    }

    // Accept new connection
    loopyStream *client = loopyStreamAccept(server);
    if (!client) {
        fprintf(stderr, "Accept returned NULL\n");
        return;
    }

    printf("New client connected\n");

    // Start reading from client
    loopyStreamReadStart(client, allocCallback, readCallback, clientData);
}

// Bind and listen
loopyStream *server = loopyStreamNewTcp(loop);
if (!loopyStreamBind(server, "0.0.0.0", 8080)) {
    fprintf(stderr, "Bind failed\n");
    return false;
}

if (!loopyStreamListen(server, 128, onNewConnection, NULL)) {
    fprintf(stderr, "Listen failed\n");
    return false;
}
```

#### Stream Client

```c
void onConnectDone(loopyStream *stream, int status, void *userData) {
    if (status != 0) {
        fprintf(stderr, "Connect failed\n");
        return;
    }

    printf("Connected! Starting to read...\n");
    loopyStreamReadStart(stream, allocCallback, readCallback, userData);
}

loopyStream *client = loopyStreamNewTcp(loop);
if (!loopyStreamConnect(client, "example.com", 8080, onConnectDone, NULL)) {
    fprintf(stderr, "Connect initiation failed\n");
    return false;
}
```

#### Stream Reading

Implement allocation and read callbacks:

```c
void allocCallback(loopyStream *stream, size_t suggested,
                   void **buf, size_t *bufLen, void *userData) {
    // Allocate buffer for reading
    void *buffer = malloc(suggested);
    *buf = buffer;
    *bufLen = buffer ? suggested : 0;
}

void readCallback(loopyStream *stream, ssize_t nread,
                  const void *buf, void *userData) {
    if (nread > 0) {
        printf("Read %zd bytes\n", nread);
        // Process data
    } else if (nread == 0) {
        printf("EOF - peer closed connection\n");
        loopyStreamClose(stream, NULL, NULL);
    } else {
        printf("Read error\n");
    }
}

// Start reading
loopyStreamReadStart(client, allocCallback, readCallback, NULL);
```

#### Stream Writing

Write data asynchronously:

```c
void writeCallback(loopyStream *stream, int status, void *userData) {
    if (status == 0) {
        printf("Write completed\n");
    } else {
        printf("Write failed: %d\n", status);
    }
}

// Queue data for writing
loopyStreamWrite(stream, "Hello\n", 6, writeCallback, NULL);

// Check write queue depth for backpressure
size_t queueSize = loopyStreamGetWriteQueueSize(stream);
if (queueSize > 65536) {
    printf("Warning: write queue is %zu bytes\n", queueSize);
    // Consider pausing reads or applying backpressure
}
```

#### Try-Write (Synchronous)

Attempt immediate write without buffering:

```c
ssize_t written = loopyStreamTryWrite(stream, data, len);
if (written > 0) {
    printf("Wrote %zd bytes immediately\n", written);
} else if (written == -1 && errno == EAGAIN) {
    // Would block - use async write instead
    loopyStreamWrite(stream, data, len, writeCallback, NULL);
} else {
    printf("Write error\n");
}
```

---

## UDP Patterns

UDP is connectionless and ideal for:

- DNS queries
- Real-time applications (gaming, streaming)
- Bulk metrics collection
- Protocols that tolerate packet loss

### Basic UDP Operations

```c
#include "loopyUDP.h"

// Create UDP handle
loopyUDP *udp = loopyUDPNew(loop);

// Bind to receive on port 5353
if (!loopyUDPBind(udp, "0.0.0.0", 5353, LOOPY_UDP_REUSEADDR)) {
    fprintf(stderr, "Bind failed: %s\n", loopyUDPGetError(udp));
    return false;
}
```

### Receiving Datagrams

```c
void recvCallback(loopyUDP *udp, ssize_t nread,
                  const void *data, const struct sockaddr *addr,
                  socklen_t addrLen, void *userData) {
    if (nread > 0) {
        // Get sender address as string
        char addrStr[INET6_ADDRSTRLEN];
        int port = 0;

        if (addr->sa_family == AF_INET) {
            struct sockaddr_in *sin = (struct sockaddr_in *)addr;
            inet_ntop(AF_INET, &sin->sin_addr, addrStr, sizeof(addrStr));
            port = ntohs(sin->sin_port);
        }

        printf("Received %zd bytes from %s:%d\n", nread, addrStr, port);
    } else if (nread == 0) {
        printf("EOF\n");
    } else {
        printf("Error: %zd\n", nread);
    }
}

// Start receiving
loopyUDPRecvStart(udp, recvCallback, NULL);
```

### Sending Datagrams

```c
void sendCallback(loopyUDP *udp, int status, void *userData) {
    if (status == 0) {
        printf("Send completed\n");
    } else {
        printf("Send failed: %d\n", status);
    }
}

// Send to specific address
loopyUDPSend(udp, "192.168.1.100", 5353, data, len, sendCallback, NULL);

// Try immediate send (non-blocking)
ssize_t sent = loopyUDPTrySend(udp, "192.168.1.100", 5353, data, len);
if (sent > 0) {
    printf("Sent %zd bytes immediately\n", sent);
} else if (sent == -1 && errno == EAGAIN) {
    // Use async send
    loopyUDPSend(udp, "192.168.1.100", 5353, data, len, sendCallback, NULL);
}
```

### Connected UDP (for specific destination)

```c
// Connect to a specific peer
loopyUDPConnect(udp, "192.168.1.100", 5353);

// Send to connected peer (more efficient)
loopyUDPSendConnected(udp, data, len, sendCallback, NULL);

// Disconnect
loopyUDPDisconnect(udp);
```

### High-Performance Batch Operations

For high-throughput scenarios, use batch send/receive:

```c
loopyUDPMessage msgs[32];
char buffers[32][1500];
struct sockaddr_in addrs[32];

// Initialize message structures for receiving
for (int i = 0; i < 32; i++) {
    msgs[i].data = buffers[i];
    msgs[i].len = sizeof(buffers[i]);
    msgs[i].addrLen = sizeof(struct sockaddr_in);
}

// Receive up to 32 datagrams in one syscall
int n = loopyUDPRecvMulti(udp, msgs, 32);
for (int i = 0; i < n; i++) {
    printf("Received %zu bytes\n", msgs[i].bytesTransferred);
}

// Send multiple datagrams
for (int i = 0; i < 10; i++) {
    msgs[i].data = packets[i].data;
    msgs[i].len = packets[i].len;

    struct sockaddr_in *sin = (struct sockaddr_in *)&msgs[i].addr;
    sin->sin_family = AF_INET;
    sin->sin_port = htons(5353);
    inet_pton(AF_INET, "192.168.1.100", &sin->sin_addr);
    msgs[i].addrLen = sizeof(*sin);
}

int sent = loopyUDPSendMulti(udp, msgs, 10);
printf("Sent %d messages\n", sent);
```

### Advanced: UDP GSO/GRO (Linux 4.18+)

UDP Segmentation Offload allows the kernel to split large payloads:

```c
loopyUDPGSOConfig gso = {
    .segmentSize = 1472,  // IPv4 typical
    .enabled = true
};

if (loopyUDPSetGSO(udp, &gso)) {
    // Send 10KB - kernel splits into ~7 packets
    loopyUDPSendGSO(udp, "192.168.1.100", 5353, data, 10000, NULL, NULL);
}

// Generic Receive Offload - kernel coalesces small packets
loopyUDPGROConfig gro = {.enabled = true};
loopyUDPSetGRO(udp, &gro);
```

---

## DNS Resolution

Loopy provides asynchronous DNS resolution without blocking the event loop.

### Forward Lookup (hostname → IP)

```c
#include "loopyDNS.h"

void dnsCallback(loopyDNS *dns, const loopyDNSResult *result) {
    if (result->status != LOOPY_DNS_OK) {
        printf("DNS lookup failed: %s\n",
               loopyDNSStatusString(result->status));
        return;
    }

    // result->addresses contains resolved IPs
    for (size_t i = 0; i < result->addressCount; i++) {
        printf("Address %zu: %s\n", i, result->addresses[i].str);
    }

    free(result->hostname);
    free(result->addresses);
}

// Create DNS resolver
loopyDNSConfig config = LOOPY_DNS_CONFIG_DEFAULT;
config.maxConcurrent = 32;
config.timeoutMs = 10000;

loopyDNS *dns = loopyDNSNew(loop, &config);

// Query for both A and AAAA records
loopyDNSQueryId queryId = loopyDNSResolve(dns, "example.com",
                                          LOOPY_DNS_ANY,
                                          dnsCallback,
                                          NULL);

if (queryId == 0) {
    fprintf(stderr, "DNS query submission failed\n");
}
```

### Reverse Lookup (IP → hostname)

```c
void reverseDnsCallback(loopyDNS *dns, const loopyDNSReverseResult *result) {
    if (result->status != LOOPY_DNS_OK) {
        printf("Reverse DNS failed\n");
        return;
    }

    printf("Hostname for %s: %s\n", result->addrStr, result->hostname);
}

// Reverse lookup
loopyDNSReverseLookup(dns, "192.168.1.1", 0, reverseDnsCallback, NULL);
```

### Query Management

```c
// Check pending queries
size_t pending = loopyDNSPendingCount(dns);
printf("Pending DNS queries: %zu\n", pending);

// Cancel a specific query
loopyDNSCancel(dns, queryId);

// Cancel all pending queries
loopyDNSCancelAll(dns);

// Cleanup DNS resolver
loopyDNSFree(dns);
```

---

## TLS/SSL

Loopy uses mbedtls for secure encrypted communication with TLS 1.2/1.3 support.

### TLS Context (Shared Configuration)

Create a context before establishing connections:

```c
#include "loopyTLS.h"

// Client context
loopyTLSContextConfig clientCfg;
loopyTLSContextConfigInit(&clientCfg, LOOPY_TLS_CLIENT);
clientCfg.version = LOOPY_TLS_VERSION_AUTO;
clientCfg.verify = LOOPY_TLS_VERIFY_REQUIRED;
clientCfg.caFile = "/etc/ssl/certs/ca-certificates.crt";

loopyTLSContext *clientCtx = loopyTLSContextNew(&clientCfg);
if (!clientCtx) {
    fprintf(stderr, "Failed to create TLS context\n");
    return false;
}
```

### Server Context

```c
// Server context
loopyTLSContextConfig serverCfg;
loopyTLSContextConfigInit(&serverCfg, LOOPY_TLS_SERVER);
serverCfg.certFile = "/path/to/cert.pem";
serverCfg.keyFile = "/path/to/key.pem";

loopyTLSContext *serverCtx = loopyTLSContextNew(&serverCfg);

// Load additional CA certificates for mutual TLS
loopyTLSContextLoadCA(serverCtx, "/path/to/ca-cert.pem", NULL);
```

### TLS Client Connection

```c
void tlsHandshakeCallback(loopyTLS *tls, loopyTLSResult result,
                          void *userData) {
    if (result == LOOPY_TLS_OK) {
        printf("TLS handshake succeeded\n");

        loopyTLSInfo info;
        loopyTLSGetInfo(tls, &info);
        printf("Protocol: %s, Cipher: %s\n", info.version, info.ciphersuite);

        // Start application data transfer
    } else {
        printf("TLS handshake failed: %s\n", loopyTLSResultName(result));
    }
}

// Wrap raw socket with TLS
loopyTLS *tls = loopyTLSNew(loop, clientCtx, clientFd);

// Set SNI hostname (for virtual hosting)
loopyTLSSetHostname(tls, "example.com");

// Perform async handshake
loopyTLSHandshakeAsync(tls, tlsHandshakeCallback, NULL);
```

### TLS Server Connection

```c
// In accept callback for new clients
void onClientConnected(loopyStream *server, int status, void *userData) {
    loopyStream *client = loopyStreamAccept(server);
    int clientFd = loopyStreamGetFd(client);

    // Wrap with TLS
    loopyTLS *tls = loopyTLSNew(loop, serverCtx, clientFd);

    // Perform handshake
    loopyTLSHandshakeAsync(tls, tlsHandshakeCallback, client);
}
```

### Reading/Writing Encrypted Data

```c
// Read decrypted data
char buf[4096];
ssize_t nread = loopyTLSRead(tls, buf, sizeof(buf));
if (nread > 0) {
    printf("Read %zd bytes (decrypted)\n", nread);
} else if (nread == LOOPY_TLS_WANT_READ) {
    // Need more encrypted data from socket
} else if (nread < 0) {
    printf("TLS read failed\n");
}

// Write data (automatically encrypted)
ssize_t written = loopyTLSWrite(tls, "Hello secure world\n", 20);
printf("Wrote %zd bytes (will be encrypted)\n", written);

// Async read/write
void readDone(loopyTLS *tls, const void *data, ssize_t len, void *userData) {
    printf("Read %zd bytes asynchronously\n", len);
}

loopyTLSReadAsync(tls, buf, sizeof(buf), readDone, NULL);
```

### Certificate Verification

```c
// Check peer certificate
const void *peerCert = loopyTLSGetPeerCert(tls);
if (peerCert) {
    printf("Peer presented certificate\n");
}

// Get verification result
char verifyResult[256];
loopyTLSGetVerifyResult(tls, verifyResult, sizeof(verifyResult));
printf("Verification: %s\n", verifyResult);
```

### Error Handling

```c
if (!loopyTLSIsHandshakeDone(tls)) {
    int err = loopyTLSGetError(tls);
    char errMsg[256];
    loopyTLSGetErrorString(tls, errMsg, sizeof(errMsg));
    printf("TLS error %d: %s\n", err, errMsg);
}

// Graceful shutdown
loopyTLSResult result = loopyTLSClose(tls);
if (result == LOOPY_TLS_OK) {
    printf("Sent close_notify\n");
}

loopyTLSFree(tls);
loopyTLSContextFree(clientCtx);
```

---

## Connection Pooling

Connection pooling eliminates the overhead of repeatedly creating expensive resources like database connections.

### Why Use Connection Pooling?

Creating connections is expensive:

- **Database**: TCP handshake + authentication + session setup (5-20ms per connection)
- **TLS**: Handshake + certificate validation + key exchange (20-100ms)
- **File handles**: Kernel syscalls + permission checks

At 1000 req/sec, creating fresh connections would waste 5-20 cores on handshakes alone.

### Basic Pool Setup

```c
#include "loopyConnPool.h"

// Implement lifecycle callbacks
void *createConnection(void *userData) {
    // userData points to your config
    MyDBConfig *cfg = (MyDBConfig *)userData;

    MyConnection *conn = malloc(sizeof(*conn));
    if (db_connect(conn, cfg->host, cfg->port)) {
        return conn;
    }
    free(conn);
    return NULL;
}

void destroyConnection(void *conn, void *userData) {
    MyConnection *myConn = (MyConnection *)conn;
    db_disconnect(myConn);
    free(myConn);
}

bool validateConnection(void *conn, void *userData) {
    MyConnection *myConn = (MyConnection *)conn;
    // Lightweight health check
    return db_ping(myConn) == 0;
}

// Create pool
loopyConnPoolConfig cfg = loopyConnPoolConfigDefault();
cfg.minIdle = 5;      // Keep 5 connections warm
cfg.maxTotal = 20;    // Allow up to 20 total
cfg.idleTimeoutUs = 30000000;  // Close after 30s idle

MyDBConfig dbCfg = {.host = "localhost", .port = 5432};

loopyConnPool *pool = loopyConnPoolNew(loop, &cfg,
                                       createConnection,
                                       destroyConnection,
                                       validateConnection,
                                       &dbCfg);
```

### Acquiring Connections

```c
// Blocking acquire (fast if connection available)
loopyConnPoolConn *poolConn = loopyConnPoolAcquire(pool);
if (!poolConn) {
    fprintf(stderr, "Failed to acquire connection\n");
    return;
}

MyConnection *conn = (MyConnection *)loopyConnPoolGetUserConn(poolConn);

// Use connection
db_query(conn, "SELECT * FROM users");

// Return to pool (CRITICAL - don't leak!)
loopyConnPoolRelease(poolConn, true);  // true = healthy
```

### Async Acquisition

When the pool is at capacity, use async acquisition:

```c
void onConnectionReady(loopyConnPoolConn *poolConn, void *userData) {
    if (!poolConn) {
        fprintf(stderr, "Acquisition timeout\n");
        return;
    }

    MyConnection *conn = (MyConnection *)loopyConnPoolGetUserConn(poolConn);

    // Use connection...

    loopyConnPoolRelease(poolConn, true);
}

// Request connection asynchronously
loopyConnPoolAcquireAsync(pool, onConnectionReady, userData);
```

### Pool Monitoring

```c
loopyConnPoolStats stats;
loopyConnPoolGetStats(pool, &stats);

printf("Connections: %u idle, %u active, %u total\n",
       stats.idleConns, stats.activeConns, stats.totalConns);
printf("Lifetime stats: %lu acquires, %lu timeouts\n",
       stats.totalAcquires, stats.totalTimeouts);
```

### Pool Maintenance

```c
// Manually trigger health check on idle connections
uint32_t failed = loopyConnPoolHealthCheck(pool);
printf("Removed %u unhealthy connections\n", failed);

// Manually cleanup idle connections
uint32_t closed = loopyConnPoolCleanupIdle(pool);
printf("Closed %u idle connections\n", closed);

// Prewarm pool at startup
uint32_t created = loopyConnPoolPrewarm(pool, 10);
printf("Prewarmed with %u connections\n", created);
```

### Lifecycle Management

```c
// Set acquire/release callbacks for state management
void onAcquire(void *conn, void *userData) {
    MyConnection *c = (MyConnection *)conn;
    // Begin transaction, reset state, etc.
    db_begin_transaction(c);
}

void onRelease(void *conn, void *userData) {
    MyConnection *c = (MyConnection *)conn;
    // Rollback, clear temp state, etc.
    db_rollback(c);
}

loopyConnPoolSetAcquireCallback(pool, onAcquire);
loopyConnPoolSetReleaseCallback(pool, onRelease);

// Cleanup
loopyConnPoolFree(pool);
```

---

## Stream Abstraction

`loopyStream` provides unified buffered I/O over TCP, pipes, and other bidirectional channels.

### Stream Types

```c
// TCP stream
loopyStream *tcpStream = loopyStreamNewTcp(loop);

// Pipe stream (Unix domain sockets)
loopyStream *pipeStream = loopyStreamNewPipe(loop);

// Wrap existing file descriptor
loopyStream *stream = loopyStreamFromFd(loop, fd, LOOPY_STREAM_TCP);
```

### Addressing

```c
// Get local address
char localAddr[INET6_ADDRSTRLEN];
int localPort;
loopyStreamGetSockName(stream, localAddr, sizeof(localAddr), &localPort);
printf("Local: %s:%d\n", localAddr, localPort);

// Get peer address
char peerAddr[INET6_ADDRSTRLEN];
int peerPort;
loopyStreamGetPeerName(stream, peerAddr, sizeof(peerAddr), &peerPort);
printf("Peer: %s:%d\n", peerAddr, peerPort);
```

### User Data

```c
// Attach context to stream
struct ClientContext {
    uint64_t requestCount;
    time_t connectedAt;
};

struct ClientContext *ctx = malloc(sizeof(*ctx));
ctx->requestCount = 0;
ctx->connectedAt = time(NULL);

loopyStreamSetData(stream, ctx);

// Retrieve in callbacks
void readCallback(loopyStream *stream, ssize_t nread,
                  const void *buf, void *userData) {
    struct ClientContext *ctx = loopyStreamGetData(stream);
    ctx->requestCount++;
}
```

### Backpressure Handling

```c
void writeCallback(loopyStream *stream, int status, void *userData) {
    if (status != 0) {
        printf("Write failed\n");
    }

    // Check queue after write completes
    size_t queueSize = loopyStreamGetWriteQueueSize(stream);
    if (queueSize < 8192) {
        // Queue is small - safe to resume reading
        loopyStreamReadStart(stream, allocCb, readCb, userData);
    }
}

// In read callback
void readCallback(loopyStream *stream, ssize_t nread,
                  const void *buf, void *userData) {
    if (nread <= 0) return;

    // Write response
    loopyStreamWrite(stream, response, respLen, writeCallback, userData);

    // Check backpressure
    size_t queueSize = loopyStreamGetWriteQueueSize(stream);
    if (queueSize > 65536) {
        // Queue is full - pause reading
        loopyStreamReadStop(stream);
    }
}
```

### Graceful Shutdown

```c
void shutdownCallback(loopyStream *stream, int status, void *userData) {
    if (status == 0) {
        printf("Graceful shutdown complete\n");
    } else {
        printf("Shutdown failed\n");
    }

    loopyStreamClose(stream, NULL, NULL);
}

void closeCallback(loopyStream *stream, void *userData) {
    printf("Stream closed and freed\n");
    struct ClientContext *ctx = userData;
    free(ctx);
}

// Stop reading, drain writes, send FIN
loopyStreamShutdown(stream, shutdownCallback, userData);

// Or immediate close
loopyStreamClose(stream, closeCallback, userData);
```

### File Descriptor Passing (IPC)

For load balancing or privilege separation:

```c
// Server accepts connection and passes to worker
void onConnection(loopyStream *server, int status, void *userData) {
    loopyStream *client = loopyStreamAccept(server);
    int clientFd = loopyStreamGetFd(client);

    // Send to worker via pipe
    loopyStreamWriteWithFd(workerPipe, "c", 1, &clientFd, 1, NULL, NULL);

    // Stream still owns clientFd - close separately if needed
    close(clientFd);
}

// Worker receives connection
void readFdCallback(loopyStream *stream, ssize_t nread,
                    const void *buf, const int *fds, int nfds,
                    void *userData) {
    if (nfds > 0) {
        int clientFd = fds[0];
        printf("Received fd %d from parent\n", clientFd);

        // Create stream and handle connection
        loopyStream *client = loopyStreamFromFd(loop, clientFd, LOOPY_STREAM_TCP);
    }
}

// Start reading with FD capability
loopyStreamReadStartWithFd(workerPipe, allocCb, readFdCallback, NULL);
```

---

## Advanced Topics

### Socket Options

```c
loopyNet sock = {0};

// Enable SO_REUSEPORT for kernel load balancing (Linux 3.9+)
loopyNetReusePortEnable(&sock);

// Reuse address to bind quickly after close
loopyNetSetReuseAddr(&sock);

// Set send buffer
loopyNetSetSendBuffer(&sock, 262144);  // 256KB
```

### Address Resolution with Extra Data

```c
loopyNetResolveData data;
if (!loopyNetResolveExtra(&net, "example.com", &data)) {
    fprintf(stderr, "Resolve failed\n");
    return;
}

printf("IP: %s\n", data.ip);
printf("Family: %s\n", data.af == AF_INET ? "IPv4" : "IPv6");

if (data.af == AF_INET) {
    struct sockaddr_in *sa = &data.sa;
    printf("Port: %d\n", ntohs(sa->sin_port));
}
```

### Unix Domain Sockets

```c
loopyNet server = {0};

// Create Unix domain server
if (!loopyNetUnixServer(&server, "/tmp/myapp.sock", 0660, 128)) {
    fprintf(stderr, "Unix server failed\n");
    return false;
}

// Client connect to Unix domain
loopyNet client = {0};
if (!loopyNetUnixNonBlockConnect(&client, "/tmp/myapp.sock")) {
    fprintf(stderr, "Unix connect failed\n");
    return false;
}
```

### Error Handling

All networking operations populate an error string when they fail:

```c
loopyNet net = {0};

if (!loopyNetTcpConnect(&net, "example.com", 8080)) {
    // Get detailed error message
    const char *err = loopyNetGetError(&net);
    fprintf(stderr, "Connect failed: %s\n", err);

    // Clear error for reuse
    loopyNetClearError(&net);
}
```

### Address Parsing and Formatting (IPv4/IPv6)

Loopy provides robust utilities for handling both IPv4 and IPv6 addresses consistently across the platform. Use these functions instead of manual string parsing.

#### Supported Address Formats

| Format                | Example            | Description               |
| --------------------- | ------------------ | ------------------------- |
| IPv4 with port        | `192.168.1.1:9000` | Standard IPv4             |
| IPv4 without port     | `192.168.1.1`      | Uses default port         |
| IPv6 with port        | `[::1]:9000`       | RFC 3986 bracket notation |
| IPv6 without port     | `[2001:db8::1]`    | Bracketed, uses default   |
| Bare IPv6             | `::1`              | No brackets, no port      |
| Hostname with port    | `localhost:9000`   | DNS name                  |
| Hostname without port | `example.com`      | Uses default port         |

#### Parsing Addresses

```c
#include "loopyNet.h"

char host[256];
int port;

// Parse address into host and port components
// Default port is used if not specified in the address string
if (!loopyNetParseAddr("[::1]:9000", host, sizeof(host), &port, 8080)) {
    fprintf(stderr, "Invalid address format\n");
    return false;
}
// host = "::1", port = 9000

// Parse IPv4
loopyNetParseAddr("192.168.1.1:8080", host, sizeof(host), &port, 9000);
// host = "192.168.1.1", port = 8080

// Parse hostname without port (uses default)
loopyNetParseAddr("localhost", host, sizeof(host), &port, 9000);
// host = "localhost", port = 9000

// Parse bare IPv6 (no brackets, no port extractable)
loopyNetParseAddr("2001:db8::1", host, sizeof(host), &port, 9000);
// host = "2001:db8::1", port = 9000 (default)
```

#### Formatting Addresses

```c
char addrBuf[256];

// Format IPv4 address
loopyNetFormatAddr(addrBuf, sizeof(addrBuf), "192.168.1.1", 8080);
printf("%s\n", addrBuf);  // "192.168.1.1:8080"

// Format IPv6 address - brackets added automatically
loopyNetFormatAddr(addrBuf, sizeof(addrBuf), "::1", 9000);
printf("%s\n", addrBuf);  // "[::1]:9000"

loopyNetFormatAddr(addrBuf, sizeof(addrBuf), "2001:db8::1", 443);
printf("%s\n", addrBuf);  // "[2001:db8::1]:443"
```

#### Checking Address Type

```c
// Check if an address string appears to be IPv6
bool isIPv6 = loopyNetAddrIsIPv6("::1");        // true
bool isIPv4 = loopyNetAddrIsIPv6("192.168.1.1"); // false
```

#### Extracting Address from Socket

```c
// Get peer address from socket
loopyNetFormatPeer(clientFd, addrBuf, sizeof(addrBuf));
printf("Peer: %s\n", addrBuf);

// Get local address from socket
loopyNetFormatSock(clientFd, addrBuf, sizeof(addrBuf));
printf("Local: %s\n", addrBuf);
```

---

## Best Practices

### Connection Management

1. **Always release connections to pools** - prevent resource exhaustion
2. **Validate connections** - health checks catch stale connections
3. **Set appropriate timeouts** - prevent hanging connections
4. **Monitor pool statistics** - tune configuration based on real usage
5. **Prewarm pools at startup** - avoid latency spikes on first requests

### TCP/Stream I/O

1. **Handle backpressure** - pause reads when write queue grows
2. **Use try-write for low latency** - avoid buffering when possible
3. **Implement graceful shutdown** - send FIN before close
4. **Track connection lifecycle** - use user data for context
5. **Check error strings** - helps debugging in production

### Error Recovery

1. **Validate DNS results** - empty results can occur
2. **Retry with exponential backoff** - for transient failures
3. **Check socket errors** - especially after async operations
4. **Handle EAGAIN/EWOULDBLOCK** - normal in non-blocking I/O
5. **Monitor connection pool timeouts** - indicates load issues

### Performance

1. **Use UDP batch operations** - 10-50x throughput improvement
2. **Enable TCP_NODELAY** - for low-latency applications
3. **Configure keepalive** - detect dead connections early
4. **Batch DNS queries** - use connection pooling style patterns
5. **Profile with real workloads** - configuration is workload-dependent
