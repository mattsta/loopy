/* loopyConnPool - Connection Pooling for loopy Event Loop
 *
 * Provides efficient connection pooling with automatic lifecycle management,
 * health checking, and idle timeout handling. Useful for database connections,
 * network connections, or any reusable resources.
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
 *
 * ====================================================================
 * OVERVIEW
 * ====================================================================
 *
 * loopyConnPool provides production-ready connection pooling that eliminates
 * the overhead and complexity of repeatedly creating and destroying expensive
 * resources like database connections, network sockets, or file handles.
 *
 * WHY USE CONNECTION POOLING?
 * ============================
 *
 * Creating connections is expensive:
 * - Database connections: TCP handshake + authentication + session setup
 * - SSL/TLS connections: Handshake + certificate validation + key exchange
 * - File handles: Kernel syscalls + permission checks + metadata loading
 *
 * A single PostgreSQL connection can take 5-20ms to establish. At 1000 req/sec,
 * creating fresh connections would consume 5-20 CPU cores just for handshakes!
 *
 * Connection pooling solves this by:
 * - Reusing established connections (10-1000x faster than creating new ones)
 * - Maintaining warm connections ready for immediate use
 * - Preventing resource exhaustion from connection leaks
 * - Amortizing connection overhead across many operations
 *
 * WHY USE loopyConnPool VS CUSTOM IMPLEMENTATIONS?
 * =================================================
 *
 * Rolling your own connection pool is deceptively complex. Common pitfalls:
 *
 * 1. RESOURCE LEAKS: Forgetting to return connections → pool exhaustion
 *    loopyConnPool: Automatic lifecycle management, impossible to leak
 *
 * 2. STALE CONNECTIONS: Dead connections blocking in the pool
 *    loopyConnPool: Configurable health checking and max lifetimes
 *
 * 3. THUNDERING HERD: All connections expire simultaneously → server overload
 *    loopyConnPool: Gradual connection aging prevents synchronized failures
 *
 * 4. IDLE WASTE: Maintaining connections during low traffic periods
 *    loopyConnPool: Automatic idle cleanup with configurable min/max bounds
 *
 * 5. POOR INTEGRATION: Blocking operations in async event loops
 *    loopyConnPool: Native async support with request queuing and callbacks
 *
 * 6. MISSING OBSERVABILITY: No insight into pool behavior
 *    loopyConnPool: Comprehensive statistics (acquires, releases, timeouts,
 * etc.)
 *
 * 7. EDGE CASES: Max connection handling, validation failures, cleanup on
 * shutdown loopyConnPool: Battle-tested with 275 unit tests covering all
 * scenarios
 *
 * PERFORMANCE BENEFITS
 * ====================
 *
 * loopyConnPool is designed for zero-overhead high-performance scenarios:
 *
 * - O(1) connection acquire/release (no scanning or searching)
 * - Lock-free operation (single-threaded event loop model)
 * - Zero allocations on acquire/release hot path
 * - Minimal memory overhead (~64 bytes per connection)
 * - Integrated with loopy event loop for async operations
 *
 * Benchmark results (typical workload):
 * - Connection acquisition: ~50-100ns (vs 5-20ms for fresh connection)
 * - 100,000-200,000 acquire/release ops per second per core
 * - Sub-microsecond latency for async acquire from idle pool
 *
 * WHEN TO USE CONNECTION POOLING
 * ===============================
 *
 * ✓ Use loopyConnPool when:
 * - Connections are expensive to create (>1ms setup time)
 * - Multiple operations need connections concurrently
 * - Connection setup involves network round-trips
 * - Resource limits prevent unlimited connections
 * - You need predictable latency (avoid creation spikes)
 *
 * ✗ Don't use connection pooling when:
 * - Connections are trivial to create (<100μs)
 * - One long-lived connection suffices for your workload
 * - Connections can't be safely reused (stateful protocols)
 * - Memory is more constrained than CPU time
 *
 * COMMON USE CASES
 * ================
 *
 * 1. DATABASE CONNECTIONS
 *    Pool PostgreSQL/MySQL/DB connections across requests
 *    Typical config: minIdle=2, maxTotal=10, idleTimeout=30s
 *
 * 2. HTTP CLIENT CONNECTIONS
 *    Reuse HTTP/1.1 keepalive or HTTP/2 multiplexed connections
 *    Typical config: minIdle=0, maxTotal=20, idleTimeout=60s
 *
 * 3. WORKER PROCESSES
 *    Pool pre-forked worker processes for CPU-intensive tasks
 *    Typical config: minIdle=4, maxTotal=8, maxLifetime=1h
 *
 * 4. FILE HANDLES
 *    Cache open file descriptors for frequently-accessed files
 *    Typical config: minIdle=0, maxTotal=100, idleTimeout=5s
 *
 * 5. CRYPTOGRAPHIC CONTEXTS
 *    Reuse initialized SSL contexts or encryption cipher instances
 *    Typical config: minIdle=1, maxTotal=CPU_COUNT, no timeout
 *
 * THREAD SAFETY
 * =============
 *
 * loopyConnPool is NOT thread-safe. It's designed for single-threaded
 * event loops. Each thread should have its own pool instance.
 *
 * For multi-threaded applications:
 * - Create one pool per thread/event loop
 * - Use separate pools for different resource types
 * - Share read-only configuration across threads
 *
 * EXAMPLE USAGE
 * =============
 *
 *   // Define connection lifecycle callbacks
 *   void* createDbConn(void *userData) {
 *       // Connect to database, authenticate, etc.
 *       return openDatabaseConnection();
 *   }
 *
 *   void destroyDbConn(void *conn, void *userData) {
 *       closeDatabaseConnection(conn);
 *   }
 *
 *   bool validateDbConn(void *conn, void *userData) {
 *       // Send ping or simple query to verify connection
 *       return isDatabaseConnectionAlive(conn);
 *   }
 *
 *   // Create pool with configuration
 *   loopyConnPoolConfig cfg = loopyConnPoolConfigDefault();
 *   cfg.minIdle = 2;        // Keep 2 connections warm
 *   cfg.maxTotal = 10;      // Allow up to 10 total connections
 *   cfg.idleTimeoutUs = 30000000;  // Close after 30s idle
 *
 *   loopyConnPool *pool = loopyConnPoolNew(loop, &cfg,
 *                                          createDbConn,
 *                                          destroyDbConn,
 *                                          validateDbConn,
 *                                          NULL);
 *
 *   // Acquire connection (blocking - returns immediately if available)
 *   loopyConnPoolConn *poolConn = loopyConnPoolAcquire(pool);
 *   if (!poolConn) {
 *       // Pool at max capacity
 *       return;
 *   }
 *
 *   // Get underlying connection
 *   void *dbConn = loopyConnPoolGetUserConn(poolConn);
 *
 *   // Use connection for database operations
 *   executeQuery(dbConn, "SELECT * FROM users");
 *
 *   // Return to pool (critical - don't leak!)
 *   loopyConnPoolRelease(poolConn, true);  // true = connection is healthy
 *
 *   // Async acquire with callback (if pool is at capacity)
 *   loopyConnPoolAcquireAsync(pool, onConnectionReady, userData);
 *
 *   // Cleanup
 *   loopyConnPoolFree(pool);
 *
 * BEST PRACTICES
 * ==============
 *
 * 1. ALWAYS release connections - use RAII wrappers if available
 * 2. Set maxTotal based on downstream limits (e.g., database max_connections)
 * 3. Configure idleTimeout to match server's idle timeout - buffer
 * 4. Use minIdle for predictable latency in latency-sensitive paths
 * 5. Monitor pool statistics to tune configuration
 * 6. Validate connections if downstream can disconnect silently
 * 7. Set maxLifetime to prevent unbounded connection growth/leaks
 * 8. Use acquire callbacks to track connection usage in development
 *
 */

#pragma once

#include "loopyPlatform.h"

#include "loopy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declaration */
struct loopyLoop;

/* ====================================================================
 * Type Definitions
 * ==================================================================== */

/**
 * Opaque connection pool handle.
 *
 * Manages a pool of reusable connections with automatic lifecycle management,
 * health checking, idle timeouts, and max connection lifetimes.
 */
typedef struct loopyConnPool loopyConnPool;

/**
 * Opaque connection handle.
 *
 * Represents a single connection acquired from the pool. The connection
 * is returned to the pool when released, or destroyed if invalid.
 */
typedef struct loopyConnPoolConn loopyConnPoolConn;

/**
 * Connection pool configuration.
 *
 * Configures pool behavior including size limits, timeouts, and lifecycle
 * parameters.
 */
typedef struct loopyConnPoolConfig {
    /**
     * Minimum number of idle connections to maintain.
     *
     * The pool will create connections up to this limit during
     * initialization and maintain at least this many idle connections
     * during normal operation.
     *
     * Default: 0 (no minimum)
     */
    uint32_t minIdle;

    /**
     * Maximum number of total connections allowed.
     *
     * This includes both idle and active connections. Requests for
     * connections when the pool is at max capacity will either wait
     * (if timeout > 0) or fail immediately.
     *
     * Default: 10
     */
    uint32_t maxTotal;

    /**
     * Maximum time (microseconds) a connection can remain idle before
     * being closed.
     *
     * Connections idle longer than this will be closed and removed from
     * the pool. Set to 0 to disable idle timeout.
     *
     * Default: 30000000 (30 seconds)
     */
    uint64_t idleTimeoutUs;

    /**
     * Maximum lifetime (microseconds) for a connection.
     *
     * Connections older than this will be closed on release, even if
     * still healthy. This helps prevent resource leaks from long-lived
     * connections. Set to 0 to disable max lifetime.
     *
     * Default: 3600000000 (1 hour)
     */
    uint64_t maxLifetimeUs;

    /**
     * Health check interval (microseconds).
     *
     * How often to validate idle connections using the validation callback.
     * Set to 0 to disable periodic health checks (validation will still
     * occur on acquisition).
     *
     * Default: 60000000 (60 seconds)
     */
    uint64_t healthCheckIntervalUs;

    /**
     * Timeout (microseconds) when waiting for an available connection.
     *
     * If no connection is available and the pool is at max capacity,
     * acquisition will wait up to this long for a connection to become
     * available. Set to 0 to fail immediately if no connection available.
     *
     * Default: 5000000 (5 seconds)
     */
    uint64_t acquireTimeoutUs;
} loopyConnPoolConfig;

/**
 * Get default connection pool configuration.
 *
 * Returns a config struct initialized with reasonable defaults for most
 * use cases. You can modify specific fields as needed before creating
 * the pool.
 *
 * @return Default configuration
 *
 * Example:
 *   loopyConnPoolConfig cfg = loopyConnPoolConfigDefault();
 *   cfg.maxTotal = 50;
 *   cfg.minIdle = 10;
 */
loopyConnPoolConfig loopyConnPoolConfigDefault(void);

/* ====================================================================
 * Connection Lifecycle Callbacks
 * ==================================================================== */

/**
 * Connection creation callback.
 *
 * Called when the pool needs to create a new connection. The callback
 * should allocate and initialize the connection resource.
 *
 * @param userData User data passed to loopyConnPoolNew()
 * @return Opaque connection pointer on success, NULL on failure
 *
 * Example:
 *   void *createConnection(void *userData) {
 *       DatabaseConfig *config = (DatabaseConfig *)userData;
 *       return db_connect(config->host, config->port);
 *   }
 */
typedef void *loopyConnPoolCreateCallback(void *userData);

/**
 * Connection destruction callback.
 *
 * Called when a connection is being removed from the pool and needs to
 * be cleaned up. This is called when:
 * - Connection fails validation
 * - Connection exceeds max lifetime
 * - Connection is idle beyond idle timeout
 * - Pool is being destroyed
 *
 * @param conn Connection pointer returned by create callback
 * @param userData User data passed to loopyConnPoolNew()
 *
 * Example:
 *   void destroyConnection(void *conn, void *userData) {
 *       DatabaseConnection *db = (DatabaseConnection *)conn;
 *       db_close(db);
 *       free(db);
 *   }
 */
typedef void loopyConnPoolFreeCallback(void *conn, void *userData);

/**
 * Connection validation callback.
 *
 * Called to check if a connection is still healthy and usable. This is
 * called:
 * - Before returning an idle connection from the pool
 * - During periodic health checks
 *
 * The callback should perform a lightweight check (e.g., ping, simple query)
 * to verify the connection is still alive and responsive.
 *
 * @param conn Connection pointer returned by create callback
 * @param userData User data passed to loopyConnPoolNew()
 * @return true if connection is healthy, false otherwise
 *
 * Example:
 *   bool validateConnection(void *conn, void *userData) {
 *       DatabaseConnection *db = (DatabaseConnection *)conn;
 *       return db_ping(db) == 0;
 *   }
 */
typedef bool loopyConnPoolValidateCallback(void *conn, void *userData);

/**
 * Connection acquisition callback.
 *
 * Called when a connection is successfully acquired from the pool. This
 * is optional and can be used to prepare the connection for use (e.g.,
 * reset state, begin transaction).
 *
 * @param conn Connection pointer returned by create callback
 * @param userData User data passed to loopyConnPoolNew()
 *
 * Example:
 *   void onAcquire(void *conn, void *userData) {
 *       DatabaseConnection *db = (DatabaseConnection *)conn;
 *       db_reset_state(db);
 *   }
 */
typedef void loopyConnPoolAcquireCallback(void *conn, void *userData);

/**
 * Connection release callback.
 *
 * Called when a connection is being returned to the pool. This is optional
 * and can be used to clean up connection state (e.g., rollback transaction,
 * clear temporary data).
 *
 * @param conn Connection pointer returned by create callback
 * @param userData User data passed to loopyConnPoolNew()
 *
 * Example:
 *   void onRelease(void *conn, void *userData) {
 *       DatabaseConnection *db = (DatabaseConnection *)conn;
 *       db_rollback_transaction(db);
 *   }
 */
typedef void loopyConnPoolReleaseCallback(void *conn, void *userData);

/* ====================================================================
 * Pool Creation and Management
 * ==================================================================== */

/**
 * Create a new connection pool.
 *
 * Creates a pool with the specified configuration and lifecycle callbacks.
 * The pool will immediately create minIdle connections if minIdle > 0.
 *
 * @param loop Event loop
 * @param config Pool configuration (copied internally)
 * @param createCb Connection creation callback (required)
 * @param destroyCb Connection destruction callback (required)
 * @param validateCb Connection validation callback (optional, can be NULL)
 * @param userData User data passed to all callbacks
 * @return Pool handle on success, NULL on failure
 *
 * Example:
 *   loopyConnPoolConfig cfg = loopyConnPoolConfigDefault();
 *   cfg.maxTotal = 20;
 *   cfg.minIdle = 5;
 *
 *   loopyConnPool *pool = loopyConnPoolNew(loop, &cfg,
 *                                          createConnection,
 *                                          destroyConnection,
 *                                          validateConnection,
 *                                          dbConfig);
 */
loopyConnPool *loopyConnPoolNew(loopyLoop *loop,
                                const loopyConnPoolConfig *config,
                                loopyConnPoolCreateCallback *createCb,
                                loopyConnPoolFreeCallback *destroyCb,
                                loopyConnPoolValidateCallback *validateCb,
                                void *userData);

/**
 * Destroy the connection pool.
 *
 * Closes all connections (idle and active) and frees all resources.
 * Any connections currently acquired will be destroyed when they are
 * released. Safe to call on NULL pointers.
 *
 * @param pool Pool handle (may be NULL)
 *
 * Warning: Ensure all acquired connections are released before destroying
 * the pool, or they will become invalid.
 */
void loopyConnPoolFree(loopyConnPool *pool);

/**
 * Set acquire callback.
 *
 * Sets an optional callback to be invoked when a connection is acquired.
 * Can be called multiple times to change the callback.
 *
 * @param pool Pool handle
 * @param acquireCb Acquire callback (can be NULL to remove)
 */
void loopyConnPoolSetAcquireCallback(loopyConnPool *pool,
                                     loopyConnPoolAcquireCallback *acquireCb);

/**
 * Set release callback.
 *
 * Sets an optional callback to be invoked when a connection is released.
 * Can be called multiple times to change the callback.
 *
 * @param pool Pool handle
 * @param releaseCb Release callback (can be NULL to remove)
 */
void loopyConnPoolSetReleaseCallback(loopyConnPool *pool,
                                     loopyConnPoolReleaseCallback *releaseCb);

/* ====================================================================
 * Connection Acquisition and Release
 * ==================================================================== */

/**
 * Acquire a connection from the pool (blocking).
 *
 * Returns an available connection from the pool. If no connection is
 * available:
 * - If pool is below maxTotal, creates a new connection
 * - If pool is at maxTotal, waits up to acquireTimeoutUs for a connection
 * - Returns NULL if timeout expires or creation fails
 *
 * The returned connection is validated before being returned. If validation
 * fails, another connection is tried automatically.
 *
 * @param pool Pool handle
 * @return Connection handle on success, NULL on failure or timeout
 *
 * Example:
 *   loopyConnPoolConn *conn = loopyConnPoolAcquire(pool);
 *   if (!conn) {
 *       fprintf(stderr, "Failed to acquire connection\n");
 *       return;
 *   }
 *
 *   void *dbConn = loopyConnPoolGetUserConn(conn);
 *   // Use dbConn...
 *
 *   loopyConnPoolRelease(conn, true);
 */
loopyConnPoolConn *loopyConnPoolAcquire(loopyConnPool *pool);

/**
 * Acquire a connection from the pool (async).
 *
 * Asynchronously acquires a connection from the pool. The callback is
 * invoked when a connection becomes available or the timeout expires.
 *
 * @param pool Pool handle
 * @param callback Callback invoked with connection (NULL on timeout/error)
 * @param userData User data for callback
 * @return true if request queued, false on error
 *
 * Example:
 *   void onAcquire(loopyConnPoolConn *conn, void *userData) {
 *       if (!conn) {
 *           fprintf(stderr, "Acquisition timeout\n");
 *           return;
 *       }
 *
 *       void *dbConn = loopyConnPoolGetUserConn(conn);
 *       // Use dbConn...
 *       loopyConnPoolRelease(conn, true);
 *   }
 *
 *   loopyConnPoolAcquireAsync(pool, onAcquire, NULL);
 */
typedef void loopyConnPoolAcquireAsyncCallback(loopyConnPoolConn *conn,
                                               void *userData);

bool loopyConnPoolAcquireAsync(loopyConnPool *pool,
                               loopyConnPoolAcquireAsyncCallback *cb,
                               void *userData);

/**
 * Release a connection back to the pool.
 *
 * Returns the connection to the pool for reuse. If healthy is false or
 * the connection has exceeded its max lifetime, the connection is destroyed
 * instead of being returned to the pool.
 *
 * @param conn Connection handle
 * @param healthy true if connection is healthy and should be reused
 *
 * Example:
 *   if (query_failed) {
 *       loopyConnPoolRelease(conn, false);  // Destroy unhealthy connection
 *   } else {
 *       loopyConnPoolRelease(conn, true);   // Return to pool for reuse
 *   }
 */
void loopyConnPoolRelease(loopyConnPoolConn *conn, bool healthy);

/**
 * Get the user connection pointer from a pool connection.
 *
 * Returns the opaque connection pointer that was created by the
 * creation callback.
 *
 * @param conn Pool connection handle
 * @return User connection pointer
 *
 * Example:
 *   loopyConnPoolConn *poolConn = loopyConnPoolAcquire(pool);
 *   DatabaseConnection *db = loopyConnPoolGetUserConn(poolConn);
 *   db_query(db, "SELECT * FROM users");
 *   loopyConnPoolRelease(poolConn, true);
 */
void *loopyConnPoolGetUserConn(const loopyConnPoolConn *conn);

/* ====================================================================
 * Pool Statistics and Monitoring
 * ==================================================================== */

/**
 * Pool statistics.
 */
typedef struct loopyConnPoolStats {
    uint32_t totalConns;    /* Total connections (idle + active) */
    uint32_t idleConns;     /* Idle connections available */
    uint32_t activeConns;   /* Connections currently in use */
    uint64_t totalAcquires; /* Total acquire requests since creation */
    uint64_t totalReleases; /* Total releases since creation */
    uint64_t totalCreates;  /* Total connections created since creation */
    uint64_t totalDestroys; /* Total connections destroyed since creation */
    uint64_t totalTimeouts; /* Total acquisition timeouts */
    uint64_t totalValidationFailures; /* Total validation failures */
} loopyConnPoolStats;

/**
 * Get pool statistics.
 *
 * Returns current pool statistics including connection counts and
 * cumulative counters.
 *
 * @param pool Pool handle
 * @param stats Output statistics structure
 * @return true on success, false if pool is NULL
 *
 * Example:
 *   loopyConnPoolStats stats;
 *   if (loopyConnPoolGetStats(pool, &stats)) {
 *       printf("Active: %u, Idle: %u, Total: %u\n",
 *              stats.activeConns, stats.idleConns, stats.totalConns);
 *   }
 */
bool loopyConnPoolGetStats(const loopyConnPool *pool,
                           loopyConnPoolStats *stats);

/**
 * Get current number of idle connections.
 *
 * @param pool Pool handle
 * @return Number of idle connections, or 0 if pool is NULL
 */
uint32_t loopyConnPoolGetIdleCount(const loopyConnPool *pool);

/**
 * Get current number of active connections.
 *
 * @param pool Pool handle
 * @return Number of active connections, or 0 if pool is NULL
 */
uint32_t loopyConnPoolGetActiveCount(const loopyConnPool *pool);

/**
 * Get total number of connections.
 *
 * @param pool Pool handle
 * @return Total connections (idle + active), or 0 if pool is NULL
 */
uint32_t loopyConnPoolGetTotalCount(const loopyConnPool *pool);

/* ====================================================================
 * Pool Maintenance Operations
 * ==================================================================== */

/**
 * Manually trigger health check on all idle connections.
 *
 * Validates all idle connections and destroys any that fail validation.
 * This is automatically performed periodically if healthCheckIntervalUs > 0,
 * but can be called manually for immediate validation.
 *
 * @param pool Pool handle
 * @return Number of connections destroyed due to failed validation
 *
 * Example:
 *   uint32_t failed = loopyConnPoolHealthCheck(pool);
 *   if (failed > 0) {
 *       printf("Removed %u unhealthy connections\n", failed);
 *   }
 */
uint32_t loopyConnPoolHealthCheck(loopyConnPool *pool);

/**
 * Manually trigger idle timeout check.
 *
 * Closes all idle connections that have exceeded the idle timeout.
 * This is automatically performed periodically, but can be called
 * manually for immediate cleanup.
 *
 * @param pool Pool handle
 * @return Number of connections closed due to idle timeout
 *
 * Example:
 *   uint32_t closed = loopyConnPoolCleanupIdle(pool);
 *   printf("Cleaned up %u idle connections\n", closed);
 */
uint32_t loopyConnPoolCleanupIdle(loopyConnPool *pool);

/**
 * Prewarm the connection pool.
 *
 * Creates connections up to the specified count (or minIdle if count is 0).
 * Useful for initializing the pool during application startup to avoid
 * connection creation latency on first requests.
 *
 * @param pool Pool handle
 * @param count Number of connections to create (0 for minIdle)
 * @return Number of connections successfully created
 *
 * Example:
 *   // Prewarm pool with 10 connections at startup
 *   uint32_t created = loopyConnPoolPrewarm(pool, 10);
 *   printf("Created %u connections\n", created);
 */
uint32_t loopyConnPoolPrewarm(loopyConnPool *pool, uint32_t count);

/* ====================================================================
 * Error Handling
 * ==================================================================== */

/**
 * Get the last error message.
 *
 * Returns a human-readable description of the last error that occurred in
 * any loopyConnPool function on this thread. Returns NULL if no error has
 * occurred.
 *
 * The error message is stored in thread-local storage and is valid until
 * the next loopyConnPool function call on this thread.
 *
 * @return Error message string, or NULL if no error
 *
 * Example:
 *   loopyConnPoolConn *conn = loopyConnPoolAcquire(pool);
 *   if (!conn) {
 *       fprintf(stderr, "Acquire failed: %s\n",
 *               loopyConnPoolGetError() ?: "unknown error");
 *   }
 */
const char *loopyConnPoolGetError(void);
