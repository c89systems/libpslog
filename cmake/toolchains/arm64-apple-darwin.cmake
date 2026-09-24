set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR arm64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

if(DEFINED ENV{OSXCROSS_ROOT} AND NOT "$ENV{OSXCROSS_ROOT}" STREQUAL "")
    set(PSLOG_OSXCROSS_ROOT "$ENV{OSXCROSS_ROOT}")
elseif(DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "")
    set(PSLOG_OSXCROSS_ROOT "$ENV{HOME}/.local/cross/osxcross")
else()
    message(FATAL_ERROR "OSXCROSS_ROOT is not set and HOME is unavailable")
endif()

set(PSLOG_OSXCROSS_BIN_DIR "${PSLOG_OSXCROSS_ROOT}/bin")
execute_process(
    COMMAND "${CMAKE_CURRENT_LIST_DIR}/../../scripts/cpkt-toolchains.sh"
        discover arm64-apple-darwin
    RESULT_VARIABLE _pslog_osxcross_result
    OUTPUT_VARIABLE _pslog_osxcross_discovery
    ERROR_VARIABLE _pslog_osxcross_error)
if(NOT _pslog_osxcross_result EQUAL 0 OR
   NOT _pslog_osxcross_discovery MATCHES "(^|\n)status=ready(\n|$)")
    message(FATAL_ERROR
        "failed to resolve a complete arm64 Apple Darwin 25.x osxcross "
        "toolchain under ${PSLOG_OSXCROSS_ROOT}. Set CPKT_OSXCROSS_HOST "
        "to pin an installed prefix. ${_pslog_osxcross_error}")
endif()
string(REGEX MATCH "(^|\n)prefix=([^\n]+)" _pslog_osxcross_prefix
    "${_pslog_osxcross_discovery}")
set(PSLOG_OSXCROSS_HOST "${CMAKE_MATCH_2}")
set(PSLOG_OSXCROSS_HOST "${PSLOG_OSXCROSS_HOST}" CACHE STRING
    "osxcross target host triple" FORCE)
set(PSLOG_MACOS_DEPLOYMENT_TARGET "15.0" CACHE STRING
    "Minimum macOS deployment target")
set(CMAKE_OSX_DEPLOYMENT_TARGET "${PSLOG_MACOS_DEPLOYMENT_TARGET}"
    CACHE STRING "" FORCE)

set(ENV{PATH} "${PSLOG_OSXCROSS_BIN_DIR}:$ENV{PATH}")
set(CMAKE_C_COMPILER
    "${PSLOG_OSXCROSS_BIN_DIR}/${PSLOG_OSXCROSS_HOST}-clang"
    CACHE FILEPATH "" FORCE)
set(CMAKE_AR
    "${PSLOG_OSXCROSS_BIN_DIR}/${PSLOG_OSXCROSS_HOST}-ar"
    CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB
    "${PSLOG_OSXCROSS_BIN_DIR}/${PSLOG_OSXCROSS_HOST}-ranlib"
    CACHE FILEPATH "" FORCE)
set(CMAKE_LINKER
    "${PSLOG_OSXCROSS_BIN_DIR}/${PSLOG_OSXCROSS_HOST}-ld"
    CACHE FILEPATH "" FORCE)
set(CMAKE_INSTALL_NAME_TOOL
    "${PSLOG_OSXCROSS_BIN_DIR}/${PSLOG_OSXCROSS_HOST}-install_name_tool"
    CACHE FILEPATH "" FORCE)
set(CMAKE_STRIP
    "${PSLOG_OSXCROSS_BIN_DIR}/${PSLOG_OSXCROSS_HOST}-strip"
    CACHE FILEPATH "" FORCE)
set(CPKT_OTOOL
    "${PSLOG_OSXCROSS_BIN_DIR}/${PSLOG_OSXCROSS_HOST}-otool"
    CACHE FILEPATH "" FORCE)

foreach(_pslog_required_tool
        CMAKE_C_COMPILER
        CMAKE_AR
        CMAKE_RANLIB
        CMAKE_LINKER
        CMAKE_INSTALL_NAME_TOOL
        CMAKE_STRIP
        CPKT_OTOOL)
    if(NOT EXISTS "${${_pslog_required_tool}}")
        message(FATAL_ERROR
            "The arm64 Apple Darwin osxcross toolchain is missing "
            "${_pslog_required_tool}: ${${_pslog_required_tool}}. "
            "Set OSXCROSS_ROOT or install osxcross under "
            "$HOME/.local/cross/osxcross.")
    endif()
endforeach()

set(_pslog_darwin_linker_flag "--ld-path=${CMAKE_LINKER}")
foreach(_pslog_linker_flags
        CMAKE_EXE_LINKER_FLAGS
        CMAKE_SHARED_LINKER_FLAGS
        CMAKE_MODULE_LINKER_FLAGS)
    string(REGEX REPLACE "(^| )--ld-path=[^ ]+" " " _pslog_clean_linker_flags
        "${${_pslog_linker_flags}}")
    string(REGEX REPLACE "(^| )-fuse-ld=[^ ]+" " " _pslog_clean_linker_flags
        "${_pslog_clean_linker_flags}")
    string(STRIP "${_pslog_clean_linker_flags}" _pslog_clean_linker_flags)
    set(${_pslog_linker_flags}
        "${_pslog_darwin_linker_flag} ${_pslog_clean_linker_flags}"
        CACHE STRING "" FORCE)
endforeach()

file(GLOB _pslog_osxcross_sdks LIST_DIRECTORIES true
     "${PSLOG_OSXCROSS_ROOT}/SDK/MacOSX*.sdk")
if(NOT _pslog_osxcross_sdks)
    message(FATAL_ERROR
        "failed to locate a usable osxcross macOS SDK under "
        "${PSLOG_OSXCROSS_ROOT}/SDK")
endif()
list(SORT _pslog_osxcross_sdks)
list(REVERSE _pslog_osxcross_sdks)
list(GET _pslog_osxcross_sdks 0 PSLOG_OSXCROSS_SDK)
if(NOT EXISTS "${PSLOG_OSXCROSS_SDK}/usr/include")
    message(FATAL_ERROR
        "failed to locate a usable osxcross macOS SDK under "
        "${PSLOG_OSXCROSS_ROOT}/SDK")
endif()

set(CMAKE_OSX_SYSROOT "${PSLOG_OSXCROSS_SDK}" CACHE PATH "" FORCE)
set(CMAKE_FIND_ROOT_PATH "${PSLOG_OSXCROSS_SDK}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(PSLOG_TARGET_ARCH arm64 CACHE STRING "" FORCE)
set(PSLOG_TARGET_OS darwin CACHE STRING "" FORCE)
set(PSLOG_TARGET_LIBC "" CACHE STRING "" FORCE)
