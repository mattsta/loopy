/* loopyNet - networking wrappers with error message propagation
 *
 * Copyright 2016 Matt Stancliff <matt@genges.com>
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

#include <arpa/inet.h>
#include <stdbool.h>
#include <stddef.h> /* size_t */
#include <stdint.h>
#include <sys/types.h>

#if __linux__
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 9, 0)
#define LOOPY_NET_HAS_KERNEL_LOAD_BALANCER 1
#endif
#endif

typedef enum loopyNetResolveFlag {
    LOOPY_NET_RESOLVE_NONE = 0x00,
    LOOPY_NET_RESOLVE_ONLY_IP = 0x01,
    /* Note: bitwise exclusive */
} loopyNetResolveFlag;

typedef enum loopyNetListenFlag {
    LOOPY_NET_SERVER_NONE = 0x00,
    LOOPY_NET_SERVER_REUSEPORT = 0x01, /* generic REUSEPORT across platforms */
    LOOPY_NET_SERVER_L4_LOAD_BALANCE = 0x02, /* REUSEPORT only when it can LB */
    /* Note: bitwise exclusive */
} loopyNetListenFlag;

typedef struct loopyNetResolveData {
    char ip[INET6_ADDRSTRLEN];
    struct sockaddr_in sa;
    struct sockaddr_in6 sa6;
    int af;
} loopyNetResolveData;

/* 256 + 4 + 4 = 264 byte struct */
typedef struct loopyNet {
    char errorString[256];
    loopyNetListenFlag listenFlag;
    int sock; /* -1 here means "invalid socket" */
} loopyNet;

/* Accessors for loopyNet fields - preferred over direct access */
#define loopyNetSockGet(_l) ((_l)->sock)
#define loopyNetSockSet(_l, _sock)                                             \
    do {                                                                       \
        (_l)->sock = (_sock);                                                  \
    } while (0)

#define loopyNetGetError(_l) ((_l)->errorString)
#define loopyNetClearError(_l)                                                 \
    do {                                                                       \
        (_l)->errorString[0] = '\0';                                           \
    } while (0)

#define loopyNetReusePortEnable(_l)                                            \
    do {                                                                       \
        (_l)->listenFlag = LOOPY_NET_SERVER_REUSEPORT;                         \
    } while (0)

#if defined(__sun) || defined(_AIX)
#define AF_LOCAL AF_UNIX
#endif

#ifdef _AIX
#undef ipLen
#endif

/**
 * Create a TCP connection to a remote address (blocking).
 *
 * Establishes a synchronous TCP connection. The socket is created and the
 * connect() syscall is performed. Returns when connected or connection fails.
 *
 * @param l loopyNet structure to hold the socket (will be initialized)
 * @param addr Hostname or IP address to connect to (required)
 * @param port Remote port number (1-65535)
 * @return true on success, false on failure (check loopyNetGetError() for
 * details)
 *
 * @note This is a blocking operation. For non-blocking I/O (recommended for
 *       event-driven code), use loopyNetTcpNonBlockConnect() instead.
 *
 * @see loopyNetTcpNonBlockConnect()
 * @see loopyNetGetError()
 * @see loopyNetSockGet()
 */
bool loopyNetTcpConnect(loopyNet *l, const char *addr, int port);

/**
 * Create a non-blocking TCP connection to a remote address.
 *
 * Creates a socket and initiates a non-blocking TCP connection. The socket
 * is set to non-blocking mode immediately. The connection may complete
 * asynchronously - use select/epoll to monitor for writability, then check
 * socket error status with loopyNetCheckSocketError().
 *
 * Example:
 * @code
 *   loopyNet net = {0};
 *   if (!loopyNetTcpNonBlockConnect(&net, "example.com", 80)) {
 *       fprintf(stderr, "Connect failed: %s\n", loopyNetGetError(&net));
 *       return;
 *   }
 *   // Socket is now in non-blocking connection state
 *   int fd = loopyNetSockGet(&net);
 *   // Register fd with epoll/select, wait for writability
 *   // When writable, check:
 *   if (!loopyNetCheckSocketError(&net)) {
 *       printf("Connection established\n");
 *   } else {
 *       printf("Connection failed\n");
 *   }
 * @endcode
 *
 * @param l loopyNet structure to hold the socket
 * @param addr Hostname or IP address to connect to (required)
 * @param port Remote port number (1-65535)
 * @return true if socket created and connect initiated (may still be
 * connecting), false if socket creation failed (check loopyNetGetError())
 *
 * @note Even if this returns true, the connection may still fail. Use
 *       loopyNetCheckSocketError() after the socket becomes writable to check
 *       actual connection status.
 *
 * @see loopyNetCheckSocketError()
 * @see loopyNetTcpConnect()
 * @see loopyNetTcpNonBlockConnectBind()
 */
bool loopyNetTcpNonBlockConnect(loopyNet *l, const char *addr, int port);

/**
 * Create a non-blocking TCP connection with explicit bind address.
 *
 * Like loopyNetTcpNonBlockConnect(), but binds the local end to a specific
 * address first. Useful for multi-homed systems or when you need to control
 * which interface the connection uses.
 *
 * @param l loopyNet structure to hold the socket
 * @param addr Remote address to connect to (required)
 * @param port Remote port number (1-65535)
 * @param bindAddr Local address to bind before connecting (required, cannot be
 * NULL)
 * @return true if connection initiated successfully, false on failure
 *
 * @note If binding fails, returns false. Use loopyNetGetError() for details.
 *
 * @see loopyNetTcpNonBlockConnect()
 * @see loopyNetTcpNonBlockConnectBindOptional()
 */
bool loopyNetTcpNonBlockConnectBind(loopyNet *l, const char *addr, int port,
                                    const char *bindAddr);

/**
 * Create a non-blocking TCP connection with optional bind address.
 *
 * Like loopyNetTcpNonBlockConnectBind(), but if binding to bindAddr fails,
 * automatically retries without a bind address. Provides a fallback mechanism
 * for flexible deployment.
 *
 * @param l loopyNet structure to hold the socket
 * @param addr Remote address to connect to (required)
 * @param port Remote port number (1-65535)
 * @param bindAddr Preferred local address to bind (optional, NULL for any)
 * @return true if connection initiated successfully, false on failure
 *
 * @note This function always attempts to connect - if binding fails, it retries
 *       without a bind address. Only returns false if both attempts fail.
 *
 * @see loopyNetTcpNonBlockConnectBind()
 * @see loopyNetTcpNonBlockConnect()
 */
bool loopyNetTcpNonBlockConnectBindOptional(loopyNet *l, const char *addr,
                                            int port, const char *bindAddr);

/**
 * Check for asynchronous connection error.
 *
 * For non-blocking connections, call this after the socket becomes writable
 * (or after connect returns) to determine if the connection succeeded. Uses
 * SO_ERROR socket option to retrieve connection status.
 *
 * @param l loopyNet structure with connected socket
 * @return true if no error (socket is ready), false if connection error
 * occurred
 *
 * @note Must be called after socket is writable. Safe to call on any socket,
 *       but most useful after non-blocking connect attempts.
 *
 * @see loopyNetTcpNonBlockConnect()
 */
bool loopyNetCheckSocketError(loopyNet *l);

/**
 * Create a blocking Unix domain socket connection.
 *
 * Establishes a synchronous connection to a Unix domain socket. The socket
 * file must exist at the specified path.
 *
 * @param l loopyNet structure to hold the socket
 * @param path Filesystem path to the Unix domain socket (required)
 * @return true on success, false on failure (check loopyNetGetError())
 *
 * @note This is a blocking operation. For event-driven code, use
 *       loopyNetUnixNonBlockConnect() instead.
 *
 * @see loopyNetUnixNonBlockConnect()
 * @see loopyNetGetError()
 */
bool loopyNetUnixConnect(loopyNet *l, const char *path);

/**
 * Create a non-blocking Unix domain socket connection.
 *
 * Creates a socket and initiates a non-blocking connection to a Unix domain
 * socket. The socket is set to non-blocking mode.
 *
 * @param l loopyNet structure to hold the socket
 * @param path Filesystem path to the Unix domain socket (required)
 * @return true if socket created and connect initiated, false on failure
 *
 * @note Use with event loop monitoring. When the socket becomes writable,
 *       use loopyNetCheckSocketError() to verify the connection succeeded.
 *
 * @see loopyNetUnixConnect()
 * @see loopyNetCheckSocketError()
 */
bool loopyNetUnixNonBlockConnect(loopyNet *l, const char *path);

/**
 * Resolve a hostname to an IP address string (synchronous DNS).
 *
 * Performs DNS resolution using getaddrinfo(). Returns the IP address as
 * a string in either IPv4 or IPv6 format.
 *
 * @param l loopyNet structure (error message stored here)
 * @param host Hostname to resolve (e.g., "example.com", "localhost")
 * @param ipbuf Output buffer for IP address string
 * @param ipbufLen Size of ipbuf (must be at least INET6_ADDRSTRLEN = 46 bytes)
 * @return true on success, false on resolution failure (check
 * loopyNetGetError())
 *
 * @note This is a blocking operation. For production use with event loops,
 *       consider async DNS libraries. Result will be IPv4 or IPv6 as available.
 *
 * @see loopyNetResolveExtra()
 * @see loopyNetResolveIP()
 */
bool loopyNetResolve(loopyNet *l, const char *host, char *ipbuf,
                     size_t ipbufLen);

/**
 * Resolve a hostname with full address information (synchronous DNS).
 *
 * Like loopyNetResolve(), but returns additional information including the
 * resolved sockaddr structures (IPv4 and IPv6) and address family.
 *
 * @param l loopyNet structure (error message stored here)
 * @param host Hostname to resolve
 * @param full Output structure containing IP string and sockaddr details
 * @return true on success, false on failure
 *
 * @note Returned sockaddr structures (full->sa and full->sa6) can be used
 *       directly with socket operations. The address family is in full->af.
 *
 * @see loopyNetResolve()
 * @see loopyNetResolveIPExtra()
 */
bool loopyNetResolveExtra(loopyNet *l, const char *host,
                          loopyNetResolveData *full);

/**
 * Validate and return an IP address string (no hostname resolution).
 *
 * Validates that the string is a valid IP address (IPv4 or IPv6) without
 * performing DNS resolution. Useful for rejecting hostnames and accepting
 * only literal IP addresses.
 *
 * @param l loopyNet structure (error message stored here)
 * @param host String to validate (should be IP address, not hostname)
 * @param ipbuf Output buffer for the IP address string (copy of host if valid)
 * @param ipbufLen Size of ipbuf
 * @return true if host is a valid IP address, false otherwise
 *
 * @note If the input is a hostname (not an IP), this returns false and does
 *       no DNS resolution. Use loopyNetResolve() for hostname resolution.
 *
 * @see loopyNetResolve()
 * @see loopyNetResolveIPExtra()
 */
bool loopyNetResolveIP(loopyNet *l, const char *host, char *ipbuf,
                       size_t ipbufLen);

/**
 * Validate an IP address with full address information.
 *
 * Like loopyNetResolveIP(), but returns additional information including
 * the sockaddr structures and address family.
 *
 * @param l loopyNet structure (error message stored here)
 * @param host String to validate (should be IP address)
 * @param full Output structure containing IP string and sockaddr details
 * @return true if host is a valid IP address, false otherwise
 *
 * @note Returns the same additional data as loopyNetResolveExtra() but
 *       only accepts literal IP addresses.
 *
 * @see loopyNetResolveIP()
 * @see loopyNetResolveExtra()
 */
bool loopyNetResolveIPExtra(loopyNet *l, const char *host,
                            loopyNetResolveData *full);

/**
 * Create a listening IPv4 TCP socket (TCP server).
 *
 * Creates and binds a TCP socket to an IPv4 address and port, then calls
 * listen(). Sets SO_REUSEADDR to avoid TIME_WAIT issues.
 *
 * Example:
 * @code
 *   loopyNet server = {0};
 *   if (!loopyNetTcp4Server(&server, 8080, "0.0.0.0", 128)) {
 *       fprintf(stderr, "Failed to create server: %s\n",
 * loopyNetGetError(&server)); return;
 *   }
 *   int fd = loopyNetSockGet(&server);
 *   // Register fd with epoll, accept connections when readable
 * @endcode
 *
 * @param l loopyNet structure to hold the listening socket
 * @param port Port number to listen on (1-65535)
 * @param bindAddr IPv4 address to bind ("0.0.0.0" for all interfaces,
 * "127.0.0.1" for localhost)
 * @param backlog Connection queue depth (typically 128-512)
 * @return true on success, false on failure (check loopyNetGetError())
 *
 * @note Backlog is a hint to the OS - actual queue depth may differ. Once
 *       created, use accept() or loopyNetTcpAccept() to accept connections.
 *
 * @see loopyNetTcp6Server()
 * @see loopyNetTcpAccept()
 * @see loopyNetGetError()
 */
bool loopyNetTcp4Server(loopyNet *l, int port, const char *bindAddr,
                        int backlog);

/**
 * Create a listening IPv6 TCP socket (TCP server).
 *
 * Like loopyNetTcp4Server(), but creates an IPv6 listening socket. The socket
 * will be IPv6-only (IPV6_V6ONLY enabled) to avoid conflicts with IPv4.
 *
 * @param l loopyNet structure to hold the listening socket
 * @param port Port number to listen on (1-65535)
 * @param bindAddr IPv6 address to bind ("::" for all interfaces, "::1" for
 * localhost)
 * @param backlog Connection queue depth
 * @return true on success, false on failure (check loopyNetGetError())
 *
 * @note This creates an IPv6-only socket (does not accept IPv4 connections).
 *       For dual-stack, use IPv4 socket with IPv4-mapped IPv6 addresses.
 *
 * @see loopyNetTcp4Server()
 * @see loopyNetGetError()
 */
bool loopyNetTcp6Server(loopyNet *l, int port, const char *bindAddr,
                        int backlog);

/**
 * Create a listening Unix domain socket.
 *
 * Creates and binds a Unix domain socket for local IPC, then calls listen().
 * The socket file is created at the specified path.
 *
 * @param l loopyNet structure to hold the listening socket
 * @param path Filesystem path for the socket file (will be created)
 * @param perm File permissions for the socket (e.g., 0666, 0755, 0 for default)
 * @param backlog Connection queue depth
 * @return true on success, false on failure (check loopyNetGetError())
 *
 * @note If the socket file already exists, bind() will fail. Remove it first
 *       if needed: unlink(path). The socket file persists until explicitly
 *       deleted - consider cleanup on shutdown.
 *
 * @see loopyNetTcp4Server()
 * @see loopyNetUnixAccept()
 */
bool loopyNetUnixServer(loopyNet *l, const char *path, mode_t perm,
                        int backlog);

/**
 * Accept an incoming TCP connection (blocking).
 *
 * Accepts a new connection on the listening socket. Blocks if no connections
 * are pending. Sets the SO_NOSIGPIPE flag on the accepted socket if available.
 *
 * @param l loopyNet structure with listening socket
 * @param ip Output buffer for client IP address string (IPv4 or IPv6)
 * @param ipLen Size of ip buffer (must be at least INET6_ADDRSTRLEN = 46)
 * @param port Output pointer for client port number (optional, may be NULL)
 * @param cliSock Output pointer for new client socket file descriptor
 * @return true on success, false on failure (check loopyNetGetError())
 *
 * @note This is a blocking operation. Only call when the listening socket is
 *       known to have a pending connection. For event-driven code, use
 *       loopyNetTcpAcceptNonBlock() instead.
 *
 * @see loopyNetTcpAcceptNonBlock()
 * @see loopyNetTcp4Server()
 */
bool loopyNetTcpAccept(loopyNet *l, char *ip, size_t ipLen, int *port,
                       int *cliSock);

/**
 * Accept an incoming TCP connection (non-blocking).
 *
 * Accepts a new connection on the listening socket. Returns immediately if
 * no connections are pending (EAGAIN/EWOULDBLOCK). Sets the socket to
 * non-blocking mode on platforms without accept4().
 *
 * @param l loopyNet structure with listening socket
 * @param ip Output buffer for client IP address string
 * @param ipLen Size of ip buffer
 * @param port Output pointer for client port number (optional, may be NULL)
 * @param cliSock Output pointer for new client socket file descriptor
 * @return true if a connection was accepted, false if no connection pending
 *         or on error (check loopyNetGetError() for error details)
 *
 * @note After successful accept, the returned cliSock is non-blocking and
 *       ready for use with event loop monitoring.
 *
 * @see loopyNetTcpAccept()
 */
bool loopyNetTcpAcceptNonBlock(loopyNet *l, char *ip, size_t ipLen, int *port,
                               int *cliSock);

/**
 * Accept an incoming Unix domain socket connection (blocking).
 *
 * Accepts a new connection on the listening Unix domain socket. Blocks if
 * no connections are pending.
 *
 * @param l loopyNet structure with listening socket
 * @param cliSock Output pointer for new client socket file descriptor
 * @return true on success, false on failure
 *
 * @note This is a blocking operation. For non-blocking code, use
 *       loopyNetUnixAcceptNonBlock() instead.
 *
 * @see loopyNetUnixAcceptNonBlock()
 * @see loopyNetUnixServer()
 */
bool loopyNetUnixAccept(loopyNet *l, int *cliSock);

/**
 * Accept an incoming Unix domain socket connection (non-blocking).
 *
 * Accepts a new connection on the listening Unix domain socket. Returns
 * immediately if no connections are pending. On platforms without accept4(),
 * the socket is set to non-blocking mode.
 *
 * @param l loopyNet structure with listening socket
 * @param cliSock Output pointer for new client socket file descriptor
 * @return true if a connection was accepted, false if no connection pending
 *         or on error
 *
 * @note The returned socket is non-blocking and ready for event loop use.
 *
 * @see loopyNetUnixAccept()
 */
bool loopyNetUnixAcceptNonBlock(loopyNet *l, int *cliSock);

/**
 * Set the socket in loopyNet structure to non-blocking mode.
 *
 * Sets the O_NONBLOCK flag on the socket. All future operations (read, write,
 * accept, connect) will return immediately rather than blocking.
 *
 * @param l loopyNet structure with socket to configure
 * @return true on success, false on error (check loopyNetGetError())
 *
 * @note The socket must be valid and stored in l->sock.
 *
 * @see loopyNetSetNonBlock()
 * @see loopyNetNonBlockDisable()
 */
bool loopyNetNonBlockEnable(loopyNet *l);

/**
 * Set non-blocking mode on any socket (not just the one in loopyNet).
 *
 * Generic version of non-blocking mode configuration. Useful when you need
 * to set non-blocking on sockets not stored in a loopyNet structure.
 *
 * @param l loopyNet structure (for error reporting, may be NULL)
 * @param sock File descriptor to configure
 * @param nonBlock true to enable non-blocking, false to disable
 * @return true on success, false on error
 *
 * @note This function modifies the given fd directly without using l->sock.
 *       Useful for configuring client sockets after accept().
 *
 * @see loopyNetNonBlockEnable()
 * @see loopyNetNonBlockDisable()
 */
bool loopyNetSetNonBlock(loopyNet *l, int sock, bool nonBlock);

/**
 * Clear non-blocking mode on the socket in loopyNet (disable non-blocking).
 *
 * Clears the O_NONBLOCK flag, returning the socket to blocking mode.
 *
 * @param l loopyNet structure with socket to configure
 * @return true on success, false on error
 *
 * @note Rarely used in event-driven code, but useful for specific scenarios.
 *
 * @see loopyNetNonBlockEnable()
 * @see loopyNetSetNonBlock()
 */
bool loopyNetNonBlockDisable(loopyNet *l);

/**
 * Enable TCP_NODELAY (Nagle's algorithm disable).
 *
 * Disables Nagle's algorithm on TCP sockets. Data is sent immediately instead
 * of being buffered. Reduces latency at the cost of more packets.
 *
 * @param l loopyNet structure with TCP socket
 * @return true on success, false on error
 *
 * @note Useful for interactive protocols (SSH, telnet) and latency-sensitive
 *       applications. Not recommended for bulk data transfer.
 *
 * @see loopyNetTcpNoDelayDisable()
 */
bool loopyNetTcpNoDelayEnable(loopyNet *l);

/**
 * Disable TCP_NODELAY (enable Nagle's algorithm).
 *
 * Re-enables Nagle's algorithm for TCP flow optimization. Data may be buffered
 * briefly to combine with ACKs.
 *
 * @param l loopyNet structure with TCP socket
 * @return true on success, false on error
 *
 * @note This is the default mode for TCP sockets in most operating systems.
 *
 * @see loopyNetTcpNoDelayEnable()
 */
bool loopyNetTcpNoDelayDisable(loopyNet *l);

/**
 * Enable TCP keep-alive with default settings.
 *
 * Enables SO_KEEPALIVE to detect dead peers. On Linux, also sets platform-
 * specific keep-alive parameters for faster failure detection.
 *
 * @param l loopyNet structure with TCP socket
 * @return true on success, false on error
 *
 * @note On Linux, uses TCP_KEEPIDLE, TCP_KEEPINTVL, TCP_KEEPCNT with sensible
 *       defaults. On macOS/BSD, just enables SO_KEEPALIVE. Interval parameter
 *       is platform-dependent.
 *
 * @see loopyNetKeepAlive()
 */
bool loopyNetTcpKeepAliveEnable(loopyNet *l);

/**
 * Configure TCP keep-alive with custom interval.
 *
 * Enables SO_KEEPALIVE with platform-specific tuning. On Linux, sets the
 * keep-alive probe interval in seconds.
 *
 * @param l loopyNet structure with TCP socket
 * @param interval Time in seconds before sending first keep-alive probe
 * @return true on success, false on error
 *
 * @note On Linux: Also sets TCP_KEEPINTVL to interval/3 and TCP_KEEPCNT to 3,
 *       providing failure detection in ~interval seconds. On other platforms,
 *       the interval parameter may be ignored.
 *
 * @see loopyNetTcpKeepAliveEnable()
 */
bool loopyNetKeepAlive(loopyNet *l, int interval);

/**
 * Set socket send timeout.
 *
 * Configures SO_SNDTIMEO to limit how long blocking send operations can take.
 * Useful for preventing indefinite hangs on slow or broken connections.
 *
 * @param l loopyNet structure with socket
 * @param ms Timeout in milliseconds (0 to disable timeout)
 * @return true on success, false on error
 *
 * @note Timeout applies to send operations. Set to 0 to disable. Note that
 *       timeouts only work properly with blocking sockets in most systems.
 *
 * @see loopyNetNonBlockEnable()
 */
bool loopyNetSendTimeout(loopyNet *l, uint64_t ms);

/* Address parsing and formatting utilities
 *
 * These functions provide consistent IPv4/IPv6 address handling across
 * the entire loopy platform. Use these instead of manual string parsing.
 *
 * Address string formats:
 *   IPv4:     "192.168.1.1:9000" or "192.168.1.1"
 *   IPv6:     "[::1]:9000" or "[2001:db8::1]:9000" or "::1"
 *   Hostname: "localhost:9000" or "example.com"
 */

/**
 * Parse an address string into host and port components.
 *
 * Handles IPv4, IPv6 (with bracket notation), and hostnames uniformly.
 * For IPv6, brackets are stripped from the output host buffer. Useful for
 * parsing connection strings from configuration or command-line arguments.
 *
 * Address string formats:
 * - IPv4: "127.0.0.1", "192.168.1.1:9000"
 * - IPv6: "[::1]", "[::1]:9000", "[2001:db8::1]:80"
 * - Hostname: "localhost", "example.com:8080"
 *
 * Examples:
 * @code
 *   loopyNetParseAddr("[::1]:9000", host, 100, &port, 80);
 *   // Result: host="::1", port=9000
 *
 *   loopyNetParseAddr("127.0.0.1:8080", host, 100, &port, 80);
 *   // Result: host="127.0.0.1", port=8080
 *
 *   loopyNetParseAddr("localhost", host, 100, &port, 80);
 *   // Result: host="localhost", port=80 (default)
 *
 *   loopyNetParseAddr("[2001:db8::1]", host, 100, &port, 80);
 *   // Result: host="2001:db8::1", port=80 (default, no brackets)
 * @endcode
 *
 * @param addr        Input address string (required)
 * @param host        Output buffer for host/IP (without brackets for IPv6)
 * @param hostLen     Size of host buffer (must be > 0)
 * @param port        Output port number (set to defaultPort if not in string)
 * @param defaultPort Default port if none specified in address (1-65535)
 * @return true on success, false on parse error (malformed input)
 *
 * @note IPv6 addresses with brackets have brackets stripped from output.
 *       Bare IPv6 addresses (without port) work but cannot be parsed with
 *       a port number - use bracketed notation for those.
 *
 * @see loopyNetFormatAddr()
 * @see loopyNetAddrIsIPv6()
 */
bool loopyNetParseAddr(const char *addr, char *host, size_t hostLen, int *port,
                       int defaultPort);

/**
 * Format host and port into an address string.
 *
 * IPv6 addresses (containing ':') are automatically bracketed. Inverse of
 * loopyNetParseAddr(). Useful for logging and error messages.
 *
 * Examples:
 * @code
 *   // IPv6 is bracketed
 *   loopyNetFormatAddr(buf, 100, "::1", 9000);
 *   // Result: "[::1]:9000"
 *
 *   // IPv4 is not bracketed
 *   loopyNetFormatAddr(buf, 100, "127.0.0.1", 8080);
 *   // Result: "127.0.0.1:8080"
 *
 *   // Hostname is not bracketed
 *   loopyNetFormatAddr(buf, 100, "localhost", 80);
 *   // Result: "localhost:80"
 * @endcode
 *
 * @param buf     Output buffer
 * @param bufLen  Size of output buffer (should be at least 100 bytes)
 * @param ip      IP address or hostname (IPv6 must be unbracketed on input)
 * @param port    Port number (1-65535)
 * @return Number of characters written (excluding null terminator),
 *         or negative value if buffer too small
 *
 * @note Uses snprintf internally. If bufLen is too small, result is truncated.
 *       For IPv6, automatically detects ':' and adds brackets.
 *
 * @see loopyNetParseAddr()
 * @see loopyNetAddrIsIPv6()
 */
int loopyNetFormatAddr(char *buf, size_t bufLen, const char *ip, int port);

/**
 * Check if an address string appears to be IPv6.
 *
 * Simple heuristic: returns true if the address contains a ':' character.
 * Useful for deciding whether to use brackets when formatting addresses.
 *
 * @param addr Address string (may be NULL, hostname, IPv4, or IPv6)
 * @return true if address contains ':', false otherwise
 *
 * @note This is a heuristic and not foolproof. Use for formatting decisions,
 *       not for validation. Bare hostnames with colons (unusual but possible)
 *       would incorrectly return true.
 *
 * @see loopyNetFormatAddr()
 * @see loopyNetParseAddr()
 */
bool loopyNetAddrIsIPv6(const char *addr);

/**
 * Get the local address and port of a socket.
 *
 * Retrieves the address and port that the socket is bound to (local end).
 * Uses getsockname().
 *
 * @param sock Socket file descriptor
 * @param ip Output buffer for local address string
 * @param ipLen Size of ip buffer (at least INET6_ADDRSTRLEN = 46 bytes)
 * @param port Output pointer for local port (optional, may be NULL)
 * @return true on success, false on error
 *
 * @note On error, ip[0] is set to '?' if buffer is large enough. Useful for
 *       logging and debugging connection information.
 *
 * @see loopyNetPeerToString()
 * @see loopyNetFormatSock()
 */
bool loopyNetSockName(int sock, char *ip, size_t ipLen, int *port);

/**
 * Get the remote peer address and port of a socket.
 *
 * Retrieves the address and port of the connected peer (remote end).
 * Uses getpeername().
 *
 * @param sock Socket file descriptor
 * @param ip Output buffer for peer address string
 * @param ipLen Size of ip buffer (at least INET6_ADDRSTRLEN = 46 bytes)
 * @param port Output pointer for peer port (optional, may be NULL)
 * @return true on success, false on error (socket not connected or invalid)
 *
 * @note On Unix domain sockets, ip is set to "[unix]". On error, ip[0] is
 *       set to '?' if possible, and port is set to -1.
 *
 * @see loopyNetSockName()
 * @see loopyNetFormatPeer()
 */
bool loopyNetPeerToString(int sock, char *ip, size_t ipLen, int *port);

/**
 * Format the remote peer address as a single address string.
 *
 * Combines loopyNetPeerToString() and loopyNetFormatAddr(). Returns the
 * peer address as a formatted string suitable for logging.
 *
 * @param fd Socket file descriptor
 * @param buf Output buffer for formatted address
 * @param bufLen Size of buf
 * @return Number of characters written (same as snprintf)
 *
 * @note Useful for error messages and logging. Format is "[addr]:port" for
 *       IPv6, "addr:port" for IPv4 and hostnames.
 *
 * @see loopyNetFormatSock()
 * @see loopyNetFormatAddr()
 */
int loopyNetFormatPeer(int fd, char *buf, size_t bufLen);

/**
 * Format the local socket address as a single address string.
 *
 * Combines loopyNetSockName() and loopyNetFormatAddr(). Returns the local
 * address as a formatted string suitable for logging.
 *
 * @param fd Socket file descriptor
 * @param buf Output buffer for formatted address
 * @param bufLen Size of buf
 * @return Number of characters written (same as snprintf)
 *
 * @note Useful for logging which address a socket is bound to. Format is
 *       "[addr]:port" for IPv6, "addr:port" for IPv4 and hostnames.
 *
 * @see loopyNetFormatPeer()
 * @see loopyNetFormatAddr()
 */
int loopyNetFormatSock(int fd, char *buf, size_t bufLen);

/* Originally, before updating to modern style and standards:
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
