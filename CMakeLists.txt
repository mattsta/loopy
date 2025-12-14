cmake_minimum_required(VERSION 3.16)
project(loopy
    VERSION 1.0.0
    DESCRIPTION "High-performance event loop library for C"
    LANGUAGES C)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_DISABLE_IN_SOURCE_BUILD ON)
set(CMAKE_DISABLE_SOURCE_CHANGES  ON)

if (TARGET ${PROJECT_NAME})
    return()
endif()

# Enable CTest for parallel test execution
enable_testing()

# ============================================================================
# Dependency Management
# ============================================================================
# Use DepsManager for automatic dependency fetching and local development
include(cmake/deps-manager/DepsManager.cmake)
deps_init(
    CMAKE_MIN_VERSION 3.16        # Patch all dependencies to cmake_minimum_required 3.16
    DEFAULT_GIT_SHALLOW           # Enable shallow clones by default
    DEFAULT_GIT_PROGRESS          # Show git progress by default
)

# Load dependency declarations
include(deps/deps.cmake)

# ============================================================================
# mbedtls 4.x Python requirements
# ============================================================================
# mbedtls 4.x requires Python with jsonschema module for driver wrapper generation.
# Set up virtual environment before resolving dependencies.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(MBEDTLS_VENV_DIR "${CMAKE_CURRENT_BINARY_DIR}/mbedtls_venv")
set(MBEDTLS_VENV_PYTHON "${MBEDTLS_VENV_DIR}/bin/python3")
set(MBEDTLS_VENV_PIP "${MBEDTLS_VENV_DIR}/bin/pip")

if(NOT EXISTS "${MBEDTLS_VENV_PYTHON}")
    message(STATUS "Creating Python virtual environment for mbedtls build...")
    execute_process(
        COMMAND ${Python3_EXECUTABLE} -m venv "${MBEDTLS_VENV_DIR}"
        RESULT_VARIABLE VENV_RESULT
    )
    if(NOT VENV_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to create virtual environment for mbedtls")
    endif()

    message(STATUS "Installing mbedtls Python dependencies (jsonschema, jinja2)...")
    execute_process(
        COMMAND ${MBEDTLS_VENV_PIP} install --quiet jsonschema jinja2
        RESULT_VARIABLE PIP_RESULT
    )
    if(NOT PIP_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to install mbedtls Python dependencies")
    endif()
endif()

# Tell mbedtls to use our venv Python
set(Python3_EXECUTABLE "${MBEDTLS_VENV_PYTHON}" CACHE FILEPATH "" FORCE)

# Allow configure_file to write into source tree (needed for mbedtls tf-psa-crypto)
# Set both the policy and the default so it propagates through FetchContent
if(POLICY CMP0150)
  cmake_policy(SET CMP0150 OLD)
endif()
set(CMAKE_POLICY_DEFAULT_CMP0150 OLD CACHE STRING "Allow configure_file into source tree")

# Enable verbose FetchContent to see actual git commands
set(FETCHCONTENT_QUIET OFF)

# Disable -Werror for all dependencies (CI builds have different compiler warnings)
set(CMAKE_COMPILE_WARNING_AS_ERROR OFF CACHE BOOL "Don't treat warnings as errors in dependencies")

# Save current flags and disable ALL warnings for dependencies (CI has different warnings)
set(LOOPY_SAVED_C_FLAGS "${CMAKE_C_FLAGS}")
set(LOOPY_SAVED_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
# Strip all warning-related flags and add -w to suppress all warnings
string(REGEX REPLACE "-W[^ ]*" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
string(REGEX REPLACE "-W[^ ]*" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -w" CACHE STRING "C flags with warnings disabled for deps" FORCE)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -w" CACHE STRING "CXX flags with warnings disabled for deps" FORCE)

# Resolve all dependencies (fetches if needed, finds local if available)
# DepsManager automatically creates symlinks in deps/ for backward compatibility
deps_resolve()

# Restore flags for main project
set(CMAKE_C_FLAGS "${LOOPY_SAVED_C_FLAGS}")
set(CMAKE_CXX_FLAGS "${LOOPY_SAVED_CXX_FLAGS}")

# Re-enable test building for loopy (deps may have disabled it globally)
set(BuildTestBinary ON CACHE BOOL "Build loopy test binaries" FORCE)

add_subdirectory(deps)
add_subdirectory(src)
add_subdirectory(examples)

# Override inherited cppcheck targets to only check loopy src/ directory
add_custom_target(loopy-cppcheck
    COMMAND cppcheck --enable=all --std=c11
        --suppress=missingIncludeSystem
        --suppress=toomanyconfigs
        --suppress=normalCheckLevelMaxBranches
        --error-exitcode=1
        --quiet
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    COMMENT "Running cppcheck on loopy src/ directory only"
    VERBATIM)

add_custom_target(loopy-cppcheck-full
    COMMAND cppcheck --enable=all --std=c11
        --suppress=missingIncludeSystem
        --suppress=toomanyconfigs
        --error-exitcode=1
        --quiet
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    COMMENT "Running full cppcheck on loopy src/ directory only"
    VERBATIM)

add_custom_target(loopy-cppcheck-xml
    COMMAND cppcheck --enable=all --std=c11
        --suppress=missingIncludeSystem
        --suppress=toomanyconfigs
        --xml-version=2
        ${CMAKE_CURRENT_SOURCE_DIR}/src > cppcheck-loopy.xml
    COMMENT "Running cppcheck with XML output on loopy src/ directory only"
    VERBATIM)

# vi:ai et sw=4 ts=4:
