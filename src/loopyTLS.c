/* loopyTLS - TLS/SSL support using mbedtls
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

/* Only compile TLS implementation if TLS support is enabled */
#ifndef LOOPY_HAVE_TLS
#define LOOPY_HAVE_TLS 0
#endif

#if LOOPY_HAVE_TLS

#include "loopyAsync.h"
#include "loopyNet.h"
#include "loopyTLS.h"

#include "../deps/datakit/src/datakit.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

/* mbedtls headers - mbedtls 4.x uses PSA crypto, some headers are in private
 * dirs */
#include <mbedtls/debug.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

/* Private crypto headers from tf-psa-crypto/drivers/builtin */
#include "ctr_drbg.h"
#include "entropy.h"

/* ====================================================================
 * Utilities
 * ==================================================================== */

/* String duplication using zmalloc for consistency with project allocators */
static char *tlsStrdup(const char *s) {
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

/* ====================================================================
 * Global State
 * ==================================================================== */

static pthread_once_t tlsInitOnce = PTHREAD_ONCE_INIT;
static mbedtls_entropy_context globalEntropy;
static mbedtls_ctr_drbg_context globalCtrDrbg;
static bool globalInitialized = false;

/* ====================================================================
 * Structures
 * ==================================================================== */

struct loopyTLSContext {
    mbedtls_ssl_config config;
    mbedtls_x509_crt cert;
    mbedtls_x509_crt ca;
    mbedtls_pk_context key;
    loopyTLSMode mode;
    loopyTLSVerify verify;
    char **alpnProtocols;
    size_t alpnCount;
    bool certLoaded;
    bool keyLoaded;
    bool caLoaded;
};

typedef struct loopyTLSPendingOp {
    struct loopyTLSPendingOp *next;
    enum { TLS_OP_HANDSHAKE, TLS_OP_READ, TLS_OP_WRITE } type;
    union {
        struct {
            loopyTLSHandshakeCallback *cb;
        } handshake;
        struct {
            loopyTLSReadCallback *cb;
            void *buf;
            size_t len;
        } read;
        struct {
            loopyTLSWriteCallback *cb;
            const void *data;
            size_t len;
        } write;
    } op;
    void *userData;
} loopyTLSPendingOp;

struct loopyTLS {
    loopyTLSContext *ctx;
    loopyLoop *loop;
    int fd;

    mbedtls_ssl_context ssl;
    bool handshakeDone;
    int lastError;

    /* For non-blocking I/O */
    bool wantRead;
    bool wantWrite;

    /* Async support */
    loopyTLSPendingOp *pendingOps;

    /* Info cache */
    char *hostname;
    char *alpnResult;
};

/* ====================================================================
 * Global Initialization
 * ==================================================================== */

static void tlsInitGlobalLocked(void) {
    /* Called with mutex held */
    if (globalInitialized) {
        return;
    }

    mbedtls_entropy_init(&globalEntropy);
    mbedtls_ctr_drbg_init(&globalCtrDrbg);

    const char *pers = "loopyTLS";
    int ret = mbedtls_ctr_drbg_seed(&globalCtrDrbg, mbedtls_entropy_func,
                                    &globalEntropy, (const unsigned char *)pers,
                                    strlen(pers));
    globalInitialized = (ret == 0);
}

loopyTLSResult loopyTLSInit(void) {
    /* Use double-checked locking for efficiency */
    if (globalInitialized) {
        return LOOPY_TLS_OK;
    }

    /* pthread_once ensures thread-safe first initialization */
    pthread_once(&tlsInitOnce, tlsInitGlobalLocked);
    return globalInitialized ? LOOPY_TLS_OK : LOOPY_TLS_ERROR;
}

void loopyTLSCleanup(void) {
    if (globalInitialized) {
        mbedtls_ctr_drbg_free(&globalCtrDrbg);
        mbedtls_entropy_free(&globalEntropy);
        globalInitialized = false;
        /* Reset the once control so Init can be called again after Cleanup */
        tlsInitOnce = (pthread_once_t)PTHREAD_ONCE_INIT;
    }
}

/* ====================================================================
 * Bio Callbacks for Non-blocking I/O
 * ==================================================================== */

static int bioSend(void *ctx, const unsigned char *buf, size_t len) {
    loopyTLS *tls = ctx;
    ssize_t ret = write(tls->fd, buf, len);

    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            tls->wantWrite = true;
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        }
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }

    return (int)ret;
}

static int bioRecv(void *ctx, unsigned char *buf, size_t len) {
    loopyTLS *tls = ctx;
    ssize_t ret = read(tls->fd, buf, len);

    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            tls->wantRead = true;
            return MBEDTLS_ERR_SSL_WANT_READ;
        }
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }

    if (ret == 0) {
        return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
    }

    return (int)ret;
}

/* ====================================================================
 * Context Configuration
 * ==================================================================== */

void loopyTLSContextConfigInit(loopyTLSContextConfig *config,
                               loopyTLSMode mode) {
    if (!config) {
        return;
    }

    memset(config, 0, sizeof(*config));
    config->mode = mode;
    config->version = LOOPY_TLS_VERSION_AUTO;
    config->verify = (mode == LOOPY_TLS_CLIENT) ? LOOPY_TLS_VERIFY_REQUIRED
                                                : LOOPY_TLS_VERIFY_NONE;
    config->sessionResumption = true;
}

/* ====================================================================
 * Context Lifecycle
 * ==================================================================== */

loopyTLSContext *loopyTLSContextNew(const loopyTLSContextConfig *config) {
    if (!config) {
        return NULL;
    }

    if (loopyTLSInit() != LOOPY_TLS_OK) {
        return NULL;
    }

    loopyTLSContext *ctx = zcalloc(1, sizeof(loopyTLSContext));
    if (!ctx) {
        return NULL;
    }

    ctx->mode = config->mode;
    ctx->verify = config->verify;

    mbedtls_ssl_config_init(&ctx->config);
    mbedtls_x509_crt_init(&ctx->cert);
    mbedtls_x509_crt_init(&ctx->ca);
    mbedtls_pk_init(&ctx->key);

    /* Set defaults */
    int endpoint = (config->mode == LOOPY_TLS_SERVER) ? MBEDTLS_SSL_IS_SERVER
                                                      : MBEDTLS_SSL_IS_CLIENT;

    int ret = mbedtls_ssl_config_defaults(&ctx->config, endpoint,
                                          MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        goto error;
    }

    /* Note: mbedtls 4.x uses PSA crypto which handles RNG internally.
     * No need to call mbedtls_ssl_conf_rng() anymore. */

    /* Set verification mode */
    int authmode;
    switch (config->verify) {
    case LOOPY_TLS_VERIFY_NONE:
        authmode = MBEDTLS_SSL_VERIFY_NONE;
        break;
    case LOOPY_TLS_VERIFY_OPTIONAL:
        authmode = MBEDTLS_SSL_VERIFY_OPTIONAL;
        break;
    case LOOPY_TLS_VERIFY_REQUIRED:
    default:
        authmode = MBEDTLS_SSL_VERIFY_REQUIRED;
        break;
    }
    mbedtls_ssl_conf_authmode(&ctx->config, authmode);

    /* Set version constraints */
    switch (config->version) {
    case LOOPY_TLS_VERSION_1_2:
        mbedtls_ssl_conf_min_tls_version(&ctx->config,
                                         MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_max_tls_version(&ctx->config,
                                         MBEDTLS_SSL_VERSION_TLS1_2);
        break;
    case LOOPY_TLS_VERSION_1_3:
        mbedtls_ssl_conf_min_tls_version(&ctx->config,
                                         MBEDTLS_SSL_VERSION_TLS1_3);
        mbedtls_ssl_conf_max_tls_version(&ctx->config,
                                         MBEDTLS_SSL_VERSION_TLS1_3);
        break;
    case LOOPY_TLS_VERSION_AUTO:
    default:
        mbedtls_ssl_conf_min_tls_version(&ctx->config,
                                         MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_max_tls_version(&ctx->config,
                                         MBEDTLS_SSL_VERSION_TLS1_3);
        break;
    }

    /* Load certificates if specified */
    if (config->certFile) {
        if (loopyTLSContextLoadCert(ctx, config->certFile) != LOOPY_TLS_OK) {
            goto error;
        }
    }

    if (config->keyFile) {
        if (loopyTLSContextLoadKey(ctx, config->keyFile, NULL) !=
            LOOPY_TLS_OK) {
            goto error;
        }
    }

    if (config->caFile || config->caPath) {
        if (loopyTLSContextLoadCA(ctx, config->caFile, config->caPath) !=
            LOOPY_TLS_OK) {
            goto error;
        }
    }

    /* Set ALPN protocols if specified */
    if (config->alpnProtocols) {
        size_t count = 0;
        while (config->alpnProtocols[count]) {
            count++;
        }

        if (count > 0) {
            ctx->alpnProtocols = zcalloc(count + 1, sizeof(char *));
            if (ctx->alpnProtocols) {
                for (size_t i = 0; i < count; i++) {
                    ctx->alpnProtocols[i] = tlsStrdup(config->alpnProtocols[i]);
                }
                ctx->alpnCount = count;
                mbedtls_ssl_conf_alpn_protocols(
                    &ctx->config, (const char **)ctx->alpnProtocols);
            }
        }
    }

    return ctx;

error:
    loopyTLSContextFree(ctx);
    return NULL;
}

void loopyTLSContextFree(loopyTLSContext *ctx) {
    if (!ctx) {
        return;
    }

    mbedtls_ssl_config_free(&ctx->config);
    mbedtls_x509_crt_free(&ctx->cert);
    mbedtls_x509_crt_free(&ctx->ca);
    mbedtls_pk_free(&ctx->key);

    if (ctx->alpnProtocols) {
        for (size_t i = 0; i < ctx->alpnCount; i++) {
            zfree(ctx->alpnProtocols[i]);
        }
        zfree(ctx->alpnProtocols);
    }

    zfree(ctx);
}

loopyTLSResult loopyTLSContextLoadCert(loopyTLSContext *ctx,
                                       const char *certFile) {
    if (!ctx || !certFile) {
        return LOOPY_TLS_ERROR;
    }

    int ret = mbedtls_x509_crt_parse_file(&ctx->cert, certFile);
    if (ret != 0) {
        return LOOPY_TLS_ERROR;
    }

    ctx->certLoaded = true;

    /* Configure certificate if key is also loaded */
    if (ctx->keyLoaded) {
        ret = mbedtls_ssl_conf_own_cert(&ctx->config, &ctx->cert, &ctx->key);
        if (ret != 0) {
            return LOOPY_TLS_ERROR;
        }
    }

    return LOOPY_TLS_OK;
}

loopyTLSResult loopyTLSContextLoadKey(loopyTLSContext *ctx, const char *keyFile,
                                      const char *password) {
    if (!ctx || !keyFile) {
        return LOOPY_TLS_ERROR;
    }

    /* mbedtls 4.x: pk_parse_keyfile takes only 3 args (ctx, path, password) */
    int ret = mbedtls_pk_parse_keyfile(&ctx->key, keyFile, password);
    if (ret != 0) {
        return LOOPY_TLS_ERROR;
    }

    ctx->keyLoaded = true;

    /* Configure certificate if cert is also loaded */
    if (ctx->certLoaded) {
        ret = mbedtls_ssl_conf_own_cert(&ctx->config, &ctx->cert, &ctx->key);
        if (ret != 0) {
            return LOOPY_TLS_ERROR;
        }
    }

    return LOOPY_TLS_OK;
}

loopyTLSResult loopyTLSContextLoadCA(loopyTLSContext *ctx, const char *caFile,
                                     const char *caPath) {
    if (!ctx) {
        return LOOPY_TLS_ERROR;
    }

    int ret = 0;

    if (caFile) {
        ret = mbedtls_x509_crt_parse_file(&ctx->ca, caFile);
        if (ret < 0) {
            return LOOPY_TLS_ERROR;
        }
    }

    if (caPath) {
        ret = mbedtls_x509_crt_parse_path(&ctx->ca, caPath);
        if (ret < 0) {
            return LOOPY_TLS_ERROR;
        }
    }

    ctx->caLoaded = true;
    mbedtls_ssl_conf_ca_chain(&ctx->config, &ctx->ca, NULL);

    return LOOPY_TLS_OK;
}

/* ====================================================================
 * Connection Lifecycle
 * ==================================================================== */

loopyTLS *loopyTLSNew(loopyLoop *loop, loopyTLSContext *ctx, int fd) {
    if (!ctx || fd < 0) {
        return NULL;
    }

    loopyTLS *tls = zcalloc(1, sizeof(loopyTLS));
    if (!tls) {
        return NULL;
    }

    tls->ctx = ctx;
    tls->loop = loop;
    tls->fd = fd;

    mbedtls_ssl_init(&tls->ssl);

    int ret = mbedtls_ssl_setup(&tls->ssl, &ctx->config);
    if (ret != 0) {
        tls->lastError = ret;
        zfree(tls);
        return NULL;
    }

    /* Set BIO callbacks for non-blocking I/O */
    mbedtls_ssl_set_bio(&tls->ssl, tls, bioSend, bioRecv, NULL);

    return tls;
}

loopyTLSResult loopyTLSSetHostname(loopyTLS *tls, const char *hostname) {
    if (!tls || !hostname) {
        return LOOPY_TLS_ERROR;
    }

    int ret = mbedtls_ssl_set_hostname(&tls->ssl, hostname);
    if (ret != 0) {
        tls->lastError = ret;
        return LOOPY_TLS_ERROR;
    }

    zfree(tls->hostname);
    tls->hostname = tlsStrdup(hostname);

    return LOOPY_TLS_OK;
}

loopyTLSResult loopyTLSHandshake(loopyTLS *tls) {
    if (!tls) {
        return LOOPY_TLS_ERROR;
    }

    if (tls->handshakeDone) {
        return LOOPY_TLS_OK;
    }

    tls->wantRead = false;
    tls->wantWrite = false;

    int ret = mbedtls_ssl_handshake(&tls->ssl);

    if (ret == 0) {
        tls->handshakeDone = true;
        return LOOPY_TLS_OK;
    }

    tls->lastError = ret;

    if (ret == MBEDTLS_ERR_SSL_WANT_READ) {
        tls->wantRead = true;
        return LOOPY_TLS_WANT_READ;
    }

    if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        tls->wantWrite = true;
        return LOOPY_TLS_WANT_WRITE;
    }

    if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
        return LOOPY_TLS_VERIFY_FAILED;
    }

    return LOOPY_TLS_ERROR;
}

bool loopyTLSHandshakeAsync(loopyTLS *tls, loopyTLSHandshakeCallback *cb,
                            void *userData) {
    if (!tls || !cb) {
        return false;
    }

    /* Try handshake immediately */
    loopyTLSResult result = loopyTLSHandshake(tls);

    if (result == LOOPY_TLS_OK || result == LOOPY_TLS_ERROR ||
        result == LOOPY_TLS_VERIFY_FAILED) {
        cb(tls, result, userData);
        return true;
    }

    /* Would block - need async handling */
    /* TODO: Register with event loop and retry */
    cb(tls, LOOPY_TLS_HANDSHAKE, userData);
    return true;
}

loopyTLSResult loopyTLSClose(loopyTLS *tls) {
    if (!tls) {
        return LOOPY_TLS_ERROR;
    }

    int ret = mbedtls_ssl_close_notify(&tls->ssl);
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        /* Non-blocking close needs retry */
        return (ret == MBEDTLS_ERR_SSL_WANT_READ) ? LOOPY_TLS_WANT_READ
                                                  : LOOPY_TLS_WANT_WRITE;
    }

    return LOOPY_TLS_OK;
}

void loopyTLSFree(loopyTLS *tls) {
    if (!tls) {
        return;
    }

    mbedtls_ssl_free(&tls->ssl);

    /* Free pending operations */
    loopyTLSPendingOp *op = tls->pendingOps;
    while (op) {
        loopyTLSPendingOp *next = op->next;
        zfree(op);
        op = next;
    }

    zfree(tls->hostname);
    zfree(tls->alpnResult);
    zfree(tls);
}

/* ====================================================================
 * I/O Operations
 * ==================================================================== */

ssize_t loopyTLSRead(loopyTLS *tls, void *buf, size_t len) {
    if (!tls || !buf || len == 0) {
        return LOOPY_TLS_ERROR;
    }

    if (!tls->handshakeDone) {
        return LOOPY_TLS_HANDSHAKE;
    }

    tls->wantRead = false;
    tls->wantWrite = false;

    int ret = mbedtls_ssl_read(&tls->ssl, buf, len);

    if (ret > 0) {
        return ret;
    }

    tls->lastError = ret;

    if (ret == MBEDTLS_ERR_SSL_WANT_READ) {
        tls->wantRead = true;
        return LOOPY_TLS_WANT_READ;
    }

    if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        tls->wantWrite = true;
        return LOOPY_TLS_WANT_WRITE;
    }

    if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == 0) {
        return LOOPY_TLS_CLOSED;
    }

    return LOOPY_TLS_ERROR;
}

ssize_t loopyTLSWrite(loopyTLS *tls, const void *data, size_t len) {
    if (!tls || !data || len == 0) {
        return LOOPY_TLS_ERROR;
    }

    if (!tls->handshakeDone) {
        return LOOPY_TLS_HANDSHAKE;
    }

    tls->wantRead = false;
    tls->wantWrite = false;

    int ret = mbedtls_ssl_write(&tls->ssl, data, len);

    if (ret > 0) {
        return ret;
    }

    tls->lastError = ret;

    if (ret == MBEDTLS_ERR_SSL_WANT_READ) {
        tls->wantRead = true;
        return LOOPY_TLS_WANT_READ;
    }

    if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        tls->wantWrite = true;
        return LOOPY_TLS_WANT_WRITE;
    }

    return LOOPY_TLS_ERROR;
}

bool loopyTLSReadAsync(loopyTLS *tls, void *buf, size_t len,
                       loopyTLSReadCallback *cb, void *userData) {
    if (!tls || !buf || !cb) {
        return false;
    }

    ssize_t ret = loopyTLSRead(tls, buf, len);

    if (ret > 0 || ret == LOOPY_TLS_CLOSED || ret == LOOPY_TLS_ERROR) {
        cb(tls, buf, ret, userData);
        return true;
    }

    /* Would block - TODO: Register with event loop */
    cb(tls, NULL, ret, userData);
    return true;
}

bool loopyTLSWriteAsync(loopyTLS *tls, const void *data, size_t len,
                        loopyTLSWriteCallback *cb, void *userData) {
    if (!tls || !data || !cb) {
        return false;
    }

    ssize_t ret = loopyTLSWrite(tls, data, len);

    if (ret > 0 || ret == LOOPY_TLS_ERROR) {
        cb(tls, ret, userData);
        return true;
    }

    /* Would block - TODO: Register with event loop */
    cb(tls, ret, userData);
    return true;
}

size_t loopyTLSPending(const loopyTLS *tls) {
    if (!tls) {
        return 0;
    }
    return mbedtls_ssl_get_bytes_avail(&tls->ssl);
}

/* ====================================================================
 * Introspection
 * ==================================================================== */

void loopyTLSGetInfo(const loopyTLS *tls, loopyTLSInfo *info) {
    if (!tls || !info) {
        return;
    }

    memset(info, 0, sizeof(*info));

    info->version = mbedtls_ssl_get_version(&tls->ssl);
    info->ciphersuite = mbedtls_ssl_get_ciphersuite(&tls->ssl);
    info->alpnProtocol = mbedtls_ssl_get_alpn_protocol(&tls->ssl);
    info->serverName = tls->hostname;
    info->handshakeComplete = tls->handshakeDone;

    /* Check session resumption */
    /* Note: mbedtls_ssl_session_resumed not available in all versions */
    info->resumed = false;
}

bool loopyTLSIsHandshakeDone(const loopyTLS *tls) {
    return tls ? tls->handshakeDone : false;
}

int loopyTLSGetFD(const loopyTLS *tls) {
    return tls ? tls->fd : -1;
}

const void *loopyTLSGetPeerCert(const loopyTLS *tls) {
    if (!tls) {
        return NULL;
    }
    return mbedtls_ssl_get_peer_cert(&tls->ssl);
}

size_t loopyTLSGetVerifyResult(const loopyTLS *tls, char *buf, size_t size) {
    if (!tls || !buf || size == 0) {
        return 0;
    }

    uint32_t flags = mbedtls_ssl_get_verify_result(&tls->ssl);
    if (flags == 0) {
        return snprintf(buf, size, "OK");
    }

    int ret = mbedtls_x509_crt_verify_info(buf, size, "", flags);
    return (ret > 0) ? (size_t)ret : 0;
}

/* ====================================================================
 * Error Handling
 * ==================================================================== */

int loopyTLSGetError(const loopyTLS *tls) {
    return tls ? tls->lastError : 0;
}

size_t loopyTLSGetErrorString(const loopyTLS *tls, char *buf, size_t size) {
    if (!tls || !buf || size == 0) {
        return 0;
    }

    mbedtls_strerror(tls->lastError, buf, size);
    return strlen(buf);
}

const char *loopyTLSResultName(loopyTLSResult result) {
    switch (result) {
    case LOOPY_TLS_OK:
        return "OK";
    case LOOPY_TLS_WANT_READ:
        return "WANT_READ";
    case LOOPY_TLS_WANT_WRITE:
        return "WANT_WRITE";
    case LOOPY_TLS_ERROR:
        return "ERROR";
    case LOOPY_TLS_CLOSED:
        return "CLOSED";
    case LOOPY_TLS_HANDSHAKE:
        return "HANDSHAKE";
    case LOOPY_TLS_VERIFY_FAILED:
        return "VERIFY_FAILED";
    default:
        return "UNKNOWN";
    }
}

#endif /* LOOPY_HAVE_TLS */
