/* loopySys - System utilities for loopy event loop
 *
 * Cross-platform system information: interfaces, CPU count, paths.
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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* ====================================================================
 * Network Interfaces
 * ==================================================================== */

/**
 * Network interface address information.
 */
typedef struct loopyInterfaceAddress {
    char name[32];    /* Interface name (e.g., "eth0", "en0") */
    char address[64]; /* IP address string */
    char netmask[64]; /* Netmask string */
    int family;       /* AF_INET or AF_INET6 */
    bool isInternal;  /* true for loopback interfaces */
} loopyInterfaceAddress;

/**
 * Get all network interface addresses.
 *
 * @param addresses OUT: array of addresses (caller frees with
 * loopyInterfaceAddressesFree)
 * @param count     OUT: number of addresses returned
 * @return true on success, false on error
 *
 * Example:
 *   loopyInterfaceAddress *addrs;
 *   size_t count;
 *   if (loopyInterfaceAddresses(&addrs, &count)) {
 *       for (size_t i = 0; i < count; i++) {
 *           printf("%s: %s\n", addrs[i].name, addrs[i].address);
 *       }
 *       loopyInterfaceAddressesFree(addrs);
 *   }
 */
bool loopyInterfaceAddresses(loopyInterfaceAddress **addresses, size_t *count);

/**
 * Free interface addresses returned by loopyInterfaceAddresses().
 *
 * @param addresses Array to free (may be NULL)
 */
void loopyInterfaceAddressesFree(loopyInterfaceAddress *addresses);

/* ====================================================================
 * System Information
 * ==================================================================== */

/**
 * Get the number of available CPU cores/threads.
 *
 * Returns the number of logical CPUs available for parallel work.
 * This is useful for sizing thread pools.
 *
 * @return Number of CPUs, or 1 if detection fails
 */
int loopyAvailableParallelism(void);

/**
 * Get the path to the current executable.
 *
 * @param buf    Buffer to store path
 * @param bufLen Buffer size
 * @return Number of bytes written (not including null terminator),
 *         or -1 on error, or required size if buffer too small
 */
ssize_t loopyExePath(char *buf, size_t bufLen);

/* ====================================================================
 * System Paths
 * ==================================================================== */

/**
 * Get the user's home directory.
 *
 * Resolution order:
 *   1. $HOME environment variable
 *   2. Password database (getpwuid)
 *
 * @param buf    Buffer to store path
 * @param bufLen Buffer size
 * @return Number of bytes written (not including null terminator),
 *         or -1 on error. If buffer is too small, returns required size.
 *
 * Example:
 *   char home[256];
 *   if (loopyHomedir(home, sizeof(home)) > 0) {
 *       printf("Home: %s\n", home);
 *   }
 */
ssize_t loopyHomedir(char *buf, size_t bufLen);

/**
 * Get the system temporary directory.
 *
 * Resolution order:
 *   1. $TMPDIR environment variable
 *   2. $TMP environment variable
 *   3. $TEMP environment variable
 *   4. /tmp (fallback)
 *
 * The returned path is guaranteed to exist and be a directory.
 *
 * @param buf    Buffer to store path
 * @param bufLen Buffer size
 * @return Number of bytes written (not including null terminator),
 *         or -1 on error. If buffer is too small, returns required size.
 *
 * Example:
 *   char tmp[256];
 *   if (loopyTmpdir(tmp, sizeof(tmp)) > 0) {
 *       printf("Temp: %s\n", tmp);
 *   }
 */
ssize_t loopyTmpdir(char *buf, size_t bufLen);

/**
 * Get the current working directory.
 *
 * @param buf    Buffer to store path
 * @param bufLen Buffer size
 * @return Number of bytes written (not including null terminator),
 *         or -1 on error. If buffer is too small, returns required size.
 */
ssize_t loopyCwd(char *buf, size_t bufLen);

/**
 * Change the current working directory.
 *
 * @param path Directory path to change to
 * @return true on success, false on error (check errno)
 */
bool loopyChdir(const char *path);

/**
 * Get the system hostname.
 *
 * @param buf    Buffer to store hostname
 * @param bufLen Buffer size
 * @return Number of bytes written (not including null terminator),
 *         or -1 on error.
 */
ssize_t loopyHostname(char *buf, size_t bufLen);
