/* loopyUDP - UDP networking for loopy event loop
 *
 * Connectionless datagram networking for DNS, QUIC, gaming, streaming
 * protocols.
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

#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

/* ====================================================================
 * Types
 * ==================================================================== */

/**
 * Opaque UDP handle structure.
 */
typedef struct loopyUDP loopyUDP;

/**
 * Bind flags for UDP socket configuration.
 */
typedef enum loopyUDPFlags {
    LOOPY_UDP_IPV6ONLY = 0x01,  /* Disable dual-stack (IPv6 only) */
    LOOPY_UDP_REUSEADDR = 0x02, /* SO_REUSEADDR */
    LOOPY_UDP_REUSEPORT = 0x04, /* SO_REUSEPORT (Linux 3.9+) */
} loopyUDPFlags;

/**
 * Receive callback - called when data is received.
 *
 * @param udp       The UDP handle
 * @param nread     Number of bytes received, or -1 on error
 * @param data      Pointer to received data (valid only during callback)
 * @param addr      Source address (valid only during callback)
 * @param addrLen   Length of source address
 * @param userData  User data from loopyUDPRecvStart()
 *
 * Thread Safety: Always called on the event loop thread.
 */
typedef void loopyUDPRecvCallback(loopyUDP *udp, ssize_t nread,
                                  const void *data, const struct sockaddr *addr,
                                  socklen_t addrLen, void *userData);

/**
 * Send callback - called when send completes.
 *
 * @param udp      The UDP handle
 * @param status   0 on success, negative on error
 * @param userData User data from loopyUDPSend()
 *
 * Thread Safety: Always called on the event loop thread.
 */
typedef void loopyUDPSendCallback(loopyUDP *udp, int status, void *userData);

/* ====================================================================
 * Lifecycle
 * ==================================================================== */

/**
 * Create a new UDP handle.
 *
 * @param loop The event loop (must not be NULL)
 * @return New UDP handle, or NULL on error
 *
 * Thread Safety: Must be called from the event loop thread.
 */
loopyUDP *loopyUDPNew(loopyLoop *loop);

/**
 * Free a UDP handle.
 *
 * Stops any active receiving, closes the socket, and frees resources.
 * Safe to call with NULL.
 *
 * @param udp The handle to free, or NULL
 *
 * Thread Safety: Must be called from the event loop thread.
 */
void loopyUDPFree(loopyUDP *udp);

/* ====================================================================
 * Binding
 * ==================================================================== */

/**
 * Bind to an IPv4 address and port.
 *
 * @param udp   The UDP handle
 * @param addr  IPv4 address to bind to (NULL or "" for INADDR_ANY)
 * @param port  Port to bind to (0 for ephemeral port)
 * @param flags Bind flags (LOOPY_UDP_REUSEADDR, etc.)
 * @return true on success, false on error
 */
bool loopyUDPBind(loopyUDP *udp, const char *addr, int port, unsigned flags);

/**
 * Bind to an IPv6 address and port.
 *
 * @param udp   The UDP handle
 * @param addr  IPv6 address to bind to (NULL or "" for in6addr_any)
 * @param port  Port to bind to (0 for ephemeral port)
 * @param flags Bind flags (LOOPY_UDP_IPV6ONLY, LOOPY_UDP_REUSEADDR, etc.)
 * @return true on success, false on error
 */
bool loopyUDPBind6(loopyUDP *udp, const char *addr, int port, unsigned flags);

/* ====================================================================
 * Connect (Optional - for default destination)
 * ==================================================================== */

/**
 * Connect to a destination address.
 *
 * After connecting, you can use loopyUDPSendConnected() for efficiency.
 * Also causes the socket to only receive datagrams from the connected address.
 *
 * @param udp  The UDP handle
 * @param addr Destination address
 * @param port Destination port
 * @return true on success, false on error
 */
bool loopyUDPConnect(loopyUDP *udp, const char *addr, int port);

/**
 * Disconnect from the destination address.
 *
 * Restores the socket to unconnected state.
 *
 * @param udp The UDP handle
 */
void loopyUDPDisconnect(loopyUDP *udp);

/**
 * Check if UDP handle is connected.
 *
 * @param udp The UDP handle
 * @return true if connected
 */
bool loopyUDPIsConnected(const loopyUDP *udp);

/* ====================================================================
 * Receiving
 * ==================================================================== */

/**
 * Start receiving datagrams.
 *
 * The callback will be invoked each time a datagram is received.
 *
 * @param udp      The UDP handle
 * @param cb       Receive callback (must not be NULL)
 * @param userData User data passed to callback
 * @return true on success, false on error
 */
bool loopyUDPRecvStart(loopyUDP *udp, loopyUDPRecvCallback *cb, void *userData);

/**
 * Stop receiving datagrams.
 *
 * @param udp The UDP handle
 *
 * @note Any pending receive callbacks that have not yet been processed
 *       by the event loop will be cancelled. Callbacks for packets already
 *       received but not yet delivered may still fire.
 */
void loopyUDPRecvStop(loopyUDP *udp);

/**
 * Check if receiving is active.
 *
 * @param udp The UDP handle
 * @return true if receiving
 */
bool loopyUDPIsReceiving(const loopyUDP *udp);

/* ====================================================================
 * Sending
 * ==================================================================== */

/**
 * Send a datagram to a specific address.
 *
 * The send is asynchronous - the callback is invoked when complete.
 * The data buffer must remain valid until the callback is invoked.
 *
 * @param udp      The UDP handle
 * @param addr     Destination address
 * @param port     Destination port
 * @param data     Data to send
 * @param len      Length of data
 * @param cb       Completion callback (may be NULL)
 * @param userData User data for callback
 * @return true if send was queued, false on error
 */
bool loopyUDPSend(loopyUDP *udp, const char *addr, int port, const void *data,
                  size_t len, loopyUDPSendCallback *cb, void *userData);

/**
 * Send a datagram to the connected address.
 *
 * More efficient than loopyUDPSend() when sending to the same destination.
 * Requires prior call to loopyUDPConnect().
 *
 * @param udp      The UDP handle
 * @param data     Data to send
 * @param len      Length of data
 * @param cb       Completion callback (may be NULL)
 * @param userData User data for callback
 * @return true if send was queued, false on error
 */
bool loopyUDPSendConnected(loopyUDP *udp, const void *data, size_t len,
                           loopyUDPSendCallback *cb, void *userData);

/**
 * Try to send a datagram immediately (non-blocking).
 *
 * If the send would block, returns -1 with errno = EAGAIN.
 *
 * @param udp  The UDP handle
 * @param addr Destination address
 * @param port Destination port
 * @param data Data to send
 * @param len  Length of data
 * @return Number of bytes sent, or -1 on error
 */
ssize_t loopyUDPTrySend(loopyUDP *udp, const char *addr, int port,
                        const void *data, size_t len);

/* ====================================================================
 * Configuration
 * ==================================================================== */

/**
 * Enable/disable broadcast.
 *
 * Allows sending to broadcast addresses (e.g., 255.255.255.255).
 *
 * @param udp    The UDP handle
 * @param enable true to enable, false to disable
 * @return true on success
 *
 * @note Broadcast is typically platform-specific. Full broadcast support
 *       is most reliable on Linux. Some systems may require special
 *       privileges to enable broadcast. On macOS/BSD, behavior is consistent.
 */
bool loopyUDPSetBroadcast(loopyUDP *udp, bool enable);

/**
 * Set time-to-live (TTL).
 *
 * @param udp The UDP handle
 * @param ttl TTL value (1-255)
 * @return true on success
 */
bool loopyUDPSetTTL(loopyUDP *udp, int ttl);

/**
 * Set multicast TTL.
 *
 * @param udp The UDP handle
 * @param ttl TTL value (1-255)
 * @return true on success
 */
bool loopyUDPSetMulticastTTL(loopyUDP *udp, int ttl);

/**
 * Enable/disable multicast loopback.
 *
 * When enabled, multicast packets sent by this socket will also be
 * received on this socket if it's a member of the group.
 *
 * @param udp    The UDP handle
 * @param enable true to enable loopback, false to disable
 * @return true on success
 *
 * @note Default behavior is typically enabled (1). Setting to 0 prevents
 *       receiving your own multicast packets. Behavior is consistent across
 *       Linux, macOS, and BSD.
 */
bool loopyUDPSetMulticastLoop(loopyUDP *udp, bool enable);

/**
 * Join a multicast group.
 *
 * @param udp   The UDP handle
 * @param group Multicast group address
 * @param iface Interface to join on (NULL for default)
 * @return true on success
 */
bool loopyUDPJoinMulticast(loopyUDP *udp, const char *group, const char *iface);

/**
 * Leave a multicast group.
 *
 * @param udp   The UDP handle
 * @param group Multicast group address
 * @param iface Interface to leave on (NULL for default)
 * @return true on success
 */
bool loopyUDPLeaveMulticast(loopyUDP *udp, const char *group,
                            const char *iface);

/* ====================================================================
 * Information
 * ==================================================================== */

/**
 * Get the local address the socket is bound to.
 *
 * Retrieves the local address and port to which the socket is bound.
 *
 * @param udp     The UDP handle
 * @param addr    Buffer to store address string (may be NULL)
 * @param addrLen Size of address buffer
 * @param port    Pointer to store port (may be NULL)
 * @return true on success, false if socket not bound or error
 *
 * @note addrLen should be at least 46 bytes (INET6_ADDRSTRLEN).
 *       If addrLen is too small, the address will be truncated.
 *       This function uses inet_ntop() internally.
 */
bool loopyUDPGetSockName(loopyUDP *udp, char *addr, size_t addrLen, int *port);

/**
 * Get the underlying socket file descriptor.
 *
 * @param udp The UDP handle
 * @return Socket fd, or -1 if not created
 */
int loopyUDPGetFd(const loopyUDP *udp);

/**
 * Get the event loop associated with this UDP handle.
 *
 * @param udp The UDP handle
 * @return The event loop, or NULL if udp is NULL
 */
loopyLoop *loopyUDPGetLoop(const loopyUDP *udp);

/**
 * Get number of pending send requests.
 *
 * @param udp The UDP handle
 * @return Number of pending sends
 */
size_t loopyUDPSendQueueCount(const loopyUDP *udp);

/**
 * Get the last error message.
 *
 * @param udp The UDP handle
 * @return Error message, or empty string if no error
 */
const char *loopyUDPGetError(const loopyUDP *udp);

/**
 * Get user data from UDP handle.
 *
 * @param udp The UDP handle
 * @return User data pointer, or NULL
 */
void *loopyUDPGetData(const loopyUDP *udp);

/**
 * Set user data on UDP handle.
 *
 * @param udp The UDP handle
 * @param data User data pointer
 */
void loopyUDPSetData(loopyUDP *udp, void *data);

/* ====================================================================
 * Batch Operations (High Performance)
 *
 * These functions use recvmmsg/sendmmsg on Linux for significant
 * performance gains (10-50x throughput) in high-volume UDP scenarios.
 * On other platforms, they fall back to looping recvfrom/sendto.
 * ==================================================================== */

/**
 * Maximum messages per batch operation.
 * Larger values reduce syscall overhead but increase memory usage.
 */
#define LOOPY_UDP_BATCH_MAX 64

/**
 * Message structure for batch send/receive operations.
 *
 * For receiving:
 *   - Set data to point to your buffer
 *   - Set len to buffer capacity
 *   - After recv, bytesTransferred contains actual bytes received
 *   - addr/addrLen contain the source address
 *
 * For sending:
 *   - Set data to point to data to send
 *   - Set len to data length
 *   - Set addr/addrLen to destination address
 *   - After send, bytesTransferred contains actual bytes sent
 */
typedef struct loopyUDPMessage {
    void *data;                   /* Data buffer */
    size_t len;                   /* Buffer size (recv) or data length (send) */
    size_t bytesTransferred;      /* Actual bytes transferred (output) */
    struct sockaddr_storage addr; /* Remote address */
    socklen_t addrLen;            /* Address length */
} loopyUDPMessage;

/**
 * Receive multiple datagrams in a single operation.
 *
 * Uses recvmmsg() on Linux for optimal performance, falls back to
 * looping recvfrom() on other platforms.
 *
 * @param udp      The UDP handle
 * @param msgs     Array of message structures (caller allocates)
 * @param nmsg     Maximum messages to receive (1 to LOOPY_UDP_BATCH_MAX)
 * @return Number of messages received (0 if would block), or -1 on error
 *
 * Example:
 *   loopyUDPMessage msgs[32];
 *   char buffers[32][1500];
 *   for (int i = 0; i < 32; i++) {
 *       msgs[i].data = buffers[i];
 *       msgs[i].len = sizeof(buffers[i]);
 *   }
 *   int n = loopyUDPRecvMulti(udp, msgs, 32);
 *   for (int i = 0; i < n; i++) {
 *       process_packet(msgs[i].data, msgs[i].bytesTransferred);
 *   }
 */
int loopyUDPRecvMulti(loopyUDP *udp, loopyUDPMessage *msgs, int nmsg);

/**
 * Send multiple datagrams in a single operation.
 *
 * Uses sendmmsg() on Linux for optimal performance, falls back to
 * looping sendto() on other platforms.
 *
 * @param udp      The UDP handle
 * @param msgs     Array of message structures with data and destinations
 * @param nmsg     Number of messages to send (1 to LOOPY_UDP_BATCH_MAX)
 * @return Number of messages sent (may be less than nmsg), or -1 on error
 *
 * Example:
 *   loopyUDPMessage msgs[10];
 *   for (int i = 0; i < 10; i++) {
 *       msgs[i].data = packets[i].data;
 *       msgs[i].len = packets[i].len;
 *       // Set destination address
 *       struct sockaddr_in *sin = (struct sockaddr_in *)&msgs[i].addr;
 *       sin->sin_family = AF_INET;
 *       sin->sin_port = htons(port);
 *       inet_pton(AF_INET, "192.168.1.1", &sin->sin_addr);
 *       msgs[i].addrLen = sizeof(struct sockaddr_in);
 *   }
 *   int n = loopyUDPSendMulti(udp, msgs, 10);
 */
int loopyUDPSendMulti(loopyUDP *udp, loopyUDPMessage *msgs, int nmsg);

/**
 * Send multiple datagrams to the connected address.
 *
 * More efficient than loopyUDPSendMulti() when sending to the same
 * destination. Requires prior call to loopyUDPConnect().
 *
 * @param udp      The UDP handle (must be connected)
 * @param msgs     Array of message structures (addr fields ignored)
 * @param nmsg     Number of messages to send
 * @return Number of messages sent, or -1 on error
 */
int loopyUDPSendMultiConnected(loopyUDP *udp, loopyUDPMessage *msgs, int nmsg);

/**
 * Check if batch operations use native syscalls.
 *
 * @return true if recvmmsg/sendmmsg are available (Linux), false otherwise
 *
 * @note On Linux (2.6.33+), returns true and uses native recvmmsg/sendmmsg
 *       which can process 64 messages per syscall. On other platforms
 *       (macOS, BSD), returns false and falls back to looping recvfrom/sendto.
 *       Native batch operations can improve throughput by 5-10x for
 *       high packet rate scenarios.
 */
bool loopyUDPHasNativeBatch(void);

/* ====================================================================
 * Advanced UDP Features (Linux 4.18+)
 *
 * GSO (Generic Segmentation Offload), GRO (Generic Receive Offload),
 * and PMTU (Path MTU Discovery) for high-performance UDP applications.
 * ==================================================================== */

/**
 * UDP Segmentation Offload (GSO) configuration.
 *
 * GSO allows sending large UDP payloads that the kernel/NIC automatically
 * segments into MTU-sized packets. This reduces CPU usage and syscall
 * overhead for high-throughput UDP applications.
 *
 * Benefits:
 *  - 10-40% CPU reduction for bulk UDP sends
 *  - Reduced syscall overhead (one send for many packets)
 *  - Hardware offload when NIC supports it
 *
 * Available on Linux 4.18+ with UDP_SEGMENT socket option.
 *
 * Typical segment sizes:
 *  - IPv4: 1472 bytes (1500 MTU - 20 IP - 8 UDP)
 *  - IPv6: 1452 bytes (1500 MTU - 40 IP - 8 UDP)
 *  - Jumbo: 8948 bytes (9000 MTU - 40 IP - 8 UDP - 4 VLAN)
 */
typedef struct loopyUDPGSOConfig {
    uint16_t segmentSize; /* Segment size in bytes (typically MTU - headers) */
    bool enabled;         /* Enable GSO for this socket */
} loopyUDPGSOConfig;

/**
 * UDP Generic Receive Offload (GRO) configuration.
 *
 * GRO allows the kernel to coalesce multiple small packets into larger
 * buffers before delivering to userspace. This reduces CPU usage and
 * syscall overhead for high packet-rate UDP applications.
 *
 * Benefits:
 *  - 10-30% CPU reduction for high packet rates
 *  - Reduced number of recv calls
 *  - Better cache efficiency
 *
 * Available on Linux 5.0+ with UDP_GRO socket option.
 */
typedef struct loopyUDPGROConfig {
    bool enabled; /* Enable GRO for this socket */
} loopyUDPGROConfig;

/**
 * Path MTU Discovery modes.
 *
 * Controls IP fragmentation behavior and PMTU discovery.
 */
typedef enum loopyUDPPMTUMode {
    LOOPY_UDP_PMTU_UNSPEC = 0, /* Use system default */
    LOOPY_UDP_PMTU_DISABLED,   /* Disable PMTU discovery, allow fragmentation */
    LOOPY_UDP_PMTU_WANT,       /* Want PMTU but allow fragmentation if needed */
    LOOPY_UDP_PMTU_DO,    /* Do PMTU discovery, set DF bit, fail if too large */
    LOOPY_UDP_PMTU_PROBE, /* Probe for PMTU (always set DF bit) */
} loopyUDPPMTUMode;

/**
 * Enable UDP GSO (Generic Segmentation Offload).
 *
 * After enabling, large payloads sent via loopyUDPSendGSO() will be
 * automatically segmented by the kernel into MTU-sized packets.
 *
 * @param udp    The UDP handle
 * @param config GSO configuration (NULL to disable)
 * @return true if GSO enabled/disabled successfully, false if unsupported
 *
 * Example:
 *   loopyUDPGSOConfig gso = {
 *       .segmentSize = 1472,  // IPv4 typical
 *       .enabled = true
 *   };
 *   if (loopyUDPSetGSO(udp, &gso)) {
 *       // Can now use loopyUDPSendGSO() for large payloads
 *   }
 */
bool loopyUDPSetGSO(loopyUDP *udp, const loopyUDPGSOConfig *config);

/**
 * Send large payload with GSO - kernel segments automatically.
 *
 * The kernel will split the payload into segments of gso.segmentSize bytes.
 * Requires prior call to loopyUDPSetGSO() with enabled=true.
 *
 * @param udp      The UDP handle (must have GSO enabled)
 * @param addr     Destination address
 * @param port     Destination port
 * @param data     Large payload data (can exceed MTU)
 * @param len      Payload length
 * @param cb       Completion callback (may be NULL)
 * @param userData User data for callback
 * @return true if send queued, false on error or GSO not enabled
 *
 * Example - send 10KB in one syscall:
 *   char data[10000];
 *   loopyUDPSendGSO(udp, "192.168.1.100", 5000, data, sizeof(data), NULL,
 * NULL);
 *   // Kernel sends ~7 packets of 1472 bytes each
 */
bool loopyUDPSendGSO(loopyUDP *udp, const char *addr, int port,
                     const void *data, size_t len, loopyUDPSendCallback *cb,
                     void *userData);

/**
 * Send large payload with GSO to connected address.
 *
 * More efficient than loopyUDPSendGSO() when sending to same destination.
 * Requires prior calls to loopyUDPConnect() and loopyUDPSetGSO().
 *
 * @param udp      The UDP handle (must be connected and have GSO enabled)
 * @param data     Large payload data
 * @param len      Payload length
 * @param cb       Completion callback (may be NULL)
 * @param userData User data for callback
 * @return true if send queued, false on error
 */
bool loopyUDPSendGSOConnected(loopyUDP *udp, const void *data, size_t len,
                              loopyUDPSendCallback *cb, void *userData);

/**
 * Enable UDP GRO (Generic Receive Offload).
 *
 * After enabling, the kernel will coalesce multiple small packets into
 * larger buffers when possible, reducing receive overhead.
 *
 * @param udp    The UDP handle
 * @param config GRO configuration (NULL to disable)
 * @return true if GRO enabled/disabled successfully, false if unsupported
 */
bool loopyUDPSetGRO(loopyUDP *udp, const loopyUDPGROConfig *config);

/**
 * Set Path MTU Discovery mode.
 *
 * Controls whether the kernel tries to discover the path MTU and how it
 * handles oversized packets.
 *
 * @param udp  The UDP handle
 * @param mode PMTU discovery mode
 * @return true on success, false on error
 *
 * Example:
 *   // Ensure packets aren't fragmented
 *   loopyUDPSetPMTUMode(udp, LOOPY_UDP_PMTU_DO);
 */
bool loopyUDPSetPMTUMode(loopyUDP *udp, loopyUDPPMTUMode mode);

/**
 * Get current PMTU for a connected UDP socket.
 *
 * Returns the maximum transmission unit for the path to the connected
 * peer. Requires the socket to be connected and PMTU discovery enabled.
 *
 * @param udp UDP socket (must be connected)
 * @return PMTU in bytes, or 0 if unknown/not connected
 *
 * Example:
 *   loopyUDPConnect(udp, "192.168.1.100", 5000);
 *   loopyUDPSetPMTUMode(udp, LOOPY_UDP_PMTU_DO);
 *   int mtu = loopyUDPGetPMTU(udp);
 *   printf("Path MTU: %d bytes\n", mtu);
 */
int loopyUDPGetPMTU(const loopyUDP *udp);

/**
 * Check if UDP GSO is available on this platform.
 *
 * @return true if GSO supported, false otherwise
 */
bool loopyUDPHasGSO(void);

/**
 * Check if UDP GRO is available on this platform.
 *
 * @return true if GRO supported, false otherwise
 */
bool loopyUDPHasGRO(void);
