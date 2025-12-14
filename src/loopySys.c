/* loopySys - System utilities for loopy event loop
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

#include "loopyPlatform.h"

#include "../deps/datakit/src/datakit.h"
#include "loopySys.h"

#include <errno.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Platform-specific includes */
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#endif

#if defined(__linux__)
#include <sys/sysinfo.h>
#endif

#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/sysctl.h>
#endif

/* For getifaddrs */
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>

/* ====================================================================
 * Network Interfaces
 * ==================================================================== */

bool loopyInterfaceAddresses(loopyInterfaceAddress **addresses, size_t *count) {
    if (!addresses || !count) {
        return false;
    }

    *addresses = NULL;
    *count = 0;

    struct ifaddrs *ifaddrs = NULL;
    if (getifaddrs(&ifaddrs) == -1) {
        return false;
    }

    /* First pass: count addresses */
    size_t numAddrs = 0;
    for (struct ifaddrs *ifa = ifaddrs; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) {
            continue;
        }
        int family = ifa->ifa_addr->sa_family;
        if (family == AF_INET || family == AF_INET6) {
            numAddrs++;
        }
    }

    if (numAddrs == 0) {
        freeifaddrs(ifaddrs);
        return true; /* Success with zero addresses */
    }

    /* Allocate array */
    loopyInterfaceAddress *addrs = zcalloc(numAddrs, sizeof(*addrs));
    if (!addrs) {
        freeifaddrs(ifaddrs);
        return false;
    }

    /* Second pass: fill in addresses */
    size_t idx = 0;
    for (struct ifaddrs *ifa = ifaddrs; ifa && idx < numAddrs;
         ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) {
            continue;
        }

        int family = ifa->ifa_addr->sa_family;
        if (family != AF_INET && family != AF_INET6) {
            continue;
        }

        loopyInterfaceAddress *addr = &addrs[idx];

        /* Copy interface name */
        strncpy(addr->name, ifa->ifa_name, sizeof(addr->name) - 1);
        addr->name[sizeof(addr->name) - 1] = '\0';

        addr->family = family;
        addr->isInternal = (ifa->ifa_flags & IFF_LOOPBACK) != 0;

        /* Convert address to string */
        if (family == AF_INET) {
            struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
            inet_ntop(AF_INET, &sin->sin_addr, addr->address,
                      sizeof(addr->address));

            if (ifa->ifa_netmask) {
                struct sockaddr_in *mask =
                    (struct sockaddr_in *)ifa->ifa_netmask;
                inet_ntop(AF_INET, &mask->sin_addr, addr->netmask,
                          sizeof(addr->netmask));
            }
        } else { /* AF_INET6 */
            struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)ifa->ifa_addr;
            inet_ntop(AF_INET6, &sin6->sin6_addr, addr->address,
                      sizeof(addr->address));

            if (ifa->ifa_netmask) {
                struct sockaddr_in6 *mask =
                    (struct sockaddr_in6 *)ifa->ifa_netmask;
                inet_ntop(AF_INET6, &mask->sin6_addr, addr->netmask,
                          sizeof(addr->netmask));
            }
        }

        idx++;
    }

    freeifaddrs(ifaddrs);

    *addresses = addrs;
    *count = idx;
    return true;
}

void loopyInterfaceAddressesFree(loopyInterfaceAddress *addresses) {
    zfree(addresses);
}

/* ====================================================================
 * System Information
 * ==================================================================== */

int loopyAvailableParallelism(void) {
#if defined(__linux__)
    /* Linux: use get_nprocs() or sysconf */
    int nprocs_linux = get_nprocs();
    if (nprocs_linux > 0) {
        return nprocs_linux;
    }
#endif

#if defined(_SC_NPROCESSORS_ONLN)
    /* POSIX: sysconf */
    long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
    if (nprocs > 0) {
        return (int)nprocs;
    }
#endif

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) ||       \
    defined(__OpenBSD__)
    /* BSD/macOS: sysctl */
    int mib[2] = {CTL_HW, HW_NCPU};
    int ncpu = 0;
    size_t len = sizeof(ncpu);
    if (sysctl(mib, 2, &ncpu, &len, NULL, 0) == 0 && ncpu > 0) {
        return ncpu;
    }
#endif

    /* Fallback: assume at least 1 CPU */
    return 1;
}

ssize_t loopyExePath(char *buf, size_t bufLen) {
    if (!buf || bufLen == 0) {
        return -1;
    }

#if defined(__linux__)
    /* Linux: read /proc/self/exe symlink */
    ssize_t len = readlink("/proc/self/exe", buf, bufLen - 1);
    if (len > 0) {
        buf[len] = '\0';
        return len;
    }
    return -1;

#elif defined(__APPLE__)
    /* macOS: _NSGetExecutablePath */
    uint32_t size = (uint32_t)bufLen;
    if (_NSGetExecutablePath(buf, &size) == 0) {
        return (ssize_t)strlen(buf);
    }
    /* Buffer too small, return required size */
    return (ssize_t)size;

#elif defined(__FreeBSD__)
    /* FreeBSD: sysctl */
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
    size_t size = bufLen;
    if (sysctl(mib, 4, buf, &size, NULL, 0) == 0) {
        return (ssize_t)(size - 1); /* Exclude null terminator */
    }
    return -1;

#elif defined(__NetBSD__)
    /* NetBSD: read /proc/curproc/exe */
    ssize_t len = readlink("/proc/curproc/exe", buf, bufLen - 1);
    if (len > 0) {
        buf[len] = '\0';
        return len;
    }
    return -1;

#else
    /* Unsupported platform */
    (void)buf;
    (void)bufLen;
    errno = ENOTSUP;
    return -1;
#endif
}

/* ====================================================================
 * System Paths
 * ==================================================================== */

ssize_t loopyHomedir(char *buf, size_t bufLen) {
    if (!buf || bufLen == 0) {
        errno = EINVAL;
        return -1;
    }

    const char *home = NULL;

    /* First, try $HOME environment variable */
    home = getenv("HOME");
    if (home && home[0] != '\0') {
        size_t len = strlen(home);

        /* Remove trailing slash if present (unless it's just "/") */
        while (len > 1 && home[len - 1] == '/') {
            len--;
        }

        if (len >= bufLen) {
            /* Buffer too small, return required size */
            return (ssize_t)(len + 1);
        }

        memcpy(buf, home, len);
        buf[len] = '\0';
        return (ssize_t)len;
    }

    /* Fallback: password database lookup */
    const struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir && pw->pw_dir[0] != '\0') {
        size_t len = strlen(pw->pw_dir);

        /* Remove trailing slash if present (unless it's just "/") */
        while (len > 1 && pw->pw_dir[len - 1] == '/') {
            len--;
        }

        if (len >= bufLen) {
            return (ssize_t)(len + 1);
        }

        memcpy(buf, pw->pw_dir, len);
        buf[len] = '\0';
        return (ssize_t)len;
    }

    errno = ENOENT;
    return -1;
}

/**
 * Helper to check if a path is a valid directory.
 */
static bool isValidDir(const char *path) {
    if (!path || path[0] == '\0') {
        return false;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }

    return S_ISDIR(st.st_mode);
}

ssize_t loopyTmpdir(char *buf, size_t bufLen) {
    if (!buf || bufLen == 0) {
        errno = EINVAL;
        return -1;
    }

    const char *tmpdir = NULL;

    /* Check environment variables in order of preference */
    static const char *envVars[] = {"TMPDIR", "TMP", "TEMP", NULL};

    for (const char **var = envVars; *var; var++) {
        tmpdir = getenv(*var);
        if (tmpdir && tmpdir[0] != '\0' && isValidDir(tmpdir)) {
            break;
        }
        tmpdir = NULL;
    }

    /* Fallback to /tmp */
    if (!tmpdir) {
        tmpdir = "/tmp";
        if (!isValidDir(tmpdir)) {
            errno = ENOENT;
            return -1;
        }
    }

    size_t len = strlen(tmpdir);

    /* Remove trailing slash if present (unless it's just "/") */
    while (len > 1 && tmpdir[len - 1] == '/') {
        len--;
    }

    if (len >= bufLen) {
        return (ssize_t)(len + 1);
    }

    memcpy(buf, tmpdir, len);
    buf[len] = '\0';
    return (ssize_t)len;
}

ssize_t loopyCwd(char *buf, size_t bufLen) {
    if (!buf || bufLen == 0) {
        errno = EINVAL;
        return -1;
    }

    if (getcwd(buf, bufLen) == NULL) {
        if (errno == ERANGE) {
            /* Buffer too small - try to get required size
             * Note: getcwd(NULL, 0) allocates via malloc, so we use
             * system free() here, not zfree() */
            char *tmp = getcwd(NULL, 0);
            if (tmp) {
                size_t len = strlen(tmp) + 1;
                /* Use libc free since getcwd uses libc malloc.
                 * Suppress deprecation warning from datakit's free() macro. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                free(tmp);
#pragma clang diagnostic pop
                return (ssize_t)len;
            }
        }
        return -1;
    }

    return (ssize_t)strlen(buf);
}

bool loopyChdir(const char *path) {
    if (!path) {
        errno = EINVAL;
        return false;
    }

    return chdir(path) == 0;
}

ssize_t loopyHostname(char *buf, size_t bufLen) {
    if (!buf || bufLen == 0) {
        errno = EINVAL;
        return -1;
    }

    if (gethostname(buf, bufLen) != 0) {
        return -1;
    }

    /* Ensure null termination (POSIX doesn't guarantee it if truncated) */
    buf[bufLen - 1] = '\0';

    return (ssize_t)strlen(buf);
}
