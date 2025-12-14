cmake_minimum_required(VERSION 3.10)

set(LOOPY_VERSION 0.3.0)
set(LOOPY_ABI_UPDATE 0.3.0)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(${CMAKE_SYSTEM_NAME} MATCHES "SunOS")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D__EXTENSIONS__ -D_XPG6")
    # Add: FINAL_LIBS+= -ldl
elseif(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    set(CMAKE_SHARED_MODULE_CREATE_C_FLAGS
        "${CMAKE_SHARED_MODULE_CREATE_C_FLAGS} -undefined dynamic_lookup")
    cmake_policy(SET CMP0042 NEW)
elseif(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    # Nothing special
endif()

option(Build32Bit "Build 32-bit Library" OFF)
option(USE_IOURING "Use io_uring on Linux (with epoll fallback)" OFF)

# If you need debug, build with:
# cmake -DCMAKE_BUILD_TYPE=Debug ..
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -pedantic -std=c11 -Wno-missing-field-initializers")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wstrict-overflow")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wshadow -fno-strict-aliasing -Wunused-macros")

#set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address -fno-omit-frame-pointer -fno-optimize-sibling-calls")

#set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=memory")
#set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=undefined")
#set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=dataflow")
#set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=cfi")
#set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=safe-stack")

if(CMAKE_BUILD_TYPE MATCHES Debug)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O0 -Wno-format")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3")
endif()

if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    add_definitions(-D_DARWIN_UNLIMITED_SELECT)
endif()

# Bring into existence loopy.c + loopyAdapter[PLATFORM].c based on
# currently detected build operating system
if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin|BSD")
    file(GLOB ADAPTER loopyAdapterBSD.c)
elseif(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    if(USE_IOURING)
        file(GLOB ADAPTER loopyAdapterIouring.c)
        message(STATUS "Using io_uring adapter with epoll fallback")
        add_definitions(-DUSE_IOURING)
    else()
        file(GLOB ADAPTER loopyAdapterLinux.c)
        message(STATUS "Using epoll adapter")
    endif()
elseif(${CMAKE_SYSTEM_NAME} MATCHES "SunOS")
    file(GLOB ADAPTER loopyAdapterSolaris.c)
else()
    message(WARNING "Support for ${CMAKE_SYSTEM_NAME} not found, using slower select() fallback.")
    file(GLOB ADAPTER loopyAdapterGeneric.c)
endif()

include(CheckSymbolExists)
list(APPEND CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
check_symbol_exists(accept4 sys/socket.h HAVE_ACCEPT4)
list(REMOVE_ITEM CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)

if(HAVE_ACCEPT4)
    list(APPEND CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
    check_symbol_exists(SOCK_NONBLOCK bits/socket_type.h HAVE_SOCK_NONBLOCK)
    list(REMOVE_ITEM CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
    if (HAVE_SOCK_NONBLOCK)
        add_definitions(-DUSE_ACCEPT4)
    endif()
endif()

# Get loopy root directory (this CMakeLists.txt is in loopy/src/)
set(LOOPY_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Include mbedtls headers for TLS support
# mbedtls 4.x has split crypto into tf-psa-crypto; need multiple include paths
# Note: DepsManager creates symlinks in deps/ to maintain backward compatibility
include_directories(${LOOPY_ROOT_DIR}/deps/mbedtls/include)
include_directories(${LOOPY_ROOT_DIR}/deps/mbedtls/tf-psa-crypto/include)
include_directories(${LOOPY_ROOT_DIR}/deps/mbedtls/tf-psa-crypto/drivers/builtin/include)
include_directories(${LOOPY_ROOT_DIR}/deps/mbedtls/tf-psa-crypto/drivers/builtin/include/mbedtls/private)

# Include rax headers for topic-based pub/sub routing (if available)
if(LOOPY_HAVE_RAX)
    include_directories(${LOOPY_ROOT_DIR}/deps/rax/src)
    add_definitions(-DLOOPY_HAVE_RAX=1)
else()
    add_definitions(-DLOOPY_HAVE_RAX=0)
endif()

# Base library source files
set(LOOPY_SOURCES
    ${ADAPTER}
    loopyMaxHeap.c
    loopyNet.c
    loopyNice.c
    loopyAsync.c
    loopyWork.c
    loopyUDP.c
    loopyProcess.c
    loopyIdle.c
    loopyStream.c
    loopyFS.c
    loopyMmap.c
    loopyFlock.c
    loopyTimer.c
    loopyPipe.c
    loopyConnPool.c
    loopyTTY.c
    loopyDNS.c
    loopySignal.c
    loopyWatch.c
    loopyFSPoll.c
    loopyRandom.c
    loopyMetrics.c
    loopySys.c
    loopyChannel.c
    loopyRateLimit.c
    loopyTLS.c
)

# Add rax-dependent modules if rax is available
if(LOOPY_HAVE_RAX)
    list(APPEND LOOPY_SOURCES
        loopyPubSub.c
        loopyConcurrencyPool.c
        loopyClusterRegistry.c
    )
endif()

add_library(${PROJECT_NAME} OBJECT ${LOOPY_SOURCES})

# Build list of dependencies
set(deps
    $<TARGET_OBJECTS:lz4>
    $<TARGET_OBJECTS:sha1>
    $<TARGET_OBJECTS:varint>
    $<TARGET_OBJECTS:xxhash>
    $<TARGET_OBJECTS:datakit>
)

# Add rax_topic if available
if(LOOPY_HAVE_RAX)
    list(APPEND deps $<TARGET_OBJECTS:rax_topic>)
endif()

add_library(${PROJECT_NAME}-shared  MODULE $<TARGET_OBJECTS:${PROJECT_NAME}> ${deps})
add_library(${PROJECT_NAME}-static  STATIC $<TARGET_OBJECTS:${PROJECT_NAME}> ${deps})
add_library(${PROJECT_NAME}-library SHARED $<TARGET_OBJECTS:${PROJECT_NAME}> ${deps})

# Don't prefix "lib"
set(CMAKE_STATIC_LIBRARY_PREFIX "")
set(CMAKE_SHARED_MODULE_PREFIX "")
#set(CMAKE_SHARED_LIBRARY_PREFIX "")

set_target_properties(${PROJECT_NAME}-shared  PROPERTIES OUTPUT_NAME ${PROJECT_NAME})
set_target_properties(${PROJECT_NAME}-static  PROPERTIES OUTPUT_NAME ${PROJECT_NAME})
set_target_properties(${PROJECT_NAME}-library PROPERTIES OUTPUT_NAME ${PROJECT_NAME})
# pthread needed for async DNS module
find_package(Threads REQUIRED)
# Link with mbedtls for TLS support (mbedtls 4.x uses tfpsacrypto instead of mbedcrypto)
target_link_libraries(${PROJECT_NAME}-static PUBLIC datakit-static mbedtls mbedx509 tfpsacrypto Threads::Threads)
target_link_libraries(${PROJECT_NAME}-library datakit-library mbedtls mbedx509 tfpsacrypto Threads::Threads)

# SOVERSION only needs to increment when introducing *breaking* changes.
# Otherwise, just increase VERSION with normal feature additions or maint.
set_target_properties(${PROJECT_NAME}-library PROPERTIES VERSION ${LOOPY_VERSION} SOVERSION ${LOOPY_ABI_UPDATE})

option(BuildTestBinary "Build test binary" On)

if(BuildTestBinary)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DLOOPY_TEST")

    # Test-only infrastructure (not part of the public library)
    add_library(${PROJECT_NAME}-test-infrastructure OBJECT loopyStressTest.c)

    add_executable(${PROJECT_NAME}Test loopyTest.c)
    target_link_libraries(${PROJECT_NAME}Test ${PROJECT_NAME}-static datakit-static mbedtls mbedx509 tfpsacrypto Threads::Threads)

    add_executable(${PROJECT_NAME}UnitTest loopyUnitTest.c $<TARGET_OBJECTS:${PROJECT_NAME}-test-infrastructure>)
    target_link_libraries(${PROJECT_NAME}UnitTest ${PROJECT_NAME}-static datakit-static mbedtls mbedx509 tfpsacrypto Threads::Threads)

    # Stress testing CLI tool with dynamic configuration
    add_executable(${PROJECT_NAME}StressCLI loopyStressCLI.c $<TARGET_OBJECTS:${PROJECT_NAME}-test-infrastructure>)
    target_link_libraries(${PROJECT_NAME}StressCLI ${PROJECT_NAME}-static datakit-static mbedtls mbedx509 tfpsacrypto Threads::Threads)

    if(APPLE)
        add_custom_command(TARGET ${PROJECT_NAME}Test POST_BUILD COMMAND dsymutil ${PROJECT_NAME}Test COMMENT "Generating OS X Debug Info")
        add_custom_command(TARGET ${PROJECT_NAME}UnitTest POST_BUILD COMMAND dsymutil ${PROJECT_NAME}UnitTest COMMENT "Generating OS X Debug Info")
        add_custom_command(TARGET ${PROJECT_NAME}StressCLI POST_BUILD COMMAND dsymutil ${PROJECT_NAME}StressCLI COMMENT "Generating OS X Debug Info")
    endif()

    # ========================================================================
    # CTest Integration - Register individual tests for parallel execution
    # ========================================================================

    # Add a meta-test that runs all tests sequentially (for backward compat)
    add_test(NAME loopy_all_tests
             COMMAND ${PROJECT_NAME}UnitTest
             WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

    # Generate CTest test registration at configure time by running the test
    # binary with --list. This requires a two-pass build: first build, then
    # reconfigure to discover tests. For CI, we use a script approach instead.

    # Create a script that discovers and registers tests at build time
    set(LOOPY_TEST_NAMES
        # Basic Loop Tests
        test_loop_create_delete test_loop_stack_allocated test_loop_resize test_adapter_name
        # File Descriptor Tests
        test_register_read test_register_write test_register_read_write
        test_unregister_read test_unregister_write test_unregister_all
        test_register_write_if_none_exists test_auto_resize test_maxfd_tracking
        # Timer Tests
        test_timer_registration
        # Event Processing Tests
        test_event_processing test_sleep_callbacks test_stop
        # Network Tests
        test_net_tcp4_server test_net_tcp6_server test_net_nonblock
        test_net_nodelay test_net_keepalive test_net_resolve test_net_format_addr
        test_tcp_connect_accept test_unix_server test_unix_connect test_send_timeout
        # loopyNice Tests
        test_nice_create_delete test_nice_pipe test_nice_socketpair test_nice_tcp_server
        # Max-Heap Tests
        test_heap_create_delete test_heap_stack_allocated test_heap_insert_peek
        test_heap_pop test_heap_remove test_heap_contains test_heap_clear
        test_heap_reserve test_heap_auto_grow test_heap_large_fds test_heap_duplicates
        test_heap_empty_operations test_heap_null_safety test_heap_single_element
        test_heap_remove_last test_heap_stress test_heap_loopy_maxfd_integration
        # Edge Case Tests
        test_null_handling test_unregister_nonexistent test_get_events_invalid_fd
        # DNS Tests
        test_dns_create_delete test_dns_create_default_config test_dns_status_strings
        test_dns_resolve_localhost test_dns_cancel test_dns_cancel_all
        test_dns_null_safety test_dns_reverse_lookup test_dns_reverse_null_safety
        # Signal Tests
        test_signal_create_delete test_signal_single_handler test_signal_register_unregister
        test_signal_delivery test_signal_names test_signal_invalid_signum
        test_signal_null_safety test_signal_oneshot
        # Async Tests
        test_async_create_delete test_async_send_same_thread test_async_send_from_thread
        test_async_coalescing test_async_multiple_handles test_async_user_data
        test_async_backend_name test_async_null_safety
        # Work Tests
        test_work_create_delete test_work_create_default_config test_work_queue_execute
        test_work_cancel_pending test_work_cancel_all test_work_multiple_concurrent
        test_work_status_strings test_work_null_safety test_work_queue_overflow
        # Watch Tests
        test_watch_create_delete test_watch_add_remove test_watch_multiple
        test_watch_modify_event test_watch_event_names test_watch_backend_name
        test_watch_nonexistent_path test_watch_null_safety
        # FSPoll Tests
        test_fspoll_create_delete test_fspoll_detect_modify test_fspoll_detect_chmod
        test_fspoll_detect_delete test_fspoll_start_stop test_fspoll_null_safety
        # UDP Tests
        test_udp_create_delete test_udp_bind_ipv4 test_udp_bind_ipv6
        test_udp_send_recv test_udp_connect test_udp_try_send test_udp_broadcast
        test_udp_ttl test_udp_recv_stop test_udp_null_safety
        test_udp_batch_send_recv test_udp_batch_connected test_udp_batch_native_check
        test_udp_batch_null_safety
        test_udp_gso_detection test_udp_gso_enable_disable test_udp_gso_invalid_segment_size
        test_udp_gso_send_not_enabled test_udp_gso_null_safety test_udp_gso_send_basic
        test_udp_gso_connected_send test_udp_gro_detection test_udp_gro_enable_disable
        test_udp_gro_null_safety test_udp_pmtu_set_mode test_udp_pmtu_get
        test_udp_pmtu_null_safety test_udp_pmtu_with_connect
        # Process Tests
        test_process_spawn_simple test_process_exit_code test_process_capture_stdout
        test_process_kill test_process_cwd test_process_env
        test_process_spawn_failure test_process_null_safety
        # Idle/Prepare/Check Tests
        test_idle_create_delete test_prepare_create_delete test_check_create_delete
        test_idle_callback_fires test_idle_auto_stop test_prepare_callback_fires
        test_check_callback_fires test_idle_stop_restart test_idle_multiple_handles
        test_idle_null_safety test_idle_rapid_iterations test_check_fires_with_idle_only
        test_prepare_fires_each_iteration test_idle_prepare_check_order test_idle_timer_no_block
        # Stream Tests
        test_stream_create_delete_tcp test_stream_create_delete_pipe test_stream_from_fd
        test_stream_bind_listen test_stream_accept test_stream_pipe_read_write
        test_stream_try_write test_stream_shutdown test_stream_write_queue
        test_stream_null_safety test_stream_fd_passing_basic test_stream_fd_passing_multiple
        test_stream_fd_passing_not_pipe test_stream_fd_passing_null_safety
        # FS Tests
        test_fs_sync_operations test_fs_async_open_close test_fs_stat
        test_fs_mkdir_rmdir test_fs_rename test_fs_pread_pwrite
        test_fs_truncate_fsync test_fs_chmod test_fs_error_handling test_fs_null_safety
        # Extended FS Tests
        test_fs_link_symlink test_fs_realpath_access test_fs_scandir
        test_fs_utime test_fs_copyfile test_fs_mkstemp_mkdtemp test_fs_extended_null_safety
        # TTY Tests
        test_tty_is_tty test_tty_create_non_tty test_tty_create_if_available
        test_tty_mode_enum test_tty_null_safety test_tty_reset_all
        # Random Tests
        test_random_sync_basic test_random_sync_various_sizes test_random_async
        test_random_uniqueness test_random_null_safety
        # Metrics Tests
        test_metrics_enable_disable test_metrics_loop_iterations test_metrics_events_processed
        test_metrics_reset test_metrics_null_safety test_metrics_poll_time
        test_metrics_timers_processed
        # System Utilities Tests
        test_sys_interface_addresses test_sys_available_parallelism test_sys_exe_path
        test_sys_statfs test_sys_null_safety test_sys_homedir test_sys_tmpdir
        test_sys_cwd_chdir test_sys_hostname test_sys_paths_null_safety
        # Mmap Tests
        test_mmap_utilities test_mmap_file_readonly test_mmap_file_readwrite
        test_mmap_anonymous test_mmap_shared_ipc test_mmap_advice test_mmap_sync
        test_mmap_lock_unlock test_mmap_offset_length test_mmap_error_handling
        test_mmap_null_safety
        # Direct I/O Tests (O_DIRECT)
        test_direct_io_aligned_allocation test_direct_io_aligned_edge_cases
        test_direct_io_read_write test_direct_io_alignment_requirements
        test_direct_io_various_sizes test_direct_io_null_safety
        # Connection Pool Tests
        test_connpool_create_delete test_connpool_default_config
        test_connpool_acquire_release test_connpool_connection_reuse
        test_connpool_min_idle test_connpool_max_total test_connpool_validation
        test_connpool_unhealthy_release test_connpool_statistics
        test_connpool_health_check test_connpool_idle_cleanup test_connpool_prewarm
        test_connpool_null_safety test_connpool_async_acquire
        # File Locking Tests
        test_flock_capabilities test_flock_exclusive_basic test_flock_shared_basic
        test_flock_fcntl_whole_file test_flock_fcntl_byte_range test_flock_test_range
        test_flock_highlevel_api test_flock_highlevel_fcntl_range
        test_flock_lockfile_pattern test_flock_error_handling test_flock_null_safety
        # Channel Tests
        test_channel_create_destroy test_channel_config_init test_channel_spsc_send_recv
        test_channel_mpsc_send_recv test_channel_mpmc_send_recv test_channel_try_send_recv
        test_channel_full_empty test_channel_close test_channel_stats test_channel_type_names
        test_channel_status_names test_channel_null_safety test_channel_introspection
        test_channel_spsc_threaded test_channel_mpsc_threaded test_channel_mpmc_threaded
        test_channel_select_basic test_channel_select_timeout
        # Rate Limiter Tests
        test_ratelimit_token_bucket_basic test_ratelimit_token_bucket_refill
        test_ratelimit_token_bucket_burst test_ratelimit_sliding_window_basic
        test_ratelimit_sliding_window_expiry test_ratelimit_leaky_bucket_basic
        test_ratelimit_fixed_window_basic test_ratelimit_fixed_window_reset
        test_ratelimit_stats test_ratelimit_peek test_ratelimit_algorithm_names
        test_concurrency_basic test_concurrency_limit test_concurrency_release
        test_concurrency_stats test_concurrency_threaded
        # TLS Tests
        test_tls_init_cleanup test_tls_context_create test_tls_context_config
        test_tls_result_names test_tls_connection_create test_tls_null_safety
    )

    # Rax-dependent tests (only registered if LOOPY_HAVE_RAX is ON)
    set(LOOPY_RAX_TEST_NAMES
        # PubSub Tests
        test_pubsub_create_destroy test_pubsub_config_init test_pubsub_subscribe_unsubscribe
        test_pubsub_publish_basic test_pubsub_wildcard_star test_pubsub_wildcard_hash
        test_pubsub_multiple_subscribers test_pubsub_queue_mode test_pubsub_stats
        test_pubsub_validation test_pubsub_null_safety test_pubsub_mode_names
        # Multi-tenant Concurrency Pool Tests
        test_concpool_create_destroy test_concpool_config_init test_concpool_add_remove_user
        test_concpool_acquire_release test_concpool_user_limit test_concpool_global_limit
        test_concpool_auto_create_user test_concpool_multiple_users test_concpool_stats
        test_concpool_iteration test_concpool_null_safety test_concpool_result_names
        # Concurrency Pool Stress/Fuzz Tests
        test_concpool_stress_single_user test_concpool_stress_multi_user
        test_concpool_stress_user_churn test_concpool_stress_invariants
        # Cluster Registry Tests
        test_cluster_create_destroy test_cluster_config_init test_cluster_register_node
        test_cluster_unregister_node test_cluster_node_state test_cluster_leader
        test_cluster_config_ops test_cluster_config_ttl test_cluster_config_iterate
        test_cluster_node_iterate test_cluster_events test_cluster_null_safety
        test_cluster_util
        # PubSub Stress/Fuzz Tests
        test_stress_pubsub_multi_publisher test_stress_pubsub_subscriber_churn
        test_stress_pubsub_pattern_matching test_stress_pubsub_message_integrity
        test_stress_pubsub_fanout
        # Registry Stress/Fuzz Tests
        test_stress_registry_node_churn test_stress_registry_config_churn
        test_stress_registry_concurrent_access test_stress_registry_events
        # Integration Stress Tests
        test_stress_channel_pubsub_integration test_stress_full_system
        # Channel Stress/Fuzz Tests
        test_stress_channel_spsc_integrity test_stress_channel_mpmc_contention
        test_stress_channel_mpmc_ordering test_stress_channel_close_under_load
        test_stress_channel_boundary_conditions
        # Stress Test Framework Tests
        test_stress_framework_basic test_stress_counter test_stress_seqtracker
        test_stress_message_integrity
    )

    # Register each test individually for parallel execution
    foreach(test_name ${LOOPY_TEST_NAMES})
        add_test(NAME ${test_name}
                 COMMAND ${PROJECT_NAME}UnitTest ${test_name}
                 WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
        # Set test properties for better parallelism
        set_tests_properties(${test_name} PROPERTIES
            TIMEOUT 60
            LABELS "unit")
    endforeach()

    # Register rax-dependent tests (only if rax is available)
    if(LOOPY_HAVE_RAX)
        foreach(test_name ${LOOPY_RAX_TEST_NAMES})
            add_test(NAME ${test_name}
                     COMMAND ${PROJECT_NAME}UnitTest ${test_name}
                     WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
            set_tests_properties(${test_name} PROPERTIES
                TIMEOUT 60
                LABELS "unit;rax")
        endforeach()
    endif()

    # Linux io_uring-specific tests (io_uring is Linux-specific)
    if(${CMAKE_SYSTEM_NAME} MATCHES "Linux" AND USE_IOURING)
        set(IOURING_TESTS
            # io_uring backend tests
            test_iouring_api_available test_iouring_null_safety
            test_iouring_adapter_name test_iouring_event_loop_works
            # io_uring fixed buffer tests
            test_iouring_fixed_buffers_registration test_iouring_fixed_buffers_read_write
            test_iouring_fixed_buffers_error_handling test_iouring_fixed_buffers_multiple
            test_iouring_fixed_buffers_null_safety
            # io_uring file operation tests
            test_iouring_fs_api_available test_iouring_fs_write_basic test_iouring_fs_read_basic
            test_iouring_fs_open_close test_iouring_fs_fsync test_iouring_fs_fdatasync
            test_iouring_fs_read_offset test_iouring_fs_invalid_fd test_iouring_fs_null_safety
            test_iouring_fs_concurrent_ops test_iouring_fs_fallback_non_iouring
            # io_uring network I/O tests
            test_iouring_net_send_basic test_iouring_net_recv_basic test_iouring_net_accept_basic
            test_iouring_net_connect_basic test_iouring_net_sendmsg_basic test_iouring_net_recvmsg_basic
            test_iouring_net_udp_send test_iouring_net_udp_recv test_iouring_net_shutdown
            test_iouring_net_accept_multishot test_iouring_net_concurrent_ops test_iouring_net_invalid_fd
            test_iouring_net_null_safety test_iouring_net_large_transfer test_iouring_net_fallback
        )
        foreach(test_name ${IOURING_TESTS})
            add_test(NAME ${test_name}
                     COMMAND ${PROJECT_NAME}UnitTest ${test_name}
                     WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
            set_tests_properties(${test_name} PROPERTIES
                TIMEOUT 60
                LABELS "unit;linux;io_uring")
        endforeach()
    endif()

    # Some tests that use the same port or signal handlers may conflict
    # Mark them to not run in parallel with each other using RESOURCE_LOCK
    set(NETWORK_TESTS
        test_net_tcp4_server test_net_tcp6_server test_tcp_connect_accept
        test_unix_server test_unix_connect test_nice_tcp_server
        test_stream_bind_listen test_stream_accept test_udp_bind_ipv4
        test_udp_bind_ipv6 test_udp_send_recv test_udp_connect
        test_udp_broadcast test_udp_batch_send_recv test_udp_batch_connected
        test_udp_gso_send_basic test_udp_gso_connected_send test_udp_pmtu_with_connect
    )
    foreach(test_name ${NETWORK_TESTS})
        if(TEST ${test_name})
            set_tests_properties(${test_name} PROPERTIES
                RESOURCE_LOCK "network_port")
        endif()
    endforeach()

    set(SIGNAL_TESTS
        test_signal_single_handler test_signal_register_unregister
        test_signal_delivery test_signal_oneshot
    )
    foreach(test_name ${SIGNAL_TESTS})
        if(TEST ${test_name})
            set_tests_properties(${test_name} PROPERTIES
                RESOURCE_LOCK "signal_handler")
        endif()
    endforeach()

    # ========================================================================
    # Stress CLI Test Configurations for CI
    # ========================================================================
    # These tests use the stress CLI tool with various configurations for
    # automated CI testing. Tests are organized by duration/intensity:
    #   - "quick" label: Fast tests suitable for every commit (~1-5 seconds each)
    #   - "medium" label: More thorough tests for regular CI (~5-30 seconds each)
    #   - "thorough" label: Comprehensive tests for nightly/release (~30-120 seconds each)
    #
    # Configure stress test duration with LOOPY_STRESS_PROFILE:
    #   - "default"   : ~3 minute total runtime (good for development)
    #   - "extended"  : ~5 minute total runtime
    #   - "long"      : ~10 minute total runtime
    #   - "marathon"  : ~30 minute total runtime (nightly builds)
    #
    # Example: cmake -DLOOPY_STRESS_PROFILE=marathon ..

    set(LOOPY_STRESS_PROFILE "default" CACHE STRING "Stress test duration profile (default, extended, long, marathon)")
    set_property(CACHE LOOPY_STRESS_PROFILE PROPERTY STRINGS default extended long marathon)

    # Duration settings (milliseconds per test combination)
    if(LOOPY_STRESS_PROFILE STREQUAL "marathon")
        # ~30 minute total runtime
        set(STRESS_QUICK_DURATION 1500)
        set(STRESS_QUICK_TIMEOUT 90)
        set(STRESS_MEDIUM_DURATION 3000)
        set(STRESS_MEDIUM_TIMEOUT 360)
        set(STRESS_THOROUGH_DURATION 15000)
        set(STRESS_THOROUGH_TIMEOUT 1800)
        set(STRESS_THOROUGH_EVENTS_MIN 10000)
        set(STRESS_THOROUGH_EVENTS_MAX 20000)
    elseif(LOOPY_STRESS_PROFILE STREQUAL "long")
        # ~10 minute total runtime
        set(STRESS_QUICK_DURATION 1000)
        set(STRESS_QUICK_TIMEOUT 60)
        set(STRESS_MEDIUM_DURATION 2000)
        set(STRESS_MEDIUM_TIMEOUT 240)
        set(STRESS_THOROUGH_DURATION 8000)
        set(STRESS_THOROUGH_TIMEOUT 900)
        set(STRESS_THOROUGH_EVENTS_MIN 8000)
        set(STRESS_THOROUGH_EVENTS_MAX 16000)
    elseif(LOOPY_STRESS_PROFILE STREQUAL "extended")
        # ~5 minute total runtime
        set(STRESS_QUICK_DURATION 500)
        set(STRESS_QUICK_TIMEOUT 30)
        set(STRESS_MEDIUM_DURATION 1000)
        set(STRESS_MEDIUM_TIMEOUT 120)
        set(STRESS_THOROUGH_DURATION 5000)
        set(STRESS_THOROUGH_TIMEOUT 600)
        set(STRESS_THOROUGH_EVENTS_MIN 5000)
        set(STRESS_THOROUGH_EVENTS_MAX 10000)
    else()
        # default: ~3 minute total runtime
        set(STRESS_QUICK_DURATION 300)
        set(STRESS_QUICK_TIMEOUT 20)
        set(STRESS_MEDIUM_DURATION 600)
        set(STRESS_MEDIUM_TIMEOUT 60)
        set(STRESS_THOROUGH_DURATION 2000)
        set(STRESS_THOROUGH_TIMEOUT 300)
        set(STRESS_THOROUGH_EVENTS_MIN 2000)
        set(STRESS_THOROUGH_EVENTS_MAX 4000)
    endif()

    message(STATUS "Stress test profile: ${LOOPY_STRESS_PROFILE}")
    message(STATUS "  Quick duration: ${STRESS_QUICK_DURATION}ms, timeout: ${STRESS_QUICK_TIMEOUT}s")
    message(STATUS "  Medium duration: ${STRESS_MEDIUM_DURATION}ms, timeout: ${STRESS_MEDIUM_TIMEOUT}s")
    message(STATUS "  Thorough duration: ${STRESS_THOROUGH_DURATION}ms, timeout: ${STRESS_THOROUGH_TIMEOUT}s")

    # Quick Channel Tests - minimal configuration for fast feedback
    # Fixed capacity/msgsize to avoid combinatorial explosion (4 combos x 3 elements = 12 tests)
    add_test(NAME stress_cli_channel_quick
             COMMAND ${PROJECT_NAME}StressCLI --channel -p 1:2 -c 1:2 -e 100:500:200 -C 128 -m 64 -d ${STRESS_QUICK_DURATION} --stop-on-fail
             WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(stress_cli_channel_quick PROPERTIES
        TIMEOUT ${STRESS_QUICK_TIMEOUT}
        LABELS "stress;quick")

    # Rax-dependent stress CLI tests (only if rax is available)
    if(LOOPY_HAVE_RAX)
        # Quick PubSub Tests
        add_test(NAME stress_cli_pubsub_quick
                 COMMAND ${PROJECT_NAME}StressCLI --pubsub -p 1:2 -c 1:2 -e 100:300:100 -C 128 -m 64 -d ${STRESS_QUICK_DURATION} --stop-on-fail
                 WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(stress_cli_pubsub_quick PROPERTIES
            TIMEOUT ${STRESS_QUICK_TIMEOUT}
            LABELS "stress;quick;rax")

        # Quick Registry Tests
        add_test(NAME stress_cli_registry_quick
                 COMMAND ${PROJECT_NAME}StressCLI --registry -p 1:2 -c 1:2 -e 100:300:100 -d ${STRESS_QUICK_DURATION} --stop-on-fail
                 WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(stress_cli_registry_quick PROPERTIES
            TIMEOUT ${STRESS_QUICK_TIMEOUT}
            LABELS "stress;quick;rax")

        # Quick Integration Tests
        add_test(NAME stress_cli_integration_quick
                 COMMAND ${PROJECT_NAME}StressCLI --integration -p 1:2 -c 1:2 -e 100:200:100 -C 128 -m 64 -d ${STRESS_QUICK_DURATION} --stop-on-fail
                 WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(stress_cli_integration_quick PROPERTIES
            TIMEOUT ${STRESS_QUICK_TIMEOUT}
            LABELS "stress;quick;rax")
    endif()

    # Medium Channel Tests - broader coverage (9 thread combos x 2 elements = 18 combos)
    add_test(NAME stress_cli_channel_medium
             COMMAND ${PROJECT_NAME}StressCLI --channel -p 1:3 -c 1:3 -e 1000:2000:1000 -C 256 -m 128 -d ${STRESS_MEDIUM_DURATION} --stop-on-fail
             WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(stress_cli_channel_medium PROPERTIES
        TIMEOUT ${STRESS_MEDIUM_TIMEOUT}
        LABELS "stress;medium")

    if(LOOPY_HAVE_RAX)
        # Medium PubSub Tests - various topic patterns (9 thread combos x 2 elements = 18 combos)
        add_test(NAME stress_cli_pubsub_medium
                 COMMAND ${PROJECT_NAME}StressCLI --pubsub -p 1:3 -c 1:3 -e 500:1000:500 -C 256 -m 128 -d ${STRESS_MEDIUM_DURATION} --stop-on-fail
                 WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(stress_cli_pubsub_medium PROPERTIES
            TIMEOUT ${STRESS_MEDIUM_TIMEOUT}
            LABELS "stress;medium;rax")

        # Medium Registry Tests - concurrent config changes (9 thread combos x 2 elements = 18 combos)
        add_test(NAME stress_cli_registry_medium
                 COMMAND ${PROJECT_NAME}StressCLI --registry -p 1:3 -c 1:3 -e 500:1000:500 -d ${STRESS_MEDIUM_DURATION} --stop-on-fail
                 WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(stress_cli_registry_medium PROPERTIES
            TIMEOUT ${STRESS_MEDIUM_TIMEOUT}
            LABELS "stress;medium;rax")

        # Medium Integration Tests - all systems together (9 thread combos x 2 elements = 18 combos)
        add_test(NAME stress_cli_integration_medium
                 COMMAND ${PROJECT_NAME}StressCLI --integration -p 1:3 -c 1:3 -e 500:1000:500 -C 256 -m 128 -d ${STRESS_MEDIUM_DURATION} --stop-on-fail
                 WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(stress_cli_integration_medium PROPERTIES
            TIMEOUT ${STRESS_MEDIUM_TIMEOUT}
            LABELS "stress;medium;rax")
    endif()

    # Thorough Channel Tests - high contention (16 combos x 2 elements = 32 combos)
    add_test(NAME stress_cli_channel_thorough
             COMMAND ${PROJECT_NAME}StressCLI --channel -p 1:4 -c 1:4 -e ${STRESS_THOROUGH_EVENTS_MIN}:${STRESS_THOROUGH_EVENTS_MAX}:${STRESS_THOROUGH_EVENTS_MIN} -C 512 -m 256 -d ${STRESS_THOROUGH_DURATION} --stop-on-fail
             WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(stress_cli_channel_thorough PROPERTIES
        TIMEOUT ${STRESS_THOROUGH_TIMEOUT}
        LABELS "stress;thorough")

    if(LOOPY_HAVE_RAX)
        # Thorough PubSub Tests - many subscribers (16 combos x 2 elements = 32 combos)
        add_test(NAME stress_cli_pubsub_thorough
                 COMMAND ${PROJECT_NAME}StressCLI --pubsub -p 1:4 -c 1:4 -e ${STRESS_THOROUGH_EVENTS_MIN}:${STRESS_THOROUGH_EVENTS_MAX}:${STRESS_THOROUGH_EVENTS_MIN} -C 512 -m 256 -d ${STRESS_THOROUGH_DURATION} --stop-on-fail
                 WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(stress_cli_pubsub_thorough PROPERTIES
            TIMEOUT ${STRESS_THOROUGH_TIMEOUT}
            LABELS "stress;thorough;rax")

        # Thorough Integration Tests - full system (16 combos x 2 elements = 32 combos)
        add_test(NAME stress_cli_integration_thorough
                 COMMAND ${PROJECT_NAME}StressCLI --integration -p 1:4 -c 1:4 -e ${STRESS_THOROUGH_EVENTS_MIN}:${STRESS_THOROUGH_EVENTS_MAX}:${STRESS_THOROUGH_EVENTS_MIN} -C 512 -m 256 -d ${STRESS_THOROUGH_DURATION} --stop-on-fail
                 WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(stress_cli_integration_thorough PROPERTIES
            TIMEOUT ${STRESS_THOROUGH_TIMEOUT}
            LABELS "stress;thorough;rax")
    endif()

    # All stress CLI tests share the same resources, prevent parallel execution
    set(STRESS_CLI_TESTS
        stress_cli_channel_quick
        stress_cli_channel_medium
        stress_cli_channel_thorough
    )
    if(LOOPY_HAVE_RAX)
        list(APPEND STRESS_CLI_TESTS
            stress_cli_pubsub_quick stress_cli_registry_quick stress_cli_integration_quick
            stress_cli_pubsub_medium stress_cli_registry_medium stress_cli_integration_medium
            stress_cli_pubsub_thorough stress_cli_integration_thorough
        )
    endif()
    foreach(test_name ${STRESS_CLI_TESTS})
        set_tests_properties(${test_name} PROPERTIES
            RESOURCE_LOCK "stress_cli")
    endforeach()

endif()

if(Build32Bit)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -m32")
    set_target_properties(${PROJECT_NAME}-shared
                          PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
    set_target_properties(${PROJECT_NAME}-static
                          PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
    set_target_properties(${PROJECT_NAME}-library
                          PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
endif()
# vi:ai et sw=4 ts=4:
