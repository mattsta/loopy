# CMakeLists.txt for loopy examples
#
# All examples are built as executables and can be run with --test flag
# for automated testing in the CTest suite.

# Find required threading library
find_package(Threads REQUIRED)

# Examples that demonstrate core loopy functionality
set(LOOPY_EXAMPLES
    echo_server
    dns_lookup
    timer_example
    file_watcher
)

# Add rax-dependent examples if rax is available
if(LOOPY_HAVE_RAX)
    list(APPEND LOOPY_EXAMPLES
        pubsub_chat
        pubsub_network
    )
endif()

# Build each example
foreach(example ${LOOPY_EXAMPLES})
    add_executable(${example} ${example}.c)
    # Link against loopy-static (provides all dependencies transitively)
    target_link_libraries(${example} PRIVATE loopy-static)
    target_include_directories(${example} PRIVATE ${CMAKE_SOURCE_DIR}/src)

    # Set output directory to examples/ in build dir
    set_target_properties(${example} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples
    )
endforeach()

# Register examples as tests (run with --test flag)
# Timer example - quick timer verification
add_test(NAME example_timer
    COMMAND $<TARGET_FILE:timer_example> --test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
set_tests_properties(example_timer PROPERTIES
    TIMEOUT 5
    LABELS "example"
)

# Echo server - TCP networking test
add_test(NAME example_echo_server
    COMMAND $<TARGET_FILE:echo_server> --test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
set_tests_properties(example_echo_server PROPERTIES
    TIMEOUT 10
    LABELS "example;network"
)

# DNS lookup - async DNS resolution test
add_test(NAME example_dns_lookup
    COMMAND $<TARGET_FILE:dns_lookup> --test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
set_tests_properties(example_dns_lookup PROPERTIES
    TIMEOUT 15
    LABELS "example;network"
)

# File watcher - filesystem watching test
add_test(NAME example_file_watcher
    COMMAND $<TARGET_FILE:file_watcher> --test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
set_tests_properties(example_file_watcher PROPERTIES
    TIMEOUT 5
    LABELS "example;filesystem"
)

# Rax-dependent example tests (only if rax is available)
if(LOOPY_HAVE_RAX)
    # Pub/Sub chat - messaging test
    add_test(NAME example_pubsub_chat
        COMMAND $<TARGET_FILE:pubsub_chat> --test
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
    set_tests_properties(example_pubsub_chat PROPERTIES
        TIMEOUT 5
        LABELS "example;rax"
    )

    # Pub/Sub network - networked messaging test
    add_test(NAME example_pubsub_network
        COMMAND $<TARGET_FILE:pubsub_network> --test
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
    set_tests_properties(example_pubsub_network PROPERTIES
        TIMEOUT 5
        LABELS "example;network;rax"
    )
endif()
