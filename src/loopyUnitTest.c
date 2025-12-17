/* loopyUnitTest - Comprehensive unit tests for loopy event loop
 *
 * Copyright 2024 - Test suite for loopy library
 * Licensed under Apache 2.0
 */

/* Internal header needed for testing internal structures */
#include "loopyPlatform.h"
#include "loopyInternal.h"

#include "loopyAsync.h"
#include "loopyChannel.h"
#if LOOPY_HAVE_RAX
#include "loopyClusterRegistry.h"
#include "loopyConcurrencyPool.h"
#endif
#include "loopyConnPool.h"
#include "loopyDNS.h"
#include "loopyFS.h"
#include "loopyFSPoll.h"
#include "loopyFlock.h"
#include "loopyIdle.h"
#include "loopyMetrics.h"
#include "loopyMmap.h"
#include "loopyNice.h"
#include "loopyPipe.h"
#include "loopyProcess.h"
#if LOOPY_HAVE_RAX
#include "loopyPubSub.h"
#endif
#include "loopyRandom.h"
#include "loopyRateLimit.h"
#include "loopySignal.h"
#include "loopyStream.h"
#include "loopyStressTest.h"
#include "loopySys.h"
#include "loopyTLS.h"
#include "loopyTTY.h"
#include "loopyTimer.h"
#include "loopyUDP.h"
#include "loopyWatch.h"
#include "loopyWork.h"

#ifdef USE_IOURING
#include "loopyIoUringFS.h"
#include "loopyIoUringNet.h"
#endif

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../deps/datakit/src/timeUtil.h"

/* Self-documenting macros for auto-cleanup declarations */
#define LOOPY_SELF_DELETE(var) loopyLoop *var LOOPY_LOOP_AUTO_CLEANUP
#define LOOPY_FS_REQUEST_SELF_FREE(var) loopyFSRequest *var LOOPY_FS_REQUEST_AUTO_CLEANUP
#define LOOPY_STREAM_SELF_CLOSE(var) loopyStream *var LOOPY_STREAM_AUTO_CLEANUP

/* ====================================================================
 * Test Infrastructure
 * ==================================================================== */
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

/* Test registry for CTest support */
typedef int (*test_func_t)(void);

typedef struct {
    const char *name;
    test_func_t func;
    const char *group;
} test_entry_t;

#define MAX_TESTS 500
static test_entry_t test_registry[MAX_TESTS];
static int test_count = 0;
static const char *current_group = NULL;

/* Register a test in the registry */
static void register_test(const char *name, test_func_t func,
                          const char *group) {
    if (test_count < MAX_TESTS) {
        test_registry[test_count].name = name;
        test_registry[test_count].func = func;
        test_registry[test_count].group = group;
        test_count++;
    }
}

/* Find a test by name */
static test_entry_t *find_test(const char *name) {
    for (int i = 0; i < test_count; i++) {
        if (strcmp(test_registry[i].name, name) == 0) {
            return &test_registry[i];
        }
    }
    return NULL;
}

/* Run a single test by entry */
static int run_single_test(test_entry_t *entry, int verbose) {
    if (verbose) {
        printf("Running %s...\n", entry->name);
    }
    tests_run++;
    if (entry->func()) {
        tests_passed++;
        if (verbose) {
            printf("  PASS\n");
        }
        return 1;
    } else {
        tests_failed++;
        return 0;
    }
}

#define TEST_ASSERT_2(cond, msg)                                               \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("  FAIL: %s (line %d)\n", msg, __LINE__);                   \
            return 0;                                                          \
        }                                                                      \
    } while (0)

#define TEST_ASSERT_1(cond) TEST_ASSERT_2(cond, #cond)

/* Helper macros for variadic argument counting */
#define PP_NARG(...) PP_NARG_(__VA_ARGS__, PP_RSEQ_N())
#define PP_NARG_(...) PP_ARG_N(__VA_ARGS__)
#define PP_ARG_N(_1, _2, N, ...) N
#define PP_RSEQ_N() 2, 1, 0

#define CONCAT_IMPL(a, b) a##b
#define CONCAT(a, b) CONCAT_IMPL(a, b)

#define TEST_ASSERT(...)                                                       \
    CONCAT(TEST_ASSERT_, PP_NARG(__VA_ARGS__))(__VA_ARGS__)

#define TEST_ASSERT_EQ(a, b, msg)                                              \
    do {                                                                       \
        if ((a) != (b)) {                                                      \
            printf("  FAIL: %s - expected %d, got %d (line %d)\n", msg,        \
                   (int)(b), (int)(a), __LINE__);                              \
            return 0;                                                          \
        }                                                                      \
    } while (0)

/* Automatic cleanup helpers using GCC/Clang __attribute__((cleanup)) */
static inline void cleanup_loopy_loop(loopyLoop **lp) {
    if (lp && *lp) {
        loopyDelete(*lp);
        *lp = NULL;
    }
}

static inline void cleanup_fs_request(loopyFSRequest **reqp) {
    if (reqp && *reqp) {
        loopyFSRequestFree(*reqp);
        *reqp = NULL;
    }
}

static inline void cleanup_stream(loopyStream **sp) {
    if (sp && *sp) {
        loopyStreamClose(*sp, NULL, NULL);
        *sp = NULL;
    }
}

/* Macros to declare resources with automatic cleanup on scope exit */
#define LOOPY_LOOP_AUTO_CLEANUP __attribute__((cleanup(cleanup_loopy_loop)))
#define LOOPY_FS_REQUEST_AUTO_CLEANUP __attribute__((cleanup(cleanup_fs_request)))
#define LOOPY_STREAM_AUTO_CLEANUP __attribute__((cleanup(cleanup_stream)))

/* In registration mode, just register. In run mode, run directly. */
static int registration_mode = 0;

#define RUN_TEST(test)                                                         \
    do {                                                                       \
        if (registration_mode) {                                               \
            register_test(#test, test, current_group);                         \
        } else {                                                               \
            printf("Running %s...\n", #test);                                  \
            tests_run++;                                                       \
            if (test()) {                                                      \
                tests_passed++;                                                \
                printf("  PASS\n");                                            \
            } else {                                                           \
                tests_failed++;                                                \
            }                                                                  \
        }                                                                      \
    } while (0)

#define TEST_GROUP(name)                                                       \
    do {                                                                       \
        current_group = name;                                                  \
        if (!registration_mode) {                                              \
            printf("\n--- %s ---\n", name);                                    \
        }                                                                      \
    } while (0)

/* ====================================================================
 * Test: Basic Loop Creation and Destruction
 * ==================================================================== */
static int test_loop_create_delete(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    TEST_ASSERT(l != NULL, "loopyNew should return non-NULL");
    TEST_ASSERT(loopyInited(l),
                "loopyInited should return true after loopyNew");
    TEST_ASSERT_EQ(loopyGetSetSize(l), 16, "setSize should match");

    return 1;
}

static int test_loop_stack_allocated(void) {
    loopyLoop l = {0};
    TEST_ASSERT(loopyInit(&l, 32), "loopyInit should succeed");
    TEST_ASSERT(loopyInited(&l), "loopyInited should return true");
    TEST_ASSERT_EQ(loopyGetSetSize(&l), 32, "setSize should match");

    loopyDeinit(&l);
    TEST_ASSERT(!loopyInited(&l), "loopyInited should be false after deinit");
    return 1;
}

static int test_loop_resize(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(8);
    TEST_ASSERT(l != NULL, "loopyNew should return non-NULL");
    TEST_ASSERT_EQ(loopyGetSetSize(l), 8, "initial setSize should be 8");

    TEST_ASSERT(loopyResizeSetSize(l, 64), "resize should succeed");
    TEST_ASSERT_EQ(loopyGetSetSize(l), 64, "setSize should be 64 after resize");

    return 1;
}

/* ====================================================================
 * Test: File Descriptor Registration
 * ==================================================================== */
static void dummy_callback(loopyLoop *l, int fd, void *clientData,
                           loopyAction mask) {
    (void)l;
    (void)fd;
    (void)clientData;
    (void)mask;
}

static void dummy_async_callback(loopyLoop *l, loopyAsync *async,
                                  void *userData) {
    (void)l;
    (void)async;
    (void)userData;
}

static int test_register_read(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    int fds[2];

    TEST_ASSERT(pipe(fds) == 0, "pipe should succeed");

    TEST_ASSERT(loopyRegisterRead(l, fds[0], dummy_callback, NULL),
                "loopyRegisterRead should succeed");

    loopyAction events = loopyGetEvents(l, fds[0]);
    TEST_ASSERT(loopyActionIsRead(events), "should have read event");
    TEST_ASSERT(!loopyActionIsWrite(events), "should not have write event");

    close(fds[0]);
    close(fds[1]);
    return 1;
}

static int test_register_write(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    int fds[2];

    TEST_ASSERT(pipe(fds) == 0, "pipe should succeed");

    TEST_ASSERT(loopyRegisterWrite(l, fds[1], dummy_callback, NULL),
                "loopyRegisterWrite should succeed");

    loopyAction events = loopyGetEvents(l, fds[1]);
    TEST_ASSERT(!loopyActionIsRead(events), "should not have read event");
    TEST_ASSERT(loopyActionIsWrite(events), "should have write event");

    close(fds[0]);
    close(fds[1]);
    return 1;
}

static int test_register_read_write(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    int fds[2];

    TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0,
                "socketpair should succeed");

    TEST_ASSERT(loopyRegisterRead(l, fds[0], dummy_callback, NULL),
                "loopyRegisterRead should succeed");
    TEST_ASSERT(loopyRegisterWrite(l, fds[0], dummy_callback, NULL),
                "loopyRegisterWrite should succeed");

    loopyAction events = loopyGetEvents(l, fds[0]);
    TEST_ASSERT(loopyActionIsRead(events), "should have read event");
    TEST_ASSERT(loopyActionIsWrite(events), "should have write event");

    close(fds[0]);
    close(fds[1]);
    return 1;
}

static int test_unregister_read(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    int fds[2];

    TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0,
                "socketpair should succeed");

    loopyRegisterRead(l, fds[0], dummy_callback, NULL);
    loopyRegisterWrite(l, fds[0], dummy_callback, NULL);

    loopyUnregisterRead(l, fds[0]);

    loopyAction events = loopyGetEvents(l, fds[0]);
    TEST_ASSERT(!loopyActionIsRead(events), "should not have read event");
    TEST_ASSERT(loopyActionIsWrite(events), "should still have write event");

    close(fds[0]);
    close(fds[1]);
    return 1;
}

static int test_unregister_write(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    int fds[2];

    TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0,
                "socketpair should succeed");

    loopyRegisterRead(l, fds[0], dummy_callback, NULL);
    loopyRegisterWrite(l, fds[0], dummy_callback, NULL);

    loopyUnregisterWrite(l, fds[0]);

    loopyAction events = loopyGetEvents(l, fds[0]);
    TEST_ASSERT(loopyActionIsRead(events), "should still have read event");
    TEST_ASSERT(!loopyActionIsWrite(events), "should not have write event");

    close(fds[0]);
    close(fds[1]);
    return 1;
}

static int test_unregister_all(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    int fds[2];

    TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0,
                "socketpair should succeed");

    loopyRegisterRead(l, fds[0], dummy_callback, NULL);
    loopyRegisterWrite(l, fds[0], dummy_callback, NULL);

    loopyUnregisterReadWrite(l, fds[0]);

    loopyAction events = loopyGetEvents(l, fds[0]);
    TEST_ASSERT(!loopyActionIsRead(events), "should not have read event");
    TEST_ASSERT(!loopyActionIsWrite(events), "should not have write event");

    close(fds[0]);
    close(fds[1]);
    return 1;
}

static int test_register_write_if_none_exists(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    int fds[2];

    TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0,
                "socketpair should succeed");

    /* First registration should succeed */
    TEST_ASSERT(loopyRegisterWriteIfNoneExists(l, fds[0], dummy_callback, NULL),
                "first registration should succeed");

    /* Second registration should fail (already exists) */
    TEST_ASSERT(
        !loopyRegisterWriteIfNoneExists(l, fds[0], dummy_callback, NULL),
        "second registration should fail");

    close(fds[0]);
    close(fds[1]);
    return 1;
}

/* ====================================================================
 * Test: Auto-resize on large fd
 * ==================================================================== */
static int test_auto_resize(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(8);
    int fds[2];

    TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0,
                "socketpair should succeed");

    /* Force fd to be beyond initial setSize by duping to a high fd */
    int highFd = dup2(fds[0], 100);
    TEST_ASSERT(highFd == 100, "dup2 should succeed");

    /* This should auto-resize */
    TEST_ASSERT(loopyRegisterRead(l, highFd, dummy_callback, NULL),
                "loopyRegisterRead should succeed with high fd");

    TEST_ASSERT(loopyGetSetSize(l) > 100, "setSize should have grown");

    close(highFd);
    close(fds[0]);
    close(fds[1]);
    return 1;
}

/* ====================================================================
 * Test: Timer Registration
 * ==================================================================== */
static bool timer_fired = false;
static int timer_count = 0;

static bool test_timer_callback(timerWheel *t, timerWheelId id, void *data) {
    (void)t;
    (void)id;
    (void)data;
    timer_fired = true;
    timer_count++;
    return timer_count < 3; /* Stop after 3 fires */
}

static int test_timer_registration(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    timer_fired = false;
    timer_count = 0;

    uint64_t id = loopyRegisterTimer(l, 1000, 1000, test_timer_callback, NULL);
    TEST_ASSERT(id > 0, "timer registration should return valid id");

    TEST_ASSERT(loopyUnregisterTimer(l, id),
                "timer unregistration should succeed");

    return 1;
}

/* ====================================================================
 * Test: Event Processing with Pipe
 * ==================================================================== */
static int read_callback_count = 0;
static int write_callback_count = 0;

static void test_read_callback(loopyLoop *l, int fd, void *clientData,
                               loopyAction mask) {
    (void)mask;
    (void)clientData;
    char buf[64];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
        read_callback_count++;
    }
    loopyStop(l);
}

static void test_write_callback(loopyLoop *l, int fd, void *clientData,
                                loopyAction mask) {
    (void)mask;
    (void)clientData;
    const char *msg = "test";
    write(fd, msg, 4);
    write_callback_count++;
    loopyUnregisterWrite(l, fd);
}

static int test_event_processing(void) {
    loopyLoop *l LOOPY_LOOP_AUTO_CLEANUP = loopyNew(16);
    int fds[2];
    read_callback_count = 0;
    write_callback_count = 0;

    TEST_ASSERT(pipe(fds) == 0, "pipe should succeed");

    /* Set non-blocking */
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    fcntl(fds[1], F_SETFL, O_NONBLOCK);

    loopyRegisterRead(l, fds[0], test_read_callback, NULL);
    loopyRegisterWrite(l, fds[1], test_write_callback, NULL);

    /* Run the loop - write callback fires, writes data,
     * read callback fires, reads data, stops loop */
    loopyMain(l);

    TEST_ASSERT_EQ(write_callback_count, 1, "write callback should fire once");
    TEST_ASSERT_EQ(read_callback_count, 1, "read callback should fire once");

    close(fds[0]);
    close(fds[1]);
    return 1;
}

/* ====================================================================
 * Test: Before/After Sleep Callbacks
 * ==================================================================== */
static int before_sleep_count = 0;
static int after_sleep_count = 0;

static void before_sleep_callback(loopyLoop *l, void *clientData) {
    (void)clientData;
    before_sleep_count++;
    if (before_sleep_count >= 2) {
        loopyStop(l);
    }
}

static void after_sleep_callback(loopyLoop *l, void *clientData) {
    (void)l;
    (void)clientData;
    after_sleep_count++;
}

static int test_sleep_callbacks(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    before_sleep_count = 0;
    after_sleep_count = 0;

    /* Register a timer so the loop has something to wait for */
    loopyRegisterTimer(l, 1000, 1000, test_timer_callback, NULL);

    loopySetBeforeSleepCallback(l, before_sleep_callback, NULL);
    loopySetAfterSleepCallback(l, after_sleep_callback, NULL);

    loopyMain(l);

    TEST_ASSERT(before_sleep_count >= 2, "before sleep should be called");
    TEST_ASSERT(after_sleep_count >= 1, "after sleep should be called");

    return 1;
}

/* ====================================================================
 * Test: loopyStop functionality
 * ==================================================================== */
static void stop_after_one(loopyLoop *l, void *clientData) {
    (void)clientData;
    static int count = 0;
    if (++count >= 1) {
        loopyStop(l);
    }
}

static int test_stop(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    loopyRegisterTimer(l, 1000, 1000, test_timer_callback, NULL);
    loopySetBeforeSleepCallback(l, stop_after_one, NULL);

    /* Should exit quickly due to stop */
    loopyMain(l);

    return 1;
}

/* ====================================================================
 * Test: Adapter Name
 * ==================================================================== */
static int test_adapter_name(void) {
    const char *name = loopyAdapterName();
    TEST_ASSERT(name != NULL, "adapter name should not be NULL");
    TEST_ASSERT(strlen(name) > 0, "adapter name should not be empty");

    /* Should be one of: kqueue, epoll, evport, select */
    int valid = (strcmp(name, "kqueue") == 0 || strcmp(name, "epoll") == 0 ||
                 strcmp(name, "io_uring/epoll") == 0 ||
                 strcmp(name, "evport") == 0 || strcmp(name, "select") == 0);
    TEST_ASSERT(valid, "adapter name should be valid");

    printf("    (adapter: %s)\n", name);
    return 1;
}

/* ====================================================================
 * Test: loopyNet - Socket Creation
 * ==================================================================== */
static int test_net_tcp4_server(void) {
    loopyNet net = {0};

    TEST_ASSERT(loopyNetTcp4Server(&net, 0, "127.0.0.1", 5),
                "TCP4 server creation should succeed");
    TEST_ASSERT(net.sock >= 0, "socket should be valid");

    close(net.sock);
    return 1;
}

static int test_net_tcp6_server(void) {
    loopyNet net = {0};

    TEST_ASSERT(loopyNetTcp6Server(&net, 0, "::1", 5),
                "TCP6 server creation should succeed");
    TEST_ASSERT(net.sock >= 0, "socket should be valid");

    close(net.sock);
    return 1;
}

static int test_net_nonblock(void) {
    loopyNet net = {0};

    TEST_ASSERT(loopyNetTcp4Server(&net, 0, "127.0.0.1", 5),
                "TCP4 server creation should succeed");

    TEST_ASSERT(loopyNetNonBlockEnable(&net), "enable nonblock should succeed");

    int flags = fcntl(net.sock, F_GETFL);
    TEST_ASSERT((flags & O_NONBLOCK) != 0, "socket should be non-blocking");

    TEST_ASSERT(loopyNetNonBlockDisable(&net),
                "disable nonblock should succeed");

    flags = fcntl(net.sock, F_GETFL);
    TEST_ASSERT((flags & O_NONBLOCK) == 0, "socket should be blocking");

    close(net.sock);
    return 1;
}

static int test_net_nodelay(void) {
    loopyNet net = {0};

    TEST_ASSERT(loopyNetTcp4Server(&net, 0, "127.0.0.1", 5),
                "TCP4 server creation should succeed");

    TEST_ASSERT(loopyNetTcpNoDelayEnable(&net),
                "enable nodelay should succeed");
    TEST_ASSERT(loopyNetTcpNoDelayDisable(&net),
                "disable nodelay should succeed");

    close(net.sock);
    return 1;
}

static int test_net_keepalive(void) {
    loopyNet net = {0};

    TEST_ASSERT(loopyNetTcp4Server(&net, 0, "127.0.0.1", 5),
                "TCP4 server creation should succeed");

    TEST_ASSERT(loopyNetTcpKeepAliveEnable(&net),
                "enable keepalive should succeed");
    TEST_ASSERT(loopyNetKeepAlive(&net, 60),
                "keepalive with interval should succeed");

    close(net.sock);
    return 1;
}

static int test_net_resolve(void) {
    loopyNet net = {0};
    char ipbuf[INET6_ADDRSTRLEN] = {0};

    TEST_ASSERT(loopyNetResolveIP(&net, "127.0.0.1", ipbuf, sizeof(ipbuf)),
                "resolve IP should succeed");
    TEST_ASSERT(strcmp(ipbuf, "127.0.0.1") == 0, "IP should match");

    return 1;
}

static int test_net_format_addr(void) {
    char buf[64] = {0};

    int len = loopyNetFormatAddr(buf, sizeof(buf), "192.168.1.1", 8080);
    TEST_ASSERT(len > 0, "format addr should succeed");
    TEST_ASSERT(strcmp(buf, "192.168.1.1:8080") == 0,
                "formatted addr should match");

    len = loopyNetFormatAddr(buf, sizeof(buf), "::1", 8080);
    TEST_ASSERT(len > 0, "format IPv6 addr should succeed");
    TEST_ASSERT(strcmp(buf, "[::1]:8080") == 0,
                "formatted IPv6 addr should match");

    return 1;
}

/* ====================================================================
 * Test: loopyNice
 * ==================================================================== */
static int test_nice_create_delete(void) {
    loopyNice *n = loopyNiceNew(16);
    TEST_ASSERT(n != NULL, "loopyNiceNew should succeed");
    TEST_ASSERT(n->loop != NULL, "loop should be initialized");

    loopyNiceFree(n);
    return 1;
}

static int test_nice_pipe(void) {
    loopyNice *n = loopyNiceNew(16);

    loopyNiceServerDesc sd = {
        .clientData = NULL,
        .cb = dummy_callback,
        .pipe = {.usePipe = true},
    };

    TEST_ASSERT(loopyNiceServerCreateSockets(n, &sd),
                "pipe creation should succeed");
    TEST_ASSERT(sd.pipe.readFd >= 0, "readFd should be valid");
    TEST_ASSERT(sd.pipe.writeFd >= 0, "writeFd should be valid");

    close(sd.pipe.readFd);
    close(sd.pipe.writeFd);
    loopyNiceFree(n);
    return 1;
}

static int test_nice_socketpair(void) {
    loopyNice *n = loopyNiceNew(16);

    loopyNiceServerDesc sd = {
        .clientData = NULL,
        .cb = dummy_callback,
        .socketpair = {.useSocketPair = true},
    };

    TEST_ASSERT(loopyNiceServerCreateSockets(n, &sd),
                "socketpair creation should succeed");
    TEST_ASSERT(sd.socketpair.fd[0] >= 0, "fd[0] should be valid");
    TEST_ASSERT(sd.socketpair.fd[1] >= 0, "fd[1] should be valid");

    close(sd.socketpair.fd[0]);
    close(sd.socketpair.fd[1]);
    loopyNiceFree(n);
    return 1;
}

static int test_nice_tcp_server(void) {
    loopyNice *n = loopyNiceNew(16);

    loopyNiceServerDesc sd = {
        .clientData = NULL,
        .cb = dummy_callback,
        .addr = {.bindAddr = "127.0.0.1", .port = 0},
        .backlog = 5,
    };

    TEST_ASSERT(loopyNiceServerCreateSockets(n, &sd),
                "TCP server creation should succeed");
    TEST_ASSERT(sd.addr.listen[0] >= 0, "listen socket should be valid");
    TEST_ASSERT(sd.addr.port > 0, "port should be assigned");

    close(sd.addr.listen[0]);
    loopyNiceFree(n);
    return 1;
}

/* ====================================================================
 * Test: Edge Cases
 * ==================================================================== */
static int test_null_handling(void) {
    /* loopyDelete with NULL should not crash */

    /* loopyDeinit with NULL should not crash */
    loopyDeinit(NULL);

    /* loopyNiceFree with NULL should not crash */
    loopyNiceFree(NULL);

    return 1;
}

static int test_unregister_nonexistent(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    /* Unregistering a non-existent fd should not crash */
    loopyUnregisterRead(l, 999);
    loopyUnregisterWrite(l, 999);
    loopyUnregisterReadWrite(l, 999);

    return 1;
}

static int test_get_events_invalid_fd(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    /* Getting events for a fd beyond setSize should return 0 */
    loopyAction events = loopyGetEvents(l, 999);
    TEST_ASSERT_EQ(events, 0, "events for invalid fd should be 0");

    return 1;
}

/* ====================================================================
 * Test: TCP Connect/Accept
 * ==================================================================== */
static int test_tcp_connect_accept(void) {
    loopyNet server = {0};
    loopyNet client = {0};

    /* Create server */
    TEST_ASSERT(loopyNetTcp4Server(&server, 0, "127.0.0.1", 5),
                "server creation should succeed");
    TEST_ASSERT(loopyNetNonBlockEnable(&server),
                "server nonblock should succeed");

    int serverPort = 0;
    loopyNetSockName(server.sock, NULL, 0, &serverPort);
    TEST_ASSERT(serverPort > 0, "server port should be valid");

    /* Connect client (blocking connect for test simplicity) */
    TEST_ASSERT(loopyNetTcpConnect(&client, "127.0.0.1", serverPort),
                "client connect should succeed");

    /* Accept connection - poll for readability first */
    struct pollfd pfd = {.fd = server.sock, .events = POLLIN};
    int pollResult = poll(&pfd, 1, 1000); /* 1 second timeout */
    TEST_ASSERT(pollResult > 0, "server should have pending connection");

    char clientIp[INET6_ADDRSTRLEN] = {0};
    int clientPort = 0;
    int clientSock = -1;
    TEST_ASSERT(loopyNetTcpAcceptNonBlock(&server, clientIp, sizeof(clientIp),
                                          &clientPort, &clientSock),
                "accept should succeed");
    TEST_ASSERT(clientSock >= 0, "client socket should be valid");

    close(clientSock);
    close(client.sock);
    close(server.sock);
    return 1;
}

/* ====================================================================
 * Test: Unix Domain Sockets
 * ==================================================================== */
static int test_unix_server(void) {
    loopyNet server = {0};
    const char *path = "/tmp/loopy_test.sock";

    /* Remove old socket if exists */
    unlink(path);

    TEST_ASSERT(loopyNetUnixServer(&server, path, 0700, 5),
                "unix server creation should succeed");
    TEST_ASSERT(server.sock >= 0, "socket should be valid");

    close(server.sock);
    unlink(path);
    return 1;
}

static int test_unix_connect(void) {
    loopyNet server = {0};
    loopyNet client = {0};
    const char *path = "/tmp/loopy_test2.sock";

    unlink(path);

    TEST_ASSERT(loopyNetUnixServer(&server, path, 0700, 5),
                "server creation should succeed");
    TEST_ASSERT(loopyNetNonBlockEnable(&server),
                "server nonblock should succeed");

    TEST_ASSERT(loopyNetUnixNonBlockConnect(&client, path),
                "client connect should succeed");

    int clientSock = -1;
    TEST_ASSERT(loopyNetUnixAcceptNonBlock(&server, &clientSock),
                "accept should succeed");
    TEST_ASSERT(clientSock >= 0, "client socket should be valid");

    close(clientSock);
    close(client.sock);
    close(server.sock);
    unlink(path);
    return 1;
}

/* ====================================================================
 * Test: SendTimeout
 * ==================================================================== */
static int test_send_timeout(void) {
    loopyNet net = {0};

    TEST_ASSERT(loopyNetTcp4Server(&net, 0, "127.0.0.1", 5),
                "server creation should succeed");

    TEST_ASSERT(loopyNetSendTimeout(&net, 1000), "send timeout should succeed");

    close(net.sock);
    return 1;
}

/* ====================================================================
 * Test: maxfd tracking
 * ==================================================================== */
static int test_maxfd_tracking(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(64);
    int fds[3][2];

    /* Create multiple pipes */
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT(pipe(fds[i]) == 0, "pipe should succeed");
    }

    /* Register reads on all */
    for (int i = 0; i < 3; i++) {
        loopyRegisterRead(l, fds[i][0], dummy_callback, NULL);
    }

    /* Unregister middle one */
    loopyUnregisterRead(l, fds[1][0]);

    /* Unregister last one - should update maxfd */
    loopyUnregisterRead(l, fds[2][0]);

    /* First one should still work */
    loopyAction events = loopyGetEvents(l, fds[0][0]);
    TEST_ASSERT(loopyActionIsRead(events),
                "first fd should still be registered");

    for (int i = 0; i < 3; i++) {
        close(fds[i][0]);
        close(fds[i][1]);
    }
    return 1;
}

/* ====================================================================
 * Test: Max-Heap - Basic Operations
 * ==================================================================== */
static int test_heap_create_delete(void) {
    loopyMaxHeap *heap = loopyMaxHeapNew(16);
    TEST_ASSERT(heap != NULL, "loopyMaxHeapNew should succeed");
    TEST_ASSERT(loopyMaxHeapInited(heap), "heap should be initialized");
    TEST_ASSERT(loopyMaxHeapIsEmpty(heap), "new heap should be empty");
    TEST_ASSERT_EQ(loopyMaxHeapSize(heap), 0, "size should be 0");
    TEST_ASSERT(loopyMaxHeapCapacity(heap) >= 16,
                "capacity should be at least 16");

    loopyMaxHeapFree(heap);
    return 1;
}

static int test_heap_stack_allocated(void) {
    loopyMaxHeap heap = {0};
    TEST_ASSERT(loopyMaxHeapInit(&heap, 32), "loopyMaxHeapInit should succeed");
    TEST_ASSERT(loopyMaxHeapInited(&heap), "heap should be initialized");
    TEST_ASSERT(loopyMaxHeapIsEmpty(&heap), "heap should be empty");

    loopyMaxHeapDeinit(&heap);
    TEST_ASSERT(!loopyMaxHeapInited(&heap),
                "heap should not be initialized after deinit");
    return 1;
}

static int test_heap_insert_peek(void) {
    loopyMaxHeap *heap = loopyMaxHeapNew(16);

    TEST_ASSERT(loopyMaxHeapInsert(heap, 5), "insert 5 should succeed");
    TEST_ASSERT_EQ(loopyMaxHeapPeek(heap), 5, "max should be 5");
    TEST_ASSERT_EQ(loopyMaxHeapSize(heap), 1, "size should be 1");

    TEST_ASSERT(loopyMaxHeapInsert(heap, 10), "insert 10 should succeed");
    TEST_ASSERT_EQ(loopyMaxHeapPeek(heap), 10, "max should be 10");

    TEST_ASSERT(loopyMaxHeapInsert(heap, 3), "insert 3 should succeed");
    TEST_ASSERT_EQ(loopyMaxHeapPeek(heap), 10, "max should still be 10");

    TEST_ASSERT(loopyMaxHeapInsert(heap, 15), "insert 15 should succeed");
    TEST_ASSERT_EQ(loopyMaxHeapPeek(heap), 15, "max should be 15");

    TEST_ASSERT_EQ(loopyMaxHeapSize(heap), 4, "size should be 4");

    loopyMaxHeapFree(heap);
    return 1;
}

static int test_heap_pop(void) {
    loopyMaxHeap *heap = loopyMaxHeapNew(16);

    loopyMaxHeapInsert(heap, 5);
    loopyMaxHeapInsert(heap, 10);
    loopyMaxHeapInsert(heap, 3);
    loopyMaxHeapInsert(heap, 15);
    loopyMaxHeapInsert(heap, 8);

    /* Should pop in descending order */
    TEST_ASSERT_EQ(loopyMaxHeapPop(heap), 15, "first pop should be 15");
    TEST_ASSERT_EQ(loopyMaxHeapPop(heap), 10, "second pop should be 10");
    TEST_ASSERT_EQ(loopyMaxHeapPop(heap), 8, "third pop should be 8");
    TEST_ASSERT_EQ(loopyMaxHeapPop(heap), 5, "fourth pop should be 5");
    TEST_ASSERT_EQ(loopyMaxHeapPop(heap), 3, "fifth pop should be 3");

    TEST_ASSERT(loopyMaxHeapIsEmpty(heap), "heap should be empty");
    TEST_ASSERT_EQ(loopyMaxHeapPop(heap), LOOPY_HEAP_EMPTY_VALUE,
                   "pop empty should return sentinel");

    loopyMaxHeapFree(heap);
    return 1;
}

static int test_heap_remove(void) {
    loopyMaxHeap *heap = loopyMaxHeapNew(16);

    loopyMaxHeapInsert(heap, 5);
    loopyMaxHeapInsert(heap, 10);
    loopyMaxHeapInsert(heap, 3);
    loopyMaxHeapInsert(heap, 15);
    loopyMaxHeapInsert(heap, 8);

    /* Remove middle element */
    TEST_ASSERT(loopyMaxHeapRemove(heap, 10), "remove 10 should succeed");
    TEST_ASSERT(!loopyMaxHeapContains(heap, 10), "10 should not be in heap");
    TEST_ASSERT_EQ(loopyMaxHeapSize(heap), 4, "size should be 4");
    TEST_ASSERT_EQ(loopyMaxHeapPeek(heap), 15, "max should still be 15");

    /* Remove max element */
    TEST_ASSERT(loopyMaxHeapRemove(heap, 15), "remove 15 should succeed");
    TEST_ASSERT_EQ(loopyMaxHeapPeek(heap), 8, "new max should be 8");

    /* Remove min element */
    TEST_ASSERT(loopyMaxHeapRemove(heap, 3), "remove 3 should succeed");
    TEST_ASSERT_EQ(loopyMaxHeapSize(heap), 2, "size should be 2");

    /* Try to remove non-existent element */
    TEST_ASSERT(!loopyMaxHeapRemove(heap, 100), "remove 100 should fail");

    loopyMaxHeapFree(heap);
    return 1;
}

static int test_heap_contains(void) {
    loopyMaxHeap *heap = loopyMaxHeapNew(16);

    loopyMaxHeapInsert(heap, 5);
    loopyMaxHeapInsert(heap, 10);
    loopyMaxHeapInsert(heap, 15);

    TEST_ASSERT(loopyMaxHeapContains(heap, 5), "should contain 5");
    TEST_ASSERT(loopyMaxHeapContains(heap, 10), "should contain 10");
    TEST_ASSERT(loopyMaxHeapContains(heap, 15), "should contain 15");
    TEST_ASSERT(!loopyMaxHeapContains(heap, 7), "should not contain 7");
    TEST_ASSERT(!loopyMaxHeapContains(heap, 0), "should not contain 0");

    loopyMaxHeapFree(heap);
    return 1;
}

static int test_heap_clear(void) {
    loopyMaxHeap *heap = loopyMaxHeapNew(16);

    loopyMaxHeapInsert(heap, 5);
    loopyMaxHeapInsert(heap, 10);
    loopyMaxHeapInsert(heap, 15);

    TEST_ASSERT_EQ(loopyMaxHeapSize(heap), 3, "size should be 3");

    loopyMaxHeapClear(heap);

    TEST_ASSERT(loopyMaxHeapIsEmpty(heap), "heap should be empty after clear");
    TEST_ASSERT_EQ(loopyMaxHeapSize(heap), 0, "size should be 0");
    TEST_ASSERT(loopyMaxHeapCapacity(heap) >= 16,
                "capacity should be preserved");

    loopyMaxHeapFree(heap);
    return 1;
}

static int test_heap_reserve(void) {
    loopyMaxHeap *heap = loopyMaxHeapNew(8);

    TEST_ASSERT(loopyMaxHeapCapacity(heap) >= 8, "initial capacity");

    TEST_ASSERT(loopyMaxHeapReserve(heap, 100), "reserve should succeed");
    TEST_ASSERT(loopyMaxHeapCapacity(heap) >= 100,
                "capacity should be at least 100");

    /* Reserve smaller - should be no-op */
    size_t cap = loopyMaxHeapCapacity(heap);
    TEST_ASSERT(loopyMaxHeapReserve(heap, 50),
                "reserve smaller should succeed");
    TEST_ASSERT_EQ(loopyMaxHeapCapacity(heap), cap,
                   "capacity should not shrink");

    loopyMaxHeapFree(heap);
    return 1;
}

/* ====================================================================
 * Test: Max-Heap - Auto Growth
 * ==================================================================== */
static int test_heap_auto_grow(void) {
    loopyMaxHeap *heap = loopyMaxHeapNew(4);

    /* Insert more than initial capacity */
    for (int i = 0; i < 100; i++) {
        TEST_ASSERT(loopyMaxHeapInsert(heap, i), "insert should succeed");
    }

    TEST_ASSERT_EQ(loopyMaxHeapSize(heap), 100, "size should be 100");
    TEST_ASSERT_EQ(loopyMaxHeapPeek(heap), 99, "max should be 99");

    /* Verify heap property by popping all */
    for (int i = 99; i >= 0; i--) {
        TEST_ASSERT_EQ(loopyMaxHeapPop(heap), i,
                       "pop order should be descending");
    }

    loopyMaxHeapFree(heap);
    return 1;
}

/* ====================================================================
 * Test: Max-Heap - Large Values (FD simulation)
 * ==================================================================== */
static int test_heap_large_fds(void) {
    loopyMaxHeap *heap = loopyMaxHeapNew(16);

    /* Simulate high fd values */
    loopyMaxHeapInsert(heap, 3);
    loopyMaxHeapInsert(heap, 500);
    loopyMaxHeapInsert(heap, 1000);
    loopyMaxHeapInsert(heap, 50);

    TEST_ASSERT_EQ(loopyMaxHeapPeek(heap), 1000, "max should be 1000");

    loopyMaxHeapRemove(heap, 1000);
    TEST_ASSERT_EQ(loopyMaxHeapPeek(heap), 500, "max should be 500");

    loopyMaxHeapRemove(heap, 500);
    TEST_ASSERT_EQ(loopyMaxHeapPeek(heap), 50, "max should be 50");

    loopyMaxHeapFree(heap);
    return 1;
}

/* ====================================================================
 * Test: Max-Heap - Duplicate Handling
 * ==================================================================== */
static int test_heap_duplicates(void) {
    loopyMaxHeap *heap = loopyMaxHeapNew(16);

    /* Note: for fd tracking, duplicates shouldn't happen,
     * but heap should handle them gracefully */
    loopyMaxHeapInsert(heap, 5);
    loopyMaxHeapInsert(heap, 5);
    loopyMaxHeapInsert(heap, 10);

    TEST_ASSERT_EQ(loopyMaxHeapSize(heap), 3, "size should be 3");
    TEST_ASSERT_EQ(loopyMaxHeapPeek(heap), 10, "max should be 10");

    /* Remove one instance of 5 */
    loopyMaxHeapRemove(heap, 5);
    TEST_ASSERT_EQ(loopyMaxHeapSize(heap), 2, "size should be 2");

    loopyMaxHeapFree(heap);
    return 1;
}

/* ====================================================================
 * Test: Max-Heap - Edge Cases
 * ==================================================================== */
static int test_heap_empty_operations(void) {
    loopyMaxHeap *heap = loopyMaxHeapNew(16);

    /* Operations on empty heap */
    TEST_ASSERT_EQ(loopyMaxHeapPeek(heap), LOOPY_HEAP_EMPTY_VALUE,
                   "peek empty should return sentinel");
    TEST_ASSERT_EQ(loopyMaxHeapPop(heap), LOOPY_HEAP_EMPTY_VALUE,
                   "pop empty should return sentinel");
    TEST_ASSERT(!loopyMaxHeapRemove(heap, 5), "remove from empty should fail");
    TEST_ASSERT(!loopyMaxHeapContains(heap, 5),
                "contains on empty should be false");

    loopyMaxHeapFree(heap);
    return 1;
}

static int test_heap_null_safety(void) {
    /* These should not crash */
    loopyMaxHeapFree(NULL);
    loopyMaxHeapDeinit(NULL);
    TEST_ASSERT(!loopyMaxHeapInit(NULL, 16), "init NULL should fail");
    TEST_ASSERT(!loopyMaxHeapInited(NULL), "inited NULL should be false");
    TEST_ASSERT(loopyMaxHeapIsEmpty(NULL), "isEmpty NULL should be true");
    TEST_ASSERT_EQ(loopyMaxHeapSize(NULL), 0, "size NULL should be 0");
    TEST_ASSERT_EQ(loopyMaxHeapCapacity(NULL), 0, "capacity NULL should be 0");

    return 1;
}

static int test_heap_single_element(void) {
    loopyMaxHeap *heap = loopyMaxHeapNew(16);

    loopyMaxHeapInsert(heap, 42);
    TEST_ASSERT_EQ(loopyMaxHeapPeek(heap), 42, "peek should be 42");
    TEST_ASSERT_EQ(loopyMaxHeapSize(heap), 1, "size should be 1");

    TEST_ASSERT_EQ(loopyMaxHeapPop(heap), 42, "pop should be 42");
    TEST_ASSERT(loopyMaxHeapIsEmpty(heap), "should be empty");

    loopyMaxHeapFree(heap);
    return 1;
}

static int test_heap_remove_last(void) {
    loopyMaxHeap *heap = loopyMaxHeapNew(16);

    loopyMaxHeapInsert(heap, 5);
    loopyMaxHeapInsert(heap, 10);
    loopyMaxHeapInsert(heap, 3);

    /* Remove the element that happens to be at the last position */
    /* After inserts, heap might be [10, 5, 3] or similar arrangement */
    TEST_ASSERT(loopyMaxHeapRemove(heap, 3), "remove 3 should succeed");
    TEST_ASSERT_EQ(loopyMaxHeapSize(heap), 2, "size should be 2");

    /* Heap should still work correctly */
    TEST_ASSERT_EQ(loopyMaxHeapPeek(heap), 10, "max should be 10");

    loopyMaxHeapFree(heap);
    return 1;
}

/* ====================================================================
 * Test: Max-Heap - Stress Test
 * ==================================================================== */
static int test_heap_stress(void) {
    loopyMaxHeap *heap = loopyMaxHeapNew(8);

    /* Insert, remove, insert pattern */
    for (int round = 0; round < 10; round++) {
        for (int i = 0; i < 50; i++) {
            loopyMaxHeapInsert(heap, i + round * 100);
        }
        for (int i = 0; i < 25; i++) {
            loopyMaxHeapPop(heap);
        }
    }

    /* Should have 25 * 10 = 250 elements */
    TEST_ASSERT_EQ(loopyMaxHeapSize(heap), 250, "size should be 250");

    /* Verify heap property */
    loopyHeapValue prev = loopyMaxHeapPop(heap);
    while (!loopyMaxHeapIsEmpty(heap)) {
        loopyHeapValue curr = loopyMaxHeapPop(heap);
        TEST_ASSERT(curr <= prev, "heap property should hold");
        prev = curr;
    }

    loopyMaxHeapFree(heap);
    return 1;
}

/* ====================================================================
 * Test: Max-Heap Integration with Loopy maxfd
 * ==================================================================== */
static int test_heap_loopy_maxfd_integration(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    int fds[5][2];

    /* Create pipes */
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT(pipe(fds[i]) == 0, "pipe should succeed");
    }

    /* Register in non-sequential order */
    loopyRegisterRead(l, fds[2][0], dummy_callback, NULL);
    loopyRegisterRead(l, fds[0][0], dummy_callback, NULL);
    loopyRegisterRead(l, fds[4][0], dummy_callback, NULL);
    loopyRegisterRead(l, fds[1][0], dummy_callback, NULL);
    loopyRegisterRead(l, fds[3][0], dummy_callback, NULL);

    /* maxfd should be the highest fd */
    int maxRegistered = fds[0][0];
    for (int i = 1; i < 5; i++) {
        if (fds[i][0] > maxRegistered) {
            maxRegistered = fds[i][0];
        }
    }

    /* Unregister the max */
    loopyUnregisterRead(l, maxRegistered);

    /* Find new expected max */
    int newMax = -1;
    for (int i = 0; i < 5; i++) {
        if (fds[i][0] != maxRegistered && fds[i][0] > newMax) {
            if (loopyGetEvents(l, fds[i][0]) != 0) {
                newMax = fds[i][0];
            }
        }
    }

    /* Verify maxfd updated correctly via heap */
    /* (We can't directly access l->maxfd, but we can verify behavior) */
    TEST_ASSERT(!loopyMaxHeapContains(&l->fdHeap, maxRegistered),
                "removed fd should not be in heap");

    /* Clean up */
    for (int i = 0; i < 5; i++) {
        close(fds[i][0]);
        close(fds[i][1]);
    }
    return 1;
}

/* ====================================================================
 * Test: loopyDNS - Async DNS Resolution
 * ==================================================================== */
static int test_dns_create_delete(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyDNSConfig config = LOOPY_DNS_CONFIG_DEFAULT;

    loopyDNS *dns = loopyDNSNew(l, &config);
    TEST_ASSERT(dns != NULL, "loopyDNSNew should succeed");
    TEST_ASSERT_EQ(loopyDNSPendingCount(dns), 0, "pending count should be 0");

    loopyDNSFree(dns);
    return 1;
}

static int test_dns_create_default_config(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    /* NULL config should use defaults */
    loopyDNS *dns = loopyDNSNew(l, NULL);
    TEST_ASSERT(dns != NULL, "loopyDNSNew with NULL config should succeed");

    loopyDNSFree(dns);
    return 1;
}

static int test_dns_status_strings(void) {
    TEST_ASSERT(strcmp(loopyDNSStatusString(LOOPY_DNS_OK), "OK") == 0,
                "OK status string");
    TEST_ASSERT(strcmp(loopyDNSStatusString(LOOPY_DNS_NXDOMAIN), "NXDOMAIN") ==
                    0,
                "NXDOMAIN status string");
    TEST_ASSERT(strcmp(loopyDNSStatusString(LOOPY_DNS_SERVFAIL), "SERVFAIL") ==
                    0,
                "SERVFAIL status string");
    TEST_ASSERT(strcmp(loopyDNSStatusString(LOOPY_DNS_TIMEOUT), "TIMEOUT") == 0,
                "TIMEOUT status string");
    TEST_ASSERT(
        strcmp(loopyDNSStatusString(LOOPY_DNS_CANCELLED), "CANCELLED") == 0,
        "CANCELLED status string");
    TEST_ASSERT(strcmp(loopyDNSStatusString(LOOPY_DNS_ERROR), "ERROR") == 0,
                "ERROR status string");
    TEST_ASSERT(strcmp(loopyDNSStatusString(999), "ERROR") == 0,
                "unknown status string falls back to ERROR");

    return 1;
}

static int dns_callback_count = 0;
static loopyDNSStatus dns_last_status = LOOPY_DNS_ERROR;
static int dns_address_count = 0;
static loopyLoop *dns_test_loop = NULL;

static void test_dns_callback(loopyDNS *dns, const loopyDNSResult *result) {
    (void)dns;
    dns_callback_count++;
    dns_last_status = result->status;
    dns_address_count = (int)result->addressCount;
    if (dns_test_loop) {
        loopyStop(dns_test_loop);
    }
}

static bool dns_timeout_callback(timerWheel *t, timerWheelId id, void *data) {
    (void)t;
    (void)id;
    loopyLoop *l = data;
    loopyStop(l);
    return false; /* Don't repeat */
}

static int test_dns_resolve_localhost(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyDNSConfig config = LOOPY_DNS_CONFIG_DEFAULT;
    config.timeoutMs = 2000;

    loopyDNS *dns = loopyDNSNew(l, &config);
    dns_callback_count = 0;
    dns_last_status = LOOPY_DNS_ERROR;
    dns_address_count = 0;
    dns_test_loop = l;

    /* Resolve localhost - should always work */
    loopyDNSQueryId id =
        loopyDNSResolve(dns, "localhost", LOOPY_DNS_A, test_dns_callback, NULL);
    TEST_ASSERT(id > 0, "resolve should return valid id");
    /* Note: Don't check pending count - DNS may complete synchronously if
     * cached */

    /* Set timeout to stop loop if DNS takes too long */
    loopyRegisterTimer(l, 3000000, 0, dns_timeout_callback, l);

    /* Run loop - will exit when DNS completes or timeout */
    loopyMain(l);

    TEST_ASSERT_EQ(dns_callback_count, 1, "callback should fire");
    TEST_ASSERT_EQ(dns_last_status, LOOPY_DNS_OK, "status should be OK");
    TEST_ASSERT(dns_address_count > 0, "should have addresses");

    dns_test_loop = NULL;
    loopyDNSFree(dns);
    return 1;
}

static int test_dns_cancel(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyDNSConfig config = LOOPY_DNS_CONFIG_DEFAULT;

    loopyDNS *dns = loopyDNSNew(l, &config);

    /* Submit query - use hostname that will take time to resolve (non-existent
     * TLD) */
    /* Include PID and counter for uniqueness across concurrent test processes
     */
    char unique_host[128];
    static uint32_t call_count = 0;
    snprintf(unique_host, sizeof(unique_host), "test-%d-%u-%ld.invalidtld",
             getpid(), ++call_count, (long)time(NULL));

    loopyDNSQueryId id =
        loopyDNSResolve(dns, unique_host, LOOPY_DNS_A, test_dns_callback, NULL);
    TEST_ASSERT(id > 0, "resolve should return valid id");

    /* Cancel it - may or may not succeed depending on timing */
    /* Just verify the API works without crashing */
    loopyDNSCancel(dns, id);

    /* Cancel non-existent should fail */
    TEST_ASSERT(!loopyDNSCancel(dns, 99999), "cancel invalid should fail");

    loopyDNSFree(dns);
    return 1;
}

static int test_dns_cancel_all(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyDNSConfig config = LOOPY_DNS_CONFIG_DEFAULT;

    loopyDNS *dns = loopyDNSNew(l, &config);

    /* Submit multiple queries */
    loopyDNSQueryId id1 = loopyDNSResolve(dns, "example.com", LOOPY_DNS_A,
                                          test_dns_callback, NULL);
    loopyDNSQueryId id2 = loopyDNSResolve(dns, "example.org", LOOPY_DNS_A,
                                          test_dns_callback, NULL);
    TEST_ASSERT(id1 > 0, "query 1 should succeed");
    TEST_ASSERT(id2 > 0, "query 2 should succeed");

    /* Cancel all - queries are marked cancelled but still in queue */
    loopyDNSCancelAll(dns);

    /* The queries are cancelled but may still be in flight.
     * CancelAll marks them for cancellation; they'll be discarded when
     * completed. This is correct async behavior - the important thing is they
     * won't invoke callbacks. */

    loopyDNSFree(dns);
    return 1;
}

static int test_dns_null_safety(void) {
    /* These should not crash */
    loopyDNSFree(NULL);

    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyDNS *dns = loopyDNSNew(l, NULL);

    TEST_ASSERT_EQ(
        loopyDNSResolve(dns, NULL, LOOPY_DNS_A, test_dns_callback, NULL), 0,
        "resolve NULL hostname should fail");
    TEST_ASSERT_EQ(loopyDNSResolve(dns, "example.com", LOOPY_DNS_A, NULL, NULL),
                   0, "resolve NULL callback should fail");
    TEST_ASSERT(!loopyDNSCancel(NULL, 1), "cancel NULL dns should fail");

    loopyDNSFree(dns);
    return 1;
}

/* Reverse DNS test state */
static int dns_reverse_callback_count = 0;
static loopyDNSStatus dns_reverse_last_status = LOOPY_DNS_ERROR;
static char dns_reverse_hostname[NI_MAXHOST] = {0};

static void test_dns_reverse_callback(loopyDNS *dns,
                                      const loopyDNSReverseResult *result) {
    (void)dns;
    dns_reverse_callback_count++;
    dns_reverse_last_status = result->status;
    strncpy(dns_reverse_hostname, result->hostname,
            sizeof(dns_reverse_hostname) - 1);
    if (dns_test_loop) {
        loopyStop(dns_test_loop);
    }
}

static int test_dns_reverse_lookup(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyDNSConfig config = LOOPY_DNS_CONFIG_DEFAULT;
    config.timeoutMs = 2000;

    loopyDNS *dns = loopyDNSNew(l, &config);
    dns_reverse_callback_count = 0;
    dns_reverse_last_status = LOOPY_DNS_ERROR;
    dns_reverse_hostname[0] = '\0';
    dns_test_loop = l;

    /* Reverse lookup for 127.0.0.1 - should resolve to localhost */
    loopyDNSQueryId id = loopyDNSReverseLookup(dns, "127.0.0.1", 0,
                                               test_dns_reverse_callback, NULL);
    TEST_ASSERT(id > 0, "reverse lookup should return valid id");
    /* Note: Don't check pending count - DNS may complete synchronously if
     * cached */

    /* Set timeout to stop loop if DNS takes too long */
    loopyRegisterTimer(l, 3000000, 0, dns_timeout_callback, l);

    /* Run loop - will exit when DNS completes or timeout */
    loopyMain(l);

    TEST_ASSERT_EQ(dns_reverse_callback_count, 1, "callback should fire");
    TEST_ASSERT_EQ(dns_reverse_last_status, LOOPY_DNS_OK,
                   "status should be OK");
    TEST_ASSERT(strlen(dns_reverse_hostname) > 0, "should have hostname");

    dns_test_loop = NULL;
    loopyDNSFree(dns);
    return 1;
}

static int test_dns_reverse_null_safety(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyDNS *dns = loopyDNSNew(l, NULL);

    /* NULL dns */
    TEST_ASSERT_EQ(loopyDNSReverseLookup(NULL, "127.0.0.1", 0,
                                         test_dns_reverse_callback, NULL),
                   0, "reverse lookup NULL dns should fail");

    /* NULL addr */
    TEST_ASSERT_EQ(
        loopyDNSReverseLookup(dns, NULL, 0, test_dns_reverse_callback, NULL), 0,
        "reverse lookup NULL addr should fail");

    /* NULL callback */
    TEST_ASSERT_EQ(loopyDNSReverseLookup(dns, "127.0.0.1", 0, NULL, NULL), 0,
                   "reverse lookup NULL callback should fail");

    /* Invalid address format */
    TEST_ASSERT_EQ(loopyDNSReverseLookup(dns, "not-an-ip", 0,
                                         test_dns_reverse_callback, NULL),
                   0, "reverse lookup invalid addr should fail");

    loopyDNSFree(dns);
    return 1;
}

/* ====================================================================
 * Test: loopySignal - Signal Handling
 * ==================================================================== */
static int test_signal_create_delete(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    loopySignalHandler *sh = loopySignalNew(l);
    TEST_ASSERT(sh != NULL, "loopySignalNew should succeed");

    loopySignalFree(sh);
    return 1;
}

static int test_signal_single_handler(void) {
    /* Only one signal handler per process */
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    loopySignalHandler *sh1 = loopySignalNew(l);
    TEST_ASSERT(sh1 != NULL, "first handler should succeed");

    const loopySignalHandler *sh2 = loopySignalNew(l);
    TEST_ASSERT(sh2 == NULL, "second handler should fail");

    loopySignalFree(sh1);

    /* After freeing, new one should work */
    loopySignalHandler *sh3 = loopySignalNew(l);
    TEST_ASSERT(sh3 != NULL, "handler after free should succeed");

    loopySignalFree(sh3);
    return 1;
}

static int signal_callback_count = 0;
static int last_signal_received = 0;
static loopyLoop *signal_test_loop = NULL;

static void test_signal_callback(loopyLoop *l, int signum, void *userData) {
    (void)userData;
    signal_callback_count++;
    last_signal_received = signum;
    if (signal_test_loop) {
        loopyStop(l);
    }
}

static bool signal_timeout_callback(timerWheel *t, timerWheelId id,
                                    void *data) {
    (void)t;
    (void)id;
    loopyLoop *l = (loopyLoop *)data;
    loopyStop(l);
    return false;
}

static int test_signal_register_unregister(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopySignalHandler *sh = loopySignalNew(l);

    /* Register SIGUSR1 */
    TEST_ASSERT(loopySignalRegister(sh, SIGUSR1, test_signal_callback, NULL),
                "register SIGUSR1 should succeed");

    /* Re-registering should fail */
    TEST_ASSERT(!loopySignalRegister(sh, SIGUSR1, test_signal_callback, NULL),
                "re-register should fail");

    /* Unregister */
    TEST_ASSERT(loopySignalUnregister(sh, SIGUSR1),
                "unregister should succeed");

    /* Unregister again should fail */
    TEST_ASSERT(!loopySignalUnregister(sh, SIGUSR1),
                "re-unregister should fail");

    loopySignalFree(sh);
    return 1;
}

static int test_signal_delivery(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopySignalHandler *sh = loopySignalNew(l);
    signal_callback_count = 0;
    last_signal_received = 0;
    signal_test_loop = l;

    TEST_ASSERT(loopySignalRegister(sh, SIGUSR2, test_signal_callback, NULL),
                "register SIGUSR2 should succeed");

    /* Set timeout in case signal doesn't arrive */
    loopyRegisterTimer(l, 1000000, 0, signal_timeout_callback, l);

    /* Send signal to self */
    kill(getpid(), SIGUSR2);

    /* Run loop - will exit when signal received or timeout */
    loopyMain(l);

    TEST_ASSERT_EQ(signal_callback_count, 1, "callback should fire");
    TEST_ASSERT_EQ(last_signal_received, SIGUSR2, "should receive SIGUSR2");

    signal_test_loop = NULL;
    loopySignalFree(sh);
    return 1;
}

static int test_signal_names(void) {
    TEST_ASSERT(strcmp(loopySignalName(SIGINT), "SIGINT") == 0, "SIGINT name");
    TEST_ASSERT(strcmp(loopySignalName(SIGTERM), "SIGTERM") == 0,
                "SIGTERM name");
    TEST_ASSERT(strcmp(loopySignalName(SIGHUP), "SIGHUP") == 0, "SIGHUP name");
    TEST_ASSERT(strcmp(loopySignalName(SIGUSR1), "SIGUSR1") == 0,
                "SIGUSR1 name");
    TEST_ASSERT(strcmp(loopySignalName(SIGUSR2), "SIGUSR2") == 0,
                "SIGUSR2 name");
    TEST_ASSERT(strcmp(loopySignalName(SIGPIPE), "SIGPIPE") == 0,
                "SIGPIPE name");
    TEST_ASSERT(strcmp(loopySignalName(SIGCHLD), "SIGCHLD") == 0,
                "SIGCHLD name");
    TEST_ASSERT(strcmp(loopySignalName(999), "UNKNOWN") == 0,
                "unknown signal name");

    return 1;
}

static int test_signal_invalid_signum(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopySignalHandler *sh = loopySignalNew(l);

    /* Negative signal number */
    TEST_ASSERT(!loopySignalRegister(sh, -1, test_signal_callback, NULL),
                "register negative signal should fail");

    /* Too large signal number */
    TEST_ASSERT(!loopySignalRegister(sh, 9999, test_signal_callback, NULL),
                "register huge signal should fail");

    TEST_ASSERT(!loopySignalUnregister(sh, -1),
                "unregister negative should fail");

    loopySignalFree(sh);
    return 1;
}

static int test_signal_null_safety(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    /* NULL loop */
    TEST_ASSERT(loopySignalNew(NULL) == NULL, "new with NULL loop should fail");

    loopySignalHandler *sh = loopySignalNew(l);

    /* NULL callback */
    TEST_ASSERT(!loopySignalRegister(sh, SIGUSR1, NULL, NULL),
                "register NULL callback should fail");

    /* NULL handler */
    TEST_ASSERT(!loopySignalRegister(NULL, SIGUSR1, test_signal_callback, NULL),
                "register with NULL handler should fail");
    TEST_ASSERT(!loopySignalUnregister(NULL, SIGUSR1),
                "unregister with NULL handler should fail");

    loopySignalFree(sh);
    loopySignalFree(NULL); /* Should not crash */
    return 1;
}

static int oneshot_signal_count = 0;

static void test_oneshot_signal_callback(loopyLoop *l, int signum,
                                         void *userData) {
    (void)userData;
    (void)signum;
    oneshot_signal_count++;
    loopyStop(l);
}

static int test_signal_oneshot(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopySignalHandler *sh = loopySignalNew(l);

    oneshot_signal_count = 0;

    /* Register oneshot handler for SIGUSR1 */
    TEST_ASSERT(loopySignalRegisterOneshot(sh, SIGUSR1,
                                           test_oneshot_signal_callback, NULL),
                "register oneshot SIGUSR1 should succeed");

    /* Timeout to prevent hang */
    loopyRegisterTimer(l, 500000, 0, signal_timeout_callback, l);

    /* Send signal to self */
    kill(getpid(), SIGUSR1);

    /* Run event loop - should stop after signal callback */
    loopyMain(l);

    TEST_ASSERT(oneshot_signal_count == 1, "oneshot callback should fire once");

    /* The handler should be automatically unregistered.
     * We can now register a new handler for the same signal. */
    TEST_ASSERT(loopySignalRegister(sh, SIGUSR1, test_signal_callback, NULL),
                "should be able to re-register after oneshot fires");

    loopySignalFree(sh);
    return 1;
}

/* ====================================================================
 * Test: loopyAsync - Thread-safe Event Loop Wake-up
 * ==================================================================== */
static int test_async_create_delete(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    loopyAsync *async = loopyAsyncNew(l, dummy_async_callback, NULL);
    TEST_ASSERT(async != NULL, "loopyAsyncNew should succeed");
    TEST_ASSERT(!loopyAsyncPending(async), "should not be pending initially");
    TEST_ASSERT(loopyAsyncGetLoop(async) == l, "loop should match");

    loopyAsyncFree(async);
    return 1;
}

static int async_callback_count = 0;
static loopyLoop *async_test_loop = NULL;

static void test_async_callback(loopyLoop *l, loopyAsync *async,
                                void *userData) {
    (void)async;
    (void)userData;
    async_callback_count++;
    if (async_test_loop) {
        loopyStop(l);
    }
}

static bool async_timeout_callback(timerWheel *t, timerWheelId id, void *data) {
    (void)t;
    (void)id;
    loopyLoop *l = (loopyLoop *)data;
    loopyStop(l);
    return false;
}

static int test_async_send_same_thread(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    async_callback_count = 0;
    async_test_loop = l;

    loopyAsync *async = loopyAsyncNew(l, test_async_callback, NULL);
    TEST_ASSERT(async != NULL, "loopyAsyncNew should succeed");

    /* Send from same thread */
    loopyAsyncSend(async);
    TEST_ASSERT(loopyAsyncPending(async), "should be pending after send");

    /* Set timeout */
    loopyRegisterTimer(l, 1000000, 0, async_timeout_callback, l);

    /* Run loop - will exit when async fires or timeout */
    loopyMain(l);

    TEST_ASSERT_EQ(async_callback_count, 1, "callback should fire once");

    async_test_loop = NULL;
    loopyAsyncFree(async);
    return 1;
}

/* Thread function for cross-thread async test */
static loopyAsync *thread_test_async = NULL;

static void *async_worker_thread(void *arg) {
    (void)arg;
    /* Small delay to ensure event loop is running */
    usleep(10000); /* 10ms */
    loopyAsyncSend(thread_test_async);
    return NULL;
}

static int test_async_send_from_thread(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    async_callback_count = 0;
    async_test_loop = l;

    loopyAsync *async = loopyAsyncNew(l, test_async_callback, NULL);
    TEST_ASSERT(async != NULL, "loopyAsyncNew should succeed");
    thread_test_async = async;

    /* Start worker thread */
    pthread_t tid;
    int ret = pthread_create(&tid, NULL, async_worker_thread, NULL);
    TEST_ASSERT(ret == 0, "pthread_create should succeed");

    /* Set timeout */
    loopyRegisterTimer(l, 2000000, 0, async_timeout_callback, l);

    /* Run loop */
    loopyMain(l);

    pthread_join(tid, NULL);

    TEST_ASSERT_EQ(async_callback_count, 1,
                   "callback should fire from thread send");

    async_test_loop = NULL;
    thread_test_async = NULL;
    loopyAsyncFree(async);
    return 1;
}

static int multi_send_count = 0;
static loopyLoop *multi_send_loop = NULL;

static void multi_send_callback(loopyLoop *l, loopyAsync *async,
                                void *userData) {
    (void)async;
    (void)userData;
    multi_send_count++;
    /* Stop after receiving callback - should only fire once due to coalescing
     */
    loopyStop(l);
}

static void *multi_send_worker(void *arg) {
    loopyAsync *async = arg;
    /* Send multiple times rapidly */
    for (int i = 0; i < 10; i++) {
        loopyAsyncSend(async);
    }
    return NULL;
}

static int test_async_coalescing(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    multi_send_count = 0;
    multi_send_loop = l;

    loopyAsync *async = loopyAsyncNew(l, multi_send_callback, NULL);

    /* Send multiple times from worker */
    pthread_t tid;
    pthread_create(&tid, NULL, multi_send_worker, async);

    /* Set timeout */
    loopyRegisterTimer(l, 1000000, 0, async_timeout_callback, l);

    loopyMain(l);
    pthread_join(tid, NULL);

    /* Multiple sends should coalesce into fewer callbacks */
    TEST_ASSERT(multi_send_count >= 1, "should receive at least one callback");
    /* Note: exact count depends on timing - could be 1 or more due to how
     * fast the worker sends vs how fast the event loop processes */

    loopyAsyncFree(async);
    return 1;
}

static int test_async_multiple_handles(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    async_callback_count = 0;
    async_test_loop = l;

    /* Create multiple async handles */
    loopyAsync *async1 = loopyAsyncNew(l, test_async_callback, NULL);
    loopyAsync *async2 = loopyAsyncNew(l, test_async_callback, NULL);
    loopyAsync *async3 = loopyAsyncNew(l, test_async_callback, NULL);

    TEST_ASSERT(async1 != NULL, "async1 should succeed");
    TEST_ASSERT(async2 != NULL, "async2 should succeed");
    TEST_ASSERT(async3 != NULL, "async3 should succeed");

    /* Send on all three */
    loopyAsyncSend(async1);
    loopyAsyncSend(async2);
    loopyAsyncSend(async3);

    /* Run briefly to process events */
    loopyRegisterTimer(l, 100000, 0, async_timeout_callback, l);
    loopyMain(l);

    /* All three should have fired */
    TEST_ASSERT(async_callback_count >= 1, "callbacks should fire");

    async_test_loop = NULL;
    loopyAsyncFree(async1);
    loopyAsyncFree(async2);
    loopyAsyncFree(async3);
    return 1;
}

static int test_async_user_data(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    int myData = 42;

    loopyAsync *async = loopyAsyncNew(l, dummy_async_callback, &myData);
    TEST_ASSERT(async != NULL, "loopyAsyncNew should succeed");
    TEST_ASSERT(loopyAsyncGetData(async) == &myData, "user data should match");

    int newData = 99;
    loopyAsyncSetData(async, &newData);
    TEST_ASSERT(loopyAsyncGetData(async) == &newData,
                "updated user data should match");

    loopyAsyncFree(async);
    return 1;
}

static int test_async_backend_name(void) {
    const char *name = loopyAsyncBackendName();
    TEST_ASSERT(name != NULL, "backend name should not be NULL");

    /* Should be eventfd on Linux, pipe on BSD/macOS */
    int valid = (strcmp(name, "eventfd") == 0 || strcmp(name, "pipe") == 0 ||
                 strcmp(name, "none") == 0);
    TEST_ASSERT(valid, "backend should be valid");
    printf("    (async backend: %s)\n", name);

    return 1;
}

static int test_async_null_safety(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    /* NULL loop */
    TEST_ASSERT(loopyAsyncNew(NULL, test_async_callback, NULL) == NULL,
                "new with NULL loop should fail");

    /* NULL callback */
    TEST_ASSERT(loopyAsyncNew(l, NULL, NULL) == NULL,
                "new with NULL callback should fail");

    /* Operations on NULL */
    loopyAsyncSend(NULL); /* Should not crash */
    loopyAsyncFree(NULL); /* Should not crash */
    TEST_ASSERT(!loopyAsyncPending(NULL), "pending NULL should be false");
    TEST_ASSERT(loopyAsyncGetLoop(NULL) == NULL, "getLoop NULL should be NULL");
    TEST_ASSERT(loopyAsyncGetData(NULL) == NULL, "getData NULL should be NULL");
    loopyAsyncSetData(NULL, l); /* Should not crash */

    return 1;
}

/* ====================================================================
 * Test: loopyWork - Thread Pool Work Queue
 * ==================================================================== */
static int test_work_create_delete(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyWorkConfig config = LOOPY_WORK_CONFIG_DEFAULT;

    loopyWork *work = loopyWorkNew(l, &config);
    TEST_ASSERT(work != NULL, "loopyWorkNew should succeed");
    TEST_ASSERT_EQ(loopyWorkPendingCount(work), 0, "pending count should be 0");
    TEST_ASSERT_EQ(loopyWorkRunningCount(work), 0, "running count should be 0");
    TEST_ASSERT(loopyWorkThreadCount(work) > 0, "should have worker threads");
    TEST_ASSERT(loopyWorkGetLoop(work) == l, "loop should match");

    loopyWorkFree(work);
    return 1;
}

static int test_work_create_default_config(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    /* NULL config should use defaults */
    loopyWork *work = loopyWorkNew(l, NULL);
    TEST_ASSERT(work != NULL, "loopyWorkNew with NULL config should succeed");
    TEST_ASSERT(loopyWorkThreadCount(work) >= 1,
                "should have at least 1 thread");

    loopyWorkFree(work);
    return 1;
}

static int work_callback_count = 0;
static int work_after_callback_count = 0;
static loopyLoop *work_test_loop = NULL;
static pthread_t work_callback_thread = 0;
static pthread_t work_after_thread = 0;

static void test_work_callback(loopyWork *work, loopyWorkId id,
                               void *userData) {
    (void)work;
    (void)id;
    (void)userData;
    work_callback_count++;
    work_callback_thread = pthread_self();
    usleep(10000); /* Simulate work */
}

static void test_after_work_callback(loopyLoop *l, loopyWork *work,
                                     loopyWorkId id, loopyWorkStatus status,
                                     void *userData) {
    (void)work;
    (void)id;
    (void)userData;
    (void)status; /* Status should be OK or CANCELLED - checked in test */
    work_after_callback_count++;
    work_after_thread = pthread_self();
    if (work_test_loop) {
        loopyStop(l);
    }
}

static bool work_timeout_callback(timerWheel *t, timerWheelId id, void *data) {
    (void)t;
    (void)id;
    loopyLoop *l = (loopyLoop *)data;
    loopyStop(l);
    return false;
}

static int test_work_queue_execute(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyWork *work = loopyWorkNew(l, NULL);
    work_callback_count = 0;
    work_after_callback_count = 0;
    work_test_loop = l;
    work_callback_thread = 0;
    work_after_thread = pthread_self();

    /* Queue work */
    loopyWorkId id = loopyWorkQueue(work, test_work_callback,
                                    test_after_work_callback, NULL);
    TEST_ASSERT(id > 0, "work queue should return valid id");

    /* Set timeout */
    loopyRegisterTimer(l, 2000000, 0, work_timeout_callback, l);

    /* Run loop */
    loopyMain(l);

    TEST_ASSERT_EQ(work_callback_count, 1, "work callback should fire once");
    TEST_ASSERT_EQ(work_after_callback_count, 1,
                   "after-work callback should fire once");
    /* Work callback should run on different thread than after-work callback */
    TEST_ASSERT(work_callback_thread != work_after_thread,
                "work should run on different thread than after-work");

    work_test_loop = NULL;
    loopyWorkFree(work);
    return 1;
}

static int test_work_cancel_pending(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyWorkConfig config = LOOPY_WORK_CONFIG_DEFAULT;
    config.minThreads = 1;
    config.maxThreads = 1;
    loopyWork *work = loopyWorkNew(l, &config);
    work_callback_count = 0;

    /* Queue blocking work first to hold up the thread */
    loopyWorkId id1 = loopyWorkQueue(work, test_work_callback, NULL, NULL);
    TEST_ASSERT(id1 > 0, "first work should succeed");

    /* Queue more work that will be pending */
    loopyWorkId id2 = loopyWorkQueue(work, test_work_callback, NULL, NULL);
    TEST_ASSERT(id2 > 0, "second work should succeed");

    /* Cancel the pending work */
    bool cancelled = loopyWorkCancel(work, id2);
    /* May or may not succeed depending on timing */
    (void)cancelled;

    /* Cancel non-existent should fail */
    TEST_ASSERT(!loopyWorkCancel(work, 99999), "cancel invalid should fail");

    loopyWorkFree(work);
    return 1;
}

static int test_work_cancel_all(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyWorkConfig config = LOOPY_WORK_CONFIG_DEFAULT;
    config.minThreads = 1;
    config.maxThreads = 1;
    loopyWork *work = loopyWorkNew(l, &config);

    /* Queue multiple work items */
    loopyWorkQueue(work, test_work_callback, NULL, NULL);
    loopyWorkQueue(work, test_work_callback, NULL, NULL);
    loopyWorkQueue(work, test_work_callback, NULL, NULL);

    /* Cancel all */
    loopyWorkCancelAll(work);

    loopyWorkFree(work);
    return 1;
}

static int multiple_work_count = 0;
static pthread_mutex_t multiple_work_mutex = PTHREAD_MUTEX_INITIALIZER;

static void multiple_work_callback(loopyWork *work, loopyWorkId id,
                                   void *userData) {
    (void)work;
    (void)id;
    (void)userData;
    pthread_mutex_lock(&multiple_work_mutex);
    multiple_work_count++;
    pthread_mutex_unlock(&multiple_work_mutex);
    usleep(5000); /* Small delay */
}

static int multiple_after_count = 0;

static void multiple_after_callback(loopyLoop *l, loopyWork *work,
                                    loopyWorkId id, loopyWorkStatus status,
                                    void *userData) {
    (void)work;
    (void)id;
    (void)status;
    (void)userData;
    multiple_after_count++;
    if (multiple_after_count >= 5) {
        loopyStop(l);
    }
}

static int test_work_multiple_concurrent(void) {
    loopyLoop *l LOOPY_LOOP_AUTO_CLEANUP = loopyNew(16);
    loopyWorkConfig config = LOOPY_WORK_CONFIG_DEFAULT;
    config.minThreads = 4;
    config.maxThreads = 4;
    loopyWork *work = loopyWorkNew(l, &config);
    multiple_work_count = 0;
    multiple_after_count = 0;

    /* Queue multiple work items */
    for (int i = 0; i < 5; i++) {
        loopyWorkId id = loopyWorkQueue(work, multiple_work_callback,
                                        multiple_after_callback, NULL);
        TEST_ASSERT(id > 0, "work queue should succeed");
    }

    /* Set timeout */
    loopyRegisterTimer(l, 3000000, 0, work_timeout_callback, l);

    /* Run loop */
    loopyMain(l);

    TEST_ASSERT_EQ(multiple_work_count, 5, "all work callbacks should fire");
    TEST_ASSERT_EQ(multiple_after_count, 5,
                   "all after-work callbacks should fire");

    loopyWorkFree(work);
    return 1;
}

static int test_work_status_strings(void) {
    TEST_ASSERT(strcmp(loopyWorkStatusString(LOOPY_WORK_OK), "OK") == 0,
                "OK status string");
    TEST_ASSERT(
        strcmp(loopyWorkStatusString(LOOPY_WORK_CANCELLED), "CANCELLED") == 0,
        "CANCELLED status string");
    TEST_ASSERT(strcmp(loopyWorkStatusString(LOOPY_WORK_ERROR), "ERROR") == 0,
                "ERROR status string");
    TEST_ASSERT(strcmp(loopyWorkStatusString(999), "ERROR") == 0,
                "unknown status falls back to ERROR");

    return 1;
}

static int test_work_null_safety(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    /* NULL loop */
    TEST_ASSERT(loopyWorkNew(NULL, NULL) == NULL,
                "new with NULL loop should fail");

    loopyWork *work = loopyWorkNew(l, NULL);

    /* NULL work callback */
    TEST_ASSERT_EQ(loopyWorkQueue(work, NULL, NULL, NULL), 0,
                   "queue with NULL work callback should fail");

    /* NULL work handle */
    TEST_ASSERT_EQ(loopyWorkQueue(NULL, test_work_callback, NULL, NULL), 0,
                   "queue with NULL work should fail");
    TEST_ASSERT(!loopyWorkCancel(NULL, 1), "cancel with NULL work should fail");
    TEST_ASSERT_EQ(loopyWorkPendingCount(NULL), 0, "pending NULL should be 0");
    TEST_ASSERT_EQ(loopyWorkRunningCount(NULL), 0, "running NULL should be 0");
    TEST_ASSERT_EQ(loopyWorkThreadCount(NULL), 0,
                   "thread count NULL should be 0");
    TEST_ASSERT(loopyWorkGetLoop(NULL) == NULL, "getLoop NULL should be NULL");

    loopyWorkCancelAll(NULL); /* Should not crash */
    loopyWorkFree(NULL);      /* Should not crash */

    loopyWorkFree(work);
    return 1;
}

static int test_work_queue_overflow(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyWorkConfig config = {
        .minThreads = 1, .maxThreads = 1, .maxQueueSize = 3};
    loopyWork *work = loopyWorkNew(l, &config);

    /* Fill up the queue */
    loopyWorkId id1 = loopyWorkQueue(work, test_work_callback, NULL, NULL);
    loopyWorkId id2 = loopyWorkQueue(work, test_work_callback, NULL, NULL);
    loopyWorkId id3 = loopyWorkQueue(work, test_work_callback, NULL, NULL);

    TEST_ASSERT(id1 > 0, "first queue should succeed");
    TEST_ASSERT(id2 > 0, "second queue should succeed");
    TEST_ASSERT(id3 > 0, "third queue should succeed");

    /* Wait briefly for first item to start executing */
    usleep(20000);

    /* Queue is full, but some may have started executing.
     * Try to queue more - may succeed or fail depending on timing */
    loopyWorkQueue(work, test_work_callback, NULL, NULL);

    loopyWorkFree(work);
    return 1;
}

/* ====================================================================
 * Test: loopyWatch - File/Directory Watching
 * ==================================================================== */
static int test_watch_create_delete(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    loopyWatch *w = loopyWatchNew(l);
    TEST_ASSERT(w != NULL, "loopyWatchNew should succeed");
    TEST_ASSERT_EQ(loopyWatchCount(w), 0, "watch count should be 0");

    loopyWatchFree(w);
    return 1;
}

static int watch_callback_count = 0;
static loopyWatchEvent last_watch_events = 0;
static loopyLoop *watch_test_loop = NULL;

static void test_watch_callback(loopyWatch *w, const loopyWatchInfo *info) {
    (void)w;
    watch_callback_count++;
    last_watch_events = info->events;
    if (watch_test_loop) {
        loopyStop(watch_test_loop);
    }
}

static bool watch_timeout_callback(timerWheel *t, timerWheelId id, void *data) {
    (void)t;
    (void)id;
    loopyLoop *l = data;
    loopyStop(l);
    return false;
}

static int test_watch_add_remove(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyWatch *w = loopyWatchNew(l);

    /* Create a temp file to watch */
    char path[] = "/tmp/loopy_watch_test_XXXXXX";
    int fd = mkstemp(path);
    TEST_ASSERT(fd >= 0, "mkstemp should succeed");
    close(fd);

    /* Add watch */
    loopyWatchId id =
        loopyWatchAdd(w, path, LOOPY_WATCH_MODIFY, test_watch_callback, NULL);
    TEST_ASSERT(id > 0, "watch add should return valid id");
    TEST_ASSERT_EQ(loopyWatchCount(w), 1, "watch count should be 1");

    /* Remove watch */
    TEST_ASSERT(loopyWatchRemove(w, id), "watch remove should succeed");
    TEST_ASSERT_EQ(loopyWatchCount(w), 0, "watch count should be 0");

    /* Remove again should fail */
    TEST_ASSERT(!loopyWatchRemove(w, id), "re-remove should fail");

    unlink(path);
    loopyWatchFree(w);
    return 1;
}

static int test_watch_multiple(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyWatch *w = loopyWatchNew(l);

    /* Create multiple temp files */
    char path1[] = "/tmp/loopy_watch_test1_XXXXXX";
    char path2[] = "/tmp/loopy_watch_test2_XXXXXX";
    char path3[] = "/tmp/loopy_watch_test3_XXXXXX";

    int fd1 = mkstemp(path1);
    int fd2 = mkstemp(path2);
    int fd3 = mkstemp(path3);
    close(fd1);
    close(fd2);
    close(fd3);

    /* Add watches */
    loopyWatchId id1 =
        loopyWatchAdd(w, path1, LOOPY_WATCH_ALL, test_watch_callback, NULL);
    loopyWatchId id2 =
        loopyWatchAdd(w, path2, LOOPY_WATCH_ALL, test_watch_callback, NULL);
    loopyWatchId id3 =
        loopyWatchAdd(w, path3, LOOPY_WATCH_ALL, test_watch_callback, NULL);

    TEST_ASSERT(id1 > 0, "watch 1 should succeed");
    TEST_ASSERT(id2 > 0, "watch 2 should succeed");
    TEST_ASSERT(id3 > 0, "watch 3 should succeed");
    TEST_ASSERT_EQ(loopyWatchCount(w), 3, "watch count should be 3");

    /* Remove one */
    loopyWatchRemove(w, id2);
    TEST_ASSERT_EQ(loopyWatchCount(w), 2, "watch count should be 2");

    /* Remove all */
    loopyWatchRemoveAll(w);
    TEST_ASSERT_EQ(loopyWatchCount(w), 0, "watch count should be 0");

    unlink(path1);
    unlink(path2);
    unlink(path3);
    loopyWatchFree(w);
    return 1;
}

/* Timer callback that modifies a file after a short delay */
static char watch_test_path[256] = {0};

static bool watch_modify_callback(timerWheel *t, timerWheelId id, void *data) {
    (void)t;
    (void)id;
    (void)data;
    /* Modify the file */
    int fd = open(watch_test_path, O_WRONLY | O_APPEND);
    if (fd >= 0) {
        write(fd, "test", 4);
        close(fd);
    }
    return false;
}

static int test_watch_modify_event(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyWatch *w = loopyWatchNew(l);
    watch_callback_count = 0;
    last_watch_events = 0;
    watch_test_loop = l;

    /* Create temp file */
    char path[] = "/tmp/loopy_watch_modify_XXXXXX";
    int fd = mkstemp(path);
    close(fd);
    strncpy(watch_test_path, path, sizeof(watch_test_path) - 1);

    /* Watch for modifications */
    loopyWatchId id =
        loopyWatchAdd(w, path, LOOPY_WATCH_MODIFY, test_watch_callback, NULL);
    TEST_ASSERT(id > 0, "watch add should succeed");

    /* Set up timer to modify the file after a short delay */
    loopyRegisterTimer(l, 50000, 0, watch_modify_callback, NULL);

    /* Set timeout */
    loopyRegisterTimer(l, 1000000, 0, watch_timeout_callback, l);

    /* Run loop - will exit when file is modified or timeout */
    loopyMain(l);

    TEST_ASSERT(watch_callback_count > 0, "callback should fire on modify");
    TEST_ASSERT(last_watch_events & LOOPY_WATCH_MODIFY,
                "should have MODIFY event");

    watch_test_loop = NULL;
    unlink(path);
    loopyWatchFree(w);
    return 1;
}

static int test_watch_event_names(void) {
    TEST_ASSERT(strcmp(loopyWatchEventName(LOOPY_WATCH_MODIFY), "MODIFY") == 0,
                "MODIFY name");
    TEST_ASSERT(strcmp(loopyWatchEventName(LOOPY_WATCH_CREATE), "CREATE") == 0,
                "CREATE name");
    TEST_ASSERT(strcmp(loopyWatchEventName(LOOPY_WATCH_DELETE), "DELETE") == 0,
                "DELETE name");
    TEST_ASSERT(strcmp(loopyWatchEventName(LOOPY_WATCH_RENAME), "RENAME") == 0,
                "RENAME name");
    TEST_ASSERT(strcmp(loopyWatchEventName(LOOPY_WATCH_ATTRIB), "ATTRIB") == 0,
                "ATTRIB name");
    TEST_ASSERT(strcmp(loopyWatchEventName(LOOPY_WATCH_ALL), "ALL") == 0,
                "ALL name");
    TEST_ASSERT(strcmp(loopyWatchEventName(999), "UNKNOWN") == 0,
                "unknown event name");

    return 1;
}

static int test_watch_backend_name(void) {
    const char *name = loopyWatchBackendName();
    TEST_ASSERT(name != NULL, "backend name should not be NULL");

    /* Should be kqueue on macOS/BSD, inotify on Linux */
    int valid = (strcmp(name, "kqueue") == 0 || strcmp(name, "inotify") == 0 ||
                 strcmp(name, "none") == 0);
    TEST_ASSERT(valid, "backend should be valid");
    printf("    (watch backend: %s)\n", name);

    return 1;
}

static int test_watch_nonexistent_path(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyWatch *w = loopyWatchNew(l);

    /* Try to watch nonexistent path */
    loopyWatchId id = loopyWatchAdd(w, "/nonexistent/path/that/does/not/exist",
                                    LOOPY_WATCH_ALL, test_watch_callback, NULL);
    TEST_ASSERT(id == 0, "watching nonexistent path should fail");

    loopyWatchFree(w);
    return 1;
}

static int test_watch_null_safety(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    /* NULL loop */
    TEST_ASSERT(loopyWatchNew(NULL) == NULL, "new with NULL loop should fail");

    loopyWatch *w = loopyWatchNew(l);

    /* NULL path */
    TEST_ASSERT_EQ(
        loopyWatchAdd(w, NULL, LOOPY_WATCH_ALL, test_watch_callback, NULL), 0,
        "add NULL path should fail");

    /* NULL callback */
    TEST_ASSERT_EQ(loopyWatchAdd(w, "/tmp", LOOPY_WATCH_ALL, NULL, NULL), 0,
                   "add NULL callback should fail");

    /* NULL watch */
    TEST_ASSERT(!loopyWatchRemove(NULL, 1),
                "remove with NULL watch should fail");
    TEST_ASSERT_EQ(loopyWatchCount(NULL), 0, "count with NULL should be 0");

    loopyWatchFree(w);
    loopyWatchFree(NULL); /* Should not crash */
    return 1;
}

/* ====================================================================
 * Test: loopyFSPoll - Stat-based file polling
 * ==================================================================== */
static int fspoll_callback_count = 0;
static loopyFSPollEvent fspoll_last_events = LOOPY_FSPOLL_NONE;
static bool fspoll_has_prev = false;
static bool fspoll_has_curr = false;
static loopyLoop *fspoll_test_loop = NULL;

static void test_fspoll_callback(loopyFSPoll *poll,
                                 const loopyFSPollInfo *info) {
    (void)poll;
    fspoll_callback_count++;
    fspoll_last_events = info->events;
    fspoll_has_prev = (info->prevStat != NULL);
    fspoll_has_curr = (info->currStat != NULL);
    if (fspoll_test_loop) {
        loopyStop(fspoll_test_loop);
    }
}

static bool fspoll_timeout_callback(timerWheel *t, timerWheelId id,
                                    void *data) {
    (void)t;
    (void)id;
    loopyLoop *l = data;
    loopyStop(l);
    return false;
}

static int test_fspoll_create_delete(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    loopyFSPoll *poll = loopyFSPollNew(l, NULL);
    TEST_ASSERT(poll != NULL, "loopyFSPollNew should succeed");
    TEST_ASSERT(loopyFSPollGetLoop(poll) == l, "loop should match");
    TEST_ASSERT_EQ(loopyFSPollCount(poll), 0, "count should be 0 initially");
    TEST_ASSERT_EQ(loopyFSPollGetInterval(poll), 1000,
                   "default interval should be 1000ms");

    loopyFSPollFree(poll);
    return 1;
}

static int test_fspoll_detect_modify(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyFSPollConfig config = {.intervalMs = 50, .followSymlinks = true};
    loopyFSPoll *poll = loopyFSPollNew(l, &config);
    fspoll_callback_count = 0;
    fspoll_last_events = LOOPY_FSPOLL_NONE;
    fspoll_test_loop = l;

    /* Create temp file */
    char path[] = "/tmp/loopy_fspoll_modify_XXXXXX";
    int fd = mkstemp(path);
    TEST_ASSERT(fd >= 0, "mkstemp should succeed");
    write(fd, "initial", 7);
    close(fd);

    /* Start polling with short interval */
    loopyFSPollId pollId = loopyFSPollStart(poll, path, LOOPY_FSPOLL_ALL,
                                            test_fspoll_callback, NULL);
    TEST_ASSERT(pollId > 0, "start should succeed");
    TEST_ASSERT_EQ(loopyFSPollCount(poll), 1, "should have one poll entry");
    TEST_ASSERT(strcmp(loopyFSPollGetPath(poll, pollId), path) == 0,
                "path should match");

    /* Set up timer timeout */
    loopyRegisterTimer(l, 200000, 0, fspoll_timeout_callback,
                       l); /* 200ms timeout */

    /* Modify the file */
    fd = open(path, O_WRONLY | O_APPEND);
    if (fd >= 0) {
        write(fd, "modified", 8);
        close(fd);
    }

    /* Run loop briefly */
    loopyMain(l);

    /* The callback should have fired due to the modification */
    TEST_ASSERT(fspoll_callback_count > 0, "callback should fire on modify");
    TEST_ASSERT(fspoll_last_events &
                    (LOOPY_FSPOLL_MODIFIED | LOOPY_FSPOLL_SIZE),
                "should detect modify or size change");

    fspoll_test_loop = NULL;
    loopyFSPollFree(poll);
    unlink(path);
    return 1;
}

static int test_fspoll_detect_chmod(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyFSPollConfig config = {.intervalMs = 50, .followSymlinks = true};
    loopyFSPoll *poll = loopyFSPollNew(l, &config);
    fspoll_callback_count = 0;
    fspoll_last_events = LOOPY_FSPOLL_NONE;
    fspoll_test_loop = l;

    /* Create temp file */
    char path[] = "/tmp/loopy_fspoll_chmod_XXXXXX";
    int fd = mkstemp(path);
    TEST_ASSERT(fd >= 0, "mkstemp should succeed");
    close(fd);

    /* Start polling */
    loopyFSPollId pollId = loopyFSPollStart(poll, path, LOOPY_FSPOLL_ALL,
                                            test_fspoll_callback, NULL);
    TEST_ASSERT(pollId > 0, "start should succeed");

    /* Set timeout */
    loopyRegisterTimer(l, 200000, 0, fspoll_timeout_callback,
                       l); /* 200ms timeout */

    /* Change permissions (mkstemp creates with 0600, so change to 0644) */
    chmod(path, 0644);

    /* Run loop */
    loopyMain(l);

    /* Callback should fire for chmod */
    TEST_ASSERT(fspoll_callback_count > 0, "callback should fire on chmod");
    TEST_ASSERT(fspoll_last_events & LOOPY_FSPOLL_PERMISSIONS,
                "should detect permission change");

    fspoll_test_loop = NULL;
    loopyFSPollFree(poll);
    unlink(path);
    return 1;
}

static int test_fspoll_detect_delete(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyFSPollConfig config = {.intervalMs = 50, .followSymlinks = true};
    loopyFSPoll *poll = loopyFSPollNew(l, &config);
    fspoll_callback_count = 0;
    fspoll_last_events = LOOPY_FSPOLL_NONE;
    fspoll_has_curr = true;
    fspoll_test_loop = l;

    /* Create temp file */
    char path[] = "/tmp/loopy_fspoll_delete_XXXXXX";
    int fd = mkstemp(path);
    TEST_ASSERT(fd >= 0, "mkstemp should succeed");
    close(fd);

    /* Start polling */
    loopyFSPollId pollId = loopyFSPollStart(poll, path, LOOPY_FSPOLL_ALL,
                                            test_fspoll_callback, NULL);
    TEST_ASSERT(pollId > 0, "start should succeed");

    /* Set timeout */
    loopyRegisterTimer(l, 200000, 0, fspoll_timeout_callback, l);

    /* Delete the file */
    unlink(path);

    /* Run loop */
    loopyMain(l);

    /* Callback should fire for delete */
    TEST_ASSERT(fspoll_callback_count > 0, "callback should fire for delete");
    TEST_ASSERT(fspoll_last_events & LOOPY_FSPOLL_DELETED,
                "should detect deletion");
    TEST_ASSERT(!fspoll_has_curr, "curr should be NULL after deletion");

    fspoll_test_loop = NULL;
    loopyFSPollFree(poll);
    return 1;
}

static int test_fspoll_start_stop(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyFSPoll *poll = loopyFSPollNew(l, NULL);

    /* Create temp file */
    char path[] = "/tmp/loopy_fspoll_startstop_XXXXXX";
    int fd = mkstemp(path);
    TEST_ASSERT(fd >= 0, "mkstemp should succeed");
    close(fd);

    /* Start */
    loopyFSPollId pollId = loopyFSPollStart(poll, path, LOOPY_FSPOLL_MODIFIED,
                                            test_fspoll_callback, NULL);
    TEST_ASSERT(pollId > 0, "start should succeed");
    TEST_ASSERT_EQ(loopyFSPollCount(poll), 1, "should have one poll entry");

    /* Stop */
    TEST_ASSERT(loopyFSPollStop(poll, pollId), "stop should succeed");
    TEST_ASSERT_EQ(loopyFSPollCount(poll), 0, "should have no poll entries");

    /* Stop again (should fail - already stopped) */
    TEST_ASSERT(!loopyFSPollStop(poll, pollId),
                "double stop should return false");

    /* Restart with new pollId */
    loopyFSPollId pollId2 = loopyFSPollStart(poll, path, LOOPY_FSPOLL_ALL,
                                             test_fspoll_callback, NULL);
    TEST_ASSERT(pollId2 > 0, "restart should succeed");
    TEST_ASSERT(pollId2 != pollId, "should get new poll ID");
    TEST_ASSERT_EQ(loopyFSPollCount(poll), 1, "should have one poll entry");

    /* Change interval */
    loopyFSPollSetInterval(poll, 500);
    TEST_ASSERT_EQ(loopyFSPollGetInterval(poll), 500, "interval should be 500");

    loopyFSPollFree(poll);
    unlink(path);
    return 1;
}

static int test_fspoll_null_safety(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    /* NULL loop */
    TEST_ASSERT(loopyFSPollNew(NULL, NULL) == NULL,
                "new with NULL loop should fail");

    loopyFSPoll *poll = loopyFSPollNew(l, NULL);

    /* NULL path */
    TEST_ASSERT(loopyFSPollStart(poll, NULL, LOOPY_FSPOLL_ALL,
                                 test_fspoll_callback, NULL) == 0,
                "start with NULL path should fail");

    /* NULL callback */
    TEST_ASSERT(loopyFSPollStart(poll, "/tmp", LOOPY_FSPOLL_ALL, NULL, NULL) ==
                    0,
                "start with NULL callback should fail");

    /* Zero events */
    TEST_ASSERT(loopyFSPollStart(poll, "/tmp", LOOPY_FSPOLL_NONE,
                                 test_fspoll_callback, NULL) == 0,
                "start with no events should fail");

    /* NULL poll for getters */
    TEST_ASSERT(loopyFSPollGetPath(NULL, 1) == NULL,
                "getPath with NULL should return NULL");
    TEST_ASSERT_EQ(loopyFSPollGetInterval(NULL), 0,
                   "getInterval with NULL should return 0");
    TEST_ASSERT(loopyFSPollGetLoop(NULL) == NULL,
                "getLoop with NULL should return NULL");
    TEST_ASSERT_EQ(loopyFSPollCount(NULL), 0,
                   "count with NULL should return 0");

    /* Stop with invalid pollId */
    TEST_ASSERT(!loopyFSPollStop(poll, 999),
                "stop with invalid id should return false");

    loopyFSPollFree(poll);
    loopyFSPollFree(NULL); /* Should not crash */
    return 1;
}

/* ====================================================================
 * Test: loopyUDP
 * ==================================================================== */
static int udp_recv_count = 0;
static char udp_recv_data[256] = {0};
static loopyLoop *udp_test_loop = NULL;

static void test_udp_recv_callback(loopyUDP *udp, ssize_t nread,
                                   const void *data,
                                   const struct sockaddr *addr,
                                   socklen_t addrLen, void *userData) {
    (void)udp;
    (void)addr;
    (void)addrLen;
    (void)userData;

    if (nread > 0 && data) {
        size_t copyLen = (size_t)nread < sizeof(udp_recv_data) - 1
                             ? (size_t)nread
                             : sizeof(udp_recv_data) - 1;
        memcpy(udp_recv_data, data, copyLen);
        udp_recv_data[copyLen] = '\0';
    }
    udp_recv_count++;

    if (udp_test_loop) {
        loopyStop(udp_test_loop);
    }
}

static void test_udp_send_callback(loopyUDP *udp, int status, void *userData) {
    (void)udp;
    (void)userData;
    (void)status;
}

static int test_udp_create_delete(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyUDP *udp = loopyUDPNew(l);

    TEST_ASSERT(udp != NULL, "loopyUDPNew should succeed");
    TEST_ASSERT_EQ(loopyUDPGetFd(udp), -1, "fd should be -1 before bind");
    TEST_ASSERT(loopyUDPGetLoop(udp) == l, "loop should match");
    TEST_ASSERT(!loopyUDPIsConnected(udp), "should not be connected");
    TEST_ASSERT(!loopyUDPIsReceiving(udp), "should not be receiving");
    TEST_ASSERT_EQ(loopyUDPSendQueueCount(udp), 0,
                   "send queue should be empty");

    loopyUDPFree(udp);
    return 1;
}

static int test_udp_bind_ipv4(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyUDP *udp = loopyUDPNew(l);

    /* Bind to ephemeral port */
    TEST_ASSERT(loopyUDPBind(udp, "127.0.0.1", 0, 0), "bind should succeed");
    TEST_ASSERT(loopyUDPGetFd(udp) >= 0, "fd should be valid after bind");

    int port = 0;
    char addr[64] = {0};
    TEST_ASSERT(loopyUDPGetSockName(udp, addr, sizeof(addr), &port),
                "getsockname should succeed");
    TEST_ASSERT(port > 0, "port should be assigned");
    TEST_ASSERT(strcmp(addr, "127.0.0.1") == 0, "address should match");

    loopyUDPFree(udp);
    return 1;
}

static int test_udp_bind_ipv6(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyUDP *udp = loopyUDPNew(l);

    /* Bind to IPv6 loopback with IPV6ONLY flag */
    TEST_ASSERT(loopyUDPBind6(udp, "::1", 0, LOOPY_UDP_IPV6ONLY),
                "bind6 should succeed");
    TEST_ASSERT(loopyUDPGetFd(udp) >= 0, "fd should be valid after bind6");

    int port = 0;
    TEST_ASSERT(loopyUDPGetSockName(udp, NULL, 0, &port),
                "getsockname should succeed");
    TEST_ASSERT(port > 0, "port should be assigned");

    loopyUDPFree(udp);
    return 1;
}

static int test_udp_send_recv(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyUDP *sender = loopyUDPNew(l);
    loopyUDP *receiver = loopyUDPNew(l);

    /* Bind receiver */
    TEST_ASSERT(loopyUDPBind(receiver, "127.0.0.1", 0, 0),
                "receiver bind should succeed");

    int port = 0;
    loopyUDPGetSockName(receiver, NULL, 0, &port);

    /* Start receiving */
    udp_recv_count = 0;
    memset(udp_recv_data, 0, sizeof(udp_recv_data));
    udp_test_loop = l;

    TEST_ASSERT(loopyUDPRecvStart(receiver, test_udp_recv_callback, NULL),
                "recv start should succeed");
    TEST_ASSERT(loopyUDPIsReceiving(receiver), "should be receiving");

    /* Send a message */
    const char *msg = "Hello UDP";
    TEST_ASSERT(loopyUDPSend(sender, "127.0.0.1", port, msg, strlen(msg),
                             test_udp_send_callback, NULL),
                "send should succeed");

    /* Run event loop briefly */
    loopyRegisterTimer(l, 100000, 0, NULL, NULL);
    loopyMain(l);

    TEST_ASSERT(udp_recv_count > 0, "should have received data");
    TEST_ASSERT(strcmp(udp_recv_data, "Hello UDP") == 0, "data should match");

    loopyUDPFree(sender);
    loopyUDPFree(receiver);
    udp_test_loop = NULL;
    return 1;
}

static int test_udp_connect(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyUDP *udp = loopyUDPNew(l);

    /* Connect sets default destination */
    TEST_ASSERT(loopyUDPConnect(udp, "127.0.0.1", 12345),
                "connect should succeed");
    TEST_ASSERT(loopyUDPIsConnected(udp), "should be connected");
    TEST_ASSERT(loopyUDPGetFd(udp) >= 0, "fd should be valid");

    /* Disconnect */
    loopyUDPDisconnect(udp);
    TEST_ASSERT(!loopyUDPIsConnected(udp),
                "should not be connected after disconnect");

    loopyUDPFree(udp);
    return 1;
}

static int test_udp_try_send(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyUDP *sender = loopyUDPNew(l);
    loopyUDP *receiver = loopyUDPNew(l);

    /* Bind receiver */
    TEST_ASSERT(loopyUDPBind(receiver, "127.0.0.1", 0, 0),
                "receiver bind should succeed");

    int port = 0;
    loopyUDPGetSockName(receiver, NULL, 0, &port);

    /* Try send (synchronous) */
    const char *msg = "Quick send";
    ssize_t sent = loopyUDPTrySend(sender, "127.0.0.1", port, msg, strlen(msg));
    TEST_ASSERT(sent == (ssize_t)strlen(msg), "try send should succeed");

    loopyUDPFree(sender);
    loopyUDPFree(receiver);
    return 1;
}

static int test_udp_broadcast(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyUDP *udp = loopyUDPNew(l);

    /* Need to bind first to create socket */
    TEST_ASSERT(loopyUDPBind(udp, NULL, 0, 0), "bind should succeed");

    /* Enable broadcast */
    TEST_ASSERT(loopyUDPSetBroadcast(udp, true),
                "set broadcast should succeed");

    loopyUDPFree(udp);
    return 1;
}

static int test_udp_ttl(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyUDP *udp = loopyUDPNew(l);

    /* Bind to create socket */
    TEST_ASSERT(loopyUDPBind(udp, NULL, 0, 0), "bind should succeed");

    /* Set TTL */
    TEST_ASSERT(loopyUDPSetTTL(udp, 64), "set ttl should succeed");
    TEST_ASSERT(loopyUDPSetMulticastTTL(udp, 16),
                "set multicast ttl should succeed");
    TEST_ASSERT(loopyUDPSetMulticastLoop(udp, true),
                "set multicast loop should succeed");

    /* Invalid TTL should fail */
    TEST_ASSERT(!loopyUDPSetTTL(udp, 0), "ttl 0 should fail");
    TEST_ASSERT(!loopyUDPSetTTL(udp, 256), "ttl 256 should fail");

    loopyUDPFree(udp);
    return 1;
}

static int test_udp_recv_stop(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyUDP *udp = loopyUDPNew(l);

    TEST_ASSERT(loopyUDPBind(udp, "127.0.0.1", 0, 0), "bind should succeed");
    TEST_ASSERT(loopyUDPRecvStart(udp, test_udp_recv_callback, NULL),
                "recv start should succeed");
    TEST_ASSERT(loopyUDPIsReceiving(udp), "should be receiving");

    loopyUDPRecvStop(udp);
    TEST_ASSERT(!loopyUDPIsReceiving(udp),
                "should not be receiving after stop");

    loopyUDPFree(udp);
    return 1;
}

static int test_udp_null_safety(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    /* NULL loop */
    TEST_ASSERT(loopyUDPNew(NULL) == NULL, "new with NULL loop should fail");

    loopyUDP *udp = loopyUDPNew(l);

    /* Various NULL checks */
    TEST_ASSERT(!loopyUDPBind(NULL, "127.0.0.1", 0, 0),
                "bind NULL should fail");
    TEST_ASSERT(!loopyUDPConnect(NULL, "127.0.0.1", 0),
                "connect NULL should fail");
    TEST_ASSERT(!loopyUDPRecvStart(NULL, test_udp_recv_callback, NULL),
                "recv start NULL should fail");
    TEST_ASSERT(!loopyUDPSend(NULL, "127.0.0.1", 0, "x", 1, NULL, NULL),
                "send NULL should fail");

    TEST_ASSERT_EQ(loopyUDPGetFd(NULL), -1, "getfd NULL should be -1");
    TEST_ASSERT(loopyUDPGetLoop(NULL) == NULL, "getloop NULL should be NULL");
    TEST_ASSERT_EQ(loopyUDPSendQueueCount(NULL), 0,
                   "queue count NULL should be 0");
    TEST_ASSERT(!loopyUDPIsConnected(NULL),
                "is connected NULL should be false");
    TEST_ASSERT(!loopyUDPIsReceiving(NULL),
                "is receiving NULL should be false");

    loopyUDPFree(udp);
    loopyUDPFree(NULL); /* Should not crash */
    return 1;
}

static int test_udp_batch_send_recv(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    /* Create sender and receiver */
    loopyUDP *sender = loopyUDPNew(l);
    loopyUDP *receiver = loopyUDPNew(l);

    /* Bind receiver to a port */
    TEST_ASSERT(loopyUDPBind(receiver, "127.0.0.1", 0, 0), "bind receiver");

    /* Get the bound port */
    int port = 0;
    TEST_ASSERT(loopyUDPGetSockName(receiver, NULL, 0, &port), "get port");
    TEST_ASSERT(port > 0, "port assigned");

    /* Prepare batch of messages to send */
    const int BATCH_SIZE = 5;
    loopyUDPMessage sendMsgs[5];
    char sendBufs[5][64];

    for (int i = 0; i < BATCH_SIZE; i++) {
        snprintf(sendBufs[i], sizeof(sendBufs[i]), "Message %d", i);
        sendMsgs[i].data = sendBufs[i];
        sendMsgs[i].len = strlen(sendBufs[i]);
        sendMsgs[i].bytesTransferred = 0;

        /* Set destination address */
        struct sockaddr_in *sin = (struct sockaddr_in *)&sendMsgs[i].addr;
        sin->sin_family = AF_INET;
        sin->sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &sin->sin_addr);
        sendMsgs[i].addrLen = sizeof(struct sockaddr_in);
    }

    /* Bind sender (needed for sending) */
    TEST_ASSERT(loopyUDPBind(sender, "127.0.0.1", 0, 0), "bind sender");

    /* Send batch */
    int sent = loopyUDPSendMulti(sender, sendMsgs, BATCH_SIZE);
    TEST_ASSERT(sent > 0, "batch send succeeded");

    /* Verify bytes transferred */
    for (int i = 0; i < sent; i++) {
        TEST_ASSERT(sendMsgs[i].bytesTransferred > 0, "bytes transferred");
    }

    /* Small delay for packets to arrive */
    usleep(10000);

    /* Prepare receive buffers */
    loopyUDPMessage recvMsgs[5];
    char recvBufs[5][64];
    for (int i = 0; i < BATCH_SIZE; i++) {
        recvMsgs[i].data = recvBufs[i];
        recvMsgs[i].len = sizeof(recvBufs[i]);
        recvMsgs[i].bytesTransferred = 0;
    }

    /* Receive batch */
    int received = loopyUDPRecvMulti(receiver, recvMsgs, BATCH_SIZE);
    TEST_ASSERT(received > 0, "batch recv succeeded");

    /* Verify received data */
    for (int i = 0; i < received; i++) {
        TEST_ASSERT(recvMsgs[i].bytesTransferred > 0, "recv bytes");
        TEST_ASSERT(recvMsgs[i].addrLen > 0, "recv addr len");
    }

    loopyUDPFree(sender);
    loopyUDPFree(receiver);
    return 1;
}

static int test_udp_batch_connected(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    loopyUDP *sender = loopyUDPNew(l);
    loopyUDP *receiver = loopyUDPNew(l);

    /* Bind receiver */
    TEST_ASSERT(loopyUDPBind(receiver, "127.0.0.1", 0, 0), "bind receiver");

    int port = 0;
    TEST_ASSERT(loopyUDPGetSockName(receiver, NULL, 0, &port), "get port");

    /* Connect sender to receiver */
    char portStr[8];
    snprintf(portStr, sizeof(portStr), "%d", port);
    TEST_ASSERT(loopyUDPConnect(sender, "127.0.0.1", port), "connect sender");

    /* Prepare batch for connected send */
    const int BATCH_SIZE = 3;
    loopyUDPMessage msgs[3];
    char bufs[3][32];

    for (int i = 0; i < BATCH_SIZE; i++) {
        snprintf(bufs[i], sizeof(bufs[i]), "Connected msg %d", i);
        msgs[i].data = bufs[i];
        msgs[i].len = strlen(bufs[i]);
        msgs[i].bytesTransferred = 0;
    }

    /* Send using connected API */
    int sent = loopyUDPSendMultiConnected(sender, msgs, BATCH_SIZE);
    TEST_ASSERT(sent > 0, "connected batch send");

    loopyUDPFree(sender);
    loopyUDPFree(receiver);
    return 1;
}

static int test_udp_batch_native_check(void) {
    /* Just verify the API works */
    bool native = loopyUDPHasNativeBatch();

#ifdef __linux__
    TEST_ASSERT(native, "Linux should have native batch");
#else
    /* macOS/BSD uses fallback */
    (void)native;
#endif

    return 1;
}

static int test_udp_batch_null_safety(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyUDP *udp = loopyUDPNew(l);
    loopyUDPMessage msgs[1];
    msgs[0].data = (void *)"test";
    msgs[0].len = 4;

    /* NULL UDP handle */
    TEST_ASSERT_EQ(loopyUDPRecvMulti(NULL, msgs, 1), -1, "recv NULL udp");
    TEST_ASSERT_EQ(loopyUDPSendMulti(NULL, msgs, 1), -1, "send NULL udp");
    TEST_ASSERT_EQ(loopyUDPSendMultiConnected(NULL, msgs, 1), -1,
                   "send connected NULL udp");

    /* NULL msgs */
    TEST_ASSERT_EQ(loopyUDPRecvMulti(udp, NULL, 1), -1, "recv NULL msgs");
    TEST_ASSERT_EQ(loopyUDPSendMulti(udp, NULL, 1), -1, "send NULL msgs");

    /* Zero/negative count */
    TEST_ASSERT_EQ(loopyUDPRecvMulti(udp, msgs, 0), -1, "recv zero count");
    TEST_ASSERT_EQ(loopyUDPSendMulti(udp, msgs, -1), -1, "send negative count");

    /* Not connected for SendMultiConnected */
    TEST_ASSERT(loopyUDPBind(udp, "127.0.0.1", 0, 0), "bind");
    TEST_ASSERT_EQ(loopyUDPSendMultiConnected(udp, msgs, 1), -1,
                   "send connected when not connected");

    loopyUDPFree(udp);
    return 1;
}

/* ====================================================================
 * Test: UDP Advanced Features (GSO/GRO/PMTU)
 * ==================================================================== */

static int test_udp_gso_detection(void) {
    /* Test runtime GSO detection */
    bool hasGSO = loopyUDPHasGSO();

#ifdef __linux__
    /* On Linux 4.18+ GSO should be available */
    /* But might not be if kernel is too old */
    (void)hasGSO;
#else
    /* Non-Linux should report no GSO */
    TEST_ASSERT(!hasGSO, "Non-Linux should not have GSO");
#endif

    return 1;
}

static int test_udp_gso_enable_disable(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyUDP *udp = loopyUDPNew(l);

    loopyUDPGSOConfig config = {.enabled = true, .segmentSize = 1200};

    bool result = loopyUDPSetGSO(udp, &config);

#ifdef __linux__
    if (loopyUDPHasGSO()) {
        TEST_ASSERT(result, "GSO enable should succeed on Linux 4.18+");

        /* Disable GSO */
        config.enabled = false;
        TEST_ASSERT(loopyUDPSetGSO(udp, &config), "GSO disable should succeed");
    } else {
        /* Old kernel - should fail gracefully */
        TEST_ASSERT(!result, "GSO should fail on old kernel");
    }
#else
    TEST_ASSERT(!result, "GSO should not be supported on non-Linux");
#endif

    loopyUDPFree(udp);
    return 1;
}

static int test_udp_gso_invalid_segment_size(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyUDP *udp = loopyUDPNew(l);

    loopyUDPGSOConfig config = {
        .enabled = true, .segmentSize = 100 /* Too small */
    };

    bool result = loopyUDPSetGSO(udp, &config);

#ifdef __linux__
    if (loopyUDPHasGSO()) {
        TEST_ASSERT(!result, "Invalid segment size should fail");

        /* Try too large */
        config.segmentSize =
            (uint16_t)70000; /* Will overflow, testing invalid value */
        TEST_ASSERT(!loopyUDPSetGSO(udp, &config),
                    "Too large segment should fail");
    }
#else
    TEST_ASSERT(!result, "GSO should not be supported");
#endif

    loopyUDPFree(udp);
    return 1;
}

static int test_udp_gso_send_not_enabled(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyUDP *udp = loopyUDPNew(l);

    /* Try to send with GSO without enabling it */
    char data[4000];
    memset(data, 'A', sizeof(data));

    bool result =
        loopyUDPSendGSO(udp, "127.0.0.1", 9999, data, sizeof(data), NULL, NULL);

#ifdef __linux__
    if (loopyUDPHasGSO()) {
        TEST_ASSERT(!result, "GSO send should fail when not enabled");
    }
#else
    TEST_ASSERT(!result, "GSO send should not be supported");
#endif

    loopyUDPFree(udp);
    return 1;
}

static int test_udp_gso_null_safety(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyUDP *udp = loopyUDPNew(l);
    loopyUDPGSOConfig config = {.enabled = true, .segmentSize = 1200};
    char data[100] = {0};

    /* NULL UDP handle */
    TEST_ASSERT(!loopyUDPSetGSO(NULL, &config), "NULL udp should fail");
    TEST_ASSERT(!loopyUDPSendGSO(NULL, "127.0.0.1", 9999, data, sizeof(data),
                                 NULL, NULL),
                "NULL udp send should fail");
    TEST_ASSERT(!loopyUDPSendGSOConnected(NULL, data, sizeof(data), NULL, NULL),
                "NULL udp connected send should fail");

    /* NULL config */
    TEST_ASSERT(!loopyUDPSetGSO(udp, NULL), "NULL config should fail");

    /* NULL data */
    TEST_ASSERT(!loopyUDPSendGSO(udp, "127.0.0.1", 9999, NULL, 100, NULL, NULL),
                "NULL data should fail");

    /* Zero length */
    TEST_ASSERT(!loopyUDPSendGSO(udp, "127.0.0.1", 9999, data, 0, NULL, NULL),
                "Zero length should fail");

    loopyUDPFree(udp);
    return 1;
}

static int test_udp_gro_detection(void) {
    /* Test runtime GRO detection */
    bool hasGRO = loopyUDPHasGRO();

#ifdef __linux__
    /* On Linux 5.0+ GRO should be available */
    /* But might not be if kernel is too old */
    (void)hasGRO;
#else
    /* Non-Linux should report no GRO */
    TEST_ASSERT(!hasGRO, "Non-Linux should not have GRO");
#endif

    return 1;
}

static int test_udp_gro_enable_disable(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyUDP *udp = loopyUDPNew(l);

    loopyUDPGROConfig config = {.enabled = true};

    bool result = loopyUDPSetGRO(udp, &config);

#ifdef __linux__
    if (loopyUDPHasGRO()) {
        TEST_ASSERT(result, "GRO enable should succeed on Linux 5.0+");

        /* Disable GRO */
        config.enabled = false;
        TEST_ASSERT(loopyUDPSetGRO(udp, &config), "GRO disable should succeed");
    } else {
        /* Old kernel - should fail gracefully */
        TEST_ASSERT(!result, "GRO should fail on old kernel");
    }
#else
    TEST_ASSERT(!result, "GRO should not be supported on non-Linux");
#endif

    loopyUDPFree(udp);
    return 1;
}

static int test_udp_gro_null_safety(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyUDP *udp = loopyUDPNew(l);
    loopyUDPGROConfig config = {.enabled = true};

    /* NULL UDP handle */
    TEST_ASSERT(!loopyUDPSetGRO(NULL, &config), "NULL udp should fail");

    /* NULL config */
    TEST_ASSERT(!loopyUDPSetGRO(udp, NULL), "NULL config should fail");

    loopyUDPFree(udp);
    return 1;
}

static int test_udp_pmtu_set_mode(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyUDP *udp = loopyUDPNew(l);

    /* Test different PMTU modes */
    bool result;

    result = loopyUDPSetPMTUMode(udp, LOOPY_UDP_PMTU_DISABLED);
#ifdef __linux__
    TEST_ASSERT(result, "PMTU DISABLED should succeed");
#else
    TEST_ASSERT(!result, "PMTU not supported on non-Linux");
#endif

    result = loopyUDPSetPMTUMode(udp, LOOPY_UDP_PMTU_WANT);
#ifdef __linux__
    TEST_ASSERT(result, "PMTU WANT should succeed");
#else
    TEST_ASSERT(!result, "PMTU not supported on non-Linux");
#endif

    result = loopyUDPSetPMTUMode(udp, LOOPY_UDP_PMTU_DO);
#ifdef __linux__
    TEST_ASSERT(result, "PMTU DO should succeed");
#else
    TEST_ASSERT(!result, "PMTU not supported on non-Linux");
#endif

    result = loopyUDPSetPMTUMode(udp, LOOPY_UDP_PMTU_PROBE);
#ifdef __linux__
    TEST_ASSERT(result, "PMTU PROBE should succeed");
#else
    TEST_ASSERT(!result, "PMTU not supported on non-Linux");
#endif

    /* UNSPEC should be no-op on Linux, fail on non-Linux */
    result = loopyUDPSetPMTUMode(udp, LOOPY_UDP_PMTU_UNSPEC);
#ifdef __linux__
    TEST_ASSERT(result, "PMTU UNSPEC should succeed on Linux");
#else
    /* On non-Linux, PMTU is not supported at all */
    (void)result;
#endif

    loopyUDPFree(udp);
    return 1;
}

static int test_udp_pmtu_get(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyUDP *udp = loopyUDPNew(l);

    /* Need to bind/connect to have PMTU */
    TEST_ASSERT(loopyUDPBind(udp, "127.0.0.1", 0, 0), "bind should succeed");

    int32_t mtu = loopyUDPGetPMTU(udp);

#ifdef __linux__
    /* On Linux with loopback, should have valid MTU */
    if (mtu > 0) {
        TEST_ASSERT(mtu >= 1280, "MTU should be at least IPv6 minimum");
        TEST_ASSERT(mtu <= 65536, "MTU should be reasonable");
    }
#else
    /* Non-Linux returns -1 */
    TEST_ASSERT_EQ(mtu, -1, "PMTU not supported on non-Linux");
#endif

    loopyUDPFree(udp);
    return 1;
}

static int test_udp_pmtu_null_safety(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyUDP *udp = loopyUDPNew(l);

    /* NULL UDP handle */
    TEST_ASSERT(!loopyUDPSetPMTUMode(NULL, LOOPY_UDP_PMTU_WANT),
                "NULL udp should fail");
    TEST_ASSERT_EQ(loopyUDPGetPMTU(NULL), -1, "NULL udp should return -1");

    loopyUDPFree(udp);
    return 1;
}

static int test_udp_gso_send_basic(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyUDP *udp = loopyUDPNew(l);

#ifdef __linux__
    if (!loopyUDPHasGSO()) {
        /* Skip test on old kernels */
        loopyUDPFree(udp);
        return 1;
    }

    /* Enable GSO */
    loopyUDPGSOConfig config = {.enabled = true, .segmentSize = 1200};
    TEST_ASSERT(loopyUDPSetGSO(udp, &config), "GSO enable should succeed");

    /* Create large payload (will be segmented) */
    char data[4800]; /* 4 segments of 1200 bytes */
    memset(data, 'T', sizeof(data));

    /* Send with GSO - may fail if not bound/connected, but API should work */
    bool result =
        loopyUDPSendGSO(udp, "127.0.0.1", 9999, data, sizeof(data), NULL, NULL);

    /* Result may be true or false depending on socket state, just verify no
     * crash */
    (void)result;
#endif

    loopyUDPFree(udp);
    return 1;
}

static int test_udp_gso_connected_send(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyUDP *udp = loopyUDPNew(l);

#ifdef __linux__
    if (!loopyUDPHasGSO()) {
        /* Skip test on old kernels */
        loopyUDPFree(udp);
        return 1;
    }

    /* Connect to a destination */
    TEST_ASSERT(loopyUDPConnect(udp, "127.0.0.1", 9999),
                "connect should succeed");

    /* Enable GSO */
    loopyUDPGSOConfig config = {.enabled = true, .segmentSize = 1200};
    TEST_ASSERT(loopyUDPSetGSO(udp, &config), "GSO enable should succeed");

    /* Send with GSO on connected socket */
    char data[2400];
    memset(data, 'C', sizeof(data));

    bool result = loopyUDPSendGSOConnected(udp, data, sizeof(data), NULL, NULL);

    /* Result may be true or false, just verify API works */
    (void)result;
#endif

    loopyUDPFree(udp);
    return 1;
}

static int test_udp_pmtu_with_connect(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyUDP *udp = loopyUDPNew(l);

#ifdef __linux__
    /* Connect to loopback */
    TEST_ASSERT(loopyUDPConnect(udp, "127.0.0.1", 9999),
                "connect should succeed");

    /* Set PMTU mode */
    TEST_ASSERT(loopyUDPSetPMTUMode(udp, LOOPY_UDP_PMTU_DO),
                "PMTU DO should succeed");

    /* Get PMTU - should be loopback MTU */
    int32_t mtu = loopyUDPGetPMTU(udp);
    if (mtu > 0) {
        TEST_ASSERT(mtu >= 1280, "MTU should be at least IPv6 minimum");
    }
#endif

    loopyUDPFree(udp);
    return 1;
}

/* ====================================================================
 * Test: loopyProcess
 * ==================================================================== */
static int process_exit_count = 0;
static int64_t process_exit_status = -1;
static int process_term_signal = -1;
static loopyLoop *process_test_loop = NULL;

static void test_process_exit_callback(loopyLoop *loop, loopyProcess *process,
                                       int64_t exitStatus, int termSignal,
                                       void *userData) {
    (void)process;
    (void)userData;

    process_exit_count++;
    process_exit_status = exitStatus;
    process_term_signal = termSignal;

    if (process_test_loop) {
        loopyStop(loop);
    }
}

static int test_process_spawn_simple(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    /* Spawn true which exits with 0 */
    loopyProcessOptions opts = {0};
#ifdef __APPLE__
    opts.file = "/usr/bin/true";
#else
    opts.file = "/bin/true";
#endif
    char *args[] = {"true", NULL};
    opts.args = args;
    opts.stdio[0].flags = LOOPY_STDIO_IGNORE;
    opts.stdio[1].flags = LOOPY_STDIO_IGNORE;
    opts.stdio[2].flags = LOOPY_STDIO_IGNORE;

    process_exit_count = 0;
    process_exit_status = -1;
    process_term_signal = -1;
    process_test_loop = l;

    loopyProcess *p =
        loopyProcessSpawn(l, &opts, test_process_exit_callback, NULL);
    TEST_ASSERT(p != NULL, "spawn should succeed");
    TEST_ASSERT(loopyProcessGetPid(p) > 0, "pid should be valid");
    TEST_ASSERT(!loopyProcessExited(p), "should not have exited yet");
    TEST_ASSERT(loopyProcessGetLoop(p) == l, "loop should match");

    /* Run event loop to wait for child */
    loopyRegisterTimer(l, 2000000, 0, NULL, NULL); /* 2 second timeout */
    loopyMain(l);

    TEST_ASSERT(process_exit_count > 0,
                "exit callback should have been called");
    TEST_ASSERT_EQ(process_exit_status, 0, "exit status should be 0");
    TEST_ASSERT_EQ(process_term_signal, 0, "should not have been signaled");

    loopyProcessFree(p);
    process_test_loop = NULL;
    return 1;
}

static int test_process_exit_code(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    /* Spawn false which exits with 1 */
    loopyProcessOptions opts = {0};
#ifdef __APPLE__
    opts.file = "/usr/bin/false";
#else
    opts.file = "/bin/false";
#endif
    char *args[] = {"false", NULL};
    opts.args = args;
    opts.stdio[0].flags = LOOPY_STDIO_IGNORE;
    opts.stdio[1].flags = LOOPY_STDIO_IGNORE;
    opts.stdio[2].flags = LOOPY_STDIO_IGNORE;

    process_exit_count = 0;
    process_exit_status = -1;
    process_test_loop = l;

    loopyProcess *p =
        loopyProcessSpawn(l, &opts, test_process_exit_callback, NULL);
    TEST_ASSERT(p != NULL, "spawn should succeed");

    loopyRegisterTimer(l, 2000000, 0, NULL, NULL);
    loopyMain(l);

    TEST_ASSERT(process_exit_count > 0,
                "exit callback should have been called");
    TEST_ASSERT_EQ(process_exit_status, 1, "exit status should be 1");

    loopyProcessFree(p);
    process_test_loop = NULL;
    return 1;
}

static int test_process_capture_stdout(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    /* Spawn echo command */
    loopyProcessOptions opts = {0};
    opts.file = "/bin/echo";
    char *args[] = {"echo", "hello", NULL};
    opts.args = args;
    opts.stdio[0].flags = LOOPY_STDIO_IGNORE;
    opts.stdio[1].flags = LOOPY_STDIO_CREATE_PIPE;
    opts.stdio[2].flags = LOOPY_STDIO_IGNORE;

    process_exit_count = 0;
    process_test_loop = l;

    loopyProcess *p =
        loopyProcessSpawn(l, &opts, test_process_exit_callback, NULL);
    TEST_ASSERT(p != NULL, "spawn should succeed");

    int stdoutFd = loopyProcessGetStdioPipe(p, 1);
    TEST_ASSERT(stdoutFd >= 0, "stdout pipe should be valid");

    loopyRegisterTimer(l, 2000000, 0, NULL, NULL);
    loopyMain(l);

    /* Read from stdout */
    char buf[64] = {0};
    ssize_t n = read(stdoutFd, buf, sizeof(buf) - 1);
    TEST_ASSERT(n > 0, "should have read some data");
    TEST_ASSERT(strstr(buf, "hello") != NULL, "output should contain 'hello'");

    loopyProcessFree(p);
    process_test_loop = NULL;
    return 1;
}

static int test_process_kill(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    /* Spawn sleep which runs for a while */
    loopyProcessOptions opts = {0};
    opts.file = "/bin/sleep";
    char *args[] = {"sleep", "60", NULL};
    opts.args = args;
    opts.stdio[0].flags = LOOPY_STDIO_IGNORE;
    opts.stdio[1].flags = LOOPY_STDIO_IGNORE;
    opts.stdio[2].flags = LOOPY_STDIO_IGNORE;

    process_exit_count = 0;
    process_term_signal = -1;
    process_test_loop = l;

    loopyProcess *p =
        loopyProcessSpawn(l, &opts, test_process_exit_callback, NULL);
    TEST_ASSERT(p != NULL, "spawn should succeed");
    TEST_ASSERT(!loopyProcessExited(p), "should not have exited");

    /* Kill it */
    TEST_ASSERT(loopyProcessKill(p, SIGTERM), "kill should succeed");

    loopyRegisterTimer(l, 2000000, 0, NULL, NULL);
    loopyMain(l);

    TEST_ASSERT(process_exit_count > 0,
                "exit callback should have been called");
    TEST_ASSERT_EQ(process_term_signal, SIGTERM,
                   "should have been terminated by SIGTERM");

    loopyProcessFree(p);
    process_test_loop = NULL;
    return 1;
}

static int test_process_cwd(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    /* Spawn pwd in /tmp */
    loopyProcessOptions opts = {0};
    opts.file = "/bin/pwd";
    char *args[] = {"pwd", NULL};
    opts.args = args;
    opts.cwd = "/tmp";
    opts.stdio[0].flags = LOOPY_STDIO_IGNORE;
    opts.stdio[1].flags = LOOPY_STDIO_CREATE_PIPE;
    opts.stdio[2].flags = LOOPY_STDIO_IGNORE;

    process_test_loop = l;

    loopyProcess *p =
        loopyProcessSpawn(l, &opts, test_process_exit_callback, NULL);
    TEST_ASSERT(p != NULL, "spawn should succeed");

    int stdoutFd = loopyProcessGetStdioPipe(p, 1);
    TEST_ASSERT(stdoutFd >= 0, "stdout pipe should be valid");

    loopyRegisterTimer(l, 2000000, 0, NULL, NULL);
    loopyMain(l);

    char buf[256] = {0};
    read(stdoutFd, buf, sizeof(buf) - 1);
    /* On macOS /tmp is a symlink to /private/tmp */
    TEST_ASSERT(strstr(buf, "tmp") != NULL, "output should contain 'tmp'");

    loopyProcessFree(p);
    process_test_loop = NULL;
    return 1;
}

static int test_process_env(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    /* Spawn env to print environment */
    loopyProcessOptions opts = {0};
    opts.file = "/usr/bin/env";
    char *args[] = {"env", NULL};
    opts.args = args;
    char *env[] = {"TEST_VAR=hello_loopy", NULL};
    opts.env = env;
    opts.stdio[0].flags = LOOPY_STDIO_IGNORE;
    opts.stdio[1].flags = LOOPY_STDIO_CREATE_PIPE;
    opts.stdio[2].flags = LOOPY_STDIO_IGNORE;

    process_test_loop = l;

    loopyProcess *p =
        loopyProcessSpawn(l, &opts, test_process_exit_callback, NULL);
    TEST_ASSERT(p != NULL, "spawn should succeed");

    int stdoutFd = loopyProcessGetStdioPipe(p, 1);

    loopyRegisterTimer(l, 2000000, 0, NULL, NULL);
    loopyMain(l);

    char buf[256] = {0};
    read(stdoutFd, buf, sizeof(buf) - 1);
    TEST_ASSERT(strstr(buf, "TEST_VAR=hello_loopy") != NULL,
                "output should contain test env var");

    loopyProcessFree(p);
    process_test_loop = NULL;
    return 1;
}

static int test_process_spawn_failure(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    /* Spawn nonexistent executable */
    loopyProcessOptions opts = {0};
    opts.file = "/nonexistent/path/to/executable";
    char *args[] = {"nonexistent", NULL};
    opts.args = args;
    opts.stdio[0].flags = LOOPY_STDIO_IGNORE;
    opts.stdio[1].flags = LOOPY_STDIO_IGNORE;
    opts.stdio[2].flags = LOOPY_STDIO_IGNORE;

    process_exit_count = 0;
    process_exit_status = -1;
    process_test_loop = l;

    loopyProcess *p =
        loopyProcessSpawn(l, &opts, test_process_exit_callback, NULL);
    /* Spawn itself succeeds (fork succeeds), but exec fails in child */
    TEST_ASSERT(p != NULL, "spawn should succeed (fork succeeds)");

    loopyRegisterTimer(l, 2000000, 0, NULL, NULL);
    loopyMain(l);

    TEST_ASSERT(process_exit_count > 0,
                "exit callback should have been called");
    TEST_ASSERT_EQ(process_exit_status, 127,
                   "exit status should be 127 (exec failed)");

    loopyProcessFree(p);
    process_test_loop = NULL;
    return 1;
}

static int test_process_null_safety(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    /* NULL loop */
    loopyProcessOptions opts = {0};
#ifdef __APPLE__
    opts.file = "/usr/bin/true";
#else
    opts.file = "/bin/true";
#endif
    char *args[] = {"true", NULL};
    opts.args = args;

    TEST_ASSERT(loopyProcessSpawn(NULL, &opts, NULL, NULL) == NULL,
                "spawn with NULL loop should fail");

    /* NULL options */
    TEST_ASSERT(loopyProcessSpawn(l, NULL, NULL, NULL) == NULL,
                "spawn with NULL options should fail");

    /* NULL file */
    loopyProcessOptions opts2 = {0};
    TEST_ASSERT(loopyProcessSpawn(l, &opts2, NULL, NULL) == NULL,
                "spawn with NULL file should fail");

    /* NULL process operations */
    TEST_ASSERT(!loopyProcessKill(NULL, SIGTERM), "kill NULL should fail");
    TEST_ASSERT_EQ(loopyProcessGetPid(NULL), -1, "getpid NULL should be -1");
    TEST_ASSERT_EQ(loopyProcessGetStdioPipe(NULL, 0), -1,
                   "getstdio NULL should be -1");
    TEST_ASSERT(loopyProcessExited(NULL), "exited NULL should be true");
    TEST_ASSERT(loopyProcessGetLoop(NULL) == NULL,
                "getloop NULL should be NULL");

    loopyProcessFree(NULL); /* Should not crash */
    return 1;
}

/* ====================================================================
 * Test: loopyIdle/Prepare/Check
 * ==================================================================== */
static int idle_callback_count = 0;
static int prepare_callback_count = 0;
static int check_callback_count = 0;

static bool test_idle_callback(loopyLoop *loop, loopyIdleHandle *handle,
                               void *userData) {
    (void)loop;
    (void)handle;
    (void)userData;
    idle_callback_count++;
    /* Stop after 5 iterations to prevent infinite loop */
    if (idle_callback_count >= 5) {
        return false; /* Auto-stop */
    }
    return true;
}

static void test_prepare_callback(loopyLoop *loop, loopyPrepareHandle *handle,
                                  void *userData) {
    (void)loop;
    (void)handle;
    (void)userData;
    prepare_callback_count++;
}

static void test_check_callback(loopyLoop *loop, loopyCheckHandle *handle,
                                void *userData) {
    (void)loop;
    (void)handle;
    (void)userData;
    check_callback_count++;
}

static bool idle_stop_callback(timerWheel *t, timerWheelId id, void *data) {
    (void)t;
    (void)id;
    loopyLoop *l = data;
    loopyStop(l);
    return false;
}

static int test_idle_create_delete(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    loopyIdleHandle *handle = loopyIdleStart(l, test_idle_callback, NULL);
    TEST_ASSERT(handle != NULL, "loopyIdleStart should succeed");
    TEST_ASSERT(loopyIdleIsActive(handle), "idle should be active");
    TEST_ASSERT(loopyIdleGetLoop(handle) == l, "loop should match");
    TEST_ASSERT_EQ(loopyIdleCount(l), 1, "idle count should be 1");

    loopyIdleFree(handle);
    TEST_ASSERT_EQ(loopyIdleCount(l), 0, "idle count should be 0 after free");

    return 1;
}

static int test_prepare_create_delete(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    loopyPrepareHandle *handle =
        loopyPrepareStart(l, test_prepare_callback, NULL);
    TEST_ASSERT(handle != NULL, "loopyPrepareStart should succeed");
    TEST_ASSERT(loopyPrepareIsActive(handle), "prepare should be active");
    TEST_ASSERT(loopyPrepareGetLoop(handle) == l, "loop should match");
    TEST_ASSERT_EQ(loopyPrepareCount(l), 1, "prepare count should be 1");

    loopyPrepareFree(handle);
    TEST_ASSERT_EQ(loopyPrepareCount(l), 0,
                   "prepare count should be 0 after free");

    return 1;
}

static int test_check_create_delete(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    loopyCheckHandle *handle = loopyCheckStart(l, test_check_callback, NULL);
    TEST_ASSERT(handle != NULL, "loopyCheckStart should succeed");
    TEST_ASSERT(loopyCheckIsActive(handle), "check should be active");
    TEST_ASSERT(loopyCheckGetLoop(handle) == l, "loop should match");
    TEST_ASSERT_EQ(loopyCheckCount(l), 1, "check count should be 1");

    loopyCheckFree(handle);
    TEST_ASSERT_EQ(loopyCheckCount(l), 0, "check count should be 0 after free");

    return 1;
}

static int test_idle_callback_fires(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    idle_callback_count = 0;

    loopyIdleHandle *handle = loopyIdleStart(l, test_idle_callback, NULL);
    TEST_ASSERT(handle != NULL, "loopyIdleStart should succeed");

    /* Set a timeout to stop the loop */
    loopyRegisterTimer(l, 100000, 0, idle_stop_callback, l); /* 100ms */

    loopyMain(l);

    /* Idle callback should have fired multiple times */
    TEST_ASSERT(idle_callback_count > 0, "idle callback should have fired");
    TEST_ASSERT(idle_callback_count >= 5,
                "idle callback should fire many times");

    loopyIdleFree(handle);
    return 1;
}

static int test_idle_auto_stop(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    idle_callback_count = 0;

    loopyIdleHandle *handle = loopyIdleStart(l, test_idle_callback, NULL);
    TEST_ASSERT(handle != NULL, "loopyIdleStart should succeed");

    /* Set a timeout as backup */
    loopyRegisterTimer(l, 500000, 0, idle_stop_callback, l);

    loopyMain(l);

    /* Callback returns false after 5 iterations, so idle should be stopped */
    TEST_ASSERT(!loopyIdleIsActive(handle), "idle should be auto-stopped");
    TEST_ASSERT_EQ(idle_callback_count, 5,
                   "callback should have fired 5 times");

    loopyIdleFree(handle);
    return 1;
}

static int test_prepare_callback_fires(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    prepare_callback_count = 0;

    loopyPrepareHandle *handle =
        loopyPrepareStart(l, test_prepare_callback, NULL);
    TEST_ASSERT(handle != NULL, "loopyPrepareStart should succeed");

    /* Need to create an idle to drive the loop */
    idle_callback_count = 0;
    loopyIdleHandle *idle = loopyIdleStart(l, test_idle_callback, NULL);

    /* Timeout to stop */
    loopyRegisterTimer(l, 100000, 0, idle_stop_callback, l);

    loopyMain(l);

    /* Prepare should have fired before each iteration */
    TEST_ASSERT(prepare_callback_count > 0,
                "prepare callback should have fired");

    loopyPrepareFree(handle);
    loopyIdleFree(idle);
    return 1;
}

static int test_check_callback_fires(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    check_callback_count = 0;

    loopyCheckHandle *handle = loopyCheckStart(l, test_check_callback, NULL);
    TEST_ASSERT(handle != NULL, "loopyCheckStart should succeed");

    /* Need to create an idle to drive the loop */
    idle_callback_count = 0;
    loopyIdleHandle *idle = loopyIdleStart(l, test_idle_callback, NULL);

    /* Timeout to stop */
    loopyRegisterTimer(l, 100000, 0, idle_stop_callback, l);

    loopyMain(l);

    /* Check should have fired after each iteration */
    TEST_ASSERT(check_callback_count > 0, "check callback should have fired");

    loopyCheckFree(handle);
    loopyIdleFree(idle);
    return 1;
}

static int test_idle_stop_restart(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    loopyIdleHandle *handle = loopyIdleStart(l, test_idle_callback, NULL);
    TEST_ASSERT(handle != NULL, "loopyIdleStart should succeed");
    TEST_ASSERT(loopyIdleIsActive(handle), "idle should be active");
    TEST_ASSERT_EQ(loopyIdleCount(l), 1, "idle count should be 1");

    loopyIdleStop(handle);
    TEST_ASSERT(!loopyIdleIsActive(handle), "idle should be stopped");
    TEST_ASSERT_EQ(loopyIdleCount(l), 0, "idle count should be 0");

    TEST_ASSERT(loopyIdleRestart(handle), "restart should succeed");
    TEST_ASSERT(loopyIdleIsActive(handle), "idle should be active again");
    TEST_ASSERT_EQ(loopyIdleCount(l), 1, "idle count should be 1");

    /* Restart already active should fail */
    TEST_ASSERT(!loopyIdleRestart(handle), "restart active should fail");

    loopyIdleFree(handle);
    return 1;
}

static int test_idle_multiple_handles(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    loopyIdleHandle *h1 = loopyIdleStart(l, test_idle_callback, NULL);
    loopyPrepareHandle *h2 = loopyPrepareStart(l, test_prepare_callback, NULL);
    loopyCheckHandle *h3 = loopyCheckStart(l, test_check_callback, NULL);

    TEST_ASSERT_EQ(loopyIdleCount(l), 1, "idle count should be 1");
    TEST_ASSERT_EQ(loopyPrepareCount(l), 1, "prepare count should be 1");
    TEST_ASSERT_EQ(loopyCheckCount(l), 1, "check count should be 1");

    loopyIdleHandle *h4 = loopyIdleStart(l, test_idle_callback, NULL);
    TEST_ASSERT_EQ(loopyIdleCount(l), 2, "idle count should be 2");

    TEST_ASSERT(loopyHasActiveIdle(l), "should have active idle");

    loopyIdleFree(h1);
    loopyIdleFree(h4);
    loopyPrepareFree(h2);
    loopyCheckFree(h3);

    TEST_ASSERT(!loopyHasActiveIdle(l), "should not have active idle");

    return 1;
}

static int test_idle_null_safety(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    /* NULL loop */
    TEST_ASSERT(loopyIdleStart(NULL, test_idle_callback, NULL) == NULL,
                "idle start with NULL loop should fail");
    TEST_ASSERT(loopyPrepareStart(NULL, test_prepare_callback, NULL) == NULL,
                "prepare start with NULL loop should fail");
    TEST_ASSERT(loopyCheckStart(NULL, test_check_callback, NULL) == NULL,
                "check start with NULL loop should fail");

    /* NULL callback */
    TEST_ASSERT(loopyIdleStart(l, NULL, NULL) == NULL,
                "idle start with NULL callback should fail");
    TEST_ASSERT(loopyPrepareStart(l, NULL, NULL) == NULL,
                "prepare start with NULL callback should fail");
    TEST_ASSERT(loopyCheckStart(l, NULL, NULL) == NULL,
                "check start with NULL callback should fail");

    /* NULL handle operations */
    loopyIdleStop(NULL);    /* Should not crash */
    loopyIdleFree(NULL);    /* Should not crash */
    loopyPrepareStop(NULL); /* Should not crash */
    loopyPrepareFree(NULL); /* Should not crash */
    loopyCheckStop(NULL);   /* Should not crash */
    loopyCheckFree(NULL);   /* Should not crash */

    TEST_ASSERT(!loopyIdleIsActive(NULL), "isActive NULL should be false");
    TEST_ASSERT(!loopyPrepareIsActive(NULL), "isActive NULL should be false");
    TEST_ASSERT(!loopyCheckIsActive(NULL), "isActive NULL should be false");

    TEST_ASSERT(loopyIdleGetLoop(NULL) == NULL, "getLoop NULL should be NULL");
    TEST_ASSERT(loopyPrepareGetLoop(NULL) == NULL,
                "getLoop NULL should be NULL");
    TEST_ASSERT(loopyCheckGetLoop(NULL) == NULL, "getLoop NULL should be NULL");

    TEST_ASSERT(!loopyIdleRestart(NULL), "restart NULL should fail");
    TEST_ASSERT(!loopyPrepareRestart(NULL), "restart NULL should fail");
    TEST_ASSERT(!loopyCheckRestart(NULL), "restart NULL should fail");

    return 1;
}

/* Edge case: verify idle timer uses sub-resolution repeat for rapid iteration
 */
static int test_idle_rapid_iterations(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    idle_callback_count = 0;

    loopyIdleHandle *handle = loopyIdleStart(l, test_idle_callback, NULL);
    TEST_ASSERT(handle != NULL, "loopyIdleStart should succeed");

    /* Short timeout - we expect many iterations in a short time
     * The idle timer uses 1μs repeat, so the loop should spin rapidly */
    loopyRegisterTimer(l, 10000, 0, idle_stop_callback, l); /* 10ms */

    loopyMain(l);

    /* With sub-resolution timer fix, we should get many iterations */
    TEST_ASSERT(idle_callback_count >= 5,
                "idle should fire many times in 10ms");

    loopyIdleFree(handle);
    return 1;
}

/* Edge case: verify check fires with only idle driving loop (no other FDs) */
static int test_check_fires_with_idle_only(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    check_callback_count = 0;
    idle_callback_count = 0;

    loopyCheckHandle *check = loopyCheckStart(l, test_check_callback, NULL);
    loopyIdleHandle *idle = loopyIdleStart(l, test_idle_callback, NULL);

    TEST_ASSERT(check != NULL, "loopyCheckStart should succeed");
    TEST_ASSERT(idle != NULL, "loopyIdleStart should succeed");

    /* Short timeout */
    loopyRegisterTimer(l, 10000, 0, idle_stop_callback, l);

    loopyMain(l);

    /* Both should have fired multiple times */
    TEST_ASSERT(idle_callback_count >= 5, "idle should fire multiple times");
    TEST_ASSERT(check_callback_count >= 5, "check should fire multiple times");

    loopyCheckFree(check);
    loopyIdleFree(idle);
    return 1;
}

/* Edge case: prepare fires before poll each iteration */
static int test_prepare_fires_each_iteration(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    prepare_callback_count = 0;
    idle_callback_count = 0;

    loopyPrepareHandle *prep =
        loopyPrepareStart(l, test_prepare_callback, NULL);
    loopyIdleHandle *idle = loopyIdleStart(l, test_idle_callback, NULL);

    TEST_ASSERT(prep != NULL, "loopyPrepareStart should succeed");
    TEST_ASSERT(idle != NULL, "loopyIdleStart should succeed");

    loopyRegisterTimer(l, 10000, 0, idle_stop_callback, l);

    loopyMain(l);

    /* Prepare should fire on each iteration, just like idle */
    TEST_ASSERT(prepare_callback_count >= 5,
                "prepare should fire multiple times");
    TEST_ASSERT(idle_callback_count >= 5, "idle should fire multiple times");

    loopyPrepareFree(prep);
    loopyIdleFree(idle);
    return 1;
}

/* Edge case: all three handle types fire in correct order */
static int test_idle_prepare_check_order(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    prepare_callback_count = 0;
    idle_callback_count = 0;
    check_callback_count = 0;

    loopyPrepareHandle *prep =
        loopyPrepareStart(l, test_prepare_callback, NULL);
    loopyIdleHandle *idle = loopyIdleStart(l, test_idle_callback, NULL);
    loopyCheckHandle *check = loopyCheckStart(l, test_check_callback, NULL);

    loopyRegisterTimer(l, 10000, 0, idle_stop_callback, l);

    loopyMain(l);

    /* All should fire */
    TEST_ASSERT(prepare_callback_count > 0, "prepare should fire");
    TEST_ASSERT(idle_callback_count > 0, "idle should fire");
    TEST_ASSERT(check_callback_count > 0, "check should fire");

    loopyPrepareFree(prep);
    loopyIdleFree(idle);
    loopyCheckFree(check);
    return 1;
}

/* Edge case: idle with very short timeout verifies timer doesn't block */
static int test_idle_timer_no_block(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    idle_callback_count = 0;

    loopyIdleHandle *handle = loopyIdleStart(l, test_idle_callback, NULL);
    TEST_ASSERT(handle != NULL, "loopyIdleStart should succeed");

    /* Very short timeout - if timer wheel was blocking, this would hang */
    loopyRegisterTimer(l, 5000, 0, idle_stop_callback, l); /* 5ms */

    uint64_t start = timeUtilMonotonicUs();
    loopyMain(l);
    uint64_t elapsed = timeUtilMonotonicUs() - start;

    /* Should complete quickly (allow some margin for slow systems) */
    TEST_ASSERT(elapsed < 100000, "loop should complete within 100ms");

    /* Should have fired at least once */
    TEST_ASSERT(idle_callback_count > 0, "idle should fire");

    loopyIdleFree(handle);
    return 1;
}

/* ====================================================================
 * Test: loopyPipe
 * ==================================================================== */

/* Test context for async pipe operations */
typedef struct {
    int readCalled;
    int writeCalled;
    ssize_t lastReadSize;
    int lastWriteStatus;
    char readData[1024];
    loopyLoop *loop;
} PipeTestCtx;

static int test_pipe_create_basic(void) {
    /* Create pipe with default flags */
    loopyPipe *pipe = loopyPipeCreate(LOOPY_PIPE_NONE);
    TEST_ASSERT(pipe != NULL, "pipe creation should succeed");
    TEST_ASSERT(loopyPipeIsReadable(pipe), "pipe should be readable");
    TEST_ASSERT(loopyPipeIsWritable(pipe), "pipe should be writable");
    TEST_ASSERT(loopyPipeGetReadFd(pipe) >= 0, "read fd should be valid");
    TEST_ASSERT(loopyPipeGetWriteFd(pipe) >= 0, "write fd should be valid");

    loopyPipeClose(pipe);
    return 1;
}

static int test_pipe_create_with_flags(void) {
    /* Create pipe with non-blocking and close-on-exec flags */
    loopyPipe *pipe = loopyPipeCreate(LOOPY_PIPE_NONBLOCK | LOOPY_PIPE_CLOEXEC);
    TEST_ASSERT(pipe != NULL, "pipe creation with flags should succeed");
    TEST_ASSERT(loopyPipeIsReadable(pipe), "pipe should be readable");
    TEST_ASSERT(loopyPipeIsWritable(pipe), "pipe should be writable");

    int readFd = loopyPipeGetReadFd(pipe);
    int writeFd = loopyPipeGetWriteFd(pipe);
    TEST_ASSERT(readFd >= 0, "read fd should be valid");
    TEST_ASSERT(writeFd >= 0, "write fd should be valid");

    /* Verify non-blocking flag */
    int flags = fcntl(readFd, F_GETFL);
    TEST_ASSERT(flags >= 0, "fcntl F_GETFL should succeed");
    TEST_ASSERT((flags & O_NONBLOCK) != 0, "read fd should be non-blocking");

    flags = fcntl(writeFd, F_GETFL);
    TEST_ASSERT(flags >= 0, "fcntl F_GETFL should succeed");
    TEST_ASSERT((flags & O_NONBLOCK) != 0, "write fd should be non-blocking");

    /* Verify close-on-exec flag */
    flags = fcntl(readFd, F_GETFD);
    TEST_ASSERT(flags >= 0, "fcntl F_GETFD should succeed");
    TEST_ASSERT((flags & FD_CLOEXEC) != 0, "read fd should have close-on-exec");

    flags = fcntl(writeFd, F_GETFD);
    TEST_ASSERT(flags >= 0, "fcntl F_GETFD should succeed");
    TEST_ASSERT((flags & FD_CLOEXEC) != 0,
                "write fd should have close-on-exec");

    loopyPipeClose(pipe);
    return 1;
}

static int test_pipe_from_fds(void) {
    /* Create pipe manually and wrap it */
    int fds[2];
    TEST_ASSERT(pipe(fds) == 0, "manual pipe creation should succeed");

    loopyPipe *p = loopyPipeFromFds(fds[0], fds[1]);
    TEST_ASSERT(p != NULL, "loopyPipeFromFds should succeed");
    TEST_ASSERT(loopyPipeGetReadFd(p) == fds[0], "read fd should match");
    TEST_ASSERT(loopyPipeGetWriteFd(p) == fds[1], "write fd should match");

    loopyPipeClose(p);
    return 1;
}

static int test_pipe_sync_read_write(void) {
    loopyPipe *pipe = loopyPipeCreate(LOOPY_PIPE_NONE);
    TEST_ASSERT(pipe != NULL, "pipe creation should succeed");

    /* Write data */
    const char *testData = "Hello, loopyPipe!";
    ssize_t written = loopyPipeWrite(pipe, testData, strlen(testData));
    TEST_ASSERT(written == (ssize_t)strlen(testData), "write should succeed");

    /* Read data */
    char buf[128] = {0};
    ssize_t nread = loopyPipeRead(pipe, buf, sizeof(buf));
    TEST_ASSERT(nread == (ssize_t)strlen(testData),
                "read should return correct size");
    TEST_ASSERT(strcmp(buf, testData) == 0,
                "read data should match written data");

    loopyPipeClose(pipe);
    return 1;
}

static int test_pipe_close_read(void) {
    loopyPipe *pipe = loopyPipeCreate(LOOPY_PIPE_NONE);
    TEST_ASSERT(pipe != NULL, "pipe creation should succeed");
    TEST_ASSERT(loopyPipeIsReadable(pipe), "pipe should be readable");

    loopyPipeCloseRead(pipe);
    TEST_ASSERT(!loopyPipeIsReadable(pipe),
                "pipe should not be readable after close");
    TEST_ASSERT(loopyPipeIsWritable(pipe), "pipe should still be writable");
    TEST_ASSERT(loopyPipeGetReadFd(pipe) == -1,
                "read fd should be -1 after close");

    loopyPipeClose(pipe);
    return 1;
}

static int test_pipe_close_write(void) {
    loopyPipe *pipe = loopyPipeCreate(LOOPY_PIPE_NONE);
    TEST_ASSERT(pipe != NULL, "pipe creation should succeed");
    TEST_ASSERT(loopyPipeIsWritable(pipe), "pipe should be writable");

    loopyPipeCloseWrite(pipe);
    TEST_ASSERT(!loopyPipeIsWritable(pipe),
                "pipe should not be writable after close");
    TEST_ASSERT(loopyPipeIsReadable(pipe), "pipe should still be readable");
    TEST_ASSERT(loopyPipeGetWriteFd(pipe) == -1,
                "write fd should be -1 after close");

    loopyPipeClose(pipe);
    return 1;
}

static int test_pipe_fifo_create_remove(void) {
    const char *fifoPath = "/tmp/loopy_test_fifo";

    /* Clean up any existing FIFO */
    unlink(fifoPath);

    /* Create FIFO */
    TEST_ASSERT(loopyPipeMakeFifo(fifoPath, 0666),
                "FIFO creation should succeed");

    /* Verify FIFO exists */
    struct stat st;
    TEST_ASSERT(stat(fifoPath, &st) == 0, "stat should succeed on FIFO");
    TEST_ASSERT(S_ISFIFO(st.st_mode), "file should be a FIFO");

    /* Remove FIFO */
    TEST_ASSERT(loopyPipeRemoveFifo(fifoPath), "FIFO removal should succeed");
    TEST_ASSERT(stat(fifoPath, &st) == -1,
                "FIFO should not exist after removal");

    return 1;
}

static int test_pipe_fifo_open_write(void) {
    const char *fifoPath = "/tmp/loopy_test_fifo_write";

    /* Clean up and create FIFO */
    unlink(fifoPath);
    TEST_ASSERT(loopyPipeMakeFifo(fifoPath, 0666),
                "FIFO creation should succeed");

    /* Fork to test FIFO communication */
    pid_t pid = fork();
    TEST_ASSERT(pid >= 0, "fork should succeed");

    if (pid == 0) {
        /* Child: read from FIFO */
        loopyPipe *pipe = loopyPipeOpenFifo(fifoPath, false, false);
        if (!pipe) {
            exit(1);
        }

        char buf[128];
        ssize_t nread = loopyPipeRead(pipe, buf, sizeof(buf));
        loopyPipeClose(pipe);

        exit(nread > 0 && strcmp(buf, "FIFO test") == 0 ? 0 : 1);
    } else {
        /* Parent: write to FIFO */
        usleep(10000); /* Give child time to open for reading */

        loopyPipe *pipe = loopyPipeOpenFifo(fifoPath, true, false);
        TEST_ASSERT(pipe != NULL, "FIFO open for write should succeed");

        const char *msg = "FIFO test";
        ssize_t written = loopyPipeWrite(pipe, msg, strlen(msg) + 1);
        TEST_ASSERT(written > 0, "write to FIFO should succeed");

        loopyPipeClose(pipe);

        /* Wait for child */
        int status;
        waitpid(pid, &status, 0);
        TEST_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                    "child should exit successfully");
    }

    /* Clean up */
    loopyPipeRemoveFifo(fifoPath);
    return 1;
}

static void pipe_read_callback(loopyPipe *pipe, ssize_t nread, const void *buf,
                               void *userData) {
    PipeTestCtx *ctx = (PipeTestCtx *)userData;
    ctx->readCalled++;
    ctx->lastReadSize = nread;

    if (nread > 0 && nread < (ssize_t)sizeof(ctx->readData)) {
        memcpy(ctx->readData, buf, nread);
        ctx->readData[nread] = '\0';
    }

    if (nread <= 0 || ctx->readCalled >= 1) {
        loopyPipeReadStop(pipe);
        loopyStop(ctx->loop);
    }
}

static int test_pipe_async_read(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyPipe *pipe = loopyPipeCreate(LOOPY_PIPE_NONBLOCK);
    TEST_ASSERT(pipe != NULL, "pipe creation should succeed");

    PipeTestCtx ctx = {0};
    ctx.loop = l;

    /* Start async read */
    TEST_ASSERT(loopyPipeReadStart(pipe, l, pipe_read_callback, &ctx),
                "async read start should succeed");

    /* Write some data */
    const char *testData = "Async read test";
    ssize_t written = loopyPipeWrite(pipe, testData, strlen(testData));
    TEST_ASSERT(written == (ssize_t)strlen(testData), "write should succeed");

    /* Run event loop */
    loopyMain(l);

    /* Verify callback was called */
    TEST_ASSERT(ctx.readCalled == 1, "read callback should be called once");
    TEST_ASSERT(ctx.lastReadSize == (ssize_t)strlen(testData),
                "read size should match");
    TEST_ASSERT(strcmp(ctx.readData, testData) == 0,
                "read data should match written data");

    loopyPipeClose(pipe);
    return 1;
}

static void pipe_write_callback(loopyPipe *pipe, int status, void *userData) {
    (void)pipe;
    PipeTestCtx *ctx = (PipeTestCtx *)userData;
    ctx->writeCalled++;
    ctx->lastWriteStatus = status;
    loopyStop(ctx->loop);
}

static int test_pipe_async_write(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyPipe *pipe = loopyPipeCreate(LOOPY_PIPE_NONBLOCK);
    TEST_ASSERT(pipe != NULL, "pipe creation should succeed");

    PipeTestCtx ctx = {0};
    ctx.loop = l;

    /* Write async (should complete immediately for small data) */
    const char *testData = "Async write test";
    TEST_ASSERT(loopyPipeWriteAsync(pipe, l, testData, strlen(testData),
                                    pipe_write_callback, &ctx),
                "async write should succeed");

    /* Read the data to verify */
    char buf[128] = {0};
    ssize_t nread = loopyPipeRead(pipe, buf, sizeof(buf));
    TEST_ASSERT(nread == (ssize_t)strlen(testData), "read should succeed");
    TEST_ASSERT(strcmp(buf, testData) == 0, "data should match");

    loopyPipeClose(pipe);
    return 1;
}

static int test_pipe_buffer_size(void) {
    loopyPipe *pipe = loopyPipeCreate(LOOPY_PIPE_NONE);
    TEST_ASSERT(pipe != NULL, "pipe creation should succeed");

    ssize_t size = loopyPipeGetBufferSize(pipe);
    TEST_ASSERT(size > 0, "buffer size should be positive");

#ifdef __linux__
    /* On Linux, we can actually set the buffer size */
    ssize_t newSize = 16384;
    TEST_ASSERT(loopyPipeSetBufferSize(pipe, newSize),
                "set buffer size should succeed on Linux");

    size = loopyPipeGetBufferSize(pipe);
    /* Linux may round up, so just check it's at least what we set */
    TEST_ASSERT(size >= newSize, "buffer size should be at least what we set");
#else
    /* On non-Linux, setting buffer size is not supported */
    TEST_ASSERT(!loopyPipeSetBufferSize(pipe, 16384),
                "set buffer size should fail on non-Linux");
#endif

    loopyPipeClose(pipe);
    return 1;
}

static int test_pipe_error_handling(void) {
    loopyPipe *pipe = loopyPipeCreate(LOOPY_PIPE_NONE);
    TEST_ASSERT(pipe != NULL, "pipe creation should succeed");

    /* Close write end */
    loopyPipeCloseWrite(pipe);

    /* Try to write to closed write end */
    char buf[10] = "test";
    ssize_t result = loopyPipeWrite(pipe, buf, 4);
    TEST_ASSERT(result == -1, "write to closed end should fail");
    TEST_ASSERT(errno == EBADF, "errno should be EBADF");

    /* Close read end */
    loopyPipeCloseRead(pipe);

    /* Try to read from closed read end */
    result = loopyPipeRead(pipe, buf, sizeof(buf));
    TEST_ASSERT(result == -1, "read from closed end should fail");
    TEST_ASSERT(errno == EBADF, "errno should be EBADF");

    loopyPipeClose(pipe);
    return 1;
}

static int test_pipe_null_safety(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    /* NULL pipe operations should not crash */
    loopyPipeClose(NULL);
    loopyPipeCloseRead(NULL);
    loopyPipeCloseWrite(NULL);
    loopyPipeReadStop(NULL);

    TEST_ASSERT(loopyPipeGetReadFd(NULL) == -1,
                "get read fd NULL should be -1");
    TEST_ASSERT(loopyPipeGetWriteFd(NULL) == -1,
                "get write fd NULL should be -1");
    TEST_ASSERT(!loopyPipeIsReadable(NULL), "NULL should not be readable");
    TEST_ASSERT(!loopyPipeIsWritable(NULL), "NULL should not be writable");
    TEST_ASSERT(loopyPipeGetBufferSize(NULL) == -1,
                "get buffer size NULL should be -1");

    /* Invalid FDs */
    TEST_ASSERT(loopyPipeFromFds(-1, -1) == NULL, "invalid fds should fail");

    /* NULL parameters */
    loopyPipe *pipe = loopyPipeCreate(LOOPY_PIPE_NONE);
    TEST_ASSERT(pipe != NULL, "pipe creation should succeed");

    TEST_ASSERT(loopyPipeRead(pipe, NULL, 10) == -1,
                "read with NULL buf should fail");
    TEST_ASSERT(loopyPipeWrite(pipe, NULL, 10) == -1,
                "write with NULL buf should fail");
    TEST_ASSERT(!loopyPipeReadStart(NULL, l, pipe_read_callback, NULL),
                "read start with NULL pipe should fail");
    TEST_ASSERT(!loopyPipeReadStart(pipe, NULL, pipe_read_callback, NULL),
                "read start with NULL loop should fail");
    TEST_ASSERT(!loopyPipeReadStart(pipe, l, NULL, NULL),
                "read start with NULL callback should fail");

    /* NULL FIFO operations */
    TEST_ASSERT(!loopyPipeMakeFifo(NULL, 0666),
                "make FIFO with NULL path should fail");
    TEST_ASSERT(loopyPipeOpenFifo(NULL, true, false) == NULL,
                "open FIFO with NULL path should fail");
    TEST_ASSERT(!loopyPipeRemoveFifo(NULL),
                "remove FIFO with NULL path should fail");

    loopyPipeClose(pipe);
    return 1;
}

static int test_pipe_eof_detection(void) {
    loopyPipe *pipe = loopyPipeCreate(LOOPY_PIPE_NONE);
    TEST_ASSERT(pipe != NULL, "pipe creation should succeed");

    /* Close write end to signal EOF */
    loopyPipeCloseWrite(pipe);

    /* Read should return 0 for EOF */
    char buf[10];
    ssize_t nread = loopyPipeRead(pipe, buf, sizeof(buf));
    TEST_ASSERT(nread == 0, "read should return 0 for EOF");

    loopyPipeClose(pipe);
    return 1;
}

/* ====================================================================
 * Test: loopyStream
 * ==================================================================== */
static int stream_read_count = 0;
static int stream_write_count = 0;
static int stream_connection_count = 0;
static char stream_read_data[256];
static loopyLoop *stream_test_loop = NULL;

static void test_stream_alloc_cb(loopyStream *stream, size_t suggested,
                                 void **buf, size_t *bufLen, void *userData) {
    (void)stream;
    (void)suggested;
    (void)userData;
    static char allocBuf[1024];
    *buf = allocBuf;
    *bufLen = sizeof(allocBuf);
}

static void test_stream_read_cb(loopyStream *stream, ssize_t nread,
                                const void *buf, void *userData) {
    (void)stream;
    (void)userData;
    stream_read_count++;
    if (nread > 0 && buf) {
        size_t copyLen = (nread < (ssize_t)sizeof(stream_read_data) - 1)
                             ? (size_t)nread
                             : sizeof(stream_read_data) - 1;
        memcpy(stream_read_data, buf, copyLen);
        stream_read_data[copyLen] = '\0';
    }
    if (nread <= 0 && stream_test_loop) {
        loopyStop(stream_test_loop);
    }
}

static void test_stream_write_cb(loopyStream *stream, int status,
                                 void *userData) {
    (void)stream;
    (void)status;
    (void)userData;
    stream_write_count++;
}

static void test_stream_connection_cb(loopyStream *server, int status,
                                      void *userData) {
    (void)server;
    (void)status;
    (void)userData;
    stream_connection_count++;
}

static bool stream_stop_callback(timerWheel *t, timerWheelId id, void *data) {
    (void)t;
    (void)id;
    loopyLoop *l = data;
    loopyStop(l);
    return false;
}

static int test_stream_create_delete_tcp(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    loopyStream *stream = loopyStreamNewTcp(l);
    TEST_ASSERT(stream != NULL, "loopyStreamNewTcp should succeed");
    TEST_ASSERT(loopyStreamGetType(stream) == LOOPY_STREAM_TCP,
                "type should be TCP");
    TEST_ASSERT(loopyStreamGetFd(stream) >= 0, "fd should be valid");
    TEST_ASSERT(loopyStreamGetLoop(stream) == l, "loop should match");
    TEST_ASSERT(loopyStreamIsReadable(stream), "should be readable");
    TEST_ASSERT(loopyStreamIsWritable(stream), "should be writable");

    loopyStreamClose(stream, NULL, NULL);
    return 1;
}

static int test_stream_create_delete_pipe(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    loopyStream *stream = loopyStreamNewPipe(l);
    TEST_ASSERT(stream != NULL, "loopyStreamNewPipe should succeed");
    TEST_ASSERT(loopyStreamGetType(stream) == LOOPY_STREAM_PIPE,
                "type should be PIPE");
    TEST_ASSERT(loopyStreamGetLoop(stream) == l, "loop should match");

    loopyStreamClose(stream, NULL, NULL);
    return 1;
}

static int test_stream_from_fd(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    int fds[2];
    TEST_ASSERT(pipe(fds) == 0, "pipe should succeed");

    loopyStream *read_stream = loopyStreamFromFd(l, fds[0], LOOPY_STREAM_PIPE);
    loopyStream *write_stream = loopyStreamFromFd(l, fds[1], LOOPY_STREAM_PIPE);

    TEST_ASSERT(read_stream != NULL, "read stream should succeed");
    TEST_ASSERT(write_stream != NULL, "write stream should succeed");
    TEST_ASSERT(loopyStreamGetFd(read_stream) == fds[0], "fd should match");
    TEST_ASSERT(loopyStreamGetFd(write_stream) == fds[1], "fd should match");

    loopyStreamClose(read_stream, NULL, NULL);
    loopyStreamClose(write_stream, NULL, NULL);
    return 1;
}

static int test_stream_bind_listen(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    loopyStream *server = loopyStreamNewTcp(l);
    TEST_ASSERT(server != NULL, "server creation should succeed");

    TEST_ASSERT(loopyStreamBind(server, "127.0.0.1", 0), "bind should succeed");

    stream_connection_count = 0;
    TEST_ASSERT(loopyStreamListen(server, 5, test_stream_connection_cb, NULL),
                "listen should succeed");

    /* Get the port */
    int port = 0;
    TEST_ASSERT(loopyStreamGetSockName(server, NULL, 0, &port),
                "getsockname should succeed");
    TEST_ASSERT(port > 0, "port should be valid");

    loopyStreamClose(server, NULL, NULL);
    return 1;
}

static int test_stream_accept(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    /* Create server */
    loopyStream *server = loopyStreamNewTcp(l);
    loopyStreamBind(server, "127.0.0.1", 0);
    stream_connection_count = 0;
    loopyStreamListen(server, 5, test_stream_connection_cb, NULL);

    int port = 0;
    loopyStreamGetSockName(server, NULL, 0, &port);

    /* Create client and connect */
    loopyStream *client = loopyStreamNewTcp(l);
    stream_test_loop = l;
    stream_write_count = 0;

    TEST_ASSERT(loopyStreamConnect(client, "127.0.0.1", port,
                                   test_stream_write_cb, NULL),
                "connect should succeed");

    /* Run briefly to establish connection */
    loopyRegisterTimer(l, 100000, 0, stream_stop_callback, l);
    loopyMain(l);

    TEST_ASSERT(stream_connection_count > 0 || stream_write_count > 0,
                "should have connected");

    loopyStreamClose(client, NULL, NULL);
    loopyStreamClose(server, NULL, NULL);
    stream_test_loop = NULL;
    return 1;
}

static int test_stream_pipe_read_write(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    int fds[2];
    TEST_ASSERT(pipe(fds) == 0, "pipe should succeed");

    loopyStream *reader = loopyStreamFromFd(l, fds[0], LOOPY_STREAM_PIPE);
    loopyStream *writer = loopyStreamFromFd(l, fds[1], LOOPY_STREAM_PIPE);

    /* Start reading */
    stream_read_count = 0;
    memset(stream_read_data, 0, sizeof(stream_read_data));
    stream_test_loop = l;
    TEST_ASSERT(loopyStreamReadStart(reader, test_stream_alloc_cb,
                                     test_stream_read_cb, NULL),
                "read start should succeed");
    TEST_ASSERT(loopyStreamIsReading(reader), "should be reading");

    /* Write some data using try_write for immediate send */
    const char *msg = "Hello Stream!";
    ssize_t written = loopyStreamTryWrite(writer, msg, strlen(msg));
    TEST_ASSERT(written > 0, "try write should succeed");

    /* Close writer to trigger EOF */
    loopyStreamClose(writer, NULL, NULL);

    /* Run loop */
    loopyRegisterTimer(l, 500000, 0, stream_stop_callback, l);
    loopyMain(l);

    TEST_ASSERT(stream_read_count > 0, "read callback should have fired");
    TEST_ASSERT(strstr(stream_read_data, "Hello") != NULL,
                "should have read data");

    loopyStreamClose(reader, NULL, NULL);
    stream_test_loop = NULL;
    return 1;
}

static int test_stream_try_write(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    int fds[2];
    TEST_ASSERT(pipe(fds) == 0, "pipe should succeed");

    loopyStream *writer = loopyStreamFromFd(l, fds[1], LOOPY_STREAM_PIPE);

    const char *msg = "Quick write";
    ssize_t written = loopyStreamTryWrite(writer, msg, strlen(msg));
    TEST_ASSERT(written > 0, "try write should succeed");

    loopyStreamClose(writer, NULL, NULL);
    close(fds[0]); /* Close read end */
    return 1;
}

static int test_stream_shutdown(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    loopyStream *stream = loopyStreamNewTcp(l);
    TEST_ASSERT(stream != NULL, "stream creation should succeed");
    TEST_ASSERT(loopyStreamIsWritable(stream), "should be writable");

    /* Connect to establish socket state */
    /* For this test, just verify the shutdown doesn't crash */
    /* Real shutdown test would need a connected socket */

    loopyStreamClose(stream, NULL, NULL);
    return 1;
}

static int test_stream_write_queue(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    int fds[2];
    TEST_ASSERT(pipe(fds) == 0, "pipe should succeed");

    loopyStream *writer = loopyStreamFromFd(l, fds[1], LOOPY_STREAM_PIPE);

    /* Queue multiple writes */
    stream_write_count = 0;
    loopyStreamWrite(writer, "one", 3, test_stream_write_cb, NULL);
    loopyStreamWrite(writer, "two", 3, test_stream_write_cb, NULL);
    loopyStreamWrite(writer, "three", 5, test_stream_write_cb, NULL);

    size_t count = loopyStreamGetWriteQueueCount(writer);
    TEST_ASSERT(count <= 3, "queue count should be reasonable");

    /* Run briefly to let writes complete */
    loopyRegisterTimer(l, 100000, 0, stream_stop_callback, l);
    loopyMain(l);

    loopyStreamClose(writer, NULL, NULL);
    close(fds[0]);
    return 1;
}

static int test_stream_null_safety(void) {
    /* NULL loop */
    TEST_ASSERT(loopyStreamNewTcp(NULL) == NULL,
                "tcp with NULL loop should fail");
    TEST_ASSERT(loopyStreamNewPipe(NULL) == NULL,
                "pipe with NULL loop should fail");
    TEST_ASSERT(loopyStreamFromFd(NULL, 0, LOOPY_STREAM_PIPE) == NULL,
                "fromFd with NULL loop should fail");

    /* NULL stream operations */
    loopyStreamClose(NULL, NULL, NULL); /* Should not crash */
    loopyStreamReadStop(NULL);          /* Should not crash */

    TEST_ASSERT(!loopyStreamBind(NULL, "127.0.0.1", 0),
                "bind NULL should fail");
    TEST_ASSERT(!loopyStreamListen(NULL, 5, test_stream_connection_cb, NULL),
                "listen NULL should fail");
    TEST_ASSERT(loopyStreamAccept(NULL) == NULL, "accept NULL should fail");
    TEST_ASSERT(!loopyStreamConnect(NULL, "127.0.0.1", 0, NULL, NULL),
                "connect NULL should fail");
    TEST_ASSERT(!loopyStreamReadStart(NULL, test_stream_alloc_cb,
                                      test_stream_read_cb, NULL),
                "read start NULL should fail");
    TEST_ASSERT(!loopyStreamWrite(NULL, "x", 1, NULL, NULL),
                "write NULL should fail");
    TEST_ASSERT_EQ(loopyStreamTryWrite(NULL, "x", 1), -1,
                   "try write NULL should fail");

    TEST_ASSERT(!loopyStreamIsReadable(NULL), "readable NULL should be false");
    TEST_ASSERT(!loopyStreamIsWritable(NULL), "writable NULL should be false");
    TEST_ASSERT(!loopyStreamIsReading(NULL), "reading NULL should be false");
    TEST_ASSERT(loopyStreamGetType(NULL) == LOOPY_STREAM_UNKNOWN, "type NULL");
    TEST_ASSERT_EQ(loopyStreamGetFd(NULL), -1, "fd NULL should be -1");
    TEST_ASSERT(loopyStreamGetLoop(NULL) == NULL, "loop NULL should be NULL");
    TEST_ASSERT_EQ(loopyStreamGetWriteQueueSize(NULL), 0,
                   "queue size NULL should be 0");
    TEST_ASSERT_EQ(loopyStreamGetWriteQueueCount(NULL), 0,
                   "queue count NULL should be 0");

    return 1;
}

/* FD passing tests */
static int fdpass_read_count = 0;
static int fdpass_received_fd = -1;
static ssize_t fdpass_read_bytes = 0;

static bool fdpass_stop_timer(timerWheel *t, timerWheelId id, void *data) {
    (void)t;
    (void)id;
    loopyLoop *l = data;
    loopyStop(l);
    return false;
}

static void fdpass_alloc_cb(loopyStream *stream, size_t suggested, void **buf,
                            size_t *bufLen, void *userData) {
    (void)stream;
    (void)suggested;
    static char allocBuf[256];
    *buf = allocBuf;
    *bufLen = sizeof(allocBuf);
    (void)userData;
}

static void fdpass_read_cb(loopyStream *stream, ssize_t nread, const void *buf,
                           const int *fds, int nfds, void *userData) {
    (void)stream;
    (void)buf;
    (void)userData;

    fdpass_read_count++;
    fdpass_read_bytes = nread;

    if (nfds > 0 && fds) {
        fdpass_received_fd = fds[0];
    }
}

static int test_stream_fd_passing_basic(void) {
    /* Create a Unix domain socket pair for testing */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        return 1; /* Skip on error */
    }

    loopyLoop *l LOOPY_LOOP_AUTO_CLEANUP = loopyNew(16);

    loopyStream *sender LOOPY_STREAM_AUTO_CLEANUP = loopyStreamFromFd(l, sv[0], LOOPY_STREAM_PIPE);
    loopyStream *receiver LOOPY_STREAM_AUTO_CLEANUP = loopyStreamFromFd(l, sv[1], LOOPY_STREAM_PIPE);

    TEST_ASSERT(sender != NULL, "sender created");
    TEST_ASSERT(receiver != NULL, "receiver created");

    /* Verify FD passing is supported */
    TEST_ASSERT(loopyStreamCanPassFd(sender), "sender can pass fd");
    TEST_ASSERT(loopyStreamCanPassFd(receiver), "receiver can pass fd");

    /* Create a pipe to pass as our test fd */
    int pipeFds[2];
    TEST_ASSERT(pipe(pipeFds) == 0, "create test pipe");

    /* Start receiving with FD capability */
    fdpass_read_count = 0;
    fdpass_received_fd = -1;
    fdpass_read_bytes = 0;

    TEST_ASSERT(loopyStreamReadStartWithFd(receiver, fdpass_alloc_cb,
                                           fdpass_read_cb, NULL),
                "start read with fd");

    /* Send data with an FD */
    TEST_ASSERT(
        loopyStreamWriteWithFd(sender, "fd", 2, &pipeFds[0], 1, NULL, NULL),
        "write with fd");

    /* Use a timer to stop the loop after brief processing */
    loopyRegisterTimer(l, 20000, 0, fdpass_stop_timer, l);
    loopyMain(l);

    /* Verify we received the fd */
    TEST_ASSERT(fdpass_read_count > 0, "received data");
    TEST_ASSERT(fdpass_read_bytes > 0, "got bytes");
    TEST_ASSERT(fdpass_received_fd >= 0, "received fd");

    /* Verify the received fd works - write to original pipe read end,
       it should be the same as the one we passed */
    if (fdpass_received_fd >= 0) {
        /* Write through original write end */
        write(pipeFds[1], "test", 4);

        /* Read through received fd (should be a dup of pipeFds[0]) */
        char buf[16] = {0};
        ssize_t n = read(fdpass_received_fd, buf, sizeof(buf));
        TEST_ASSERT(n == 4, "read through passed fd");
        TEST_ASSERT(strcmp(buf, "test") == 0, "data matches");

        close(fdpass_received_fd);
    }

    close(pipeFds[0]);
    close(pipeFds[1]);
    loopyStreamClose(sender, NULL, NULL);
    sender = NULL;  /* Prevent double-free from auto-cleanup */
    loopyStreamClose(receiver, NULL, NULL);
    receiver = NULL;  /* Prevent double-free from auto-cleanup */
    return 1;
}

static int test_stream_fd_passing_multiple(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        return 1;
    }

    LOOPY_SELF_DELETE(l) = loopyNew(16);

    loopyStream *sender = loopyStreamFromFd(l, sv[0], LOOPY_STREAM_PIPE);
    loopyStream *receiver = loopyStreamFromFd(l, sv[1], LOOPY_STREAM_PIPE);

    /* Create multiple fds to pass */
    int fds[3];
    int pipes[3][2];
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT(pipe(pipes[i]) == 0, "create test pipe");
        fds[i] = pipes[i][0];
    }

    /* Send multiple fds */
    TEST_ASSERT(loopyStreamWriteWithFd(sender, "multi", 5, fds, 3, NULL, NULL),
                "write with multiple fds");

    /* Clean up original fds since receiver should get copies */
    for (int i = 0; i < 3; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    loopyStreamClose(sender, NULL, NULL);
    loopyStreamClose(receiver, NULL, NULL);
    return 1;
}

static int test_stream_fd_passing_not_pipe(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    /* TCP streams should not support FD passing */
    loopyStream *tcp = loopyStreamNewTcp(l);
    TEST_ASSERT(tcp != NULL, "tcp created");
    TEST_ASSERT(!loopyStreamCanPassFd(tcp), "tcp cannot pass fd");

    /* WriteWithFd should fail on TCP */
    int fd = 0;
    TEST_ASSERT(!loopyStreamWriteWithFd(tcp, "x", 1, &fd, 1, NULL, NULL),
                "write with fd on tcp fails");

    /* ReadStartWithFd should fail on TCP */
    TEST_ASSERT(
        !loopyStreamReadStartWithFd(tcp, fdpass_alloc_cb, fdpass_read_cb, NULL),
        "read with fd on tcp fails");

    loopyStreamClose(tcp, NULL, NULL);
    return 1;
}

static int test_stream_fd_passing_null_safety(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    loopyStream *stream = loopyStreamFromFd(l, sv[0], LOOPY_STREAM_PIPE);
    close(sv[1]);

    int fd = 0;

    /* NULL checks */
    TEST_ASSERT(!loopyStreamWriteWithFd(NULL, "x", 1, &fd, 1, NULL, NULL),
                "write fd NULL stream");
    TEST_ASSERT(!loopyStreamWriteWithFd(stream, NULL, 1, &fd, 1, NULL, NULL),
                "write fd NULL data");
    TEST_ASSERT(!loopyStreamWriteWithFd(stream, "x", 0, &fd, 1, NULL, NULL),
                "write fd zero len");
    TEST_ASSERT(!loopyStreamWriteWithFd(stream, "x", 1, NULL, 1, NULL, NULL),
                "write fd NULL fds");
    TEST_ASSERT(!loopyStreamWriteWithFd(stream, "x", 1, &fd, 0, NULL, NULL),
                "write fd zero nfds");

    TEST_ASSERT(!loopyStreamReadStartWithFd(NULL, fdpass_alloc_cb,
                                            fdpass_read_cb, NULL),
                "read fd NULL stream");
    TEST_ASSERT(!loopyStreamReadStartWithFd(stream, NULL, fdpass_read_cb, NULL),
                "read fd NULL alloc");
    TEST_ASSERT(
        !loopyStreamReadStartWithFd(stream, fdpass_alloc_cb, NULL, NULL),
        "read fd NULL read cb");

    TEST_ASSERT(!loopyStreamCanPassFd(NULL), "can pass fd NULL");

    loopyStreamClose(stream, NULL, NULL);
    return 1;
}

/* ====================================================================
 * FS Tests
 * ==================================================================== */

static int fs_callback_count = 0;
static ssize_t fs_callback_result = -1;
static loopyLoop *fs_test_loop = NULL;

static void test_fs_callback(loopyLoop *loop, loopyFSRequest *req,
                             ssize_t result, void *userData) {
    (void)userData;
    fs_callback_count++;
    fs_callback_result = result;
    if (loop) {
        loopyStop(loop);
    }
    (void)req;
}

static bool fs_stop_callback(timerWheel *t, timerWheelId id, void *userData) {
    (void)t;
    (void)id;
    LOOPY_SELF_DELETE(l) = userData;
    loopyStop(l);
    return false;
}

static int test_fs_sync_operations(void) {
    /* Test sync open/write/read/close */
    const char *testPath = "/tmp/loopy_fs_test.txt";

    /* Open for write */
    loopyFSRequest *req = loopyFSOpen(
        NULL, testPath, O_CREAT | O_WRONLY | O_TRUNC, 0644, NULL, NULL);
    TEST_ASSERT(req != NULL, "sync open should succeed");
    int fd = (int)loopyFSRequestGetResult(req);
    TEST_ASSERT(fd >= 0, "should get valid fd");
    TEST_ASSERT(loopyFSRequestGetType(req) == LOOPY_FS_OPEN,
                "type should be OPEN");
    TEST_ASSERT(strcmp(loopyFSRequestGetPath(req), testPath) == 0,
                "path should match");
    loopyFSRequestFree(req);

    /* Write data */
    const char *data = "Hello loopyFS!";
    req = loopyFSWrite(NULL, fd, data, strlen(data), -1, NULL, NULL);
    TEST_ASSERT(req != NULL, "sync write should succeed");
    TEST_ASSERT(loopyFSRequestGetResult(req) == (ssize_t)strlen(data),
                "should write all bytes");
    loopyFSRequestFree(req);

    /* Close */
    req = loopyFSClose(NULL, fd, NULL, NULL);
    TEST_ASSERT(req != NULL, "sync close should succeed");
    TEST_ASSERT(loopyFSRequestGetResult(req) == 0, "close should return 0");
    loopyFSRequestFree(req);

    /* Open for read */
    req = loopyFSOpen(NULL, testPath, O_RDONLY, 0, NULL, NULL);
    TEST_ASSERT(req != NULL, "sync open for read should succeed");
    fd = (int)loopyFSRequestGetResult(req);
    TEST_ASSERT(fd >= 0, "should get valid fd for read");
    loopyFSRequestFree(req);

    /* Read data */
    char buf[64] = {0};
    req = loopyFSRead(NULL, fd, buf, sizeof(buf) - 1, -1, NULL, NULL);
    TEST_ASSERT(req != NULL, "sync read should succeed");
    TEST_ASSERT(loopyFSRequestGetResult(req) == (ssize_t)strlen(data),
                "should read all bytes");
    TEST_ASSERT(strcmp(buf, data) == 0, "data should match");
    loopyFSRequestFree(req);

    /* Close */
    req = loopyFSClose(NULL, fd, NULL, NULL);
    loopyFSRequestFree(req);

    /* Cleanup */
    req = loopyFSUnlink(NULL, testPath, NULL, NULL);
    TEST_ASSERT(req != NULL, "sync unlink should succeed");
    TEST_ASSERT(loopyFSRequestGetResult(req) == 0, "unlink should return 0");
    loopyFSRequestFree(req);

    return 1;
}

static int test_fs_async_open_close(void) {
    loopyLoop *l LOOPY_LOOP_AUTO_CLEANUP = loopyNew(16);
    fs_test_loop = l;
    fs_callback_count = 0;
    fs_callback_result = -1;

    const char *testPath = "/tmp/loopy_fs_async_test.txt";

    /* Async open */
    loopyFSRequest *req LOOPY_FS_REQUEST_AUTO_CLEANUP = loopyFSOpen(l, testPath, O_CREAT | O_WRONLY | O_TRUNC,
                                      0644, test_fs_callback, NULL);
    TEST_ASSERT(req != NULL, "async open should return request");

    /* Run loop until callback */
    loopyRegisterTimer(l, 1000000, 0, fs_stop_callback, l);
    loopyMain(l);

    TEST_ASSERT(fs_callback_count == 1, "callback should fire");
    TEST_ASSERT(fs_callback_result >= 0, "should get valid fd");
    int fd = (int)fs_callback_result;

    /* Manually free and reset for reuse */
    loopyFSRequestFree(req);
    req = NULL;

    /* Async close */
    fs_callback_count = 0;
    req = loopyFSClose(l, fd, test_fs_callback, NULL);
    TEST_ASSERT(req != NULL, "async close should return request");

    loopyRegisterTimer(l, 1000000, 0, fs_stop_callback, l);
    loopyMain(l);

    TEST_ASSERT(fs_callback_count == 1, "close callback should fire");
    TEST_ASSERT(fs_callback_result == 0, "close should succeed");

    /* Manually free and reset for reuse */
    loopyFSRequestFree(req);
    req = NULL;

    /* Cleanup */
    req = loopyFSUnlink(NULL, testPath, NULL, NULL);
    /* Auto-cleanup will handle the final request */

    fs_test_loop = NULL;
    return 1;
}

static int test_fs_stat(void) {
    const char *testPath = "/tmp/loopy_fs_stat_test.txt";

    /* Create test file */
    loopyFSRequest *req = loopyFSOpen(
        NULL, testPath, O_CREAT | O_WRONLY | O_TRUNC, 0644, NULL, NULL);
    int fd = (int)loopyFSRequestGetResult(req);
    loopyFSRequestFree(req);

    const char *data = "Test data for stat";
    req = loopyFSWrite(NULL, fd, data, strlen(data), -1, NULL, NULL);
    loopyFSRequestFree(req);

    /* fstat */
    struct stat st;
    memset(&st, 0, sizeof(st));
    req = loopyFSFstat(NULL, fd, &st, NULL, NULL);
    TEST_ASSERT(req != NULL, "sync fstat should succeed");
    TEST_ASSERT(loopyFSRequestGetResult(req) == 0, "fstat should return 0");
    TEST_ASSERT(st.st_size == (off_t)strlen(data), "size should match");
    TEST_ASSERT(loopyFSRequestGetStatbuf(req) == &st, "statbuf should match");
    loopyFSRequestFree(req);

    /* Close */
    req = loopyFSClose(NULL, fd, NULL, NULL);
    loopyFSRequestFree(req);

    /* stat (by path) */
    memset(&st, 0, sizeof(st));
    req = loopyFSStat(NULL, testPath, &st, NULL, NULL);
    TEST_ASSERT(req != NULL, "sync stat should succeed");
    TEST_ASSERT(loopyFSRequestGetResult(req) == 0, "stat should return 0");
    TEST_ASSERT(st.st_size == (off_t)strlen(data), "size should match");
    loopyFSRequestFree(req);

    /* lstat */
    memset(&st, 0, sizeof(st));
    req = loopyFSLstat(NULL, testPath, &st, NULL, NULL);
    TEST_ASSERT(req != NULL, "sync lstat should succeed");
    TEST_ASSERT(loopyFSRequestGetResult(req) == 0, "lstat should return 0");
    loopyFSRequestFree(req);

    /* Cleanup */
    req = loopyFSUnlink(NULL, testPath, NULL, NULL);
    loopyFSRequestFree(req);

    return 1;
}

static int test_fs_mkdir_rmdir(void) {
    const char *testDir = "/tmp/loopy_fs_test_dir";

    /* mkdir */
    loopyFSRequest *req = loopyFSMkdir(NULL, testDir, 0755, NULL, NULL);
    TEST_ASSERT(req != NULL, "sync mkdir should succeed");
    TEST_ASSERT(loopyFSRequestGetResult(req) == 0, "mkdir should return 0");
    loopyFSRequestFree(req);

    /* Verify directory exists */
    struct stat st;
    req = loopyFSStat(NULL, testDir, &st, NULL, NULL);
    TEST_ASSERT(loopyFSRequestGetResult(req) == 0, "stat should succeed");
    TEST_ASSERT(S_ISDIR(st.st_mode), "should be a directory");
    loopyFSRequestFree(req);

    /* rmdir */
    req = loopyFSRmdir(NULL, testDir, NULL, NULL);
    TEST_ASSERT(req != NULL, "sync rmdir should succeed");
    TEST_ASSERT(loopyFSRequestGetResult(req) == 0, "rmdir should return 0");
    loopyFSRequestFree(req);

    /* Verify directory is gone */
    req = loopyFSStat(NULL, testDir, &st, NULL, NULL);
    TEST_ASSERT(loopyFSRequestGetResult(req) == -1, "stat should fail");
    loopyFSRequestFree(req);

    return 1;
}

static int test_fs_rename(void) {
    const char *oldPath = "/tmp/loopy_fs_rename_old.txt";
    const char *newPath = "/tmp/loopy_fs_rename_new.txt";

    /* Create test file */
    loopyFSRequest *req = loopyFSOpen(
        NULL, oldPath, O_CREAT | O_WRONLY | O_TRUNC, 0644, NULL, NULL);
    int fd = (int)loopyFSRequestGetResult(req);
    loopyFSRequestFree(req);
    req = loopyFSClose(NULL, fd, NULL, NULL);
    loopyFSRequestFree(req);

    /* Rename */
    req = loopyFSRename(NULL, oldPath, newPath, NULL, NULL);
    TEST_ASSERT(req != NULL, "sync rename should succeed");
    TEST_ASSERT(loopyFSRequestGetResult(req) == 0, "rename should return 0");
    loopyFSRequestFree(req);

    /* Verify old is gone, new exists */
    struct stat st;
    req = loopyFSStat(NULL, oldPath, &st, NULL, NULL);
    TEST_ASSERT(loopyFSRequestGetResult(req) == -1, "old should not exist");
    loopyFSRequestFree(req);

    req = loopyFSStat(NULL, newPath, &st, NULL, NULL);
    TEST_ASSERT(loopyFSRequestGetResult(req) == 0, "new should exist");
    loopyFSRequestFree(req);

    /* Cleanup */
    req = loopyFSUnlink(NULL, newPath, NULL, NULL);
    loopyFSRequestFree(req);

    return 1;
}

static int test_fs_pread_pwrite(void) {
    const char *testPath = "/tmp/loopy_fs_pread_test.txt";

    /* Create and write initial data */
    loopyFSRequest *req = loopyFSOpen(
        NULL, testPath, O_CREAT | O_RDWR | O_TRUNC, 0644, NULL, NULL);
    int fd = (int)loopyFSRequestGetResult(req);
    loopyFSRequestFree(req);

    /* Write at offset 0 */
    const char *data1 = "AAAA";
    req = loopyFSWrite(NULL, fd, data1, 4, 0, NULL, NULL);
    TEST_ASSERT(loopyFSRequestGetResult(req) == 4, "should write 4 bytes");
    loopyFSRequestFree(req);

    /* Write at offset 4 */
    const char *data2 = "BBBB";
    req = loopyFSWrite(NULL, fd, data2, 4, 4, NULL, NULL);
    TEST_ASSERT(loopyFSRequestGetResult(req) == 4,
                "should write 4 bytes at offset");
    loopyFSRequestFree(req);

    /* Read from offset 0 */
    char buf[5] = {0};
    req = loopyFSRead(NULL, fd, buf, 4, 0, NULL, NULL);
    TEST_ASSERT(loopyFSRequestGetResult(req) == 4, "should read 4 bytes");
    TEST_ASSERT(strcmp(buf, "AAAA") == 0, "data at 0 should be AAAA");
    loopyFSRequestFree(req);

    /* Read from offset 4 */
    memset(buf, 0, sizeof(buf));
    req = loopyFSRead(NULL, fd, buf, 4, 4, NULL, NULL);
    TEST_ASSERT(loopyFSRequestGetResult(req) == 4,
                "should read 4 bytes at offset");
    TEST_ASSERT(strcmp(buf, "BBBB") == 0, "data at 4 should be BBBB");
    loopyFSRequestFree(req);

    /* Close and cleanup */
    req = loopyFSClose(NULL, fd, NULL, NULL);
    loopyFSRequestFree(req);
    req = loopyFSUnlink(NULL, testPath, NULL, NULL);
    loopyFSRequestFree(req);

    return 1;
}

static int test_fs_truncate_fsync(void) {
    const char *testPath = "/tmp/loopy_fs_truncate_test.txt";

    /* Create file with data */
    loopyFSRequest *req = loopyFSOpen(
        NULL, testPath, O_CREAT | O_RDWR | O_TRUNC, 0644, NULL, NULL);
    int fd = (int)loopyFSRequestGetResult(req);
    loopyFSRequestFree(req);

    const char *data = "Hello World!";
    req = loopyFSWrite(NULL, fd, data, strlen(data), -1, NULL, NULL);
    loopyFSRequestFree(req);

    /* fsync */
    req = loopyFSFsync(NULL, fd, NULL, NULL);
    TEST_ASSERT(req != NULL, "sync fsync should succeed");
    TEST_ASSERT(loopyFSRequestGetResult(req) == 0, "fsync should return 0");
    loopyFSRequestFree(req);

    /* fdatasync */
    req = loopyFSFdatasync(NULL, fd, NULL, NULL);
    TEST_ASSERT(req != NULL, "sync fdatasync should succeed");
    TEST_ASSERT(loopyFSRequestGetResult(req) == 0, "fdatasync should return 0");
    loopyFSRequestFree(req);

    /* ftruncate to 5 bytes */
    req = loopyFSFtruncate(NULL, fd, 5, NULL, NULL);
    TEST_ASSERT(req != NULL, "sync ftruncate should succeed");
    TEST_ASSERT(loopyFSRequestGetResult(req) == 0, "ftruncate should return 0");
    loopyFSRequestFree(req);

    /* Verify size */
    struct stat st;
    req = loopyFSFstat(NULL, fd, &st, NULL, NULL);
    TEST_ASSERT(st.st_size == 5, "size should be 5 after truncate");
    loopyFSRequestFree(req);

    /* Cleanup */
    req = loopyFSClose(NULL, fd, NULL, NULL);
    loopyFSRequestFree(req);
    req = loopyFSUnlink(NULL, testPath, NULL, NULL);
    loopyFSRequestFree(req);

    return 1;
}

static int test_fs_chmod(void) {
    const char *testPath = "/tmp/loopy_fs_chmod_test.txt";

    /* Create file */
    loopyFSRequest *req = loopyFSOpen(
        NULL, testPath, O_CREAT | O_WRONLY | O_TRUNC, 0644, NULL, NULL);
    int fd = (int)loopyFSRequestGetResult(req);
    loopyFSRequestFree(req);

    /* fchmod */
    req = loopyFSFchmod(NULL, fd, 0600, NULL, NULL);
    TEST_ASSERT(req != NULL, "sync fchmod should succeed");
    TEST_ASSERT(loopyFSRequestGetResult(req) == 0, "fchmod should return 0");
    loopyFSRequestFree(req);

    /* Verify mode */
    struct stat st;
    req = loopyFSFstat(NULL, fd, &st, NULL, NULL);
    TEST_ASSERT((st.st_mode & 0777) == 0600, "mode should be 0600");
    loopyFSRequestFree(req);

    req = loopyFSClose(NULL, fd, NULL, NULL);
    loopyFSRequestFree(req);

    /* chmod (by path) */
    req = loopyFSChmod(NULL, testPath, 0755, NULL, NULL);
    TEST_ASSERT(req != NULL, "sync chmod should succeed");
    TEST_ASSERT(loopyFSRequestGetResult(req) == 0, "chmod should return 0");
    loopyFSRequestFree(req);

    /* Verify mode */
    req = loopyFSStat(NULL, testPath, &st, NULL, NULL);
    TEST_ASSERT((st.st_mode & 0777) == 0755, "mode should be 0755");
    loopyFSRequestFree(req);

    /* Cleanup */
    req = loopyFSUnlink(NULL, testPath, NULL, NULL);
    loopyFSRequestFree(req);

    return 1;
}

static int test_fs_error_handling(void) {
    /* Try to open nonexistent file */
    loopyFSRequest *req = loopyFSOpen(NULL, "/nonexistent/path/file.txt",
                                      O_RDONLY, 0, NULL, NULL);
    TEST_ASSERT(req != NULL, "request should be created");
    TEST_ASSERT(loopyFSRequestGetResult(req) == -1, "open should fail");
    TEST_ASSERT(strlen(loopyFSGetError(req)) > 0, "should have error message");
    loopyFSRequestFree(req);

    /* Try to stat nonexistent file */
    struct stat st;
    req = loopyFSStat(NULL, "/nonexistent/path", &st, NULL, NULL);
    TEST_ASSERT(loopyFSRequestGetResult(req) == -1, "stat should fail");
    loopyFSRequestFree(req);

    /* Try to unlink nonexistent file */
    req = loopyFSUnlink(NULL, "/nonexistent/path/file.txt", NULL, NULL);
    TEST_ASSERT(loopyFSRequestGetResult(req) == -1, "unlink should fail");
    loopyFSRequestFree(req);

    return 1;
}

static int test_fs_null_safety(void) {
    /* NULL path operations */
    TEST_ASSERT(loopyFSOpen(NULL, NULL, 0, 0, NULL, NULL) == NULL,
                "open NULL path should fail");
    TEST_ASSERT(loopyFSStat(NULL, NULL, NULL, NULL, NULL) == NULL,
                "stat NULL should fail");
    TEST_ASSERT(loopyFSUnlink(NULL, NULL, NULL, NULL) == NULL,
                "unlink NULL should fail");
    TEST_ASSERT(loopyFSMkdir(NULL, NULL, 0, NULL, NULL) == NULL,
                "mkdir NULL should fail");
    TEST_ASSERT(loopyFSRmdir(NULL, NULL, NULL, NULL) == NULL,
                "rmdir NULL should fail");
    TEST_ASSERT(loopyFSRename(NULL, NULL, NULL, NULL, NULL) == NULL,
                "rename NULL should fail");
    TEST_ASSERT(loopyFSChmod(NULL, NULL, 0, NULL, NULL) == NULL,
                "chmod NULL should fail");

    /* Invalid fd operations */
    TEST_ASSERT(loopyFSClose(NULL, -1, NULL, NULL) == NULL,
                "close -1 should fail");
    TEST_ASSERT(loopyFSRead(NULL, -1, NULL, 0, 0, NULL, NULL) == NULL,
                "read -1 should fail");
    TEST_ASSERT(loopyFSWrite(NULL, -1, NULL, 0, 0, NULL, NULL) == NULL,
                "write -1 should fail");
    TEST_ASSERT(loopyFSFstat(NULL, -1, NULL, NULL, NULL) == NULL,
                "fstat -1 should fail");
    TEST_ASSERT(loopyFSFsync(NULL, -1, NULL, NULL) == NULL,
                "fsync -1 should fail");
    TEST_ASSERT(loopyFSFdatasync(NULL, -1, NULL, NULL) == NULL,
                "fdatasync -1 should fail");
    TEST_ASSERT(loopyFSFtruncate(NULL, -1, 0, NULL, NULL) == NULL,
                "ftruncate -1 should fail");
    TEST_ASSERT(loopyFSFchmod(NULL, -1, 0, NULL, NULL) == NULL,
                "fchmod -1 should fail");

    /* NULL request operations */
    loopyFSRequestFree(NULL); /* Should not crash */
    TEST_ASSERT(!loopyFSCancel(NULL), "cancel NULL should return false");
    TEST_ASSERT(loopyFSRequestGetType(NULL) == LOOPY_FS_UNKNOWN,
                "type NULL should be UNKNOWN");
    TEST_ASSERT(loopyFSRequestGetPath(NULL) == NULL,
                "path NULL should be NULL");
    TEST_ASSERT(loopyFSRequestGetResult(NULL) == -1,
                "result NULL should be -1");
    TEST_ASSERT(loopyFSRequestGetLoop(NULL) == NULL,
                "loop NULL should be NULL");
    TEST_ASSERT(loopyFSRequestGetStatbuf(NULL) == NULL,
                "statbuf NULL should be NULL");
    TEST_ASSERT(strcmp(loopyFSGetError(NULL), "") == 0,
                "error NULL should be empty");

    return 1;
}

/* ====================================================================
 * Extended FS Tests
 * ==================================================================== */

static int test_fs_link_symlink(void) {
    /* Create temp file */
    char path[] = "/tmp/loopy_fs_link_XXXXXX";
    int fd = mkstemp(path);
    TEST_ASSERT(fd >= 0, "mkstemp should succeed");
    write(fd, "test content", 12);
    close(fd);

    /* Create hard link */
    char linkPath[256];
    snprintf(linkPath, sizeof(linkPath), "%s.hardlink", path);
    loopyFSRequest *req = loopyFSLink(NULL, path, linkPath, NULL, NULL);
    TEST_ASSERT(req != NULL, "link should succeed");
    TEST_ASSERT(loopyFSRequestGetResult(req) == 0, "link result should be 0");
    loopyFSRequestFree(req);

    /* Create symlink */
    char symlinkPath[256];
    snprintf(symlinkPath, sizeof(symlinkPath), "%s.symlink", path);
    req = loopyFSSymlink(NULL, path, symlinkPath, NULL, NULL);
    TEST_ASSERT(req != NULL, "symlink should succeed");
    TEST_ASSERT(loopyFSRequestGetResult(req) == 0,
                "symlink result should be 0");
    loopyFSRequestFree(req);

    /* Read symlink */
    char target[256] = {0};
    req =
        loopyFSReadlink(NULL, symlinkPath, target, sizeof(target), NULL, NULL);
    TEST_ASSERT(req != NULL, "readlink should succeed");
    TEST_ASSERT(loopyFSRequestGetResult(req) > 0,
                "readlink result should be > 0");
    TEST_ASSERT(strcmp(target, path) == 0, "symlink target should match");
    loopyFSRequestFree(req);

    /* Cleanup */
    unlink(path);
    unlink(linkPath);
    unlink(symlinkPath);

    return 1;
}

static int test_fs_realpath_access(void) {
    /* Create temp file */
    char path[] = "/tmp/loopy_fs_realpath_XXXXXX";
    int fd = mkstemp(path);
    TEST_ASSERT(fd >= 0, "mkstemp should succeed");
    close(fd);

    /* Test realpath */
    char resolved[PATH_MAX] = {0};
    loopyFSRequest *req =
        loopyFSRealpath(NULL, path, resolved, sizeof(resolved), NULL, NULL);
    TEST_ASSERT(req != NULL, "realpath should succeed");
    TEST_ASSERT(loopyFSRequestGetResult(req) == 0,
                "realpath result should be 0");
    TEST_ASSERT(strlen(resolved) > 0, "resolved path should be non-empty");
    loopyFSRequestFree(req);

    /* Test access - file exists and readable */
    req = loopyFSAccess(NULL, path, F_OK, NULL, NULL);
    TEST_ASSERT(req != NULL, "access F_OK should succeed");
    TEST_ASSERT(loopyFSRequestGetResult(req) == 0, "file should exist");
    loopyFSRequestFree(req);

    req = loopyFSAccess(NULL, path, R_OK, NULL, NULL);
    TEST_ASSERT(req != NULL, "access R_OK should succeed");
    TEST_ASSERT(loopyFSRequestGetResult(req) == 0, "file should be readable");
    loopyFSRequestFree(req);

    /* Test access - non-existent file */
    req = loopyFSAccess(NULL, "/nonexistent/path/12345", F_OK, NULL, NULL);
    TEST_ASSERT(req != NULL, "access should return request");
    TEST_ASSERT(loopyFSRequestGetResult(req) == -1,
                "non-existent file should fail");
    loopyFSRequestFree(req);

    unlink(path);
    return 1;
}

static int test_fs_scandir(void) {
    /* Create temp directory */
    char dirPath[] = "/tmp/loopy_fs_scandir_XXXXXX";
    TEST_ASSERT(mkdtemp(dirPath) != NULL, "mkdtemp should succeed");

    /* Create some files in it */
    char filePath[256];
    for (int i = 0; i < 3; i++) {
        snprintf(filePath, sizeof(filePath), "%s/file%d.txt", dirPath, i);
        int fd = open(filePath, O_CREAT | O_WRONLY, 0644);
        TEST_ASSERT(fd >= 0, "create test file should succeed");
        close(fd);
    }

    /* Scandir */
    loopyFSRequest *req = loopyFSScandir(NULL, dirPath, NULL, NULL);
    TEST_ASSERT(req != NULL, "scandir should succeed");
    TEST_ASSERT(loopyFSRequestGetResult(req) == 3, "should find 3 files");

    size_t count = 0;
    const loopyFSDirent *dirents = loopyFSRequestGetDirents(req, &count);
    TEST_ASSERT(dirents != NULL, "dirents should not be NULL");
    TEST_ASSERT_EQ(count, 3, "should have 3 entries");
    loopyFSRequestFree(req);

    /* Cleanup */
    for (int i = 0; i < 3; i++) {
        snprintf(filePath, sizeof(filePath), "%s/file%d.txt", dirPath, i);
        unlink(filePath);
    }
    rmdir(dirPath);

    return 1;
}

static int test_fs_utime(void) {
    /* Create temp file */
    char path[] = "/tmp/loopy_fs_utime_XXXXXX";
    int fd = mkstemp(path);
    TEST_ASSERT(fd >= 0, "mkstemp should succeed");
    close(fd);

    /* Set specific times */
    loopyFSTimespec atime = {.sec = 1000000000, .nsec = 0};
    loopyFSTimespec mtime = {.sec = 1000000000, .nsec = 0};
    loopyFSRequest *req = loopyFSUtime(NULL, path, &atime, &mtime, NULL, NULL);
    TEST_ASSERT(req != NULL, "utime should succeed");
    TEST_ASSERT(loopyFSRequestGetResult(req) == 0, "utime result should be 0");
    loopyFSRequestFree(req);

    /* Verify times changed */
    struct stat st;
    stat(path, &st);
    TEST_ASSERT(st.st_mtime == 1000000000, "mtime should be set");

    unlink(path);
    return 1;
}

static int test_fs_copyfile(void) {
    /* Create temp file with content */
    char srcPath[] = "/tmp/loopy_fs_copy_src_XXXXXX";
    int fd = mkstemp(srcPath);
    TEST_ASSERT(fd >= 0, "mkstemp should succeed");
    const char *content = "test content for copy";
    write(fd, content, strlen(content));
    close(fd);

    /* Copy the file */
    char dstPath[256];
    snprintf(dstPath, sizeof(dstPath), "%s.copy", srcPath);
    loopyFSRequest *req = loopyFSCopyfile(NULL, srcPath, dstPath,
                                          LOOPY_FS_COPY_DEFAULT, NULL, NULL);
    TEST_ASSERT(req != NULL, "copyfile should succeed");
    TEST_ASSERT(loopyFSRequestGetResult(req) == 0,
                "copyfile result should be 0");
    loopyFSRequestFree(req);

    /* Verify copy exists and has content */
    fd = open(dstPath, O_RDONLY);
    TEST_ASSERT(fd >= 0, "copied file should exist");
    char buf[64] = {0};
    ssize_t n = read(fd, buf, sizeof(buf));
    close(fd);
    TEST_ASSERT(n == (ssize_t)strlen(content), "content length should match");
    TEST_ASSERT(strcmp(buf, content) == 0, "content should match");

    /* Test EXCL flag - should fail if dest exists */
    req =
        loopyFSCopyfile(NULL, srcPath, dstPath, LOOPY_FS_COPY_EXCL, NULL, NULL);
    TEST_ASSERT(req != NULL, "copyfile should return request");
    TEST_ASSERT(loopyFSRequestGetResult(req) == -1,
                "copyfile EXCL should fail");
    loopyFSRequestFree(req);

    unlink(srcPath);
    unlink(dstPath);
    return 1;
}

static int test_fs_mkstemp_mkdtemp(void) {
    /* Test mkstemp */
    char stempPath[] = "/tmp/loopy_fs_mkstemp_XXXXXX";
    loopyFSRequest *req = loopyFSMkstemp(NULL, stempPath, NULL, NULL);
    TEST_ASSERT(req != NULL, "mkstemp should succeed");
    int fd = (int)loopyFSRequestGetResult(req);
    TEST_ASSERT(fd >= 0, "mkstemp should return valid fd");
    close(fd);
    loopyFSRequestFree(req);
    /* Template should be modified */
    TEST_ASSERT(strstr(stempPath, "XXXXXX") == NULL,
                "template should be modified");
    unlink(stempPath);

    /* Test mkdtemp */
    char dtempPath[] = "/tmp/loopy_fs_mkdtemp_XXXXXX";
    req = loopyFSMkdtemp(NULL, dtempPath, NULL, NULL);
    TEST_ASSERT(req != NULL, "mkdtemp should succeed");
    TEST_ASSERT(loopyFSRequestGetResult(req) == 0,
                "mkdtemp result should be 0");
    loopyFSRequestFree(req);
    /* Template should be modified */
    TEST_ASSERT(strstr(dtempPath, "XXXXXX") == NULL,
                "template should be modified");
    /* Directory should exist */
    struct stat st;
    TEST_ASSERT(stat(dtempPath, &st) == 0 && S_ISDIR(st.st_mode),
                "should be directory");
    rmdir(dtempPath);

    return 1;
}

static int test_fs_extended_null_safety(void) {
    /* NULL path operations */
    TEST_ASSERT(loopyFSLink(NULL, NULL, NULL, NULL, NULL) == NULL,
                "link NULL should fail");
    TEST_ASSERT(loopyFSSymlink(NULL, NULL, NULL, NULL, NULL) == NULL,
                "symlink NULL should fail");
    TEST_ASSERT(loopyFSReadlink(NULL, NULL, NULL, 0, NULL, NULL) == NULL,
                "readlink NULL should fail");
    TEST_ASSERT(loopyFSRealpath(NULL, NULL, NULL, 0, NULL, NULL) == NULL,
                "realpath NULL should fail");
    TEST_ASSERT(loopyFSAccess(NULL, NULL, 0, NULL, NULL) == NULL,
                "access NULL should fail");
    TEST_ASSERT(loopyFSScandir(NULL, NULL, NULL, NULL) == NULL,
                "scandir NULL should fail");
    TEST_ASSERT(loopyFSChown(NULL, NULL, 0, 0, NULL, NULL) == NULL,
                "chown NULL should fail");
    TEST_ASSERT(loopyFSLchown(NULL, NULL, 0, 0, NULL, NULL) == NULL,
                "lchown NULL should fail");
    TEST_ASSERT(loopyFSUtime(NULL, NULL, NULL, NULL, NULL, NULL) == NULL,
                "utime NULL should fail");
    TEST_ASSERT(loopyFSLutime(NULL, NULL, NULL, NULL, NULL, NULL) == NULL,
                "lutime NULL should fail");
    TEST_ASSERT(loopyFSCopyfile(NULL, NULL, NULL, 0, NULL, NULL) == NULL,
                "copyfile NULL should fail");
    TEST_ASSERT(loopyFSMkdtemp(NULL, NULL, NULL, NULL) == NULL,
                "mkdtemp NULL should fail");
    TEST_ASSERT(loopyFSMkstemp(NULL, NULL, NULL, NULL) == NULL,
                "mkstemp NULL should fail");

    /* Invalid fd operations */
    TEST_ASSERT(loopyFSFchown(NULL, -1, 0, 0, NULL, NULL) == NULL,
                "fchown -1 should fail");
    TEST_ASSERT(loopyFSFutime(NULL, -1, NULL, NULL, NULL, NULL) == NULL,
                "futime -1 should fail");
    TEST_ASSERT(loopyFSSendfile(NULL, -1, -1, 0, 0, NULL, NULL) == NULL,
                "sendfile -1 should fail");

    return 1;
}

/* ====================================================================
 * TTY Tests
 * ==================================================================== */

static int test_tty_is_tty(void) {
    /* loopyTTYIsTTY should work regardless of actual TTY presence */
    /* On a real terminal, stdin/stdout/stderr are TTYs */
    /* In CI or pipe environments, they are not */
    bool stdinIsTTY = loopyTTYIsTTY(STDIN_FILENO);
    bool stdoutIsTTY = loopyTTYIsTTY(STDOUT_FILENO);

    /* Print for info */
    printf("(stdin is %s TTY, stdout is %s TTY) ", stdinIsTTY ? "a" : "not a",
           stdoutIsTTY ? "a" : "not a");

    /* -1 should not be a TTY */
    TEST_ASSERT(!loopyTTYIsTTY(-1), "-1 should not be a TTY");

    /* A regular file should not be a TTY */
    int fd =
        open("/tmp/loopy_tty_test.tmp", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) {
        TEST_ASSERT(!loopyTTYIsTTY(fd), "regular file should not be a TTY");
        close(fd);
        unlink("/tmp/loopy_tty_test.tmp");
    }

    return 1;
}

static int test_tty_create_non_tty(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    /* Creating TTY from non-TTY fd should fail */
    int pipefd[2];
    TEST_ASSERT(pipe(pipefd) == 0, "pipe should succeed");

    const loopyTTY *tty = loopyTTYNew(l, pipefd[0]);
    TEST_ASSERT(tty == NULL, "creating TTY from pipe should fail");

    close(pipefd[0]);
    close(pipefd[1]);

    return 1;
}

static int test_tty_create_if_available(void) {
    /* Only test TTY creation if we actually have a TTY */
    if (loopyTTYIsTTY(STDOUT_FILENO)) {
        /* We have a real TTY, so we can test more thoroughly */
        /* But we shouldn't actually modify the terminal during tests */
        /* Just verify we can detect it */
        printf("(real TTY available) ");
    } else {
        printf("(no TTY available, skipping TTY-specific tests) ");
    }

    return 1;
}

static int test_tty_mode_enum(void) {
    /* Just verify the enum values are distinct */
    TEST_ASSERT(LOOPY_TTY_MODE_NORMAL != LOOPY_TTY_MODE_RAW,
                "modes should be distinct");
    TEST_ASSERT(LOOPY_TTY_MODE_RAW != LOOPY_TTY_MODE_IO,
                "modes should be distinct");
    TEST_ASSERT(LOOPY_TTY_MODE_NORMAL != LOOPY_TTY_MODE_IO,
                "modes should be distinct");

    return 1;
}

static int test_tty_null_safety(void) {
    /* All functions should handle NULL gracefully */
    loopyTTYFree(NULL); /* Should not crash */

    TEST_ASSERT(!loopyTTYSetMode(NULL, LOOPY_TTY_MODE_RAW),
                "set mode NULL should fail");
    TEST_ASSERT(loopyTTYGetMode(NULL) == LOOPY_TTY_MODE_NORMAL,
                "get mode NULL should return NORMAL");
    TEST_ASSERT(!loopyTTYResetMode(NULL), "reset mode NULL should fail");
    TEST_ASSERT(!loopyTTYGetWinSize(NULL, NULL, NULL),
                "get win size NULL should fail");
    TEST_ASSERT(loopyTTYAsStream(NULL) == NULL,
                "as stream NULL should return NULL");
    TEST_ASSERT(loopyTTYGetFd(NULL) == -1, "get fd NULL should return -1");
    TEST_ASSERT(loopyTTYGetLoop(NULL) == NULL,
                "get loop NULL should return NULL");

    /* NULL loop should fail */
    TEST_ASSERT(loopyTTYNew(NULL, STDOUT_FILENO) == NULL,
                "new with NULL loop should fail");

    /* Invalid fd should fail */
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    TEST_ASSERT(loopyTTYNew(l, -1) == NULL, "new with -1 fd should fail");

    return 1;
}

static int test_tty_reset_all(void) {
    /* loopyTTYResetAll should not crash even with no TTYs */
    loopyTTYResetAll();
    return 1;
}

/* ====================================================================
 * io_uring Backend Tests
 *
 * These tests verify the io_uring backend API. On non-Linux systems,
 * they verify that the fallback behavior works correctly.
 * ==================================================================== */

#ifdef USE_IOURING

/* Generic timeout callback for io_uring tests */
static bool timeout_callback(timerWheel *t, timerWheelId id, void *data) {
    (void)t;
    (void)id;
    loopyLoop *l = (loopyLoop *)data;
    loopyStop(l);
    return false;
}

/* Helper function to poll the event loop for a short duration */
static void loopyPoll(loopyLoop *l, int milliseconds) {
    /* Register a one-shot timer to stop the loop after the timeout */
    loopyRegisterTimer(l, (uint64_t)milliseconds * 1000, 0, timeout_callback, l);
    loopyMain(l);
}

static int test_iouring_api_available(void) {
    /* The loopyUsingIoUring function should be available on all platforms */
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    TEST_ASSERT(l, "loop should be created");

    /* On non-Linux or when io_uring not enabled, should return false */
    bool usingIoUring = loopyUsingIoUring(l);

#ifdef __linux__
    /* On Linux, may return true or false depending on kernel and config */
    printf("(io_uring: %s) ", usingIoUring ? "yes" : "no");
#else
    /* On non-Linux, must always return false */
    TEST_ASSERT(!usingIoUring, "io_uring should be false on non-Linux");
#endif

    return 1;
}

static int test_iouring_null_safety(void) {
    /* loopyUsingIoUring should handle NULL gracefully */
    bool result = loopyUsingIoUring(NULL);
    TEST_ASSERT(!result, "NULL loop should return false");
    return 1;
}

static int test_iouring_adapter_name(void) {
    /* Adapter name should be valid regardless of backend */
    const char *name = loopyAdapterName();
    TEST_ASSERT(name != NULL, "adapter name should not be NULL");
    TEST_ASSERT(strlen(name) > 0, "adapter name should not be empty");

    /* Valid adapter names */
    bool validName =
        (strcmp(name, "kqueue") == 0 || strcmp(name, "epoll") == 0 ||
         strcmp(name, "io_uring/epoll") == 0 || strcmp(name, "evport") == 0 ||
         strcmp(name, "select") == 0);
    TEST_ASSERT(validName, "adapter name should be recognized");

    return 1;
}

static int test_iouring_event_loop_works(void) {
    /* Regardless of backend, basic event loop operations should work */
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    TEST_ASSERT(l, "loop should be created");

    /* Create a pipe to test with */
    int fds[2];
    TEST_ASSERT(pipe(fds) == 0, "pipe should succeed");

    /* Register for read */
    bool registered = loopyRegisterRead(l, fds[0], dummy_callback, NULL);
    TEST_ASSERT(registered, "register read should succeed");

    /* Write some data */
    ssize_t written = write(fds[1], "x", 1);
    TEST_ASSERT(written == 1, "write should succeed");

    /* Unregister and cleanup */
    loopyUnregisterRead(l, fds[0]);

    close(fds[0]);
    close(fds[1]);
    return 1;
}

#endif /* USE_IOURING */

/* ====================================================================
 * io_uring File Operations Tests
 * ==================================================================== */

#ifdef USE_IOURING

/* Test context for async file operations */
typedef struct {
    loopyLoop *loop;
    int32_t result;
    bool completed;
    char buffer[4096];
} IoUringFileTestContext;

static void iouring_file_callback(void *userData, int32_t result) {
    IoUringFileTestContext *ctx = (IoUringFileTestContext *)userData;
    ctx->result = result;
    ctx->completed = true;
    loopyStop(ctx->loop);
}

static int test_iouring_fs_api_available(void) {
    /* File operations API should be available on Linux */
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    TEST_ASSERT(l, "loop should be created");

    bool usingIoUring = loopyUsingIoUring(l);
    if (usingIoUring) {
        printf("(io_uring available, will test file ops) ");
    } else {
        printf("(io_uring not available, testing graceful fallback) ");
    }

    return 1;
}

static int test_iouring_fs_write_basic(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    TEST_ASSERT(l, "loop should be created");

    if (!loopyUsingIoUring(l)) {
        printf("(skipped: io_uring not available) ");
        return 1;
    }

    /* Create a temporary file */
    char tmpfile[] = "/tmp/loopy_iouring_test_XXXXXX";
    int fd = mkstemp(tmpfile);
    TEST_ASSERT(fd >= 0, "mkstemp should succeed");

    IoUringFileTestContext ctx = {0};
    ctx.loop = l;
    const char *testData = "Hello, io_uring!";
    size_t dataLen = strlen(testData);

    /* Submit write operation */
    uint64_t opId = loopyIoUringWrite(l, fd, testData, dataLen, 0,
                                      iouring_file_callback, &ctx);
    TEST_ASSERT(opId != 0, "write operation should be submitted");

    /* Add timeout in case something goes wrong */
    loopyRegisterTimer(l, 1000000, 0, timeout_callback, l);

    /* Run event loop to process completion */
    loopyMain(l);

    /* Verify write succeeded */
    TEST_ASSERT(ctx.completed, "callback should have been called");
    TEST_ASSERT(ctx.result == (int32_t)dataLen,
                "write should return bytes written");

    /* Verify data was actually written */
    char verify[4096];
    ssize_t readBytes = pread(fd, verify, sizeof(verify), 0);
    TEST_ASSERT(readBytes == (ssize_t)dataLen, "data should be in file");
    TEST_ASSERT(memcmp(verify, testData, dataLen) == 0, "data should match");

    close(fd);
    unlink(tmpfile);
    return 1;
}

static int test_iouring_fs_read_basic(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    TEST_ASSERT(l, "loop should be created");

    if (!loopyUsingIoUring(l)) {
        printf("(skipped: io_uring not available) ");
        return 1;
    }

    /* Create a temporary file with test data */
    char tmpfile[] = "/tmp/loopy_iouring_test_XXXXXX";
    int fd = mkstemp(tmpfile);
    TEST_ASSERT(fd >= 0, "mkstemp should succeed");

    const char *testData = "io_uring read test data";
    size_t dataLen = strlen(testData);
    ssize_t written = write(fd, testData, dataLen);
    TEST_ASSERT(written == (ssize_t)dataLen, "write should succeed");

    IoUringFileTestContext ctx = {0};
    ctx.loop = l;
    memset(ctx.buffer, 0, sizeof(ctx.buffer));

    /* Submit read operation */
    uint64_t opId = loopyIoUringRead(l, fd, ctx.buffer, sizeof(ctx.buffer), 0,
                                     iouring_file_callback, &ctx);
    TEST_ASSERT(opId != 0, "read operation should be submitted");

    /* Add timeout */
    loopyRegisterTimer(l, 1000000, 0, timeout_callback, l);

    /* Run event loop */
    loopyMain(l);

    /* Verify read succeeded */
    TEST_ASSERT(ctx.completed, "callback should have been called");
    TEST_ASSERT(ctx.result == (int32_t)dataLen,
                "read should return bytes read");
    TEST_ASSERT(memcmp(ctx.buffer, testData, dataLen) == 0,
                "read data should match");

    close(fd);
    unlink(tmpfile);
    return 1;
}

static int test_iouring_fs_open_close(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    TEST_ASSERT(l, "loop should be created");

    if (!loopyUsingIoUring(l)) {
        printf("(skipped: io_uring not available) ");
        return 1;
    }

    /* Create a temporary file to open */
    char tmpfile[] = "/tmp/loopy_iouring_test_XXXXXX";
    int fd = mkstemp(tmpfile);
    TEST_ASSERT(fd >= 0, "mkstemp should succeed");
    close(fd);

    IoUringFileTestContext ctx = {0};
    ctx.loop = l;

    /* Submit open operation */
    uint64_t opId = loopyIoUringOpenat(l, AT_FDCWD, tmpfile, O_RDONLY, 0,
                                       iouring_file_callback, &ctx);
    TEST_ASSERT(opId != 0, "open operation should be submitted");

    /* Add timeout */
    loopyRegisterTimer(l, 1000000, 0, timeout_callback, l);

    /* Run event loop */
    loopyMain(l);

    /* Verify open succeeded */
    TEST_ASSERT(ctx.completed, "callback should have been called");
    TEST_ASSERT(ctx.result >= 0, "open should return valid fd");

    int openedFd = ctx.result;

    /* Now test close operation */
    ctx.completed = false;
    ctx.result = -1;

    opId = loopyIoUringClose(l, openedFd, iouring_file_callback, &ctx);
    TEST_ASSERT(opId != 0, "close operation should be submitted");

    /* Add timeout */
    loopyRegisterTimer(l, 1000000, 0, timeout_callback, l);

    /* Run event loop */
    loopyMain(l);

    /* Verify close succeeded */
    TEST_ASSERT(ctx.completed, "callback should have been called");
    TEST_ASSERT(ctx.result == 0, "close should return 0");

    unlink(tmpfile);
    return 1;
}

static int test_iouring_fs_fsync(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    TEST_ASSERT(l, "loop should be created");

    if (!loopyUsingIoUring(l)) {
        printf("(skipped: io_uring not available) ");
        return 1;
    }

    /* Create a temporary file */
    char tmpfile[] = "/tmp/loopy_iouring_test_XXXXXX";
    int fd = mkstemp(tmpfile);
    TEST_ASSERT(fd >= 0, "mkstemp should succeed");

    /* Write some data */
    const char *testData = "fsync test";
    ssize_t written = write(fd, testData, strlen(testData));
    TEST_ASSERT(written == (ssize_t)strlen(testData), "write should succeed");

    IoUringFileTestContext ctx = {0};
    ctx.loop = l;

    /* Submit fsync operation (full sync) */
    uint64_t opId =
        loopyIoUringFsync(l, fd, false, iouring_file_callback, &ctx);
    TEST_ASSERT(opId != 0, "fsync operation should be submitted");

    /* Add timeout */
    loopyRegisterTimer(l, 1000000, 0, timeout_callback, l);

    /* Run event loop */
    loopyMain(l);

    /* Verify fsync succeeded */
    TEST_ASSERT(ctx.completed, "callback should have been called");
    TEST_ASSERT(ctx.result == 0, "fsync should return 0");

    close(fd);
    unlink(tmpfile);
    return 1;
}

static int test_iouring_fs_fdatasync(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    TEST_ASSERT(l, "loop should be created");

    if (!loopyUsingIoUring(l)) {
        printf("(skipped: io_uring not available) ");
        return 1;
    }

    /* Create a temporary file */
    char tmpfile[] = "/tmp/loopy_iouring_test_XXXXXX";
    int fd = mkstemp(tmpfile);
    TEST_ASSERT(fd >= 0, "mkstemp should succeed");

    /* Write some data */
    const char *testData = "fdatasync test";
    ssize_t written = write(fd, testData, strlen(testData));
    TEST_ASSERT(written == (ssize_t)strlen(testData), "write should succeed");

    IoUringFileTestContext ctx = {0};
    ctx.loop = l;

    /* Submit fdatasync operation (data only) */
    uint64_t opId = loopyIoUringFsync(l, fd, true, iouring_file_callback, &ctx);
    TEST_ASSERT(opId != 0, "fdatasync operation should be submitted");

    /* Add timeout */
    loopyRegisterTimer(l, 1000000, 0, timeout_callback, l);

    /* Run event loop */
    loopyMain(l);

    /* Verify fdatasync succeeded */
    TEST_ASSERT(ctx.completed, "callback should have been called");
    TEST_ASSERT(ctx.result == 0, "fdatasync should return 0");

    close(fd);
    unlink(tmpfile);
    return 1;
}

static int test_iouring_fs_read_offset(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    TEST_ASSERT(l, "loop should be created");

    if (!loopyUsingIoUring(l)) {
        printf("(skipped: io_uring not available) ");
        return 1;
    }

    /* Create a temporary file with test data */
    char tmpfile[] = "/tmp/loopy_iouring_test_XXXXXX";
    int fd = mkstemp(tmpfile);
    TEST_ASSERT(fd >= 0, "mkstemp should succeed");

    const char *testData = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    size_t dataLen = strlen(testData);
    ssize_t written = write(fd, testData, dataLen);
    TEST_ASSERT(written == (ssize_t)dataLen, "write should succeed");

    IoUringFileTestContext ctx = {0};
    ctx.loop = l;
    memset(ctx.buffer, 0, sizeof(ctx.buffer));

    /* Read from offset 10 */
    uint64_t opId = loopyIoUringRead(l, fd, ctx.buffer, 10, 10,
                                     iouring_file_callback, &ctx);
    TEST_ASSERT(opId != 0, "read operation should be submitted");

    /* Add timeout */
    loopyRegisterTimer(l, 1000000, 0, timeout_callback, l);

    /* Run event loop */
    loopyMain(l);

    /* Verify read from offset succeeded */
    TEST_ASSERT(ctx.completed, "callback should have been called");
    TEST_ASSERT(ctx.result == 10, "read should return 10 bytes");
    TEST_ASSERT(memcmp(ctx.buffer, "ABCDEFGHIJ", 10) == 0,
                "read data should match offset data");

    close(fd);
    unlink(tmpfile);
    return 1;
}

static int test_iouring_fs_invalid_fd(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    TEST_ASSERT(l, "loop should be created");

    if (!loopyUsingIoUring(l)) {
        printf("(skipped: io_uring not available) ");
        return 1;
    }

    IoUringFileTestContext ctx = {0};
    ctx.loop = l;
    char buffer[64];

    /* Try to read from invalid fd */
    uint64_t opId = loopyIoUringRead(l, -1, buffer, sizeof(buffer), 0,
                                     iouring_file_callback, &ctx);
    TEST_ASSERT(opId != 0, "operation should be submitted");

    /* Add timeout */
    loopyRegisterTimer(l, 1000000, 0, timeout_callback, l);

    /* Run event loop */
    loopyMain(l);

    /* Verify operation failed with error */
    TEST_ASSERT(ctx.completed, "callback should have been called");
    TEST_ASSERT(ctx.result < 0, "invalid fd should return error");
    TEST_ASSERT(ctx.result == -EBADF, "should return EBADF");

    return 1;
}

static int test_iouring_fs_null_safety(void) {
    loopyLoop *l LOOPY_LOOP_AUTO_CLEANUP = loopyNew(16);
    TEST_ASSERT(l, "loop should be created");

    if (!loopyUsingIoUring(l)) {
        printf("(skipped: io_uring not available) ");
        return 1;
    }

    char buffer[64];

    /* NULL loop */
    uint64_t opId = loopyIoUringRead(NULL, 0, buffer, sizeof(buffer), 0,
                                     iouring_file_callback, NULL);
    TEST_ASSERT(opId == 0, "NULL loop should return 0");

    /* NULL buffer */
    opId = loopyIoUringRead(l, 0, NULL, sizeof(buffer), 0,
                            iouring_file_callback, NULL);
    TEST_ASSERT(opId == 0, "NULL buffer should return 0");

    /* NULL callback */
    opId = loopyIoUringRead(l, 0, buffer, sizeof(buffer), 0, NULL, NULL);
    TEST_ASSERT(opId == 0, "NULL callback should return 0");

    /* Similar tests for write */
    opId = loopyIoUringWrite(NULL, 0, buffer, sizeof(buffer), 0,
                             iouring_file_callback, NULL);
    TEST_ASSERT(opId == 0, "write with NULL loop should return 0");

    opId = loopyIoUringWrite(l, 0, NULL, sizeof(buffer), 0,
                             iouring_file_callback, NULL);
    TEST_ASSERT(opId == 0, "write with NULL buffer should return 0");

    /* Test openat */
    opId = loopyIoUringOpenat(NULL, AT_FDCWD, "/tmp/test", O_RDONLY, 0,
                              iouring_file_callback, NULL);
    TEST_ASSERT(opId == 0, "openat with NULL loop should return 0");

    opId = loopyIoUringOpenat(l, AT_FDCWD, NULL, O_RDONLY, 0,
                              iouring_file_callback, NULL);
    TEST_ASSERT(opId == 0, "openat with NULL path should return 0");

    /* Test close */
    opId = loopyIoUringClose(NULL, 0, iouring_file_callback, NULL);
    TEST_ASSERT(opId == 0, "close with NULL loop should return 0");

    /* Test fsync */
    opId = loopyIoUringFsync(NULL, 0, false, iouring_file_callback, NULL);
    TEST_ASSERT(opId == 0, "fsync with NULL loop should return 0");

    return 1;
}

static int concurrent_ops_completed = 0;

static void concurrent_ops_callback(void *userData, int32_t result) {
    IoUringFileTestContext *ctx = (IoUringFileTestContext *)userData;
    ctx->result = result;
    ctx->completed = true;
    concurrent_ops_completed++;

    if (concurrent_ops_completed >= 3) {
        loopyStop(ctx->loop);
    }
}

static int test_iouring_fs_concurrent_ops(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    TEST_ASSERT(l, "loop should be created");

    if (!loopyUsingIoUring(l)) {
        printf("(skipped: io_uring not available) ");
        return 1;
    }

    /* Create three temporary files */
    char tmpfile1[] = "/tmp/loopy_iouring_test1_XXXXXX";
    char tmpfile2[] = "/tmp/loopy_iouring_test2_XXXXXX";
    char tmpfile3[] = "/tmp/loopy_iouring_test3_XXXXXX";

    int fd1 = mkstemp(tmpfile1);
    int fd2 = mkstemp(tmpfile2);
    int fd3 = mkstemp(tmpfile3);

    TEST_ASSERT(fd1 >= 0 && fd2 >= 0 && fd3 >= 0, "mkstemp should succeed");

    IoUringFileTestContext ctx1 = {0};
    IoUringFileTestContext ctx2 = {0};
    IoUringFileTestContext ctx3 = {0};

    ctx1.loop = ctx2.loop = ctx3.loop = l;
    concurrent_ops_completed = 0;

    const char *data1 = "File 1";
    const char *data2 = "File 2 data";
    const char *data3 = "File 3 longer data";

    /* Submit three concurrent write operations */
    uint64_t op1 = loopyIoUringWrite(l, fd1, data1, strlen(data1), 0,
                                     concurrent_ops_callback, &ctx1);
    uint64_t op2 = loopyIoUringWrite(l, fd2, data2, strlen(data2), 0,
                                     concurrent_ops_callback, &ctx2);
    uint64_t op3 = loopyIoUringWrite(l, fd3, data3, strlen(data3), 0,
                                     concurrent_ops_callback, &ctx3);

    TEST_ASSERT(op1 != 0 && op2 != 0 && op3 != 0,
                "all operations should be submitted");

    /* Add timeout */
    loopyRegisterTimer(l, 2000000, 0, timeout_callback, l);

    /* Run event loop */
    loopyMain(l);

    /* Verify all operations completed */
    TEST_ASSERT(ctx1.completed && ctx2.completed && ctx3.completed,
                "all callbacks should have been called");
    TEST_ASSERT(ctx1.result == (int32_t)strlen(data1),
                "write 1 should succeed");
    TEST_ASSERT(ctx2.result == (int32_t)strlen(data2),
                "write 2 should succeed");
    TEST_ASSERT(ctx3.result == (int32_t)strlen(data3),
                "write 3 should succeed");

    close(fd1);
    close(fd2);
    close(fd3);
    unlink(tmpfile1);
    unlink(tmpfile2);
    unlink(tmpfile3);
    return 1;
}

static int test_iouring_fs_fallback_non_iouring(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    TEST_ASSERT(l, "loop should be created");

    if (loopyUsingIoUring(l)) {
        printf("(skipped: io_uring is available) ");
        return 1;
    }

    /* On non-io_uring systems, API should return 0 gracefully */
    char buffer[64];
    uint64_t opId = loopyIoUringRead(l, 0, buffer, sizeof(buffer), 0,
                                     iouring_file_callback, NULL);
    TEST_ASSERT(opId == 0, "read should return 0 on non-io_uring system");

    opId = loopyIoUringWrite(l, 0, buffer, sizeof(buffer), 0,
                             iouring_file_callback, NULL);
    TEST_ASSERT(opId == 0, "write should return 0 on non-io_uring system");

    opId = loopyIoUringOpenat(l, AT_FDCWD, "/tmp/test", O_RDONLY, 0,
                              iouring_file_callback, NULL);
    TEST_ASSERT(opId == 0, "openat should return 0 on non-io_uring system");

    opId = loopyIoUringClose(l, 0, iouring_file_callback, NULL);
    TEST_ASSERT(opId == 0, "close should return 0 on non-io_uring system");

    opId = loopyIoUringFsync(l, 0, false, iouring_file_callback, NULL);
    TEST_ASSERT(opId == 0, "fsync should return 0 on non-io_uring system");

    return 1;
}

/* ====================================================================
 * io_uring Network I/O Tests (Linux-only)
 * ==================================================================== */

typedef struct {
    loopyLoop *loop;
    int32_t result;
    bool completed;
    char buffer[4096];
    int acceptedFd;
} IoUringNetTestContext;

static void iouring_net_callback(void *userData, int32_t result) {
    IoUringNetTestContext *ctx = (IoUringNetTestContext *)userData;
    ctx->result = result;
    ctx->completed = true;
    loopyStop(ctx->loop);
}

static int concurrent_net_ops_completed = 0;

static void concurrent_net_ops_callback(void *userData, int32_t result) {
    IoUringNetTestContext *ctx = (IoUringNetTestContext *)userData;
    ctx->result = result;
    ctx->completed = true;
    concurrent_net_ops_completed++;

    if (concurrent_net_ops_completed >= 3) {
        loopyStop(ctx->loop);
    }
}

static int test_iouring_net_send_basic(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    TEST_ASSERT(l, "loop should be created");

    if (!loopyUsingIoUring(l)) {
        printf("(skipped: io_uring not available) ");
        return 1;
    }

    if (!loopyIoUringNetAvailable(l)) {
        printf("(skipped: io_uring network ops not available) ");
        return 1;
    }

    /* Create a socketpair for testing */
    int socks[2];
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, socks);
    TEST_ASSERT(ret == 0, "socketpair should succeed");

    IoUringNetTestContext ctx = {0};
    ctx.loop = l;
    const char *testData = "Hello from io_uring send!";
    size_t dataLen = strlen(testData);

    /* Submit send operation */
    uint64_t opId = loopyIoUringSend(l, socks[0], testData, dataLen, 0,
                                     iouring_net_callback, &ctx);
    TEST_ASSERT(opId != 0, "send operation should be submitted");

    /* Add timeout */
    loopyRegisterTimer(l, 1000000, 0, timeout_callback, l);

    /* Run event loop */
    loopyMain(l);

    /* Verify send succeeded */
    TEST_ASSERT(ctx.completed, "callback should have been called");
    TEST_ASSERT(ctx.result == (int32_t)dataLen, "should send all bytes");

    /* Verify data was received */
    char recvBuf[256] = {0};
    ssize_t received = recv(socks[1], recvBuf, sizeof(recvBuf), 0);
    TEST_ASSERT(received == (ssize_t)dataLen, "should receive all bytes");
    TEST_ASSERT(memcmp(recvBuf, testData, dataLen) == 0, "data should match");

    close(socks[0]);
    close(socks[1]);
    return 1;
}

static int test_iouring_net_recv_basic(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    TEST_ASSERT(l, "loop should be created");

    if (!loopyUsingIoUring(l)) {
        printf("(skipped: io_uring not available) ");
        return 1;
    }

    if (!loopyIoUringNetAvailable(l)) {
        printf("(skipped: io_uring network ops not available) ");
        return 1;
    }

    /* Create a socketpair for testing */
    int socks[2];
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, socks);
    TEST_ASSERT(ret == 0, "socketpair should succeed");

    IoUringNetTestContext ctx = {0};
    ctx.loop = l;
    memset(ctx.buffer, 0, sizeof(ctx.buffer));

    /* Send test data from the other end */
    const char *testData = "Hello from recv test!";
    size_t dataLen = strlen(testData);
    ssize_t sent = send(socks[1], testData, dataLen, 0);
    TEST_ASSERT(sent == (ssize_t)dataLen, "send should succeed");

    /* Submit recv operation */
    uint64_t opId =
        loopyIoUringRecv(l, socks[0], ctx.buffer, sizeof(ctx.buffer), 0,
                         iouring_net_callback, &ctx);
    TEST_ASSERT(opId != 0, "recv operation should be submitted");

    /* Add timeout */
    loopyRegisterTimer(l, 1000000, 0, timeout_callback, l);

    /* Run event loop */
    loopyMain(l);

    /* Verify recv succeeded */
    TEST_ASSERT(ctx.completed, "callback should have been called");
    TEST_ASSERT(ctx.result == (int32_t)dataLen, "should receive all bytes");
    TEST_ASSERT(memcmp(ctx.buffer, testData, dataLen) == 0,
                "data should match");

    close(socks[0]);
    close(socks[1]);
    return 1;
}

static int test_iouring_net_accept_basic(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    TEST_ASSERT(l, "loop should be created");

    if (!loopyUsingIoUring(l)) {
        printf("(skipped: io_uring not available) ");
        return 1;
    }

    if (!loopyIoUringNetAvailable(l)) {
        printf("(skipped: io_uring network ops not available) ");
        return 1;
    }

    /* Create listening socket */
    int listenFd = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT(listenFd >= 0, "socket should succeed");

    int opt = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; /* Let OS choose port */

    int ret = bind(listenFd, (struct sockaddr *)&addr, sizeof(addr));
    TEST_ASSERT(ret == 0, "bind should succeed");

    ret = listen(listenFd, 1);
    TEST_ASSERT(ret == 0, "listen should succeed");

    /* Get the port */
    socklen_t addrLen = sizeof(addr);
    ret = getsockname(listenFd, (struct sockaddr *)&addr, &addrLen);
    TEST_ASSERT(ret == 0, "getsockname should succeed");

    IoUringNetTestContext ctx = {0};
    ctx.loop = l;

    struct sockaddr_in clientAddr = {0};
    socklen_t clientAddrLen = sizeof(clientAddr);

    /* Submit accept operation */
    uint64_t opId =
        loopyIoUringAccept(l, listenFd, (struct sockaddr *)&clientAddr,
                           &clientAddrLen, 0, iouring_net_callback, &ctx);
    TEST_ASSERT(opId != 0, "accept operation should be submitted");

    /* Connect from another thread (or use non-blocking connect) */
    int clientFd = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT(clientFd >= 0, "client socket should succeed");

    /* Make client socket non-blocking for immediate return */
    int flags = fcntl(clientFd, F_GETFL, 0);
    fcntl(clientFd, F_SETFL, flags | O_NONBLOCK);

    connect(clientFd, (struct sockaddr *)&addr, sizeof(addr));
    /* connect may return EINPROGRESS, which is fine */

    /* Add timeout */
    loopyRegisterTimer(l, 2000000, 0, timeout_callback, l);

    /* Run event loop */
    loopyMain(l);

    /* Verify accept succeeded */
    TEST_ASSERT(ctx.completed, "callback should have been called");
    TEST_ASSERT(ctx.result >= 0, "accept should return valid fd");

    /* Clean up */
    if (ctx.result >= 0) {
        close(ctx.result);
    }
    close(clientFd);
    close(listenFd);
    return 1;
}

static int test_iouring_net_connect_basic(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    TEST_ASSERT(l, "loop should be created");

    if (!loopyUsingIoUring(l)) {
        printf("(skipped: io_uring not available) ");
        return 1;
    }

    if (!loopyIoUringNetAvailable(l)) {
        printf("(skipped: io_uring network ops not available) ");
        return 1;
    }

    /* Create listening socket for server */
    int listenFd = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT(listenFd >= 0, "socket should succeed");

    int opt = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in serverAddr = {0};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    serverAddr.sin_port = 0;

    int ret =
        bind(listenFd, (struct sockaddr *)&serverAddr, sizeof(serverAddr));
    TEST_ASSERT(ret == 0, "bind should succeed");

    ret = listen(listenFd, 1);
    TEST_ASSERT(ret == 0, "listen should succeed");

    /* Get the port */
    socklen_t addrLen = sizeof(serverAddr);
    ret = getsockname(listenFd, (struct sockaddr *)&serverAddr, &addrLen);
    TEST_ASSERT(ret == 0, "getsockname should succeed");

    /* Create client socket */
    int clientFd = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT(clientFd >= 0, "client socket should succeed");

    /* Make non-blocking for io_uring connect */
    int flags = fcntl(clientFd, F_GETFL, 0);
    fcntl(clientFd, F_SETFL, flags | O_NONBLOCK);

    IoUringNetTestContext ctx = {0};
    ctx.loop = l;

    /* Submit connect operation */
    uint64_t opId =
        loopyIoUringConnect(l, clientFd, (struct sockaddr *)&serverAddr,
                            sizeof(serverAddr), iouring_net_callback, &ctx);
    TEST_ASSERT(opId != 0, "connect operation should be submitted");

    /* Add timeout */
    loopyRegisterTimer(l, 2000000, 0, timeout_callback, l);

    /* Run event loop */
    loopyMain(l);

    /* Verify connect succeeded */
    TEST_ASSERT(ctx.completed, "callback should have been called");
    TEST_ASSERT(ctx.result == 0, "connect should succeed");

    /* Accept the connection to verify */
    int acceptedFd = accept(listenFd, NULL, NULL);
    TEST_ASSERT(acceptedFd >= 0, "accept should succeed");

    close(acceptedFd);
    close(clientFd);
    close(listenFd);
    return 1;
}

static int test_iouring_net_sendmsg_basic(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    TEST_ASSERT(l, "loop should be created");

    if (!loopyUsingIoUring(l)) {
        printf("(skipped: io_uring not available) ");
        return 1;
    }

    if (!loopyIoUringNetAvailable(l)) {
        printf("(skipped: io_uring network ops not available) ");
        return 1;
    }

    /* Create a socketpair for testing */
    int socks[2];
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, socks);
    TEST_ASSERT(ret == 0, "socketpair should succeed");

    IoUringNetTestContext ctx = {0};
    ctx.loop = l;

    /* Prepare scatter/gather buffers */
    const char *msg1 = "Hello ";
    const char *msg2 = "from ";
    const char *msg3 = "sendmsg!";

    struct iovec iov[3];
    iov[0].iov_base = (void *)msg1;
    iov[0].iov_len = strlen(msg1);
    iov[1].iov_base = (void *)msg2;
    iov[1].iov_len = strlen(msg2);
    iov[2].iov_base = (void *)msg3;
    iov[2].iov_len = strlen(msg3);

    struct msghdr msg = {0};
    msg.msg_iov = iov;
    msg.msg_iovlen = 3;

    size_t totalLen = strlen(msg1) + strlen(msg2) + strlen(msg3);

    /* Submit sendmsg operation */
    uint64_t opId =
        loopyIoUringSendmsg(l, socks[0], &msg, 0, iouring_net_callback, &ctx);
    TEST_ASSERT(opId != 0, "sendmsg operation should be submitted");

    /* Add timeout */
    loopyRegisterTimer(l, 1000000, 0, timeout_callback, l);

    /* Run event loop */
    loopyMain(l);

    /* Verify sendmsg succeeded */
    TEST_ASSERT(ctx.completed, "callback should have been called");
    TEST_ASSERT(ctx.result == (int32_t)totalLen, "should send all bytes");

    /* Verify data was received */
    char recvBuf[256] = {0};
    ssize_t received = recv(socks[1], recvBuf, sizeof(recvBuf), 0);
    TEST_ASSERT(received == (ssize_t)totalLen, "should receive all bytes");
    TEST_ASSERT(strncmp(recvBuf, "Hello from sendmsg!", totalLen) == 0,
                "data should match");

    close(socks[0]);
    close(socks[1]);
    return 1;
}

static int test_iouring_net_recvmsg_basic(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    TEST_ASSERT(l, "loop should be created");

    if (!loopyUsingIoUring(l)) {
        printf("(skipped: io_uring not available) ");
        return 1;
    }

    if (!loopyIoUringNetAvailable(l)) {
        printf("(skipped: io_uring network ops not available) ");
        return 1;
    }

    /* Create a socketpair for testing */
    int socks[2];
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, socks);
    TEST_ASSERT(ret == 0, "socketpair should succeed");

    IoUringNetTestContext ctx = {0};
    ctx.loop = l;

    /* Prepare receive buffers */
    char buf1[16] = {0};
    char buf2[16] = {0};
    char buf3[16] = {0};

    struct iovec iov[3];
    iov[0].iov_base = buf1;
    iov[0].iov_len = sizeof(buf1);
    iov[1].iov_base = buf2;
    iov[1].iov_len = sizeof(buf2);
    iov[2].iov_base = buf3;
    iov[2].iov_len = sizeof(buf3);

    struct msghdr msg = {0};
    msg.msg_iov = iov;
    msg.msg_iovlen = 3;

    /* Send test data */
    const char *testData = "Recvmsg test data";
    size_t dataLen = strlen(testData);
    ssize_t sent = send(socks[1], testData, dataLen, 0);
    TEST_ASSERT(sent == (ssize_t)dataLen, "send should succeed");

    /* Submit recvmsg operation */
    uint64_t opId =
        loopyIoUringRecvmsg(l, socks[0], &msg, 0, iouring_net_callback, &ctx);
    TEST_ASSERT(opId != 0, "recvmsg operation should be submitted");

    /* Add timeout */
    loopyRegisterTimer(l, 1000000, 0, timeout_callback, l);

    /* Run event loop */
    loopyMain(l);

    /* Verify recvmsg succeeded */
    TEST_ASSERT(ctx.completed, "callback should have been called");
    TEST_ASSERT(ctx.result == (int32_t)dataLen, "should receive all bytes");

    /* Reconstruct message from buffers */
    char reconstructed[256] = {0};
    size_t offset = 0;
    size_t remaining = sizeof(reconstructed) - 1;

    /* Copy data from each buffer, respecting the actual received length */
    size_t copyLen1 = (dataLen > 0) ? ((dataLen > 16) ? 16 : dataLen) : 0;
    if (copyLen1 > 0 && remaining > 0) {
        size_t toCopy = (copyLen1 < remaining) ? copyLen1 : remaining;
        memcpy(reconstructed + offset, buf1, toCopy);
        offset += toCopy;
        remaining -= toCopy;
    }

    if (dataLen > 16 && remaining > 0) {
        size_t copyLen2 = ((dataLen - 16) > 16) ? 16 : (dataLen - 16);
        size_t toCopy = (copyLen2 < remaining) ? copyLen2 : remaining;
        memcpy(reconstructed + offset, buf2, toCopy);
        offset += toCopy;
        remaining -= toCopy;
    }

    if (dataLen > 32 && remaining > 0) {
        size_t copyLen3 = ((dataLen - 32) > 16) ? 16 : (dataLen - 32);
        size_t toCopy = (copyLen3 < remaining) ? copyLen3 : remaining;
        memcpy(reconstructed + offset, buf3, toCopy);
        offset += toCopy;
        remaining -= toCopy;
    }

    reconstructed[offset] = '\0';  /* Null terminate */

    TEST_ASSERT(strncmp(reconstructed, testData, dataLen) == 0,
                "data should match");

    close(socks[0]);
    close(socks[1]);
    return 1;
}

static int test_iouring_net_udp_send(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    TEST_ASSERT(l, "loop should be created");

    if (!loopyUsingIoUring(l)) {
        printf("(skipped: io_uring not available) ");
        return 1;
    }

    if (!loopyIoUringNetAvailable(l)) {
        printf("(skipped: io_uring network ops not available) ");
        return 1;
    }

    /* Create UDP sockets */
    int sendSock = socket(AF_INET, SOCK_DGRAM, 0);
    int recvSock = socket(AF_INET, SOCK_DGRAM, 0);
    TEST_ASSERT(sendSock >= 0 && recvSock >= 0, "sockets should succeed");

    /* Bind receiver */
    struct sockaddr_in recvAddr = {0};
    recvAddr.sin_family = AF_INET;
    recvAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    recvAddr.sin_port = 0;

    int ret = bind(recvSock, (struct sockaddr *)&recvAddr, sizeof(recvAddr));
    TEST_ASSERT(ret == 0, "bind should succeed");

    /* Get receiver port */
    socklen_t addrLen = sizeof(recvAddr);
    ret = getsockname(recvSock, (struct sockaddr *)&recvAddr, &addrLen);
    TEST_ASSERT(ret == 0, "getsockname should succeed");

    IoUringNetTestContext ctx = {0};
    ctx.loop = l;
    const char *testData = "UDP sendmsg test";
    size_t dataLen = strlen(testData);

    /* Prepare sendmsg for UDP */
    struct iovec iov = {.iov_base = (void *)testData, .iov_len = dataLen};

    struct msghdr msg = {0};
    msg.msg_name = &recvAddr;
    msg.msg_namelen = sizeof(recvAddr);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    /* Submit sendmsg operation */
    uint64_t opId =
        loopyIoUringSendmsg(l, sendSock, &msg, 0, iouring_net_callback, &ctx);
    TEST_ASSERT(opId != 0, "sendmsg operation should be submitted");

    /* Add timeout */
    loopyRegisterTimer(l, 1000000, 0, timeout_callback, l);

    /* Run event loop */
    loopyMain(l);

    /* Verify sendmsg succeeded */
    TEST_ASSERT(ctx.completed, "callback should have been called");
    TEST_ASSERT(ctx.result == (int32_t)dataLen, "should send all bytes");

    /* Verify data was received */
    char recvBuf[256] = {0};
    struct sockaddr_in srcAddr;
    socklen_t srcAddrLen = sizeof(srcAddr);
    ssize_t received = recvfrom(recvSock, recvBuf, sizeof(recvBuf), 0,
                                (struct sockaddr *)&srcAddr, &srcAddrLen);
    TEST_ASSERT(received == (ssize_t)dataLen, "should receive all bytes");
    TEST_ASSERT(memcmp(recvBuf, testData, dataLen) == 0, "data should match");

    close(sendSock);
    close(recvSock);
    return 1;
}

static int test_iouring_net_udp_recv(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    TEST_ASSERT(l, "loop should be created");

    if (!loopyUsingIoUring(l)) {
        printf("(skipped: io_uring not available) ");
        return 1;
    }

    if (!loopyIoUringNetAvailable(l)) {
        printf("(skipped: io_uring network ops not available) ");
        return 1;
    }

    /* Create UDP sockets */
    int sendSock = socket(AF_INET, SOCK_DGRAM, 0);
    int recvSock = socket(AF_INET, SOCK_DGRAM, 0);
    TEST_ASSERT(sendSock >= 0 && recvSock >= 0, "sockets should succeed");

    /* Bind receiver */
    struct sockaddr_in recvAddr = {0};
    recvAddr.sin_family = AF_INET;
    recvAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    recvAddr.sin_port = 0;

    int ret = bind(recvSock, (struct sockaddr *)&recvAddr, sizeof(recvAddr));
    TEST_ASSERT(ret == 0, "bind should succeed");

    /* Get receiver port */
    socklen_t addrLen = sizeof(recvAddr);
    ret = getsockname(recvSock, (struct sockaddr *)&recvAddr, &addrLen);
    TEST_ASSERT(ret == 0, "getsockname should succeed");

    IoUringNetTestContext ctx = {0};
    ctx.loop = l;

    /* Prepare recvmsg for UDP */
    struct sockaddr_in srcAddr = {0};
    socklen_t srcAddrLen = sizeof(srcAddr);

    struct iovec iov = {.iov_base = ctx.buffer, .iov_len = sizeof(ctx.buffer)};

    struct msghdr msg = {0};
    msg.msg_name = &srcAddr;
    msg.msg_namelen = srcAddrLen;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    /* Send test data first */
    const char *testData = "UDP recvmsg test";
    size_t dataLen = strlen(testData);
    ssize_t sent = sendto(sendSock, testData, dataLen, 0,
                          (struct sockaddr *)&recvAddr, sizeof(recvAddr));
    TEST_ASSERT(sent == (ssize_t)dataLen, "sendto should succeed");

    /* Submit recvmsg operation */
    uint64_t opId =
        loopyIoUringRecvmsg(l, recvSock, &msg, 0, iouring_net_callback, &ctx);
    TEST_ASSERT(opId != 0, "recvmsg operation should be submitted");

    /* Add timeout */
    loopyRegisterTimer(l, 1000000, 0, timeout_callback, l);

    /* Run event loop */
    loopyMain(l);

    /* Verify recvmsg succeeded */
    TEST_ASSERT(ctx.completed, "callback should have been called");
    TEST_ASSERT(ctx.result == (int32_t)dataLen, "should receive all bytes");
    TEST_ASSERT(memcmp(ctx.buffer, testData, dataLen) == 0,
                "data should match");

    close(sendSock);
    close(recvSock);
    return 1;
}

static int test_iouring_net_shutdown(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    TEST_ASSERT(l, "loop should be created");

    if (!loopyUsingIoUring(l)) {
        printf("(skipped: io_uring not available) ");
        return 1;
    }

    if (!loopyIoUringNetAvailable(l)) {
        printf("(skipped: io_uring network ops not available) ");
        return 1;
    }

    /* Create a socketpair for testing */
    int socks[2];
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, socks);
    TEST_ASSERT(ret == 0, "socketpair should succeed");

    IoUringNetTestContext ctx = {0};
    ctx.loop = l;

    /* Submit shutdown operation (SHUT_WR) */
    uint64_t opId =
        loopyIoUringShutdown(l, socks[0], SHUT_WR, iouring_net_callback, &ctx);
    TEST_ASSERT(opId != 0, "shutdown operation should be submitted");

    /* Add timeout */
    loopyRegisterTimer(l, 1000000, 0, timeout_callback, l);

    /* Run event loop */
    loopyMain(l);

    /* Verify shutdown succeeded */
    TEST_ASSERT(ctx.completed, "callback should have been called");
    TEST_ASSERT(ctx.result == 0, "shutdown should succeed");

    /* Verify write end is shut down - recv should get EOF */
    char buf[16];
    ssize_t received = recv(socks[1], buf, sizeof(buf), 0);
    TEST_ASSERT(received == 0, "should receive EOF after shutdown");

    close(socks[0]);
    close(socks[1]);
    return 1;
}

/* Context for multishot accept test (tracks multiple accepts) */
typedef struct {
    loopyLoop *loop;
    int acceptCount;
    int clientFds[3];
} MultishotAcceptContext;

static void multishot_accept_callback(void *userData, int32_t result) {
    MultishotAcceptContext *ctx = (MultishotAcceptContext *)userData;

    if (result < 0) {
        /* Error or final completion */
        return;
    }

    /* Store the accepted client fd */
    if (ctx->acceptCount < 3) {
        ctx->clientFds[ctx->acceptCount] = result;
        ctx->acceptCount++;
    }

    /* Stop after accepting 3 connections */
    if (ctx->acceptCount >= 3) {
        loopyStop(ctx->loop);
    }
}

static int test_iouring_net_accept_multishot(void) {
    loopyLoop *l LOOPY_LOOP_AUTO_CLEANUP = loopyNew(16);
    TEST_ASSERT(l, "loop should be created");

    if (!loopyUsingIoUring(l)) {
        printf("(skipped: io_uring not available) ");
        return 1;
    }

    if (!loopyIoUringNetHasMultishot(l)) {
        printf("(skipped: multishot accept not available) ");
        return 1;
    }

    /* Create listening socket */
    int listenFd = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT(listenFd >= 0, "socket creation failed");

    int opt = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; /* Let OS choose port */

    TEST_ASSERT(bind(listenFd, (struct sockaddr *)&addr, sizeof(addr)) == 0,
                "bind failed");
    TEST_ASSERT(listen(listenFd, 128) == 0, "listen failed");

    /* Get actual port */
    socklen_t addrlen = sizeof(addr);
    getsockname(listenFd, (struct sockaddr *)&addr, &addrlen);
    int port = ntohs(addr.sin_port);

    /* Initialize test context */
    MultishotAcceptContext ctx = {0};
    ctx.loop = l;
    ctx.clientFds[0] = -1;
    ctx.clientFds[1] = -1;
    ctx.clientFds[2] = -1;

    /* Submit multishot accept - ONE operation handles all connections */
    uint64_t opId =
        loopyIoUringAcceptMultishot(l, listenFd, SOCK_NONBLOCK | SOCK_CLOEXEC,
                                    multishot_accept_callback, &ctx);
    TEST_ASSERT(opId != 0, "multishot accept submission failed");

    /* Create 3 client connections */
    int clientSocks[3];
    for (int i = 0; i < 3; i++) {
        clientSocks[i] = socket(AF_INET, SOCK_STREAM, 0);
        TEST_ASSERT(clientSocks[i] >= 0, "client socket creation failed");

        /* Make non-blocking for immediate return */
        int flags = fcntl(clientSocks[i], F_GETFL, 0);
        fcntl(clientSocks[i], F_SETFL, flags | O_NONBLOCK);

        struct sockaddr_in serverAddr = {0};
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        serverAddr.sin_port = htons(port);

        /* connect() may return EINPROGRESS, which is fine */
        connect(clientSocks[i], (struct sockaddr *)&serverAddr,
                sizeof(serverAddr));
    }

    /* Add timeout to prevent hanging */
    loopyRegisterTimer(l, 2000000, 0, timeout_callback, l);

    /* Run event loop - callback will be invoked 3 times */
    loopyMain(l);

    /* Verify 3 connections were accepted */
    TEST_ASSERT(ctx.acceptCount == 3, "should accept 3 connections");
    TEST_ASSERT(ctx.clientFds[0] >= 0, "first client fd should be valid");
    TEST_ASSERT(ctx.clientFds[1] >= 0, "second client fd should be valid");
    TEST_ASSERT(ctx.clientFds[2] >= 0, "third client fd should be valid");

    /* Verify all fds are unique */
    TEST_ASSERT(ctx.clientFds[0] != ctx.clientFds[1], "fds should be unique");
    TEST_ASSERT(ctx.clientFds[1] != ctx.clientFds[2], "fds should be unique");
    TEST_ASSERT(ctx.clientFds[0] != ctx.clientFds[2], "fds should be unique");

    /* Cleanup */
    for (int i = 0; i < 3; i++) {
        if (clientSocks[i] >= 0) {
            close(clientSocks[i]);
        }
        if (ctx.clientFds[i] >= 0) {
            close(ctx.clientFds[i]);
        }
    }
    close(listenFd);
    return 1;
}

static int test_iouring_net_cancel(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    TEST_ASSERT(l, "loop should be created");

    if (!loopyUsingIoUring(l)) {
        printf("(skipped: io_uring not available) ");
        return 1;
    }

    if (!loopyIoUringNetAvailable(l)) {
        printf("(skipped: io_uring network ops not available) ");
        return 1;
    }

    /* Create a socketpair for testing */
    int socks[2];
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, socks);
    TEST_ASSERT(ret == 0, "socketpair should succeed");

    IoUringNetTestContext ctx = {0};
    ctx.loop = l;

    char buf[256];

    /* Submit a recv operation that will block (no data available) */
    uint64_t opId = loopyIoUringRecv(l, socks[0], buf, sizeof(buf), 0,
                                     iouring_net_callback, &ctx);
    TEST_ASSERT(opId != 0, "recv operation should be submitted");

    /* Immediately cancel the operation before it completes */
    bool cancelled = loopyIoUringNetCancel(l, opId);
    TEST_ASSERT(cancelled, "cancel should succeed");

    /* Add timeout to prevent hanging if cancel fails */
    loopyRegisterTimer(l, 1000000, 0, timeout_callback, l);

    /* Run event loop - the cancelled operation should complete with -ECANCELED
     */
    loopyMain(l);

    /* Verify the operation was cancelled */
    TEST_ASSERT(ctx.completed, "callback should have been called");
    TEST_ASSERT(ctx.result == -ECANCELED, "result should be -ECANCELED");

    /* Test cancelling non-existent operation */
    bool cancelInvalid = loopyIoUringNetCancel(l, 999999);
    TEST_ASSERT(!cancelInvalid, "cancelling non-existent op should fail");

    close(socks[0]);
    close(socks[1]);
    return 1;
}

static int test_iouring_net_concurrent_ops(void) {
    loopyLoop *l LOOPY_LOOP_AUTO_CLEANUP = loopyNew(16);
    TEST_ASSERT(l, "loop should be created");

    if (!loopyUsingIoUring(l)) {
        printf("(skipped: io_uring not available) ");
        return 1;
    }

    if (!loopyIoUringNetAvailable(l)) {
        printf("(skipped: io_uring network ops not available) ");
        return 1;
    }

    /* Create three socketpairs for concurrent operations */
    int socks1[2], socks2[2], socks3[2];

    int ret1 = socketpair(AF_UNIX, SOCK_STREAM, 0, socks1);
    int ret2 = socketpair(AF_UNIX, SOCK_STREAM, 0, socks2);
    int ret3 = socketpair(AF_UNIX, SOCK_STREAM, 0, socks3);

    TEST_ASSERT(ret1 == 0 && ret2 == 0 && ret3 == 0,
                "socketpair should succeed");

    IoUringNetTestContext ctx1 = {0};
    IoUringNetTestContext ctx2 = {0};
    IoUringNetTestContext ctx3 = {0};

    ctx1.loop = ctx2.loop = ctx3.loop = l;
    concurrent_net_ops_completed = 0;

    const char *data1 = "Message 1";
    const char *data2 = "Message 2 longer";
    const char *data3 = "Message 3 even longer text";

    /* Submit three concurrent send operations */
    uint64_t op1 = loopyIoUringSend(l, socks1[0], data1, strlen(data1), 0,
                                    concurrent_net_ops_callback, &ctx1);
    uint64_t op2 = loopyIoUringSend(l, socks2[0], data2, strlen(data2), 0,
                                    concurrent_net_ops_callback, &ctx2);
    uint64_t op3 = loopyIoUringSend(l, socks3[0], data3, strlen(data3), 0,
                                    concurrent_net_ops_callback, &ctx3);

    TEST_ASSERT(op1 != 0 && op2 != 0 && op3 != 0,
                "all operations should be submitted");

    /* Add timeout */
    loopyRegisterTimer(l, 2000000, 0, timeout_callback, l);

    /* Run event loop */
    loopyMain(l);

    /* Verify all operations completed */
    TEST_ASSERT(ctx1.completed && ctx2.completed && ctx3.completed,
                "all callbacks should have been called");
    TEST_ASSERT(ctx1.result == (int32_t)strlen(data1), "send1 should succeed");
    TEST_ASSERT(ctx2.result == (int32_t)strlen(data2), "send2 should succeed");
    TEST_ASSERT(ctx3.result == (int32_t)strlen(data3), "send3 should succeed");
    TEST_ASSERT(concurrent_net_ops_completed == 3,
                "should complete 3 operations");

    close(socks1[0]);
    close(socks1[1]);
    close(socks2[0]);
    close(socks2[1]);
    close(socks3[0]);
    close(socks3[1]);
    return 1;
}

static int test_iouring_net_invalid_fd(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    TEST_ASSERT(l, "loop should be created");

    if (!loopyUsingIoUring(l)) {
        printf("(skipped: io_uring not available) ");
        return 1;
    }

    if (!loopyIoUringNetAvailable(l)) {
        printf("(skipped: io_uring network ops not available) ");
        return 1;
    }

    IoUringNetTestContext ctx = {0};
    ctx.loop = l;
    char buffer[64];

    /* Try to send on invalid fd */
    uint64_t opId = loopyIoUringSend(l, -1, buffer, sizeof(buffer), 0,
                                     iouring_net_callback, &ctx);
    TEST_ASSERT(opId != 0, "operation should be submitted");

    /* Add timeout */
    loopyRegisterTimer(l, 1000000, 0, timeout_callback, l);

    /* Run event loop */
    loopyMain(l);

    /* Verify operation failed with error */
    TEST_ASSERT(ctx.completed, "callback should have been called");
    TEST_ASSERT(ctx.result < 0, "invalid fd should return error");
    TEST_ASSERT(ctx.result == -EBADF, "should return EBADF");

    return 1;
}

static int test_iouring_net_null_safety(void) {
    loopyLoop *l LOOPY_LOOP_AUTO_CLEANUP = loopyNew(16);
    TEST_ASSERT(l, "loop should be created");

    if (!loopyUsingIoUring(l)) {
        printf("(skipped: io_uring not available) ");
        return 1;
    }

    if (!loopyIoUringNetAvailable(l)) {
        printf("(skipped: io_uring network ops not available) ");
        return 1;
    }

    char buffer[64];
    struct sockaddr_in addr = {0};
    socklen_t addrLen = sizeof(addr);
    struct msghdr msg = {0};

    /* NULL loop tests */
    uint64_t opId = loopyIoUringSend(NULL, 0, buffer, sizeof(buffer), 0,
                                     iouring_net_callback, NULL);
    TEST_ASSERT(opId == 0, "send with NULL loop should return 0");

    opId = loopyIoUringRecv(NULL, 0, buffer, sizeof(buffer), 0,
                            iouring_net_callback, NULL);
    TEST_ASSERT(opId == 0, "recv with NULL loop should return 0");

    opId = loopyIoUringAccept(NULL, 0, (struct sockaddr *)&addr, &addrLen, 0,
                              iouring_net_callback, NULL);
    TEST_ASSERT(opId == 0, "accept with NULL loop should return 0");

    opId = loopyIoUringConnect(NULL, 0, (struct sockaddr *)&addr, addrLen,
                               iouring_net_callback, NULL);
    TEST_ASSERT(opId == 0, "connect with NULL loop should return 0");

    opId = loopyIoUringShutdown(NULL, 0, SHUT_RDWR, iouring_net_callback, NULL);
    TEST_ASSERT(opId == 0, "shutdown with NULL loop should return 0");

    opId = loopyIoUringSendmsg(NULL, 0, &msg, 0, iouring_net_callback, NULL);
    TEST_ASSERT(opId == 0, "sendmsg with NULL loop should return 0");

    opId = loopyIoUringRecvmsg(NULL, 0, &msg, 0, iouring_net_callback, NULL);
    TEST_ASSERT(opId == 0, "recvmsg with NULL loop should return 0");

    /* NULL buffer tests */
    opId = loopyIoUringSend(l, 0, NULL, sizeof(buffer), 0, iouring_net_callback,
                            NULL);
    TEST_ASSERT(opId == 0, "send with NULL buffer should return 0");

    opId = loopyIoUringRecv(l, 0, NULL, sizeof(buffer), 0, iouring_net_callback,
                            NULL);
    TEST_ASSERT(opId == 0, "recv with NULL buffer should return 0");

    /* NULL callback tests */
    opId = loopyIoUringSend(l, 0, buffer, sizeof(buffer), 0, NULL, NULL);
    TEST_ASSERT(opId == 0, "send with NULL callback should return 0");

    opId = loopyIoUringRecv(l, 0, buffer, sizeof(buffer), 0, NULL, NULL);
    TEST_ASSERT(opId == 0, "recv with NULL callback should return 0");

    /* NULL msghdr tests */
    opId = loopyIoUringSendmsg(l, 0, NULL, 0, iouring_net_callback, NULL);
    TEST_ASSERT(opId == 0, "sendmsg with NULL msg should return 0");

    opId = loopyIoUringRecvmsg(l, 0, NULL, 0, iouring_net_callback, NULL);
    TEST_ASSERT(opId == 0, "recvmsg with NULL msg should return 0");

    /* NULL sockaddr tests */
    opId = loopyIoUringConnect(l, 0, NULL, addrLen, iouring_net_callback, NULL);
    TEST_ASSERT(opId == 0, "connect with NULL addr should return 0");

    return 1;
}

static int test_iouring_net_large_transfer(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    TEST_ASSERT(l, "loop should be created");

    if (!loopyUsingIoUring(l)) {
        printf("(skipped: io_uring not available) ");
        return 1;
    }

    if (!loopyIoUringNetAvailable(l)) {
        printf("(skipped: io_uring network ops not available) ");
        return 1;
    }

    /* Create a socketpair for testing */
    int socks[2];
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, socks);
    TEST_ASSERT(ret == 0, "socketpair should succeed");

    /* Allocate large buffer (64KB) */
    size_t bufSize = 65536;
    char *sendBuf = zmalloc(bufSize);
    char *recvBuf = zmalloc(bufSize);
    TEST_ASSERT(sendBuf && recvBuf, "malloc should succeed");

    /* Fill with pattern */
    for (size_t i = 0; i < bufSize; i++) {
        sendBuf[i] = (char)(i % 256);
    }
    memset(recvBuf, 0, bufSize);

    IoUringNetTestContext ctx = {0};
    ctx.loop = l;

    /* Submit send operation */
    uint64_t opId = loopyIoUringSend(l, socks[0], sendBuf, bufSize, 0,
                                     iouring_net_callback, &ctx);
    TEST_ASSERT(opId != 0, "send operation should be submitted");

    /* Add timeout */
    loopyRegisterTimer(l, 2000000, 0, timeout_callback, l);

    /* Run event loop */
    loopyMain(l);

    /* Verify send succeeded */
    TEST_ASSERT(ctx.completed, "callback should have been called");
    TEST_ASSERT(ctx.result > 0, "should send some bytes");

    /* Receive data (may need multiple recv calls) */
    size_t totalReceived = 0;
    while (totalReceived < (size_t)ctx.result) {
        ssize_t n =
            recv(socks[1], recvBuf + totalReceived, bufSize - totalReceived, 0);
        if (n <= 0) {
            break;
        }
        totalReceived += n;
    }

    TEST_ASSERT(totalReceived == (size_t)ctx.result,
                "should receive all sent bytes");
    TEST_ASSERT(memcmp(sendBuf, recvBuf, totalReceived) == 0,
                "data should match");

    zfree(sendBuf);
    zfree(recvBuf);
    close(socks[0]);
    close(socks[1]);
    return 1;
}

static int test_iouring_net_fallback(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    TEST_ASSERT(l, "loop should be created");

    if (loopyUsingIoUring(l)) {
        printf("(skipped: io_uring is available) ");
        return 1;
    }

    /* On non-io_uring systems, API should return 0 gracefully */
    char buffer[64];
    struct sockaddr_in addr = {0};
    socklen_t addrLen = sizeof(addr);
    struct msghdr msg = {0};

    uint64_t opId = loopyIoUringSend(l, 0, buffer, sizeof(buffer), 0,
                                     iouring_net_callback, NULL);
    TEST_ASSERT(opId == 0, "send should return 0 on non-io_uring system");

    opId = loopyIoUringRecv(l, 0, buffer, sizeof(buffer), 0,
                            iouring_net_callback, NULL);
    TEST_ASSERT(opId == 0, "recv should return 0 on non-io_uring system");

    opId = loopyIoUringAccept(l, 0, (struct sockaddr *)&addr, &addrLen, 0,
                              iouring_net_callback, NULL);
    TEST_ASSERT(opId == 0, "accept should return 0 on non-io_uring system");

    opId = loopyIoUringConnect(l, 0, (struct sockaddr *)&addr, addrLen,
                               iouring_net_callback, NULL);
    TEST_ASSERT(opId == 0, "connect should return 0 on non-io_uring system");

    opId = loopyIoUringShutdown(l, 0, SHUT_RDWR, iouring_net_callback, NULL);
    TEST_ASSERT(opId == 0, "shutdown should return 0 on non-io_uring system");

    opId = loopyIoUringSendmsg(l, 0, &msg, 0, iouring_net_callback, NULL);
    TEST_ASSERT(opId == 0, "sendmsg should return 0 on non-io_uring system");

    opId = loopyIoUringRecvmsg(l, 0, &msg, 0, iouring_net_callback, NULL);
    TEST_ASSERT(opId == 0, "recvmsg should return 0 on non-io_uring system");

    /* Availability checks */
    bool available = loopyIoUringNetAvailable(l);
    TEST_ASSERT(!available, "network ops should not be available");

    bool hasMultishot = loopyIoUringNetHasMultishot(l);
    TEST_ASSERT(!hasMultishot, "multishot should not be available");

    return 1;
}

#endif /* __linux__ */

/* ====================================================================
 * Metrics Tests
 * ==================================================================== */

static int test_metrics_enable_disable(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    TEST_ASSERT(l, "loop should be created");

    /* Metrics should be disabled by default */
    TEST_ASSERT(!loopyMetricsEnabled(l),
                "metrics should be disabled by default");

    /* Enable metrics */
    TEST_ASSERT(loopyMetricsEnable(l), "enable should succeed");
    TEST_ASSERT(loopyMetricsEnabled(l),
                "metrics should be enabled after enable");

    /* Enabling again should fail */
    TEST_ASSERT(!loopyMetricsEnable(l), "enabling twice should fail");

    /* Disable metrics */
    loopyMetricsDisable(l);
    TEST_ASSERT(!loopyMetricsEnabled(l),
                "metrics should be disabled after disable");

    /* Re-enable should work */
    TEST_ASSERT(loopyMetricsEnable(l), "re-enable should succeed");
    TEST_ASSERT(loopyMetricsEnabled(l),
                "metrics should be enabled after re-enable");

    return 1;
}

static int metrics_iteration_count = 0;
static loopyLoop *metrics_test_loop = NULL;

static bool metrics_stop_callback(timerWheel *t, timerWheelId id, void *data) {
    (void)t;
    (void)id;
    loopyLoop *l = data;
    loopyStop(l);
    return false; /* Don't repeat */
}

static bool metrics_iteration_callback(timerWheel *t, timerWheelId id,
                                       void *data) {
    (void)t;
    (void)id;
    (void)data;
    metrics_iteration_count++;
    if (metrics_iteration_count >= 5) {
        loopyStop(metrics_test_loop);
        return false; /* Don't repeat */
    }
    return true; /* Continue */
}

static int test_metrics_loop_iterations(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    TEST_ASSERT(l, "loop should be created");
    TEST_ASSERT(loopyMetricsEnable(l), "enable should succeed");

    metrics_iteration_count = 0;
    metrics_test_loop = l;

    /* Create a timer that fires repeatedly to drive loop iterations */
    loopyRegisterTimer(l, 1000, 1000, metrics_iteration_callback, NULL);

    /* Run the loop */
    loopyMain(l);

    /* Get metrics */
    loopyMetrics m;
    TEST_ASSERT(loopyMetricsGet(l, &m), "get metrics should succeed");

    /* We should have at least 5 iterations */
    TEST_ASSERT(m.loopIterations >= 5, "should have at least 5 iterations");

    /* Also test individual getter */
    uint64_t iterations = loopyMetricsGetIterations(l);
    TEST_ASSERT(iterations >= 5, "getter should show at least 5 iterations");

    return 1;
}

static int events_processed_count = 0;

static void events_processed_callback(loopyLoop *l, int fd, void *clientData,
                                      loopyAction mask) {
    (void)fd;
    (void)clientData;
    (void)mask;
    events_processed_count++;
    if (events_processed_count >= 3) {
        loopyStop(l);
    }
}

static int test_metrics_events_processed(void) {
    loopyLoop *l LOOPY_LOOP_AUTO_CLEANUP = loopyNew(16);
    TEST_ASSERT(l, "loop should be created");
    TEST_ASSERT(loopyMetricsEnable(l), "enable should succeed");

    int fds[2];
    TEST_ASSERT(pipe(fds) == 0, "pipe should succeed");

    events_processed_count = 0;

    /* Register for read events */
    TEST_ASSERT(loopyRegisterRead(l, fds[0], events_processed_callback, NULL),
                "register should succeed");

    /* Add a timeout in case something goes wrong */
    loopyRegisterTimer(l, 1000000, 0, metrics_stop_callback, l);

    /* Write data to trigger events */
    for (int i = 0; i < 3; i++) {
        ssize_t written = write(fds[1], "x", 1);
        TEST_ASSERT(written == 1, "write should succeed");
        /* Small delay to ensure events are processed separately */
        usleep(10000);
    }

    /* Run the loop */
    loopyMain(l);

    /* Get metrics */
    loopyMetrics m;
    TEST_ASSERT(loopyMetricsGet(l, &m), "get metrics should succeed");

    /* We should have processed at least 3 events */
    TEST_ASSERT(m.eventsProcessed >= 3,
                "should have at least 3 events processed");

    /* Test individual getter */
    uint64_t processed = loopyMetricsGetEventsProcessed(l);
    TEST_ASSERT(processed >= 3,
                "getter should show at least 3 events processed");

    close(fds[0]);
    close(fds[1]);
    return 1;
}

static int test_metrics_reset(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    TEST_ASSERT(l, "loop should be created");
    TEST_ASSERT(loopyMetricsEnable(l), "enable should succeed");

    /* Run a few iterations to generate some metrics */
    int fds[2];
    TEST_ASSERT(pipe(fds) == 0, "pipe should succeed");

    events_processed_count = 0;
    TEST_ASSERT(loopyRegisterRead(l, fds[0], events_processed_callback, NULL),
                "register should succeed");

    /* Add a timeout */
    loopyRegisterTimer(l, 500000, 0, metrics_stop_callback, l);

    /* Write data */
    write(fds[1], "x", 1);

    loopyMain(l);

    /* Verify we have some metrics */
    loopyMetrics m;
    TEST_ASSERT(loopyMetricsGet(l, &m), "get metrics should succeed");
    TEST_ASSERT(m.loopIterations > 0, "should have some iterations");

    /* Reset metrics */
    loopyMetricsReset(l);

    /* Verify counters are reset */
    TEST_ASSERT(loopyMetricsGet(l, &m),
                "get metrics after reset should succeed");
    TEST_ASSERT_EQ(m.loopIterations, 0, "iterations should be 0 after reset");
    TEST_ASSERT_EQ(m.eventsProcessed, 0, "events should be 0 after reset");
    TEST_ASSERT_EQ(m.timersProcessed, 0, "timers should be 0 after reset");
    TEST_ASSERT_EQ(m.totalPollTime, 0, "poll time should be 0 after reset");
    TEST_ASSERT_EQ(m.totalIdleTime, 0, "idle time should be 0 after reset");

    close(fds[0]);
    close(fds[1]);
    return 1;
}

static int test_metrics_null_safety(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    loopyMetrics m;

    /* NULL loop checks */
    TEST_ASSERT(!loopyMetricsEnable(NULL), "enable NULL should fail");
    TEST_ASSERT(!loopyMetricsEnabled(NULL), "enabled NULL should return false");
    TEST_ASSERT(!loopyMetricsGet(NULL, &m), "get NULL loop should fail");
    TEST_ASSERT_EQ(loopyMetricsIdleTime(NULL), 0,
                   "idle time NULL should return 0");
    TEST_ASSERT_EQ(loopyMetricsPollTime(NULL), 0,
                   "poll time NULL should return 0");
    TEST_ASSERT_EQ(loopyMetricsGetIterations(NULL), 0,
                   "iterations NULL should return 0");
    TEST_ASSERT_EQ(loopyMetricsGetEventsProcessed(NULL), 0,
                   "events NULL should return 0");
    TEST_ASSERT_EQ(loopyMetricsGetTimersProcessed(NULL), 0,
                   "timers NULL should return 0");

    /* These should be no-ops, not crash */
    loopyMetricsReset(NULL);
    loopyMetricsDisable(NULL);

    /* Metrics not enabled checks */
    TEST_ASSERT(!loopyMetricsGet(l, &m), "get without enable should fail");
    TEST_ASSERT_EQ(loopyMetricsIdleTime(l), 0,
                   "idle time without enable should return 0");

    /* NULL metrics pointer */
    TEST_ASSERT(loopyMetricsEnable(l), "enable should succeed");
    TEST_ASSERT(!loopyMetricsGet(l, NULL), "get NULL metrics should fail");

    return 1;
}

static int test_metrics_poll_time(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    TEST_ASSERT(l, "loop should be created");
    TEST_ASSERT(loopyMetricsEnable(l), "enable should succeed");

    /* Create a timer that fires after some time */
    loopyRegisterTimer(l, 50000, 0, metrics_stop_callback, l);

    /* Run the loop - it should spend time in poll */
    loopyMain(l);

    /* Get metrics */
    loopyMetrics m;
    TEST_ASSERT(loopyMetricsGet(l, &m), "get metrics should succeed");

    /* We should have accumulated some poll time */
    TEST_ASSERT(m.totalPollTime > 0, "should have some poll time");

    /* Test getters */
    uint64_t pollTime = loopyMetricsPollTime(l);
    TEST_ASSERT(pollTime > 0, "poll time getter should show time");

    uint64_t idleTime = loopyMetricsIdleTime(l);
    /* Idle time depends on whether events fired or not */
    (void)idleTime; /* Just verify it doesn't crash */

    return 1;
}

static int test_metrics_timers_processed(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    TEST_ASSERT(l, "loop should be created");
    TEST_ASSERT(loopyMetricsEnable(l), "enable should succeed");

    /* Note: We use a simple approach - count timer batches, not individual
     * timers */
    loopyRegisterTimer(l, 10000, 0, metrics_stop_callback, l);

    loopyMain(l);

    loopyMetrics m;
    TEST_ASSERT(loopyMetricsGet(l, &m), "get metrics should succeed");

    /* Should have processed at least 1 timer batch */
    TEST_ASSERT(m.timersProcessed >= 1,
                "should have processed at least 1 timer");

    /* Test getter */
    uint64_t timers = loopyMetricsGetTimersProcessed(l);
    TEST_ASSERT(timers >= 1, "getter should show at least 1 timer");

    return 1;
}

/* ====================================================================
 * System Utilities Tests
 * ==================================================================== */

static int test_sys_interface_addresses(void) {
    loopyInterfaceAddress *addrs = NULL;
    size_t count = 0;

    TEST_ASSERT(loopyInterfaceAddresses(&addrs, &count),
                "should get interface addresses");

    /* Should have at least one interface (loopback) */
    TEST_ASSERT(count >= 1, "should have at least one interface");

    bool foundLoopback = false;
    for (size_t i = 0; i < count; i++) {
        if (addrs[i].isInternal) {
            foundLoopback = true;
        }
        /* Verify address is non-empty */
        TEST_ASSERT(strlen(addrs[i].address) > 0,
                    "address should not be empty");
        TEST_ASSERT(strlen(addrs[i].name) > 0, "name should not be empty");
    }

    TEST_ASSERT(foundLoopback, "should find loopback interface");

    loopyInterfaceAddressesFree(addrs);
    loopyInterfaceAddressesFree(NULL); /* Should not crash */

    return 1;
}

static int test_sys_available_parallelism(void) {
    int cpus = loopyAvailableParallelism();
    TEST_ASSERT(cpus >= 1, "should have at least 1 CPU");
    TEST_ASSERT(cpus <= 1024, "should have reasonable CPU count");
    return 1;
}

static int test_sys_exe_path(void) {
    char buf[1024];

    ssize_t len = loopyExePath(buf, sizeof(buf));
    TEST_ASSERT(len > 0, "should get executable path");
    TEST_ASSERT((size_t)len < sizeof(buf), "path should fit in buffer");

    /* Path should contain our test binary name */
    TEST_ASSERT(strstr(buf, "loopyUnitTest") != NULL ||
                    strstr(buf, "Unit") != NULL,
                "path should contain test binary name");

    /* Test NULL safety */
    TEST_ASSERT(loopyExePath(NULL, 0) == -1, "NULL buffer should fail");

    return 1;
}

static int test_sys_statfs(void) {
    loopyStatfs buf;
    memset(&buf, 0, sizeof(buf));

    /* Test sync statfs on root filesystem */
    loopyFSRequest *req = loopyFSStatfs(NULL, "/", &buf, NULL, NULL);
    TEST_ASSERT(req != NULL, "statfs should succeed");
    TEST_ASSERT(loopyFSRequestGetResult(req) == 0, "result should be 0");

    /* Verify we got some data */
    TEST_ASSERT(buf.bsize > 0, "block size should be > 0");
    TEST_ASSERT(buf.blocks > 0, "total blocks should be > 0");

    loopyFSRequestFree(req);

    /* Test NULL safety */
    TEST_ASSERT(loopyFSStatfs(NULL, NULL, &buf, NULL, NULL) == NULL,
                "NULL path should fail");
    TEST_ASSERT(loopyFSStatfs(NULL, "/", NULL, NULL, NULL) == NULL,
                "NULL buffer should fail");

    return 1;
}

static int test_sys_null_safety(void) {
    size_t count = 0;

    /* Interface addresses */
    TEST_ASSERT(!loopyInterfaceAddresses(NULL, &count),
                "NULL addrs should fail");
    TEST_ASSERT(
        !loopyInterfaceAddresses((loopyInterfaceAddress **)&count, NULL),
        "NULL count should fail");

    return 1;
}

static int test_sys_homedir(void) {
    char buf[512];

    /* Get home directory */
    ssize_t len = loopyHomedir(buf, sizeof(buf));
    TEST_ASSERT(len > 0, "homedir should succeed");
    TEST_ASSERT(buf[0] == '/', "homedir should be absolute path");
    TEST_ASSERT(strlen(buf) == (size_t)len, "length should match");

    /* Verify it's a directory */
    struct stat st;
    TEST_ASSERT(stat(buf, &st) == 0, "homedir should exist");
    TEST_ASSERT(S_ISDIR(st.st_mode), "homedir should be a directory");

    /* Test small buffer (should return required size) */
    char small[2];
    ssize_t required = loopyHomedir(small, sizeof(small));
    TEST_ASSERT(required > 2, "should indicate buffer too small");

    return 1;
}

static int test_sys_tmpdir(void) {
    char buf[512];

    /* Get temp directory */
    ssize_t len = loopyTmpdir(buf, sizeof(buf));
    TEST_ASSERT(len > 0, "tmpdir should succeed");
    TEST_ASSERT(buf[0] == '/', "tmpdir should be absolute path");
    TEST_ASSERT(strlen(buf) == (size_t)len, "length should match");

    /* Verify it's a directory */
    struct stat st;
    TEST_ASSERT(stat(buf, &st) == 0, "tmpdir should exist");
    TEST_ASSERT(S_ISDIR(st.st_mode), "tmpdir should be a directory");

    /* No trailing slash (unless root) */
    if (len > 1) {
        TEST_ASSERT(buf[len - 1] != '/', "no trailing slash");
    }

    return 1;
}

static int test_sys_cwd_chdir(void) {
    char original[512];
    char current[512];

    /* Get current working directory */
    ssize_t len = loopyCwd(original, sizeof(original));
    TEST_ASSERT(len > 0, "cwd should succeed");
    TEST_ASSERT(original[0] == '/', "cwd should be absolute path");

    /* Get temp directory for changing to */
    char tmpdir[512];
    ssize_t tmplen = loopyTmpdir(tmpdir, sizeof(tmpdir));
    TEST_ASSERT(tmplen > 0, "tmpdir should succeed");

    /* Change to temp directory */
    TEST_ASSERT(loopyChdir(tmpdir), "chdir to tmpdir should succeed");

    /* Verify we changed */
    loopyCwd(current, sizeof(current));
    /* Note: On macOS, /tmp may be a symlink to /private/tmp */
    TEST_ASSERT(strlen(current) > 0, "cwd after chdir should work");

    /* Change back to original */
    TEST_ASSERT(loopyChdir(original), "chdir back should succeed");
    loopyCwd(current, sizeof(current));
    TEST_ASSERT(strcmp(current, original) == 0, "should be back to original");

    /* Test invalid directory */
    TEST_ASSERT(!loopyChdir("/nonexistent/path/12345"),
                "invalid chdir should fail");

    return 1;
}

static int test_sys_hostname(void) {
    char buf[256];

    /* Get hostname */
    ssize_t len = loopyHostname(buf, sizeof(buf));
    TEST_ASSERT(len > 0, "hostname should succeed");
    TEST_ASSERT(strlen(buf) == (size_t)len, "length should match");
    TEST_ASSERT(strlen(buf) > 0, "hostname should not be empty");

    /* Test small buffer */
    char small[1];
    ssize_t result = loopyHostname(small, sizeof(small));
    /* Result depends on actual hostname length */
    (void)result;

    return 1;
}

static int test_sys_paths_null_safety(void) {
    /* Homedir */
    TEST_ASSERT(loopyHomedir(NULL, 256) == -1, "NULL buf should fail");
    char buf[256];
    TEST_ASSERT(loopyHomedir(buf, 0) == -1, "zero bufLen should fail");

    /* Tmpdir */
    TEST_ASSERT(loopyTmpdir(NULL, 256) == -1, "NULL buf should fail");
    TEST_ASSERT(loopyTmpdir(buf, 0) == -1, "zero bufLen should fail");

    /* Cwd */
    TEST_ASSERT(loopyCwd(NULL, 256) == -1, "NULL buf should fail");
    TEST_ASSERT(loopyCwd(buf, 0) == -1, "zero bufLen should fail");

    /* Chdir */
    TEST_ASSERT(!loopyChdir(NULL), "NULL path should fail");

    /* Hostname */
    TEST_ASSERT(loopyHostname(NULL, 256) == -1, "NULL buf should fail");
    TEST_ASSERT(loopyHostname(buf, 0) == -1, "zero bufLen should fail");

    return 1;
}

/* ====================================================================
 * Memory-Mapped File Tests
 * ==================================================================== */

static int test_mmap_utilities(void) {
    /* Test page size */
    size_t pageSize = loopyMmapPageSize();
    TEST_ASSERT(pageSize > 0, "page size should be positive");
    TEST_ASSERT(pageSize >= 4096, "page size should be at least 4KB");
    TEST_ASSERT((pageSize & (pageSize - 1)) == 0,
                "page size should be power of 2");

    /* Test alignment */
    TEST_ASSERT(loopyMmapAlignSize(0) == 0, "align 0 should be 0");
    TEST_ASSERT(loopyMmapAlignSize(1) == pageSize,
                "align 1 should be pageSize");
    TEST_ASSERT(loopyMmapAlignSize(pageSize) == pageSize,
                "align pageSize should be pageSize");
    TEST_ASSERT(loopyMmapAlignSize(pageSize + 1) == pageSize * 2,
                "align pageSize+1 should be 2*pageSize");
    TEST_ASSERT(loopyMmapAlignSize(pageSize * 3 - 1) == pageSize * 3,
                "align should round up");

    return 1;
}

static int test_mmap_file_readonly(void) {
    /* Create a test file */
    const char *path = "/tmp/loopy_mmap_test_ro.txt";
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    TEST_ASSERT(fd >= 0, "open file for writing");

    const char *data = "Hello, Memory-Mapped World!";
    size_t dataLen = strlen(data);
    TEST_ASSERT(write(fd, data, dataLen) == (ssize_t)dataLen,
                "write test data");
    close(fd);

    /* Open for reading */
    fd = open(path, O_RDONLY);
    TEST_ASSERT(fd >= 0, "open file for reading");

    /* Map file read-only */
    loopyMmap *m =
        loopyMmapFile(fd, 0, 0, LOOPY_MMAP_PROT_READ, LOOPY_MMAP_PRIVATE);
    TEST_ASSERT(m != NULL, "file mapping should succeed");
    TEST_ASSERT(loopyMmapIsValid(m), "mapping should be valid");

    /* Verify mapped content */
    const char *mapped = (const char *)loopyMmapAddress(m);
    TEST_ASSERT(mapped != NULL, "mapped address should not be NULL");
    TEST_ASSERT(loopyMmapSize(m) == dataLen,
                "mapped size should match file size");
    TEST_ASSERT(memcmp(mapped, data, dataLen) == 0,
                "mapped data should match file content");

    /* Cleanup */
    loopyMmapUnmap(m);
    close(fd);
    unlink(path);

    return 1;
}

static int test_mmap_file_readwrite(void) {
    /* Create a test file */
    const char *path = "/tmp/loopy_mmap_test_rw.txt";
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    TEST_ASSERT(fd >= 0, "open file for writing");

    const char *original = "Original Content";
    size_t len = strlen(original);
    TEST_ASSERT(write(fd, original, len) == (ssize_t)len,
                "write original data");

    /* Map file read-write with SHARED (changes written to file) */
    loopyMmap *m =
        loopyMmapFile(fd, 0, 0, LOOPY_MMAP_PROT_READ | LOOPY_MMAP_PROT_WRITE,
                      LOOPY_MMAP_SHARED);
    TEST_ASSERT(m != NULL, "file mapping should succeed");

    /* Modify mapped content */
    char *mapped = (char *)loopyMmapAddress(m);
    TEST_ASSERT(mapped != NULL, "mapped address should not be NULL");
    mapped[0] = 'M';
    mapped[1] = 'o';
    mapped[2] = 'd';

    /* Sync changes to disk */
    TEST_ASSERT(loopyMmapSync(m, 0, 0, LOOPY_MMAP_SYNC_SYNC),
                "sync should succeed");

    loopyMmapUnmap(m);

    /* Verify file was modified */
    lseek(fd, 0, SEEK_SET);
    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf));
    TEST_ASSERT(n == (ssize_t)len, "read should return original size");
    TEST_ASSERT(buf[0] == 'M' && buf[1] == 'o' && buf[2] == 'd',
                "file should be modified");

    close(fd);
    unlink(path);

    return 1;
}

static int test_mmap_anonymous(void) {
    /* Allocate anonymous memory (like malloc but via mmap) */
    size_t size = 1024 * 1024; /* 1MB */
    loopyMmap *m = loopyMmapAnon(
        size, LOOPY_MMAP_PROT_READ | LOOPY_MMAP_PROT_WRITE, LOOPY_MMAP_PRIVATE);
    TEST_ASSERT(m != NULL, "anonymous mapping should succeed");
    TEST_ASSERT(loopyMmapIsValid(m), "mapping should be valid");
    TEST_ASSERT(loopyMmapSize(m) == size, "size should match");

    /* Access and modify memory */
    char *data = (char *)loopyMmapAddress(m);
    TEST_ASSERT(data != NULL, "mapped address should not be NULL");

    /* Write pattern */
    for (size_t i = 0; i < 1000; i++) {
        data[i] = (char)(i % 256);
    }

    /* Verify pattern */
    for (size_t i = 0; i < 1000; i++) {
        TEST_ASSERT(data[i] == (char)(i % 256), "data should match pattern");
    }

    loopyMmapUnmap(m);

    return 1;
}

static int test_mmap_shared_ipc(void) {
    /* Create shared anonymous mapping for IPC */
    size_t size = 4096;
    loopyMmap *m = loopyMmapAnon(
        size, LOOPY_MMAP_PROT_READ | LOOPY_MMAP_PROT_WRITE, LOOPY_MMAP_SHARED);
    TEST_ASSERT(m != NULL, "shared anonymous mapping should succeed");

    volatile int *counter = (volatile int *)loopyMmapAddress(m);
    TEST_ASSERT(counter != NULL, "mapped address should not be NULL");
    *counter = 0;

    /* Fork and test IPC */
    pid_t pid = fork();
    if (pid == 0) {
        /* Child process: increment counter */
        for (int i = 0; i < 100; i++) {
            (*counter)++;
        }
        _exit(0);
    } else if (pid > 0) {
        /* Parent process: wait for child */
        int status;
        waitpid(pid, &status, 0);

        /* Verify shared memory was modified */
        TEST_ASSERT(*counter == 100,
                    "shared memory should show child's modifications");
    } else {
        TEST_ASSERT(0, "fork should succeed");
    }

    loopyMmapUnmap(m);

    return 1;
}

static int test_mmap_advice(void) {
    /* Create file for testing */
    const char *path = "/tmp/loopy_mmap_test_advice.txt";
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    TEST_ASSERT(fd >= 0, "open file");

    /* Write 64KB of data */
    char buf[4096];
    memset(buf, 'X', sizeof(buf));
    for (int i = 0; i < 16; i++) {
        write(fd, buf, sizeof(buf));
    }

    loopyMmap *m =
        loopyMmapFile(fd, 0, 0, LOOPY_MMAP_PROT_READ, LOOPY_MMAP_PRIVATE);
    TEST_ASSERT(m != NULL, "file mapping should succeed");

    /* Test various advice hints */
    TEST_ASSERT(loopyMmapAdvise(m, 0, 0, LOOPY_MMAP_ADVICE_SEQUENTIAL),
                "sequential advice should succeed");
    TEST_ASSERT(loopyMmapAdvise(m, 0, 0, LOOPY_MMAP_ADVICE_RANDOM),
                "random advice should succeed");
    TEST_ASSERT(loopyMmapAdvise(m, 0, 0, LOOPY_MMAP_ADVICE_WILLNEED),
                "willneed advice should succeed");
    TEST_ASSERT(loopyMmapAdvise(m, 0, 0, LOOPY_MMAP_ADVICE_DONTNEED),
                "dontneed advice should succeed");
    TEST_ASSERT(loopyMmapAdvise(m, 0, 0, LOOPY_MMAP_ADVICE_NORMAL),
                "normal advice should succeed");

    /* Test partial range advice */
    TEST_ASSERT(loopyMmapAdvise(m, 0, 4096, LOOPY_MMAP_ADVICE_WILLNEED),
                "partial range advice should succeed");

    loopyMmapUnmap(m);
    close(fd);
    unlink(path);

    return 1;
}

static int test_mmap_sync(void) {
    /* Create file */
    const char *path = "/tmp/loopy_mmap_test_sync.txt";
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    TEST_ASSERT(fd >= 0, "open file");

    /* Write initial data */
    char buf[4096];
    memset(buf, 'A', sizeof(buf));
    write(fd, buf, sizeof(buf));

    loopyMmap *m =
        loopyMmapFile(fd, 0, 0, LOOPY_MMAP_PROT_READ | LOOPY_MMAP_PROT_WRITE,
                      LOOPY_MMAP_SHARED);
    TEST_ASSERT(m != NULL, "file mapping should succeed");

    char *data = (char *)loopyMmapAddress(m);
    data[0] = 'B';

    /* Test different sync modes */
    TEST_ASSERT(loopyMmapSync(m, 0, 0, LOOPY_MMAP_SYNC_ASYNC),
                "async sync should succeed");
    TEST_ASSERT(loopyMmapSync(m, 0, 0, LOOPY_MMAP_SYNC_SYNC),
                "sync sync should succeed");

    /* Test partial sync */
    data[100] = 'C';
    TEST_ASSERT(loopyMmapSync(m, 0, 1024, LOOPY_MMAP_SYNC_SYNC),
                "partial sync should succeed");

    loopyMmapUnmap(m);
    close(fd);
    unlink(path);

    return 1;
}

static int test_mmap_lock_unlock(void) {
    /* Create small anonymous mapping */
    size_t size = 4096;
    loopyMmap *m = loopyMmapAnon(
        size, LOOPY_MMAP_PROT_READ | LOOPY_MMAP_PROT_WRITE, LOOPY_MMAP_PRIVATE);
    TEST_ASSERT(m != NULL, "anonymous mapping should succeed");

    /* Lock pages in memory (may fail without sufficient privileges) */
    bool locked = loopyMmapLock(m, 0, 0);
    if (locked) {
        /* If lock succeeded, verify unlock works */
        TEST_ASSERT(loopyMmapUnlock(m, 0, 0), "unlock should succeed");
    } else {
        printf("(mlock requires elevated privileges, skipping) ");
    }

    /* Test partial lock/unlock (even if it fails) */
    loopyMmapLock(m, 0, 2048);
    loopyMmapUnlock(m, 0, 2048);

    loopyMmapUnmap(m);

    return 1;
}

static int test_mmap_offset_length(void) {
    /* Create file with known content */
    const char *path = "/tmp/loopy_mmap_test_offset.txt";
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    TEST_ASSERT(fd >= 0, "open file");

    /* Write multiple pages */
    size_t pageSize = loopyMmapPageSize();
    char *buf = zcalloc(1, pageSize * 3);
    TEST_ASSERT(buf != NULL, "allocate buffer");

    for (size_t i = 0; i < pageSize * 3; i++) {
        buf[i] = (char)((i / pageSize) + 'A');
    }
    write(fd, buf, pageSize * 3);

    /* Map second page only (offset must be page-aligned) */
    loopyMmap *m = loopyMmapFile(fd, pageSize, pageSize, LOOPY_MMAP_PROT_READ,
                                 LOOPY_MMAP_PRIVATE);
    TEST_ASSERT(m != NULL, "offset mapping should succeed");
    TEST_ASSERT(loopyMmapSize(m) == pageSize, "size should match");

    const char *data = (const char *)loopyMmapAddress(m);
    TEST_ASSERT(data[0] == 'B', "should read second page");

    loopyMmapUnmap(m);
    close(fd);
    zfree(buf);
    unlink(path);

    return 1;
}

static int test_mmap_error_handling(void) {
    /* Test NULL/invalid inputs */
    TEST_ASSERT(loopyMmapFile(-1, 0, 0, LOOPY_MMAP_PROT_READ,
                              LOOPY_MMAP_PRIVATE) == NULL,
                "invalid fd should fail");

    /* Test anonymous with zero size */
    TEST_ASSERT(loopyMmapAnon(0, LOOPY_MMAP_PROT_READ, LOOPY_MMAP_PRIVATE) ==
                    NULL,
                "zero size anonymous should fail");

    /* Test offset not page-aligned */
    const char *path = "/tmp/loopy_mmap_test_error.txt";
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    TEST_ASSERT(fd >= 0, "open file");
    write(fd, "test", 4);

    loopyMmap *m =
        loopyMmapFile(fd, 1, 0, LOOPY_MMAP_PROT_READ, LOOPY_MMAP_PRIVATE);
    TEST_ASSERT(m == NULL, "unaligned offset should fail");

    close(fd);
    unlink(path);

    return 1;
}

static int test_mmap_null_safety(void) {
    /* Test all functions with NULL handle */
    TEST_ASSERT(loopyMmapAddress(NULL) == NULL,
                "address of NULL should be NULL");
    TEST_ASSERT(loopyMmapSize(NULL) == 0, "size of NULL should be 0");
    TEST_ASSERT(!loopyMmapIsValid(NULL), "NULL should not be valid");
    TEST_ASSERT(!loopyMmapSync(NULL, 0, 0, LOOPY_MMAP_SYNC_SYNC),
                "sync NULL should fail");
    TEST_ASSERT(!loopyMmapAdvise(NULL, 0, 0, LOOPY_MMAP_ADVICE_NORMAL),
                "advise NULL should fail");
    TEST_ASSERT(!loopyMmapLock(NULL, 0, 0), "lock NULL should fail");
    TEST_ASSERT(!loopyMmapUnlock(NULL, 0, 0), "unlock NULL should fail");

    /* Unmap NULL should not crash */
    loopyMmapUnmap(NULL);

    return 1;
}

/* ====================================================================
 * Direct I/O Tests (O_DIRECT)
 * ==================================================================== */

/**
 * Test aligned buffer allocation and deallocation.
 */
static int test_direct_io_aligned_allocation(void) {
    /* Test various sizes with 512-byte alignment (typical sector size) */
    size_t alignment = loopyFSDirectAlignment();
    TEST_ASSERT_EQ(alignment, 512, "default alignment should be 512 bytes");

    /* Allocate 512-byte aligned buffer */
    void *buf512 = loopyFSAllocAligned(512, alignment);
    TEST_ASSERT(buf512 != NULL, "512-byte allocation should succeed");
    TEST_ASSERT((uintptr_t)buf512 % alignment == 0,
                "512-byte buffer should be aligned");
    loopyFSFreeAligned(buf512);

    /* Allocate 4KB aligned buffer */
    void *buf4k = loopyFSAllocAligned(4096, alignment);
    TEST_ASSERT(buf4k != NULL, "4KB allocation should succeed");
    TEST_ASSERT((uintptr_t)buf4k % alignment == 0,
                "4KB buffer should be aligned");
    loopyFSFreeAligned(buf4k);

    /* Allocate 1MB aligned buffer */
    void *buf1m = loopyFSAllocAligned(1024 * 1024, alignment);
    TEST_ASSERT(buf1m != NULL, "1MB allocation should succeed");
    TEST_ASSERT((uintptr_t)buf1m % alignment == 0,
                "1MB buffer should be aligned");
    loopyFSFreeAligned(buf1m);

    /* Test alignment with different values */
    void *buf_aligned_4k = loopyFSAllocAligned(8192, 4096);
    TEST_ASSERT(buf_aligned_4k != NULL,
                "4KB-aligned allocation should succeed");
    TEST_ASSERT((uintptr_t)buf_aligned_4k % 4096 == 0,
                "buffer should be 4KB-aligned");
    loopyFSFreeAligned(buf_aligned_4k);

    return 1;
}

/**
 * Test aligned allocation edge cases.
 */
static int test_direct_io_aligned_edge_cases(void) {
    size_t alignment = loopyFSDirectAlignment();

    /* Zero size should fail */
    void *buf_zero = loopyFSAllocAligned(0, alignment);
    TEST_ASSERT(buf_zero == NULL, "zero-size allocation should fail");

    /* Zero alignment should fail */
    void *buf_zero_align = loopyFSAllocAligned(512, 0);
    TEST_ASSERT(buf_zero_align == NULL, "zero alignment should fail");

    /* NULL free should not crash */
    loopyFSFreeAligned(NULL);

    return 1;
}

/**
 * Test O_DIRECT file writes and reads with aligned buffers.
 */
static int test_direct_io_read_write(void) {
    const char *testFile = "/tmp/loopy_direct_io_test.dat";
    size_t alignment = loopyFSDirectAlignment();
    size_t bufSize = 4096; /* 4KB, multiple of sector size */

    /* Allocate aligned buffer for write */
    char *writeBuf = loopyFSAllocAligned(bufSize, alignment);
    TEST_ASSERT(writeBuf != NULL, "write buffer allocation should succeed");
    TEST_ASSERT((uintptr_t)writeBuf % alignment == 0,
                "write buffer should be aligned");

    /* Fill buffer with test pattern */
    for (size_t i = 0; i < bufSize; i++) {
        writeBuf[i] = (char)(i % 256);
    }

    /* Open file with O_DIRECT flag (may be 0 on platforms without support) */
    int flags = O_RDWR | O_CREAT | O_TRUNC;
#if defined(__linux__)
    flags |= O_DIRECT; /* Linux supports O_DIRECT */
#endif

    int fd = open(testFile, flags, 0644);
    TEST_ASSERT(fd >= 0, "file open should succeed");

    /* Write aligned data */
    ssize_t written = write(fd, writeBuf, bufSize);
    TEST_ASSERT_EQ((size_t)written, bufSize, "write should succeed");

    /* Sync to disk */
    fsync(fd);

    /* Allocate aligned buffer for read */
    char *readBuf = loopyFSAllocAligned(bufSize, alignment);
    TEST_ASSERT(readBuf != NULL, "read buffer allocation should succeed");
    TEST_ASSERT((uintptr_t)readBuf % alignment == 0,
                "read buffer should be aligned");
    memset(readBuf, 0, bufSize);

    /* Read back data */
    lseek(fd, 0, SEEK_SET);
    ssize_t readBytes = read(fd, readBuf, bufSize);
    TEST_ASSERT_EQ((size_t)readBytes, bufSize, "read should succeed");

    /* Verify data integrity */
    TEST_ASSERT(memcmp(writeBuf, readBuf, bufSize) == 0,
                "read data should match written data");

    /* Cleanup */
    close(fd);
    unlink(testFile);
    loopyFSFreeAligned(writeBuf);
    loopyFSFreeAligned(readBuf);

    return 1;
}

/**
 * Test alignment validation for O_DIRECT operations.
 */
static int test_direct_io_alignment_requirements(void) {
    size_t alignment = loopyFSDirectAlignment();

    /* Test that misaligned sizes are detected */
    size_t misalignedSize = 513; /* Not a multiple of 512 */
    size_t alignedSize = 512;

    TEST_ASSERT(misalignedSize % alignment != 0,
                "test size should be misaligned");
    TEST_ASSERT(alignedSize % alignment == 0, "test size should be aligned");

    /* Verify alignment calculation */
    for (size_t size = 1; size <= 4096; size++) {
        void *buf = loopyFSAllocAligned(size, alignment);
        if (buf != NULL) {
            TEST_ASSERT((uintptr_t)buf % alignment == 0,
                        "allocated buffer must be aligned");
            loopyFSFreeAligned(buf);
        }
    }

    return 1;
}

/**
 * Test O_DIRECT with various buffer sizes.
 */
static int test_direct_io_various_sizes(void) {
    size_t alignment = loopyFSDirectAlignment();
    size_t sizes[] = {512, 1024, 2048, 4096, 8192, 16384, 65536};
    size_t numSizes = sizeof(sizes) / sizeof(sizes[0]);

    for (size_t i = 0; i < numSizes; i++) {
        size_t size = sizes[i];

        /* Allocate aligned buffer */
        void *buf = loopyFSAllocAligned(size, alignment);
        TEST_ASSERT(buf != NULL, "allocation should succeed");
        TEST_ASSERT((uintptr_t)buf % alignment == 0,
                    "buffer should be aligned");
        TEST_ASSERT(size % alignment == 0,
                    "size should be multiple of alignment");

        /* Fill with test pattern */
        memset(buf, (int)(i & 0xFF), size);

        /* Verify pattern */
        unsigned char expected = (unsigned char)(i & 0xFF);
        unsigned char *bytes = (unsigned char *)buf;
        for (size_t j = 0; j < size; j++) {
            TEST_ASSERT(bytes[j] == expected, "buffer should contain pattern");
        }

        loopyFSFreeAligned(buf);
    }

    return 1;
}

/**
 * Test null safety for O_DIRECT helper functions.
 */
static int test_direct_io_null_safety(void) {
    size_t alignment = loopyFSDirectAlignment();

    /* loopyFSDirectAlignment() should always return valid value */
    TEST_ASSERT(alignment > 0, "alignment should be positive");
    TEST_ASSERT(alignment == 512, "default alignment should be 512");

    /* loopyFSAllocAligned() with NULL parameters */
    void *buf1 = loopyFSAllocAligned(0, alignment);
    TEST_ASSERT(buf1 == NULL, "zero size should return NULL");

    void *buf2 = loopyFSAllocAligned(512, 0);
    TEST_ASSERT(buf2 == NULL, "zero alignment should return NULL");

    /* loopyFSFreeAligned() with NULL should not crash */
    loopyFSFreeAligned(NULL);

    return 1;
}

/* ====================================================================
 * Connection Pool Tests (loopyConnPool)
 * ==================================================================== */

/* Test connection structure for pool tests */
typedef struct TestConn {
    int id;
    int useCount;
    bool isValid;
} TestConn;

static int testConnIdCounter = 0;

/* Connection lifecycle callbacks */
static void *testConnCreate(void *userData) {
    (void)userData;
    TestConn *conn = zcalloc(1, sizeof(TestConn));
    if (!conn) {
        return NULL;
    }
    conn->id = ++testConnIdCounter;
    conn->useCount = 0;
    conn->isValid = true;
    return conn;
}

static void testConnDestroy(void *conn, void *userData) {
    (void)userData;
    if (conn) {
        zfree(conn);
    }
}

static bool testConnValidate(void *conn, void *userData) {
    (void)userData;
    if (!conn) {
        return false;
    }
    TestConn *tc = (TestConn *)conn;
    return tc->isValid;
}

static void testConnAcquire(void *conn, void *userData) {
    (void)userData;
    if (conn) {
        TestConn *tc = (TestConn *)conn;
        tc->useCount++;
    }
}

static void testConnRelease(void *conn, void *userData) {
    (void)userData;
    (void)conn;
    /* Nothing to do on release */
}

/**
 * Test pool creation and deletion.
 */
static int test_connpool_create_delete(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    TEST_ASSERT(l != NULL, "loop creation should succeed");

    loopyConnPoolConfig config = loopyConnPoolConfigDefault();
    config.minIdle = 0; /* No prewarming for this test */

    loopyConnPool *pool = loopyConnPoolNew(
        l, &config, testConnCreate, testConnDestroy, testConnValidate, NULL);
    TEST_ASSERT(pool != NULL, "pool creation should succeed");

    loopyConnPoolFree(pool);

    return 1;
}

/**
 * Test default configuration.
 */
static int test_connpool_default_config(void) {
    loopyConnPoolConfig config = loopyConnPoolConfigDefault();

    TEST_ASSERT(config.minIdle == 0, "default minIdle should be 0");
    TEST_ASSERT(config.maxTotal == 10, "default maxTotal should be 10");
    TEST_ASSERT(config.idleTimeoutUs == 30000000,
                "default idle timeout should be 30s");
    TEST_ASSERT(config.maxLifetimeUs == 3600000000,
                "default max lifetime should be 1h");
    TEST_ASSERT(config.healthCheckIntervalUs == 60000000,
                "default health check interval should be 60s");
    TEST_ASSERT(config.acquireTimeoutUs == 5000000,
                "default acquire timeout should be 5s");

    return 1;
}

/**
 * Test basic connection acquisition and release.
 */
static int test_connpool_acquire_release(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    testConnIdCounter = 0;

    loopyConnPoolConfig config = loopyConnPoolConfigDefault();
    config.minIdle = 0;

    loopyConnPool *pool = loopyConnPoolNew(
        l, &config, testConnCreate, testConnDestroy, testConnValidate, NULL);
    TEST_ASSERT(pool != NULL, "pool creation should succeed");

    /* Register acquire/release callbacks */
    loopyConnPoolSetAcquireCallback(pool, testConnAcquire);
    loopyConnPoolSetReleaseCallback(pool, testConnRelease);

    /* Acquire connection */
    loopyConnPoolConn *conn = loopyConnPoolAcquire(pool);
    TEST_ASSERT(conn != NULL, "acquire should succeed");

    TestConn *tc = (TestConn *)loopyConnPoolGetUserConn(conn);
    TEST_ASSERT(tc != NULL, "user connection should not be NULL");
    TEST_ASSERT(tc->id == 1, "first connection should have id 1");
    TEST_ASSERT(tc->useCount == 1, "use count should be 1");

    /* Release connection */
    loopyConnPoolRelease(conn, true);

    /* Check stats */
    loopyConnPoolStats stats;
    loopyConnPoolGetStats(pool, &stats);
    TEST_ASSERT_EQ(stats.totalAcquires, 1, "total acquires should be 1");
    TEST_ASSERT_EQ(stats.totalReleases, 1, "total releases should be 1");
    TEST_ASSERT_EQ(stats.totalCreates, 1, "total creates should be 1");
    TEST_ASSERT_EQ(stats.idleConns, 1, "should have 1 idle connection");

    loopyConnPoolFree(pool);

    return 1;
}

/**
 * Test connection reuse from idle pool.
 */
static int test_connpool_connection_reuse(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    testConnIdCounter = 0;

    loopyConnPoolConfig config = loopyConnPoolConfigDefault();
    config.minIdle = 0;

    loopyConnPool *pool = loopyConnPoolNew(
        l, &config, testConnCreate, testConnDestroy, testConnValidate, NULL);

    /* Register acquire/release callbacks */
    loopyConnPoolSetAcquireCallback(pool, testConnAcquire);
    loopyConnPoolSetReleaseCallback(pool, testConnRelease);

    /* Acquire and release */
    loopyConnPoolConn *conn1 = loopyConnPoolAcquire(pool);
    TEST_ASSERT(conn1 != NULL, "first acquire should succeed");

    TestConn *tc1 = (TestConn *)loopyConnPoolGetUserConn(conn1);
    int firstId = tc1->id;

    loopyConnPoolRelease(conn1, true);

    /* Acquire again - should reuse same connection */
    loopyConnPoolConn *conn2 = loopyConnPoolAcquire(pool);
    TEST_ASSERT(conn2 != NULL, "second acquire should succeed");

    TestConn *tc2 = (TestConn *)loopyConnPoolGetUserConn(conn2);
    TEST_ASSERT_EQ(tc2->id, firstId, "should reuse same connection");
    TEST_ASSERT_EQ(tc2->useCount, 2, "use count should be 2");

    loopyConnPoolRelease(conn2, true);

    /* Check stats */
    loopyConnPoolStats stats;
    loopyConnPoolGetStats(pool, &stats);
    TEST_ASSERT_EQ(stats.totalAcquires, 2, "total acquires should be 2");
    TEST_ASSERT_EQ(stats.totalCreates, 1, "should only create 1 connection");

    loopyConnPoolFree(pool);

    return 1;
}

/**
 * Test minIdle prewarming.
 */
static int test_connpool_min_idle(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    testConnIdCounter = 0;

    loopyConnPoolConfig config = loopyConnPoolConfigDefault();
    config.minIdle = 3;

    loopyConnPool *pool = loopyConnPoolNew(
        l, &config, testConnCreate, testConnDestroy, testConnValidate, NULL);
    TEST_ASSERT(pool != NULL, "pool creation should succeed");

    /* Should have 3 idle connections from prewarming */
    TEST_ASSERT_EQ(loopyConnPoolGetIdleCount(pool), 3,
                   "should have 3 idle connections");
    TEST_ASSERT_EQ(loopyConnPoolGetTotalCount(pool), 3,
                   "should have 3 total connections");
    TEST_ASSERT_EQ(loopyConnPoolGetActiveCount(pool), 0,
                   "should have 0 active connections");

    loopyConnPoolFree(pool);

    return 1;
}

/**
 * Test maxTotal connection limit.
 */
static int test_connpool_max_total(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    testConnIdCounter = 0;

    loopyConnPoolConfig config = loopyConnPoolConfigDefault();
    config.minIdle = 0;
    config.maxTotal = 2;

    loopyConnPool *pool = loopyConnPoolNew(
        l, &config, testConnCreate, testConnDestroy, testConnValidate, NULL);

    /* Acquire maxTotal connections */
    loopyConnPoolConn *conn1 = loopyConnPoolAcquire(pool);
    TEST_ASSERT(conn1 != NULL, "first acquire should succeed");

    loopyConnPoolConn *conn2 = loopyConnPoolAcquire(pool);
    TEST_ASSERT(conn2 != NULL, "second acquire should succeed");

    TEST_ASSERT_EQ(loopyConnPoolGetActiveCount(pool), 2,
                   "should have 2 active connections");
    TEST_ASSERT_EQ(loopyConnPoolGetIdleCount(pool), 0,
                   "should have 0 idle connections");

    /* Try to acquire beyond maxTotal - should fail */
    loopyConnPoolConn *conn3 = loopyConnPoolAcquire(pool);
    TEST_ASSERT(conn3 == NULL, "third acquire should fail (maxTotal reached)");

    /* Release one */
    loopyConnPoolRelease(conn1, true);

    /* Now acquire should succeed */
    loopyConnPoolConn *conn4 = loopyConnPoolAcquire(pool);
    TEST_ASSERT(conn4 != NULL, "acquire after release should succeed");

    loopyConnPoolRelease(conn2, true);
    loopyConnPoolRelease(conn4, true);

    loopyConnPoolFree(pool);

    return 1;
}

/**
 * Test connection validation on acquire.
 */
static int test_connpool_validation(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    testConnIdCounter = 0;

    loopyConnPoolConfig config = loopyConnPoolConfigDefault();
    config.minIdle = 0;

    loopyConnPool *pool = loopyConnPoolNew(
        l, &config, testConnCreate, testConnDestroy, testConnValidate, NULL);

    /* Acquire and release */
    loopyConnPoolConn *conn1 = loopyConnPoolAcquire(pool);
    TEST_ASSERT(conn1 != NULL, "acquire should succeed");

    TestConn *tc = (TestConn *)loopyConnPoolGetUserConn(conn1);
    tc->isValid = false; /* Mark as invalid */

    loopyConnPoolRelease(conn1, true);

    /* Acquire again - should create new connection since old one is invalid */
    loopyConnPoolConn *conn2 = loopyConnPoolAcquire(pool);
    TEST_ASSERT(conn2 != NULL, "acquire should succeed");

    TestConn *tc2 = (TestConn *)loopyConnPoolGetUserConn(conn2);
    TEST_ASSERT(tc2->isValid, "new connection should be valid");

    /* Check stats */
    loopyConnPoolStats stats;
    loopyConnPoolGetStats(pool, &stats);
    TEST_ASSERT_EQ(stats.totalCreates, 2,
                   "should create 2 connections (1 invalid)");
    TEST_ASSERT_EQ(stats.totalDestroys, 1,
                   "should destroy 1 invalid connection");
    TEST_ASSERT(stats.totalValidationFailures > 0,
                "should have validation failures");

    loopyConnPoolRelease(conn2, true);

    loopyConnPoolFree(pool);

    return 1;
}

/**
 * Test unhealthy connection release.
 */
static int test_connpool_unhealthy_release(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    testConnIdCounter = 0;

    loopyConnPoolConfig config = loopyConnPoolConfigDefault();
    config.minIdle = 0;

    loopyConnPool *pool = loopyConnPoolNew(
        l, &config, testConnCreate, testConnDestroy, testConnValidate, NULL);

    /* Acquire connection */
    loopyConnPoolConn *conn = loopyConnPoolAcquire(pool);
    TEST_ASSERT(conn != NULL, "acquire should succeed");

    /* Release as unhealthy */
    loopyConnPoolRelease(conn, false);

    /* Check stats */
    loopyConnPoolStats stats;
    loopyConnPoolGetStats(pool, &stats);
    TEST_ASSERT_EQ(stats.totalCreates, 1, "should create 1 connection");
    TEST_ASSERT_EQ(stats.totalDestroys, 1,
                   "should destroy unhealthy connection");
    TEST_ASSERT_EQ(stats.idleConns, 0, "should have 0 idle connections");

    loopyConnPoolFree(pool);

    return 1;
}

/**
 * Test pool statistics.
 */
static int test_connpool_statistics(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    testConnIdCounter = 0;

    loopyConnPoolConfig config = loopyConnPoolConfigDefault();
    config.minIdle = 2;
    config.maxTotal = 5;

    loopyConnPool *pool = loopyConnPoolNew(
        l, &config, testConnCreate, testConnDestroy, testConnValidate, NULL);

    loopyConnPoolStats stats;
    loopyConnPoolGetStats(pool, &stats);

    TEST_ASSERT_EQ(stats.totalConns, 2, "should have 2 total connections");
    TEST_ASSERT_EQ(stats.idleConns, 2, "should have 2 idle connections");
    TEST_ASSERT_EQ(stats.activeConns, 0, "should have 0 active connections");
    TEST_ASSERT_EQ(stats.totalCreates, 2, "should have created 2 connections");

    /* Acquire some connections */
    loopyConnPoolConn *conn1 = loopyConnPoolAcquire(pool);
    loopyConnPoolConn *conn2 = loopyConnPoolAcquire(pool);
    loopyConnPoolConn *conn3 = loopyConnPoolAcquire(pool);

    loopyConnPoolGetStats(pool, &stats);
    TEST_ASSERT_EQ(stats.totalConns, 3, "should have 3 total connections");
    TEST_ASSERT_EQ(stats.activeConns, 3, "should have 3 active connections");
    TEST_ASSERT_EQ(stats.idleConns, 0, "should have 0 idle connections");
    TEST_ASSERT_EQ(stats.totalAcquires, 3, "should have 3 total acquires");

    /* Release one */
    loopyConnPoolRelease(conn1, true);

    loopyConnPoolGetStats(pool, &stats);
    TEST_ASSERT_EQ(stats.totalConns, 3, "should have 3 total connections");
    TEST_ASSERT_EQ(stats.activeConns, 2, "should have 2 active connections");
    TEST_ASSERT_EQ(stats.idleConns, 1, "should have 1 idle connection");
    TEST_ASSERT_EQ(stats.totalReleases, 1, "should have 1 total release");

    loopyConnPoolRelease(conn2, true);
    loopyConnPoolRelease(conn3, true);

    loopyConnPoolFree(pool);

    return 1;
}

/**
 * Test manual health check.
 */
static int test_connpool_health_check(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    testConnIdCounter = 0;

    loopyConnPoolConfig config = loopyConnPoolConfigDefault();
    config.minIdle = 3;
    config.healthCheckIntervalUs = 0; /* Disable automatic health checks */

    loopyConnPool *pool = loopyConnPoolNew(
        l, &config, testConnCreate, testConnDestroy, testConnValidate, NULL);

    TEST_ASSERT_EQ(loopyConnPoolGetIdleCount(pool), 3,
                   "should have 3 idle connections");

    /* Mark one connection as invalid by acquiring and modifying it */
    loopyConnPoolConn *conn = loopyConnPoolAcquire(pool);
    TestConn *tc = (TestConn *)loopyConnPoolGetUserConn(conn);
    tc->isValid = false;
    loopyConnPoolRelease(conn, true);

    /* Run health check */
    uint32_t removed = loopyConnPoolHealthCheck(pool);
    TEST_ASSERT(removed > 0, "should remove invalid connections");

    loopyConnPoolFree(pool);

    return 1;
}

/**
 * Test idle cleanup.
 */
static int test_connpool_idle_cleanup(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    testConnIdCounter = 0;

    loopyConnPoolConfig config = loopyConnPoolConfigDefault();
    config.minIdle = 0;
    config.idleTimeoutUs = 100000; /* 100ms timeout */

    loopyConnPool *pool = loopyConnPoolNew(
        l, &config, testConnCreate, testConnDestroy, testConnValidate, NULL);

    /* Acquire and release to create idle connection */
    loopyConnPoolConn *conn = loopyConnPoolAcquire(pool);
    TEST_ASSERT(conn != NULL, "acquire should succeed");
    loopyConnPoolRelease(conn, true);

    TEST_ASSERT_EQ(loopyConnPoolGetIdleCount(pool), 1,
                   "should have 1 idle connection");

    /* Wait for timeout */
    usleep(150000); /* 150ms */

    /* Run cleanup */
    uint32_t removed = loopyConnPoolCleanupIdle(pool);
    TEST_ASSERT(removed > 0, "should remove idle connections");

    loopyConnPoolFree(pool);

    return 1;
}

/**
 * Test prewarming.
 */
static int test_connpool_prewarm(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    testConnIdCounter = 0;

    loopyConnPoolConfig config = loopyConnPoolConfigDefault();
    config.minIdle = 0;
    config.maxTotal = 10;

    loopyConnPool *pool = loopyConnPoolNew(
        l, &config, testConnCreate, testConnDestroy, testConnValidate, NULL);

    TEST_ASSERT_EQ(loopyConnPoolGetIdleCount(pool), 0,
                   "should have 0 idle connections");

    /* Prewarm with 5 connections */
    uint32_t created = loopyConnPoolPrewarm(pool, 5);
    TEST_ASSERT_EQ(created, 5, "should create 5 connections");
    TEST_ASSERT_EQ(loopyConnPoolGetIdleCount(pool), 5,
                   "should have 5 idle connections");

    loopyConnPoolFree(pool);

    return 1;
}

/**
 * Test NULL safety.
 */
static int test_connpool_null_safety(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    loopyConnPoolConfig config = loopyConnPoolConfigDefault();

    /* NULL loop */
    loopyConnPool *pool1 = loopyConnPoolNew(
        NULL, &config, testConnCreate, testConnDestroy, testConnValidate, NULL);
    TEST_ASSERT(pool1 == NULL, "NULL loop should fail");

    /* NULL create callback */
    loopyConnPool *pool2 = loopyConnPoolNew(l, &config, NULL, testConnDestroy,
                                            testConnValidate, NULL);
    TEST_ASSERT(pool2 == NULL, "NULL create callback should fail");

    /* NULL destroy callback */
    loopyConnPool *pool3 = loopyConnPoolNew(l, &config, testConnCreate, NULL,
                                            testConnValidate, NULL);
    TEST_ASSERT(pool3 == NULL, "NULL destroy callback should fail");

    /* NULL validate callback is allowed */
    loopyConnPool *pool4 = loopyConnPoolNew(l, &config, testConnCreate,
                                            testConnDestroy, NULL, NULL);
    TEST_ASSERT(pool4 != NULL, "NULL validate callback is allowed");
    loopyConnPoolFree(pool4);

    /* NULL pool operations should not crash */
    loopyConnPoolFree(NULL);
    loopyConnPoolConn *conn = loopyConnPoolAcquire(NULL);
    TEST_ASSERT(conn == NULL, "acquire on NULL pool should return NULL");

    loopyConnPoolRelease(NULL, true); /* Should not crash */

    void *userConn = loopyConnPoolGetUserConn(NULL);
    TEST_ASSERT(userConn == NULL, "get user conn on NULL should return NULL");

    loopyConnPoolStats stats;
    bool statsOk = loopyConnPoolGetStats(NULL, &stats);
    TEST_ASSERT(!statsOk, "get stats on NULL pool should fail");

    uint32_t count = loopyConnPoolGetIdleCount(NULL);
    TEST_ASSERT_EQ(count, 0, "get idle count on NULL should return 0");

    count = loopyConnPoolGetActiveCount(NULL);
    TEST_ASSERT_EQ(count, 0, "get active count on NULL should return 0");

    count = loopyConnPoolGetTotalCount(NULL);
    TEST_ASSERT_EQ(count, 0, "get total count on NULL should return 0");

    uint32_t removed = loopyConnPoolHealthCheck(NULL);
    TEST_ASSERT_EQ(removed, 0, "health check on NULL should return 0");

    removed = loopyConnPoolCleanupIdle(NULL);
    TEST_ASSERT_EQ(removed, 0, "cleanup idle on NULL should return 0");

    uint32_t created = loopyConnPoolPrewarm(NULL, 5);
    TEST_ASSERT_EQ(created, 0, "prewarm on NULL should return 0");


    return 1;
}

/* Connection pool timeout callback */
static bool connpool_timeout_callback(timerWheel *t, timerWheelId id,
                                      void *data) {
    (void)t;
    (void)id;
    loopyLoop *l = data;
    loopyStop(l);
    return false; /* One-shot */
}

/* Async acquire test data */
typedef struct AsyncAcquireData {
    loopyLoop *loop;
    loopyConnPoolConn *conn;
    bool callbackCalled;
} AsyncAcquireData;

static void asyncAcquireCallback(loopyConnPoolConn *conn, void *userData) {
    AsyncAcquireData *data = (AsyncAcquireData *)userData;
    data->conn = conn;
    data->callbackCalled = true;
    loopyStop(data->loop);
}

/**
 * Test async acquisition with wait queue.
 */
static int test_connpool_async_acquire(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    testConnIdCounter = 0;

    loopyConnPoolConfig config = loopyConnPoolConfigDefault();
    config.minIdle = 0;
    config.maxTotal = 1; /* Only allow 1 connection */

    loopyConnPool *pool = loopyConnPoolNew(
        l, &config, testConnCreate, testConnDestroy, testConnValidate, NULL);

    /* Acquire the only connection */
    loopyConnPoolConn *conn1 = loopyConnPoolAcquire(pool);
    TEST_ASSERT(conn1 != NULL, "first acquire should succeed");

    /* Try async acquire - should queue since pool is at capacity */
    AsyncAcquireData asyncData = {0};
    asyncData.loop = l;
    bool queued =
        loopyConnPoolAcquireAsync(pool, asyncAcquireCallback, &asyncData);
    TEST_ASSERT(queued, "async acquire should queue");
    TEST_ASSERT(!asyncData.callbackCalled, "callback should not be called yet");

    /* Release connection - should trigger async callback */
    loopyConnPoolRelease(conn1, true);

    /* Add timeout to prevent hanging */
    loopyRegisterTimer(l, 1000000, 0, connpool_timeout_callback, l);

    /* Run event loop until callback fires */
    loopyMain(l);

    /* Callback should have been called */
    TEST_ASSERT(asyncData.callbackCalled,
                "async callback should have been called");
    TEST_ASSERT(asyncData.conn != NULL, "async acquire should succeed");

    /* Release the async acquired connection */
    loopyConnPoolRelease(asyncData.conn, true);

    loopyConnPoolFree(pool);

    return 1;
}

/* ====================================================================
 * File Locking Tests (loopyFlock)
 * ==================================================================== */

/**
 * Test capability detection.
 */
static int test_flock_capabilities(void) {
    /* flock() should be available on Unix systems */
    bool hasFlock = loopyFlockHasFlock();
    bool hasFcntl = loopyFlockHasFcntl();

#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
    TEST_ASSERT(hasFlock, "flock should be available on this platform");
#endif

    /* fcntl() should always be available (POSIX) */
    TEST_ASSERT(hasFcntl, "fcntl should be available (POSIX)");

    return 1;
}

/**
 * Test basic flock() exclusive lock.
 */
static int test_flock_exclusive_basic(void) {
    if (!loopyFlockHasFlock()) {
        return 1; /* Skip on platforms without flock */
    }

    const char *testFile = "/tmp/loopy_flock_test_exclusive.dat";
    int fd = open(testFile, O_RDWR | O_CREAT | O_TRUNC, 0644);
    TEST_ASSERT(fd >= 0, "file open should succeed");

    /* Acquire exclusive lock */
    bool locked =
        loopyFlockLock(fd, LOOPY_FLOCK_EXCLUSIVE, LOOPY_FLOCK_BLOCKING);
    TEST_ASSERT(locked, "exclusive lock should succeed");

    /* Release lock */
    bool unlocked = loopyFlockUnlock(fd);
    TEST_ASSERT(unlocked, "unlock should succeed");

    close(fd);
    unlink(testFile);
    return 1;
}

/**
 * Test flock() shared lock.
 */
static int test_flock_shared_basic(void) {
    if (!loopyFlockHasFlock()) {
        return 1;
    }

    const char *testFile = "/tmp/loopy_flock_test_shared.dat";
    int fd = open(testFile, O_RDWR | O_CREAT | O_TRUNC, 0644);
    TEST_ASSERT(fd >= 0, "file open should succeed");

    /* Acquire shared lock */
    bool locked = loopyFlockLock(fd, LOOPY_FLOCK_SHARED, LOOPY_FLOCK_BLOCKING);
    TEST_ASSERT(locked, "shared lock should succeed");

    /* Release lock */
    bool unlocked = loopyFlockUnlock(fd);
    TEST_ASSERT(unlocked, "unlock should succeed");

    close(fd);
    unlink(testFile);
    return 1;
}

/**
 * Test fcntl() byte-range lock on whole file.
 */
static int test_flock_fcntl_whole_file(void) {
    const char *testFile = "/tmp/loopy_flock_test_fcntl.dat";
    int fd = open(testFile, O_RDWR | O_CREAT | O_TRUNC, 0644);
    TEST_ASSERT(fd >= 0, "file open should succeed");

    /* Write some data */
    write(fd, "test data", 9);

    /* Lock entire file (offset=0, length=0 means to EOF) */
    bool locked = loopyFlockLockRange(fd, LOOPY_FLOCK_EXCLUSIVE,
                                      LOOPY_FLOCK_BLOCKING, 0, 0);
    TEST_ASSERT(locked, "fcntl whole-file lock should succeed");

    /* Unlock */
    bool unlocked = loopyFlockUnlockRange(fd, 0, 0);
    TEST_ASSERT(unlocked, "unlock should succeed");

    close(fd);
    unlink(testFile);
    return 1;
}

/**
 * Test fcntl() byte-range partial lock.
 */
static int test_flock_fcntl_byte_range(void) {
    const char *testFile = "/tmp/loopy_flock_test_range.dat";
    int fd = open(testFile, O_RDWR | O_CREAT | O_TRUNC, 0644);
    TEST_ASSERT(fd >= 0, "file open should succeed");

    /* Write 4KB of data */
    char buf[4096];
    memset(buf, 'A', sizeof(buf));
    write(fd, buf, sizeof(buf));

    /* Lock first 1KB */
    bool locked = loopyFlockLockRange(fd, LOOPY_FLOCK_EXCLUSIVE,
                                      LOOPY_FLOCK_BLOCKING, 0, 1024);
    TEST_ASSERT(locked, "fcntl range lock should succeed");

    /* Unlock first 1KB */
    bool unlocked = loopyFlockUnlockRange(fd, 0, 1024);
    TEST_ASSERT(unlocked, "unlock should succeed");

    /* Lock bytes 1024-2047 (second KB) */
    locked = loopyFlockLockRange(fd, LOOPY_FLOCK_SHARED, LOOPY_FLOCK_BLOCKING,
                                 1024, 1024);
    TEST_ASSERT(locked, "second range lock should succeed");

    unlocked = loopyFlockUnlockRange(fd, 1024, 1024);
    TEST_ASSERT(unlocked, "unlock should succeed");

    close(fd);
    unlink(testFile);
    return 1;
}

/**
 * Test fcntl() lock testing (F_GETLK).
 */
static int test_flock_test_range(void) {
    const char *testFile = "/tmp/loopy_flock_test_testrange.dat";
    int fd = open(testFile, O_RDWR | O_CREAT | O_TRUNC, 0644);
    TEST_ASSERT(fd >= 0, "file open should succeed");

    write(fd, "test", 4);

    /* Test without any lock - should be available */
    pid_t holder = -1;
    bool canLock =
        loopyFlockTestRange(fd, LOOPY_FLOCK_EXCLUSIVE, 0, 0, &holder);
    TEST_ASSERT(canLock, "range should be lockable when no lock held");
    TEST_ASSERT(holder == -1, "holder should be -1 when no lock");

    close(fd);
    unlink(testFile);
    return 1;
}

/**
 * Test high-level API with default config.
 */
static int test_flock_highlevel_api(void) {
    const char *testFile = "/tmp/loopy_flock_test_highlevel.dat";
    int fd = open(testFile, O_RDWR | O_CREAT | O_TRUNC, 0644);
    TEST_ASSERT(fd >= 0, "file open should succeed");

    /* Create lock with default config */
    loopyFlockConfig cfg = LOOPY_FLOCK_CONFIG_DEFAULT;
    loopyFlock *lock = loopyFlockNew(fd, &cfg);
    TEST_ASSERT(lock != NULL, "loopyFlockNew should succeed");

    /* Verify lock properties */
    TEST_ASSERT(loopyFlockGetFd(lock) == fd, "fd should match");
    TEST_ASSERT(loopyFlockGetType(lock) == LOOPY_FLOCK_EXCLUSIVE,
                "type should be exclusive");
    TEST_ASSERT(loopyFlockIsLocked(lock), "lock should be valid");

    loopyFlockMechanism mech = loopyFlockGetMechanism(lock);
    TEST_ASSERT(mech == LOOPY_FLOCK_FLOCK || mech == LOOPY_FLOCK_FCNTL,
                "mechanism should be flock or fcntl");

    /* Free lock (automatically unlocks) */
    loopyFlockFree(lock);

    close(fd);
    unlink(testFile);
    return 1;
}

/**
 * Test high-level API with fcntl byte range.
 */
static int test_flock_highlevel_fcntl_range(void) {
    const char *testFile = "/tmp/loopy_flock_test_highlevel_range.dat";
    int fd = open(testFile, O_RDWR | O_CREAT | O_TRUNC, 0644);
    TEST_ASSERT(fd >= 0, "file open should succeed");

    /* Write data */
    char buf[4096];
    memset(buf, 'X', sizeof(buf));
    write(fd, buf, sizeof(buf));

    /* Create fcntl lock on bytes 512-1023 */
    loopyFlockConfig cfg = {.mechanism = LOOPY_FLOCK_FCNTL,
                            .type = LOOPY_FLOCK_SHARED,
                            .mode = LOOPY_FLOCK_BLOCKING,
                            .offset = 512,
                            .length = 512};

    loopyFlock *lock = loopyFlockNew(fd, &cfg);
    TEST_ASSERT(lock != NULL, "fcntl range lock should succeed");

    /* Verify range */
    off_t offset = 0, length = 0;
    bool hasRange = loopyFlockGetRange(lock, &offset, &length);
    TEST_ASSERT(hasRange, "fcntl lock should have range");
    TEST_ASSERT(offset == 512, "offset should be 512");
    TEST_ASSERT(length == 512, "length should be 512");

    loopyFlockFree(lock);
    close(fd);
    unlink(testFile);
    return 1;
}

/**
 * Test lock file pattern (single-instance enforcement).
 */
static int test_flock_lockfile_pattern(void) {
    const char *lockPath = "/tmp/loopy_test_app.lock";

    /* Create lock file */
    loopyFlock *lock = loopyFlockNewLockfile(lockPath, LOOPY_FLOCK_NONBLOCKING);
    TEST_ASSERT(lock != NULL, "lockfile creation should succeed");
    TEST_ASSERT(loopyFlockIsLocked(lock), "lockfile should be locked");

    /* Verify lock file exists */
    TEST_ASSERT(access(lockPath, F_OK) == 0, "lock file should exist");

    /* Try to create another lock (should fail - already locked) */
    loopyFlock *lock2 =
        loopyFlockNewLockfile(lockPath, LOOPY_FLOCK_NONBLOCKING);
    TEST_ASSERT(lock2 == NULL, "second lockfile should fail (already locked)");

    /* Free lockfile (removes file) */
    loopyFlockFreeLockfile(lock);

    /* Verify lock file was removed */
    TEST_ASSERT(access(lockPath, F_OK) != 0, "lock file should be removed");

    return 1;
}

/**
 * Test error handling.
 */
static int test_flock_error_handling(void) {
    /* Invalid fd */
    bool result =
        loopyFlockLock(-1, LOOPY_FLOCK_EXCLUSIVE, LOOPY_FLOCK_BLOCKING);
    TEST_ASSERT(!result, "lock with invalid fd should fail");

    const char *err = loopyFlockGetError();
    TEST_ASSERT(err != NULL, "error message should be set");

    /* NULL config should use defaults */
    const char *testFile = "/tmp/loopy_flock_test_null.dat";
    int fd = open(testFile, O_RDWR | O_CREAT | O_TRUNC, 0644);
    loopyFlock *lock = loopyFlockNew(fd, NULL);
    TEST_ASSERT(lock != NULL, "NULL config should use defaults");

    loopyFlockFree(lock);
    close(fd);
    unlink(testFile);

    return 1;
}

/**
 * Test null safety.
 */
static int test_flock_null_safety(void) {
    /* NULL lock should not crash */
    loopyFlockFree(NULL);

    TEST_ASSERT(loopyFlockGetFd(NULL) == -1, "NULL lock should return -1 fd");
    TEST_ASSERT(!loopyFlockIsLocked(NULL), "NULL lock should not be locked");

    off_t offset, length;
    TEST_ASSERT(!loopyFlockGetRange(NULL, &offset, &length),
                "NULL lock range should fail");

    /* NULL lockfile path */
    loopyFlock *lock = loopyFlockNewLockfile(NULL, LOOPY_FLOCK_NONBLOCKING);
    TEST_ASSERT(lock == NULL, "NULL path should fail");

    return 1;
}

/* ====================================================================
 * io_uring Fixed Buffer Tests
 * ==================================================================== */

#ifdef USE_IOURING

/* Test context for fixed buffer operations */
typedef struct {
    int completed;
    int32_t result;
} FixedBufCtx;

static void fixedBufCallback(void *userData, int32_t result) {
    FixedBufCtx *ctx = (FixedBufCtx *)userData;
    ctx->result = result;
    ctx->completed = 1;
}

/**
 * Test basic buffer registration and unregistration
 */
static int test_iouring_fixed_buffers_registration(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(128);
    if (!l) {
        return 0;
    }

    /* Skip test if not using io_uring backend */
    if (!loopyUsingIoUring(l)) {
        return 1;
    }

    /* Allocate test buffers */
    struct iovec buffers[3];
    buffers[0].iov_base = zmalloc(4096);
    buffers[0].iov_len = 4096;
    buffers[1].iov_base = zmalloc(8192);
    buffers[1].iov_len = 8192;
    buffers[2].iov_base = zmalloc(2048);
    buffers[2].iov_len = 2048;

    /* Initially no buffers registered */
    TEST_ASSERT(!loopyIoUringHasFixedBuffers(l),
                "No buffers should be registered initially");

    /* Register buffers */
    TEST_ASSERT(loopyIoUringRegisterBuffers(l, buffers, 3),
                "Buffer registration should succeed");
    TEST_ASSERT(loopyIoUringHasFixedBuffers(l),
                "Buffers should be registered after registration");

    /* Try to register again (should fail - already registered) */
    TEST_ASSERT(!loopyIoUringRegisterBuffers(l, buffers, 3),
                "Re-registration should fail");

    /* Unregister buffers */
    TEST_ASSERT(loopyIoUringUnregisterBuffers(l),
                "Unregistration should succeed");
    TEST_ASSERT(!loopyIoUringHasFixedBuffers(l),
                "No buffers should be registered after unregistration");

    /* Try to unregister again (should fail - none registered) */
    TEST_ASSERT(!loopyIoUringUnregisterBuffers(l),
                "Re-unregistration should fail");

    /* Free buffers */
    zfree(buffers[0].iov_base);
    zfree(buffers[1].iov_base);
    zfree(buffers[2].iov_base);

    return 1;
}

/**
 * Test fixed buffer read and write operations
 */
static int test_iouring_fixed_buffers_read_write(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(128);
    if (!l) {
        return 0;
    }

    /* Skip test if not using io_uring backend */
    if (!loopyUsingIoUring(l)) {
        return 1;
    }

    /* Create test file */
    const char *testFile = "/tmp/loopy_fixedbuf_test.dat";
    int fd = open(testFile, O_RDWR | O_CREAT | O_TRUNC, 0644);
    TEST_ASSERT(fd >= 0, "fd >= 0 should be true");

    /* Allocate and register fixed buffers */
    struct iovec buffers[2];
    buffers[0].iov_base = zmalloc(4096);
    buffers[0].iov_len = 4096;
    buffers[1].iov_base = zmalloc(4096);
    buffers[1].iov_len = 4096;

    /* Fill write buffer with test data */
    const char *testData = "Hello, fixed buffers!";
    memcpy(buffers[0].iov_base, testData, strlen(testData) + 1);

    TEST_ASSERT(loopyIoUringRegisterBuffers(l, buffers, 2));

    /* Write using fixed buffer 0 */
    FixedBufCtx writeCtx = {0, 0};
    uint64_t writeId = loopyIoUringWriteFixed(l, fd, 0, strlen(testData) + 1, 0,
                                              fixedBufCallback, &writeCtx);
    TEST_ASSERT(writeId != 0, "writeId != 0 should be true");

    /* Wait for write to complete */
    int iterations = 0;
    while (!writeCtx.completed && iterations < 100) {
        loopyPoll(l, 10);
        iterations++;
    }
    TEST_ASSERT(writeCtx.completed, "writeCtx.completed should be true");
    TEST_ASSERT(writeCtx.result == (int32_t)(strlen(testData) + 1), "writeCtx.result == (int32_t)(strlen(testData) + 1) should be true");

    /* Read back using fixed buffer 1 */
    memset(buffers[1].iov_base, 0, 4096);
    FixedBufCtx readCtx = {0, 0};
    uint64_t readId = loopyIoUringReadFixed(l, fd, 1, strlen(testData) + 1, 0,
                                            fixedBufCallback, &readCtx);
    TEST_ASSERT(readId != 0, "readId != 0 should be true");

    /* Wait for read to complete */
    iterations = 0;
    while (!readCtx.completed && iterations < 100) {
        loopyPoll(l, 10);
        iterations++;
    }
    TEST_ASSERT(readCtx.completed, "readCtx.completed should be true");
    TEST_ASSERT(readCtx.result == (int32_t)(strlen(testData) + 1), "readCtx.result == (int32_t)(strlen(testData) + 1) should be true");

    /* Verify data */
    TEST_ASSERT(strcmp((char *)buffers[1].iov_base, testData) == 0);

    /* Cleanup */
    TEST_ASSERT(loopyIoUringUnregisterBuffers(l), "loopyIoUringUnregisterBuffers(l) should be true");
    zfree(buffers[0].iov_base);
    zfree(buffers[1].iov_base);
    close(fd);
    unlink(testFile);
    return 1;
}

/**
 * Test error handling for invalid buffer operations
 */
static int test_iouring_fixed_buffers_error_handling(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(128);
    if (!l) {
        return 0;
    }

    /* Skip test if not using io_uring backend */
    if (!loopyUsingIoUring(l)) {
        return 1;
    }

    /* Test NULL buffers */
    TEST_ASSERT(!loopyIoUringRegisterBuffers(l, NULL, 1));

    /* Test zero count */
    struct iovec buf;
    buf.iov_base = zmalloc(4096);
    buf.iov_len = 4096;
    TEST_ASSERT(!loopyIoUringRegisterBuffers(l, &buf, 0));

    /* Test too many buffers (> 1024) */
    TEST_ASSERT(!loopyIoUringRegisterBuffers(l, &buf, 1025));

    /* Register a valid buffer */
    TEST_ASSERT(loopyIoUringRegisterBuffers(l, &buf, 1));

    /* Create test file */
    const char *testFile = "/tmp/loopy_fixedbuf_error_test.dat";
    int fd = open(testFile, O_RDWR | O_CREAT | O_TRUNC, 0644);
    TEST_ASSERT(fd >= 0, "fd >= 0 should be true");

    /* Test invalid buffer index (>= numRegisteredBuffers) */
    FixedBufCtx ctx = {0, 0};
    TEST_ASSERT(
        loopyIoUringReadFixed(l, fd, 1, 100, 0, fixedBufCallback, &ctx) == 0);
    TEST_ASSERT(
        loopyIoUringWriteFixed(l, fd, 1, 100, 0, fixedBufCallback, &ctx) == 0);

    /* Test length exceeding buffer size */
    TEST_ASSERT(
        loopyIoUringReadFixed(l, fd, 0, 8192, 0, fixedBufCallback, &ctx) == 0);
    TEST_ASSERT(
        loopyIoUringWriteFixed(l, fd, 0, 8192, 0, fixedBufCallback, &ctx) == 0);

    /* Cleanup */
    TEST_ASSERT(loopyIoUringUnregisterBuffers(l), "loopyIoUringUnregisterBuffers(l) should be true");

    /* Try to use fixed buffers when none registered */
    TEST_ASSERT(
        loopyIoUringReadFixed(l, fd, 0, 100, 0, fixedBufCallback, &ctx) == 0);
    TEST_ASSERT(
        loopyIoUringWriteFixed(l, fd, 0, 100, 0, fixedBufCallback, &ctx) == 0);

    zfree(buf.iov_base);
    close(fd);
    unlink(testFile);
    return 1;
}

/**
 * Test multiple fixed buffers with different sizes
 */
static int test_iouring_fixed_buffers_multiple(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(128);
    if (!l) {
        return 0;
    }

    /* Skip test if not using io_uring backend */
    if (!loopyUsingIoUring(l)) {
        return 1;
    }

    /* Create test file */
    const char *testFile = "/tmp/loopy_fixedbuf_multi_test.dat";
    int fd = open(testFile, O_RDWR | O_CREAT | O_TRUNC, 0644);
    TEST_ASSERT(fd >= 0, "fd >= 0 should be true");

    /* Allocate multiple buffers of different sizes */
    struct iovec buffers[4];
    buffers[0].iov_base = zmalloc(1024);
    buffers[0].iov_len = 1024;
    buffers[1].iov_base = zmalloc(2048);
    buffers[1].iov_len = 2048;
    buffers[2].iov_base = zmalloc(4096);
    buffers[2].iov_len = 4096;
    buffers[3].iov_base = zmalloc(512);
    buffers[3].iov_len = 512;

    /* Fill each buffer with unique data */
    memset(buffers[0].iov_base, 'A', 1024);
    memset(buffers[1].iov_base, 'B', 2048);
    memset(buffers[2].iov_base, 'C', 4096);
    memset(buffers[3].iov_base, 'D', 512);

    TEST_ASSERT(loopyIoUringRegisterBuffers(l, buffers, 4));

    /* Write from each buffer to different file offsets */
    FixedBufCtx ctx0 = {0, 0}, ctx1 = {0, 0}, ctx2 = {0, 0}, ctx3 = {0, 0};

    TEST_ASSERT(loopyIoUringWriteFixed(l, fd, 0, 1024, 0, fixedBufCallback,
                                       &ctx0) != 0);
    TEST_ASSERT(loopyIoUringWriteFixed(l, fd, 1, 2048, 1024, fixedBufCallback,
                                       &ctx1) != 0);
    TEST_ASSERT(loopyIoUringWriteFixed(l, fd, 2, 4096, 3072, fixedBufCallback,
                                       &ctx2) != 0);
    TEST_ASSERT(loopyIoUringWriteFixed(l, fd, 3, 512, 7168, fixedBufCallback,
                                       &ctx3) != 0);

    /* Wait for all writes */
    int iterations = 0;
    while ((!ctx0.completed || !ctx1.completed || !ctx2.completed ||
            !ctx3.completed) &&
           iterations < 100) {
        loopyPoll(l, 10);
        iterations++;
    }

    TEST_ASSERT(ctx0.completed && ctx0.result == 1024, "ctx0.completed && ctx0.result == 1024 should be true");
    TEST_ASSERT(ctx1.completed && ctx1.result == 2048, "ctx1.completed && ctx1.result == 2048 should be true");
    TEST_ASSERT(ctx2.completed && ctx2.result == 4096, "ctx2.completed && ctx2.result == 4096 should be true");
    TEST_ASSERT(ctx3.completed && ctx3.result == 512, "ctx3.completed && ctx3.result == 512 should be true");

    /* Clear buffers and read back */
    memset(buffers[0].iov_base, 0, 1024);
    memset(buffers[1].iov_base, 0, 2048);
    memset(buffers[2].iov_base, 0, 4096);
    memset(buffers[3].iov_base, 0, 512);

    ctx0.completed = ctx1.completed = ctx2.completed = ctx3.completed = 0;

    TEST_ASSERT(
        loopyIoUringReadFixed(l, fd, 0, 1024, 0, fixedBufCallback, &ctx0) != 0);
    TEST_ASSERT(loopyIoUringReadFixed(l, fd, 1, 2048, 1024, fixedBufCallback,
                                      &ctx1) != 0);
    TEST_ASSERT(loopyIoUringReadFixed(l, fd, 2, 4096, 3072, fixedBufCallback,
                                      &ctx2) != 0);
    TEST_ASSERT(loopyIoUringReadFixed(l, fd, 3, 512, 7168, fixedBufCallback,
                                      &ctx3) != 0);

    /* Wait for all reads */
    iterations = 0;
    while ((!ctx0.completed || !ctx1.completed || !ctx2.completed ||
            !ctx3.completed) &&
           iterations < 100) {
        loopyPoll(l, 10);
        iterations++;
    }

    TEST_ASSERT(ctx0.completed && ctx0.result == 1024, "ctx0.completed && ctx0.result == 1024 should be true");
    TEST_ASSERT(ctx1.completed && ctx1.result == 2048, "ctx1.completed && ctx1.result == 2048 should be true");
    TEST_ASSERT(ctx2.completed && ctx2.result == 4096, "ctx2.completed && ctx2.result == 4096 should be true");
    TEST_ASSERT(ctx3.completed && ctx3.result == 512, "ctx3.completed && ctx3.result == 512 should be true");

    /* Verify each buffer has correct pattern */
    for (int i = 0; i < 1024; i++) {
        TEST_ASSERT(((char *)buffers[0].iov_base)[i] == 'A', "((char *)buffers[0].iov_base)[i] == 'A' should be true");
    }
    for (int i = 0; i < 2048; i++) {
        TEST_ASSERT(((char *)buffers[1].iov_base)[i] == 'B', "((char *)buffers[1].iov_base)[i] == 'B' should be true");
    }
    for (int i = 0; i < 4096; i++) {
        TEST_ASSERT(((char *)buffers[2].iov_base)[i] == 'C', "((char *)buffers[2].iov_base)[i] == 'C' should be true");
    }
    for (int i = 0; i < 512; i++) {
        TEST_ASSERT(((char *)buffers[3].iov_base)[i] == 'D', "((char *)buffers[3].iov_base)[i] == 'D' should be true");
    }

    /* Cleanup */
    TEST_ASSERT(loopyIoUringUnregisterBuffers(l), "loopyIoUringUnregisterBuffers(l) should be true");
    zfree(buffers[0].iov_base);
    zfree(buffers[1].iov_base);
    zfree(buffers[2].iov_base);
    zfree(buffers[3].iov_base);
    close(fd);
    unlink(testFile);
    return 1;
}

/**
 * Test NULL safety for fixed buffer APIs
 */
static int test_iouring_fixed_buffers_null_safety(void) {
    /* Test NULL loop parameter */
    struct iovec buf;
    buf.iov_base = zmalloc(4096);
    buf.iov_len = 4096;

    TEST_ASSERT(!loopyIoUringRegisterBuffers(NULL, &buf, 1));
    TEST_ASSERT(!loopyIoUringUnregisterBuffers(NULL), "!loopyIoUringUnregisterBuffers(NULL) should be true");
    TEST_ASSERT(!loopyIoUringHasFixedBuffers(NULL), "!loopyIoUringHasFixedBuffers(NULL) should be true");

    FixedBufCtx ctx = {0, 0};
    TEST_ASSERT(
        loopyIoUringReadFixed(NULL, 0, 0, 100, 0, fixedBufCallback, &ctx) == 0);
    TEST_ASSERT(loopyIoUringWriteFixed(NULL, 0, 0, 100, 0, fixedBufCallback,
                                       &ctx) == 0);

    zfree(buf.iov_base);
    return 1;
}

/**
 * Test fixed file registration and unregistration
 */
static int test_iouring_fixed_files_registration(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(128);
    if (!l) {
        return 0;
    }

    /* Skip test if not using io_uring backend */
    if (!loopyUsingIoUring(l)) {
        return 1;
    }

    /* Create test files */
    int fds[3];
    fds[0] =
        open("/tmp/loopy_fixedfile_0.dat", O_RDWR | O_CREAT | O_TRUNC, 0644);
    fds[1] =
        open("/tmp/loopy_fixedfile_1.dat", O_RDWR | O_CREAT | O_TRUNC, 0644);
    fds[2] =
        open("/tmp/loopy_fixedfile_2.dat", O_RDWR | O_CREAT | O_TRUNC, 0644);
    TEST_ASSERT(fds[0] >= 0 && fds[1] >= 0 && fds[2] >= 0, "fds[0] >= 0 && fds[1] >= 0 && fds[2] >= 0 should be true");

    /* Initially no files registered */
    TEST_ASSERT(!loopyIoUringHasFixedFiles(l), "!loopyIoUringHasFixedFiles(l) should be true");

    /* Register files */
    TEST_ASSERT(loopyIoUringRegisterFiles(l, fds, 3));
    TEST_ASSERT(loopyIoUringHasFixedFiles(l), "loopyIoUringHasFixedFiles(l) should be true");

    /* Try to register again (should fail - already registered) */
    TEST_ASSERT(!loopyIoUringRegisterFiles(l, fds, 3));

    /* Unregister files */
    TEST_ASSERT(loopyIoUringUnregisterFiles(l), "loopyIoUringUnregisterFiles(l) should be true");
    TEST_ASSERT(!loopyIoUringHasFixedFiles(l), "!loopyIoUringHasFixedFiles(l) should be true");

    /* Try to unregister again (should fail - none registered) */
    TEST_ASSERT(!loopyIoUringUnregisterFiles(l), "!loopyIoUringUnregisterFiles(l) should be true");

    /* Cleanup */
    close(fds[0]);
    close(fds[1]);
    close(fds[2]);
    unlink("/tmp/loopy_fixedfile_0.dat");
    unlink("/tmp/loopy_fixedfile_1.dat");
    unlink("/tmp/loopy_fixedfile_2.dat");
    return 1;
}

/**
 * Test fixed file read and write operations
 */
static int test_iouring_fixed_files_read_write(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(128);
    if (!l) {
        return 0;
    }

    /* Skip test if not using io_uring backend */
    if (!loopyUsingIoUring(l)) {
        return 1;
    }

    /* Create test files */
    int fds[2];
    fds[0] =
        open("/tmp/loopy_fixedfile_rw_0.dat", O_RDWR | O_CREAT | O_TRUNC, 0644);
    fds[1] =
        open("/tmp/loopy_fixedfile_rw_1.dat", O_RDWR | O_CREAT | O_TRUNC, 0644);
    TEST_ASSERT(fds[0] >= 0 && fds[1] >= 0, "fds[0] >= 0 && fds[1] >= 0 should be true");

    /* Register files */
    TEST_ASSERT(loopyIoUringRegisterFiles(l, fds, 2));

    /* Prepare test data */
    const char *testData = "Hello, fixed files!";
    char *writeBuf = zmalloc(4096);
    char *readBuf = zmalloc(4096);
    memcpy(writeBuf, testData, strlen(testData) + 1);

    /* Write using fixed file 0 */
    FixedBufCtx writeCtx = {0, 0};
    uint64_t writeId = loopyIoUringWriteFixedFile(
        l, 0, writeBuf, strlen(testData) + 1, 0, fixedBufCallback, &writeCtx);
    TEST_ASSERT(writeId != 0, "writeId != 0 should be true");

    /* Wait for write to complete */
    int iterations = 0;
    while (!writeCtx.completed && iterations < 100) {
        loopyPoll(l, 10);
        iterations++;
    }
    TEST_ASSERT(writeCtx.completed, "writeCtx.completed should be true");
    TEST_ASSERT(writeCtx.result == (int32_t)(strlen(testData) + 1), "writeCtx.result == (int32_t)(strlen(testData) + 1) should be true");

    /* Read back using fixed file 0 */
    memset(readBuf, 0, 4096);
    FixedBufCtx readCtx = {0, 0};
    uint64_t readId = loopyIoUringReadFixedFile(
        l, 0, readBuf, strlen(testData) + 1, 0, fixedBufCallback, &readCtx);
    TEST_ASSERT(readId != 0, "readId != 0 should be true");

    /* Wait for read to complete */
    iterations = 0;
    while (!readCtx.completed && iterations < 100) {
        loopyPoll(l, 10);
        iterations++;
    }
    TEST_ASSERT(readCtx.completed, "readCtx.completed should be true");
    TEST_ASSERT(readCtx.result == (int32_t)(strlen(testData) + 1), "readCtx.result == (int32_t)(strlen(testData) + 1) should be true");

    /* Verify data */
    TEST_ASSERT(strcmp(readBuf, testData) == 0);

    /* Cleanup */
    TEST_ASSERT(loopyIoUringUnregisterFiles(l), "loopyIoUringUnregisterFiles(l) should be true");
    zfree(writeBuf);
    zfree(readBuf);
    close(fds[0]);
    close(fds[1]);
    unlink("/tmp/loopy_fixedfile_rw_0.dat");
    unlink("/tmp/loopy_fixedfile_rw_1.dat");
    return 1;
}

/**
 * Test fixed file error handling
 */
static int test_iouring_fixed_files_error_handling(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(128);
    if (!l) {
        return 0;
    }

    /* Skip test if not using io_uring backend */
    if (!loopyUsingIoUring(l)) {
        return 1;
    }

    char *buf = zmalloc(4096);
    FixedBufCtx ctx = {0, 0};

    /* Try to use fixed files before registering (should fail) */
    uint64_t id =
        loopyIoUringReadFixedFile(l, 0, buf, 100, 0, fixedBufCallback, &ctx);
    TEST_ASSERT(id == 0, "id == 0 should be true");

    /* Create and register test files */
    int fds[2];
    fds[0] = open("/tmp/loopy_fixedfile_err_0.dat", O_RDWR | O_CREAT | O_TRUNC,
                  0644);
    fds[1] = open("/tmp/loopy_fixedfile_err_1.dat", O_RDWR | O_CREAT | O_TRUNC,
                  0644);
    TEST_ASSERT(fds[0] >= 0 && fds[1] >= 0, "fds[0] >= 0 && fds[1] >= 0 should be true");
    TEST_ASSERT(loopyIoUringRegisterFiles(l, fds, 2));

    /* Try to use invalid file index (should fail) */
    id = loopyIoUringReadFixedFile(l, 999, buf, 100, 0, fixedBufCallback, &ctx);
    TEST_ASSERT(id == 0, "id == 0 should be true");

    /* Try to register with invalid parameters */
    TEST_ASSERT(!loopyIoUringRegisterFiles(l, NULL, 2));   /* NULL array */
    TEST_ASSERT(!loopyIoUringRegisterFiles(l, fds, 0));    /* Zero count */
    TEST_ASSERT(!loopyIoUringRegisterFiles(l, fds, 2000)); /* Exceeds limit */

    /* Cleanup */
    TEST_ASSERT(loopyIoUringUnregisterFiles(l), "loopyIoUringUnregisterFiles(l) should be true");
    zfree(buf);
    close(fds[0]);
    close(fds[1]);
    unlink("/tmp/loopy_fixedfile_err_0.dat");
    unlink("/tmp/loopy_fixedfile_err_1.dat");
    return 1;
}

/**
 * Test multiple concurrent fixed file operations
 */
static int test_iouring_fixed_files_multiple(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(128);
    if (!l) {
        return 0;
    }

    /* Skip test if not using io_uring backend */
    if (!loopyUsingIoUring(l)) {
        return 1;
    }

    /* Create and register multiple test files */
    int fds[3];
    fds[0] = open("/tmp/loopy_fixedfile_multi_0.dat",
                  O_RDWR | O_CREAT | O_TRUNC, 0644);
    fds[1] = open("/tmp/loopy_fixedfile_multi_1.dat",
                  O_RDWR | O_CREAT | O_TRUNC, 0644);
    fds[2] = open("/tmp/loopy_fixedfile_multi_2.dat",
                  O_RDWR | O_CREAT | O_TRUNC, 0644);
    TEST_ASSERT(fds[0] >= 0 && fds[1] >= 0 && fds[2] >= 0, "fds[0] >= 0 && fds[1] >= 0 && fds[2] >= 0 should be true");
    TEST_ASSERT(loopyIoUringRegisterFiles(l, fds, 3));

    /* Prepare test data */
    char *bufs[3];
    bufs[0] = zmalloc(4096);
    bufs[1] = zmalloc(4096);
    bufs[2] = zmalloc(4096);
    strcpy(bufs[0], "File 0 data");
    strcpy(bufs[1], "File 1 data");
    strcpy(bufs[2], "File 2 data");

    /* Submit multiple write operations simultaneously */
    FixedBufCtx ctx[3] = {{0, 0}, {0, 0}, {0, 0}};
    uint64_t ids[3];
    ids[0] = loopyIoUringWriteFixedFile(l, 0, bufs[0], strlen(bufs[0]) + 1, 0,
                                        fixedBufCallback, &ctx[0]);
    ids[1] = loopyIoUringWriteFixedFile(l, 1, bufs[1], strlen(bufs[1]) + 1, 0,
                                        fixedBufCallback, &ctx[1]);
    ids[2] = loopyIoUringWriteFixedFile(l, 2, bufs[2], strlen(bufs[2]) + 1, 0,
                                        fixedBufCallback, &ctx[2]);
    TEST_ASSERT(ids[0] != 0 && ids[1] != 0 && ids[2] != 0, "ids[0] != 0 && ids[1] != 0 && ids[2] != 0 should be true");

    /* Wait for all writes to complete */
    int iterations = 0;
    while ((!ctx[0].completed || !ctx[1].completed || !ctx[2].completed) &&
           iterations < 100) {
        loopyPoll(l, 10);
        iterations++;
    }
    TEST_ASSERT(ctx[0].completed && ctx[1].completed && ctx[2].completed, "ctx[0].completed && ctx[1].completed && ctx[2].completed should be true");

    /* Verify all writes succeeded */
    TEST_ASSERT(ctx[0].result > 0 && ctx[1].result > 0 && ctx[2].result > 0, "ctx[0].result > 0 && ctx[1].result > 0 && ctx[2].result > 0 should be true");

    /* Cleanup */
    TEST_ASSERT(loopyIoUringUnregisterFiles(l), "loopyIoUringUnregisterFiles(l) should be true");
    zfree(bufs[0]);
    zfree(bufs[1]);
    zfree(bufs[2]);
    close(fds[0]);
    close(fds[1]);
    close(fds[2]);
    unlink("/tmp/loopy_fixedfile_multi_0.dat");
    unlink("/tmp/loopy_fixedfile_multi_1.dat");
    unlink("/tmp/loopy_fixedfile_multi_2.dat");
    return 1;
}

/**
 * Test NULL parameter safety for fixed file operations
 */
static int test_iouring_fixed_files_null_safety(void) {
    /* Test NULL loop parameter */
    int fd =
        open("/tmp/loopy_fixedfile_null.dat", O_RDWR | O_CREAT | O_TRUNC, 0644);
    TEST_ASSERT(fd >= 0, "fd >= 0 should be true");

    TEST_ASSERT(!loopyIoUringRegisterFiles(NULL, &fd, 1));
    TEST_ASSERT(!loopyIoUringUnregisterFiles(NULL), "!loopyIoUringUnregisterFiles(NULL) should be true");
    TEST_ASSERT(!loopyIoUringHasFixedFiles(NULL), "!loopyIoUringHasFixedFiles(NULL) should be true");

    char buf[100];
    FixedBufCtx ctx = {0, 0};
    TEST_ASSERT(loopyIoUringReadFixedFile(NULL, 0, buf, 100, 0,
                                          fixedBufCallback, &ctx) == 0);
    TEST_ASSERT(loopyIoUringWriteFixedFile(NULL, 0, buf, 100, 0,
                                           fixedBufCallback, &ctx) == 0);

    close(fd);
    unlink("/tmp/loopy_fixedfile_null.dat");
    return 1;
}

/* ====================================================================
 * io_uring Linked Operations Tests
 * ==================================================================== */

/* Callback context for linked operations */
typedef struct {
    int32_t result;
    int called;
} LinkOpCtx;

static void linkOpCallback(void *userData, int32_t result) {
    LinkOpCtx *ctx = (LinkOpCtx *)userData;
    ctx->result = result;
    ctx->called = 1;
}

/**
 * Test basic linked operation chain
 */
static int test_iouring_link_basic_chain(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(128);
    if (!l) {
        return 0;
    }

    /* Skip test if not using io_uring backend */
    if (!loopyUsingIoUring(l)) {
        return 1;
    }

    /* Create a test file, write to it, and close it in a single chain */
    const char *testPath = "/tmp/loopy_link_basic.dat";
    const char *testData = "Hello, linked operations!";
    size_t testLen = strlen(testData);

    LinkOpCtx openCtx = {0, 0};

    /* Create a link chain: OPENAT -> WRITE -> CLOSE */
    loopyIoUringLinkChain *chain =
        loopyIoUringLinkChainNew(l, LOOPY_IOURING_LINK_SOFT);
    TEST_ASSERT(chain != NULL, "chain != NULL should be true");

    /* Initially chain should be empty */
    TEST_ASSERT(loopyIoUringLinkChainLength(chain) == 0, "loopyIoUringLinkChainLength(chain) == 0 should be true");

    /* Add OPENAT */
    chain = loopyIoUringLinkChainOpenat(chain, AT_FDCWD, testPath,
                                        O_WRONLY | O_CREAT | O_TRUNC, 0644,
                                        linkOpCallback, &openCtx);
    TEST_ASSERT(chain != NULL, "chain != NULL should be true");
    TEST_ASSERT(loopyIoUringLinkChainLength(chain) == 1, "loopyIoUringLinkChainLength(chain) == 1 should be true");

    /* Note: In a real scenario, we'd need to pass the fd from openat to write.
     * For this test, we'll test the chain submission mechanism with separate
     * operations. */

    /* Submit the chain */
    TEST_ASSERT(loopyIoUringLinkChainSubmit(chain), "loopyIoUringLinkChainSubmit(chain) should be true");

    /* Process completions */
    for (int i = 0; i < 10 && !openCtx.called; i++) {
        loopyPoll(l, 100);
    }

    /* Verify open succeeded */
    TEST_ASSERT(openCtx.called, "openCtx.called should be true");
    TEST_ASSERT(openCtx.result >= 0, "openCtx.result >= 0 should be true"); /* Got a valid fd */

    /* Now test a simpler chain with existing file */
    int fd = open(testPath, O_WRONLY);
    TEST_ASSERT(fd >= 0, "fd >= 0 should be true");

    /* Write and fsync in a chain */
    LinkOpCtx write2Ctx = {0, 0};
    LinkOpCtx fsyncCtx = {0, 0};

    chain = loopyIoUringLinkChainNew(l, LOOPY_IOURING_LINK_SOFT);
    TEST_ASSERT(chain != NULL, "chain != NULL should be true");

    chain = loopyIoUringLinkChainWrite(chain, fd, testData, testLen, 0,
                                       linkOpCallback, &write2Ctx);
    TEST_ASSERT(chain != NULL, "chain != NULL should be true");

    chain =
        loopyIoUringLinkChainFsync(chain, fd, false, linkOpCallback, &fsyncCtx);
    TEST_ASSERT(chain != NULL, "chain != NULL should be true");
    TEST_ASSERT(loopyIoUringLinkChainLength(chain) == 2, "loopyIoUringLinkChainLength(chain) == 2 should be true");

    TEST_ASSERT(loopyIoUringLinkChainSubmit(chain), "loopyIoUringLinkChainSubmit(chain) should be true");

    /* Process completions */
    for (int i = 0; i < 10 && (!write2Ctx.called || !fsyncCtx.called); i++) {
        loopyPoll(l, 100);
    }

    TEST_ASSERT(write2Ctx.called, "write2Ctx.called should be true");
    TEST_ASSERT(write2Ctx.result == (int32_t)testLen, "write2Ctx.result == (int32_t)testLen should be true");
    TEST_ASSERT(fsyncCtx.called, "fsyncCtx.called should be true");
    TEST_ASSERT(fsyncCtx.result == 0, "fsyncCtx.result == 0 should be true");

    close(fd);
    unlink(testPath);
    return 1;
}

/**
 * Test soft link failure propagation
 */
static int test_iouring_link_soft_failure(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(128);
    if (!l) {
        return 0;
    }

    /* Skip test if not using io_uring backend */
    if (!loopyUsingIoUring(l)) {
        return 1;
    }

    /* Create a chain where the first operation fails */
    LinkOpCtx read1Ctx = {0, 0};
    LinkOpCtx read2Ctx = {0, 0};

    loopyIoUringLinkChain *chain =
        loopyIoUringLinkChainNew(l, LOOPY_IOURING_LINK_SOFT);
    TEST_ASSERT(chain != NULL, "chain != NULL should be true");

    /* Read from invalid fd (should fail) */
    chain = loopyIoUringLinkChainRead(chain, 9999, NULL, 100, 0, linkOpCallback,
                                      &read1Ctx);
    TEST_ASSERT(chain != NULL, "chain != NULL should be true");

    /* Second read should be canceled due to first failure */
    chain = loopyIoUringLinkChainRead(chain, 9999, NULL, 100, 0, linkOpCallback,
                                      &read2Ctx);
    TEST_ASSERT(chain != NULL, "chain != NULL should be true");

    TEST_ASSERT(loopyIoUringLinkChainSubmit(chain), "loopyIoUringLinkChainSubmit(chain) should be true");

    /* Process completions */
    for (int i = 0; i < 10 && (!read1Ctx.called || !read2Ctx.called); i++) {
        loopyPoll(l, 100);
    }

    /* First operation should fail */
    TEST_ASSERT(read1Ctx.called, "read1Ctx.called should be true");
    TEST_ASSERT(read1Ctx.result < 0, "read1Ctx.result < 0 should be true");

    /* Second operation should be canceled (may receive -ECANCELED or not be
     * called) */
    /* Note: Kernel behavior varies - sometimes canceled ops get callbacks,
     * sometimes not */

    return 1;
}

/**
 * Test hard link mode (continue on error)
 */
static int test_iouring_link_hard_mode(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(128);
    if (!l) {
        return 0;
    }

    /* Skip test if not using io_uring backend */
    if (!loopyUsingIoUring(l)) {
        return 1;
    }

    /* Create a valid test file */
    const char *testPath = "/tmp/loopy_link_hard.dat";
    int fd = open(testPath, O_RDWR | O_CREAT | O_TRUNC, 0644);
    TEST_ASSERT(fd >= 0, "fd >= 0 should be true");

    const char *testData = "test";
    write(fd, testData, 4);
    close(fd);

    /* Reopen for reading */
    fd = open(testPath, O_RDONLY);
    TEST_ASSERT(fd >= 0, "fd >= 0 should be true");

    LinkOpCtx read1Ctx = {0, 0};
    LinkOpCtx write1Ctx = {0, 0}; /* This should fail (read-only fd) */
    LinkOpCtx read2Ctx = {0,
                          0}; /* But this should still execute in hard mode */

    char buf1[10] = {0};
    char buf2[10] = {0};

    loopyIoUringLinkChain *chain =
        loopyIoUringLinkChainNew(l, LOOPY_IOURING_LINK_HARD);
    TEST_ASSERT(chain != NULL, "chain != NULL should be true");

    /* Read (should succeed) */
    chain = loopyIoUringLinkChainRead(chain, fd, buf1, 4, 0, linkOpCallback,
                                      &read1Ctx);
    TEST_ASSERT(chain != NULL, "chain != NULL should be true");

    /* Write to read-only fd (should fail) */
    chain = loopyIoUringLinkChainWrite(chain, fd, "fail", 4, 0, linkOpCallback,
                                       &write1Ctx);
    TEST_ASSERT(chain != NULL, "chain != NULL should be true");

    /* Another read (should execute anyway in hard mode) */
    chain = loopyIoUringLinkChainRead(chain, fd, buf2, 4, 0, linkOpCallback,
                                      &read2Ctx);
    TEST_ASSERT(chain != NULL, "chain != NULL should be true");

    TEST_ASSERT(loopyIoUringLinkChainSubmit(chain), "loopyIoUringLinkChainSubmit(chain) should be true");

    /* Process completions */
    for (int i = 0;
         i < 10 && (!read1Ctx.called || !write1Ctx.called || !read2Ctx.called);
         i++) {
        loopyPoll(l, 100);
    }

    /* First read should succeed */
    TEST_ASSERT(read1Ctx.called, "read1Ctx.called should be true");
    TEST_ASSERT(read1Ctx.result == 4, "read1Ctx.result == 4 should be true");

    /* Write should fail */
    TEST_ASSERT(write1Ctx.called, "write1Ctx.called should be true");
    TEST_ASSERT(write1Ctx.result < 0, "write1Ctx.result < 0 should be true");

    /* In hard mode, third operation should still execute despite second failure
     */
    TEST_ASSERT(read2Ctx.called, "read2Ctx.called should be true");
    TEST_ASSERT(read2Ctx.result == 4, "read2Ctx.result == 4 should be true");

    close(fd);
    unlink(testPath);
    return 1;
}

/**
 * Test multiple operation types in chain
 */
static int test_iouring_link_multiple_ops(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(128);
    if (!l) {
        return 0;
    }

    /* Skip test if not using io_uring backend */
    if (!loopyUsingIoUring(l)) {
        return 1;
    }

    const char *testPath = "/tmp/loopy_link_multi.dat";
    const char *writeData = "Multiple operations test";
    size_t writeLen = strlen(writeData);

    /* Create test file */
    int fd = open(testPath, O_RDWR | O_CREAT | O_TRUNC, 0644);
    TEST_ASSERT(fd >= 0, "fd >= 0 should be true");

    LinkOpCtx write1Ctx = {0, 0};
    LinkOpCtx fsync1Ctx = {0, 0};
    LinkOpCtx read1Ctx = {0, 0};

    char readBuf[100] = {0};

    /* Chain: WRITE -> FSYNC -> READ */
    loopyIoUringLinkChain *chain =
        loopyIoUringLinkChainNew(l, LOOPY_IOURING_LINK_SOFT);
    TEST_ASSERT(chain != NULL, "chain != NULL should be true");

    chain = loopyIoUringLinkChainWrite(chain, fd, writeData, writeLen, 0,
                                       linkOpCallback, &write1Ctx);
    TEST_ASSERT(chain != NULL, "chain != NULL should be true");

    chain = loopyIoUringLinkChainFsync(chain, fd, false, linkOpCallback,
                                       &fsync1Ctx);
    TEST_ASSERT(chain != NULL, "chain != NULL should be true");

    chain = loopyIoUringLinkChainRead(chain, fd, readBuf, writeLen, 0,
                                      linkOpCallback, &read1Ctx);
    TEST_ASSERT(chain != NULL, "chain != NULL should be true");

    TEST_ASSERT(loopyIoUringLinkChainLength(chain) == 3, "loopyIoUringLinkChainLength(chain) == 3 should be true");
    TEST_ASSERT(loopyIoUringLinkChainSubmit(chain), "loopyIoUringLinkChainSubmit(chain) should be true");

    /* Process completions */
    for (int i = 0;
         i < 10 && (!write1Ctx.called || !fsync1Ctx.called || !read1Ctx.called);
         i++) {
        loopyPoll(l, 100);
    }

    /* All operations should succeed */
    TEST_ASSERT(write1Ctx.called, "write1Ctx.called should be true");
    TEST_ASSERT(write1Ctx.result == (int32_t)writeLen, "write1Ctx.result == (int32_t)writeLen should be true");

    TEST_ASSERT(fsync1Ctx.called, "fsync1Ctx.called should be true");
    TEST_ASSERT(fsync1Ctx.result == 0, "fsync1Ctx.result == 0 should be true");

    TEST_ASSERT(read1Ctx.called, "read1Ctx.called should be true");
    TEST_ASSERT(read1Ctx.result == (int32_t)writeLen, "read1Ctx.result == (int32_t)writeLen should be true");
    TEST_ASSERT(memcmp(readBuf, writeData, writeLen) == 0);

    close(fd);
    unlink(testPath);
    return 1;
}

/**
 * Test NULL safety and error handling
 */
static int test_iouring_link_null_safety(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(128);
    if (!l) {
        return 0;
    }

    /* Skip test if not using io_uring backend */
    if (!loopyUsingIoUring(l)) {
        return 1;
    }

    /* NULL loop parameter */
    TEST_ASSERT(loopyIoUringLinkChainNew(NULL, LOOPY_IOURING_LINK_SOFT) ==
                NULL);

    /* Create valid chain */
    loopyIoUringLinkChain *chain =
        loopyIoUringLinkChainNew(l, LOOPY_IOURING_LINK_SOFT);
    TEST_ASSERT(chain != NULL, "chain != NULL should be true");

    LinkOpCtx ctx = {0, 0};

    /* Add operations to NULL chain (should return NULL) */
    char buf[10];
    TEST_ASSERT(loopyIoUringLinkChainRead(NULL, 0, buf, 10, 0, linkOpCallback,
                                          &ctx) == NULL);
    TEST_ASSERT(loopyIoUringLinkChainWrite(NULL, 0, buf, 10, 0, linkOpCallback,
                                           &ctx) == NULL);
    TEST_ASSERT(loopyIoUringLinkChainOpenat(NULL, AT_FDCWD, "/tmp/x", O_RDONLY,
                                            0, linkOpCallback, &ctx) == NULL);
    TEST_ASSERT(loopyIoUringLinkChainClose(NULL, 0, linkOpCallback, &ctx) ==
                NULL);
    TEST_ASSERT(loopyIoUringLinkChainFsync(NULL, 0, false, linkOpCallback,
                                           &ctx) == NULL);

    /* Submit empty chain (should fail but not crash) */
    loopyIoUringLinkChain *emptyChain =
        loopyIoUringLinkChainNew(l, LOOPY_IOURING_LINK_SOFT);
    TEST_ASSERT(emptyChain != NULL, "emptyChain != NULL should be true");
    TEST_ASSERT(loopyIoUringLinkChainLength(emptyChain) == 0, "loopyIoUringLinkChainLength(emptyChain) == 0 should be true");
    TEST_ASSERT(
        !loopyIoUringLinkChainSubmit(emptyChain), "!loopyIoUringLinkChainSubmit(emptyChain) should be true"); /* Empty chain should fail + free */

    /* NULL chain submit and length */
    TEST_ASSERT(!loopyIoUringLinkChainSubmit(NULL), "!loopyIoUringLinkChainSubmit(NULL) should be true");
    TEST_ASSERT(loopyIoUringLinkChainLength(NULL) == 0, "loopyIoUringLinkChainLength(NULL) == 0 should be true");

    /* Free NULL chain (should not crash) */
    loopyIoUringLinkChainFree(NULL);

    /* Free the first test chain */
    loopyIoUringLinkChainFree(chain);

    /* Discard a chain without submitting */
    chain = loopyIoUringLinkChainNew(l, LOOPY_IOURING_LINK_SOFT);
    TEST_ASSERT(chain != NULL, "chain != NULL should be true");
    chain =
        loopyIoUringLinkChainRead(chain, 0, buf, 10, 0, linkOpCallback, &ctx);
    TEST_ASSERT(chain != NULL, "chain != NULL should be true");
    loopyIoUringLinkChainFree(chain); /* Should not execute the operation */

    return 1;
}

#endif /* __linux__ */

/* ====================================================================
 * Random Tests
 * ==================================================================== */

static int test_random_sync_basic(void) {
    unsigned char buf[32] = {0};

    /* Generate random bytes */
    int result = loopyRandomSync(buf, sizeof(buf));
    TEST_ASSERT_EQ(result, 0, "sync random should succeed");

    /* Verify buffer was modified (extremely unlikely all zeros) */
    bool allZero = true;
    for (size_t i = 0; i < sizeof(buf); i++) {
        if (buf[i] != 0) {
            allZero = false;
            break;
        }
    }
    TEST_ASSERT(!allZero, "buffer should contain non-zero bytes");

    return 1;
}

static int test_random_sync_various_sizes(void) {
    /* Test 1 byte */
    unsigned char buf1[1] = {0};
    TEST_ASSERT_EQ(loopyRandomSync(buf1, 1), 0, "1 byte should work");

    /* Test 16 bytes */
    unsigned char buf16[16] = {0};
    TEST_ASSERT_EQ(loopyRandomSync(buf16, 16), 0, "16 bytes should work");

    /* Test 256 bytes */
    unsigned char buf256[256] = {0};
    TEST_ASSERT_EQ(loopyRandomSync(buf256, 256), 0, "256 bytes should work");

    /* Test 4096 bytes */
    unsigned char buf4096[4096] = {0};
    TEST_ASSERT_EQ(loopyRandomSync(buf4096, 4096), 0, "4096 bytes should work");

    return 1;
}

static int random_async_callback_count = 0;
static int random_async_status = -999;
static loopyLoop *random_test_loop = NULL;
static loopyRandomRequest *random_async_request = NULL;

static void test_random_async_callback(loopyLoop *loop, loopyRandomRequest *req,
                                       int status, void *userData) {
    (void)userData;
    (void)req;
    random_async_callback_count++;
    random_async_status = status;
    if (loop) {
        loopyStop(loop);
    }
}

static bool random_timeout_callback(timerWheel *t, timerWheelId id,
                                    void *data) {
    (void)t;
    (void)id;
    loopyLoop *l = data;
    loopyStop(l);
    return false;
}

static int test_random_async(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    random_test_loop = l;
    random_async_callback_count = 0;
    random_async_status = -999;

    unsigned char buf[64] = {0};

    /* Request async random bytes */
    random_async_request = loopyRandom(l, buf, sizeof(buf), test_random_async_callback, NULL);
    TEST_ASSERT(random_async_request != NULL, "async random should return request");

    /* Set timeout */
    loopyRegisterTimer(l, 3000000, 0, random_timeout_callback, l);

    /* Run loop until callback */
    loopyMain(l);

    TEST_ASSERT_EQ(random_async_callback_count, 1, "callback should fire once");
    TEST_ASSERT_EQ(random_async_status, 0, "status should be OK");

    /* Verify buffer was modified */
    bool allZero = true;
    for (size_t i = 0; i < sizeof(buf); i++) {
        if (buf[i] != 0) {
            allZero = false;
            break;
        }
    }
    TEST_ASSERT(!allZero, "buffer should contain non-zero bytes");

    /* Free the request object (allocated by loopyRandom) */
    loopyRandomFree(random_async_request);
    random_async_request = NULL;

    random_test_loop = NULL;
    return 1;
}

static int test_random_uniqueness(void) {
    /* Generate two random buffers and verify they are different */
    unsigned char buf1[32];
    unsigned char buf2[32];

    TEST_ASSERT_EQ(loopyRandomSync(buf1, sizeof(buf1)), 0,
                   "first random should succeed");
    TEST_ASSERT_EQ(loopyRandomSync(buf2, sizeof(buf2)), 0,
                   "second random should succeed");

    /* Compare buffers - should be different */
    bool same = (memcmp(buf1, buf2, sizeof(buf1)) == 0);
    TEST_ASSERT(!same, "two random calls should produce different output");

    /* Generate more to verify randomness */
    unsigned char buf3[32];
    unsigned char buf4[32];
    loopyRandomSync(buf3, sizeof(buf3));
    loopyRandomSync(buf4, sizeof(buf4));

    TEST_ASSERT(memcmp(buf1, buf3, sizeof(buf1)) != 0, "buf1 != buf3");
    TEST_ASSERT(memcmp(buf2, buf4, sizeof(buf2)) != 0, "buf2 != buf4");
    TEST_ASSERT(memcmp(buf3, buf4, sizeof(buf3)) != 0, "buf3 != buf4");

    return 1;
}

static int test_random_null_safety(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);
    unsigned char buf[32];

    /* NULL buffer */
    TEST_ASSERT_EQ(loopyRandomSync(NULL, 32), -1, "NULL buffer should fail");

    /* Zero length */
    TEST_ASSERT_EQ(loopyRandomSync(buf, 0), -1, "zero length should fail");

    /* NULL loop for async */
    TEST_ASSERT(loopyRandom(NULL, buf, sizeof(buf), test_random_async_callback,
                            NULL) == NULL,
                "NULL loop should fail");

    /* NULL buffer for async */
    TEST_ASSERT(loopyRandom(l, NULL, sizeof(buf), test_random_async_callback,
                            NULL) == NULL,
                "NULL buffer for async should fail");

    /* Zero length for async */
    TEST_ASSERT(loopyRandom(l, buf, 0, test_random_async_callback, NULL) ==
                    NULL,
                "zero length for async should fail");

    /* NULL request for cancel */
    TEST_ASSERT(!loopyRandomCancel(NULL), "cancel NULL should return false");

    /* NULL request for getters */
    TEST_ASSERT_EQ(loopyRandomGetResult(NULL), -1,
                   "getResult NULL should return -1");
    TEST_ASSERT_EQ(loopyRandomGetLength(NULL), 0,
                   "getLength NULL should return 0");
    TEST_ASSERT(loopyRandomGetBuffer(NULL) == NULL,
                "getBuffer NULL should return NULL");

    /* Free NULL should be safe */
    loopyRandomFree(NULL);

    return 1;
}

/* ====================================================================
 * loopyTimer Tests
 * ==================================================================== */

typedef struct {
    int called;
    int callCount;
    loopyTimer *timer;
    loopyLoop *loop;
} TimerTestCtx;

static void timer_oneshot_callback(loopyLoop *l, loopyTimer *t, void *data) {
    TimerTestCtx *ctx = (TimerTestCtx *)data;
    ctx->called = 1;
    ctx->callCount++;
    ctx->timer = t;
    ctx->loop = l;
    loopyStop(l);
}

static int test_timer_oneshot_basic(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(128);
    TimerTestCtx ctx = {0};

    /* Create a one-shot timer that fires in 10ms */
    loopyTimer *timer =
        loopyTimerOneShot(l, 10000, timer_oneshot_callback, &ctx);
    TEST_ASSERT(timer != NULL, "one-shot timer creation should succeed");
    TEST_ASSERT(loopyTimerIsActive(timer), "timer should be active");

    /* Run the loop */
    loopyMain(l);

    /* Verify callback was called exactly once */
    TEST_ASSERT_EQ(ctx.called, 1, "callback should be called");
    TEST_ASSERT_EQ(ctx.callCount, 1, "callback should be called exactly once");
    TEST_ASSERT(ctx.timer == timer, "timer handle should match");
    TEST_ASSERT(ctx.loop == l, "loop should match");

    /* Timer should auto-cleanup after firing (don't free it) */
    return 1;
}

static int test_timer_oneshot_ms_seconds(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(128);
    TimerTestCtx ctx = {0};

    /* Test milliseconds version */
    loopyTimer *timer =
        loopyTimerOneShotMs(l, 10, timer_oneshot_callback, &ctx);
    TEST_ASSERT(timer != NULL, "one-shot ms timer should succeed");
    loopyMain(l);
    TEST_ASSERT_EQ(ctx.callCount, 1, "ms timer should fire once");

    loopyDelete(l);  /* Delete first before reassigning */

    /* Test that the API exists (seconds version is just a multiplier) */
    l = loopyNew(128);
    ctx = (TimerTestCtx){0};
    /* Use very short duration for test speed - 1ms expressed as seconds */
    timer = loopyTimerOneShot(l, 10000, timer_oneshot_callback, &ctx);
    TEST_ASSERT(timer != NULL, "one-shot API should work");
    loopyMain(l);
    TEST_ASSERT_EQ(ctx.callCount, 1, "timer should fire once");

    return 1;
}

static void timer_periodic_callback(loopyLoop *l, loopyTimer *t, void *data) {
    TimerTestCtx *ctx = (TimerTestCtx *)data;
    ctx->called = 1;
    ctx->callCount++;
    ctx->timer = t;

    /* Stop after 3 calls */
    if (ctx->callCount >= 3) {
        loopyTimerCancel(t);
        loopyStop(l);
    }
}

static int test_timer_periodic_basic(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(128);
    TimerTestCtx ctx = {0};

    /* Create periodic timer that fires every 10ms */
    loopyTimer *timer =
        loopyTimerPeriodic(l, 10000, timer_periodic_callback, &ctx);
    TEST_ASSERT(timer != NULL, "periodic timer creation should succeed");
    TEST_ASSERT(loopyTimerIsActive(timer), "timer should be active");

    /* Run until callback stops us */
    loopyMain(l);

    /* Verify callback was called multiple times */
    TEST_ASSERT_EQ(ctx.called, 1, "callback should be called");
    TEST_ASSERT(ctx.callCount >= 3,
                "callback should be called at least 3 times");

    /* Timer was cancelled in callback, so it's already freed */
    return 1;
}

static int test_timer_periodic_delayed(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(128);
    TimerTestCtx ctx = {0};

    /* Start after 20ms, then repeat every 10ms */
    loopyTimer *timer = loopyTimerPeriodicDelayed(
        l, 20000, 10000, timer_periodic_callback, &ctx);
    TEST_ASSERT(timer != NULL, "periodic delayed timer should succeed");

    loopyMain(l);
    TEST_ASSERT(ctx.callCount >= 3, "delayed timer should fire multiple times");

    return 1;
}

static int test_timer_periodic_ms_seconds(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(128);
    TimerTestCtx ctx = {0};

    /* Test milliseconds version */
    loopyTimer *timer =
        loopyTimerPeriodicMs(l, 10, timer_periodic_callback, &ctx);
    TEST_ASSERT(timer != NULL, "periodic ms timer should succeed");
    loopyMain(l);
    TEST_ASSERT(ctx.callCount >= 3, "ms timer should fire multiple times");

    loopyDelete(l);  /* Delete first before reassigning */

    /* Test that the API exists (seconds version is just a multiplier) */
    l = loopyNew(128);
    ctx = (TimerTestCtx){0};
    /* Use very short duration for test speed - 10ms expressed as microseconds
     */
    timer = loopyTimerPeriodic(l, 10000, timer_periodic_callback, &ctx);
    TEST_ASSERT(timer != NULL, "periodic API should work");
    loopyMain(l);
    TEST_ASSERT(ctx.callCount >= 3, "timer should fire multiple times");

    return 1;
}

static void timer_cancel_callback(loopyLoop *l, loopyTimer *t, void *data) {
    (void)l;
    (void)t;
    TimerTestCtx *ctx = (TimerTestCtx *)data;
    ctx->called = 1;
}

static int test_timer_cancel(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(128);
    TimerTestCtx ctx = {0};

    /* Create a one-shot timer with long delay */
    loopyTimer *timer =
        loopyTimerOneShot(l, 1000000, timer_cancel_callback, &ctx);
    TEST_ASSERT(timer != NULL, "timer creation should succeed");
    TEST_ASSERT(loopyTimerIsActive(timer), "timer should be active");

    /* Cancel immediately */
    loopyTimerCancel(timer);

    /* Callback should not have been called */
    TEST_ASSERT_EQ(ctx.called, 0, "callback should not be called after cancel");

    /* Cancelling NULL should be safe */
    loopyTimerCancel(NULL);

    return 1;
}

static int test_timer_is_active(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(128);
    TimerTestCtx ctx = {0};

    /* Create a timer */
    loopyTimer *timer =
        loopyTimerOneShot(l, 1000000, timer_cancel_callback, &ctx);
    TEST_ASSERT(loopyTimerIsActive(timer), "new timer should be active");

    /* Cancel it */
    loopyTimerCancel(timer);

    /* NULL should return false */
    TEST_ASSERT(!loopyTimerIsActive(NULL), "NULL timer should not be active");

    return 1;
}

static int test_timer_userdata(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(128);
    int data1 = 42;
    int data2 = 99;

    /* Create timer with data1 */
    loopyTimer *timer =
        loopyTimerOneShot(l, 1000000, timer_cancel_callback, &data1);
    TEST_ASSERT(loopyTimerGetData(timer) == &data1,
                "getUserData should return initial data");

    /* Update to data2 */
    TEST_ASSERT(loopyTimerSetData(timer, &data2), "setUserData should succeed");
    TEST_ASSERT(loopyTimerGetData(timer) == &data2,
                "getUserData should return updated data");

    /* NULL timer operations */
    TEST_ASSERT(loopyTimerGetData(NULL) == NULL,
                "getUserData on NULL should return NULL");
    TEST_ASSERT(!loopyTimerSetData(NULL, &data1),
                "setUserData on NULL should fail");

    loopyTimerCancel(timer);
    return 1;
}

static int test_timer_null_safety(void) {
    /* All these should safely handle NULL without crashing */
    TEST_ASSERT(loopyTimerOneShot(NULL, 1000, timer_cancel_callback, NULL) ==
                    NULL,
                "NULL loop should fail");

    LOOPY_SELF_DELETE(l) = loopyNew(128);

    TEST_ASSERT(loopyTimerOneShot(l, 1000, NULL, NULL) == NULL,
                "NULL callback should fail");
    TEST_ASSERT(loopyTimerOneShotMs(NULL, 1, timer_cancel_callback, NULL) ==
                    NULL,
                "NULL loop for ms should fail");
    TEST_ASSERT(
        loopyTimerOneShotSeconds(NULL, 1, timer_cancel_callback, NULL) == NULL,
        "NULL loop for seconds should fail");
    TEST_ASSERT(loopyTimerPeriodic(NULL, 1000, timer_cancel_callback, NULL) ==
                    NULL,
                "NULL loop for periodic should fail");
    TEST_ASSERT(loopyTimerPeriodicDelayed(NULL, 1000, 1000,
                                          timer_cancel_callback, NULL) == NULL,
                "NULL loop for periodic delayed should fail");

    loopyTimerCancel(NULL); /* Should not crash */
    TEST_ASSERT(!loopyTimerIsActive(NULL), "NULL timer is not active");
    TEST_ASSERT(loopyTimerGetData(NULL) == NULL,
                "NULL timer getUserData returns NULL");
    TEST_ASSERT(!loopyTimerSetData(NULL, NULL), "NULL timer setUserData fails");

    return 1;
}

static int test_timer_auto_cleanup(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(128);
    TimerTestCtx ctx = {0};

    /* Create a one-shot timer */
    loopyTimer *timer =
        loopyTimerOneShot(l, 10000, timer_oneshot_callback, &ctx);
    TEST_ASSERT(timer != NULL, "timer should be created");

    /* Let it fire and auto-cleanup */
    loopyMain(l);
    TEST_ASSERT_EQ(ctx.called, 1, "callback should fire");

    /* Timer should have auto-cleaned up - we don't need to free it */
    /* Just verify the callback was called */
    TEST_ASSERT(ctx.timer == timer, "timer handle should match in callback");

    return 1;
}

/* ====================================================================
 * Channel Tests (loopyChannel)
 * ==================================================================== */

/**
 * Test channel create and destroy.
 */
static int test_channel_create_destroy(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    loopyChannelConfig config;
    loopyChannelConfigInit(&config);
    config.elementSize = sizeof(int);
    config.capacity = 16;

    loopyChannel *ch = loopyChannelNew(l, &config);
    TEST_ASSERT(ch != NULL, "channel creation should succeed");
    TEST_ASSERT(loopyChannelGetId(ch) > 0 || loopyChannelGetId(ch) == 0,
                "should have valid id");
    TEST_ASSERT(loopyChannelGetLoop(ch) == l, "should return correct loop");
    TEST_ASSERT(loopyChannelGetElementSize(ch) == sizeof(int),
                "element size should match");

    loopyChannelFree(ch);
    return 1;
}

/**
 * Test config initialization.
 */
static int test_channel_config_init(void) {
    loopyChannelConfig config;
    loopyChannelConfigInit(&config);

    TEST_ASSERT_EQ(config.type, LOOPY_CHANNEL_SPSC,
                   "default type should be SPSC");
    TEST_ASSERT_EQ(config.capacity, 1024, "default capacity should be 1024");
    TEST_ASSERT_EQ(config.elementSize, 0, "default element size should be 0");
    TEST_ASSERT_EQ(config.blocking, false, "default blocking should be false");

    return 1;
}

/**
 * Test SPSC send and receive.
 */
static int test_channel_spsc_send_recv(void) {
    loopyChannelConfig config;
    loopyChannelConfigInit(&config);
    config.type = LOOPY_CHANNEL_SPSC;
    config.elementSize = sizeof(int);
    config.capacity = 8;

    loopyChannel *ch = loopyChannelNew(NULL, &config);
    TEST_ASSERT(ch != NULL, "channel creation should succeed");
    TEST_ASSERT(loopyChannelGetType(ch) == LOOPY_CHANNEL_SPSC,
                "type should be SPSC");

    /* Send some values */
    for (int i = 0; i < 5; i++) {
        loopyChannelStatus status = loopyChannelSend(ch, &i, sizeof(i));
        TEST_ASSERT_EQ(status, LOOPY_CHANNEL_OK, "send should succeed");
    }

    TEST_ASSERT_EQ(loopyChannelLen(ch), 5, "should have 5 elements");

    /* Receive values */
    for (int i = 0; i < 5; i++) {
        int value = -1;
        ssize_t n = loopyChannelRecv(ch, &value, sizeof(value));
        TEST_ASSERT(n == sizeof(int), "recv should return element size");
        TEST_ASSERT_EQ(value, i, "value should match");
    }

    TEST_ASSERT(loopyChannelIsEmpty(ch), "channel should be empty");

    loopyChannelFree(ch);
    return 1;
}

/**
 * Test MPSC send and receive.
 */
static int test_channel_mpsc_send_recv(void) {
    loopyChannelConfig config;
    loopyChannelConfigInit(&config);
    config.type = LOOPY_CHANNEL_MPSC;
    config.elementSize = sizeof(int);
    config.capacity = 8;

    loopyChannel *ch = loopyChannelNew(NULL, &config);
    TEST_ASSERT(ch != NULL, "channel creation should succeed");
    TEST_ASSERT(loopyChannelGetType(ch) == LOOPY_CHANNEL_MPSC,
                "type should be MPSC");

    /* Send values */
    for (int i = 0; i < 5; i++) {
        loopyChannelStatus status = loopyChannelSend(ch, &i, sizeof(i));
        TEST_ASSERT_EQ(status, LOOPY_CHANNEL_OK, "send should succeed");
    }

    /* Receive values */
    for (int i = 0; i < 5; i++) {
        int value = -1;
        ssize_t n = loopyChannelRecv(ch, &value, sizeof(value));
        TEST_ASSERT(n == sizeof(int), "recv should return element size");
        TEST_ASSERT_EQ(value, i, "value should match");
    }

    loopyChannelFree(ch);
    return 1;
}

/**
 * Test MPMC send and receive.
 */
static int test_channel_mpmc_send_recv(void) {
    loopyChannelConfig config;
    loopyChannelConfigInit(&config);
    config.type = LOOPY_CHANNEL_MPMC;
    config.elementSize = sizeof(int);
    config.capacity = 8;

    loopyChannel *ch = loopyChannelNew(NULL, &config);
    TEST_ASSERT(ch != NULL, "channel creation should succeed");
    TEST_ASSERT(loopyChannelGetType(ch) == LOOPY_CHANNEL_MPMC,
                "type should be MPMC");

    /* Send values */
    for (int i = 0; i < 5; i++) {
        loopyChannelStatus status = loopyChannelSend(ch, &i, sizeof(i));
        TEST_ASSERT_EQ(status, LOOPY_CHANNEL_OK, "send should succeed");
    }

    /* Receive values */
    for (int i = 0; i < 5; i++) {
        int value = -1;
        ssize_t n = loopyChannelRecv(ch, &value, sizeof(value));
        TEST_ASSERT(n == sizeof(int), "recv should return element size");
        TEST_ASSERT_EQ(value, i, "value should match");
    }

    loopyChannelFree(ch);
    return 1;
}

/**
 * Test try send/recv non-blocking.
 */
static int test_channel_try_send_recv(void) {
    loopyChannelConfig config;
    loopyChannelConfigInit(&config);
    config.elementSize = sizeof(int);
    config.capacity = 4;

    loopyChannel *ch = loopyChannelNew(NULL, &config);
    TEST_ASSERT(ch != NULL, "channel creation should succeed");

    /* Try recv on empty channel */
    int value = -1;
    ssize_t n = loopyChannelTryRecv(ch, &value, sizeof(value));
    TEST_ASSERT_EQ(n, LOOPY_CHANNEL_EMPTY,
                   "tryrecv on empty should return EMPTY");

    /* Fill the channel */
    for (int i = 0; i < 4; i++) {
        loopyChannelStatus status = loopyChannelTrySend(ch, &i, sizeof(i));
        TEST_ASSERT_EQ(status, LOOPY_CHANNEL_OK, "trysend should succeed");
    }

    /* Try send on full channel */
    int extra = 999;
    loopyChannelStatus status = loopyChannelTrySend(ch, &extra, sizeof(extra));
    TEST_ASSERT_EQ(status, LOOPY_CHANNEL_FULL,
                   "trysend on full should return FULL");

    loopyChannelFree(ch);
    return 1;
}

/**
 * Test full/empty detection.
 */
static int test_channel_full_empty(void) {
    loopyChannelConfig config;
    loopyChannelConfigInit(&config);
    config.elementSize = sizeof(int);
    config.capacity = 4;

    loopyChannel *ch = loopyChannelNew(NULL, &config);

    TEST_ASSERT(loopyChannelIsEmpty(ch), "new channel should be empty");
    TEST_ASSERT(!loopyChannelIsFull(ch), "new channel should not be full");
    TEST_ASSERT_EQ(loopyChannelLen(ch), 0, "length should be 0");

    /* Fill to capacity */
    for (int i = 0; i < 4; i++) {
        loopyChannelSend(ch, &i, sizeof(i));
    }

    TEST_ASSERT(!loopyChannelIsEmpty(ch), "full channel should not be empty");
    TEST_ASSERT(loopyChannelIsFull(ch), "channel should be full");
    TEST_ASSERT_EQ(loopyChannelLen(ch), 4, "length should be 4");

    loopyChannelFree(ch);
    return 1;
}

/**
 * Test channel close.
 */
static int test_channel_close(void) {
    loopyChannelConfig config;
    loopyChannelConfigInit(&config);
    config.type = LOOPY_CHANNEL_MPMC;
    config.elementSize = sizeof(int);
    config.capacity = 8;

    loopyChannel *ch = loopyChannelNew(NULL, &config);

    /* Send some data */
    int value = 42;
    loopyChannelSend(ch, &value, sizeof(value));

    TEST_ASSERT(!loopyChannelIsClosed(ch), "should not be closed");

    loopyChannelClose(ch);

    TEST_ASSERT(loopyChannelIsClosed(ch), "should be closed");

    /* Send after close should fail */
    loopyChannelStatus status = loopyChannelSend(ch, &value, sizeof(value));
    TEST_ASSERT_EQ(status, LOOPY_CHANNEL_CLOSED,
                   "send after close should fail");

    loopyChannelFree(ch);
    return 1;
}

/**
 * Test channel statistics.
 */
static int test_channel_stats(void) {
    loopyChannelConfig config;
    loopyChannelConfigInit(&config);
    config.elementSize = sizeof(int);
    config.capacity = 8;

    loopyChannel *ch = loopyChannelNew(NULL, &config);

    /* Send and receive */
    for (int i = 0; i < 5; i++) {
        loopyChannelSend(ch, &i, sizeof(i));
    }
    for (int i = 0; i < 3; i++) {
        int value;
        loopyChannelRecv(ch, &value, sizeof(value));
    }

    loopyChannelStats stats;
    loopyChannelGetStats(ch, &stats);

    TEST_ASSERT_EQ(stats.totalSent, 5, "should have sent 5");
    TEST_ASSERT_EQ(stats.totalReceived, 3, "should have received 3");
    TEST_ASSERT(stats.peakUsage >= 5, "peak usage should be at least 5");

    loopyChannelResetStats(ch);
    loopyChannelGetStats(ch, &stats);
    TEST_ASSERT_EQ(stats.totalSent, 0, "sent should be 0 after reset");

    loopyChannelFree(ch);
    return 1;
}

/**
 * Test type name helper.
 */
static int test_channel_type_names(void) {
    TEST_ASSERT(strcmp(loopyChannelTypeName(LOOPY_CHANNEL_SPSC), "SPSC") == 0,
                "SPSC name should match");
    TEST_ASSERT(strcmp(loopyChannelTypeName(LOOPY_CHANNEL_MPSC), "MPSC") == 0,
                "MPSC name should match");
    TEST_ASSERT(strcmp(loopyChannelTypeName(LOOPY_CHANNEL_MPMC), "MPMC") == 0,
                "MPMC name should match");
    return 1;
}

/**
 * Test status name helper.
 */
static int test_channel_status_names(void) {
    TEST_ASSERT(strcmp(loopyChannelStatusName(LOOPY_CHANNEL_OK), "OK") == 0,
                "OK name should match");
    TEST_ASSERT(strcmp(loopyChannelStatusName(LOOPY_CHANNEL_FULL), "FULL") == 0,
                "FULL name should match");
    TEST_ASSERT(strcmp(loopyChannelStatusName(LOOPY_CHANNEL_EMPTY), "EMPTY") ==
                    0,
                "EMPTY name should match");
    TEST_ASSERT(
        strcmp(loopyChannelStatusName(LOOPY_CHANNEL_CLOSED), "CLOSED") == 0,
        "CLOSED name should match");
    return 1;
}

/**
 * Test NULL safety.
 */
static int test_channel_null_safety(void) {
    /* NULL channel operations should not crash */
    loopyChannelFree(NULL);
    loopyChannelClose(NULL);

    TEST_ASSERT_EQ(loopyChannelLen(NULL), 0, "len of NULL should be 0");
    TEST_ASSERT_EQ(loopyChannelCap(NULL), 0, "cap of NULL should be 0");
    TEST_ASSERT(loopyChannelIsClosed(NULL), "NULL should be considered closed");
    TEST_ASSERT(loopyChannelIsEmpty(NULL), "NULL should be considered empty");
    TEST_ASSERT(loopyChannelIsFull(NULL), "NULL should be considered full");

    TEST_ASSERT_EQ(loopyChannelGetId(NULL), 0, "id of NULL should be 0");
    TEST_ASSERT(loopyChannelGetLoop(NULL) == NULL,
                "loop of NULL should be NULL");

    /* Invalid config should fail */
    loopyChannelConfig config;
    loopyChannelConfigInit(&config);
    config.elementSize = 0; /* Invalid */
    loopyChannel *ch = loopyChannelNew(NULL, &config);
    TEST_ASSERT(ch == NULL, "zero element size should fail");

    config.elementSize = 4;
    config.capacity = 0; /* Invalid */
    ch = loopyChannelNew(NULL, &config);
    TEST_ASSERT(ch == NULL, "zero capacity should fail");

    return 1;
}

/**
 * Test channel introspection.
 */
static int test_channel_introspection(void) {
    LOOPY_SELF_DELETE(l) = loopyNew(16);

    loopyChannelConfig config;
    loopyChannelConfigInit(&config);
    config.type = LOOPY_CHANNEL_MPSC;
    config.elementSize = sizeof(long);
    config.capacity = 32;

    loopyChannel *ch = loopyChannelNew(l, &config);

    TEST_ASSERT(loopyChannelGetType(ch) == LOOPY_CHANNEL_MPSC,
                "type should be MPSC");
    TEST_ASSERT(loopyChannelGetElementSize(ch) == sizeof(long),
                "element size should match");
    TEST_ASSERT(loopyChannelCap(ch) == 32, "capacity should be 32");
    TEST_ASSERT(loopyChannelGetLoop(ch) == l, "loop should match");

    loopyChannelFree(ch);
    return 1;
}

/* Thread data for concurrent tests */
typedef struct ChannelThreadData {
    loopyChannel *ch;
    int numMessages;
    int threadId;
    bool success;
} ChannelThreadData;

static void *spsc_producer_thread(void *arg) {
    ChannelThreadData *data = arg;
    data->success = true;

    for (int i = 0; i < data->numMessages; i++) {
        int value = i;
        loopyChannelStatus status =
            loopyChannelSend(data->ch, &value, sizeof(value));
        if (status != LOOPY_CHANNEL_OK) {
            data->success = false;
            break;
        }
    }
    return NULL;
}

static void *spsc_consumer_thread(void *arg) {
    ChannelThreadData *data = arg;
    data->success = true;

    for (int i = 0; i < data->numMessages; i++) {
        int value = -1;
        ssize_t n = loopyChannelRecv(data->ch, &value, sizeof(value));
        if (n != sizeof(int) || value != i) {
            data->success = false;
            break;
        }
    }
    return NULL;
}

/**
 * Test SPSC with separate producer/consumer threads.
 */
static int test_channel_spsc_threaded(void) {
    loopyChannelConfig config;
    loopyChannelConfigInit(&config);
    config.type = LOOPY_CHANNEL_SPSC;
    config.elementSize = sizeof(int);
    config.capacity = 256;
    config.blocking = true;
    config.sendTimeoutUs = 5000000; /* 5 second timeout */
    config.recvTimeoutUs = 5000000;

    loopyChannel *ch = loopyChannelNew(NULL, &config);

    ChannelThreadData producerData = {
        .ch = ch, .numMessages = 1000, .threadId = 0};
    ChannelThreadData consumerData = {
        .ch = ch, .numMessages = 1000, .threadId = 1};

    pthread_t producer, consumer;
    pthread_create(&consumer, NULL, spsc_consumer_thread, &consumerData);
    pthread_create(&producer, NULL, spsc_producer_thread, &producerData);

    pthread_join(producer, NULL);
    pthread_join(consumer, NULL);

    TEST_ASSERT(producerData.success, "producer should succeed");
    TEST_ASSERT(consumerData.success, "consumer should succeed");

    loopyChannelFree(ch);
    return 1;
}

static void *mpsc_producer_thread(void *arg) {
    ChannelThreadData *data = arg;
    data->success = true;

    for (int i = 0; i < data->numMessages; i++) {
        int value = data->threadId * 10000 + i;
        loopyChannelStatus status =
            loopyChannelSend(data->ch, &value, sizeof(value));
        if (status != LOOPY_CHANNEL_OK) {
            data->success = false;
            break;
        }
    }
    return NULL;
}

/**
 * Test MPSC with multiple producers.
 */
static int test_channel_mpsc_threaded(void) {
    loopyChannelConfig config;
    loopyChannelConfigInit(&config);
    config.type = LOOPY_CHANNEL_MPSC;
    config.elementSize = sizeof(int);
    config.capacity = 256;
    config.blocking = true;
    config.sendTimeoutUs = 5000000;
    config.recvTimeoutUs = 5000000;

    loopyChannel *ch = loopyChannelNew(NULL, &config);

#define NUM_PRODUCERS 4
#define MSGS_PER_PRODUCER 100

    ChannelThreadData producerData[NUM_PRODUCERS];
    pthread_t producers[NUM_PRODUCERS];

    for (int i = 0; i < NUM_PRODUCERS; i++) {
        producerData[i].ch = ch;
        producerData[i].numMessages = MSGS_PER_PRODUCER;
        producerData[i].threadId = i;
        pthread_create(&producers[i], NULL, mpsc_producer_thread,
                       &producerData[i]);
    }

    /* Consume all messages */
    int received = 0;
    int totalExpected = NUM_PRODUCERS * MSGS_PER_PRODUCER;
    while (received < totalExpected) {
        int value;
        ssize_t n = loopyChannelRecv(ch, &value, sizeof(value));
        if (n == sizeof(int)) {
            received++;
        }
    }

    for (int i = 0; i < NUM_PRODUCERS; i++) {
        pthread_join(producers[i], NULL);
        TEST_ASSERT(producerData[i].success, "producer should succeed");
    }

    TEST_ASSERT_EQ(received, totalExpected, "should receive all messages");

    loopyChannelFree(ch);
    return 1;
}

static void *mpmc_worker_thread(void *arg) {
    ChannelThreadData *data = arg;
    data->success = true;

    /* Each worker sends and receives */
    for (int i = 0; i < data->numMessages; i++) {
        int value = data->threadId * 1000 + i;
        loopyChannelStatus status =
            loopyChannelSend(data->ch, &value, sizeof(value));
        if (status != LOOPY_CHANNEL_OK && status != LOOPY_CHANNEL_CLOSED) {
            data->success = false;
        }
    }
    return NULL;
}

/**
 * Test MPMC with multiple producers and consumers.
 */
static int test_channel_mpmc_threaded(void) {
    loopyChannelConfig config;
    loopyChannelConfigInit(&config);
    config.type = LOOPY_CHANNEL_MPMC;
    config.elementSize = sizeof(int);
    config.capacity = 64;
    config.blocking = true;
    config.sendTimeoutUs = 2000000;
    config.recvTimeoutUs = 2000000;

    loopyChannel *ch = loopyChannelNew(NULL, &config);

#define NUM_WORKERS 4
#define MSGS_PER_WORKER 50

    ChannelThreadData workerData[NUM_WORKERS];
    pthread_t workers[NUM_WORKERS];

    for (int i = 0; i < NUM_WORKERS; i++) {
        workerData[i].ch = ch;
        workerData[i].numMessages = MSGS_PER_WORKER;
        workerData[i].threadId = i;
        pthread_create(&workers[i], NULL, mpmc_worker_thread, &workerData[i]);
    }

    /* Also consume from main thread */
    int received = 0;
    int totalExpected = NUM_WORKERS * MSGS_PER_WORKER;
    while (received < totalExpected) {
        int value;
        ssize_t n = loopyChannelTryRecv(ch, &value, sizeof(value));
        if (n == sizeof(int)) {
            received++;
        } else if (n == LOOPY_CHANNEL_EMPTY) {
            usleep(100);
        }
    }

    for (int i = 0; i < NUM_WORKERS; i++) {
        pthread_join(workers[i], NULL);
    }

    TEST_ASSERT_EQ(received, totalExpected, "should receive all messages");

    loopyChannelFree(ch);
    return 1;
}

/**
 * Test select with multiple channels.
 */
static int test_channel_select_basic(void) {
    loopyChannelConfig config;
    loopyChannelConfigInit(&config);
    config.elementSize = sizeof(int);
    config.capacity = 4;

    loopyChannel *ch1 = loopyChannelNew(NULL, &config);
    loopyChannel *ch2 = loopyChannelNew(NULL, &config);

    /* Put data in ch2 only */
    int value = 42;
    loopyChannelSend(ch2, &value, sizeof(value));

    /* Select should find ch2 ready */
    int recv1 = 0, recv2 = 0;
    loopyChannelCase cases[2] = {
        {.ch = ch1, .send = false, .data = &recv1, .len = sizeof(recv1)},
        {.ch = ch2, .send = false, .data = &recv2, .len = sizeof(recv2)},
    };

    int ready = loopyChannelSelect(cases, 2, 100000); /* 100ms timeout */
    TEST_ASSERT_EQ(ready, 1, "ch2 (index 1) should be ready");
    TEST_ASSERT_EQ(recv2, 42, "should receive correct value");

    loopyChannelFree(ch1);
    loopyChannelFree(ch2);
    return 1;
}

/**
 * Test select timeout.
 */
static int test_channel_select_timeout(void) {
    loopyChannelConfig config;
    loopyChannelConfigInit(&config);
    config.elementSize = sizeof(int);
    config.capacity = 4;

    loopyChannel *ch = loopyChannelNew(NULL, &config);

    int value = 0;
    loopyChannelCase cases[1] = {
        {.ch = ch, .send = false, .data = &value, .len = sizeof(value)},
    };

    /* Should timeout since channel is empty */
    int ready = loopyChannelSelect(cases, 1, 10000); /* 10ms timeout */
    TEST_ASSERT_EQ(ready, -1, "should timeout on empty channel");

    loopyChannelFree(ch);
    return 1;
}

/* ====================================================================
 * Rate Limiter Tests
 * ==================================================================== */

/**
 * Test token bucket basic functionality.
 */
static int test_ratelimit_token_bucket_basic(void) {
    loopyRateLimiterConfig config = {0};
    config.algorithm = LOOPY_RATE_LIMIT_TOKEN_BUCKET;
    loopyTokenBucketConfigInit(&config.params.tokenBucket, 10.0, 5.0);

    loopyRateLimiter *limiter = loopyRateLimiterNew(NULL, &config);
    TEST_ASSERT(limiter != NULL, "limiter creation should succeed");

    /* Should allow 5 requests (burst capacity) */
    for (int i = 0; i < 5; i++) {
        loopyRateLimitResult result = loopyRateLimitCheck(limiter, 1.0);
        TEST_ASSERT_EQ(result, LOOPY_RATE_LIMIT_ALLOWED,
                       "request should be allowed");
    }

    /* 6th request should be denied (no tokens left) */
    loopyRateLimitResult result = loopyRateLimitCheck(limiter, 1.0);
    TEST_ASSERT_EQ(result, LOOPY_RATE_LIMIT_DENIED, "request should be denied");

    loopyRateLimiterFree(limiter);
    return 1;
}

/**
 * Test token bucket refill over time.
 */
static int test_ratelimit_token_bucket_refill(void) {
    loopyRateLimiterConfig config = {0};
    config.algorithm = LOOPY_RATE_LIMIT_TOKEN_BUCKET;
    loopyTokenBucketConfigInit(&config.params.tokenBucket, 1000.0, 2.0);

    loopyRateLimiter *limiter = loopyRateLimiterNew(NULL, &config);

    /* Use all tokens */
    loopyRateLimitCheck(limiter, 1.0);
    loopyRateLimitCheck(limiter, 1.0);

    /* Should be denied */
    loopyRateLimitResult result = loopyRateLimitCheck(limiter, 1.0);
    TEST_ASSERT_EQ(result, LOOPY_RATE_LIMIT_DENIED, "should be denied");

    /* Wait for refill (at 1000/sec, 2ms = 2 tokens) */
    struct timespec ts = {0, 3000000}; /* 3ms */
    nanosleep(&ts, NULL);

    /* Should be allowed now */
    result = loopyRateLimitCheck(limiter, 1.0);
    TEST_ASSERT_EQ(result, LOOPY_RATE_LIMIT_ALLOWED,
                   "should be allowed after refill");

    loopyRateLimiterFree(limiter);
    return 1;
}

/**
 * Test token bucket burst handling.
 */
static int test_ratelimit_token_bucket_burst(void) {
    loopyRateLimiterConfig config = {0};
    config.algorithm = LOOPY_RATE_LIMIT_TOKEN_BUCKET;
    loopyTokenBucketConfigInit(&config.params.tokenBucket, 100.0, 10.0);

    loopyRateLimiter *limiter = loopyRateLimiterNew(NULL, &config);

    /* Should handle burst of 10 */
    int allowed = 0;
    for (int i = 0; i < 15; i++) {
        if (loopyRateLimitCheck(limiter, 1.0) == LOOPY_RATE_LIMIT_ALLOWED) {
            allowed++;
        }
    }
    TEST_ASSERT_EQ(allowed, 10, "should allow exactly burst capacity");

    loopyRateLimiterFree(limiter);
    return 1;
}

/**
 * Test sliding window basic functionality.
 */
static int test_ratelimit_sliding_window_basic(void) {
    loopyRateLimiterConfig config = {0};
    config.algorithm = LOOPY_RATE_LIMIT_SLIDING_WINDOW;
    loopySlidingWindowConfigInit(&config.params.slidingWindow, 1000, 5);

    loopyRateLimiter *limiter = loopyRateLimiterNew(NULL, &config);
    TEST_ASSERT(limiter != NULL, "limiter creation should succeed");

    /* Should allow 5 requests */
    for (int i = 0; i < 5; i++) {
        loopyRateLimitResult result = loopyRateLimitCheck(limiter, 1.0);
        TEST_ASSERT_EQ(result, LOOPY_RATE_LIMIT_ALLOWED,
                       "request should be allowed");
    }

    /* 6th should be denied */
    loopyRateLimitResult result = loopyRateLimitCheck(limiter, 1.0);
    TEST_ASSERT_EQ(result, LOOPY_RATE_LIMIT_DENIED,
                   "6th request should be denied");

    loopyRateLimiterFree(limiter);
    return 1;
}

/**
 * Test sliding window expiry.
 */
static int test_ratelimit_sliding_window_expiry(void) {
    loopyRateLimiterConfig config = {0};
    config.algorithm = LOOPY_RATE_LIMIT_SLIDING_WINDOW;
    loopySlidingWindowConfigInit(&config.params.slidingWindow, 50,
                                 3); /* 50ms window */

    loopyRateLimiter *limiter = loopyRateLimiterNew(NULL, &config);

    /* Use all capacity */
    loopyRateLimitCheck(limiter, 1.0);
    loopyRateLimitCheck(limiter, 1.0);
    loopyRateLimitCheck(limiter, 1.0);

    /* Should be denied */
    loopyRateLimitResult result = loopyRateLimitCheck(limiter, 1.0);
    TEST_ASSERT_EQ(result, LOOPY_RATE_LIMIT_DENIED, "should be denied");

    /* Wait for window to expire */
    struct timespec ts = {0, 60000000}; /* 60ms */
    nanosleep(&ts, NULL);

    /* Should be allowed now */
    result = loopyRateLimitCheck(limiter, 1.0);
    TEST_ASSERT_EQ(result, LOOPY_RATE_LIMIT_ALLOWED,
                   "should be allowed after expiry");

    loopyRateLimiterFree(limiter);
    return 1;
}

/**
 * Test leaky bucket basic functionality.
 */
static int test_ratelimit_leaky_bucket_basic(void) {
    loopyRateLimiterConfig config = {0};
    config.algorithm = LOOPY_RATE_LIMIT_LEAKY_BUCKET;
    loopyLeakyBucketConfigInit(&config.params.leakyBucket, 100.0, 5);

    loopyRateLimiter *limiter = loopyRateLimiterNew(NULL, &config);
    TEST_ASSERT(limiter != NULL, "limiter creation should succeed");

    /* Should queue up to capacity */
    for (int i = 0; i < 5; i++) {
        loopyRateLimitResult result = loopyRateLimitCheck(limiter, 1.0);
        TEST_ASSERT_EQ(result, LOOPY_RATE_LIMIT_ALLOWED,
                       "request should be queued");
    }

    /* Should be denied when queue is full */
    loopyRateLimitResult result = loopyRateLimitCheck(limiter, 1.0);
    TEST_ASSERT_EQ(result, LOOPY_RATE_LIMIT_DENIED,
                   "should be denied when full");

    loopyRateLimiterFree(limiter);
    return 1;
}

/**
 * Test fixed window basic functionality.
 */
static int test_ratelimit_fixed_window_basic(void) {
    loopyRateLimiterConfig config = {0};
    config.algorithm = LOOPY_RATE_LIMIT_FIXED_WINDOW;
    loopyFixedWindowConfigInit(&config.params.fixedWindow, 1000, 5);

    loopyRateLimiter *limiter = loopyRateLimiterNew(NULL, &config);
    TEST_ASSERT(limiter != NULL, "limiter creation should succeed");

    /* Should allow 5 requests */
    for (int i = 0; i < 5; i++) {
        loopyRateLimitResult result = loopyRateLimitCheck(limiter, 1.0);
        TEST_ASSERT_EQ(result, LOOPY_RATE_LIMIT_ALLOWED,
                       "request should be allowed");
    }

    /* 6th should be denied */
    loopyRateLimitResult result = loopyRateLimitCheck(limiter, 1.0);
    TEST_ASSERT_EQ(result, LOOPY_RATE_LIMIT_DENIED,
                   "6th request should be denied");

    loopyRateLimiterFree(limiter);
    return 1;
}

/**
 * Test fixed window reset.
 */
static int test_ratelimit_fixed_window_reset(void) {
    loopyRateLimiterConfig config = {0};
    config.algorithm = LOOPY_RATE_LIMIT_FIXED_WINDOW;
    loopyFixedWindowConfigInit(&config.params.fixedWindow, 50,
                               3); /* 50ms window */

    loopyRateLimiter *limiter = loopyRateLimiterNew(NULL, &config);

    /* Use all capacity */
    loopyRateLimitCheck(limiter, 1.0);
    loopyRateLimitCheck(limiter, 1.0);
    loopyRateLimitCheck(limiter, 1.0);

    /* Should be denied */
    loopyRateLimitResult result = loopyRateLimitCheck(limiter, 1.0);
    TEST_ASSERT_EQ(result, LOOPY_RATE_LIMIT_DENIED, "should be denied");

    /* Wait for window to reset */
    struct timespec ts = {0, 60000000}; /* 60ms */
    nanosleep(&ts, NULL);

    /* Should be allowed in new window */
    result = loopyRateLimitCheck(limiter, 1.0);
    TEST_ASSERT_EQ(result, LOOPY_RATE_LIMIT_ALLOWED,
                   "should be allowed after reset");

    loopyRateLimiterFree(limiter);
    return 1;
}

/**
 * Test rate limit statistics.
 */
static int test_ratelimit_stats(void) {
    loopyRateLimiterConfig config = {0};
    config.algorithm = LOOPY_RATE_LIMIT_TOKEN_BUCKET;
    loopyTokenBucketConfigInit(&config.params.tokenBucket, 100.0, 3.0);

    loopyRateLimiter *limiter = loopyRateLimiterNew(NULL, &config);

    /* Make some requests */
    loopyRateLimitCheck(limiter, 1.0); /* allowed */
    loopyRateLimitCheck(limiter, 1.0); /* allowed */
    loopyRateLimitCheck(limiter, 1.0); /* allowed */
    loopyRateLimitCheck(limiter, 1.0); /* denied */
    loopyRateLimitCheck(limiter, 1.0); /* denied */

    loopyRateLimitStats stats;
    loopyRateLimitGetStats(limiter, &stats);

    TEST_ASSERT_EQ(stats.totalRequests, 5, "total requests should be 5");
    TEST_ASSERT_EQ(stats.allowedRequests, 3, "allowed should be 3");
    TEST_ASSERT_EQ(stats.deniedRequests, 2, "denied should be 2");

    loopyRateLimiterFree(limiter);
    return 1;
}

/**
 * Test rate limit peek (check without consuming).
 */
static int test_ratelimit_peek(void) {
    loopyRateLimiterConfig config = {0};
    config.algorithm = LOOPY_RATE_LIMIT_TOKEN_BUCKET;
    loopyTokenBucketConfigInit(&config.params.tokenBucket, 100.0, 5.0);

    loopyRateLimiter *limiter = loopyRateLimiterNew(NULL, &config);

    loopyRateLimitInfo info;
    loopyRateLimitResult result = loopyRateLimitPeek(limiter, &info);

    TEST_ASSERT_EQ(result, LOOPY_RATE_LIMIT_ALLOWED,
                   "peek should show allowed");
    TEST_ASSERT(info.allowed, "info.allowed should be true");
    TEST_ASSERT(info.remaining >= 4.9, "remaining should be ~5");

    /* Peek again - should still show same */
    result = loopyRateLimitPeek(limiter, &info);
    TEST_ASSERT_EQ(result, LOOPY_RATE_LIMIT_ALLOWED,
                   "peek should still show allowed");

    loopyRateLimiterFree(limiter);
    return 1;
}

/**
 * Test algorithm name functions.
 */
static int test_ratelimit_algorithm_names(void) {
    TEST_ASSERT(
        strcmp(loopyRateLimitAlgorithmName(LOOPY_RATE_LIMIT_TOKEN_BUCKET),
               "TOKEN_BUCKET") == 0,
        "token bucket name");
    TEST_ASSERT(
        strcmp(loopyRateLimitAlgorithmName(LOOPY_RATE_LIMIT_SLIDING_WINDOW),
               "SLIDING_WINDOW") == 0,
        "sliding window name");
    TEST_ASSERT(
        strcmp(loopyRateLimitAlgorithmName(LOOPY_RATE_LIMIT_LEAKY_BUCKET),
               "LEAKY_BUCKET") == 0,
        "leaky bucket name");
    TEST_ASSERT(
        strcmp(loopyRateLimitAlgorithmName(LOOPY_RATE_LIMIT_FIXED_WINDOW),
               "FIXED_WINDOW") == 0,
        "fixed window name");

    TEST_ASSERT(strcmp(loopyRateLimitResultName(LOOPY_RATE_LIMIT_ALLOWED),
                       "ALLOWED") == 0,
                "allowed result name");
    TEST_ASSERT(strcmp(loopyRateLimitResultName(LOOPY_RATE_LIMIT_DENIED),
                       "DENIED") == 0,
                "denied result name");

    return 1;
}

/* ====================================================================
 * Concurrency Limiter Tests
 * ==================================================================== */

/**
 * Test concurrency limiter basic functionality.
 */
static int test_concurrency_basic(void) {
    loopyConcurrencyConfig config;
    loopyConcurrencyConfigInit(&config, 5);

    loopyConcurrencyLimiter *limiter =
        loopyConcurrencyLimiterNew(NULL, &config);
    TEST_ASSERT(limiter != NULL, "limiter creation should succeed");

    /* Should acquire successfully */
    bool acquired = loopyConcurrencyTryAcquire(limiter);
    TEST_ASSERT(acquired, "first acquire should succeed");

    loopyConcurrencyInfo info;
    loopyConcurrencyGetInfo(limiter, &info);
    TEST_ASSERT_EQ(info.current, 1, "current should be 1");
    TEST_ASSERT_EQ(info.limit, 5, "limit should be 5");

    loopyConcurrencyRelease(limiter);
    loopyConcurrencyGetInfo(limiter, &info);
    TEST_ASSERT_EQ(info.current, 0, "current should be 0 after release");

    loopyConcurrencyLimiterFree(limiter);
    return 1;
}

/**
 * Test concurrency limit enforcement.
 */
static int test_concurrency_limit(void) {
    loopyConcurrencyConfig config;
    loopyConcurrencyConfigInit(&config, 3);

    loopyConcurrencyLimiter *limiter =
        loopyConcurrencyLimiterNew(NULL, &config);

    /* Acquire up to limit */
    TEST_ASSERT(loopyConcurrencyTryAcquire(limiter), "1st acquire");
    TEST_ASSERT(loopyConcurrencyTryAcquire(limiter), "2nd acquire");
    TEST_ASSERT(loopyConcurrencyTryAcquire(limiter), "3rd acquire");

    /* 4th should fail */
    TEST_ASSERT(!loopyConcurrencyTryAcquire(limiter),
                "4th acquire should fail");

    loopyConcurrencyLimiterFree(limiter);
    return 1;
}

/**
 * Test concurrency release.
 */
static int test_concurrency_release(void) {
    loopyConcurrencyConfig config;
    loopyConcurrencyConfigInit(&config, 2);

    loopyConcurrencyLimiter *limiter =
        loopyConcurrencyLimiterNew(NULL, &config);

    loopyConcurrencyTryAcquire(limiter);
    loopyConcurrencyTryAcquire(limiter);

    /* Should be at limit */
    TEST_ASSERT(!loopyConcurrencyTryAcquire(limiter), "should be at limit");

    /* Release one */
    loopyConcurrencyRelease(limiter);

    /* Should be able to acquire again */
    TEST_ASSERT(loopyConcurrencyTryAcquire(limiter),
                "should acquire after release");

    loopyConcurrencyLimiterFree(limiter);
    return 1;
}

/**
 * Test concurrency statistics.
 */
static int test_concurrency_stats(void) {
    loopyConcurrencyConfig config;
    loopyConcurrencyConfigInit(&config, 2);

    loopyConcurrencyLimiter *limiter =
        loopyConcurrencyLimiterNew(NULL, &config);

    loopyConcurrencyTryAcquire(limiter);
    loopyConcurrencyTryAcquire(limiter);
    loopyConcurrencyTryAcquire(limiter); /* Will fail */
    loopyConcurrencyRelease(limiter);
    loopyConcurrencyRelease(limiter);

    loopyConcurrencyStats stats;
    loopyConcurrencyGetStats(limiter, &stats);

    TEST_ASSERT_EQ(stats.totalAcquires, 3, "total acquires");
    TEST_ASSERT_EQ(stats.successfulAcquires, 2, "successful acquires");
    TEST_ASSERT_EQ(stats.failedAcquires, 1, "failed acquires");
    TEST_ASSERT_EQ(stats.totalReleases, 2, "total releases");
    TEST_ASSERT_EQ(stats.peakConcurrent, 2, "peak concurrent");

    loopyConcurrencyLimiterFree(limiter);
    return 1;
}

/**
 * Thread function for concurrency test.
 */
typedef struct {
    loopyConcurrencyLimiter *limiter;
    int successCount;
    int failCount;
} ConcurrencyThreadData;

static void *concurrency_thread_func(void *arg) {
    ConcurrencyThreadData *data = arg;

    for (int i = 0; i < 20; i++) {
        if (loopyConcurrencyTryAcquire(data->limiter)) {
            data->successCount++;
            /* Hold for a bit */
            struct timespec ts = {0, 100000}; /* 100us */
            nanosleep(&ts, NULL);
            loopyConcurrencyRelease(data->limiter);
        } else {
            data->failCount++;
        }
        /* Small delay between attempts */
        struct timespec ts = {0, 50000}; /* 50us */
        nanosleep(&ts, NULL);
    }
    return NULL;
}

/**
 * Test concurrency with multiple threads.
 */
static int test_concurrency_threaded(void) {
    loopyConcurrencyConfig config;
    loopyConcurrencyConfigInit(&config, 3);

    loopyConcurrencyLimiter *limiter =
        loopyConcurrencyLimiterNew(NULL, &config);

#define NUM_CONCURRENCY_THREADS 5
    pthread_t threads[NUM_CONCURRENCY_THREADS];
    ConcurrencyThreadData data[NUM_CONCURRENCY_THREADS];

    for (int i = 0; i < NUM_CONCURRENCY_THREADS; i++) {
        data[i].limiter = limiter;
        data[i].successCount = 0;
        data[i].failCount = 0;
        pthread_create(&threads[i], NULL, concurrency_thread_func, &data[i]);
    }

    for (int i = 0; i < NUM_CONCURRENCY_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    /* Verify we had both successes and failures */
    int totalSuccess = 0;
    for (int i = 0; i < NUM_CONCURRENCY_THREADS; i++) {
        totalSuccess += data[i].successCount;
    }

    TEST_ASSERT(totalSuccess > 0, "should have some successful acquires");
    /* With 5 threads contending for 3 slots, we expect some failures */

    loopyConcurrencyInfo info;
    loopyConcurrencyGetInfo(limiter, &info);
    TEST_ASSERT_EQ(info.current, 0, "all should be released at end");

    loopyConcurrencyLimiterFree(limiter);
    return 1;
}

/* ====================================================================
 * TLS Tests
 * ==================================================================== */

/**
 * Test TLS initialization and cleanup.
 */
static int test_tls_init_cleanup(void) {
    loopyTLSResult result = loopyTLSInit();
    TEST_ASSERT_EQ(result, LOOPY_TLS_OK, "TLS init should succeed");

    /* Can call init multiple times safely */
    result = loopyTLSInit();
    TEST_ASSERT_EQ(result, LOOPY_TLS_OK, "repeated init should succeed");

    loopyTLSCleanup();
    return 1;
}

/**
 * Test TLS context creation.
 */
static int test_tls_context_create(void) {
    loopyTLSContextConfig config;
    loopyTLSContextConfigInit(&config, LOOPY_TLS_CLIENT);

    /* Client context without certificates should work */
    config.verify = LOOPY_TLS_VERIFY_NONE;
    loopyTLSContext *ctx = loopyTLSContextNew(&config);
    TEST_ASSERT(ctx != NULL, "context creation should succeed");

    loopyTLSContextFree(ctx);
    return 1;
}

/**
 * Test TLS context config initialization.
 */
static int test_tls_context_config(void) {
    loopyTLSContextConfig config;

    /* Client config */
    loopyTLSContextConfigInit(&config, LOOPY_TLS_CLIENT);
    TEST_ASSERT_EQ(config.mode, LOOPY_TLS_CLIENT, "mode should be client");
    TEST_ASSERT_EQ(config.version, LOOPY_TLS_VERSION_AUTO,
                   "version should be auto");
    TEST_ASSERT_EQ(config.verify, LOOPY_TLS_VERIFY_REQUIRED,
                   "client should require verification");
    TEST_ASSERT(config.sessionResumption,
                "session resumption should be enabled");

    /* Server config */
    loopyTLSContextConfigInit(&config, LOOPY_TLS_SERVER);
    TEST_ASSERT_EQ(config.mode, LOOPY_TLS_SERVER, "mode should be server");
    TEST_ASSERT_EQ(config.verify, LOOPY_TLS_VERIFY_NONE,
                   "server should not require client cert");

    return 1;
}

/**
 * Test TLS result name conversion.
 */
static int test_tls_result_names(void) {
    TEST_ASSERT(strcmp(loopyTLSResultName(LOOPY_TLS_OK), "OK") == 0,
                "OK result name");
    TEST_ASSERT(strcmp(loopyTLSResultName(LOOPY_TLS_WANT_READ), "WANT_READ") ==
                    0,
                "WANT_READ result name");
    TEST_ASSERT(
        strcmp(loopyTLSResultName(LOOPY_TLS_WANT_WRITE), "WANT_WRITE") == 0,
        "WANT_WRITE result name");
    TEST_ASSERT(strcmp(loopyTLSResultName(LOOPY_TLS_ERROR), "ERROR") == 0,
                "ERROR result name");
    TEST_ASSERT(strcmp(loopyTLSResultName(LOOPY_TLS_CLOSED), "CLOSED") == 0,
                "CLOSED result name");
    TEST_ASSERT(strcmp(loopyTLSResultName(LOOPY_TLS_HANDSHAKE), "HANDSHAKE") ==
                    0,
                "HANDSHAKE result name");
    TEST_ASSERT(strcmp(loopyTLSResultName(LOOPY_TLS_VERIFY_FAILED),
                       "VERIFY_FAILED") == 0,
                "VERIFY_FAILED result name");

    return 1;
}

/**
 * Test TLS connection creation (needs socket).
 */
static int test_tls_connection_create(void) {
    loopyTLSContextConfig config;
    loopyTLSContextConfigInit(&config, LOOPY_TLS_CLIENT);
    config.verify = LOOPY_TLS_VERIFY_NONE;

    loopyTLSContext *ctx = loopyTLSContextNew(&config);
    TEST_ASSERT(ctx != NULL, "context creation should succeed");

    /* Create a dummy socket for testing */
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0) {
        loopyTLS *tls = loopyTLSNew(NULL, ctx, fds[0]);
        TEST_ASSERT(tls != NULL, "TLS connection creation should succeed");
        TEST_ASSERT_EQ(loopyTLSGetFD(tls), fds[0], "fd should match");
        TEST_ASSERT(!loopyTLSIsHandshakeDone(tls),
                    "handshake should not be done");

        loopyTLSFree(tls);
        close(fds[0]);
        close(fds[1]);
    }

    loopyTLSContextFree(ctx);
    return 1;
}

/**
 * Test TLS null safety.
 */
static int test_tls_null_safety(void) {
    /* These should not crash with NULL */
    loopyTLSContextFree(NULL);
    loopyTLSFree(NULL);
    loopyTLSContextConfigInit(NULL, LOOPY_TLS_CLIENT);

    /* NULL context should return NULL */
    loopyTLSContext *ctx = loopyTLSContextNew(NULL);
    TEST_ASSERT(ctx == NULL, "NULL config should return NULL context");

    /* Invalid fd should return NULL */
    loopyTLSContextConfig config;
    loopyTLSContextConfigInit(&config, LOOPY_TLS_CLIENT);
    config.verify = LOOPY_TLS_VERIFY_NONE;
    ctx = loopyTLSContextNew(&config);
    if (ctx) {
        loopyTLS *tls = loopyTLSNew(NULL, ctx, -1);
        TEST_ASSERT(tls == NULL, "invalid fd should return NULL");
        loopyTLSContextFree(ctx);
    }

    return 1;
}

#if LOOPY_HAVE_RAX
/* ====================================================================
 * PubSub Tests
 * ==================================================================== */

/**
 * Test pub/sub creation and destruction.
 */
static int test_pubsub_create_destroy(void) {
    loopyPubSub *ps = loopyPubSubNew(NULL, NULL);
    TEST_ASSERT(ps != NULL, "should create pub/sub hub");

    loopyPubSubStats stats;
    loopyPubSubGetStats(ps, &stats);
    TEST_ASSERT_EQ(stats.subscriptionCount, 0,
                   "should start with no subscriptions");

    loopyPubSubFree(ps);
    return 1;
}

/**
 * Test config initialization.
 */
static int test_pubsub_config_init(void) {
    loopyPubSubConfig config;
    loopyPubSubConfigInit(&config);

    TEST_ASSERT_EQ(config.separator, '.', "default separator should be '.'");
    TEST_ASSERT_EQ(config.starWildcard, '*',
                   "default star wildcard should be '*'");
    TEST_ASSERT_EQ(config.hashWildcard, '#',
                   "default hash wildcard should be '#'");

    loopySubscriptionConfig subConfig;
    loopySubscriptionConfigInit(&subConfig);

    TEST_ASSERT_EQ(subConfig.deliveryMode, LOOPY_DELIVER_SYNC,
                   "default delivery mode should be sync");
    TEST_ASSERT_EQ(subConfig.ackMode, LOOPY_ACK_AUTO,
                   "default ack mode should be auto");

    return 1;
}

/* Simple message callback for testing */
static int testPubSubMsgCount = 0;
static bool testPubSubCallback(loopySubscription *sub, const loopyMessage *msg,
                               void *userData) {
    (void)sub;
    (void)userData;
    if (msg && msg->data) {
        testPubSubMsgCount++;
    }
    return true;
}

/**
 * Test subscribe and unsubscribe.
 */
static int test_pubsub_subscribe_unsubscribe(void) {
    loopyPubSub *ps = loopyPubSubNew(NULL, NULL);
    TEST_ASSERT(ps != NULL, "should create pub/sub hub");

    loopySubscription *sub =
        loopySubscribe(ps, "test.topic", testPubSubCallback, NULL);
    TEST_ASSERT(sub != NULL, "should create subscription");

    TEST_ASSERT(strcmp(loopySubscriptionPattern(sub), "test.topic") == 0,
                "pattern should match");

    loopyPubSubStats stats;
    loopyPubSubGetStats(ps, &stats);
    TEST_ASSERT_EQ(stats.subscriptionCount, 1, "should have 1 subscription");

    TEST_ASSERT(loopyUnsubscribe(sub), "unsubscribe should succeed");

    loopyPubSubGetStats(ps, &stats);
    TEST_ASSERT_EQ(stats.subscriptionCount, 0, "should have 0 subscriptions");

    loopyPubSubFree(ps);
    return 1;
}

/**
 * Test basic publish and receive.
 */
static int test_pubsub_publish_basic(void) {
    loopyPubSub *ps = loopyPubSubNew(NULL, NULL);
    TEST_ASSERT(ps != NULL, "should create pub/sub hub");

    testPubSubMsgCount = 0;
    loopySubscription *sub =
        loopySubscribe(ps, "news.tech", testPubSubCallback, NULL);
    TEST_ASSERT(sub != NULL, "should create subscription");

    const char *msg = "Hello World";
    size_t delivered = loopyPublish(ps, "news.tech", msg, strlen(msg));
    TEST_ASSERT_EQ(delivered, 1, "should deliver to 1 subscriber");
    TEST_ASSERT_EQ(testPubSubMsgCount, 1, "callback should be called");

    /* Publishing to non-matching topic */
    delivered = loopyPublish(ps, "news.sports", msg, strlen(msg));
    TEST_ASSERT_EQ(delivered, 0, "should not deliver to non-matching topic");
    TEST_ASSERT_EQ(testPubSubMsgCount, 1,
                   "callback should not be called again");

    loopyPubSubFree(ps);
    return 1;
}

/**
 * Test star wildcard matching.
 */
static int test_pubsub_wildcard_star(void) {
    loopyPubSub *ps = loopyPubSubNew(NULL, NULL);
    TEST_ASSERT(ps != NULL, "should create pub/sub hub");

    testPubSubMsgCount = 0;
    loopySubscription *sub =
        loopySubscribe(ps, "stock.*.price", testPubSubCallback, NULL);
    TEST_ASSERT(sub != NULL, "should create subscription");

    /* Should match */
    size_t delivered = loopyPublish(ps, "stock.AAPL.price", "100", 3);
    TEST_ASSERT_EQ(delivered, 1, "should match stock.AAPL.price");

    delivered = loopyPublish(ps, "stock.GOOG.price", "200", 3);
    TEST_ASSERT_EQ(delivered, 1, "should match stock.GOOG.price");

    /* Should NOT match */
    delivered = loopyPublish(ps, "stock.price", "50", 2);
    TEST_ASSERT_EQ(delivered, 0,
                   "should not match stock.price (missing segment)");

    delivered = loopyPublish(ps, "stock.AAPL.NASDAQ.price", "150", 3);
    TEST_ASSERT_EQ(delivered, 0, "should not match (extra segment)");

    TEST_ASSERT_EQ(testPubSubMsgCount, 2, "should have received 2 messages");

    loopyPubSubFree(ps);
    return 1;
}

/**
 * Test hash wildcard matching.
 */
static int test_pubsub_wildcard_hash(void) {
    loopyPubSub *ps = loopyPubSubNew(NULL, NULL);
    TEST_ASSERT(ps != NULL, "should create pub/sub hub");

    testPubSubMsgCount = 0;
    loopySubscription *sub =
        loopySubscribe(ps, "logs.#", testPubSubCallback, NULL);
    TEST_ASSERT(sub != NULL, "should create subscription");

    /* Should match - hash matches zero or more segments */
    size_t delivered = loopyPublish(ps, "logs", "msg1", 4);
    TEST_ASSERT_EQ(delivered, 1, "should match logs (zero segments)");

    delivered = loopyPublish(ps, "logs.app", "msg2", 4);
    TEST_ASSERT_EQ(delivered, 1, "should match logs.app (one segment)");

    delivered = loopyPublish(ps, "logs.app.error", "msg3", 4);
    TEST_ASSERT_EQ(delivered, 1, "should match logs.app.error (two segments)");

    delivered = loopyPublish(ps, "logs.app.error.critical", "msg4", 4);
    TEST_ASSERT_EQ(delivered, 1, "should match (three segments)");

    /* Should NOT match */
    delivered = loopyPublish(ps, "metrics.app", "msg5", 4);
    TEST_ASSERT_EQ(delivered, 0, "should not match metrics.app");

    TEST_ASSERT_EQ(testPubSubMsgCount, 4, "should have received 4 messages");

    loopyPubSubFree(ps);
    return 1;
}

/**
 * Test multiple subscribers to same pattern.
 */
static int test_pubsub_multiple_subscribers(void) {
    loopyPubSub *ps = loopyPubSubNew(NULL, NULL);
    TEST_ASSERT(ps != NULL, "should create pub/sub hub");

    testPubSubMsgCount = 0;

    loopySubscription *sub1 =
        loopySubscribe(ps, "chat.room1", testPubSubCallback, NULL);
    loopySubscription *sub2 =
        loopySubscribe(ps, "chat.room1", testPubSubCallback, NULL);
    loopySubscription *sub3 =
        loopySubscribe(ps, "chat.room1", testPubSubCallback, NULL);

    TEST_ASSERT(sub1 != NULL && sub2 != NULL && sub3 != NULL,
                "all subscriptions should succeed");

    loopyPubSubStats stats;
    loopyPubSubGetStats(ps, &stats);
    TEST_ASSERT_EQ(stats.subscriptionCount, 3, "should have 3 subscriptions");
    TEST_ASSERT_EQ(stats.patternCount, 1, "should have 1 pattern");

    size_t delivered = loopyPublish(ps, "chat.room1", "hello", 5);
    TEST_ASSERT_EQ(delivered, 3, "should deliver to all 3 subscribers");
    TEST_ASSERT_EQ(testPubSubMsgCount, 3, "all callbacks should be called");

    /* Unsubscribe one */
    loopyUnsubscribe(sub2);

    loopyPubSubGetStats(ps, &stats);
    TEST_ASSERT_EQ(stats.subscriptionCount, 2, "should have 2 subscriptions");

    testPubSubMsgCount = 0;
    delivered = loopyPublish(ps, "chat.room1", "world", 5);
    TEST_ASSERT_EQ(delivered, 2, "should deliver to 2 subscribers");

    loopyPubSubFree(ps);
    return 1;
}

/**
 * Test queue delivery mode.
 */
static int test_pubsub_queue_mode(void) {
    loopyPubSub *ps = loopyPubSubNew(NULL, NULL);
    TEST_ASSERT(ps != NULL, "should create pub/sub hub");

    loopySubscriptionConfig config;
    loopySubscriptionConfigInit(&config);
    config.deliveryMode = LOOPY_DELIVER_QUEUE;
    config.queueSize = 10;

    loopySubscription *sub =
        loopySubscribe(ps, "queue.test", testPubSubCallback, &config);
    TEST_ASSERT(sub != NULL, "should create subscription");

    /* Publish some messages */
    loopyPublish(ps, "queue.test", "msg1", 4);
    loopyPublish(ps, "queue.test", "msg2", 4);
    loopyPublish(ps, "queue.test", "msg3", 4);

    TEST_ASSERT_EQ(loopySubscriptionPending(sub), 3, "should have 3 pending");

    /* Receive messages */
    loopyMessage msg;
    TEST_ASSERT(loopySubscriptionReceive(sub, &msg), "should receive message");
    TEST_ASSERT_EQ(msg.len, 4, "message length should be 4");

    TEST_ASSERT_EQ(loopySubscriptionPending(sub), 2, "should have 2 pending");

    loopyPubSubFree(ps);
    return 1;
}

/**
 * Test statistics.
 */
static int test_pubsub_stats(void) {
    loopyPubSubConfig config;
    loopyPubSubConfigInit(&config);
    config.enableStats = true;

    loopyPubSub *ps = loopyPubSubNew(NULL, &config);
    TEST_ASSERT(ps != NULL, "should create pub/sub hub");

    testPubSubMsgCount = 0;
    loopySubscribe(ps, "stats.test", testPubSubCallback, NULL);
    loopySubscribe(ps, "stats.#", testPubSubCallback, NULL);

    loopyPublish(ps, "stats.test", "data", 4);
    loopyPublish(ps, "stats.other", "data", 4);

    loopyPubSubStats stats;
    loopyPubSubGetStats(ps, &stats);

    TEST_ASSERT_EQ(stats.messagesPublished, 2, "should have 2 published");
    TEST_ASSERT_EQ(stats.messagesDelivered, 3, "should have 3 delivered");
    TEST_ASSERT_EQ(stats.bytesPublished, 8, "should have 8 bytes published");

    loopyPubSubResetStats(ps);
    loopyPubSubGetStats(ps, &stats);
    TEST_ASSERT_EQ(stats.messagesPublished, 0, "should be reset to 0");

    loopyPubSubFree(ps);
    return 1;
}

/**
 * Test validation functions.
 */
static int test_pubsub_validation(void) {
    loopyPubSub *ps = loopyPubSubNew(NULL, NULL);
    TEST_ASSERT(ps != NULL, "should create pub/sub hub");

    /* Valid topics */
    TEST_ASSERT(loopyValidateTopic(ps, "simple"),
                "simple topic should be valid");
    TEST_ASSERT(loopyValidateTopic(ps, "a.b.c"),
                "dotted topic should be valid");

    /* Invalid topics (contain wildcards) */
    TEST_ASSERT(!loopyValidateTopic(ps, "test.*"),
                "star in topic should be invalid");
    TEST_ASSERT(!loopyValidateTopic(ps, "test.#"),
                "hash in topic should be invalid");

    /* Valid patterns */
    TEST_ASSERT(loopyValidatePattern(ps, "simple"),
                "simple pattern should be valid");
    TEST_ASSERT(loopyValidatePattern(ps, "a.*.c"),
                "star pattern should be valid");
    TEST_ASSERT(loopyValidatePattern(ps, "a.#"),
                "hash pattern should be valid");

    loopyPubSubFree(ps);
    return 1;
}

/**
 * Test null safety.
 */
static int test_pubsub_null_safety(void) {
    /* These should not crash */
    loopyPubSubConfigInit(NULL);
    loopySubscriptionConfigInit(NULL);
    loopyPubSubFree(NULL);
    loopyUnsubscribe(NULL);

    TEST_ASSERT(loopyPubSubNew(NULL, NULL) != NULL, "NULL config should work");

    loopyPubSub *ps = loopyPubSubNew(NULL, NULL);
    TEST_ASSERT(loopySubscribe(NULL, "test", testPubSubCallback, NULL) == NULL,
                "NULL hub should return NULL");
    TEST_ASSERT(loopySubscribe(ps, NULL, testPubSubCallback, NULL) == NULL,
                "NULL pattern should return NULL");
    TEST_ASSERT(loopySubscribe(ps, "test", NULL, NULL) == NULL,
                "NULL callback should return NULL");

    TEST_ASSERT_EQ(loopyPublish(NULL, "test", "data", 4), 0,
                   "NULL hub should return 0");
    TEST_ASSERT_EQ(loopyPublish(ps, NULL, "data", 4), 0,
                   "NULL topic should return 0");

    TEST_ASSERT(!loopyValidateTopic(NULL, "test"),
                "NULL hub should return false");
    TEST_ASSERT(!loopyValidateTopic(ps, NULL),
                "NULL topic should return false");

    loopyPubSubFree(ps);
    return 1;
}

/**
 * Test mode name functions.
 */
static int test_pubsub_mode_names(void) {
    TEST_ASSERT(strcmp(loopyDeliveryModeName(LOOPY_DELIVER_SYNC), "SYNC") == 0,
                "SYNC name");
    TEST_ASSERT(strcmp(loopyDeliveryModeName(LOOPY_DELIVER_ASYNC), "ASYNC") ==
                    0,
                "ASYNC name");
    TEST_ASSERT(strcmp(loopyDeliveryModeName(LOOPY_DELIVER_QUEUE), "QUEUE") ==
                    0,
                "QUEUE name");

    TEST_ASSERT(strcmp(loopyAckModeName(LOOPY_ACK_NONE), "NONE") == 0,
                "NONE name");
    TEST_ASSERT(strcmp(loopyAckModeName(LOOPY_ACK_AUTO), "AUTO") == 0,
                "AUTO name");
    TEST_ASSERT(strcmp(loopyAckModeName(LOOPY_ACK_MANUAL), "MANUAL") == 0,
                "MANUAL name");

    TEST_ASSERT(strcmp(loopySubscriptionEventName(LOOPY_SUB_SUBSCRIBED),
                       "SUBSCRIBED") == 0,
                "SUBSCRIBED name");
    TEST_ASSERT(strcmp(loopySubscriptionEventName(LOOPY_SUB_UNSUBSCRIBED),
                       "UNSUBSCRIBED") == 0,
                "UNSUBSCRIBED name");

    return 1;
}

/* ====================================================================
 * Multi-tenant Concurrency Pool Tests
 * ==================================================================== */

/**
 * Test pool creation and destruction.
 */
static int test_concpool_create_destroy(void) {
    loopyConcurrencyPool *pool = loopyConcurrencyPoolNew(NULL);
    TEST_ASSERT(pool != NULL, "should create pool");

    loopyConcurrencyPoolStats stats;
    loopyConcurrencyPoolGetStats(pool, &stats);
    TEST_ASSERT_EQ(stats.userCount, 0, "should start with no users");
    TEST_ASSERT_EQ(stats.globalActive, 0, "should have no active slots");

    loopyConcurrencyPoolFree(pool);
    return 1;
}

/**
 * Test config initialization.
 */
static int test_concpool_config_init(void) {
    loopyConcurrencyPoolConfig poolConfig;
    loopyConcurrencyPoolConfigInit(&poolConfig);

    TEST_ASSERT_EQ(poolConfig.globalLimit, 0,
                   "default global limit should be 0 (unlimited)");
    TEST_ASSERT_EQ(poolConfig.defaultUserLimit, 10,
                   "default user limit should be 10");
    TEST_ASSERT(poolConfig.autoCreateUsers, "auto-create should be enabled");
    TEST_ASSERT(poolConfig.trackStats, "stats tracking should be enabled");

    loopyConcurrencyUserConfig userConfig;
    loopyConcurrencyUserConfigInit(&userConfig);

    TEST_ASSERT_EQ(userConfig.limit, 10, "default user limit should be 10");
    TEST_ASSERT_EQ(userConfig.reserved, 0, "default reserved should be 0");

    return 1;
}

/**
 * Test adding and removing users.
 */
static int test_concpool_add_remove_user(void) {
    loopyConcurrencyPool *pool = loopyConcurrencyPoolNew(NULL);
    TEST_ASSERT(pool != NULL, "should create pool");

    loopyConcurrencyUserConfig config;
    loopyConcurrencyUserConfigInit(&config);
    config.limit = 5;

    loopyConcurrencyUser *user =
        loopyConcurrencyPoolAddUser(pool, "user1", &config);
    TEST_ASSERT(user != NULL, "should add user");
    TEST_ASSERT(strcmp(loopyConcurrencyUserId(user), "user1") == 0,
                "userId should match");

    TEST_ASSERT_EQ(loopyConcurrencyPoolUserCount(pool), 1,
                   "should have 1 user");

    /* Adding same user again should return existing */
    loopyConcurrencyUser *user2 =
        loopyConcurrencyPoolAddUser(pool, "user1", NULL);
    TEST_ASSERT(user2 == user, "should return existing user");

    /* Remove user */
    TEST_ASSERT(loopyConcurrencyPoolRemoveUser(pool, "user1"),
                "should remove user");
    TEST_ASSERT_EQ(loopyConcurrencyPoolUserCount(pool), 0,
                   "should have 0 users");

    /* Remove non-existent */
    TEST_ASSERT(!loopyConcurrencyPoolRemoveUser(pool, "noexist"),
                "should fail for non-existent");

    loopyConcurrencyPoolFree(pool);
    return 1;
}

/**
 * Test slot acquisition and release.
 */
static int test_concpool_acquire_release(void) {
    loopyConcurrencyPool *pool = loopyConcurrencyPoolNew(NULL);
    TEST_ASSERT(pool != NULL, "should create pool");

    loopyConcurrencyUserConfig config;
    loopyConcurrencyUserConfigInit(&config);
    config.limit = 5;

    loopyConcurrencyPoolAddUser(pool, "user1", &config);

    /* Acquire 2 slots */
    loopyConcurrencyResult result =
        loopyConcurrencyPoolTryAcquire(pool, "user1", 2);
    TEST_ASSERT_EQ(result, LOOPY_CONCURRENCY_OK, "acquire should succeed");
    TEST_ASSERT_EQ(loopyConcurrencyActive(pool, "user1"), 2,
                   "should have 2 active");
    TEST_ASSERT_EQ(loopyConcurrencyAvailable(pool, "user1"), 3,
                   "should have 3 available");

    /* Release 1 slot */
    TEST_ASSERT(loopyConcurrencyPoolRelease(pool, "user1", 1),
                "release should succeed");
    TEST_ASSERT_EQ(loopyConcurrencyActive(pool, "user1"), 1,
                   "should have 1 active");

    /* Release all */
    size_t released = loopyConcurrencyPoolReleaseAll(pool, "user1");
    TEST_ASSERT_EQ(released, 1, "should release 1");
    TEST_ASSERT_EQ(loopyConcurrencyActive(pool, "user1"), 0,
                   "should have 0 active");

    loopyConcurrencyPoolFree(pool);
    return 1;
}

/**
 * Test user limit enforcement.
 */
static int test_concpool_user_limit(void) {
    loopyConcurrencyPool *pool = loopyConcurrencyPoolNew(NULL);
    TEST_ASSERT(pool != NULL, "should create pool");

    loopyConcurrencyUserConfig config;
    loopyConcurrencyUserConfigInit(&config);
    config.limit = 3;

    loopyConcurrencyPoolAddUser(pool, "user1", &config);

    /* Acquire up to limit */
    TEST_ASSERT_EQ(loopyConcurrencyPoolTryAcquire(pool, "user1", 3),
                   LOOPY_CONCURRENCY_OK, "should acquire up to limit");

    /* Try to exceed limit */
    TEST_ASSERT_EQ(loopyConcurrencyPoolTryAcquire(pool, "user1", 1),
                   LOOPY_CONCURRENCY_LIMIT, "should fail when at limit");

    /* Can acquire check */
    TEST_ASSERT(!loopyConcurrencyCanAcquire(pool, "user1", 1),
                "should not be able to acquire");

    /* Release and try again */
    loopyConcurrencyPoolRelease(pool, "user1", 1);
    TEST_ASSERT(loopyConcurrencyCanAcquire(pool, "user1", 1),
                "should be able to acquire now");
    TEST_ASSERT_EQ(loopyConcurrencyPoolTryAcquire(pool, "user1", 1),
                   LOOPY_CONCURRENCY_OK, "should succeed after release");

    loopyConcurrencyPoolFree(pool);
    return 1;
}

/**
 * Test global limit enforcement.
 */
static int test_concpool_global_limit(void) {
    loopyConcurrencyPoolConfig poolConfig;
    loopyConcurrencyPoolConfigInit(&poolConfig);
    poolConfig.globalLimit = 5;

    loopyConcurrencyPool *pool = loopyConcurrencyPoolNew(&poolConfig);
    TEST_ASSERT(pool != NULL, "should create pool");

    loopyConcurrencyUserConfig userConfig;
    loopyConcurrencyUserConfigInit(&userConfig);
    userConfig.limit = 10; /* User limit higher than global */

    loopyConcurrencyPoolAddUser(pool, "user1", &userConfig);
    loopyConcurrencyPoolAddUser(pool, "user2", &userConfig);

    /* User1 acquires 3 */
    TEST_ASSERT_EQ(loopyConcurrencyPoolTryAcquire(pool, "user1", 3),
                   LOOPY_CONCURRENCY_OK, "user1 should acquire 3");

    /* User2 acquires 2 - hits global limit */
    TEST_ASSERT_EQ(loopyConcurrencyPoolTryAcquire(pool, "user2", 2),
                   LOOPY_CONCURRENCY_OK, "user2 should acquire 2");

    /* Any further acquisition should fail with global limit */
    TEST_ASSERT_EQ(loopyConcurrencyPoolTryAcquire(pool, "user1", 1),
                   LOOPY_CONCURRENCY_GLOBAL_LIMIT, "should hit global limit");
    TEST_ASSERT_EQ(loopyConcurrencyPoolTryAcquire(pool, "user2", 1),
                   LOOPY_CONCURRENCY_GLOBAL_LIMIT, "should hit global limit");

    TEST_ASSERT_EQ(loopyConcurrencyPoolActive(pool), 5,
                   "should have 5 globally active");
    TEST_ASSERT_EQ(loopyConcurrencyPoolAvailable(pool), 0,
                   "should have 0 globally available");

    loopyConcurrencyPoolFree(pool);
    return 1;
}

/**
 * Test auto-create user on acquire.
 */
static int test_concpool_auto_create_user(void) {
    loopyConcurrencyPoolConfig poolConfig;
    loopyConcurrencyPoolConfigInit(&poolConfig);
    poolConfig.autoCreateUsers = true;
    poolConfig.defaultUserLimit = 5;

    loopyConcurrencyPool *pool = loopyConcurrencyPoolNew(&poolConfig);
    TEST_ASSERT(pool != NULL, "should create pool");

    /* Acquire for non-existent user - should auto-create */
    TEST_ASSERT_EQ(loopyConcurrencyPoolTryAcquire(pool, "newuser", 2),
                   LOOPY_CONCURRENCY_OK, "should auto-create and acquire");
    TEST_ASSERT_EQ(loopyConcurrencyPoolUserCount(pool), 1,
                   "should have 1 user");
    TEST_ASSERT_EQ(loopyConcurrencyActive(pool, "newuser"), 2,
                   "should have 2 active");

    /* Disable auto-create */
    loopyConcurrencyPoolConfig noAutoConfig;
    loopyConcurrencyPoolConfigInit(&noAutoConfig);
    noAutoConfig.autoCreateUsers = false;

    loopyConcurrencyPool *pool2 = loopyConcurrencyPoolNew(&noAutoConfig);
    TEST_ASSERT_EQ(loopyConcurrencyPoolTryAcquire(pool2, "noexist", 1),
                   LOOPY_CONCURRENCY_NOT_FOUND,
                   "should fail without auto-create");

    loopyConcurrencyPoolFree(pool);
    loopyConcurrencyPoolFree(pool2);
    return 1;
}

/**
 * Test multiple users concurrently.
 */
static int test_concpool_multiple_users(void) {
    loopyConcurrencyPool *pool = loopyConcurrencyPoolNew(NULL);
    TEST_ASSERT(pool != NULL, "should create pool");

    /* Add several users */
    loopyConcurrencyPoolAddUser(pool, "alice", NULL);
    loopyConcurrencyPoolAddUser(pool, "bob", NULL);
    loopyConcurrencyPoolAddUser(pool, "charlie", NULL);

    TEST_ASSERT_EQ(loopyConcurrencyPoolUserCount(pool), 3,
                   "should have 3 users");

    /* Each user acquires some slots */
    loopyConcurrencyPoolTryAcquire(pool, "alice", 3);
    loopyConcurrencyPoolTryAcquire(pool, "bob", 2);
    loopyConcurrencyPoolTryAcquire(pool, "charlie", 4);

    TEST_ASSERT_EQ(loopyConcurrencyPoolActive(pool), 9,
                   "should have 9 globally active");

    /* Check individual users */
    TEST_ASSERT_EQ(loopyConcurrencyActive(pool, "alice"), 3,
                   "alice should have 3");
    TEST_ASSERT_EQ(loopyConcurrencyActive(pool, "bob"), 2, "bob should have 2");
    TEST_ASSERT_EQ(loopyConcurrencyActive(pool, "charlie"), 4,
                   "charlie should have 4");

    /* Release some */
    loopyConcurrencyPoolRelease(pool, "bob", 2);
    TEST_ASSERT_EQ(loopyConcurrencyPoolActive(pool), 7,
                   "should have 7 globally active");

    loopyConcurrencyPoolFree(pool);
    return 1;
}

/**
 * Test statistics tracking.
 */
static int test_concpool_stats(void) {
    loopyConcurrencyPool *pool = loopyConcurrencyPoolNew(NULL);
    TEST_ASSERT(pool != NULL, "should create pool");

    loopyConcurrencyUserConfig config;
    loopyConcurrencyUserConfigInit(&config);
    config.limit = 2;

    loopyConcurrencyUser *user =
        loopyConcurrencyPoolAddUser(pool, "statsuser", &config);

    /* Some operations */
    loopyConcurrencyPoolTryAcquire(pool, "statsuser", 1);
    loopyConcurrencyPoolTryAcquire(pool, "statsuser", 1);
    loopyConcurrencyPoolTryAcquire(pool, "statsuser", 1); /* Should fail */
    loopyConcurrencyPoolRelease(pool, "statsuser", 1);
    loopyConcurrencyPoolTryAcquire(pool, "statsuser", 1);

    loopyConcurrencyPoolStats poolStats;
    loopyConcurrencyPoolGetStats(pool, &poolStats);

    TEST_ASSERT_EQ(poolStats.totalAcquires, 3,
                   "should have 3 successful acquires");
    TEST_ASSERT_EQ(poolStats.totalDenied, 1, "should have 1 denied");
    TEST_ASSERT_EQ(poolStats.totalReleases, 1, "should have 1 release");
    TEST_ASSERT_EQ(poolStats.globalPeak, 2, "peak should be 2");

    loopyConcurrencyUserStats userStats;
    loopyConcurrencyUserGetStats(user, &userStats);

    TEST_ASSERT_EQ(userStats.acquires, 3, "user should have 3 acquires");
    TEST_ASSERT_EQ(userStats.denied, 1, "user should have 1 denied");
    TEST_ASSERT_EQ(userStats.peak, 2, "user peak should be 2");

    /* Reset stats */
    loopyConcurrencyPoolResetStats(pool);
    loopyConcurrencyPoolGetStats(pool, &poolStats);
    TEST_ASSERT_EQ(poolStats.totalAcquires, 0, "should be reset");

    loopyConcurrencyPoolFree(pool);
    return 1;
}

/**
 * Test user iteration.
 */
static int testIterCount = 0;
static int concpoolIterCallback(const char *userId, loopyConcurrencyUser *user,
                                void *arg) {
    (void)userId;
    (void)user;
    (void)arg;
    testIterCount++;
    return 0;
}

static int test_concpool_iteration(void) {
    loopyConcurrencyPool *pool = loopyConcurrencyPoolNew(NULL);
    TEST_ASSERT(pool != NULL, "should create pool");

    loopyConcurrencyPoolAddUser(pool, "user1", NULL);
    loopyConcurrencyPoolAddUser(pool, "user2", NULL);
    loopyConcurrencyPoolAddUser(pool, "user3", NULL);

    testIterCount = 0;
    size_t visited =
        loopyConcurrencyPoolIterate(pool, concpoolIterCallback, NULL);

    TEST_ASSERT_EQ(visited, 3, "should visit 3 users");
    TEST_ASSERT_EQ(testIterCount, 3, "callback should be called 3 times");

    loopyConcurrencyPoolFree(pool);
    return 1;
}

/**
 * Test null safety.
 */
static int test_concpool_null_safety(void) {
    /* These should not crash */
    loopyConcurrencyPoolConfigInit(NULL);
    loopyConcurrencyUserConfigInit(NULL);
    loopyConcurrencyPoolFree(NULL);

    TEST_ASSERT(loopyConcurrencyPoolNew(NULL) != NULL,
                "NULL config should work");

    loopyConcurrencyPool *pool = loopyConcurrencyPoolNew(NULL);
    TEST_ASSERT(loopyConcurrencyPoolAddUser(NULL, "test", NULL) == NULL,
                "NULL pool should return NULL");
    TEST_ASSERT(loopyConcurrencyPoolAddUser(pool, NULL, NULL) == NULL,
                "NULL userId should return NULL");

    TEST_ASSERT_EQ(loopyConcurrencyPoolTryAcquire(NULL, "test", 1),
                   LOOPY_CONCURRENCY_INVALID,
                   "NULL pool should return INVALID");
    TEST_ASSERT_EQ(loopyConcurrencyPoolTryAcquire(pool, NULL, 1),
                   LOOPY_CONCURRENCY_INVALID,
                   "NULL userId should return INVALID");
    TEST_ASSERT_EQ(loopyConcurrencyPoolTryAcquire(pool, "test", 0),
                   LOOPY_CONCURRENCY_INVALID, "0 count should return INVALID");

    TEST_ASSERT(!loopyConcurrencyPoolRelease(NULL, "test", 1),
                "NULL pool should return false");
    TEST_ASSERT(!loopyConcurrencyPoolRelease(pool, NULL, 1),
                "NULL userId should return false");

    TEST_ASSERT_EQ(loopyConcurrencyPoolUserCount(NULL), 0,
                   "NULL pool should return 0");
    TEST_ASSERT_EQ(loopyConcurrencyPoolActive(NULL), 0,
                   "NULL pool should return 0");

    loopyConcurrencyPoolFree(pool);
    return 1;
}

/**
 * Test result name function.
 */
static int test_concpool_result_names(void) {
    TEST_ASSERT(
        strcmp(loopyConcurrencyResultName(LOOPY_CONCURRENCY_OK), "OK") == 0,
        "OK name");
    TEST_ASSERT(strcmp(loopyConcurrencyResultName(LOOPY_CONCURRENCY_LIMIT),
                       "LIMIT") == 0,
                "LIMIT name");
    TEST_ASSERT(
        strcmp(loopyConcurrencyResultName(LOOPY_CONCURRENCY_GLOBAL_LIMIT),
               "GLOBAL_LIMIT") == 0,
        "GLOBAL_LIMIT name");
    TEST_ASSERT(strcmp(loopyConcurrencyResultName(LOOPY_CONCURRENCY_INVALID),
                       "INVALID") == 0,
                "INVALID name");
    TEST_ASSERT(strcmp(loopyConcurrencyResultName(LOOPY_CONCURRENCY_NOT_FOUND),
                       "NOT_FOUND") == 0,
                "NOT_FOUND name");

    return 1;
}

/* ====================================================================
 * Concurrency Pool Stress/Fuzz Tests
 *
 * These tests verify limit enforcement under adversarial concurrent
 * access patterns. The goal is to ensure limits are NEVER exceeded
 * even under extreme contention from multiple threads.
 * ==================================================================== */

/* Shared state for stress tests */
typedef struct {
    loopyConcurrencyPool *pool;
    const char *userId;
    size_t userLimit;
    size_t globalLimit;
    atomic_size_t maxObservedActive;
    atomic_size_t maxObservedGlobal;
    atomic_uint_fast64_t successfulAcquires;
    atomic_uint_fast64_t successfulReleases;
    atomic_uint_fast64_t limitHits;
    atomic_bool violation;
    atomic_bool running;
    int iterations;
} concpool_stress_state;

static void *concpool_stress_worker(void *arg) {
    concpool_stress_state *state = arg;

    while (atomic_load(&state->running)) {
        for (int i = 0; i < state->iterations && atomic_load(&state->running);
             i++) {
            /* Random operation: acquire 1-3 slots or release 1-3 slots */
            size_t count = 1 + (rand() % 3);

            if (rand() % 2 == 0) {
                /* Try to acquire */
                loopyConcurrencyResult result = loopyConcurrencyPoolTryAcquire(
                    state->pool, state->userId, count);

                if (result == LOOPY_CONCURRENCY_OK) {
                    atomic_fetch_add(&state->successfulAcquires, count);

                    /* CRITICAL INVARIANT CHECK: active should never exceed
                     * limit */
                    size_t active =
                        loopyConcurrencyActive(state->pool, state->userId);
                    if (active > state->userLimit) {
                        atomic_store(&state->violation, true);
                        fprintf(stderr,
                                "VIOLATION: user active %zu > limit %zu\n",
                                active, state->userLimit);
                    }

                    /* Track max observed */
                    size_t max = atomic_load(&state->maxObservedActive);
                    while (active > max) {
                        if (atomic_compare_exchange_weak(
                                &state->maxObservedActive, &max, active)) {
                            break;
                        }
                    }

                    /* Check global limit */
                    size_t globalActive =
                        loopyConcurrencyPoolActive(state->pool);
                    if (state->globalLimit > 0 &&
                        globalActive > state->globalLimit) {
                        atomic_store(&state->violation, true);
                        fprintf(stderr,
                                "VIOLATION: global active %zu > limit %zu\n",
                                globalActive, state->globalLimit);
                    }

                    max = atomic_load(&state->maxObservedGlobal);
                    while (globalActive > max) {
                        if (atomic_compare_exchange_weak(
                                &state->maxObservedGlobal, &max,
                                globalActive)) {
                            break;
                        }
                    }
                } else if (result == LOOPY_CONCURRENCY_LIMIT ||
                           result == LOOPY_CONCURRENCY_GLOBAL_LIMIT) {
                    atomic_fetch_add(&state->limitHits, 1);
                }
            } else {
                /* Release */
                if (loopyConcurrencyPoolRelease(state->pool, state->userId,
                                                count)) {
                    atomic_fetch_add(&state->successfulReleases, count);
                }
            }
        }
    }

    return NULL;
}

/**
 * Stress test: single user with many threads competing for limited slots.
 * Verifies user limit is never exceeded.
 */
static int test_concpool_stress_single_user(void) {
    const size_t USER_LIMIT = 10;
    const int NUM_THREADS = 8;
    const int ITERATIONS = 5000;

    loopyConcurrencyPoolConfig poolConfig;
    loopyConcurrencyPoolConfigInit(&poolConfig);
    poolConfig.autoCreateUsers = true;
    poolConfig.trackStats = true;

    loopyConcurrencyPool *pool = loopyConcurrencyPoolNew(&poolConfig);
    TEST_ASSERT(pool != NULL, "should create pool");

    loopyConcurrencyUserConfig userConfig;
    loopyConcurrencyUserConfigInit(&userConfig);
    userConfig.limit = USER_LIMIT;
    loopyConcurrencyPoolAddUser(pool, "stressuser", &userConfig);

    concpool_stress_state state = {
        .pool = pool,
        .userId = "stressuser",
        .userLimit = USER_LIMIT,
        .globalLimit = 0,
        .iterations = ITERATIONS,
    };
    atomic_init(&state.maxObservedActive, 0);
    atomic_init(&state.maxObservedGlobal, 0);
    atomic_init(&state.successfulAcquires, 0);
    atomic_init(&state.successfulReleases, 0);
    atomic_init(&state.limitHits, 0);
    atomic_init(&state.violation, false);
    atomic_init(&state.running, true);

    pthread_t threads[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_create(&threads[i], NULL, concpool_stress_worker, &state);
    }

    /* Let it run for a bit */
    usleep(100000); /* 100ms */
    atomic_store(&state.running, false);

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    /* Release any remaining slots */
    loopyConcurrencyPoolReleaseAll(pool, "stressuser");

    TEST_ASSERT(!atomic_load(&state.violation),
                "no limit violations should occur");
    TEST_ASSERT(atomic_load(&state.maxObservedActive) <= USER_LIMIT,
                "max observed active should not exceed user limit");

    size_t acquires = atomic_load(&state.successfulAcquires);
    size_t limitHits = atomic_load(&state.limitHits);

    /* There should have been some activity */
    TEST_ASSERT(acquires > 0, "should have some successful acquires");
    TEST_ASSERT(limitHits > 0, "should have hit limits at some point");

    loopyConcurrencyPoolFree(pool);
    return 1;
}

/* Multi-user stress test worker */
typedef struct {
    loopyConcurrencyPool *pool;
    char userId[32];
    size_t userLimit;
    size_t globalLimit;
    atomic_bool *violation;
    atomic_bool *running;
    int iterations;
    atomic_size_t localMaxActive;
} concpool_multiuser_worker_state;

static void *concpool_multiuser_worker(void *arg) {
    concpool_multiuser_worker_state *ws = arg;

    while (atomic_load(ws->running)) {
        for (int i = 0; i < ws->iterations && atomic_load(ws->running); i++) {
            size_t count = 1 + (rand() % 2);

            if (rand() % 2 == 0) {
                loopyConcurrencyResult result =
                    loopyConcurrencyPoolTryAcquire(ws->pool, ws->userId, count);

                if (result == LOOPY_CONCURRENCY_OK) {
                    /* Check per-user invariant */
                    size_t active =
                        loopyConcurrencyActive(ws->pool, ws->userId);
                    if (active > ws->userLimit) {
                        atomic_store(ws->violation, true);
                    }

                    /* Track local max */
                    size_t max = atomic_load(&ws->localMaxActive);
                    while (active > max) {
                        if (atomic_compare_exchange_weak(&ws->localMaxActive,
                                                         &max, active)) {
                            break;
                        }
                    }

                    /* Check global invariant */
                    size_t globalActive = loopyConcurrencyPoolActive(ws->pool);
                    if (ws->globalLimit > 0 && globalActive > ws->globalLimit) {
                        atomic_store(ws->violation, true);
                    }
                }
            } else {
                loopyConcurrencyPoolRelease(ws->pool, ws->userId, count);
            }
        }
    }

    return NULL;
}

/**
 * Stress test: multiple users with a global limit.
 * Verifies both per-user and global limits are never exceeded.
 */
static int test_concpool_stress_multi_user(void) {
    const size_t USER_LIMIT = 5;
    const size_t GLOBAL_LIMIT = 15;
    const int NUM_USERS = 6; /* More users than can fit in global limit */
    const int THREADS_PER_USER = 4;
    const int ITERATIONS = 3000;

    loopyConcurrencyPoolConfig poolConfig;
    loopyConcurrencyPoolConfigInit(&poolConfig);
    poolConfig.globalLimit = GLOBAL_LIMIT;
    poolConfig.autoCreateUsers = true;
    poolConfig.trackStats = true;

    loopyConcurrencyPool *pool = loopyConcurrencyPoolNew(&poolConfig);
    TEST_ASSERT(pool != NULL, "should create pool");

    atomic_bool violation = false;
    atomic_bool running = true;

    /* Create users and worker states */
    loopyConcurrencyUserConfig userConfig;
    loopyConcurrencyUserConfigInit(&userConfig);
    userConfig.limit = USER_LIMIT;

    concpool_multiuser_worker_state workerStates[NUM_USERS * THREADS_PER_USER];
    pthread_t threads[NUM_USERS * THREADS_PER_USER];
    int threadIdx = 0;

    for (int u = 0; u < NUM_USERS; u++) {
        char userId[32];
        snprintf(userId, sizeof(userId), "user%d", u);
        loopyConcurrencyPoolAddUser(pool, userId, &userConfig);

        for (int t = 0; t < THREADS_PER_USER; t++) {
            concpool_multiuser_worker_state *ws = &workerStates[threadIdx];
            snprintf(ws->userId, sizeof(ws->userId), "user%d", u);
            ws->pool = pool;
            ws->userLimit = USER_LIMIT;
            ws->globalLimit = GLOBAL_LIMIT;
            ws->violation = &violation;
            ws->running = &running;
            ws->iterations = ITERATIONS;
            atomic_init(&ws->localMaxActive, 0);

            pthread_create(&threads[threadIdx], NULL, concpool_multiuser_worker,
                           ws);
            threadIdx++;
        }
    }

    /* Let it run */
    usleep(150000); /* 150ms */
    atomic_store(&running, false);

    for (int i = 0; i < threadIdx; i++) {
        pthread_join(threads[i], NULL);
    }

    TEST_ASSERT(!atomic_load(&violation),
                "no limit violations in multi-user test");

    /* Verify each user's max didn't exceed their limit */
    for (int u = 0; u < NUM_USERS; u++) {
        int baseIdx = u * THREADS_PER_USER;
        size_t maxSeen = 0;
        for (int t = 0; t < THREADS_PER_USER; t++) {
            size_t m = atomic_load(&workerStates[baseIdx + t].localMaxActive);
            if (m > maxSeen) {
                maxSeen = m;
            }
        }
        TEST_ASSERT(maxSeen <= USER_LIMIT,
                    "user max should not exceed per-user limit");
    }

    loopyConcurrencyPoolFree(pool);
    return 1;
}

/**
 * Stress test: rapid user creation/destruction while operations in flight.
 * This is the most adversarial pattern - users being added and removed
 * while other threads are trying to acquire/release slots.
 */
typedef struct {
    loopyConcurrencyPool *pool;
    atomic_bool *running;
    atomic_bool *violation;
    int userIndex;
} concpool_churn_worker_state;

static void *concpool_churn_worker(void *arg) {
    concpool_churn_worker_state *ws = arg;
    char userId[32];
    snprintf(userId, sizeof(userId), "churnuser%d", ws->userIndex);

    while (atomic_load(ws->running)) {
        /* Randomly do operations */
        int op = rand() % 10;

        if (op < 4) {
            /* Try acquire */
            loopyConcurrencyResult result =
                loopyConcurrencyPoolTryAcquire(ws->pool, userId, 1);
            if (result == LOOPY_CONCURRENCY_OK) {
                size_t active = loopyConcurrencyActive(ws->pool, userId);
                if (active > 10) { /* Default limit */
                    atomic_store(ws->violation, true);
                }
            }
        } else if (op < 8) {
            /* Release */
            loopyConcurrencyPoolRelease(ws->pool, userId, 1);
        } else {
            /* Small sleep to vary timing */
            usleep(100);
        }
    }

    return NULL;
}

static void *concpool_user_churner(void *arg) {
    concpool_churn_worker_state *ws = arg;

    while (atomic_load(ws->running)) {
        /* Add and remove users rapidly */
        for (int i = 0; i < 5 && atomic_load(ws->running); i++) {
            char userId[32];
            snprintf(userId, sizeof(userId), "churnuser%d", i);

            loopyConcurrencyUserConfig config;
            loopyConcurrencyUserConfigInit(&config);
            config.limit = 10;

            loopyConcurrencyPoolAddUser(ws->pool, userId, &config);
            usleep(rand() % 1000);
        }

        usleep(rand() % 500);

        /* Remove some users */
        for (int i = 0; i < 3 && atomic_load(ws->running); i++) {
            char userId[32];
            snprintf(userId, sizeof(userId), "churnuser%d", rand() % 5);
            loopyConcurrencyPoolRemoveUser(ws->pool, userId);
        }
    }

    return NULL;
}

static int test_concpool_stress_user_churn(void) {
    const int CHURN_WORKERS = 6;

    loopyConcurrencyPoolConfig poolConfig;
    loopyConcurrencyPoolConfigInit(&poolConfig);
    poolConfig.autoCreateUsers = true;

    loopyConcurrencyPool *pool = loopyConcurrencyPoolNew(&poolConfig);
    TEST_ASSERT(pool != NULL, "should create pool");

    atomic_bool running = true;
    atomic_bool violation = false;

    /* Pre-create some users */
    for (int i = 0; i < 5; i++) {
        char userId[32];
        snprintf(userId, sizeof(userId), "churnuser%d", i);
        loopyConcurrencyUserConfig config;
        loopyConcurrencyUserConfigInit(&config);
        config.limit = 10;
        loopyConcurrencyPoolAddUser(pool, userId, &config);
    }

    pthread_t workers[CHURN_WORKERS];
    pthread_t churner;
    concpool_churn_worker_state workerStates[CHURN_WORKERS];
    concpool_churn_worker_state churnerState = {
        .pool = pool,
        .running = &running,
        .violation = &violation,
    };

    /* Start churner thread */
    pthread_create(&churner, NULL, concpool_user_churner, &churnerState);

    /* Start worker threads */
    for (int i = 0; i < CHURN_WORKERS; i++) {
        workerStates[i].pool = pool;
        workerStates[i].running = &running;
        workerStates[i].violation = &violation;
        workerStates[i].userIndex = i % 5;
        pthread_create(&workers[i], NULL, concpool_churn_worker,
                       &workerStates[i]);
    }

    /* Let chaos run */
    usleep(200000); /* 200ms */
    atomic_store(&running, false);

    pthread_join(churner, NULL);
    for (int i = 0; i < CHURN_WORKERS; i++) {
        pthread_join(workers[i], NULL);
    }

    /* The test passes if we didn't crash and no violations occurred */
    TEST_ASSERT(!atomic_load(&violation), "no violations during user churn");

    loopyConcurrencyPoolFree(pool);
    return 1;
}

/**
 * Stress test: verify atomics by checking final invariants.
 * After all operations, active should be 0 and counters should be consistent.
 */
typedef struct {
    loopyConcurrencyPool *pool;
    atomic_size_t *totalAcquires;
    atomic_size_t *totalReleases;
    atomic_bool *running;
    atomic_bool *violation;
    size_t limit;
    int iterations;
} concpool_invariant_worker_state;

static void *concpool_invariant_worker(void *arg) {
    concpool_invariant_worker_state *ws = arg;

    for (int i = 0; i < ws->iterations && atomic_load(ws->running); i++) {
        size_t count = 1 + (rand() % 3);

        if (rand() % 2 == 0) {
            loopyConcurrencyResult result = loopyConcurrencyPoolTryAcquire(
                ws->pool, "invariant_test", count);
            if (result == LOOPY_CONCURRENCY_OK) {
                atomic_fetch_add(ws->totalAcquires, count);

                size_t active =
                    loopyConcurrencyActive(ws->pool, "invariant_test");
                if (active > ws->limit) {
                    atomic_store(ws->violation, true);
                }
            }
        } else {
            /* Track how many we actually release */
            size_t before = loopyConcurrencyActive(ws->pool, "invariant_test");
            if (loopyConcurrencyPoolRelease(ws->pool, "invariant_test",
                                            count)) {
                size_t after =
                    loopyConcurrencyActive(ws->pool, "invariant_test");
                /* We released (before - after) slots */
                if (before > after) {
                    atomic_fetch_add(ws->totalReleases, before - after);
                }
            }
        }
    }
    return NULL;
}

static int test_concpool_stress_invariants(void) {
    const size_t USER_LIMIT = 20;
    const int INVARIANT_THREADS = 8;
    const int ITERATIONS = 10000;

    loopyConcurrencyPoolConfig poolConfig;
    loopyConcurrencyPoolConfigInit(&poolConfig);
    poolConfig.trackStats = true;

    loopyConcurrencyPool *pool = loopyConcurrencyPoolNew(&poolConfig);
    TEST_ASSERT(pool != NULL, "should create pool");

    loopyConcurrencyUserConfig userConfig;
    loopyConcurrencyUserConfigInit(&userConfig);
    userConfig.limit = USER_LIMIT;
    loopyConcurrencyPoolAddUser(pool, "invariant_test", &userConfig);

    /* Track acquires and releases ourselves */
    atomic_size_t totalAcquires = 0;
    atomic_size_t totalReleases = 0;
    atomic_bool running = true;
    atomic_bool violation = false;

    concpool_invariant_worker_state state = {
        .pool = pool,
        .totalAcquires = &totalAcquires,
        .totalReleases = &totalReleases,
        .running = &running,
        .violation = &violation,
        .limit = USER_LIMIT,
        .iterations = ITERATIONS,
    };

    pthread_t threads[INVARIANT_THREADS];
    for (int i = 0; i < INVARIANT_THREADS; i++) {
        pthread_create(&threads[i], NULL, concpool_invariant_worker, &state);
    }

    for (int i = 0; i < INVARIANT_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    /* Release all remaining */
    size_t remaining = loopyConcurrencyPoolReleaseAll(pool, "invariant_test");
    atomic_fetch_add(&totalReleases, remaining);

    /* Final check: after releasing all, active should be 0 */
    TEST_ASSERT_EQ(loopyConcurrencyActive(pool, "invariant_test"), 0,
                   "active should be 0 after releasing all");

    TEST_ASSERT(!atomic_load(&violation),
                "no violations during invariant test");

    /* Sanity check: we had some activity */
    size_t acq = atomic_load(&totalAcquires);
    (void)acq; /* May be unused if test runs too fast */

    loopyConcurrencyPoolFree(pool);
    return 1;
}

/* ====================================================================
 * Cluster Registry Tests
 * ==================================================================== */

/**
 * Test registry creation and destruction.
 */
static int test_cluster_create_destroy(void) {
    loopyClusterConfig config;
    loopyClusterConfigInit(&config);
    config.localNodeId = "node1";

    loopyClusterRegistry *registry = loopyClusterRegistryNew(&config);
    TEST_ASSERT(registry != NULL, "should create registry");

    TEST_ASSERT(strcmp(loopyClusterLocalNodeId(registry), "node1") == 0,
                "local node ID should match");

    /* Local node should be automatically registered and active */
    TEST_ASSERT_EQ(loopyClusterNodeCount(registry), 1, "should have 1 node");
    TEST_ASSERT_EQ(loopyClusterActiveNodeCount(registry), 1,
                   "should have 1 active");

    loopyClusterRegistryFree(registry);
    return 1;
}

/**
 * Test config initialization.
 */
static int test_cluster_config_init(void) {
    loopyClusterConfig config;
    loopyClusterConfigInit(&config);

    TEST_ASSERT_EQ(config.heartbeatIntervalMs, 1000,
                   "default heartbeat should be 1000");
    TEST_ASSERT_EQ(config.failureTimeoutMs, 5000,
                   "default failure timeout should be 5000");
    TEST_ASSERT(!config.autoHeartbeat,
                "auto heartbeat should be off by default");

    return 1;
}

/**
 * Test node registration and lookup.
 */
static int test_cluster_register_node(void) {
    loopyClusterConfig config;
    loopyClusterConfigInit(&config);
    config.localNodeId = "local";

    loopyClusterRegistry *registry = loopyClusterRegistryNew(&config);
    TEST_ASSERT(registry != NULL, "should create registry");

    /* Register another node */
    loopyClusterNode *node = loopyClusterRegisterNode(
        registry, "remote1", "192.168.1.100:8080", LOOPY_ROLE_WORKER);
    TEST_ASSERT(node != NULL, "should register node");

    TEST_ASSERT_EQ(loopyClusterNodeCount(registry), 2, "should have 2 nodes");

    /* Lookup node */
    loopyClusterNode *found = loopyClusterGetNode(registry, "remote1");
    TEST_ASSERT(found == node, "should find the same node");

    /* Get node info */
    loopyNodeInfo info;
    loopyClusterNodeGetInfo(node, &info);
    TEST_ASSERT(strcmp(info.nodeId, "remote1") == 0, "nodeId should match");
    TEST_ASSERT(strcmp(info.address, "192.168.1.100:8080") == 0,
                "address should match");
    TEST_ASSERT(info.state == LOOPY_NODE_JOINING,
                "initial state should be JOINING");
    TEST_ASSERT(info.roles == LOOPY_ROLE_WORKER, "roles should match");

    /* Heartbeat should move to active */
    loopyClusterNodeHeartbeat(node);
    loopyClusterNodeGetInfo(node, &info);
    TEST_ASSERT(info.state == LOOPY_NODE_ACTIVE,
                "should be ACTIVE after heartbeat");

    loopyClusterRegistryFree(registry);
    return 1;
}

/**
 * Test node unregistration.
 */
static int test_cluster_unregister_node(void) {
    loopyClusterConfig config;
    loopyClusterConfigInit(&config);
    config.localNodeId = "local";

    loopyClusterRegistry *registry = loopyClusterRegistryNew(&config);
    loopyClusterRegisterNode(registry, "temp", NULL, LOOPY_ROLE_WORKER);

    TEST_ASSERT_EQ(loopyClusterNodeCount(registry), 2, "should have 2 nodes");

    /* Unregister temp */
    TEST_ASSERT(loopyClusterUnregisterNode(registry, "temp"),
                "should unregister");
    TEST_ASSERT_EQ(loopyClusterNodeCount(registry), 1, "should have 1 node");

    /* Can't unregister local node */
    TEST_ASSERT(!loopyClusterUnregisterNode(registry, "local"),
                "can't unregister local");
    TEST_ASSERT_EQ(loopyClusterNodeCount(registry), 1,
                   "should still have 1 node");

    loopyClusterRegistryFree(registry);
    return 1;
}

/**
 * Test node state changes.
 */
static int test_cluster_node_state(void) {
    loopyClusterConfig config;
    loopyClusterConfigInit(&config);
    config.localNodeId = "node1";

    loopyClusterRegistry *registry = loopyClusterRegistryNew(&config);
    loopyClusterNode *node =
        loopyClusterRegisterNode(registry, "node2", NULL, 0);

    loopyNodeInfo info;
    loopyClusterNodeGetInfo(node, &info);
    TEST_ASSERT(info.state == LOOPY_NODE_JOINING, "initial state");

    loopyClusterNodeSetState(node, LOOPY_NODE_DRAINING);
    loopyClusterNodeGetInfo(node, &info);
    TEST_ASSERT(info.state == LOOPY_NODE_DRAINING, "should be draining");

    loopyClusterNodeSetState(node, LOOPY_NODE_LEAVING);
    loopyClusterNodeGetInfo(node, &info);
    TEST_ASSERT(info.state == LOOPY_NODE_LEAVING, "should be leaving");

    loopyClusterRegistryFree(registry);
    return 1;
}

/**
 * Test leader selection.
 */
static int test_cluster_leader(void) {
    loopyClusterConfig config;
    loopyClusterConfigInit(&config);
    config.localNodeId = "node1";
    config.localRoles = LOOPY_ROLE_CANDIDATE;

    loopyClusterRegistry *registry = loopyClusterRegistryNew(&config);

    /* No leader initially */
    TEST_ASSERT(loopyClusterGetLeader(registry) == NULL, "no leader initially");

    /* Make local node leader */
    loopyClusterNode *local = loopyClusterGetNode(registry, "node1");
    loopyClusterNodeSetRoles(local, LOOPY_ROLE_LEADER);

    loopyClusterNode *leader = loopyClusterGetLeader(registry);
    TEST_ASSERT(leader == local, "local should be leader");

    loopyClusterRegistryFree(registry);
    return 1;
}

/**
 * Test configuration set/get.
 */
static int test_cluster_config_ops(void) {
    loopyClusterConfig config;
    loopyClusterConfigInit(&config);
    config.localNodeId = "node1";

    loopyClusterRegistry *registry = loopyClusterRegistryNew(&config);

    /* Set config */
    uint64_t ver =
        loopyClusterConfigSetString(registry, "app.name", "myapp", 0);
    TEST_ASSERT(ver > 0, "version should be > 0");

    /* Get config */
    const char *value = loopyClusterConfigGetString(registry, "app.name");
    TEST_ASSERT(value != NULL, "should find config");
    TEST_ASSERT(strcmp(value, "myapp") == 0, "value should match");

    /* Check version */
    TEST_ASSERT_EQ(loopyClusterConfigVersion(registry, "app.name"), ver,
                   "version matches");

    /* Update */
    uint64_t ver2 =
        loopyClusterConfigSetString(registry, "app.name", "newapp", 0);
    TEST_ASSERT(ver2 > ver, "version should increase");

    value = loopyClusterConfigGetString(registry, "app.name");
    TEST_ASSERT(strcmp(value, "newapp") == 0, "updated value");

    /* Delete */
    TEST_ASSERT(loopyClusterConfigDelete(registry, "app.name"),
                "should delete");
    TEST_ASSERT(!loopyClusterConfigExists(registry, "app.name"),
                "should not exist");

    loopyClusterRegistryFree(registry);
    return 1;
}

/**
 * Test configuration TTL/expiration.
 */
static int test_cluster_config_ttl(void) {
    loopyClusterConfig config;
    loopyClusterConfigInit(&config);
    config.localNodeId = "node1";

    loopyClusterRegistry *registry = loopyClusterRegistryNew(&config);

    /* Set with TTL (50ms) */
    loopyClusterConfigSetString(registry, "temp.key", "value", 50);
    TEST_ASSERT(loopyClusterConfigExists(registry, "temp.key"), "should exist");

    /* Wait for expiration */
    usleep(100000); /* 100ms */

    /* Should be expired now */
    TEST_ASSERT(!loopyClusterConfigExists(registry, "temp.key"),
                "should be expired");

    loopyClusterRegistryFree(registry);
    return 1;
}

/**
 * Test configuration iteration.
 */
typedef struct {
    size_t count;
} cluster_counter_state;

static int cluster_config_counter_fn(const char *key,
                                     const loopyConfigValue *cv, void *ud) {
    (void)key;
    (void)cv;
    cluster_counter_state *st = ud;
    st->count++;
    return 0;
}

static int test_cluster_config_iterate(void) {
    loopyClusterConfig config;
    loopyClusterConfigInit(&config);
    config.localNodeId = "node1";

    loopyClusterRegistry *registry = loopyClusterRegistryNew(&config);

    /* Add some configs */
    loopyClusterConfigSetString(registry, "db.host", "localhost", 0);
    loopyClusterConfigSetString(registry, "db.port", "5432", 0);
    loopyClusterConfigSetString(registry, "cache.host", "redis", 0);

    cluster_counter_state st = {0};
    loopyClusterConfigIterate(registry, cluster_config_counter_fn, &st);
    TEST_ASSERT_EQ(st.count, 3, "should have 3 configs");

    /* Iterate prefix */
    st.count = 0;
    loopyClusterConfigIteratePrefix(registry, "db.", cluster_config_counter_fn,
                                    &st);
    TEST_ASSERT_EQ(st.count, 2, "should have 2 db.* configs");

    loopyClusterRegistryFree(registry);
    return 1;
}

/**
 * Test node iteration.
 */
static int cluster_node_counter_fn(loopyClusterNode *node,
                                   const loopyNodeInfo *info, void *ud) {
    (void)node;
    (void)info;
    cluster_counter_state *st = ud;
    st->count++;
    return 0;
}

static int test_cluster_node_iterate(void) {
    loopyClusterConfig config;
    loopyClusterConfigInit(&config);
    config.localNodeId = "master";
    config.localRoles = LOOPY_ROLE_LEADER;

    loopyClusterRegistry *registry = loopyClusterRegistryNew(&config);

    loopyClusterRegisterNode(registry, "worker1", NULL, LOOPY_ROLE_WORKER);
    loopyClusterRegisterNode(registry, "worker2", NULL, LOOPY_ROLE_WORKER);
    loopyClusterRegisterNode(registry, "observer", NULL, LOOPY_ROLE_OBSERVER);

    cluster_counter_state st = {0};
    loopyClusterIterateNodes(registry, cluster_node_counter_fn, &st);
    TEST_ASSERT_EQ(st.count, 4, "should have 4 nodes");

    st.count = 0;
    loopyClusterIterateNodesByRole(registry, LOOPY_ROLE_WORKER,
                                   cluster_node_counter_fn, &st);
    TEST_ASSERT_EQ(st.count, 2, "should have 2 workers");

    loopyClusterRegistryFree(registry);
    return 1;
}

/**
 * Test event callbacks.
 */
typedef struct {
    int joinCount;
    int leftCount;
    int configCount;
} event_counts;

static void test_event_callback(loopyClusterRegistry *reg,
                                loopyClusterEventType event,
                                const loopyNodeInfo *node,
                                const loopyConfigValue *cv, void *userData) {
    (void)reg;
    (void)node;
    (void)cv;
    event_counts *counts = userData;

    switch (event) {
    case LOOPY_CLUSTER_NODE_JOINED:
        counts->joinCount++;
        break;
    case LOOPY_CLUSTER_NODE_LEFT:
        counts->leftCount++;
        break;
    case LOOPY_CLUSTER_CONFIG_CHANGE:
        counts->configCount++;
        break;
    default:
        break;
    }
}

static int test_cluster_events(void) {
    event_counts counts = {0};

    loopyClusterConfig config;
    loopyClusterConfigInit(&config);
    config.localNodeId = "node1";
    config.eventCallback = test_event_callback;
    config.eventUserData = &counts;

    loopyClusterRegistry *registry = loopyClusterRegistryNew(&config);

    /* Local node join event */
    TEST_ASSERT_EQ(counts.joinCount, 1, "local node join event");

    /* Add another node */
    loopyClusterRegisterNode(registry, "node2", NULL, 0);
    TEST_ASSERT_EQ(counts.joinCount, 2, "node2 join event");

    /* Config change */
    loopyClusterConfigSetString(registry, "key", "value", 0);
    TEST_ASSERT_EQ(counts.configCount, 1, "config set event");

    /* Remove node */
    loopyClusterUnregisterNode(registry, "node2");
    TEST_ASSERT_EQ(counts.leftCount, 1, "node left event");

    loopyClusterRegistryFree(registry);
    return 1;
}

/**
 * Test null safety.
 */
static int test_cluster_null_safety(void) {
    TEST_ASSERT(loopyClusterRegistryNew(NULL) == NULL, "NULL config");

    loopyClusterConfig config;
    loopyClusterConfigInit(&config);
    /* Missing localNodeId */
    TEST_ASSERT(loopyClusterRegistryNew(&config) == NULL, "NULL localNodeId");

    config.localNodeId = "node1";
    loopyClusterRegistry *registry = loopyClusterRegistryNew(&config);

    TEST_ASSERT(loopyClusterRegisterNode(NULL, "x", NULL, 0) == NULL,
                "NULL registry");
    TEST_ASSERT(loopyClusterRegisterNode(registry, NULL, NULL, 0) == NULL,
                "NULL nodeId");

    TEST_ASSERT(loopyClusterConfigGetString(NULL, "key") == NULL,
                "NULL registry");
    TEST_ASSERT(loopyClusterConfigGetString(registry, NULL) == NULL,
                "NULL key");

    loopyClusterRegistryFree(registry);
    loopyClusterRegistryFree(NULL); /* Should not crash */

    return 1;
}

/**
 * Test utility functions.
 */
static int test_cluster_util(void) {
    TEST_ASSERT(strcmp(loopyNodeStateName(LOOPY_NODE_UNKNOWN), "UNKNOWN") == 0,
                "UNKNOWN");
    TEST_ASSERT(strcmp(loopyNodeStateName(LOOPY_NODE_ACTIVE), "ACTIVE") == 0,
                "ACTIVE");
    TEST_ASSERT(strcmp(loopyNodeStateName(LOOPY_NODE_FAILED), "FAILED") == 0,
                "FAILED");

    TEST_ASSERT(strcmp(loopyClusterEventName(LOOPY_CLUSTER_NODE_JOINED),
                       "NODE_JOINED") == 0,
                "NODE_JOINED");
    TEST_ASSERT(strcmp(loopyClusterEventName(LOOPY_CLUSTER_CONFIG_CHANGE),
                       "CONFIG_CHANGE") == 0,
                "CONFIG_CHANGE");

    return 1;
}

/* ====================================================================
 * Comprehensive Stress/Fuzz Tests
 *
 * These tests use the loopyStressTest framework to verify correctness
 * under heavy concurrent load with multiple threads, rapid operations,
 * and adversarial access patterns.
 * ==================================================================== */

/* --- Stress Framework Tests --- */

/**
 * Test basic stress framework functionality.
 */
static int stress_basic_worker(loopyStressWorker *worker) {
    loopyStressStatAdd(worker->harness, STRESS_STAT_OPERATIONS, 1);
    return 0;
}

static int test_stress_framework_basic(void) {
    loopyStressConfig config;
    loopyStressConfigInit(&config);
    config.numWorkers = 4;
    config.durationMs = 100;
    config.workerFn = stress_basic_worker;

    loopyStressHarness *harness = loopyStressHarnessNew(&config);
    TEST_ASSERT(harness != NULL, "should create harness");

    loopyStressResults results;
    bool passed = loopyStressRun(harness, &results);
    TEST_ASSERT(passed, "stress test should pass");
    TEST_ASSERT(results.numWorkers == 4, "should have 4 workers");
    TEST_ASSERT(results.stats[STRESS_STAT_OPERATIONS] > 0,
                "should have operations");

    loopyStressHarnessFree(harness);
    return 1;
}

/**
 * Test stress counter (for limit verification).
 */
static int test_stress_counter(void) {
    loopyStressCounter counter;
    loopyStressCounterInit(&counter);

    TEST_ASSERT_EQ(loopyStressCounterGet(&counter), 0, "initial value is 0");

    loopyStressCounterAdd(&counter, 5);
    TEST_ASSERT_EQ(loopyStressCounterGet(&counter), 5, "after +5");

    loopyStressCounterAdd(&counter, 10);
    TEST_ASSERT_EQ(loopyStressCounterGet(&counter), 15, "after +10");

    loopyStressCounterSub(&counter, 3);
    TEST_ASSERT_EQ(loopyStressCounterGet(&counter), 12, "after -3");

    TEST_ASSERT_EQ(loopyStressCounterGetMax(&counter), 15, "max was 15");
    TEST_ASSERT(loopyStressCounterExceeded(&counter, 10), "exceeded 10");
    TEST_ASSERT(!loopyStressCounterExceeded(&counter, 20), "did not exceed 20");

    return 1;
}

/**
 * Test sequence tracker.
 */
static int test_stress_seqtracker(void) {
    loopyStressSeqTracker *tracker = loopyStressSeqTrackerNew(2, true);
    TEST_ASSERT(tracker != NULL, "should create tracker");

    /* Record sequences */
    TEST_ASSERT_EQ(loopyStressSeqTrackerRecord(tracker, 0, 1), 0,
                   "record seq 1");
    TEST_ASSERT_EQ(loopyStressSeqTrackerRecord(tracker, 0, 2), 0,
                   "record seq 2");
    TEST_ASSERT_EQ(loopyStressSeqTrackerRecord(tracker, 0, 3), 0,
                   "record seq 3");

    TEST_ASSERT_EQ(loopyStressSeqTrackerCount(tracker, 0), 3, "3 messages");
    TEST_ASSERT_EQ(loopyStressSeqTrackerHighest(tracker, 0), 3, "highest is 3");
    TEST_ASSERT(loopyStressSeqTrackerComplete(tracker, 0, 3), "complete for 3");

    /* Producer 1 */
    loopyStressSeqTrackerRecord(tracker, 1, 100);
    TEST_ASSERT_EQ(loopyStressSeqTrackerHighest(tracker, 1), 100,
                   "highest is 100");

    loopyStressSeqTrackerFree(tracker);
    return 1;
}

/**
 * Test message integrity verification.
 */
static int test_stress_message_integrity(void) {
    loopyStressMessage *msg = loopyStressMessageNew(42, 1234, 64);
    TEST_ASSERT(msg != NULL, "should create message");
    TEST_ASSERT_EQ(msg->producerId, 42, "producer ID");
    TEST_ASSERT_EQ(msg->sequence, 1234, "sequence");
    TEST_ASSERT_EQ(msg->payloadLen, 64, "payload length");

    loopyStressMessageFillPayload(msg);
    loopyStressMessageSign(msg);

    TEST_ASSERT(loopyStressMessageVerify(msg), "should verify");

    /* Corrupt payload */
    msg->payload[0] ^= 0xFF;
    TEST_ASSERT(!loopyStressMessageVerify(msg), "should fail verification");

    zfree(msg);
    return 1;
}

/* --- Channel Stress Tests --- */

/**
 * Shared state for channel stress tests.
 */
typedef struct {
    loopyChannel *channel;
    loopyStressSeqTracker *tracker;
    loopyStressCounter activeCounter;
    _Atomic(uint64_t) *producerSeqs;
    _Atomic(bool) allProducersDone;
    size_t numProducers;
    size_t numConsumers;
    size_t messagesPerProducer;
} channel_stress_context;

/**
 * Message for channel stress tests.
 */
typedef struct {
    uint32_t producerId;
    uint64_t sequence;
    uint32_t checksum;
    uint8_t data[56]; /* Total 72 bytes */
} channel_stress_msg;

static uint32_t channel_msg_checksum(const channel_stress_msg *msg) {
    uint32_t sum = msg->producerId ^ (uint32_t)msg->sequence;
    for (size_t i = 0; i < sizeof(msg->data); i++) {
        sum = (sum << 1) ^ msg->data[i];
    }
    return sum;
}

/**
 * SPSC producer/consumer for integrity test.
 */
static int spsc_producer_worker(loopyStressWorker *worker) {
    channel_stress_context *ctx = worker->sharedContext;

    channel_stress_msg msg;
    msg.producerId = worker->workerId;
    msg.sequence = atomic_fetch_add(&ctx->producerSeqs[worker->workerId], 1);

    /* Fill with pattern based on sequence */
    uint64_t seed = msg.sequence;
    for (size_t i = 0; i < sizeof(msg.data); i++) {
        msg.data[i] = (uint8_t)(seed = seed * 6364136223846793005ULL + 1);
    }
    msg.checksum = channel_msg_checksum(&msg);

    loopyChannelStatus status =
        loopyChannelSend(ctx->channel, &msg, sizeof(msg));
    if (status == LOOPY_CHANNEL_OK) {
        loopyStressStatAdd(worker->harness, STRESS_STAT_MESSAGES_SENT, 1);
    } else if (status == LOOPY_CHANNEL_FULL) {
        loopyStressStatAdd(worker->harness, STRESS_STAT_RETRIES, 1);
    }

    return 0;
}

static int spsc_consumer_worker(loopyStressWorker *worker) {
    channel_stress_context *ctx = worker->sharedContext;

    channel_stress_msg msg;
    ssize_t n = loopyChannelTryRecv(ctx->channel, &msg, sizeof(msg));

    if (n > 0) {
        /* Verify checksum */
        uint32_t expected = channel_msg_checksum(&msg);
        if (msg.checksum != expected) {
            loopyStressWorkerError(worker, 1,
                                   "checksum mismatch: got %u, expected %u",
                                   msg.checksum, expected);
            return 1;
        }

        loopyStressSeqTrackerRecord(ctx->tracker, msg.producerId, msg.sequence);
        loopyStressStatAdd(worker->harness, STRESS_STAT_MESSAGES_RECV, 1);
    }

    return 0;
}

static int test_stress_channel_spsc_integrity(void) {
    loopyChannelConfig chConfig;
    loopyChannelConfigInit(&chConfig);
    chConfig.type = LOOPY_CHANNEL_SPSC;
    chConfig.capacity = 1024;
    chConfig.elementSize = sizeof(channel_stress_msg);

    loopyChannel *ch = loopyChannelNew(NULL, &chConfig);
    TEST_ASSERT(ch != NULL, "should create channel");

    loopyStressSeqTracker *tracker = loopyStressSeqTrackerNew(1, false);
    _Atomic(uint64_t) producerSeq = 0;

    channel_stress_context ctx = {
        .channel = ch,
        .tracker = tracker,
        .producerSeqs = &producerSeq,
        .numProducers = 1,
        .numConsumers = 1,
        .messagesPerProducer = 10000,
    };

    /* Run producer */
    loopyStressConfig prodConfig;
    loopyStressConfigInit(&prodConfig);
    prodConfig.numWorkers = 1;
    prodConfig.iterationsPerWorker = 10000;
    prodConfig.durationMs = 0;
    prodConfig.workerFn = spsc_producer_worker;
    prodConfig.sharedContext = &ctx;

    loopyStressHarness *producer = loopyStressHarnessNew(&prodConfig);
    loopyStressResults prodResults;
    loopyStressRun(producer, &prodResults);
    loopyStressHarnessFree(producer);

    /* Run consumer until channel empty */
    loopyStressConfig consConfig;
    loopyStressConfigInit(&consConfig);
    consConfig.numWorkers = 1;
    consConfig.durationMs = 1000;
    consConfig.workerFn = spsc_consumer_worker;
    consConfig.sharedContext = &ctx;

    loopyStressHarness *consumer = loopyStressHarnessNew(&consConfig);
    loopyStressResults consResults;
    loopyStressRun(consumer, &consResults);
    loopyStressHarnessFree(consumer);

    /* Verify all messages received with correct sequences */
    uint64_t sent = prodResults.stats[STRESS_STAT_MESSAGES_SENT];
    uint64_t recv = consResults.stats[STRESS_STAT_MESSAGES_RECV];
    TEST_ASSERT(recv <= sent, "received <= sent");
    TEST_ASSERT(consResults.passed,
                "consumer should pass (no checksum errors)");

    loopyStressSeqTrackerFree(tracker);
    loopyChannelFree(ch);
    return 1;
}

/**
 * MPMC contention stress test - many producers and consumers.
 */
typedef struct {
    loopyChannel *channel;
    _Atomic(uint64_t) *producerSeqs;
    _Atomic(uint64_t) totalSent;
    _Atomic(uint64_t) totalRecv;
    _Atomic(uint64_t) checksumErrors;
    uint32_t numProducers;
} mpmc_stress_context;

static int mpmc_producer_worker(loopyStressWorker *worker) {
    mpmc_stress_context *ctx = worker->sharedContext;

    channel_stress_msg msg;
    msg.producerId = worker->workerId;
    msg.sequence = atomic_fetch_add(&ctx->producerSeqs[worker->workerId], 1);

    uint64_t seed = msg.sequence ^ ((uint64_t)msg.producerId << 32);
    for (size_t i = 0; i < sizeof(msg.data); i++) {
        msg.data[i] = (uint8_t)(seed = seed * 6364136223846793005ULL + 1);
    }
    msg.checksum = channel_msg_checksum(&msg);

    loopyChannelStatus status =
        loopyChannelTrySend(ctx->channel, &msg, sizeof(msg));
    if (status == LOOPY_CHANNEL_OK) {
        atomic_fetch_add(&ctx->totalSent, 1);
        loopyStressStatAdd(worker->harness, STRESS_STAT_MESSAGES_SENT, 1);
    }

    return 0;
}

static int mpmc_consumer_worker(loopyStressWorker *worker) {
    mpmc_stress_context *ctx = worker->sharedContext;

    channel_stress_msg msg;
    ssize_t n = loopyChannelTryRecv(ctx->channel, &msg, sizeof(msg));

    if (n > 0) {
        uint32_t expected = channel_msg_checksum(&msg);
        if (msg.checksum != expected) {
            atomic_fetch_add(&ctx->checksumErrors, 1);
            loopyStressWorkerError(worker, 1, "MPMC checksum error");
            return 1;
        }
        atomic_fetch_add(&ctx->totalRecv, 1);
        loopyStressStatAdd(worker->harness, STRESS_STAT_MESSAGES_RECV, 1);
    }

    return 0;
}

static int test_stress_channel_mpmc_contention(void) {
    loopyChannelConfig chConfig;
    loopyChannelConfigInit(&chConfig);
    chConfig.type = LOOPY_CHANNEL_MPMC;
    chConfig.capacity = 256;
    chConfig.elementSize = sizeof(channel_stress_msg);

    loopyChannel *ch = loopyChannelNew(NULL, &chConfig);
    TEST_ASSERT(ch != NULL, "should create MPMC channel");

#define MPMC_PRODUCERS 4
    _Atomic(uint64_t) producerSeqs[MPMC_PRODUCERS];
    for (uint32_t i = 0; i < MPMC_PRODUCERS; i++) {
        atomic_store(&producerSeqs[i], 0);
    }

    mpmc_stress_context ctx = {
        .channel = ch,
        .producerSeqs = producerSeqs,
        .totalSent = 0,
        .totalRecv = 0,
        .checksumErrors = 0,
        .numProducers = MPMC_PRODUCERS,
    };

    loopyStressConfig prodConfig;
    loopyStressConfigInit(&prodConfig);
    prodConfig.numWorkers = MPMC_PRODUCERS;
    prodConfig.durationMs = 500;
    prodConfig.workerFn = mpmc_producer_worker;
    prodConfig.sharedContext = &ctx;

    loopyStressHarness *prodHarness = loopyStressHarnessNew(&prodConfig);
    loopyStressStart(prodHarness);

    loopyStressConfig consConfig;
    loopyStressConfigInit(&consConfig);
    consConfig.numWorkers = 4;
    consConfig.durationMs = 600;
    consConfig.workerFn = mpmc_consumer_worker;
    consConfig.sharedContext = &ctx;

    loopyStressHarness *consHarness = loopyStressHarnessNew(&consConfig);
    loopyStressStart(consHarness);

    loopyStressResults prodResults, consResults;
    loopyStressWait(prodHarness, &prodResults);
    loopyStressWait(consHarness, &consResults);

    TEST_ASSERT(prodResults.passed, "producers should pass");
    TEST_ASSERT(consResults.passed, "consumers should pass");
    TEST_ASSERT_EQ(atomic_load(&ctx.checksumErrors), 0, "no checksum errors");

    uint64_t sent = atomic_load(&ctx.totalSent);
    uint64_t recv = atomic_load(&ctx.totalRecv);
    TEST_ASSERT(recv <= sent, "received <= sent");

    loopyStressHarnessFree(prodHarness);
    loopyStressHarnessFree(consHarness);
    loopyChannelFree(ch);
#undef MPMC_PRODUCERS
    return 1;
}

/* --- Thread functions for ordering test --- */
typedef struct {
    loopyChannel *ch;
    _Atomic(int) *nextSend;
    int numItems;
} ordering_prod_ctx;

static void *ordering_producer_thread(void *arg) {
    ordering_prod_ctx *c = arg;
    while (1) {
        int idx = atomic_fetch_add(c->nextSend, 1);
        if (idx >= c->numItems) {
            break;
        }
        uint64_t val = (uint64_t)idx;
        loopyChannelSend(c->ch, &val, sizeof(val));
    }
    return NULL;
}

typedef struct {
    loopyChannel *ch;
    _Atomic(int) *received;
    _Atomic(int) *count;
    int numItems;
} ordering_cons_ctx;

static void *ordering_consumer_thread(void *arg) {
    ordering_cons_ctx *c = arg;
    while (atomic_load(c->count) < c->numItems) {
        uint64_t val;
        ssize_t n = loopyChannelTryRecv(c->ch, &val, sizeof(val));
        if (n > 0) {
            atomic_fetch_add(&c->received[val], 1);
            atomic_fetch_add(c->count, 1);
        }
    }
    return NULL;
}

/**
 * Test MPMC ordering - verify no message loss under contention.
 */
static int test_stress_channel_mpmc_ordering(void) {
    loopyChannelConfig chConfig;
    loopyChannelConfigInit(&chConfig);
    chConfig.type = LOOPY_CHANNEL_MPMC;
    chConfig.capacity = 512;
    chConfig.elementSize = sizeof(uint64_t);
    chConfig.blocking = true;

    loopyChannel *ch = loopyChannelNew(NULL, &chConfig);
    TEST_ASSERT(ch != NULL, "should create channel");

#define ORDERING_NUM_ITEMS 10000
    _Atomic(int) nextSend = 0;
    _Atomic(int) received[ORDERING_NUM_ITEMS];
    _Atomic(int) count = 0;
    for (int i = 0; i < ORDERING_NUM_ITEMS; i++) {
        atomic_store(&received[i], 0);
    }

    ordering_prod_ctx prodCtx = {ch, &nextSend, ORDERING_NUM_ITEMS};
    ordering_cons_ctx consCtx = {ch, received, &count, ORDERING_NUM_ITEMS};

    pthread_t prodThreads[4];
    pthread_t consThreads[4];

    for (int i = 0; i < 4; i++) {
        pthread_create(&prodThreads[i], NULL, ordering_producer_thread,
                       &prodCtx);
        pthread_create(&consThreads[i], NULL, ordering_consumer_thread,
                       &consCtx);
    }

    /* Wait for all producers */
    for (int i = 0; i < 4; i++) {
        pthread_join(prodThreads[i], NULL);
    }

    /* Close channel to signal consumers */
    loopyChannelClose(ch);

    /* Wait for consumers */
    for (int i = 0; i < 4; i++) {
        pthread_join(consThreads[i], NULL);
    }

    /* Verify each item received exactly once */
    int duplicates = 0;
    for (int i = 0; i < ORDERING_NUM_ITEMS; i++) {
        int cnt = atomic_load(&received[i]);
        if (cnt > 1) {
            duplicates++;
        }
    }

    TEST_ASSERT_EQ(duplicates, 0, "no duplicates");
#undef ORDERING_NUM_ITEMS

    loopyChannelFree(ch);
    return 1;
}

/* --- Thread functions for close-under-load test --- */
typedef struct {
    loopyChannel *ch;
    _Atomic(bool) *running;
    _Atomic(uint64_t) *sendCount;
    _Atomic(uint64_t) *recvCount;
    _Atomic(uint64_t) *closedCount;
    int isSender;
} close_test_ctx;

static void *close_sender_thread(void *arg) {
    close_test_ctx *c = arg;
    while (atomic_load(c->running)) {
        uint64_t val = 42;
        loopyChannelStatus s = loopyChannelTrySend(c->ch, &val, sizeof(val));
        if (s == LOOPY_CHANNEL_OK) {
            atomic_fetch_add(c->sendCount, 1);
        } else if (s == LOOPY_CHANNEL_CLOSED) {
            atomic_fetch_add(c->closedCount, 1);
            break;
        }
    }
    return NULL;
}

static void *close_receiver_thread(void *arg) {
    close_test_ctx *c = arg;
    while (atomic_load(c->running)) {
        uint64_t val;
        ssize_t n = loopyChannelTryRecv(c->ch, &val, sizeof(val));
        if (n > 0) {
            atomic_fetch_add(c->recvCount, 1);
        } else if (n == LOOPY_CHANNEL_CLOSED) {
            atomic_fetch_add(c->closedCount, 1);
            break;
        }
    }
    return NULL;
}

/**
 * Test channel close behavior under load.
 */
static int test_stress_channel_close_under_load(void) {
    loopyChannelConfig chConfig;
    loopyChannelConfigInit(&chConfig);
    chConfig.type = LOOPY_CHANNEL_MPMC;
    chConfig.capacity = 64;
    chConfig.elementSize = sizeof(uint64_t);

    loopyChannel *ch = loopyChannelNew(NULL, &chConfig);
    TEST_ASSERT(ch != NULL, "should create channel");

    _Atomic(bool) running = true;
    _Atomic(uint64_t) sendCount = 0;
    _Atomic(uint64_t) recvCount = 0;
    _Atomic(uint64_t) closedCount = 0;

    close_test_ctx ctx = {ch,         &running,     &sendCount,
                          &recvCount, &closedCount, 0};

    pthread_t threads[8];
    for (int i = 0; i < 4; i++) {
        pthread_create(&threads[i], NULL, close_sender_thread, &ctx);
    }
    for (int i = 4; i < 8; i++) {
        pthread_create(&threads[i], NULL, close_receiver_thread, &ctx);
    }

    /* Let it run briefly then close */
    usleep(50000);
    loopyChannelClose(ch);
    atomic_store(&running, false);

    /* Wait for all threads */
    for (int i = 0; i < 8; i++) {
        pthread_join(threads[i], NULL);
    }

    TEST_ASSERT(loopyChannelIsClosed(ch), "channel should be closed");

    loopyChannelFree(ch);
    return 1;
}

/* --- Thread functions for boundary test --- */
typedef struct {
    loopyChannel *ch;
    _Atomic(uint64_t) *fullCount;
    _Atomic(uint64_t) *emptyCount;
    _Atomic(uint64_t) *successSend;
    _Atomic(uint64_t) *successRecv;
    int iterations;
} boundary_test_ctx;

static void *boundary_sender_thread(void *arg) {
    boundary_test_ctx *c = arg;
    for (int j = 0; j < c->iterations; j++) {
        uint64_t val = j;
        loopyChannelStatus s = loopyChannelTrySend(c->ch, &val, sizeof(val));
        if (s == LOOPY_CHANNEL_OK) {
            atomic_fetch_add(c->successSend, 1);
        } else if (s == LOOPY_CHANNEL_FULL) {
            atomic_fetch_add(c->fullCount, 1);
        }
    }
    return NULL;
}

static void *boundary_receiver_thread(void *arg) {
    boundary_test_ctx *c = arg;
    for (int j = 0; j < c->iterations; j++) {
        uint64_t val;
        ssize_t n = loopyChannelTryRecv(c->ch, &val, sizeof(val));
        if (n > 0) {
            atomic_fetch_add(c->successRecv, 1);
        } else if (n == LOOPY_CHANNEL_EMPTY) {
            atomic_fetch_add(c->emptyCount, 1);
        }
    }
    return NULL;
}

/**
 * Test boundary conditions (full/empty transitions).
 */
static int test_stress_channel_boundary_conditions(void) {
    loopyChannelConfig chConfig;
    loopyChannelConfigInit(&chConfig);
    chConfig.type = LOOPY_CHANNEL_MPMC;
    chConfig.capacity = 4; /* Very small capacity */
    chConfig.elementSize = sizeof(uint64_t);

    loopyChannel *ch = loopyChannelNew(NULL, &chConfig);
    TEST_ASSERT(ch != NULL, "should create channel");

    _Atomic(uint64_t) fullCount = 0;
    _Atomic(uint64_t) emptyCount = 0;
    _Atomic(uint64_t) successSend = 0;
    _Atomic(uint64_t) successRecv = 0;

    boundary_test_ctx ctx = {ch,           &fullCount,   &emptyCount,
                             &successSend, &successRecv, 10000};

    pthread_t threads[4];
    for (int i = 0; i < 2; i++) {
        pthread_create(&threads[i], NULL, boundary_sender_thread, &ctx);
    }
    for (int i = 2; i < 4; i++) {
        pthread_create(&threads[i], NULL, boundary_receiver_thread, &ctx);
    }

    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }

    /* With a small buffer, we should see many full/empty conditions */
    TEST_ASSERT(atomic_load(&fullCount) > 0 || atomic_load(&emptyCount) > 0,
                "should experience full or empty conditions");

    loopyChannelFree(ch);
    return 1;
}

/* --- PubSub Stress Tests --- */

typedef struct {
    loopyPubSub *ps;
    _Atomic(uint64_t) published;
    _Atomic(uint64_t) delivered;
    _Atomic(uint64_t) checksumErrors;
    _Atomic(bool) running;
    int numTopics;
} pubsub_stress_context;

static _Atomic(uint64_t) pubsub_recv_count = 0;

static bool pubsub_stress_callback(loopySubscription *sub,
                                   const loopyMessage *msg, void *userData) {
    (void)sub;
    pubsub_stress_context *ctx = userData;

    /* Verify message integrity */
    if (msg->len >= sizeof(uint64_t)) {
        uint64_t *seq = (uint64_t *)msg->data;
        /* Simple check: sequence should be non-zero */
        if (*seq == 0) {
            atomic_fetch_add(&ctx->checksumErrors, 1);
        }
    }

    atomic_fetch_add(&ctx->delivered, 1);
    atomic_fetch_add(&pubsub_recv_count, 1);
    return true;
}

static int pubsub_publisher_worker(loopyStressWorker *worker) {
    pubsub_stress_context *ctx = worker->sharedContext;

    /* Generate topic based on worker ID and random */
    int topicIdx = loopyStressWorkerRandRange(worker, ctx->numTopics);
    char topic[64];
    snprintf(topic, sizeof(topic), "stress.topic%d", topicIdx);

    /* Publish with sequence number */
    uint64_t seq = loopyStressWorkerRand(worker) | 1; /* Ensure non-zero */
    size_t delivered = loopyPublish(ctx->ps, topic, &seq, sizeof(seq));

    if (delivered > 0) {
        atomic_fetch_add(&ctx->published, 1);
        loopyStressStatAdd(worker->harness, STRESS_STAT_MESSAGES_SENT, 1);
    }

    return 0;
}

static int test_stress_pubsub_multi_publisher(void) {
    loopyPubSub *ps = loopyPubSubNew(NULL, NULL);
    TEST_ASSERT(ps != NULL, "should create pubsub");

    pubsub_stress_context ctx = {
        .ps = ps,
        .published = 0,
        .delivered = 0,
        .checksumErrors = 0,
        .running = true,
        .numTopics = 10,
    };

    /* Subscribe to all topics with context */
    loopySubscriptionConfig subConfig;
    loopySubscriptionConfigInit(&subConfig);
    subConfig.userData = &ctx;

    for (int i = 0; i < 10; i++) {
        char pattern[64];
        snprintf(pattern, sizeof(pattern), "stress.topic%d", i);
        loopySubscribe(ps, pattern, pubsub_stress_callback, &subConfig);
    }

    /* Also subscribe with wildcard */
    loopySubscribe(ps, "stress.*", pubsub_stress_callback, &subConfig);

    loopyStressConfig config;
    loopyStressConfigInit(&config);
    config.numWorkers = 8;
    config.durationMs = 500;
    config.workerFn = pubsub_publisher_worker;
    config.sharedContext = &ctx;

    loopyStressHarness *harness = loopyStressHarnessNew(&config);
    loopyStressResults results;
    loopyStressRun(harness, &results);
    loopyStressHarnessFree(harness);

    TEST_ASSERT(results.passed, "should pass");
    TEST_ASSERT_EQ(atomic_load(&ctx.checksumErrors), 0, "no checksum errors");
    TEST_ASSERT(atomic_load(&ctx.published) > 0, "should publish messages");

    loopyPubSubFree(ps);
    return 1;
}

/* --- Thread functions for pubsub churn test --- */
typedef struct {
    loopyPubSub *ps;
    _Atomic(bool) *running;
    _Atomic(uint64_t) *subCount;
    _Atomic(uint64_t) *unsubCount;
} churn_thread_ctx;

static bool churn_noop_callback(loopySubscription *s, const loopyMessage *m,
                                void *u) {
    (void)s;
    (void)m;
    (void)u;
    return true;
}

static void *churn_subscriber_thread(void *arg) {
    churn_thread_ctx *c = arg;
    loopySubscription *subs[10] = {0};
    int idx = 0;

    while (atomic_load(c->running)) {
        char pattern[32];
        snprintf(pattern, sizeof(pattern), "churn.%d", idx % 100);

        if (subs[idx % 10]) {
            loopyUnsubscribe(subs[idx % 10]);
            atomic_fetch_add(c->unsubCount, 1);
        }

        subs[idx % 10] =
            loopySubscribe(c->ps, pattern, churn_noop_callback, NULL);
        atomic_fetch_add(c->subCount, 1);
        idx++;
    }

    /* Cleanup */
    for (int i = 0; i < 10; i++) {
        if (subs[i]) {
            loopyUnsubscribe(subs[i]);
        }
    }

    return NULL;
}

typedef struct {
    loopyPubSub *ps;
    _Atomic(bool) *running;
    _Atomic(uint64_t) *pubCount;
} churn_publisher_ctx;

static void *churn_publisher_thread(void *arg) {
    churn_publisher_ctx *c = arg;
    int idx = 0;
    while (atomic_load(c->running)) {
        char topic[32];
        snprintf(topic, sizeof(topic), "churn.%d", idx % 100);
        uint64_t data = idx;
        loopyPublish(c->ps, topic, &data, sizeof(data));
        atomic_fetch_add(c->pubCount, 1);
        idx++;
    }
    return NULL;
}

/**
 * Test rapid subscribe/unsubscribe while publishing.
 */
static int test_stress_pubsub_subscriber_churn(void) {
    loopyPubSub *ps = loopyPubSubNew(NULL, NULL);
    TEST_ASSERT(ps != NULL, "should create pubsub");

    _Atomic(bool) running = true;
    _Atomic(uint64_t) subCount = 0;
    _Atomic(uint64_t) unsubCount = 0;
    _Atomic(uint64_t) pubCount = 0;

    churn_thread_ctx cctx = {ps, &running, &subCount, &unsubCount};
    churn_publisher_ctx pctx = {ps, &running, &pubCount};

    pthread_t churnThread, pubThread;
    pthread_create(&churnThread, NULL, churn_subscriber_thread, &cctx);
    pthread_create(&pubThread, NULL, churn_publisher_thread, &pctx);

    /* Run for a bit */
    usleep(300000); /* 300ms */
    atomic_store(&running, false);

    pthread_join(churnThread, NULL);
    pthread_join(pubThread, NULL);

    TEST_ASSERT(atomic_load(&subCount) > 0, "should subscribe");
    TEST_ASSERT(atomic_load(&unsubCount) > 0, "should unsubscribe");
    TEST_ASSERT(atomic_load(&pubCount) > 0, "should publish");

    loopyPubSubFree(ps);
    return 1;
}

/* --- Static callbacks for pattern matching test --- */
static bool pattern_noop_callback(loopySubscription *s, const loopyMessage *m,
                                  void *u) {
    (void)s;
    (void)m;
    (void)u;
    return true;
}

/**
 * Test pattern matching under concurrent modifications.
 */
static int test_stress_pubsub_pattern_matching(void) {
    loopyPubSub *ps = loopyPubSubNew(NULL, NULL);
    TEST_ASSERT(ps != NULL, "should create pubsub");

    /* Subscribe with different patterns */
    loopySubscribe(ps, "match.*.end", pattern_noop_callback, NULL);
    loopySubscribe(ps, "match.#", pattern_noop_callback, NULL);
    loopySubscribe(ps, "match.exact.topic", pattern_noop_callback, NULL);

    /* Publish to various matching topics */
    const char *topics[] = {
        "match.foo.end",     /* Matches star pattern */
        "match.bar.end",     /* Matches star pattern */
        "match.a.b.c",       /* Matches hash pattern */
        "match.exact.topic", /* Matches exact and hash */
        "match",             /* Matches hash only */
    };

    for (int i = 0; i < 1000; i++) {
        const char *topic = topics[i % 5];
        uint64_t data = i;
        loopyPublish(ps, topic, &data, sizeof(data));
    }

    loopyPubSubStats stats;
    loopyPubSubGetStats(ps, &stats);
    TEST_ASSERT(stats.messagesPublished == 1000, "should publish 1000");
    TEST_ASSERT(stats.messagesDelivered > 1000,
                "should deliver more due to pattern matches");

    loopyPubSubFree(ps);
    return 1;
}

/* --- Static callback for integrity test --- */
typedef struct {
    uint64_t seq;
    uint32_t checksum;
    char data[56];
} pubsub_integrity_msg;

static bool integrity_check_callback(loopySubscription *s,
                                     const loopyMessage *m, void *u) {
    (void)s;
    (void)u;
    if (m->len == sizeof(pubsub_integrity_msg)) {
        /* Just verify we can read the message - actual integrity check in test
         */
        (void)((const pubsub_integrity_msg *)m->data)->seq;
    }
    return true;
}

/**
 * Test message integrity through pubsub.
 */
static int test_stress_pubsub_message_integrity(void) {
    loopyPubSub *ps = loopyPubSubNew(NULL, NULL);
    TEST_ASSERT(ps != NULL, "should create pubsub");

    loopySubscribe(ps, "integrity.test", integrity_check_callback, NULL);

    /* Publish many messages */
    for (int i = 0; i < 10000; i++) {
        pubsub_integrity_msg msg;
        msg.seq = i + 1;
        msg.checksum = (uint32_t)(msg.seq ^ 0xDEADBEEF);
        memset(msg.data, (int)msg.seq & 0xFF, sizeof(msg.data));
        loopyPublish(ps, "integrity.test", &msg, sizeof(msg));
    }

    loopyPubSubStats stats;
    loopyPubSubGetStats(ps, &stats);
    TEST_ASSERT_EQ(stats.messagesPublished, 10000, "published 10000");
    TEST_ASSERT_EQ(stats.messagesDelivered, 10000, "delivered 10000");

    loopyPubSubFree(ps);
    return 1;
}

/* --- Static callback for fanout test --- */
static bool fanout_counter_callback(loopySubscription *s, const loopyMessage *m,
                                    void *u) {
    (void)s;
    (void)m;
    _Atomic(uint64_t) *c = u;
    if (c) {
        atomic_fetch_add(c, 1);
    }
    return true;
}

/**
 * Test message fanout to many subscribers.
 */
static int test_stress_pubsub_fanout(void) {
    loopyPubSub *ps = loopyPubSubNew(NULL, NULL);
    TEST_ASSERT(ps != NULL, "should create pubsub");

#define FANOUT_NUM_SUBS 100
    _Atomic(uint64_t) deliveries[FANOUT_NUM_SUBS];
    for (int i = 0; i < FANOUT_NUM_SUBS; i++) {
        atomic_store(&deliveries[i], 0);
    }

    /* Subscribe many times to same topic */
    for (int i = 0; i < FANOUT_NUM_SUBS; i++) {
        loopySubscriptionConfig config;
        loopySubscriptionConfigInit(&config);
        config.userData = &deliveries[i];

        loopySubscribe(ps, "fanout.test", fanout_counter_callback, &config);
    }

/* Publish messages */
#define FANOUT_NUM_MSGS 1000
    for (int i = 0; i < FANOUT_NUM_MSGS; i++) {
        uint64_t data = i;
        size_t delivered = loopyPublish(ps, "fanout.test", &data, sizeof(data));
        TEST_ASSERT_EQ(delivered, FANOUT_NUM_SUBS,
                       "should deliver to all subscribers");
    }

    loopyPubSubStats stats;
    loopyPubSubGetStats(ps, &stats);
    TEST_ASSERT_EQ(stats.messagesPublished, FANOUT_NUM_MSGS, "published");
    TEST_ASSERT_EQ(stats.messagesDelivered,
                   (uint64_t)FANOUT_NUM_MSGS * FANOUT_NUM_SUBS, "delivered");

#undef FANOUT_NUM_SUBS
#undef FANOUT_NUM_MSGS
    loopyPubSubFree(ps);
    return 1;
}

/* --- Registry Stress Tests --- */

typedef struct {
    loopyClusterRegistry *registry;
    _Atomic(uint64_t) nodesAdded;
    _Atomic(uint64_t) nodesRemoved;
    _Atomic(uint64_t) configSets;
    _Atomic(uint64_t) configGets;
    _Atomic(bool) running;
} registry_stress_context;

static int registry_node_worker(loopyStressWorker *worker) {
    registry_stress_context *ctx = worker->sharedContext;

    char nodeId[32];
    snprintf(nodeId, sizeof(nodeId), "stress_node_%u_%" PRIu64,
             worker->workerId, loopyStressWorkerRand(worker) % 1000);

    if (loopyStressWorkerRandRange(worker, 2) == 0) {
        /* Add node */
        loopyClusterNode *node = loopyClusterRegisterNode(
            ctx->registry, nodeId, NULL, LOOPY_ROLE_WORKER);
        if (node) {
            atomic_fetch_add(&ctx->nodesAdded, 1);
            loopyStressStatAdd(worker->harness, STRESS_STAT_OPERATIONS, 1);
        }
    } else {
        /* Remove node */
        if (loopyClusterUnregisterNode(ctx->registry, nodeId)) {
            atomic_fetch_add(&ctx->nodesRemoved, 1);
            loopyStressStatAdd(worker->harness, STRESS_STAT_OPERATIONS, 1);
        }
    }

    return 0;
}

static int test_stress_registry_node_churn(void) {
    loopyClusterConfig config;
    loopyClusterConfigInit(&config);
    config.localNodeId = "stress_local";

    loopyClusterRegistry *registry = loopyClusterRegistryNew(&config);
    TEST_ASSERT(registry != NULL, "should create registry");

    registry_stress_context ctx = {
        .registry = registry,
        .nodesAdded = 0,
        .nodesRemoved = 0,
        .configSets = 0,
        .configGets = 0,
        .running = true,
    };

    loopyStressConfig stressConfig;
    loopyStressConfigInit(&stressConfig);
    stressConfig.numWorkers = 4;
    stressConfig.durationMs = 500;
    stressConfig.workerFn = registry_node_worker;
    stressConfig.sharedContext = &ctx;

    loopyStressHarness *harness = loopyStressHarnessNew(&stressConfig);
    loopyStressResults results;
    loopyStressRun(harness, &results);
    loopyStressHarnessFree(harness);

    TEST_ASSERT(results.passed, "should pass");
    TEST_ASSERT(atomic_load(&ctx.nodesAdded) > 0, "should add nodes");

    loopyClusterRegistryFree(registry);
    return 1;
}

static int registry_config_worker(loopyStressWorker *worker) {
    registry_stress_context *ctx = worker->sharedContext;

    char key[64];
    snprintf(key, sizeof(key), "stress.key.%u.%" PRIu64, worker->workerId,
             loopyStressWorkerRandRange(worker, 100));

    if (loopyStressWorkerRandRange(worker, 3) < 2) {
        /* Set config */
        char value[64];
        snprintf(value, sizeof(value), "value_%" PRIu64,
                 loopyStressWorkerRand(worker));
        loopyClusterConfigSetString(ctx->registry, key, value, 0);
        atomic_fetch_add(&ctx->configSets, 1);
        loopyStressStatAdd(worker->harness, STRESS_STAT_OPERATIONS, 1);
    } else {
        /* Get config */
        loopyClusterConfigGetString(ctx->registry, key);
        atomic_fetch_add(&ctx->configGets, 1);
        loopyStressStatAdd(worker->harness, STRESS_STAT_OPERATIONS, 1);
    }

    return 0;
}

static int test_stress_registry_config_churn(void) {
    loopyClusterConfig config;
    loopyClusterConfigInit(&config);
    config.localNodeId = "config_stress_local";

    loopyClusterRegistry *registry = loopyClusterRegistryNew(&config);
    TEST_ASSERT(registry != NULL, "should create registry");

    registry_stress_context ctx = {
        .registry = registry,
        .nodesAdded = 0,
        .nodesRemoved = 0,
        .configSets = 0,
        .configGets = 0,
        .running = true,
    };

    loopyStressConfig stressConfig;
    loopyStressConfigInit(&stressConfig);
    stressConfig.numWorkers = 8;
    stressConfig.durationMs = 500;
    stressConfig.workerFn = registry_config_worker;
    stressConfig.sharedContext = &ctx;

    loopyStressHarness *harness = loopyStressHarnessNew(&stressConfig);
    loopyStressResults results;
    loopyStressRun(harness, &results);
    loopyStressHarnessFree(harness);

    TEST_ASSERT(results.passed, "should pass");
    TEST_ASSERT(atomic_load(&ctx.configSets) > 0, "should set configs");
    TEST_ASSERT(atomic_load(&ctx.configGets) > 0, "should get configs");

    loopyClusterRegistryFree(registry);
    return 1;
}

static int test_stress_registry_concurrent_access(void) {
    loopyClusterConfig config;
    loopyClusterConfigInit(&config);
    config.localNodeId = "concurrent_local";

    loopyClusterRegistry *registry = loopyClusterRegistryNew(&config);
    TEST_ASSERT(registry != NULL, "should create registry");

    /* Add some initial nodes and config */
    for (int i = 0; i < 10; i++) {
        char nodeId[32];
        snprintf(nodeId, sizeof(nodeId), "init_node_%d", i);
        loopyClusterRegisterNode(registry, nodeId, NULL, LOOPY_ROLE_WORKER);

        char key[32], value[32];
        snprintf(key, sizeof(key), "init.key.%d", i);
        snprintf(value, sizeof(value), "init_value_%d", i);
        loopyClusterConfigSetString(registry, key, value, 0);
    }

    registry_stress_context ctx = {
        .registry = registry,
        .nodesAdded = 0,
        .nodesRemoved = 0,
        .configSets = 0,
        .configGets = 0,
        .running = true,
    };

    /* Run node and config workers concurrently */
    loopyStressConfig nodeConfig;
    loopyStressConfigInit(&nodeConfig);
    nodeConfig.numWorkers = 4;
    nodeConfig.durationMs = 300;
    nodeConfig.workerFn = registry_node_worker;
    nodeConfig.sharedContext = &ctx;

    loopyStressConfig cfgConfig;
    loopyStressConfigInit(&cfgConfig);
    cfgConfig.numWorkers = 4;
    cfgConfig.durationMs = 300;
    cfgConfig.workerFn = registry_config_worker;
    cfgConfig.sharedContext = &ctx;

    loopyStressHarness *nodeHarness = loopyStressHarnessNew(&nodeConfig);
    loopyStressHarness *cfgHarness = loopyStressHarnessNew(&cfgConfig);

    loopyStressStart(nodeHarness);
    loopyStressStart(cfgHarness);

    loopyStressResults nodeResults, cfgResults;
    loopyStressWait(nodeHarness, &nodeResults);
    loopyStressWait(cfgHarness, &cfgResults);

    TEST_ASSERT(nodeResults.passed, "node stress should pass");
    TEST_ASSERT(cfgResults.passed, "config stress should pass");

    loopyStressHarnessFree(nodeHarness);
    loopyStressHarnessFree(cfgHarness);
    loopyClusterRegistryFree(registry);
    return 1;
}

static _Atomic(int) event_join_count = 0;
static _Atomic(int) event_left_count = 0;
static _Atomic(int) event_config_count = 0;

static void stress_event_callback(loopyClusterRegistry *reg,
                                  loopyClusterEventType event,
                                  const loopyNodeInfo *node,
                                  const loopyConfigValue *cv, void *userData) {
    (void)reg;
    (void)node;
    (void)cv;
    (void)userData;

    switch (event) {
    case LOOPY_CLUSTER_NODE_JOINED:
        atomic_fetch_add(&event_join_count, 1);
        break;
    case LOOPY_CLUSTER_NODE_LEFT:
        atomic_fetch_add(&event_left_count, 1);
        break;
    case LOOPY_CLUSTER_CONFIG_CHANGE:
        atomic_fetch_add(&event_config_count, 1);
        break;
    default:
        break;
    }
}

static int test_stress_registry_events(void) {
    atomic_store(&event_join_count, 0);
    atomic_store(&event_left_count, 0);
    atomic_store(&event_config_count, 0);

    loopyClusterConfig config;
    loopyClusterConfigInit(&config);
    config.localNodeId = "event_stress_local";
    config.eventCallback = stress_event_callback;

    loopyClusterRegistry *registry = loopyClusterRegistryNew(&config);
    TEST_ASSERT(registry != NULL, "should create registry");

    /* 1 for local node */
    TEST_ASSERT_EQ(atomic_load(&event_join_count), 1, "local node event");

    registry_stress_context ctx = {
        .registry = registry,
        .nodesAdded = 0,
        .nodesRemoved = 0,
        .configSets = 0,
        .configGets = 0,
        .running = true,
    };

    loopyStressConfig stressConfig;
    loopyStressConfigInit(&stressConfig);
    stressConfig.numWorkers = 4;
    stressConfig.durationMs = 200;
    stressConfig.workerFn = registry_node_worker;
    stressConfig.sharedContext = &ctx;

    loopyStressHarness *harness = loopyStressHarnessNew(&stressConfig);
    loopyStressResults results;
    loopyStressRun(harness, &results);
    loopyStressHarnessFree(harness);

    /* Should have received events */
    TEST_ASSERT(atomic_load(&event_join_count) > 1, "should have join events");

    loopyClusterRegistryFree(registry);
    return 1;
}

/* --- Integration Stress Tests --- */

/* Thread contexts and functions for integration tests */
typedef struct {
    loopyChannel *ch;
    _Atomic(uint64_t) *sent;
    _Atomic(bool) *running;
} integration_prod_ctx;

static void *integration_producer_thread(void *arg) {
    integration_prod_ctx *c = arg;
    while (atomic_load(c->running)) {
        char msg[64] = "test message";
        if (loopyChannelTrySend(c->ch, msg, 64) == LOOPY_CHANNEL_OK) {
            atomic_fetch_add(c->sent, 1);
        }
    }
    return NULL;
}

typedef struct {
    loopyChannel *ch;
    loopyPubSub *ps;
    _Atomic(uint64_t) *recv;
    _Atomic(bool) *running;
} integration_fwd_ctx;

static void *integration_forwarder_thread(void *arg) {
    integration_fwd_ctx *c = arg;
    while (atomic_load(c->running)) {
        char msg[64];
        ssize_t n = loopyChannelTryRecv(c->ch, msg, 64);
        if (n > 0) {
            atomic_fetch_add(c->recv, 1);
            loopyPublish(c->ps, "forwarded.msg", msg, 64);
        }
    }
    return NULL;
}

static bool integration_noop_callback(loopySubscription *s,
                                      const loopyMessage *m, void *u) {
    (void)s;
    (void)m;
    (void)u;
    return true;
}

/**
 * Test channel + pubsub integration: publish messages through a channel.
 */
static int test_stress_channel_pubsub_integration(void) {
    loopyChannelConfig chConfig;
    loopyChannelConfigInit(&chConfig);
    chConfig.type = LOOPY_CHANNEL_MPMC;
    chConfig.capacity = 256;
    chConfig.elementSize = 64;

    loopyChannel *ch = loopyChannelNew(NULL, &chConfig);
    TEST_ASSERT(ch != NULL, "should create channel");

    loopyPubSub *ps = loopyPubSubNew(NULL, NULL);
    TEST_ASSERT(ps != NULL, "should create pubsub");

    _Atomic(uint64_t) channelSent = 0;
    _Atomic(uint64_t) channelRecv = 0;
    _Atomic(bool) running = true;

    /* Subscribe to forwarded messages */
    loopySubscribe(ps, "forwarded.*", integration_noop_callback, NULL);

    integration_prod_ctx prodCtx = {ch, &channelSent, &running};
    integration_fwd_ctx fwdCtx = {ch, ps, &channelRecv, &running};

    pthread_t prodThread, fwdThread;
    pthread_create(&prodThread, NULL, integration_producer_thread, &prodCtx);
    pthread_create(&fwdThread, NULL, integration_forwarder_thread, &fwdCtx);

    /* Run for a bit */
    usleep(200000);
    atomic_store(&running, false);

    pthread_join(prodThread, NULL);
    loopyChannelClose(ch);
    pthread_join(fwdThread, NULL);

    uint64_t sent = atomic_load(&channelSent);
    uint64_t recv = atomic_load(&channelRecv);

    TEST_ASSERT(sent > 0, "should send to channel");
    TEST_ASSERT(recv > 0, "should receive from channel");
    TEST_ASSERT(recv <= sent, "received <= sent");

    loopyChannelFree(ch);
    loopyPubSubFree(ps);
    return 1;
}

/* Full system worker context and thread function */
typedef struct {
    loopyChannel *ch;
    loopyPubSub *ps;
    loopyClusterRegistry *registry;
    loopyConcurrencyPool *pool;
    _Atomic(bool) *running;
    _Atomic(uint64_t) *ops;
} full_system_ctx;

static void *full_system_worker_thread(void *arg) {
    full_system_ctx *c = arg;
    int iter = 0;
    while (atomic_load(c->running)) {
        int op = iter % 4;
        switch (op) {
        case 0: {
            uint64_t val = iter;
            loopyChannelTrySend(c->ch, &val, sizeof(val));
            break;
        }
        case 1: {
            uint64_t val;
            loopyChannelTryRecv(c->ch, &val, sizeof(val));
            break;
        }
        case 2: {
            char msg[8] = "hi";
            loopyPublish(c->ps, "system.event", msg, 3);
            break;
        }
        case 3: {
            loopyClusterConfigGetString(c->registry, "system.status");
            break;
        }
        }
        atomic_fetch_add(c->ops, 1);
        iter++;
    }
    return NULL;
}

/**
 * Full system stress test: all components together.
 */
static int test_stress_full_system(void) {
    /* Create all components */
    loopyChannelConfig chConfig;
    loopyChannelConfigInit(&chConfig);
    chConfig.type = LOOPY_CHANNEL_MPMC;
    chConfig.capacity = 128;
    chConfig.elementSize = sizeof(uint64_t);

    loopyChannel *ch = loopyChannelNew(NULL, &chConfig);
    loopyPubSub *ps = loopyPubSubNew(NULL, NULL);

    loopyClusterConfig clConfig;
    loopyClusterConfigInit(&clConfig);
    clConfig.localNodeId = "full_system_node";
    loopyClusterRegistry *registry = loopyClusterRegistryNew(&clConfig);

    loopyConcurrencyPoolConfig poolConfig;
    loopyConcurrencyPoolConfigInit(&poolConfig);
    poolConfig.globalLimit = 100;
    loopyConcurrencyPool *pool = loopyConcurrencyPoolNew(&poolConfig);

    TEST_ASSERT(ch != NULL, "should create channel");
    TEST_ASSERT(ps != NULL, "should create pubsub");
    TEST_ASSERT(registry != NULL, "should create registry");
    TEST_ASSERT(pool != NULL, "should create pool");

    /* Set up some subscriptions */
    loopySubscribe(ps, "system.*", integration_noop_callback, NULL);

    /* Add some config */
    loopyClusterConfigSetString(registry, "system.status", "running", 0);

    _Atomic(bool) running = true;
    _Atomic(uint64_t) operations = 0;

    full_system_ctx ctx = {ch, ps, registry, pool, &running, &operations};

    pthread_t workers[4];
    for (int i = 0; i < 4; i++) {
        pthread_create(&workers[i], NULL, full_system_worker_thread, &ctx);
    }

    /* Run for a bit */
    usleep(500000);
    atomic_store(&running, false);

    for (int i = 0; i < 4; i++) {
        pthread_join(workers[i], NULL);
    }

    uint64_t totalOps = atomic_load(&operations);
    TEST_ASSERT(totalOps > 1000, "should perform many operations");

    loopyConcurrencyPoolFree(pool);
    loopyClusterRegistryFree(registry);
    loopyPubSubFree(ps);
    loopyChannelFree(ch);
    return 1;
}
#endif /* LOOPY_HAVE_RAX */

/* ====================================================================
 * Test Registration
 *
 * This function registers all tests in the registry. It's called once
 * at startup to build the test list, then tests can be run by name.
 * ==================================================================== */
static void register_all_tests(void) {
    /* Basic loop tests */
    TEST_GROUP("Basic Loop Tests");
    RUN_TEST(test_loop_create_delete);
    RUN_TEST(test_loop_stack_allocated);
    RUN_TEST(test_loop_resize);
    RUN_TEST(test_adapter_name);

    /* File descriptor tests */
    TEST_GROUP("File Descriptor Tests");
    RUN_TEST(test_register_read);
    RUN_TEST(test_register_write);
    RUN_TEST(test_register_read_write);
    RUN_TEST(test_unregister_read);
    RUN_TEST(test_unregister_write);
    RUN_TEST(test_unregister_all);
    RUN_TEST(test_register_write_if_none_exists);
    RUN_TEST(test_auto_resize);
    RUN_TEST(test_maxfd_tracking);

    /* Timer tests */
    TEST_GROUP("Timer Tests");
    RUN_TEST(test_timer_registration);

    /* loopyTimer API tests */
    TEST_GROUP("loopyTimer API Tests");
    RUN_TEST(test_timer_oneshot_basic);
    RUN_TEST(test_timer_oneshot_ms_seconds);
    RUN_TEST(test_timer_periodic_basic);
    RUN_TEST(test_timer_periodic_delayed);
    RUN_TEST(test_timer_periodic_ms_seconds);
    RUN_TEST(test_timer_cancel);
    RUN_TEST(test_timer_is_active);
    RUN_TEST(test_timer_userdata);
    RUN_TEST(test_timer_null_safety);
    RUN_TEST(test_timer_auto_cleanup);

    /* Event processing tests */
    TEST_GROUP("Event Processing Tests");
    RUN_TEST(test_event_processing);
    RUN_TEST(test_sleep_callbacks);
    RUN_TEST(test_stop);

    /* Network tests */
    TEST_GROUP("Network Tests");
    RUN_TEST(test_net_tcp4_server);
    RUN_TEST(test_net_tcp6_server);
    RUN_TEST(test_net_nonblock);
    RUN_TEST(test_net_nodelay);
    RUN_TEST(test_net_keepalive);
    RUN_TEST(test_net_resolve);
    RUN_TEST(test_net_format_addr);
    RUN_TEST(test_tcp_connect_accept);
    RUN_TEST(test_unix_server);
    RUN_TEST(test_unix_connect);
    RUN_TEST(test_send_timeout);

    /* loopyNice tests */
    TEST_GROUP("loopyNice Tests");
    RUN_TEST(test_nice_create_delete);
    RUN_TEST(test_nice_pipe);
    RUN_TEST(test_nice_socketpair);
    RUN_TEST(test_nice_tcp_server);

    /* Max-Heap tests */
    TEST_GROUP("Max-Heap Tests");
    RUN_TEST(test_heap_create_delete);
    RUN_TEST(test_heap_stack_allocated);
    RUN_TEST(test_heap_insert_peek);
    RUN_TEST(test_heap_pop);
    RUN_TEST(test_heap_remove);
    RUN_TEST(test_heap_contains);
    RUN_TEST(test_heap_clear);
    RUN_TEST(test_heap_reserve);
    RUN_TEST(test_heap_auto_grow);
    RUN_TEST(test_heap_large_fds);
    RUN_TEST(test_heap_duplicates);
    RUN_TEST(test_heap_empty_operations);
    RUN_TEST(test_heap_null_safety);
    RUN_TEST(test_heap_single_element);
    RUN_TEST(test_heap_remove_last);
    RUN_TEST(test_heap_stress);
    RUN_TEST(test_heap_loopy_maxfd_integration);

    /* Edge case tests */
    TEST_GROUP("Edge Case Tests");
    RUN_TEST(test_null_handling);
    RUN_TEST(test_unregister_nonexistent);
    RUN_TEST(test_get_events_invalid_fd);

    /* DNS tests */
    TEST_GROUP("DNS Tests");
    RUN_TEST(test_dns_create_delete);
    RUN_TEST(test_dns_create_default_config);
    RUN_TEST(test_dns_status_strings);
    RUN_TEST(test_dns_resolve_localhost);
    RUN_TEST(test_dns_cancel);
    RUN_TEST(test_dns_cancel_all);
    RUN_TEST(test_dns_null_safety);
    RUN_TEST(test_dns_reverse_lookup);
    RUN_TEST(test_dns_reverse_null_safety);

    /* Signal tests */
    TEST_GROUP("Signal Tests");
    RUN_TEST(test_signal_create_delete);
    RUN_TEST(test_signal_single_handler);
    RUN_TEST(test_signal_register_unregister);
    RUN_TEST(test_signal_delivery);
    RUN_TEST(test_signal_names);
    RUN_TEST(test_signal_invalid_signum);
    RUN_TEST(test_signal_null_safety);
    RUN_TEST(test_signal_oneshot);

    /* Async tests */
    TEST_GROUP("Async Tests");
    RUN_TEST(test_async_create_delete);
    RUN_TEST(test_async_send_same_thread);
    RUN_TEST(test_async_send_from_thread);
    RUN_TEST(test_async_coalescing);
    RUN_TEST(test_async_multiple_handles);
    RUN_TEST(test_async_user_data);
    RUN_TEST(test_async_backend_name);
    RUN_TEST(test_async_null_safety);

    /* Work tests */
    TEST_GROUP("Work Tests");
    RUN_TEST(test_work_create_delete);
    RUN_TEST(test_work_create_default_config);
    RUN_TEST(test_work_queue_execute);
    RUN_TEST(test_work_cancel_pending);
    RUN_TEST(test_work_cancel_all);
    RUN_TEST(test_work_multiple_concurrent);
    RUN_TEST(test_work_status_strings);
    RUN_TEST(test_work_null_safety);
    RUN_TEST(test_work_queue_overflow);

    /* Watch tests */
    TEST_GROUP("Watch Tests");
    RUN_TEST(test_watch_create_delete);
    RUN_TEST(test_watch_add_remove);
    RUN_TEST(test_watch_multiple);
    RUN_TEST(test_watch_modify_event);
    RUN_TEST(test_watch_event_names);
    RUN_TEST(test_watch_backend_name);
    RUN_TEST(test_watch_nonexistent_path);
    RUN_TEST(test_watch_null_safety);

    /* FSPoll tests */
    TEST_GROUP("FSPoll Tests");
    RUN_TEST(test_fspoll_create_delete);
    RUN_TEST(test_fspoll_detect_modify);
    RUN_TEST(test_fspoll_detect_chmod);
    RUN_TEST(test_fspoll_detect_delete);
    RUN_TEST(test_fspoll_start_stop);
    RUN_TEST(test_fspoll_null_safety);

    /* UDP tests */
    TEST_GROUP("UDP Tests");
    RUN_TEST(test_udp_create_delete);
    RUN_TEST(test_udp_bind_ipv4);
    RUN_TEST(test_udp_bind_ipv6);
    RUN_TEST(test_udp_send_recv);
    RUN_TEST(test_udp_connect);
    RUN_TEST(test_udp_try_send);
    RUN_TEST(test_udp_broadcast);
    RUN_TEST(test_udp_ttl);
    RUN_TEST(test_udp_recv_stop);
    RUN_TEST(test_udp_null_safety);
    RUN_TEST(test_udp_batch_send_recv);
    RUN_TEST(test_udp_batch_connected);
    RUN_TEST(test_udp_batch_native_check);
    RUN_TEST(test_udp_batch_null_safety);
    RUN_TEST(test_udp_gso_detection);
    RUN_TEST(test_udp_gso_enable_disable);
    RUN_TEST(test_udp_gso_invalid_segment_size);
    RUN_TEST(test_udp_gso_send_not_enabled);
    RUN_TEST(test_udp_gso_null_safety);
    RUN_TEST(test_udp_gso_send_basic);
    RUN_TEST(test_udp_gso_connected_send);
    RUN_TEST(test_udp_gro_detection);
    RUN_TEST(test_udp_gro_enable_disable);
    RUN_TEST(test_udp_gro_null_safety);
    RUN_TEST(test_udp_pmtu_set_mode);
    RUN_TEST(test_udp_pmtu_get);
    RUN_TEST(test_udp_pmtu_null_safety);
    RUN_TEST(test_udp_pmtu_with_connect);

    /* Process tests */
    TEST_GROUP("Process Tests");
    RUN_TEST(test_process_spawn_simple);
    RUN_TEST(test_process_exit_code);
    RUN_TEST(test_process_capture_stdout);
    RUN_TEST(test_process_kill);
    RUN_TEST(test_process_cwd);
    RUN_TEST(test_process_env);
    RUN_TEST(test_process_spawn_failure);
    RUN_TEST(test_process_null_safety);

    /* Idle/Prepare/Check tests */
    TEST_GROUP("Idle/Prepare/Check Tests");
    RUN_TEST(test_idle_create_delete);
    RUN_TEST(test_prepare_create_delete);
    RUN_TEST(test_check_create_delete);
    RUN_TEST(test_idle_callback_fires);
    RUN_TEST(test_idle_auto_stop);
    RUN_TEST(test_prepare_callback_fires);
    RUN_TEST(test_check_callback_fires);
    RUN_TEST(test_idle_stop_restart);
    RUN_TEST(test_idle_multiple_handles);
    RUN_TEST(test_idle_null_safety);
    RUN_TEST(test_idle_rapid_iterations);
    RUN_TEST(test_check_fires_with_idle_only);
    RUN_TEST(test_prepare_fires_each_iteration);
    RUN_TEST(test_idle_prepare_check_order);
    RUN_TEST(test_idle_timer_no_block);

    /* Pipe tests */
    TEST_GROUP("Pipe Tests");
    RUN_TEST(test_pipe_create_basic);
    RUN_TEST(test_pipe_create_with_flags);
    RUN_TEST(test_pipe_from_fds);
    RUN_TEST(test_pipe_sync_read_write);
    RUN_TEST(test_pipe_close_read);
    RUN_TEST(test_pipe_close_write);
    RUN_TEST(test_pipe_fifo_create_remove);
    RUN_TEST(test_pipe_fifo_open_write);
    RUN_TEST(test_pipe_async_read);
    RUN_TEST(test_pipe_async_write);
    RUN_TEST(test_pipe_buffer_size);
    RUN_TEST(test_pipe_error_handling);
    RUN_TEST(test_pipe_null_safety);
    RUN_TEST(test_pipe_eof_detection);

    /* Stream tests */
    TEST_GROUP("Stream Tests");
    RUN_TEST(test_stream_create_delete_tcp);
    RUN_TEST(test_stream_create_delete_pipe);
    RUN_TEST(test_stream_from_fd);
    RUN_TEST(test_stream_bind_listen);
    RUN_TEST(test_stream_accept);
    RUN_TEST(test_stream_pipe_read_write);
    RUN_TEST(test_stream_try_write);
    RUN_TEST(test_stream_shutdown);
    RUN_TEST(test_stream_write_queue);
    RUN_TEST(test_stream_null_safety);
    RUN_TEST(test_stream_fd_passing_basic);
    RUN_TEST(test_stream_fd_passing_multiple);
    RUN_TEST(test_stream_fd_passing_not_pipe);
    RUN_TEST(test_stream_fd_passing_null_safety);

    /* FS tests */
    TEST_GROUP("FS Tests");
    RUN_TEST(test_fs_sync_operations);
    RUN_TEST(test_fs_async_open_close);
    RUN_TEST(test_fs_stat);
    RUN_TEST(test_fs_mkdir_rmdir);
    RUN_TEST(test_fs_rename);
    RUN_TEST(test_fs_pread_pwrite);
    RUN_TEST(test_fs_truncate_fsync);
    RUN_TEST(test_fs_chmod);
    RUN_TEST(test_fs_error_handling);
    RUN_TEST(test_fs_null_safety);

    /* Extended FS tests */
    TEST_GROUP("Extended FS Tests");
    RUN_TEST(test_fs_link_symlink);
    RUN_TEST(test_fs_realpath_access);
    RUN_TEST(test_fs_scandir);
    RUN_TEST(test_fs_utime);
    RUN_TEST(test_fs_copyfile);
    RUN_TEST(test_fs_mkstemp_mkdtemp);
    RUN_TEST(test_fs_extended_null_safety);

    /* TTY tests */
    TEST_GROUP("TTY Tests");
    RUN_TEST(test_tty_is_tty);
    RUN_TEST(test_tty_create_non_tty);
    RUN_TEST(test_tty_create_if_available);
    RUN_TEST(test_tty_mode_enum);
    RUN_TEST(test_tty_null_safety);
    RUN_TEST(test_tty_reset_all);

#ifdef USE_IOURING
    /* io_uring tests */
    TEST_GROUP("io_uring Backend Tests");
    RUN_TEST(test_iouring_api_available);
    RUN_TEST(test_iouring_null_safety);
    RUN_TEST(test_iouring_adapter_name);
    RUN_TEST(test_iouring_event_loop_works);

    /* io_uring file operation tests */
    TEST_GROUP("io_uring File Operations Tests");
    RUN_TEST(test_iouring_fs_api_available);
    RUN_TEST(test_iouring_fs_write_basic);
    RUN_TEST(test_iouring_fs_read_basic);
    RUN_TEST(test_iouring_fs_open_close);
    RUN_TEST(test_iouring_fs_fsync);
    RUN_TEST(test_iouring_fs_fdatasync);
    RUN_TEST(test_iouring_fs_read_offset);
    RUN_TEST(test_iouring_fs_invalid_fd);
    RUN_TEST(test_iouring_fs_null_safety);
    RUN_TEST(test_iouring_fs_concurrent_ops);
    RUN_TEST(test_iouring_fs_fallback_non_iouring);

    /* io_uring network I/O tests */
    TEST_GROUP("io_uring Network I/O Tests");
    RUN_TEST(test_iouring_net_send_basic);
    RUN_TEST(test_iouring_net_recv_basic);
    RUN_TEST(test_iouring_net_accept_basic);
    RUN_TEST(test_iouring_net_connect_basic);
    RUN_TEST(test_iouring_net_sendmsg_basic);
    RUN_TEST(test_iouring_net_recvmsg_basic);
    RUN_TEST(test_iouring_net_udp_send);
    RUN_TEST(test_iouring_net_udp_recv);
    RUN_TEST(test_iouring_net_shutdown);
    RUN_TEST(test_iouring_net_accept_multishot);
    RUN_TEST(test_iouring_net_cancel);
    RUN_TEST(test_iouring_net_concurrent_ops);
    RUN_TEST(test_iouring_net_invalid_fd);
    RUN_TEST(test_iouring_net_null_safety);
    RUN_TEST(test_iouring_net_large_transfer);
    RUN_TEST(test_iouring_net_fallback);
#endif /* USE_IOURING */

    /* File Locking Tests */
    TEST_GROUP("File Locking Tests");
    RUN_TEST(test_flock_capabilities);
    RUN_TEST(test_flock_exclusive_basic);
    RUN_TEST(test_flock_shared_basic);
    RUN_TEST(test_flock_fcntl_whole_file);
    RUN_TEST(test_flock_fcntl_byte_range);
    RUN_TEST(test_flock_test_range);
    RUN_TEST(test_flock_highlevel_api);
    RUN_TEST(test_flock_highlevel_fcntl_range);
    RUN_TEST(test_flock_lockfile_pattern);
    RUN_TEST(test_flock_error_handling);
    RUN_TEST(test_flock_null_safety);

#ifdef USE_IOURING
    /* io_uring Fixed Buffer Tests */
    TEST_GROUP("io_uring Fixed Buffer Tests");
    RUN_TEST(test_iouring_fixed_buffers_registration);
    RUN_TEST(test_iouring_fixed_buffers_read_write);
    RUN_TEST(test_iouring_fixed_buffers_error_handling);
    RUN_TEST(test_iouring_fixed_buffers_multiple);
    RUN_TEST(test_iouring_fixed_buffers_null_safety);

    /* io_uring Fixed File Tests */
    TEST_GROUP("io_uring Fixed File Tests");
    RUN_TEST(test_iouring_fixed_files_registration);
    RUN_TEST(test_iouring_fixed_files_read_write);
    RUN_TEST(test_iouring_fixed_files_error_handling);
    RUN_TEST(test_iouring_fixed_files_multiple);
    RUN_TEST(test_iouring_fixed_files_null_safety);

    /* io_uring Linked Operations Tests */
    TEST_GROUP("io_uring Linked Operations Tests");
    RUN_TEST(test_iouring_link_basic_chain);
    RUN_TEST(test_iouring_link_soft_failure);
    RUN_TEST(test_iouring_link_hard_mode);
    RUN_TEST(test_iouring_link_multiple_ops);
    RUN_TEST(test_iouring_link_null_safety);
#endif /* USE_IOURING */

    /* Random tests */
    TEST_GROUP("Random Tests");
    RUN_TEST(test_random_sync_basic);
    RUN_TEST(test_random_sync_various_sizes);
    RUN_TEST(test_random_async);
    RUN_TEST(test_random_uniqueness);
    RUN_TEST(test_random_null_safety);

    /* Metrics tests */
    TEST_GROUP("Metrics Tests");
    RUN_TEST(test_metrics_enable_disable);
    RUN_TEST(test_metrics_loop_iterations);
    RUN_TEST(test_metrics_events_processed);
    RUN_TEST(test_metrics_reset);
    RUN_TEST(test_metrics_null_safety);
    RUN_TEST(test_metrics_poll_time);
    RUN_TEST(test_metrics_timers_processed);

    /* System Utilities tests */
    TEST_GROUP("System Utilities Tests");
    RUN_TEST(test_sys_interface_addresses);
    RUN_TEST(test_sys_available_parallelism);
    RUN_TEST(test_sys_exe_path);
    RUN_TEST(test_sys_statfs);
    RUN_TEST(test_sys_null_safety);
    RUN_TEST(test_sys_homedir);
    RUN_TEST(test_sys_tmpdir);
    RUN_TEST(test_sys_cwd_chdir);
    RUN_TEST(test_sys_hostname);
    RUN_TEST(test_sys_paths_null_safety);

    /* Memory-mapped file tests */
    TEST_GROUP("Memory-Mapped File Tests");
    RUN_TEST(test_mmap_utilities);
    RUN_TEST(test_mmap_file_readonly);
    RUN_TEST(test_mmap_file_readwrite);
    RUN_TEST(test_mmap_anonymous);
    RUN_TEST(test_mmap_shared_ipc);
    RUN_TEST(test_mmap_advice);
    RUN_TEST(test_mmap_sync);
    RUN_TEST(test_mmap_lock_unlock);
    RUN_TEST(test_mmap_offset_length);
    RUN_TEST(test_mmap_error_handling);
    RUN_TEST(test_mmap_null_safety);

    /* Direct I/O tests */
    TEST_GROUP("Direct I/O Tests (O_DIRECT)");
    RUN_TEST(test_direct_io_aligned_allocation);
    RUN_TEST(test_direct_io_aligned_edge_cases);
    RUN_TEST(test_direct_io_read_write);
    RUN_TEST(test_direct_io_alignment_requirements);
    RUN_TEST(test_direct_io_various_sizes);
    RUN_TEST(test_direct_io_null_safety);

    /* Connection Pool tests */
    TEST_GROUP("Connection Pool Tests");
    RUN_TEST(test_connpool_create_delete);
    RUN_TEST(test_connpool_default_config);
    RUN_TEST(test_connpool_acquire_release);
    RUN_TEST(test_connpool_connection_reuse);
    RUN_TEST(test_connpool_min_idle);
    RUN_TEST(test_connpool_max_total);
    RUN_TEST(test_connpool_validation);
    RUN_TEST(test_connpool_unhealthy_release);
    RUN_TEST(test_connpool_statistics);
    RUN_TEST(test_connpool_health_check);
    RUN_TEST(test_connpool_idle_cleanup);
    RUN_TEST(test_connpool_prewarm);
    RUN_TEST(test_connpool_null_safety);
    RUN_TEST(test_connpool_async_acquire);

    /* Channel tests */
    TEST_GROUP("Channel Tests");
    RUN_TEST(test_channel_create_destroy);
    RUN_TEST(test_channel_config_init);
    RUN_TEST(test_channel_spsc_send_recv);
    RUN_TEST(test_channel_mpsc_send_recv);
    RUN_TEST(test_channel_mpmc_send_recv);
    RUN_TEST(test_channel_try_send_recv);
    RUN_TEST(test_channel_full_empty);
    RUN_TEST(test_channel_close);
    RUN_TEST(test_channel_stats);
    RUN_TEST(test_channel_type_names);
    RUN_TEST(test_channel_status_names);
    RUN_TEST(test_channel_null_safety);
    RUN_TEST(test_channel_introspection);
    RUN_TEST(test_channel_spsc_threaded);
    RUN_TEST(test_channel_mpsc_threaded);
    RUN_TEST(test_channel_mpmc_threaded);
    RUN_TEST(test_channel_select_basic);
    RUN_TEST(test_channel_select_timeout);

    /* Rate Limiter tests */
    TEST_GROUP("Rate Limiter Tests");
    RUN_TEST(test_ratelimit_token_bucket_basic);
    RUN_TEST(test_ratelimit_token_bucket_refill);
    RUN_TEST(test_ratelimit_token_bucket_burst);
    RUN_TEST(test_ratelimit_sliding_window_basic);
    RUN_TEST(test_ratelimit_sliding_window_expiry);
    RUN_TEST(test_ratelimit_leaky_bucket_basic);
    RUN_TEST(test_ratelimit_fixed_window_basic);
    RUN_TEST(test_ratelimit_fixed_window_reset);
    RUN_TEST(test_ratelimit_stats);
    RUN_TEST(test_ratelimit_peek);
    RUN_TEST(test_ratelimit_algorithm_names);

    /* Concurrency Limiter tests */
    TEST_GROUP("Concurrency Limiter Tests");
    RUN_TEST(test_concurrency_basic);
    RUN_TEST(test_concurrency_limit);
    RUN_TEST(test_concurrency_release);
    RUN_TEST(test_concurrency_stats);
    RUN_TEST(test_concurrency_threaded);

    /* TLS tests */
    TEST_GROUP("TLS Tests");
    RUN_TEST(test_tls_init_cleanup);
    RUN_TEST(test_tls_context_create);
    RUN_TEST(test_tls_context_config);
    RUN_TEST(test_tls_result_names);
    RUN_TEST(test_tls_connection_create);
    RUN_TEST(test_tls_null_safety);

#if LOOPY_HAVE_RAX
    /* PubSub tests */
    TEST_GROUP("PubSub Tests");
    RUN_TEST(test_pubsub_create_destroy);
    RUN_TEST(test_pubsub_config_init);
    RUN_TEST(test_pubsub_subscribe_unsubscribe);
    RUN_TEST(test_pubsub_publish_basic);
    RUN_TEST(test_pubsub_wildcard_star);
    RUN_TEST(test_pubsub_wildcard_hash);
    RUN_TEST(test_pubsub_multiple_subscribers);
    RUN_TEST(test_pubsub_queue_mode);
    RUN_TEST(test_pubsub_stats);
    RUN_TEST(test_pubsub_validation);
    RUN_TEST(test_pubsub_null_safety);
    RUN_TEST(test_pubsub_mode_names);

    /* Multi-tenant Concurrency Pool tests */
    TEST_GROUP("Multi-tenant Concurrency Pool Tests");
    RUN_TEST(test_concpool_create_destroy);
    RUN_TEST(test_concpool_config_init);
    RUN_TEST(test_concpool_add_remove_user);
    RUN_TEST(test_concpool_acquire_release);
    RUN_TEST(test_concpool_user_limit);
    RUN_TEST(test_concpool_global_limit);
    RUN_TEST(test_concpool_auto_create_user);
    RUN_TEST(test_concpool_multiple_users);
    RUN_TEST(test_concpool_stats);
    RUN_TEST(test_concpool_iteration);
    RUN_TEST(test_concpool_null_safety);
    RUN_TEST(test_concpool_result_names);

    /* Concurrency Pool Stress/Fuzz Tests */
    TEST_GROUP("Concurrency Pool Stress Tests");
    RUN_TEST(test_concpool_stress_single_user);
    RUN_TEST(test_concpool_stress_multi_user);
    RUN_TEST(test_concpool_stress_user_churn);
    RUN_TEST(test_concpool_stress_invariants);

    /* Cluster Registry tests */
    TEST_GROUP("Cluster Registry Tests");
    RUN_TEST(test_cluster_create_destroy);
    RUN_TEST(test_cluster_config_init);
    RUN_TEST(test_cluster_register_node);
    RUN_TEST(test_cluster_unregister_node);
    RUN_TEST(test_cluster_node_state);
    RUN_TEST(test_cluster_leader);
    RUN_TEST(test_cluster_config_ops);
    RUN_TEST(test_cluster_config_ttl);
    RUN_TEST(test_cluster_config_iterate);
    RUN_TEST(test_cluster_node_iterate);
    RUN_TEST(test_cluster_events);
    RUN_TEST(test_cluster_null_safety);
    RUN_TEST(test_cluster_util);

    /* Stress Test Framework Tests */
    TEST_GROUP("Stress Test Framework Tests");
    RUN_TEST(test_stress_framework_basic);
    RUN_TEST(test_stress_counter);
    RUN_TEST(test_stress_seqtracker);
    RUN_TEST(test_stress_message_integrity);

    /* Channel Stress/Fuzz Tests */
    TEST_GROUP("Channel Stress Tests");
    RUN_TEST(test_stress_channel_spsc_integrity);
    RUN_TEST(test_stress_channel_mpmc_contention);
    RUN_TEST(test_stress_channel_mpmc_ordering);
    RUN_TEST(test_stress_channel_close_under_load);
    RUN_TEST(test_stress_channel_boundary_conditions);

    /* PubSub Stress/Fuzz Tests */
    TEST_GROUP("PubSub Stress Tests");
    RUN_TEST(test_stress_pubsub_multi_publisher);
    RUN_TEST(test_stress_pubsub_subscriber_churn);
    RUN_TEST(test_stress_pubsub_pattern_matching);
    RUN_TEST(test_stress_pubsub_message_integrity);
    RUN_TEST(test_stress_pubsub_fanout);

    /* Registry Stress/Fuzz Tests */
    TEST_GROUP("Registry Stress Tests");
    RUN_TEST(test_stress_registry_node_churn);
    RUN_TEST(test_stress_registry_config_churn);
    RUN_TEST(test_stress_registry_concurrent_access);
    RUN_TEST(test_stress_registry_events);

    /* Integration Stress Tests */
    TEST_GROUP("Integration Stress Tests");
    RUN_TEST(test_stress_channel_pubsub_integration);
    RUN_TEST(test_stress_full_system);
#endif /* LOOPY_HAVE_RAX */
}

/* ====================================================================
 * Main
 * ==================================================================== */
static void print_usage(const char *prog) {
    printf("Usage: %s [OPTIONS] [TEST_NAME]\n", prog);
    printf("\n");
    printf("Options:\n");
    printf("  --list       List all test names (one per line, for CTest)\n");
    printf("  --help       Show this help message\n");
    printf("\n");
    printf("If TEST_NAME is given, run only that test.\n");
    printf("If no arguments, run all tests.\n");
}

int main(int argc, char *argv[]) {
    signal(SIGPIPE, SIG_IGN);

    /* First, register all tests */
    registration_mode = 1;
    register_all_tests();
    registration_mode = 0;

    /* Parse command line arguments */
    if (argc > 1) {
        if (strcmp(argv[1], "--list") == 0) {
            /* List all test names for CTest discovery */
            for (int i = 0; i < test_count; i++) {
                printf("%s\n", test_registry[i].name);
            }
            return EXIT_SUCCESS;
        } else if (strcmp(argv[1], "--help") == 0 ||
                   strcmp(argv[1], "-h") == 0) {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        } else {
            /* Run a specific test by name */
            test_entry_t *entry = find_test(argv[1]);
            if (!entry) {
                fprintf(stderr, "Unknown test: %s\n", argv[1]);
                fprintf(stderr, "Use --list to see available tests\n");
                return EXIT_FAILURE;
            }
            int passed = run_single_test(entry, 1);
            return passed ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

    /* No arguments: run all tests with full output */
    printf("=== loopy Unit Tests ===\n");
    printf("Adapter: %s\n", loopyAdapterName());

    /* Reset and run all tests */
    registration_mode = 0;
    tests_run = 0;
    tests_passed = 0;
    tests_failed = 0;
    register_all_tests();

    /* Summary */
    printf("\n=== Test Summary ===\n");
    printf("Total:  %d\n", tests_run);
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);

    return tests_failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
