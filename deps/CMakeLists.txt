# Dependencies are managed by DepsManager (see deps/deps.cmake)
# This file only handles optional manual dependencies

# rax - radix tree for topic routing (optional dependency)
# Only build rax-dependent features if deps/rax exists
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/rax/src/CMakeLists.txt")
    message(STATUS "Found rax dependency - enabling pub/sub features")
    add_subdirectory(rax EXCLUDE_FROM_ALL)
    set(LOOPY_HAVE_RAX ON PARENT_SCOPE)
else()
    message(STATUS "rax not found in deps/ - pub/sub features disabled")
    set(LOOPY_HAVE_RAX OFF PARENT_SCOPE)
endif()
# vi:ai et sw=4 ts=4:
