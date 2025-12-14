# deps.cmake - Loopy Dependency Configuration
# ==============================================
#
# This file declares all external dependencies for the loopy project.
# Dependencies will be automatically fetched if not found locally.
#
# Local development: Create deps/.local.cmake to override paths
# Example:
#   set(DEPS_DATAKIT_PATH "$ENV{HOME}/repos/datakit")
#   set(DEPS_MBEDTLS_PATH "/path/to/local/mbedtls")

# datakit - Data structures and utilities
# Note: GIT_SHALLOW and GIT_PROGRESS inherit from global defaults
deps_add(datakit
    GIT https://github.com/mattsta/datakit.git
    TAG main
    TARGETS datakit-static datakit-library
    OPTIONS
        BuildTestBinary=OFF
        BUILD_TESTING=OFF
        CMAKE_COMPILE_WARNING_AS_ERROR=OFF
    EXCLUDE_FROM_ALL
)

# mbedtls - TLS/SSL library
# Note: GIT_SHALLOW, GIT_PROGRESS, and PATCH_CMAKE_VERSION inherit from global defaults
# Note: mbedtls 4.x renamed mbedcrypto to tfpsacrypto
deps_add(mbedtls
    GIT https://github.com/Mbed-TLS/mbedtls.git
    TAG v4.0.0
    TARGETS mbedtls mbedx509 tfpsacrypto
    OPTIONS
        ENABLE_PROGRAMS=OFF
        ENABLE_TESTING=OFF
        MBEDTLS_FATAL_WARNINGS=OFF
        CMAKE_DISABLE_FIND_PACKAGE_Doxygen=TRUE
        CMAKE_POSITION_INDEPENDENT_CODE=ON
    EXCLUDE_FROM_ALL
)

# rax - Radix tree (optional - only if present locally)
# Note: rax is optional and should be manually placed in deps/rax if needed
# It will not be auto-fetched to maintain the optional nature of pub/sub features
