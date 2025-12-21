/* loopyTLS - TLS/SSL support for loopy using mbedtls
 *
 * Provides secure communication over TCP with TLS 1.2/1.3 support.
 * Integrates with the loopy event loop for non-blocking I/O.
 *
 * Key Features:
 * - TLS client and server modes
 * - Non-blocking handshake and I/O
 * - Certificate and key management
 * - SNI (Server Name Indication) support
 * - ALPN (Application-Layer Protocol Negotiation)
 * - Session resumption
 * - Event loop integration
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

#pragma once

#include "loopyPlatform.h"

#include "loopy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

/* Check if TLS support is enabled at compile time.
 * Set by CMake via -DLOOPY_HAVE_TLS=1 or -DLOOPY_HAVE_TLS=0
 * If not defined, default to disabled for safety. */
#ifndef LOOPY_HAVE_TLS
#define LOOPY_HAVE_TLS 0
#endif

/* ====================================================================
 * Types
 * ==================================================================== */

/**
 * Opaque TLS context - shared configuration for multiple connections.
 */
typedef struct loopyTLSContext loopyTLSContext;

/**
 * Opaque TLS connection.
 */
typedef struct loopyTLS loopyTLS;

/**
 * TLS connection mode.
 */
typedef enum loopyTLSMode {
    LOOPY_TLS_CLIENT = 0, /* Client mode */
    LOOPY_TLS_SERVER = 1, /* Server mode */
} loopyTLSMode;

/**
 * TLS operation result codes.
 *
 * Common codes reference base loopyStatus values directly.
 * Module-specific codes use the -300 range.
 */
typedef enum loopyTLSResult {
    LOOPY_TLS_OK = LOOPY_OK,         /* Success */
    LOOPY_TLS_ERROR = LOOPY_ERROR,   /* Error occurred */
    LOOPY_TLS_CLOSED = LOOPY_CLOSED, /* Connection closed */
    LOOPY_TLS_AGAIN = LOOPY_AGAIN,   /* Try again */
    /* Module-specific codes */
    LOOPY_TLS_WANT_READ = -300,     /* Need more data to read */
    LOOPY_TLS_WANT_WRITE = -301,    /* Need to write data */
    LOOPY_TLS_HANDSHAKE = -302,     /* Handshake in progress */
    LOOPY_TLS_VERIFY_FAILED = -303, /* Certificate verification failed */
} loopyTLSResult;

/**
 * TLS version preferences.
 */
typedef enum loopyTLSVersion {
    LOOPY_TLS_VERSION_1_2 = 0,  /* TLS 1.2 only */
    LOOPY_TLS_VERSION_1_3 = 1,  /* TLS 1.3 only */
    LOOPY_TLS_VERSION_AUTO = 2, /* Auto-negotiate (prefer 1.3) */
} loopyTLSVersion;

/**
 * Certificate verification mode.
 */
typedef enum loopyTLSVerify {
    LOOPY_TLS_VERIFY_NONE = 0,     /* No verification (insecure) */
    LOOPY_TLS_VERIFY_OPTIONAL = 1, /* Verify if certificate present */
    LOOPY_TLS_VERIFY_REQUIRED = 2, /* Require valid certificate */
} loopyTLSVerify;

/**
 * TLS context configuration.
 */
typedef struct loopyTLSContextConfig {
    loopyTLSMode mode;          /* Client or server mode */
    loopyTLSVersion version;    /* TLS version preference */
    loopyTLSVerify verify;      /* Certificate verification mode */
    const char *certFile;       /* Certificate file path (PEM) */
    const char *keyFile;        /* Private key file path (PEM) */
    const char *caFile;         /* CA certificate file path (PEM) */
    const char *caPath;         /* CA certificate directory path */
    const char *ciphersuites;   /* Colon-separated cipher list */
    const char **alpnProtocols; /* NULL-terminated ALPN protocol list */
    bool sessionResumption;     /* Enable session tickets/caching */
} loopyTLSContextConfig;

/**
 * TLS connection information.
 */
typedef struct loopyTLSInfo {
    const char *version;      /* TLS version string */
    const char *ciphersuite;  /* Negotiated cipher suite */
    const char *alpnProtocol; /* Negotiated ALPN protocol */
    const char *serverName;   /* SNI hostname */
    bool resumed;             /* Session was resumed */
    bool handshakeComplete;   /* Handshake finished */
} loopyTLSInfo;

/* ====================================================================
 * Callbacks
 * ==================================================================== */

/**
 * Handshake completion callback.
 *
 * @param tls       The TLS connection
 * @param result    Handshake result
 * @param userData  User-provided data
 */
typedef void loopyTLSHandshakeCallback(loopyTLS *tls, loopyTLSResult result,
                                       void *userData);

/**
 * Read completion callback.
 *
 * @param tls       The TLS connection
 * @param data      Buffer containing read data
 * @param len       Bytes read, or negative result code
 * @param userData  User-provided data
 */
typedef void loopyTLSReadCallback(loopyTLS *tls, const void *data, ssize_t len,
                                  void *userData);

/**
 * Write completion callback.
 *
 * @param tls       The TLS connection
 * @param len       Bytes written, or negative result code
 * @param userData  User-provided data
 */
typedef void loopyTLSWriteCallback(loopyTLS *tls, ssize_t len, void *userData);

/**
 * Close notification callback.
 *
 * @param tls       The TLS connection
 * @param userData  User-provided data
 */
typedef void loopyTLSCloseCallback(loopyTLS *tls, void *userData);

/* ====================================================================
 * Context Lifecycle
 * ==================================================================== */

#if LOOPY_HAVE_TLS

/**
 * Initialize context configuration with defaults.
 *
 * @param config  Configuration to initialize
 * @param mode    Client or server mode
 */
void loopyTLSContextConfigInit(loopyTLSContextConfig *config,
                               loopyTLSMode mode);

/**
 * Create a TLS context.
 *
 * The context holds shared configuration (certificates, ciphers) and
 * can be used to create multiple TLS connections.
 *
 * @param config  Context configuration
 * @return New context, or NULL on error
 */
loopyTLSContext *loopyTLSContextNew(const loopyTLSContextConfig *config);

/**
 * Destroy a TLS context.
 *
 * All connections using this context must be closed first.
 *
 * @param ctx The context to destroy
 */
void loopyTLSContextFree(loopyTLSContext *ctx);

/**
 * Load a certificate chain from PEM file.
 *
 * @param ctx       The context
 * @param certFile  Path to certificate file
 * @return LOOPY_TLS_OK on success
 */
loopyTLSResult loopyTLSContextLoadCert(loopyTLSContext *ctx,
                                       const char *certFile);

/**
 * Load a private key from PEM file.
 *
 * @param ctx      The context
 * @param keyFile  Path to key file
 * @param password Password for encrypted key (NULL if unencrypted)
 * @return LOOPY_TLS_OK on success
 */
loopyTLSResult loopyTLSContextLoadKey(loopyTLSContext *ctx, const char *keyFile,
                                      const char *password);

/**
 * Load CA certificates for verification.
 *
 * @param ctx     The context
 * @param caFile  Path to CA certificate file
 * @param caPath  Path to CA certificate directory (NULL for file only)
 * @return LOOPY_TLS_OK on success
 */
loopyTLSResult loopyTLSContextLoadCA(loopyTLSContext *ctx, const char *caFile,
                                     const char *caPath);

/* ====================================================================
 * Connection Lifecycle
 * ==================================================================== */

/**
 * Create a TLS connection wrapper around an existing socket.
 *
 * @param loop  Event loop for async operations
 * @param ctx   TLS context
 * @param fd    Socket file descriptor (must be non-blocking)
 * @return New TLS connection, or NULL on error
 *
 * Note: The socket should already be connected (for client) or
 *       accepted (for server). Ownership of the fd is NOT transferred.
 */
loopyTLS *loopyTLSNew(loopyLoop *loop, loopyTLSContext *ctx, int fd);

/**
 * Set the server name for SNI (client mode).
 *
 * Must be called before handshake.
 *
 * @param tls      The TLS connection
 * @param hostname Server hostname
 * @return LOOPY_TLS_OK on success
 */
loopyTLSResult loopyTLSSetHostname(loopyTLS *tls, const char *hostname);

/**
 * Perform the TLS handshake.
 *
 * This is a synchronous call that may return WANT_READ/WANT_WRITE
 * indicating the operation should be retried when the socket is ready.
 *
 * @param tls The TLS connection
 * @return LOOPY_TLS_OK on success, or status code
 */
loopyTLSResult loopyTLSHandshake(loopyTLS *tls);

/**
 * Perform async TLS handshake.
 *
 * The callback is invoked when handshake completes or fails.
 *
 * @param tls       The TLS connection
 * @param cb        Completion callback
 * @param userData  User data for callback
 * @return true if handshake started
 */
bool loopyTLSHandshakeAsync(loopyTLS *tls, loopyTLSHandshakeCallback *cb,
                            void *userData);

/**
 * Close the TLS connection gracefully.
 *
 * Sends close_notify and waits for peer acknowledgment.
 *
 * @param tls The TLS connection
 * @return LOOPY_TLS_OK on success
 */
loopyTLSResult loopyTLSClose(loopyTLS *tls);

/**
 * Free the TLS connection.
 *
 * Does not close the underlying socket.
 *
 * @param tls The TLS connection
 */
void loopyTLSFree(loopyTLS *tls);

/* ====================================================================
 * I/O Operations
 * ==================================================================== */

/**
 * Read decrypted data from the TLS connection.
 *
 * @param tls    The TLS connection
 * @param buf    Buffer to read into
 * @param len    Maximum bytes to read
 * @return Bytes read, or negative result code
 */
ssize_t loopyTLSRead(loopyTLS *tls, void *buf, size_t len);

/**
 * Write data to the TLS connection (will be encrypted).
 *
 * @param tls    The TLS connection
 * @param data   Data to write
 * @param len    Bytes to write
 * @return Bytes written, or negative result code
 */
ssize_t loopyTLSWrite(loopyTLS *tls, const void *data, size_t len);

/**
 * Async read with callback.
 *
 * @param tls       The TLS connection
 * @param buf       Buffer to read into
 * @param len       Maximum bytes to read
 * @param cb        Completion callback
 * @param userData  User data for callback
 * @return true if read was queued
 */
bool loopyTLSReadAsync(loopyTLS *tls, void *buf, size_t len,
                       loopyTLSReadCallback *cb, void *userData);

/**
 * Async write with callback.
 *
 * @param tls       The TLS connection
 * @param data      Data to write
 * @param len       Bytes to write
 * @param cb        Completion callback
 * @param userData  User data for callback
 * @return true if write was queued
 */
bool loopyTLSWriteAsync(loopyTLS *tls, const void *data, size_t len,
                        loopyTLSWriteCallback *cb, void *userData);

/**
 * Check if there is buffered data available for reading.
 *
 * TLS may have decrypted more data than was returned by the last read.
 *
 * @param tls The TLS connection
 * @return Bytes available
 */
size_t loopyTLSPending(const loopyTLS *tls);

/* ====================================================================
 * Introspection
 * ==================================================================== */

/**
 * Get connection information.
 *
 * Retrieves comprehensive information about the TLS connection, including
 * the negotiated protocol version, cipher suite, and ALPN protocol.
 *
 * @param tls   The TLS connection
 * @param info  Output info structure (must not be NULL)
 *
 * @note The pointers within info (version, ciphersuite, etc.) point to
 *       internal strings that are valid only during the connection's
 *       lifetime. Do NOT use these after loopyTLSFree() or loopyTLSClose().
 *
 * @note For closed connections, the info will contain the last values
 *       from before closure. Call this function before closing if you
 *       need to preserve the information.
 *
 * @note If the handshake hasn't completed yet (handshakeComplete is false),
 *       the version and ciphersuite fields may be NULL.
 *
 * @note The serverName field is the hostname set via loopyTLSSetHostname(),
 *       which may differ from what the server reports.
 *
 * Example:
 * @code
 *   loopyTLSInfo info;
 *   loopyTLSGetInfo(tls, &info);
 *   if (info.handshakeComplete) {
 *       printf("TLS Version: %s\n", info.version);
 *       printf("Cipher: %s\n", info.ciphersuite);
 *       if (info.alpnProtocol) {
 *           printf("ALPN: %s\n", info.alpnProtocol);
 *       }
 *   }
 * @endcode
 */
void loopyTLSGetInfo(const loopyTLS *tls, loopyTLSInfo *info);

/**
 * Check if handshake is complete.
 *
 * @param tls The TLS connection
 * @return true if handshake finished successfully
 */
bool loopyTLSIsHandshakeDone(const loopyTLS *tls);

/**
 * Get the underlying socket file descriptor.
 *
 * @param tls The TLS connection
 * @return Socket fd
 */
int loopyTLSGetFD(const loopyTLS *tls);

/**
 * Get the peer's certificate (if any).
 *
 * @param tls The TLS connection
 * @return Opaque pointer to mbedtls certificate, or NULL
 */
const void *loopyTLSGetPeerCert(const loopyTLS *tls);

/**
 * Get verification result as string.
 *
 * Retrieves a human-readable string describing certificate verification
 * results. On successful verification, returns "OK". On failure, returns
 * detailed error information.
 *
 * @param tls  The TLS connection
 * @param buf  Buffer for result string (must not be NULL)
 * @param size Buffer size in bytes (must be > 0)
 * @return Number of characters written (excluding null terminator),
 *         or 0 if tls is NULL or buf is invalid
 *
 * @note The function returns "OK" for successful verification, or a
 *       comma-separated list of verification failures like:
 *       "certificate verify failed, self signed certificate"
 *
 * @note The result is only meaningful after the handshake completes.
 *       Before handshake, this may return an empty or default string.
 *
 * @note If the buffer is too small, the result will be truncated.
 *       Recommend at least 256 bytes for safety.
 *
 * @note This function only provides verification results if verification
 *       was requested (verify mode != LOOPY_TLS_VERIFY_NONE).
 *
 * Example:
 * @code
 *   char verify_info[256];
 *   size_t len = loopyTLSGetVerifyResult(tls, verify_info,
 * sizeof(verify_info)); if (len > 0) { printf("Verification: %s\n",
 * verify_info); } else { printf("Verification OK\n");
 *   }
 * @endcode
 */
size_t loopyTLSGetVerifyResult(const loopyTLS *tls, char *buf, size_t size);

/* ====================================================================
 * Error Handling
 * ==================================================================== */

/**
 * Get the last error code.
 *
 * @param tls The TLS connection
 * @return mbedtls error code
 */
int loopyTLSGetError(const loopyTLS *tls);

/**
 * Get error description string.
 *
 * Converts the last TLS error code into a human-readable error description.
 * Useful for debugging TLS connection failures.
 *
 * @param tls  The TLS connection
 * @param buf  Buffer for error string (must not be NULL)
 * @param size Buffer size in bytes (must be > 0)
 * @return Number of characters written (excluding null terminator),
 *         or 0 if tls is NULL or buf is invalid
 *
 * @note mbedtls error strings are limited to ~200-300 characters typically.
 *       Recommend allocating at least 256 bytes for the buffer.
 *
 * @note The error string describes the last error that occurred on this
 *       connection (from handshake, read, write, or close operations).
 *
 * @note If no error has occurred, returns an empty string or "no error".
 *
 * @note The buffer will be null-terminated.
 *
 * @note Common TLS errors include:
 *       - "SSL - Bad input parameters to function"
 *       - "X509 - Certificate verification failed"
 *       - "SSL - The handshake negotiation failed"
 *
 * Example:
 * @code
 *   if (loopyTLSHandshake(tls) != LOOPY_TLS_OK) {
 *       char errbuf[256];
 *       loopyTLSGetErrorString(tls, errbuf, sizeof(errbuf));
 *       fprintf(stderr, "Handshake failed: %s\n", errbuf);
 *   }
 * @endcode
 */
size_t loopyTLSGetErrorString(const loopyTLS *tls, char *buf, size_t size);

/**
 * Convert result code to string.
 *
 * Converts a loopyTLSResult code to a human-readable string representation.
 * Useful for logging and debugging TLS operation results.
 *
 * @param result The result code (loopyTLSResult enum value)
 * @return Static string describing the result code, never NULL.
 *         Returns "UNKNOWN" for unrecognized codes.
 *
 * @note The returned strings are:
 *       - LOOPY_TLS_OK -> "OK"
 *       - LOOPY_TLS_ERROR -> "ERROR"
 *       - LOOPY_TLS_CLOSED -> "CLOSED"
 *       - LOOPY_TLS_AGAIN -> "AGAIN"
 *       - LOOPY_TLS_WANT_READ -> "WANT_READ"
 *       - LOOPY_TLS_WANT_WRITE -> "WANT_WRITE"
 *       - LOOPY_TLS_HANDSHAKE -> "HANDSHAKE"
 *       - LOOPY_TLS_VERIFY_FAILED -> "VERIFY_FAILED"
 *       - (other) -> "UNKNOWN"
 *
 * @note The returned pointer is valid for the lifetime of the program
 *       (static string storage).
 *
 * Example:
 * @code
 *   loopyTLSResult result = loopyTLSHandshake(tls);
 *   printf("Handshake result: %s\n", loopyTLSResultName(result));
 *
 *   if (result == LOOPY_TLS_WANT_READ || result == LOOPY_TLS_WANT_WRITE) {
 *       printf("Waiting for I/O: %s\n", loopyTLSResultName(result));
 *   }
 * @endcode
 */
const char *loopyTLSResultName(loopyTLSResult result);

/* ====================================================================
 * Utility
 * ==================================================================== */

/**
 * Initialize the TLS library (called automatically).
 *
 * Initializes global TLS state including entropy sources and random number
 * generators. This is called automatically by loopyTLSContextNew(), but you
 * can call it explicitly to ensure initialization happens at a specific time.
 *
 * @return LOOPY_TLS_OK on success, LOOPY_TLS_ERROR if initialization failed
 *
 * @note Thread Safety: Uses pthread_once internally for thread-safe one-time
 *       initialization. Safe to call from multiple threads.
 *
 * @note Idempotent: Calling multiple times has no negative effect (only
 *       initializes once).
 *
 * @note Automatic: loopyTLSContextNew() calls this automatically, so you
 *       typically don't need to call it explicitly.
 *
 * @note Must be balanced with loopyTLSCleanup() before program exit if you
 *       want to free global TLS resources.
 *
 * Example:
 * @code
 *   // Explicit initialization (optional)
 *   if (loopyTLSInit() != LOOPY_TLS_OK) {
 *       fprintf(stderr, "TLS initialization failed\n");
 *       return -1;
 *   }
 *   // ... use TLS ...
 *   loopyTLSCleanup();
 * @endcode
 */
loopyTLSResult loopyTLSInit(void);

/**
 * Cleanup the TLS library.
 *
 * Frees all global TLS resources (entropy, RNG) allocated by loopyTLSInit().
 * Should be called at program exit after all TLS connections are closed
 * and contexts are freed.
 *
 * @note Thread Safety: Must be called from the main thread or with
 *       synchronization to avoid concurrent TLS operations.
 *
 * @note Idempotent: Safe to call multiple times (subsequent calls are no-ops).
 *
 * @note After calling this, loopyTLSInit() can be called again to
 *       re-initialize if needed (e.g., after fork()).
 *
 * @note Proper cleanup sequence:
 *       1. Close all TLS connections (loopyTLSClose)
 *       2. Free all TLS contexts (loopyTLSContextFree)
 *       3. Call loopyTLSCleanup()
 *
 * Example:
 * @code
 *   int main() {
 *       loopyTLSInit();
 *       // ... create contexts, connections, use TLS ...
 *       // ... close all connections and free contexts ...
 *       loopyTLSCleanup();
 *       return 0;
 *   }
 * @endcode
 */
void loopyTLSCleanup(void);

#else /* !LOOPY_HAVE_TLS */

/* ====================================================================
 * Stub Implementations (TLS disabled at compile time)
 * ====================================================================
 * These inline stubs allow code to compile without TLS support.
 * All functions return appropriate error values indicating TLS
 * is not available.
 */

static inline void loopyTLSContextConfigInit(loopyTLSContextConfig *config,
                                              loopyTLSMode mode) {
    (void)config;
    (void)mode;
}

static inline loopyTLSContext *loopyTLSContextNew(
    const loopyTLSContextConfig *config) {
    (void)config;
    return NULL;
}

static inline void loopyTLSContextFree(loopyTLSContext *ctx) {
    (void)ctx;
}

static inline loopyTLSResult loopyTLSContextLoadCert(loopyTLSContext *ctx,
                                                      const char *certFile) {
    (void)ctx;
    (void)certFile;
    return LOOPY_TLS_ERROR;
}

static inline loopyTLSResult loopyTLSContextLoadKey(loopyTLSContext *ctx,
                                                     const char *keyFile,
                                                     const char *password) {
    (void)ctx;
    (void)keyFile;
    (void)password;
    return LOOPY_TLS_ERROR;
}

static inline loopyTLSResult loopyTLSContextLoadCA(loopyTLSContext *ctx,
                                                    const char *caFile,
                                                    const char *caPath) {
    (void)ctx;
    (void)caFile;
    (void)caPath;
    return LOOPY_TLS_ERROR;
}

static inline loopyTLS *loopyTLSNew(loopyLoop *loop, loopyTLSContext *ctx,
                                     int fd) {
    (void)loop;
    (void)ctx;
    (void)fd;
    return NULL;
}

static inline loopyTLSResult loopyTLSSetHostname(loopyTLS *tls,
                                                  const char *hostname) {
    (void)tls;
    (void)hostname;
    return LOOPY_TLS_ERROR;
}

static inline loopyTLSResult loopyTLSHandshake(loopyTLS *tls) {
    (void)tls;
    return LOOPY_TLS_ERROR;
}

static inline bool loopyTLSHandshakeAsync(loopyTLS *tls,
                                           loopyTLSHandshakeCallback *cb,
                                           void *userData) {
    (void)tls;
    (void)cb;
    (void)userData;
    return false;
}

static inline loopyTLSResult loopyTLSClose(loopyTLS *tls) {
    (void)tls;
    return LOOPY_TLS_ERROR;
}

static inline void loopyTLSFree(loopyTLS *tls) {
    (void)tls;
}

static inline ssize_t loopyTLSRead(loopyTLS *tls, void *buf, size_t len) {
    (void)tls;
    (void)buf;
    (void)len;
    return LOOPY_TLS_ERROR;
}

static inline ssize_t loopyTLSWrite(loopyTLS *tls, const void *data,
                                     size_t len) {
    (void)tls;
    (void)data;
    (void)len;
    return LOOPY_TLS_ERROR;
}

static inline bool loopyTLSReadAsync(loopyTLS *tls, void *buf, size_t len,
                                      loopyTLSReadCallback *cb, void *userData) {
    (void)tls;
    (void)buf;
    (void)len;
    (void)cb;
    (void)userData;
    return false;
}

static inline bool loopyTLSWriteAsync(loopyTLS *tls, const void *data,
                                       size_t len, loopyTLSWriteCallback *cb,
                                       void *userData) {
    (void)tls;
    (void)data;
    (void)len;
    (void)cb;
    (void)userData;
    return false;
}

static inline size_t loopyTLSPending(const loopyTLS *tls) {
    (void)tls;
    return 0;
}

static inline void loopyTLSGetInfo(const loopyTLS *tls, loopyTLSInfo *info) {
    (void)tls;
    if (info) {
        info->version = NULL;
        info->ciphersuite = NULL;
        info->alpnProtocol = NULL;
        info->serverName = NULL;
        info->resumed = false;
        info->handshakeComplete = false;
    }
}

static inline bool loopyTLSIsHandshakeDone(const loopyTLS *tls) {
    (void)tls;
    return false;
}

static inline int loopyTLSGetFD(const loopyTLS *tls) {
    (void)tls;
    return -1;
}

static inline const void *loopyTLSGetPeerCert(const loopyTLS *tls) {
    (void)tls;
    return NULL;
}

static inline size_t loopyTLSGetVerifyResult(const loopyTLS *tls, char *buf,
                                              size_t size) {
    (void)tls;
    if (buf && size > 0) {
        buf[0] = '\0';
    }
    return 0;
}

static inline int loopyTLSGetError(const loopyTLS *tls) {
    (void)tls;
    return -1;
}

static inline size_t loopyTLSGetErrorString(const loopyTLS *tls, char *buf,
                                             size_t size) {
    (void)tls;
    if (buf && size > 0) {
        const char *msg = "TLS not supported (compiled without USE_TLS)";
        size_t len = strlen(msg);
        if (len >= size) {
            len = size - 1;
        }
        memcpy(buf, msg, len);
        buf[len] = '\0';
        return len;
    }
    return 0;
}

static inline const char *loopyTLSResultName(loopyTLSResult result) {
    switch (result) {
    case LOOPY_TLS_OK:
        return "OK";
    case LOOPY_TLS_ERROR:
        return "ERROR";
    case LOOPY_TLS_CLOSED:
        return "CLOSED";
    case LOOPY_TLS_AGAIN:
        return "AGAIN";
    case LOOPY_TLS_WANT_READ:
        return "WANT_READ";
    case LOOPY_TLS_WANT_WRITE:
        return "WANT_WRITE";
    case LOOPY_TLS_HANDSHAKE:
        return "HANDSHAKE";
    case LOOPY_TLS_VERIFY_FAILED:
        return "VERIFY_FAILED";
    default:
        return "UNKNOWN";
    }
}

static inline loopyTLSResult loopyTLSInit(void) {
    return LOOPY_TLS_ERROR;
}

static inline void loopyTLSCleanup(void) {
}

#endif /* LOOPY_HAVE_TLS */
