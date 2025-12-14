# DepsManager.cmake - Universal CMake Dependency Manager
# ======================================================
#
# A generalized system for managing CMake dependencies with support for:
#   - Edit Mode: Use local checkouts for active development
#   - Remote Mode: Fetch from git repositories
#   - Vendored Mode: Use bundled sources for offline builds
#
# USAGE:
#   1. Copy this directory (cmake/deps-manager/) to your project
#   2. Create a deps.cmake configuration file
#   3. Include and use in your CMakeLists.txt:
#
#      include(cmake/deps-manager/DepsManager.cmake)
#      deps_init()
#      include(deps/deps.cmake)  # Your dependency declarations
#      deps_resolve()            # Fetch/resolve all dependencies
#
# CONFIGURATION:
#   deps/deps.cmake      - Dependency declarations (committed)
#   deps/.local.cmake    - Local overrides (gitignored)
#   deps/<name>/         - Local checkouts or symlinks
#
# ENVIRONMENT VARIABLES:
#   DEPS_MODE            - Global mode: "auto", "local", "remote" (default: auto)
#   DEPS_LOCAL_ROOT      - Root directory for local checkouts (default: ~/repos)
#   DEPS_<NAME>_PATH     - Per-dependency local path override
#
# CMAKE VARIABLES:
#   DEPS_DEFAULT_MODE    - Default mode for all dependencies
#   DEPS_VENDOR_DIRS     - List of directories to check for vendored deps
#
# ======================================================

include_guard(GLOBAL)
include(FetchContent)

# Internal state
set(_DEPS_INITIALIZED FALSE)
set(_DEPS_LIST "" CACHE INTERNAL "List of declared dependencies")
set(_DEPS_RESOLVED FALSE)

# Default configuration
set(DEPS_DEFAULT_MODE "auto" CACHE STRING "Default dependency mode: auto, local, remote")
set(DEPS_LOCAL_ROOT "$ENV{HOME}/repos" CACHE PATH "Default root for local checkouts")
set(DEPS_DIR "${CMAKE_SOURCE_DIR}/deps" CACHE PATH "Dependencies directory")
set(DEPS_VENDOR_DIRS "vendor;external;third_party;deps" CACHE STRING "Directories to check for vendored deps")
set(DEPS_CMAKE_MIN_VERSION "" CACHE STRING "Global cmake_minimum_required version to patch (empty = no patching)")
set(DEPS_DEFAULT_GIT_SHALLOW ON CACHE BOOL "Default shallow clone setting for dependencies")
set(DEPS_DEFAULT_GIT_PROGRESS ON CACHE BOOL "Default git progress display setting")
set(DEPS_SYMLINK_DIR "${CMAKE_SOURCE_DIR}/deps" CACHE PATH "Directory for dependency symlinks")

# ============================================================================
# PUBLIC API
# ============================================================================

#[[
deps_init()

Initialize the dependency manager. Call this once before declaring dependencies.

Options:
  QUIET                - Suppress status messages
  LOCAL_ROOT path      - Override default local checkout root
  VENDOR_DIRS dirs     - Override vendor directory search list
  CMAKE_MIN_VERSION v  - Patch all deps to this cmake_minimum_required (empty = no patching)
  DEFAULT_GIT_SHALLOW  - Enable shallow clones by default (ON/OFF)
  DEFAULT_GIT_PROGRESS - Show git progress by default (ON/OFF)
  SYMLINK_DIR path     - Directory for dependency symlinks (default: ${CMAKE_SOURCE_DIR}/deps)
]]
function(deps_init)
    set(options QUIET DEFAULT_GIT_SHALLOW DEFAULT_GIT_PROGRESS)
    set(oneValueArgs LOCAL_ROOT CMAKE_MIN_VERSION SYMLINK_DIR)
    set(multiValueArgs VENDOR_DIRS)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(ARG_LOCAL_ROOT)
        set(DEPS_LOCAL_ROOT "${ARG_LOCAL_ROOT}" CACHE PATH "" FORCE)
    endif()

    if(ARG_VENDOR_DIRS)
        set(DEPS_VENDOR_DIRS "${ARG_VENDOR_DIRS}" CACHE STRING "" FORCE)
    endif()

    if(DEFINED ARG_CMAKE_MIN_VERSION)
        set(DEPS_CMAKE_MIN_VERSION "${ARG_CMAKE_MIN_VERSION}" CACHE STRING "" FORCE)
    endif()

    if(DEFINED ARG_DEFAULT_GIT_SHALLOW)
        set(DEPS_DEFAULT_GIT_SHALLOW ${ARG_DEFAULT_GIT_SHALLOW} CACHE BOOL "" FORCE)
    endif()

    if(DEFINED ARG_DEFAULT_GIT_PROGRESS)
        set(DEPS_DEFAULT_GIT_PROGRESS ${ARG_DEFAULT_GIT_PROGRESS} CACHE BOOL "" FORCE)
    endif()

    if(ARG_SYMLINK_DIR)
        set(DEPS_SYMLINK_DIR "${ARG_SYMLINK_DIR}" CACHE PATH "" FORCE)
    endif()

    # Load local overrides if they exist
    if(EXISTS "${DEPS_DIR}/.local.cmake")
        if(NOT ARG_QUIET)
            message(STATUS "[DepsManager] Loading local overrides from ${DEPS_DIR}/.local.cmake")
        endif()
        include("${DEPS_DIR}/.local.cmake")
    endif()

    # Check for environment variable overrides
    if(DEFINED ENV{DEPS_MODE})
        set(DEPS_DEFAULT_MODE "$ENV{DEPS_MODE}" CACHE STRING "" FORCE)
    endif()

    if(DEFINED ENV{DEPS_LOCAL_ROOT})
        set(DEPS_LOCAL_ROOT "$ENV{DEPS_LOCAL_ROOT}" CACHE PATH "" FORCE)
    endif()

    set(_DEPS_INITIALIZED TRUE PARENT_SCOPE)

    if(NOT ARG_QUIET)
        message(STATUS "[DepsManager] Initialized (mode=${DEPS_DEFAULT_MODE}, local_root=${DEPS_LOCAL_ROOT})")
    endif()
endfunction()

#[[
deps_add(name ...)

Declare a dependency.

Required:
  name              - Dependency name (used for targets, directories, variables)

Options:
  GIT url           - Git repository URL
  TAG tag           - Git tag, branch, or commit (default: main)
  TARGETS t1 t2     - CMake targets this dependency provides
  OPTIONS k=v       - CMake options to set before including
  COMPONENTS c1     - Components for find_package (if using system install)
  SUBDIRECTORY d    - Subdirectory within repo containing CMakeLists.txt
  MODE mode         - Override mode for this dep: auto, local, remote, vendored
  LOCAL_PATH p      - Explicit local path (overrides auto-detection)
  EXCLUDE_FROM_ALL  - Don't include in ALL target
  GIT_SHALLOW       - Enable shallow clone (overrides default)
  GIT_PROGRESS      - Show git progress (overrides default)
  PATCH_CMAKE_VERSION v - Patch cmake_minimum_required to version (overrides global)
  CREATE_SYMLINK    - Create symlink in SYMLINK_DIR (default: ON)

Example:
  deps_add(varint
      GIT https://github.com/mattsta/varint.git
      TAG v1.0.0
      TARGETS varint-static varintDimension-static
      GIT_SHALLOW
      PATCH_CMAKE_VERSION 3.16
  )
]]
function(deps_add name)
    set(options EXCLUDE_FROM_ALL GIT_SHALLOW GIT_PROGRESS GIT_SUBMODULES_DISABLE CREATE_SYMLINK)
    set(oneValueArgs GIT TAG MODE LOCAL_PATH SUBDIRECTORY GIT_SUBMODULES_SHALLOW PATCH_CMAKE_VERSION)
    set(multiValueArgs TARGETS OPTIONS COMPONENTS GIT_SUBMODULES)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    # Store dependency info in cache variables
    string(TOUPPER "${name}" NAME_UPPER)

    # Apply defaults for git options if not explicitly set
    # Note: cmake_parse_arguments options are ALWAYS defined as TRUE/FALSE
    if(ARG_GIT_SHALLOW)
        set(effective_git_shallow TRUE)
    else()
        set(effective_git_shallow ${DEPS_DEFAULT_GIT_SHALLOW})
    endif()

    if(ARG_GIT_PROGRESS)
        set(effective_git_progress TRUE)
    else()
        set(effective_git_progress ${DEPS_DEFAULT_GIT_PROGRESS})
    endif()

    # Determine CMAKE version to patch to
    if(DEFINED ARG_PATCH_CMAKE_VERSION)
        set(effective_cmake_patch "${ARG_PATCH_CMAKE_VERSION}")
    else()
        set(effective_cmake_patch "${DEPS_CMAKE_MIN_VERSION}")
    endif()

    # CREATE_SYMLINK defaults to ON if not specified
    # Note: cmake_parse_arguments options are ALWAYS defined as TRUE/FALSE
    # Since CREATE_SYMLINK is an option, if not specified it's FALSE
    # We want default to be ON, so we invert the logic: if NOT specified (FALSE), use ON
    if(ARG_CREATE_SYMLINK)
        set(effective_create_symlink TRUE)
    else()
        # Not specified, use ON as default
        set(effective_create_symlink ON)
    endif()

    set(_DEP_${NAME_UPPER}_GIT "${ARG_GIT}" CACHE INTERNAL "")
    set(_DEP_${NAME_UPPER}_TAG "${ARG_TAG}" CACHE INTERNAL "")
    set(_DEP_${NAME_UPPER}_TARGETS "${ARG_TARGETS}" CACHE INTERNAL "")
    set(_DEP_${NAME_UPPER}_OPTIONS "${ARG_OPTIONS}" CACHE INTERNAL "")
    set(_DEP_${NAME_UPPER}_COMPONENTS "${ARG_COMPONENTS}" CACHE INTERNAL "")
    set(_DEP_${NAME_UPPER}_SUBDIRECTORY "${ARG_SUBDIRECTORY}" CACHE INTERNAL "")
    set(_DEP_${NAME_UPPER}_MODE "${ARG_MODE}" CACHE INTERNAL "")
    set(_DEP_${NAME_UPPER}_LOCAL_PATH "${ARG_LOCAL_PATH}" CACHE INTERNAL "")
    set(_DEP_${NAME_UPPER}_EXCLUDE_FROM_ALL "${ARG_EXCLUDE_FROM_ALL}" CACHE INTERNAL "")
    set(_DEP_${NAME_UPPER}_GIT_SHALLOW "${effective_git_shallow}" CACHE INTERNAL "")
    set(_DEP_${NAME_UPPER}_GIT_PROGRESS "${effective_git_progress}" CACHE INTERNAL "")
    set(_DEP_${NAME_UPPER}_GIT_SUBMODULES "${ARG_GIT_SUBMODULES}" CACHE INTERNAL "")
    set(_DEP_${NAME_UPPER}_GIT_SUBMODULES_DISABLE "${ARG_GIT_SUBMODULES_DISABLE}" CACHE INTERNAL "")
    set(_DEP_${NAME_UPPER}_GIT_SUBMODULES_SHALLOW "${ARG_GIT_SUBMODULES_SHALLOW}" CACHE INTERNAL "")
    set(_DEP_${NAME_UPPER}_PATCH_CMAKE_VERSION "${effective_cmake_patch}" CACHE INTERNAL "")
    set(_DEP_${NAME_UPPER}_CREATE_SYMLINK "${effective_create_symlink}" CACHE INTERNAL "")

    # Default tag to main if not specified
    if(NOT ARG_TAG)
        set(_DEP_${NAME_UPPER}_TAG "main" CACHE INTERNAL "" FORCE)
    endif()

    # Add to dependency list
    list(APPEND _DEPS_LIST "${name}")
    list(REMOVE_DUPLICATES _DEPS_LIST)
    set(_DEPS_LIST "${_DEPS_LIST}" CACHE INTERNAL "")

    message(STATUS "[DepsManager] Declared: ${name}")
endfunction()

#[[
deps_resolve()

Resolve all declared dependencies. Call this after all deps_add() calls.

Options:
  QUIET          - Suppress detailed status messages
]]
function(deps_resolve)
    set(options QUIET)
    cmake_parse_arguments(ARG "${options}" "" "" ${ARGN})

    if(NOT _DEPS_INITIALIZED)
        message(FATAL_ERROR "[DepsManager] Not initialized. Call deps_init() first.")
    endif()

    if(_DEPS_RESOLVED)
        message(WARNING "[DepsManager] Dependencies already resolved. Skipping.")
        return()
    endif()

    if(NOT ARG_QUIET)
        message(STATUS "")
        message(STATUS "[DepsManager] Resolving ${CMAKE_CURRENT_LIST_LENGTH} dependencies...")
        message(STATUS "")
    endif()

    foreach(dep IN LISTS _DEPS_LIST)
        _deps_resolve_one("${dep}" ${ARG_QUIET})

        # Propagate <name>_SOURCE_DIR to parent scope
        string(TOLOWER "${dep}" dep_lower)
        if(${dep_lower}_SOURCE_DIR)
            set(${dep}_SOURCE_DIR "${${dep_lower}_SOURCE_DIR}" PARENT_SCOPE)
        endif()
    endforeach()

    set(_DEPS_RESOLVED TRUE PARENT_SCOPE)

    if(NOT ARG_QUIET)
        message(STATUS "")
        message(STATUS "[DepsManager] All dependencies resolved.")
        deps_status()
    endif()
endfunction()

#[[
deps_status()

Print status of all dependencies.
]]
function(deps_status)
    message(STATUS "")
    message(STATUS "╔══════════════════════════════════════════════════════════════════╗")
    message(STATUS "║                    Dependency Status                             ║")
    message(STATUS "╠══════════════════════════════════════════════════════════════════╣")

    foreach(dep IN LISTS _DEPS_LIST)
        string(TOUPPER "${dep}" DEP_UPPER)
        set(mode "${_DEP_${DEP_UPPER}_RESOLVED_MODE}")
        set(source "${_DEP_${DEP_UPPER}_RESOLVED_SOURCE}")

        # Truncate source path for display
        string(LENGTH "${source}" source_len)
        if(source_len GREATER 50)
            string(SUBSTRING "${source}" 0 47 source)
            set(source "${source}...")
        endif()

        # Format mode with color indicator
        if(mode STREQUAL "LOCAL")
            set(mode_str "LOCAL ")
        elseif(mode STREQUAL "VENDORED")
            set(mode_str "VENDOR")
        else()
            set(mode_str "REMOTE")
        endif()

        message(STATUS "║  ${dep}: ${mode_str} -> ${source}")
    endforeach()

    message(STATUS "╚══════════════════════════════════════════════════════════════════╝")
    message(STATUS "")
endfunction()

#[[
deps_lock()

Generate a lock file with exact versions of all dependencies.
]]
function(deps_lock)
    set(lock_file "${DEPS_DIR}/deps.lock")

    file(WRITE "${lock_file}" "# Dependency Lock File\n")
    file(APPEND "${lock_file}" "# Generated: ${CMAKE_CURRENT_LIST_FILE}\n")
    file(APPEND "${lock_file}" "# Timestamp: ")
    string(TIMESTAMP ts "%Y-%m-%d %H:%M:%S")
    file(APPEND "${lock_file}" "${ts}\n")
    file(APPEND "${lock_file}" "#\n")
    file(APPEND "${lock_file}" "# To use: include this file AFTER deps.cmake\n")
    file(APPEND "${lock_file}" "#\n\n")

    foreach(dep IN LISTS _DEPS_LIST)
        string(TOUPPER "${dep}" DEP_UPPER)
        set(mode "${_DEP_${DEP_UPPER}_RESOLVED_MODE}")
        set(source "${_DEP_${DEP_UPPER}_RESOLVED_SOURCE}")
        set(tag "${_DEP_${DEP_UPPER}_TAG}")

        file(APPEND "${lock_file}" "# ${dep}\n")
        file(APPEND "${lock_file}" "set(_DEP_${DEP_UPPER}_LOCKED_MODE \"${mode}\")\n")
        file(APPEND "${lock_file}" "set(_DEP_${DEP_UPPER}_LOCKED_SOURCE \"${source}\")\n")

        # Try to get git commit if it's a git repo
        if(EXISTS "${source}/.git" OR EXISTS "${source}/../.git")
            execute_process(
                COMMAND git rev-parse HEAD
                WORKING_DIRECTORY "${source}"
                OUTPUT_VARIABLE git_hash
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
            )
            if(git_hash)
                file(APPEND "${lock_file}" "set(_DEP_${DEP_UPPER}_LOCKED_COMMIT \"${git_hash}\")\n")
            endif()
        endif()

        file(APPEND "${lock_file}" "\n")
    endforeach()

    message(STATUS "[DepsManager] Lock file written to: ${lock_file}")
endfunction()

# ============================================================================
# INTERNAL FUNCTIONS
# ============================================================================

# Resolve a single dependency
function(_deps_resolve_one name quiet)
    string(TOUPPER "${name}" NAME_UPPER)

    # Get stored config
    set(git_url "${_DEP_${NAME_UPPER}_GIT}")
    set(git_tag "${_DEP_${NAME_UPPER}_TAG}")
    set(dep_mode "${_DEP_${NAME_UPPER}_MODE}")
    set(local_path "${_DEP_${NAME_UPPER}_LOCAL_PATH}")
    set(subdirectory "${_DEP_${NAME_UPPER}_SUBDIRECTORY}")
    set(options "${_DEP_${NAME_UPPER}_OPTIONS}")
    set(exclude_from_all "${_DEP_${NAME_UPPER}_EXCLUDE_FROM_ALL}")
    set(git_shallow "${_DEP_${NAME_UPPER}_GIT_SHALLOW}")
    set(git_progress "${_DEP_${NAME_UPPER}_GIT_PROGRESS}")
    set(git_submodules "${_DEP_${NAME_UPPER}_GIT_SUBMODULES}")
    set(git_submodules_disable "${_DEP_${NAME_UPPER}_GIT_SUBMODULES_DISABLE}")
    set(git_submodules_shallow "${_DEP_${NAME_UPPER}_GIT_SUBMODULES_SHALLOW}")
    set(patch_cmake_version "${_DEP_${NAME_UPPER}_PATCH_CMAKE_VERSION}")

    # Determine effective mode
    if(NOT dep_mode)
        set(dep_mode "${DEPS_DEFAULT_MODE}")
    endif()

    # Check for environment variable override: DEPS_<NAME>_PATH
    if(DEFINED ENV{DEPS_${NAME_UPPER}_PATH})
        set(local_path "$ENV{DEPS_${NAME_UPPER}_PATH}")
        set(dep_mode "local")
    endif()

    # Check for CMake variable override
    if(DEPS_${NAME_UPPER}_PATH)
        set(local_path "${DEPS_${NAME_UPPER}_PATH}")
        set(dep_mode "local")
    endif()

    # Check for FetchContent override
    if(FETCHCONTENT_SOURCE_DIR_${NAME_UPPER})
        set(local_path "${FETCHCONTENT_SOURCE_DIR_${NAME_UPPER}}")
        set(dep_mode "local")
    endif()

    # Resolve mode to actual source
    set(resolved_mode "")
    set(resolved_source "")

    if(dep_mode STREQUAL "local" OR dep_mode STREQUAL "auto")
        # Try explicit local path
        if(local_path AND EXISTS "${local_path}/CMakeLists.txt")
            set(resolved_mode "LOCAL")
            set(resolved_source "${local_path}")
        endif()

        # Try deps directory
        if(NOT resolved_mode AND EXISTS "${DEPS_DIR}/${name}/CMakeLists.txt")
            set(resolved_mode "LOCAL")
            set(resolved_source "${DEPS_DIR}/${name}")
        endif()

        # Try default local root
        if(NOT resolved_mode AND EXISTS "${DEPS_LOCAL_ROOT}/${name}/CMakeLists.txt")
            set(resolved_mode "LOCAL")
            set(resolved_source "${DEPS_LOCAL_ROOT}/${name}")
        endif()

        # Try vendor directories
        if(NOT resolved_mode)
            foreach(vendor_dir IN LISTS DEPS_VENDOR_DIRS)
                if(EXISTS "${CMAKE_SOURCE_DIR}/${vendor_dir}/${name}/CMakeLists.txt")
                    set(resolved_mode "VENDORED")
                    set(resolved_source "${CMAKE_SOURCE_DIR}/${vendor_dir}/${name}")
                    break()
                endif()
            endforeach()
        endif()
    endif()

    # Fall back to remote if auto mode and nothing local found
    if(NOT resolved_mode AND (dep_mode STREQUAL "remote" OR dep_mode STREQUAL "auto"))
        if(git_url)
            set(resolved_mode "REMOTE")
            set(resolved_source "${git_url}@${git_tag}")
        else()
            message(FATAL_ERROR "[DepsManager] ${name}: No local source found and no GIT url specified")
        endif()
    endif()

    if(NOT resolved_mode)
        message(FATAL_ERROR "[DepsManager] ${name}: Could not resolve dependency (mode=${dep_mode})")
    endif()

    # Apply options before including
    foreach(opt IN LISTS options)
        string(REPLACE "=" ";" opt_parts "${opt}")
        list(GET opt_parts 0 opt_name)
        list(GET opt_parts 1 opt_value)
        # Use STRING type for all options to handle various CMake variable types
        set(${opt_name} ${opt_value} CACHE STRING "" FORCE)
        if(NOT quiet)
            message(STATUS "[DepsManager] ${name}: Setting ${opt_name}=${opt_value}")
        endif()
    endforeach()

    # Declare the dependency
    if(resolved_mode STREQUAL "LOCAL" OR resolved_mode STREQUAL "VENDORED")
        set(source_dir "${resolved_source}")
        if(subdirectory)
            set(source_dir "${resolved_source}/${subdirectory}")
        endif()

        if(NOT quiet)
            message(STATUS "[DepsManager] ${name}: ${resolved_mode} -> ${source_dir}")
        endif()

        FetchContent_Declare(${name} SOURCE_DIR "${source_dir}")
    else()
        if(NOT quiet)
            message(STATUS "[DepsManager] ${name}: REMOTE -> ${git_url} @ ${git_tag}")
            message(STATUS "[DepsManager] ${name}: shallow='${git_shallow}' progress='${git_progress}' patch_cmake='${patch_cmake_version}'")
        endif()

        # For shallow clones, use custom DOWNLOAD_COMMAND to avoid CMake's broken --no-single-branch behavior
        if(git_shallow)
            message(STATUS "[DepsManager] ${name}: Using shallow clone (--depth 1 --single-branch)")

            # Get source directory where FetchContent will place the code
            string(TOLOWER "${name}" name_lower)
            set(source_dir "${CMAKE_BINARY_DIR}/_deps/${name_lower}-src")

            # Create custom download script
            set(download_script "${CMAKE_BINARY_DIR}/_deps/${name_lower}-download.cmake")
            file(WRITE "${download_script}" "
# Custom shallow clone script for ${name}
# This bypasses CMake's FetchContent GIT_SHALLOW which adds --no-single-branch

# Clone if not already present
if(NOT EXISTS \"${source_dir}/.git\" AND NOT EXISTS \"${source_dir}/CMakeLists.txt\")
    message(STATUS \"Shallow cloning ${name} from ${git_url} @ ${git_tag}\")

    # Find git executable
    find_program(GIT_EXECUTABLE git REQUIRED)

    # Clone with true shallow clone (--single-branch --depth 1)
    execute_process(
        COMMAND \${GIT_EXECUTABLE} clone
            --depth 1
            --single-branch
            --branch ${git_tag}
            --no-tags
            ${git_url}
            \"${source_dir}\"
        RESULT_VARIABLE clone_result
        ERROR_VARIABLE clone_error
    )

    if(NOT clone_result EQUAL 0)
        message(FATAL_ERROR \"Failed to clone ${name}: \${clone_error}\")
    endif()

    # Initialize submodules with shallow clone
    execute_process(
        COMMAND \${GIT_EXECUTABLE} submodule update --init --recursive --depth 1 --recommend-shallow
        WORKING_DIRECTORY \"${source_dir}\"
        RESULT_VARIABLE submodule_result
        ERROR_VARIABLE submodule_error
    )

    if(NOT submodule_result EQUAL 0)
        message(WARNING \"Submodule init failed for ${name}: \${submodule_error}\")
    endif()
else()
    message(STATUS \"${name} already cloned, skipping clone step\")
endif()

# Patch CMakeLists.txt files if requested
set(patch_version \"${patch_cmake_version}\")
if(patch_version)
    file(GLOB_RECURSE cmake_files \"${source_dir}/CMakeLists.txt\")
    list(LENGTH cmake_files file_count)
    message(STATUS \"Patching \${file_count} CMakeLists.txt files to cmake_minimum_required(VERSION \${patch_version})\")
    foreach(cmake_file IN LISTS cmake_files)
        file(READ \"\${cmake_file}\" content)
        string(REGEX REPLACE \"cmake_minimum_required\\\\(VERSION [0-9]+\\\\.[0-9]+\\\\.?[0-9]*\" \"cmake_minimum_required(VERSION \${patch_version}\" new_content \"\${content}\")
        if(NOT content STREQUAL new_content)
            file(WRITE \"\${cmake_file}\" \"\${new_content}\")
            message(STATUS \"  Patched: \${cmake_file}\")
        endif()
    endforeach()
else()
    message(STATUS \"CMake version patching disabled\")
endif()

# mbedtls-specific: Disable doxygen generation that writes to source tree
if(\"${name}\" STREQUAL \"mbedtls\")
    set(tfpsa_cmake \"${source_dir}/tf-psa-crypto/CMakeLists.txt\")
    if(EXISTS \"\${tfpsa_cmake}\")
        file(READ \"\${tfpsa_cmake}\" tfpsa_content)
        # Check if not already patched
        if(tfpsa_content MATCHES \"doxygen/input/doc_mainpage.h\")
            # Simply empty the version_number_files list so foreach does nothing
            string(REPLACE \"set(version_number_files\\n        doxygen/input/doc_mainpage.h\\n        doxygen/tfpsacrypto.doxyfile)\" \"set(version_number_files) # Disabled for loopy build\" tfpsa_patched \"\${tfpsa_content}\")
            if(NOT tfpsa_content STREQUAL tfpsa_patched)
                file(WRITE \"\${tfpsa_cmake}\" \"\${tfpsa_patched}\")
                message(STATUS \"  Disabled doxygen generation in tf-psa-crypto/CMakeLists.txt\")
            endif()
        endif()
    endif()
endif()

message(STATUS \"${name} download complete\")
")

            if(subdirectory)
                FetchContent_Declare(${name}
                    DOWNLOAD_COMMAND ${CMAKE_COMMAND} -P "${download_script}"
                    SOURCE_DIR "${source_dir}"
                    SOURCE_SUBDIR "${subdirectory}"
                    UPDATE_DISCONNECTED TRUE
                )
            else()
                FetchContent_Declare(${name}
                    DOWNLOAD_COMMAND ${CMAKE_COMMAND} -P "${download_script}"
                    SOURCE_DIR "${source_dir}"
                    UPDATE_DISCONNECTED TRUE
                )
            endif()
        elseif(git_progress)
            message(STATUS "[DepsManager] ${name}: Using FetchContent with progress (full clone)")
            if(subdirectory)
                FetchContent_Declare(${name}
                    GIT_REPOSITORY "${git_url}"
                    GIT_TAG "${git_tag}"
                    GIT_PROGRESS TRUE
                    SOURCE_SUBDIR "${subdirectory}"
                    UPDATE_DISCONNECTED TRUE
                )
            else()
                FetchContent_Declare(${name}
                    GIT_REPOSITORY "${git_url}"
                    GIT_TAG "${git_tag}"
                    GIT_PROGRESS TRUE
                    UPDATE_DISCONNECTED TRUE
                )
            endif()
        else()
            message(STATUS "[DepsManager] ${name}: Using FetchContent (full clone, no progress)")
            if(subdirectory)
                FetchContent_Declare(${name}
                    GIT_REPOSITORY "${git_url}"
                    GIT_TAG "${git_tag}"
                    SOURCE_SUBDIR "${subdirectory}"
                    UPDATE_DISCONNECTED TRUE
                )
            else()
                FetchContent_Declare(${name}
                    GIT_REPOSITORY "${git_url}"
                    GIT_TAG "${git_tag}"
                    UPDATE_DISCONNECTED TRUE
                )
            endif()
        endif()

        set(resolved_source "${git_url}@${git_tag}")
    endif()

    # Make available
    if(exclude_from_all)
        FetchContent_MakeAvailable(${name})
        # Note: EXCLUDE_FROM_ALL requires CMake 3.28+
        # For older CMake, would need different approach
    else()
        FetchContent_MakeAvailable(${name})
    endif()

    # Ensure <name>_SOURCE_DIR is set and propagated to parent scope
    # FetchContent sets <lowercasename>_SOURCE_DIR, but we need to ensure it's available
    string(TOLOWER "${name}" name_lower)
    if(${name_lower}_SOURCE_DIR)
        set(${name}_SOURCE_DIR "${${name_lower}_SOURCE_DIR}" PARENT_SCOPE)
        if(NOT quiet)
            message(STATUS "[DepsManager] ${name}: SOURCE_DIR = ${${name_lower}_SOURCE_DIR}")
        endif()

        # Always create symlink for backward compatibility with relative includes
        set(symlink_path "${DEPS_SYMLINK_DIR}/${name}")
        if(NOT EXISTS "${symlink_path}")
            file(CREATE_LINK "${${name_lower}_SOURCE_DIR}" "${symlink_path}" SYMBOLIC)
            if(NOT quiet)
                message(STATUS "[DepsManager] Created symlink: ${symlink_path} -> ${${name_lower}_SOURCE_DIR}")
            endif()
        endif()
    endif()

    # Store resolved info
    set(_DEP_${NAME_UPPER}_RESOLVED_MODE "${resolved_mode}" CACHE INTERNAL "")
    set(_DEP_${NAME_UPPER}_RESOLVED_SOURCE "${resolved_source}" CACHE INTERNAL "")
endfunction()

# ============================================================================
# UTILITY FUNCTIONS
# ============================================================================

#[[
deps_get_mode(name output_var)

Get the resolved mode for a dependency.
]]
function(deps_get_mode name output_var)
    string(TOUPPER "${name}" NAME_UPPER)
    set(${output_var} "${_DEP_${NAME_UPPER}_RESOLVED_MODE}" PARENT_SCOPE)
endfunction()

#[[
deps_get_source(name output_var)

Get the resolved source path/url for a dependency.
]]
function(deps_get_source name output_var)
    string(TOUPPER "${name}" NAME_UPPER)
    set(${output_var} "${_DEP_${NAME_UPPER}_RESOLVED_SOURCE}" PARENT_SCOPE)
endfunction()

#[[
deps_is_local(name output_var)

Check if a dependency is using local sources.
]]
function(deps_is_local name output_var)
    string(TOUPPER "${name}" NAME_UPPER)
    set(mode "${_DEP_${NAME_UPPER}_RESOLVED_MODE}")
    if(mode STREQUAL "LOCAL" OR mode STREQUAL "VENDORED")
        set(${output_var} TRUE PARENT_SCOPE)
    else()
        set(${output_var} FALSE PARENT_SCOPE)
    endif()
endfunction()

#[[
deps_list(output_var)

Get list of all declared dependencies.
]]
function(deps_list output_var)
    set(${output_var} "${_DEPS_LIST}" PARENT_SCOPE)
endfunction()
