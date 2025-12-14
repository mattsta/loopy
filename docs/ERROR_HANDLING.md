# Error Handling in loopy

The loopy event loop library uses a unified, hierarchical error code system that provides consistent error reporting across all modules. This document describes how error handling works and best practices for using the library.

## Table of Contents

1. [Unified Status Enum](#unified-status-enum)
2. [Module-Specific Status Codes](#module-specific-status-codes)
3. [Thread-Local Error Storage](#thread-local-error-storage)
4. [Error Checking Patterns](#error-checking-patterns)
5. [Status String Functions](#status-string-functions)
6. [Best Practices](#best-practices)

## Unified Status Enum

All loopy operations that can fail return standardized status codes defined by the `loopyStatus` enumeration in `loopy.h`. This provides a consistent error-handling interface across all modules.

### Core Status Codes

```c
typedef enum loopyStatus {
    LOOPY_OK = 0,            /* Operation succeeded */
    LOOPY_ERROR = -1,        /* Generic/internal error */
    LOOPY_INVALID = -2,      /* Invalid argument */
    LOOPY_TIMEOUT = -3,      /* Operation timed out */
    LOOPY_CANCELLED = -4,    /* Operation was cancelled */
    LOOPY_CLOSED = -5,       /* Resource is closed */
    LOOPY_WOULD_BLOCK = -6,  /* Would block (use with non-blocking ops) */
    LOOPY_NOMEM = -7,        /* Out of memory */
    LOOPY_NOT_FOUND = -8,    /* Resource not found */
    LOOPY_BUSY = -9,         /* Resource is busy */
    LOOPY_AGAIN = -10,       /* Transient failure, try again */
    LOOPY_EOF = -11,         /* End of file/stream */
} loopyStatus;
```

### Semantic Meaning of Core Codes

| Code                | Value | Meaning                        | Recovery                                          |
| ------------------- | ----- | ------------------------------ | ------------------------------------------------- |
| `LOOPY_OK`          | 0     | Operation succeeded            | No action needed                                  |
| `LOOPY_ERROR`       | -1    | Generic or internal error      | Check errno or module-specific error strings      |
| `LOOPY_INVALID`     | -2    | Invalid argument provided      | Review argument values and types                  |
| `LOOPY_TIMEOUT`     | -3    | Operation timed out            | Retry with longer timeout or handle timeout case  |
| `LOOPY_CANCELLED`   | -4    | Operation was cancelled        | May retry or propagate cancellation               |
| `LOOPY_CLOSED`      | -5    | Resource is closed/unavailable | Cannot recover; use new resource                  |
| `LOOPY_WOULD_BLOCK` | -6    | Non-blocking op cannot proceed | Try again when ready (async) or use blocking mode |
| `LOOPY_NOMEM`       | -7    | Out of memory                  | Free resources or exit gracefully                 |
| `LOOPY_NOT_FOUND`   | -8    | Resource not found             | Check if resource exists; adjust query            |
| `LOOPY_BUSY`        | -9    | Resource is busy               | Retry after resource becomes free                 |
| `LOOPY_AGAIN`       | -10   | Transient failure              | Retry operation (similar to EAGAIN)               |
| `LOOPY_EOF`         | -11   | End of file/stream reached     | Normal completion for streaming operations        |

### Checking Status Codes

All status codes are **zero or negative**:

- Positive values: Reserved for future use
- `LOOPY_OK (0)`: Success case
- Negative values: Error or special condition

```c
loopyStatus result = someOperation();

/* Typical patterns */
if (result == LOOPY_OK) {
    /* Success */
}

if (result != LOOPY_OK) {
    /* Error occurred */
}

/* Specific error handling */
switch (result) {
case LOOPY_OK:
    /* Success */
    break;
case LOOPY_TIMEOUT:
    /* Handle timeout */
    break;
case LOOPY_NOMEM:
    /* Handle out of memory */
    break;
default:
    /* Handle other errors */
    break;
}
```

## Module-Specific Status Codes

Each loopy module extends the core status codes with module-specific codes in designated negative ranges. This allows modules to report domain-specific errors while maintaining compatibility with the core enum.

### Status Code Ranges

```
Core loopy codes:           -1 to -11
loopyChannel codes:         -100 to -199
loopyDNS codes:             -200 to -299
loopyTLS codes:             -300 to -399
loopyRateLimit codes:       -400 to -499
loopyConcurrency codes:     -500 to -599
```

### loopyChannel Status Codes

Defined in `loopyChannel.h`:

```c
typedef enum loopyChannelStatus {
    /* Common codes from loopyStatus */
    LOOPY_CHANNEL_OK = LOOPY_OK,           /* Operation succeeded */
    LOOPY_CHANNEL_ERROR = LOOPY_ERROR,     /* Internal error */
    LOOPY_CHANNEL_INVALID = LOOPY_INVALID, /* Invalid argument */
    LOOPY_CHANNEL_TIMEOUT = LOOPY_TIMEOUT, /* Operation timed out */
    LOOPY_CHANNEL_CLOSED = LOOPY_CLOSED,   /* Channel has been closed */

    /* Module-specific codes */
    LOOPY_CHANNEL_FULL = -100,             /* Channel is full (non-blocking send) */
    LOOPY_CHANNEL_EMPTY = -101,            /* Channel is empty (non-blocking recv) */
} loopyChannelStatus;
```

**Usage Example:**

```c
loopyChannelStatus status = loopyChannelTrySend(ch, data, len);

if (status == LOOPY_CHANNEL_OK) {
    printf("Message sent successfully\n");
} else if (status == LOOPY_CHANNEL_FULL) {
    printf("Channel is full, message not sent\n");
} else if (status == LOOPY_CHANNEL_CLOSED) {
    printf("Channel is closed\n");
}

/* Convert to human-readable string */
printf("Error: %s\n", loopyChannelStatusName(status));
```

### loopyDNS Status Codes

Defined in `loopyDNS.h`:

```c
typedef enum loopyDNSStatus {
    /* Common codes from loopyStatus */
    LOOPY_DNS_OK = LOOPY_OK,             /* Success */
    LOOPY_DNS_ERROR = LOOPY_ERROR,       /* Other error */
    LOOPY_DNS_TIMEOUT = LOOPY_TIMEOUT,   /* Query timed out */
    LOOPY_DNS_CANCELLED = LOOPY_CANCELLED, /* Query was cancelled */

    /* Module-specific codes */
    LOOPY_DNS_NXDOMAIN = -200,           /* Domain does not exist */
    LOOPY_DNS_SERVFAIL = -201,           /* Server failure */
} loopyDNSStatus;
```

**Usage Example:**

```c
void dns_callback(loopyDNS *dns, const loopyDNSResult *result) {
    if (result->status == LOOPY_DNS_OK) {
        printf("Resolved %s to %d addresses\n",
               result->hostname, result->addressCount);
        for (size_t i = 0; i < result->addressCount; i++) {
            printf("  %s\n", result->addresses[i].str);
        }
    } else if (result->status == LOOPY_DNS_NXDOMAIN) {
        printf("Domain %s does not exist\n", result->hostname);
    } else if (result->status == LOOPY_DNS_TIMEOUT) {
        printf("DNS query timed out\n");
    } else {
        printf("DNS error: %s\n", loopyDNSStatusString(result->status));
    }
}
```

### loopyTLS Status Codes

Defined in `loopyTLS.h`:

```c
typedef enum loopyTLSResult {
    /* Common codes from loopyStatus */
    LOOPY_TLS_OK = LOOPY_OK,         /* Success */
    LOOPY_TLS_ERROR = LOOPY_ERROR,   /* Error occurred */
    LOOPY_TLS_CLOSED = LOOPY_CLOSED, /* Connection closed */
    LOOPY_TLS_AGAIN = LOOPY_AGAIN,   /* Try again */

    /* Module-specific codes */
    LOOPY_TLS_WANT_READ = -300,      /* Need more data to read */
    LOOPY_TLS_WANT_WRITE = -301,     /* Need to write data */
    LOOPY_TLS_HANDSHAKE = -302,      /* Handshake in progress */
    LOOPY_TLS_VERIFY_FAILED = -303,  /* Certificate verification failed */
} loopyTLSResult;
```

**Usage Example:**

```c
loopyTLSResult result = loopyTLSHandshake(tls);

if (result == LOOPY_TLS_OK) {
    printf("Handshake complete\n");
} else if (result == LOOPY_TLS_WANT_READ) {
    /* Need more data from socket - wait for read event */
    printf("Handshake needs read\n");
} else if (result == LOOPY_TLS_WANT_WRITE) {
    /* Need to send data - wait for write event */
    printf("Handshake needs write\n");
} else if (result == LOOPY_TLS_VERIFY_FAILED) {
    printf("Certificate verification failed\n");
    char buf[256];
    loopyTLSGetErrorString(tls, buf, sizeof(buf));
    printf("Details: %s\n", buf);
}
```

### loopyRateLimit Status Codes

Defined in `loopyRateLimit.h`:

```c
typedef enum loopyRateLimitResult {
    /* Common codes from loopyStatus */
    LOOPY_RATE_LIMIT_OK = LOOPY_OK,       /* Request allowed */
    LOOPY_RATE_LIMIT_ERROR = LOOPY_ERROR, /* Internal error */

    /* Module-specific codes */
    LOOPY_RATE_LIMIT_DENIED = -400,       /* Request denied (rate exceeded) */

    /* Alias for clarity */
    LOOPY_RATE_LIMIT_ALLOWED = LOOPY_RATE_LIMIT_OK,
} loopyRateLimitResult;
```

**Usage Example:**

```c
loopyRateLimitResult result = loopyRateLimitCheck(limiter, 1.0);

if (result == LOOPY_RATE_LIMIT_ALLOWED) {
    /* Process request */
    handleRequest();
} else if (result == LOOPY_RATE_LIMIT_DENIED) {
    /* Rate limit exceeded */
    loopyRateLimitInfo info;
    loopyRateLimitPeek(limiter, &info);
    printf("Rate limit exceeded. Retry after %llu ms\n", info.retryAfterMs);
    rejectRequest();
}
```

### loopyConcurrency Status Codes

The concurrency limiter uses boolean return values for acquire operations, with timeout semantics:

```c
/* Try to acquire a slot (non-blocking) */
bool acquired = loopyConcurrencyTryAcquire(limiter);

/* Acquire with timeout (blocking) */
uint64_t timeoutMs = 5000;
bool acquired = loopyConcurrencyAcquire(limiter, timeoutMs);

if (acquired) {
    /* Slot acquired, perform operation */
    doWork();
} else {
    /* Timeout or limit exceeded */
    rejectRequest();
}

/* Always release when done */
loopyConcurrencyRelease(limiter);
```

## Thread-Local Error Storage

Some modules, like `loopyFlock`, use thread-local storage to maintain error messages that can be retrieved after operations fail. This allows you to get human-readable error descriptions without passing error buffers everywhere.

### loopyFlock Error Messages

The file locking module stores the last error per thread:

```c
int fd = open("data.txt", O_RDWR);
loopyFlockConfig config = LOOPY_FLOCK_CONFIG_DEFAULT;

loopyFlock *lock = loopyFlockNew(fd, &config);
if (!lock) {
    /* Get the error message - valid for this thread only */
    const char *error = loopyFlockGetError();
    if (error) {
        fprintf(stderr, "Lock failed: %s\n", error);
    } else {
        fprintf(stderr, "Lock failed (unknown error)\n");
    }
    close(fd);
    return -1;
}

/* Use lock... */
loopyFlockFree(lock);
close(fd);
```

**Important Notes:**

- The error message is stored per-thread in thread-local storage
- The message is valid until the next lock operation on the same thread
- Must not be shared across threads
- Always call the getter immediately after the failed operation
- Safe for multi-threaded applications (each thread has its own message)

## Error Checking Patterns

### Pattern 1: Simple Success Check

```c
loopyStatus result = loopyChannelSend(ch, &msg, sizeof(msg));

if (result != LOOPY_OK) {
    fprintf(stderr, "Send failed: %s\n", loopyChannelStatusName(result));
    return -1;
}

/* Continue processing */
```

### Pattern 2: Specific Error Handling

```c
loopyChannelStatus status = loopyChannelTrySend(ch, &msg, sizeof(msg));

switch (status) {
case LOOPY_CHANNEL_OK:
    printf("Message sent\n");
    break;

case LOOPY_CHANNEL_FULL:
    printf("Channel full, queuing for async send...\n");
    loopyChannelSendAsync(ch, &msg, sizeof(msg), sendCallback, userData);
    break;

case LOOPY_CHANNEL_CLOSED:
    fprintf(stderr, "Channel is closed, cannot send\n");
    return -1;

default:
    fprintf(stderr, "Send failed: %s\n", loopyChannelStatusName(status));
    return -1;
}
```

### Pattern 3: Conditional Retry Logic

```c
loopyStatus result = attemptOperation();

if (result == LOOPY_AGAIN || result == LOOPY_WOULD_BLOCK) {
    /* Transient failure, safe to retry */
    printf("Transient error, retrying...\n");
    result = attemptOperation();
    if (result != LOOPY_OK) {
        fprintf(stderr, "Retry failed\n");
        return -1;
    }
} else if (result != LOOPY_OK) {
    /* Permanent failure */
    fprintf(stderr, "Operation failed: %s\n", loopyStatusString(result));
    return -1;
}
```

### Pattern 4: Timeout Handling

```c
loopyDNSQueryId queryId = loopyDNSResolve(dns, hostname,
                                          LOOPY_DNS_A,
                                          dns_callback,
                                          userData);

if (queryId == 0) {
    fprintf(stderr, "DNS query submission failed\n");
    return -1;
}

/* In callback, check for timeout */
void dns_callback(loopyDNS *dns, const loopyDNSResult *result) {
    if (result->status == LOOPY_DNS_TIMEOUT) {
        fprintf(stderr, "DNS resolution timed out for %s\n",
                result->hostname);
        /* Handle timeout - may retry with different server */
    } else if (result->status == LOOPY_DNS_OK) {
        /* Process result */
    }
}
```

### Pattern 5: Resource Cleanup on Error

```c
loopyRateLimiter *limiter = loopyRateLimiterNew(loop, &config);
if (!limiter) {
    fprintf(stderr, "Failed to create rate limiter\n");
    return -1;
}

loopyChannel *ch = loopyChannelNew(loop, &ch_config);
if (!ch) {
    fprintf(stderr, "Failed to create channel\n");
    loopyRateLimiterFree(limiter);  /* Clean up previously allocated resource */
    return -1;
}

/* ... use both resources ... */

/* Clean up in reverse order */
loopyChannelFree(ch);
loopyRateLimiterFree(limiter);
```

## Status String Functions

All modules provide functions to convert status codes to human-readable strings:

### Core Status String

```c
const char *loopyStatusString(loopyStatus status);
```

Converts any core loopyStatus code to a descriptive string.

```c
loopyStatus result = someOperation();
printf("Result: %s\n", loopyStatusString(result));
```

### Module-Specific Status Strings

Each module provides a specialized status string function:

```c
/* loopyChannel */
const char *loopyChannelStatusName(loopyChannelStatus status);

/* loopyDNS */
const char *loopyDNSStatusString(loopyDNSStatus status);

/* loopyTLS */
const char *loopyTLSResultName(loopyTLSResult result);

/* loopyRateLimit */
const char *loopyRateLimitResultName(loopyRateLimitResult result);

/* loopyFlock */
const char *loopyFlockGetError(void);  /* Thread-local error message */
```

### Detailed Error Information

Some modules provide detailed error information for debugging:

```c
/* TLS: Get detailed error information */
void loopyTLSGetErrorString(loopyTLS *tls, char *buf, size_t size);

/* TLS: Get verification result details */
size_t loopyTLSGetVerifyResult(loopyTLS *tls, char *buf, size_t size);

/* DNS: Access result structure for details */
void dns_callback(loopyDNS *dns, const loopyDNSResult *result) {
    if (result->status != LOOPY_DNS_OK) {
        /* result->gaierrno contains system error code */
        printf("DNS error: %s (gaierrno=%d)\n",
               loopyDNSStatusString(result->status),
               result->gaierrno);
    }
}
```

## Best Practices

### 1. Always Check Return Values

```c
/* Good */
loopyStatus result = loopyChannelSend(ch, data, len);
if (result != LOOPY_OK) {
    handleError(result);
}

/* Bad - ignores errors */
loopyChannelSend(ch, data, len);
```

### 2. Use Appropriate Error Level for the Code

```c
loopyStatus result = operation();

if (result == LOOPY_NOMEM) {
    /* Critical error, should log and exit */
    fprintf(stderr, "FATAL: Out of memory\n");
    exit(1);
} else if (result == LOOPY_TIMEOUT) {
    /* Recoverable error, may retry */
    printf("WARNING: Operation timed out, retrying...\n");
    result = operation();
} else if (result == LOOPY_WOULD_BLOCK) {
    /* Expected in non-blocking mode, handle asynchronously */
    scheduleAsync();
}
```

### 3. Distinguish Between Transient and Permanent Failures

```c
/* Transient: LOOPY_AGAIN, LOOPY_WOULD_BLOCK, LOOPY_BUSY */
loopyStatus result = operation();
if (result == LOOPY_AGAIN) {
    /* Safe to retry immediately or later */
    sleep(100);  /* small backoff */
    result = operation();
}

/* Permanent: LOOPY_INVALID, LOOPY_NOT_FOUND, LOOPY_CLOSED */
if (result == LOOPY_INVALID) {
    /* Cannot proceed, must fix input and retry */
    exit(1);
}
```

### 4. Provide Context in Error Messages

```c
loopyChannelStatus status = loopyChannelRecv(ch, buf, bufLen);
if (status != LOOPY_CHANNEL_OK) {
    fprintf(stderr, "Failed to receive from channel %llu: %s\n",
            loopyChannelGetId(ch),
            loopyChannelStatusName(status));
}
```

### 5. Use Callbacks for Async Error Handling

```c
void my_callback(loopyChannel *ch, const void *data,
                 size_t len, loopyChannelStatus status,
                 void *userData) {
    if (status != LOOPY_CHANNEL_OK) {
        fprintf(stderr, "Async recv failed: %s\n",
                loopyChannelStatusName(status));
        return;
    }

    /* Process data */
}

bool queued = loopyChannelRecvAsync(ch, my_callback, userData);
if (!queued) {
    fprintf(stderr, "Failed to queue async receive\n");
}
```

### 6. Handle Resource Cleanup on Error

```c
loopyTLSContext *ctx = loopyTLSContextNew(&config);
if (!ctx) {
    fprintf(stderr, "Failed to create TLS context\n");
    return -1;
}

loopyTLSResult result = loopyTLSContextLoadCert(ctx, certPath);
if (result != LOOPY_TLS_OK) {
    fprintf(stderr, "Failed to load certificate: %s\n",
            loopyTLSResultName(result));
    loopyTLSContextFree(ctx);  /* Always clean up */
    return -1;
}

loopyTLS *tls = loopyTLSNew(loop, ctx, fd);
if (!tls) {
    fprintf(stderr, "Failed to create TLS connection\n");
    loopyTLSContextFree(ctx);  /* Clean up context */
    return -1;
}

/* Use tls... */

loopyTLSFree(tls);
loopyTLSContextFree(ctx);
```

### 7. Log Enough Information for Debugging

```c
typedef struct {
    const char *operation;
    loopyStatus status;
    uint64_t timestamp;
    const char *context;
} ErrorLog;

void logError(const char *op, loopyStatus status, const char *ctx) {
    fprintf(stderr, "[ERROR] Operation: %s, Status: %s, Context: %s\n",
            op, loopyStatusString(status), ctx ? ctx : "none");
}

/* Usage */
loopyStatus result = loopyChannelSend(ch, data, len);
if (result != LOOPY_OK) {
    logError("loopyChannelSend", result, "sending message to worker thread");
}
```

### 8. Consider Retry Strategies for Specific Errors

```c
#define MAX_RETRIES 3

loopyStatus result;
for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
    result = operation();

    if (result == LOOPY_OK) {
        break;  /* Success */
    } else if (result == LOOPY_AGAIN || result == LOOPY_BUSY) {
        /* Transient, retry with backoff */
        usleep(100000 * (attempt + 1));  /* exponential backoff */
        continue;
    } else {
        /* Permanent failure */
        fprintf(stderr, "Permanent error: %s\n",
                loopyStatusString(result));
        return -1;
    }
}

if (result != LOOPY_OK) {
    fprintf(stderr, "Failed after %d retries\n", MAX_RETRIES);
    return -1;
}
```

### 9. Use Dedicated Error Checking in Critical Sections

```c
loopyStatus status;

/* Critical section: multiple operations must succeed */
status = operation1();
if (status != LOOPY_OK) goto error;

status = operation2();
if (status != LOOPY_OK) goto error;

status = operation3();
if (status != LOOPY_OK) goto error;

/* Success path */
return 0;

error:
    fprintf(stderr, "Critical operation failed: %s\n",
            loopyStatusString(status));
    cleanup();
    return -1;
```

### 10. Document Error Conditions in Your API

```c
/**
 * Process a message from the channel.
 *
 * @param ch   Input channel
 * @param msg  Output message buffer
 * @return LOOPY_OK on success
 *         LOOPY_INVALID if ch or msg is NULL
 *         LOOPY_CLOSED if channel is closed
 *         LOOPY_TIMEOUT if receive times out
 *         LOOPY_NOMEM on memory allocation failure
 *
 * Thread Safety: Safe to call from multiple threads.
 */
loopyStatus processMessage(loopyChannel *ch, struct Message *msg);
```

## Summary

The loopy error handling system provides:

- **Unified status codes** for consistent error reporting across modules
- **Module-specific extensions** for domain-specific errors
- **Thread-local error messages** for detailed error information
- **Standard conversion functions** to human-readable error strings
- **Clear semantic meaning** for each error code to guide recovery strategies

By following these patterns and best practices, you can build robust, maintainable error handling in applications using loopy.
