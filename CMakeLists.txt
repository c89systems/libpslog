cmake_minimum_required(VERSION 3.21)

include(cmake/pslog_version.cmake)

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND
   (NOT DEFINED CMAKE_TOOLCHAIN_FILE OR CMAKE_TOOLCHAIN_FILE STREQUAL ""))
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchains/host.cmake"
        CACHE FILEPATH "Native toolchain policy" FORCE)
endif()

project(libpslog VERSION ${PSLOG_PROJECT_VERSION_CORE} LANGUAGES C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

get_property(PSLOG_VERIFIED_C_COMPILER GLOBAL PROPERTY PSLOG_VERIFIED_C_COMPILER)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT PSLOG_ALLOW_HOST_COMPILER AND
   (NOT PSLOG_VERIFIED_C_COMPILER OR NOT CMAKE_C_COMPILER STREQUAL PSLOG_VERIFIED_C_COMPILER))
    message(FATAL_ERROR
        "Linux builds require a pinned Bootlin toolchain. Configure with a libpslog preset or "
        "-DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/host.cmake in a fresh build directory (remove stale compiler state).")
endif()

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

set(PSLOG_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
file(MAKE_DIRECTORY "${PSLOG_GENERATED_INCLUDE_DIR}")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/pslog_version.h.in"
    "${PSLOG_GENERATED_INCLUDE_DIR}/pslog_version.h"
    @ONLY
)
set(PSLOG_GENERATED_VERSION_HEADER "${PSLOG_GENERATED_INCLUDE_DIR}/pslog_version.h")
set(PSLOG_SINGLE_HEADER_BUILD "${PSLOG_GENERATED_INCLUDE_DIR}/pslog_single_header.h")
set(PSLOG_SINGLE_HEADER_ALIAS_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/single-header/include")
set(PSLOG_SINGLE_HEADER_BUILD_ALIAS "${PSLOG_SINGLE_HEADER_ALIAS_INCLUDE_DIR}/pslog.h")
set(PSLOG_SINGLE_HEADER_DIST "${CMAKE_CURRENT_SOURCE_DIR}/dist/pslog-${PSLOG_RESOLVED_VERSION}.h")
set(PSLOG_SINGLE_HEADER_DIST_GZ "${PSLOG_SINGLE_HEADER_DIST}.gz")

function(pslog_configure_link_warnings target)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_options(${target} PRIVATE -Wl,--fatal-warnings)
    elseif(APPLE)
        target_link_options(${target} PRIVATE -Wl,-fatal_warnings)
    endif()
endfunction()

function(pslog_configure_c_target target)
    pslog_configure_link_warnings(${target})
    if(UNIX OR APPLE OR CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
        target_compile_definitions(${target} PRIVATE _POSIX_C_SOURCE=200809L)
    endif()
    if(APPLE)
        target_compile_definitions(${target} PRIVATE _DARWIN_C_SOURCE=1)
    endif()
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE
            -std=c89
            -Wall
            -Wextra
            -Wpedantic
            -pedantic-errors
            -Werror
        )
    endif()
endfunction()

function(pslog_configure_project_warnings target)
    pslog_configure_link_warnings(${target})
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Werror
        )
    endif()
endfunction()

function(pslog_enable_coverage_if_requested target)
    get_target_property(target_type ${target} TYPE)
    if(PSLOG_ENABLE_COVERAGE AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_definitions(${target} PRIVATE PSLOG_COVERAGE_BUILD=1)
        target_compile_options(${target} PRIVATE --coverage -O0)
        if(NOT target_type STREQUAL "STATIC_LIBRARY")
            target_link_options(${target} PRIVATE --coverage)
        endif()
    endif()
endfunction()

include(cmake/pslog_local_runtime.cmake)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND PSLOG_NATIVE_EXECUTION AND NOT PSLOG_ALLOW_HOST_COMPILER)
    enable_testing()
    add_executable(pslog_runtime_probe tests/runtime_probe.c)
    target_compile_definitions(pslog_runtime_probe PRIVATE PSLOG_RUNTIME_ROOT="${PSLOG_BOOTLIN_ROOT}")
    pslog_configure_c_target(pslog_runtime_probe)
    pslog_configure_nonshipped_executable(pslog_runtime_probe)
    add_test(NAME runtime_mapping_and_exec_test COMMAND pslog_runtime_probe)
    set_tests_properties(runtime_mapping_and_exec_test PROPERTIES TIMEOUT 30 LABELS "local;runtime;target")
endif()

find_program(CLANG_FORMAT_BIN NAMES clang-format)
file(GLOB_RECURSE PSLOG_FORMAT_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/bench/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bench/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/bench/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/bench/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/examples/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/examples/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/examples/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/examples/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lua/src/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lua/src/*.h
)

if(CLANG_FORMAT_BIN)
    add_custom_target(clang-format
        COMMAND ${CLANG_FORMAT_BIN} -i ${PSLOG_FORMAT_SOURCES}
        VERBATIM
    )
    add_custom_target(format-check
        COMMAND ${CLANG_FORMAT_BIN} --dry-run --Werror ${PSLOG_FORMAT_SOURCES}
        VERBATIM
    )
else()
    add_custom_target(clang-format
        COMMAND ${CMAKE_COMMAND} -E echo "clang-format not found"
        COMMAND ${CMAKE_COMMAND} -E false
        VERBATIM
    )
    add_custom_target(format-check
        COMMAND ${CMAKE_COMMAND} -E echo "clang-format not found"
        COMMAND ${CMAKE_COMMAND} -E false
        VERBATIM
    )
endif()

add_custom_target(format DEPENDS clang-format)

option(PSLOG_BUILD_TESTS "Build unit tests" ON)
option(PSLOG_BUILD_BENCHMARKS "Build benchmarks" ON)
option(PSLOG_BUILD_FUZZ "Build fuzz targets" OFF)
option(PSLOG_BUILD_LUA "Enable the staged Lua facade CTest" OFF)
option(PSLOG_BENCHMARK_WITH_LIBLOGGER "Enable optional benchmark-only liblogger comparisons" OFF)
option(PSLOG_BENCHMARK_WITH_QUILL "Enable optional benchmark-only Quill comparisons" OFF)
option(PSLOG_ENABLE_COVERAGE "Build test targets with gcov coverage instrumentation" OFF)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/pslog_dependencies.cmake")
set(PSLOG_LUA_STAGE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/build/lua-host" CACHE PATH
    "Repository-local staged Lua 5.5 development root")
set(PSLOG_LUA_SDK_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/build/lua-sdk" CACHE PATH
    "Repository-local installed libpslog SDK prefix used by Lua checks")
set(PSLOG_LUA_STAGE_INCLUDE_DIR "${PSLOG_LUA_STAGE_ROOT}/include")
set(PSLOG_LUA_STAGE_LIBRARY "${PSLOG_LUA_STAGE_ROOT}/lib/liblua.a")
set(PSLOG_LUA_SDK_INCLUDE_DIR "${PSLOG_LUA_SDK_PREFIX}/include")
set(PSLOG_LUA_SDK_LIBRARY
    "${PSLOG_LUA_SDK_PREFIX}/lib/${CMAKE_SHARED_LIBRARY_PREFIX}pslog${CMAKE_SHARED_LIBRARY_SUFFIX}")
if(PSLOG_BUILD_LUA)
    foreach(required_lua_path IN ITEMS
            "${PSLOG_LUA_STAGE_INCLUDE_DIR}/lua.h"
            "${PSLOG_LUA_STAGE_LIBRARY}"
            "${PSLOG_LUA_SDK_INCLUDE_DIR}/pslog.h"
            "${PSLOG_LUA_SDK_LIBRARY}")
        if(NOT EXISTS "${required_lua_path}")
            message(FATAL_ERROR
                "staged Lua CTest dependency is missing: ${required_lua_path}. "
                "Run 'make lua-rock' before configuring the debug-lua preset.")
        endif()
    endforeach()
endif()

if(PSLOG_BUILD_TESTS OR PSLOG_BENCHMARK_WITH_LIBLOGGER OR PSLOG_BENCHMARK_WITH_QUILL)
    include(FetchContent)
endif()

if(PSLOG_BUILD_TESTS OR PSLOG_BENCHMARK_WITH_LIBLOGGER)
    set(JANSSON_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(JANSSON_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(JANSSON_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(JANSSON_WITHOUT_TESTS ON CACHE BOOL "" FORCE)
    set(JANSSON_INSTALL OFF CACHE BOOL "" FORCE)
    pslog_acquire_verified_archive(
        COMPONENT jansson URL https://github.com/akheron/jansson/releases/download/v2.15.0/jansson-2.15.0.tar.gz
        SHA256 070a629590723228dc3b744ae90e965a569efb9c535b3309b52e80e75d8eb3be
        OUTPUT jansson_archive)
    pslog_stage_dependency_archive(
        COMPONENT jansson ARCHIVE "${jansson_archive}"
        SHA256 070a629590723228dc3b744ae90e965a569efb9c535b3309b52e80e75d8eb3be
        OUTPUT jansson_source_dir)
    FetchContent_Declare(jansson SOURCE_DIR "${jansson_source_dir}")
    FetchContent_MakeAvailable(jansson)
    set_target_properties(jansson PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS ON
    )
endif()

set(PSLOG_TARGET_ARCH "${CMAKE_SYSTEM_PROCESSOR}" CACHE STRING "Artifact architecture name")
set(PSLOG_TARGET_OS "${CMAKE_SYSTEM_NAME}" CACHE STRING "Artifact operating system name")
set(PSLOG_TARGET_LIBC "" CACHE STRING "Artifact libc suffix for Linux targets (gnu, musl, or empty)")

string(TOLOWER "${PSLOG_TARGET_OS}" PSLOG_TARGET_OS_LOWER)
string(TOLOWER "${PSLOG_TARGET_ARCH}" PSLOG_TARGET_ARCH_LOWER)

if(PSLOG_TARGET_ARCH_LOWER STREQUAL "amd64")
    set(PSLOG_TARGET_ARCH_LOWER "x86_64")
endif()
if(PSLOG_TARGET_ARCH_LOWER STREQUAL "arm64")
    set(PSLOG_TARGET_ARCH_LOWER "aarch64")
endif()

if(PSLOG_TARGET_OS_LOWER STREQUAL "linux" AND NOT PSLOG_TARGET_LIBC)
    set(PSLOG_TARGET_LIBC "gnu")
endif()

if(PSLOG_TARGET_OS_LOWER STREQUAL "darwin" AND
   PSLOG_TARGET_ARCH_LOWER STREQUAL "aarch64")
    set(PSLOG_TARGET_ID "arm64-apple-darwin")
elseif(PSLOG_TARGET_OS_LOWER STREQUAL "darwin")
    set(PSLOG_TARGET_ID "${PSLOG_TARGET_ARCH_LOWER}-darwin")
elseif(PSLOG_TARGET_OS_LOWER STREQUAL "freebsd")
    set(PSLOG_TARGET_ID "${PSLOG_TARGET_ARCH_LOWER}-freebsd")
elseif(PSLOG_TARGET_OS_LOWER STREQUAL "linux")
    if(PSLOG_TARGET_LIBC)
        set(PSLOG_TARGET_ID "${PSLOG_TARGET_ARCH_LOWER}-linux-${PSLOG_TARGET_LIBC}")
    else()
        set(PSLOG_TARGET_ID "${PSLOG_TARGET_ARCH_LOWER}-linux")
    endif()
else()
    set(PSLOG_TARGET_ID "${PSLOG_TARGET_ARCH_LOWER}-${PSLOG_TARGET_OS_LOWER}")
endif()

set(PSLOG_SOURCES
    src/pslog.c
    src/pslog_palette.c
    src/pslog_emit_console.c
    src/pslog_emit_json.c
)
set(PSLOG_SINGLE_HEADER_SOURCE_PATHS "")
foreach(pslog_source IN LISTS PSLOG_SOURCES)
    list(APPEND PSLOG_SINGLE_HEADER_SOURCE_PATHS "${CMAKE_CURRENT_SOURCE_DIR}/${pslog_source}")
endforeach()
list(JOIN PSLOG_SINGLE_HEADER_SOURCE_PATHS "|" PSLOG_SINGLE_HEADER_SOURCE_PATHS_ARG)
list(JOIN CMAKE_CROSSCOMPILING_EMULATOR "|" PSLOG_CROSSCOMPILING_EMULATOR_ARG)

set(PSLOG_BENCH_SUPPORT_SOURCES
    bench/bench_fixture.c
    bench/bench_production_dataset.c
)

add_library(pslog_shared SHARED ${PSLOG_SOURCES})
add_library(pslog_static STATIC ${PSLOG_SOURCES})

foreach(target pslog_shared pslog_static)
    target_include_directories(${target}
        PUBLIC
            $<BUILD_INTERFACE:${PSLOG_GENERATED_INCLUDE_DIR}>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(${target} PRIVATE PSLOG_VERSION_STRING="${PSLOG_RESOLVED_VERSION}")
    set_target_properties(${target} PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        VISIBILITY_INLINES_HIDDEN NO
    )
    target_link_libraries(${target} PRIVATE Threads::Threads)
    pslog_configure_c_target(${target})
    pslog_enable_coverage_if_requested(${target})
endforeach()

# Keep an exact main-example public-consumer command in compile_commands.json
# for clangd. This target is excluded from normal builds because it is an
# editor validation surface, not a second example build path.
add_executable(pslog_clangd_public_consumer EXCLUDE_FROM_ALL examples/example.c)
target_link_libraries(pslog_clangd_public_consumer PRIVATE pslog_static Threads::Threads)
pslog_configure_c_target(pslog_clangd_public_consumer)
pslog_configure_nonshipped_executable(pslog_clangd_public_consumer)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(pslog_shared PRIVATE -fvisibility=hidden)
endif()

if(PSLOG_BUILD_TESTS)
    add_library(pslog_test_static STATIC ${PSLOG_SOURCES})
    target_include_directories(pslog_test_static
        PUBLIC
            $<BUILD_INTERFACE:${PSLOG_GENERATED_INCLUDE_DIR}>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(pslog_test_static PRIVATE
        PSLOG_VERSION_STRING="${PSLOG_RESOLVED_VERSION}"
        PSLOG_TEST_HOOKS=1
    )
    set_target_properties(pslog_test_static PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        VISIBILITY_INLINES_HIDDEN NO
        OUTPUT_NAME pslog_test
    )
    target_link_libraries(pslog_test_static PRIVATE Threads::Threads)
    pslog_configure_c_target(pslog_test_static)
    pslog_enable_coverage_if_requested(pslog_test_static)
endif()

set_target_properties(pslog_shared PROPERTIES
    OUTPUT_NAME pslog
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    INSTALL_NAME_DIR "@rpath"
    BUILD_WITH_INSTALL_NAME_DIR ON
)

set_target_properties(pslog_static PROPERTIES
    OUTPUT_NAME pslog
)

if(PSLOG_BUILD_TESTS)
    enable_testing()
    function(pslog_set_test_properties test_name test_labels test_timeout)
        set_tests_properties(${test_name} PROPERTIES
            LABELS "${test_labels}"
            TIMEOUT ${test_timeout}
        )
    endfunction()

    function(pslog_add_binary_test test_name target test_labels test_timeout)
        # Keep a target name here: CMake injects the toolchain's
        # CMAKE_CROSSCOMPILING_EMULATOR for QEMU targets. Native Bootlin
        # executables run directly from their own ELF metadata.
        add_test(NAME ${test_name} COMMAND ${target})
        pslog_set_test_properties(${test_name} "${test_labels}" ${test_timeout})
    endfunction()

    string(TOUPPER "${CMAKE_BUILD_TYPE}" PSLOG_TEST_BUILD_TYPE_UPPER)
    set(PSLOG_TEST_C_FLAGS "${CMAKE_C_FLAGS}")
    if(DEFINED CMAKE_C_FLAGS_${PSLOG_TEST_BUILD_TYPE_UPPER})
        string(APPEND PSLOG_TEST_C_FLAGS " ${CMAKE_C_FLAGS_${PSLOG_TEST_BUILD_TYPE_UPPER}}")
    endif()
    set(PSLOG_TEST_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
    if(DEFINED CMAKE_EXE_LINKER_FLAGS_${PSLOG_TEST_BUILD_TYPE_UPPER})
        string(APPEND PSLOG_TEST_EXE_LINKER_FLAGS " ${CMAKE_EXE_LINKER_FLAGS_${PSLOG_TEST_BUILD_TYPE_UPPER}}")
    endif()
    if(PSLOG_ENABLE_COVERAGE AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        string(APPEND PSLOG_TEST_EXE_LINKER_FLAGS " --coverage")
    endif()
    if(DEFINED PSLOG_NONSHIPPED_ELF_LINKER_FLAGS AND
       NOT PSLOG_NONSHIPPED_ELF_LINKER_FLAGS STREQUAL "")
        string(APPEND PSLOG_TEST_EXE_LINKER_FLAGS " ${PSLOG_NONSHIPPED_ELF_LINKER_FLAGS}")
    endif()
    add_executable(pslog_tests
        tests/test_main.c
        ${PSLOG_BENCH_SUPPORT_SOURCES}
    )
    target_include_directories(pslog_tests PRIVATE
        ${PSLOG_GENERATED_INCLUDE_DIR}
        include
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${jansson_BINARY_DIR}/include
    )
    target_compile_definitions(pslog_tests PRIVATE PSLOG_TEST_HOOKS=1)
    if(UNIX OR APPLE OR CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
        target_compile_definitions(pslog_tests PRIVATE _POSIX_C_SOURCE=200809L)
    endif()
    if(APPLE)
        target_compile_definitions(pslog_tests PRIVATE _DARWIN_C_SOURCE=1)
    endif()
    target_link_libraries(pslog_tests PRIVATE pslog_test_static jansson)
    pslog_configure_project_warnings(pslog_tests)
    pslog_enable_coverage_if_requested(pslog_tests)
    pslog_configure_nonshipped_executable(pslog_tests)
    pslog_add_binary_test(pslog_tests pslog_tests "unit;local" 30)

    # Keep the native Valgrind gate deliberately small.  The full unit binary
    # also exercises intentional termination children and large benchmark
    # fixtures, neither of which is part of the facade memory-check contract.
    add_executable(pslog_valgrind_facade_tests tests/valgrind_facade_test.c)
    target_include_directories(pslog_valgrind_facade_tests PRIVATE
        ${PSLOG_GENERATED_INCLUDE_DIR}
        include
    )
    target_link_libraries(pslog_valgrind_facade_tests PRIVATE pslog_static)
    pslog_configure_project_warnings(pslog_valgrind_facade_tests)
    pslog_enable_coverage_if_requested(pslog_valgrind_facade_tests)
    pslog_configure_nonshipped_executable(pslog_valgrind_facade_tests)
    pslog_add_binary_test(pslog_valgrind_facade_tests pslog_valgrind_facade_tests "unit;local;memory-check" 30)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND PSLOG_NATIVE_EXECUTION AND NOT PSLOG_ALLOW_HOST_COMPILER)
        add_test(NAME elf_release_gate_test
            COMMAND ${CMAKE_COMMAND}
                -DPSLOG_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
                -DPSLOG_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
                -DPSLOG_C_COMPILER=${CMAKE_C_COMPILER}
                -DPSLOG_READELF=${CMAKE_READELF}
                -DPSLOG_BOOTLIN_INTERPRETER=${PSLOG_BOOTLIN_ELF_INTERPRETER}
                -DPSLOG_BOOTLIN_RUNTIME_SEARCH_PATH=${PSLOG_BOOTLIN_RUNTIME_SEARCH_PATH}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/elf_release_gate_test.cmake)
        set_tests_properties(elf_release_gate_test PROPERTIES
            RUN_SERIAL TRUE LABELS "local;packaging;runtime" TIMEOUT 30)
        add_test(NAME bootlin_elf_runtime_test
            COMMAND ${CMAKE_COMMAND}
                -DPSLOG_BINARY=$<TARGET_FILE:pslog_valgrind_facade_tests>
                -DPSLOG_READELF=${CMAKE_READELF}
                -DPSLOG_BOOTLIN_INTERPRETER=${PSLOG_BOOTLIN_ELF_INTERPRETER}
                -DPSLOG_BOOTLIN_SYSROOT=${PSLOG_BOOTLIN_SYSROOT}
                -DPSLOG_BOOTLIN_RPATH=${PSLOG_BOOTLIN_RUNTIME_SEARCH_PATH}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bootlin_elf_runtime_test.cmake
        )
        set_tests_properties(bootlin_elf_runtime_test PROPERTIES
            RUN_SERIAL TRUE
            LABELS "local;smoke;runtime;target"
            TIMEOUT 30
        )
    endif()
    if(PSLOG_BUILD_LUA)
        add_executable(pslog_lua_interop_tests
            tests/lua_interop_embedder_test.c
            lua/src/pslog_lua.c
        )
        target_include_directories(pslog_lua_interop_tests PRIVATE
            ${PSLOG_LUA_STAGE_INCLUDE_DIR}
            ${PSLOG_LUA_SDK_INCLUDE_DIR}
            include
        )
        target_compile_definitions(pslog_lua_interop_tests PRIVATE
            PSLOG_VERSION_STRING="${PSLOG_RESOLVED_VERSION}"
            PSLOG_TEST_HOOKS=1
            _POSIX_C_SOURCE=200809L
        )
        target_link_libraries(pslog_lua_interop_tests PRIVATE
            pslog_test_static
            ${PSLOG_LUA_STAGE_LIBRARY}
            m
            ${CMAKE_DL_LIBS}
        )
        pslog_configure_project_warnings(pslog_lua_interop_tests)
        pslog_enable_coverage_if_requested(pslog_lua_interop_tests)
        pslog_configure_nonshipped_executable(pslog_lua_interop_tests)
        pslog_add_binary_test(pslog_lua_interop_tests pslog_lua_interop_tests "unit;local;lua" 30)
        set_tests_properties(pslog_lua_interop_tests PROPERTIES
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        )
        set_property(TEST pslog_lua_interop_tests APPEND PROPERTY LABELS target)
    endif()
    add_executable(pslog_single_header_tests
        tests/test_main_single_header.c
        ${PSLOG_BENCH_SUPPORT_SOURCES}
    )
    target_include_directories(pslog_single_header_tests PRIVATE
        ${PSLOG_SINGLE_HEADER_ALIAS_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${jansson_BINARY_DIR}/include
    )
    target_compile_definitions(pslog_single_header_tests PRIVATE PSLOG_TEST_HOOKS=1)
    if(UNIX OR APPLE OR CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
        target_compile_definitions(pslog_single_header_tests PRIVATE _POSIX_C_SOURCE=200809L)
    endif()
    if(APPLE)
        target_compile_definitions(pslog_single_header_tests PRIVATE _DARWIN_C_SOURCE=1)
    endif()
    target_link_libraries(pslog_single_header_tests PRIVATE Threads::Threads jansson)
    pslog_configure_project_warnings(pslog_single_header_tests)
    add_dependencies(pslog_single_header_tests package-single-header)
    pslog_enable_coverage_if_requested(pslog_single_header_tests)
    pslog_configure_nonshipped_executable(pslog_single_header_tests)
    pslog_add_binary_test(pslog_single_header_tests pslog_single_header_tests "unit;local;smoke" 30)
    add_test(NAME package_archives_test
        COMMAND ${CMAKE_COMMAND}
            -DPSLOG_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -DPSLOG_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
            -DPSLOG_VERSION=${PSLOG_RESOLVED_VERSION}
        -DPSLOG_TARGET_ID=${PSLOG_TARGET_ID}
        -DPSLOG_STATIC_LIB_NAME=$<TARGET_FILE_NAME:pslog_static>
        -DPSLOG_SHARED_LIB_NAME=$<TARGET_FILE_NAME:pslog_shared>
        -DPSLOG_SHARED_LINK_NAME=$<TARGET_LINKER_FILE_NAME:pslog_shared>
        -DPSLOG_SHARED_SONAME=$<TARGET_SONAME_FILE_NAME:pslog_shared>
        -DPSLOG_C_COMPILER=${CMAKE_C_COMPILER}
        -DPSLOG_ALLOW_HOST_COMPILER=${PSLOG_ALLOW_HOST_COMPILER}
        -DPSLOG_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
        -DPSLOG_CROSSCOMPILING=${CMAKE_CROSSCOMPILING}
        -DPSLOG_NATIVE_EXECUTION=${PSLOG_NATIVE_EXECUTION}
        -DPSLOG_CROSSCOMPILING_EMULATOR=${PSLOG_CROSSCOMPILING_EMULATOR_ARG}
        -DPSLOG_EXE_LINKER_FLAGS=${PSLOG_TEST_EXE_LINKER_FLAGS}
        -DPSLOG_BOOTLIN_INTERPRETER=${PSLOG_BOOTLIN_ELF_INTERPRETER}
        -DPSLOG_BOOTLIN_ROOT=${PSLOG_BOOTLIN_ROOT}
        -DPSLOG_READELF=${CMAKE_READELF}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/package_archives_test.cmake
    )
    set_tests_properties(package_archives_test PROPERTIES
        RUN_SERIAL TRUE
        LABELS "packaging;local"
        TIMEOUT 120
    )
    add_test(NAME version_resolution_test
        COMMAND ${CMAKE_COMMAND}
            -DPSLOG_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -DPSLOG_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/version_resolution_test.cmake
    )
    set_tests_properties(version_resolution_test PROPERTIES
        RUN_SERIAL TRUE
        LABELS "local;smoke"
        TIMEOUT 30
    )
    add_test(NAME release_privacy_gate_test
        COMMAND ${CMAKE_COMMAND}
            -DPSLOG_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -DPSLOG_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/release_privacy_gate_test.cmake
    )
    set_tests_properties(release_privacy_gate_test PROPERTIES
        RUN_SERIAL TRUE
        LABELS "packaging;local"
        TIMEOUT 60
    )
    add_test(NAME privacy_tool_discovery_test
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/privacy_tool_discovery_test.sh
            ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(privacy_tool_discovery_test PROPERTIES
        RUN_SERIAL TRUE
        LABELS "packaging;local;smoke"
        TIMEOUT 30
    )
    add_test(NAME archive_reproducibility_test
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/archive_reproducibility_test.sh
            ${CMAKE_CURRENT_SOURCE_DIR} binary-source
    )
    set_tests_properties(archive_reproducibility_test PROPERTIES
        RUN_SERIAL TRUE
        LABELS "packaging;local;smoke"
        TIMEOUT 30
    )
    add_test(NAME package_checksums_test
        COMMAND ${CMAKE_COMMAND}
            -DPSLOG_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
            -DPSLOG_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/package_checksums_test.cmake
    )
    set_tests_properties(package_checksums_test PROPERTIES
        RUN_SERIAL TRUE
        LABELS "packaging;local;smoke"
        TIMEOUT 30
    )
    add_test(NAME lua_rockspec_boundary_test
        COMMAND ${CMAKE_COMMAND}
            -DPSLOG_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/lua_rockspec_boundary_test.cmake
    )
    set_tests_properties(lua_rockspec_boundary_test PROPERTIES
        RUN_SERIAL TRUE
        LABELS "local;smoke"
        TIMEOUT 30
    )
    add_test(NAME lua_interop_compiler_discovery_test
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/lua_interop_compiler_discovery_test.sh
            ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(lua_interop_compiler_discovery_test PROPERTIES
        RUN_SERIAL TRUE
        LABELS "local;smoke;toolchain;lua"
        TIMEOUT 30
    )
    if(PSLOG_BUILD_LUA)
        add_test(NAME lua_archive_reproducibility_test
            COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/archive_reproducibility_test.sh
                ${CMAKE_CURRENT_SOURCE_DIR} lua
        )
        set_tests_properties(lua_archive_reproducibility_test PROPERTIES
            RUN_SERIAL TRUE
            LABELS "local;packaging;smoke;lua"
            TIMEOUT 30
        )
        add_test(NAME lua_release_artifacts_test
            COMMAND ${CMAKE_COMMAND}
                -DPSLOG_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
                -DPSLOG_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
                -DPSLOG_TARGET_ID=${PSLOG_TARGET_ID}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/lua_release_artifacts_test.cmake
        )
        set_tests_properties(lua_release_artifacts_test PROPERTIES
            RUN_SERIAL TRUE
            LABELS "local;packaging;smoke;lua"
            TIMEOUT 90
        )
    endif()
    add_test(NAME c_only_configure_test
        COMMAND ${CMAKE_COMMAND}
            -DPSLOG_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -DPSLOG_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
            -DPSLOG_C_COMPILER=${CMAKE_C_COMPILER}
            -DPSLOG_ALLOW_HOST_COMPILER=${PSLOG_ALLOW_HOST_COMPILER}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/c_only_configure_test.cmake
    )
    set_tests_properties(c_only_configure_test PROPERTIES
        RUN_SERIAL TRUE
        LABELS "local;smoke"
        TIMEOUT 60
    )
    add_test(NAME clean_script_test
        COMMAND ${CMAKE_COMMAND}
            -DPSLOG_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -DPSLOG_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/clean_script_test.cmake
    )
    set_tests_properties(clean_script_test PROPERTIES
        RUN_SERIAL TRUE
        LABELS "local;smoke"
        TIMEOUT 30
    )
    foreach(check toolchain_discovery_only_test required_ctest_test)
        add_test(NAME ${check}
            COMMAND ${CMAKE_COMMAND}
                -DPSLOG_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
                -DPSLOG_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/${check}.cmake)
        set_tests_properties(${check} PROPERTIES
            RUN_SERIAL TRUE LABELS "local;smoke;toolchain" TIMEOUT 30)
    endforeach()
    add_test(NAME preset_contract_test
        COMMAND ${CMAKE_COMMAND}
            -DPSLOG_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/preset_contract_test.cmake
    )
    set_tests_properties(preset_contract_test PROPERTIES
        RUN_SERIAL TRUE
        LABELS "local;smoke"
        TIMEOUT 30
    )
    add_test(NAME host_toolchain_darwin_test
        COMMAND ${CMAKE_COMMAND}
            -DPSLOG_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/host_toolchain_darwin_test.cmake
    )
    set_tests_properties(host_toolchain_darwin_test PROPERTIES
        RUN_SERIAL TRUE
        LABELS "local;smoke;darwin"
        TIMEOUT 30
    )
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
        add_test(NAME linux_bootlin_required_test
            COMMAND ${CMAKE_COMMAND}
                -DPSLOG_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
                -DPSLOG_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
                -DPSLOG_ALLOW_HOST_COMPILER=${PSLOG_ALLOW_HOST_COMPILER}
                -DPSLOG_HOST_SYSTEM_PROCESSOR=${CMAKE_HOST_SYSTEM_PROCESSOR}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/linux_bootlin_required_test.cmake
        )
        set_tests_properties(linux_bootlin_required_test PROPERTIES
            RUN_SERIAL TRUE
            LABELS "local;smoke;toolchain"
            TIMEOUT 30
        )
    endif()
    add_test(NAME dependency_cache_test
        COMMAND ${CMAKE_COMMAND}
            -DPSLOG_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -DPSLOG_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/dependency_cache_test.cmake
    )
    set_tests_properties(dependency_cache_test PROPERTIES
        RUN_SERIAL TRUE
        LABELS "local;smoke"
        TIMEOUT 30
    )
    add_test(NAME cpkt_toolchains_cache_recovery_test
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/cpkt_toolchains_cache_recovery_test.sh
            ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(cpkt_toolchains_cache_recovery_test PROPERTIES
        ENVIRONMENT "TMPDIR=${CMAKE_CURRENT_BINARY_DIR}"
        RUN_SERIAL TRUE
        LABELS "local;smoke"
        TIMEOUT 30
    )
    add_test(NAME release_matrix_luarocks_override_test
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/release_matrix_luarocks_override_test.sh
            ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(release_matrix_luarocks_override_test PROPERTIES
        RUN_SERIAL TRUE
        LABELS "local;smoke"
        TIMEOUT 30
    )
    add_test(NAME cpkt_aflpp_cache_lock_test
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/cpkt_aflpp_cache_lock_test.sh
            ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(cpkt_aflpp_cache_lock_test PROPERTIES
        ENVIRONMENT "TMPDIR=${CMAKE_CURRENT_BINARY_DIR}"
        RUN_SERIAL TRUE
        LABELS "fuzz;local;smoke"
        TIMEOUT 30
    )
    add_test(NAME fuzz_gate_test
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz_gate_test.sh
            ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(fuzz_gate_test PROPERTIES
        RUN_SERIAL TRUE
        LABELS "fuzz;local;smoke"
        TIMEOUT 30
    )
    add_test(NAME darwin_linker_route_test
        COMMAND ${CMAKE_COMMAND}
            -DPSLOG_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -DPSLOG_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/darwin_linker_route_test.cmake
    )
    set_tests_properties(darwin_linker_route_test PROPERTIES
        RUN_SERIAL TRUE
        LABELS "local;smoke"
        TIMEOUT 30
    )
    add_test(NAME discover_target_tools_test
        COMMAND ${CMAKE_COMMAND}
            -DPSLOG_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -DPSLOG_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/discover_target_tools_test.cmake
    )
    set_tests_properties(discover_target_tools_test PROPERTIES
        RUN_SERIAL TRUE
        LABELS "local;smoke"
        TIMEOUT 30
    )
    add_test(NAME with_lock_test
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/with_lock_test.sh
            ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(with_lock_test PROPERTIES
        RUN_SERIAL TRUE
        LABELS "local;smoke"
        TIMEOUT 30
    )
    add_test(NAME release_pipeline_test
        COMMAND ${CMAKE_COMMAND}
            -DPSLOG_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/release_pipeline_test.cmake
    )
    set_tests_properties(release_pipeline_test PROPERTIES
        RUN_SERIAL TRUE
        LABELS "local;smoke"
        TIMEOUT 30
    )
    add_test(NAME lifecycle_version_contract_test
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/lifecycle_version_contract_test.sh
            ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(lifecycle_version_contract_test PROPERTIES
        RUN_SERIAL TRUE
        LABELS "local;smoke"
        TIMEOUT 30
    )
    add_test(NAME make_parse_test
        COMMAND ${CMAKE_COMMAND}
            -DPSLOG_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/make_parse_test.cmake
    )
    set_tests_properties(make_parse_test PROPERTIES
        RUN_SERIAL TRUE
        LABELS "local;smoke"
        TIMEOUT 30
    )
    add_test(NAME run_timed_test
        COMMAND ${CMAKE_COMMAND}
            -DPSLOG_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -DPSLOG_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_timed_test.cmake
    )
    set_tests_properties(run_timed_test PROPERTIES
        RUN_SERIAL TRUE
        LABELS "local;smoke"
        TIMEOUT 30
    )
    add_test(NAME host_baseline_test
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/host_baseline_test.sh
            ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(host_baseline_test PROPERTIES
        RUN_SERIAL TRUE
        LABELS "local;smoke"
        TIMEOUT 30
    )
    add_test(NAME perf_sample_selection_test
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/perf_sample_selection_test.sh
            ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(perf_sample_selection_test PROPERTIES
        RUN_SERIAL TRUE
        LABELS "local;smoke"
        TIMEOUT 30
    )
    add_test(NAME configure_cmake_test
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/configure_cmake_test.sh
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${PSLOG_ALLOW_HOST_COMPILER}
    )
    set_tests_properties(configure_cmake_test PROPERTIES
        RUN_SERIAL TRUE
        LABELS "local;smoke;toolchain"
        TIMEOUT 60
    )
    if(CMAKE_CURRENT_BINARY_DIR MATCHES "/build/host$")
        add_test(NAME rebaseline_compiler_fallback_test
            COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/rebaseline_compiler_fallback_test.sh
                ${CMAKE_CURRENT_SOURCE_DIR}
        )
        set_tests_properties(rebaseline_compiler_fallback_test PROPERTIES
            RUN_SERIAL TRUE
            LABELS "local;smoke"
            TIMEOUT 30
        )
    endif()
    add_test(NAME public_symbol_visibility_test
        COMMAND ${CMAKE_COMMAND}
            -DPSLOG_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -DPSLOG_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
            -DPSLOG_PUBLIC_HEADER=${CMAKE_CURRENT_SOURCE_DIR}/include/pslog.h
            -DPSLOG_SHARED_LIB=$<TARGET_FILE:pslog_shared>
            -DPSLOG_NM=${CMAKE_NM}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/public_symbol_visibility_test.cmake
    )
    set_tests_properties(public_symbol_visibility_test PROPERTIES
        RUN_SERIAL TRUE
        LABELS "local;smoke"
        TIMEOUT 30
    )
    add_test(NAME public_api_docs_test
        COMMAND ${CMAKE_COMMAND}
            -DPSLOG_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/public_api_docs_test.cmake
    )
    set_tests_properties(public_api_docs_test PROPERTIES
        RUN_SERIAL TRUE
        LABELS "local;smoke"
        TIMEOUT 30
    )
    add_test(NAME example_integration_test
        COMMAND ${CMAKE_COMMAND}
            -DPSLOG_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -DPSLOG_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
            -DPSLOG_C_COMPILER=${CMAKE_C_COMPILER}
            -DPSLOG_C_FLAGS=${PSLOG_TEST_C_FLAGS}
            -DPSLOG_EXE_LINKER_FLAGS=${PSLOG_TEST_EXE_LINKER_FLAGS}
            -DPSLOG_THREAD_LIBS=${CMAKE_THREAD_LIBS_INIT}
        -DPSLOG_CROSSCOMPILING=${CMAKE_CROSSCOMPILING}
        -DPSLOG_NATIVE_EXECUTION=${PSLOG_NATIVE_EXECUTION}
        -DPSLOG_CROSSCOMPILING_EMULATOR=${PSLOG_CROSSCOMPILING_EMULATOR_ARG}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/example_integration_test.cmake
    )
    set_tests_properties(example_integration_test PROPERTIES
        RUN_SERIAL TRUE
        LABELS "example-smoke;local"
        TIMEOUT 60
    )
    add_custom_target(pslog_ctest_prerequisites
        DEPENDS pslog_tests pslog_valgrind_facade_tests pslog_single_header_tests pslog_shared
    )
    set_property(TEST pslog_tests pslog_valgrind_facade_tests
        pslog_single_header_tests package_archives_test
        public_symbol_visibility_test example_integration_test
        APPEND PROPERTY LABELS target)
endif()

if(PSLOG_BUILD_BENCHMARKS)
    if(PSLOG_BENCHMARK_WITH_LIBLOGGER)
        add_library(pslog_bench_liblogger SHARED)
    else()
        add_library(pslog_bench_liblogger SHARED bench/bench_liblogger_stub.c)
    endif()
    target_include_directories(pslog_bench_liblogger PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    pslog_configure_project_warnings(pslog_bench_liblogger)
    set_target_properties(pslog_bench_liblogger PROPERTIES
        OUTPUT_NAME pslog_bench_liblogger
        POSITION_INDEPENDENT_CODE ON
    )

    if(PSLOG_BENCHMARK_WITH_LIBLOGGER)
        pslog_acquire_verified_archive(
            COMPONENT liblogger URL https://github.com/briandowns/liblogger/archive/3ecb5c636b5a97fc35d0a6373a3238f1596be82a.tar.gz
            SHA256 c10ce24f036291e3b52c32a30f1bf53eb3d518693adac1c7d70e4ea87f974464
            OUTPUT liblogger_archive)
        pslog_stage_dependency_archive(
            COMPONENT liblogger ARCHIVE "${liblogger_archive}"
            SHA256 c10ce24f036291e3b52c32a30f1bf53eb3d518693adac1c7d70e4ea87f974464
            OUTPUT liblogger_SOURCE_DIR)

        target_sources(pslog_bench_liblogger PRIVATE
            bench/bench_liblogger.c
            ${liblogger_SOURCE_DIR}/logger.c
        )
        target_include_directories(pslog_bench_liblogger PRIVATE
            ${liblogger_SOURCE_DIR}
            ${jansson_BINARY_DIR}/include
        )
        target_link_libraries(pslog_bench_liblogger PRIVATE jansson)
        set_source_files_properties(
            bench/bench_liblogger.c
            ${liblogger_SOURCE_DIR}/logger.c
            PROPERTIES COMPILE_OPTIONS "-std=gnu99;-Wno-pedantic"
        )
        target_compile_definitions(pslog_bench_liblogger PRIVATE PSLOG_HAVE_LIBLOGGER_BENCH=1)
    endif()

    if(PSLOG_BENCHMARK_WITH_QUILL)
        include(CheckLanguage)
        check_language(CXX)
        if(NOT CMAKE_CXX_COMPILER)
            message(FATAL_ERROR
                "PSLOG_BENCHMARK_WITH_QUILL=ON requires a working C++ compiler for the benchmark-only Quill helper")
        endif()
        enable_language(CXX)

        add_library(pslog_bench_quill SHARED bench/bench_quill.cpp)
        pslog_acquire_verified_archive(
            COMPONENT quill URL https://github.com/odygrd/quill/archive/7c0ffa54e51c2b8db6ec091c2922aeaf9b3b08c0.tar.gz
            SHA256 d8072c2a9c079200d16164b40bfa26ce7690488ab647b5cb48ee5e84a13c0e76
            OUTPUT quill_archive)
        pslog_stage_dependency_archive(
            COMPONENT quill ARCHIVE "${quill_archive}"
            SHA256 d8072c2a9c079200d16164b40bfa26ce7690488ab647b5cb48ee5e84a13c0e76
            OUTPUT quill_SOURCE_DIR)

        target_include_directories(pslog_bench_quill PRIVATE
            ${quill_SOURCE_DIR}/include
        )
        target_link_libraries(pslog_bench_quill PRIVATE Threads::Threads)
        target_compile_features(pslog_bench_quill PRIVATE cxx_std_17)
    else()
        add_library(pslog_bench_quill SHARED bench/bench_quill_stub.c)
    endif()
    target_include_directories(pslog_bench_quill PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    pslog_configure_project_warnings(pslog_bench_quill)
    set_target_properties(pslog_bench_quill PROPERTIES
        OUTPUT_NAME pslog_bench_quill
        POSITION_INDEPENDENT_CODE ON
    )

    add_executable(pslog_bench
        bench/bench_main.c
        ${PSLOG_BENCH_SUPPORT_SOURCES}
    )
    target_include_directories(pslog_bench PRIVATE include ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(pslog_bench PRIVATE pslog_static)
    pslog_configure_project_warnings(pslog_bench)
    pslog_enable_coverage_if_requested(pslog_bench)
    pslog_configure_nonshipped_executable(pslog_bench)
    if(PSLOG_BENCHMARK_WITH_LIBLOGGER)
        target_link_libraries(pslog_bench PRIVATE pslog_bench_liblogger)
        target_compile_definitions(pslog_bench PRIVATE PSLOG_HAVE_LIBLOGGER_BENCH=1)
    endif()
endif()

if(PSLOG_BUILD_FUZZ)
    add_executable(pslog_fuzz fuzz/pslog_fuzz.c)
    target_include_directories(pslog_fuzz PRIVATE include)
    target_link_libraries(pslog_fuzz PRIVATE pslog_static)
    pslog_configure_c_target(pslog_fuzz)
    pslog_configure_nonshipped_executable(pslog_fuzz)
endif()

execute_process(
    COMMAND "${CMAKE_C_COMPILER}" -print-prog-name=gcov
    OUTPUT_VARIABLE GCOV_BIN
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT IS_ABSOLUTE "${GCOV_BIN}" OR NOT EXISTS "${GCOV_BIN}")
    set(PSLOG_GCOV_PROGRAM "${GCOV_BIN}")
    unset(GCOV_BIN)
    unset(GCOV_BIN CACHE)
    if(PSLOG_ALLOW_HOST_COMPILER AND NOT PSLOG_GCOV_PROGRAM STREQUAL "")
        find_program(GCOV_BIN NAMES "${PSLOG_GCOV_PROGRAM}")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_program(GCOV_BIN NAMES "${CMAKE_C_COMPILER_TARGET}-gcov"
            PATHS "${PSLOG_BOOTLIN_ROOT}/bin" NO_DEFAULT_PATH)
    endif()
endif()

if(PSLOG_ENABLE_COVERAGE AND NOT GCOV_BIN)
    message(FATAL_ERROR "Coverage requires gcov from the selected compiler collection")
endif()

if(PSLOG_BUILD_TESTS AND PSLOG_ENABLE_COVERAGE AND GCOV_BIN)
    add_custom_target(coverage-report
        COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_CURRENT_BINARY_DIR} --output-on-failure --stop-on-failure --no-tests=error
        COMMAND ${CMAKE_COMMAND} -E env GCOV=${GCOV_BIN}
                ${CMAKE_CURRENT_SOURCE_DIR}/scripts/coverage_report.sh
                ${CMAKE_CURRENT_BINARY_DIR}
                ${CMAKE_CURRENT_BINARY_DIR}/coverage-report
        DEPENDS pslog_ctest_prerequisites
        USES_TERMINAL
        VERBATIM
    )
endif()

install(FILES
    include/pslog.h
    "${PSLOG_GENERATED_VERSION_HEADER}"
    DESTINATION include
)
install(TARGETS pslog_shared
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
    ARCHIVE DESTINATION lib
)
install(TARGETS pslog_static
    ARCHIVE DESTINATION lib
)
set(PSLOG_PACKAGE_METADATA_DIR "${CMAKE_CURRENT_BINARY_DIR}/package-metadata")
set(PSLOG_PACKAGE_SHARED_LIB_NAME "$<TARGET_FILE_NAME:pslog_shared>")
set(PSLOG_PACKAGE_STATIC_LIB_NAME "$<TARGET_FILE_NAME:pslog_static>")
configure_file(cmake/pslog.pc.in "${PSLOG_PACKAGE_METADATA_DIR}/pslog.pc" @ONLY)
configure_file(cmake/pslogConfig.cmake.in "${PSLOG_PACKAGE_METADATA_DIR}/pslogConfig.cmake.in" @ONLY)
file(GENERATE OUTPUT "${PSLOG_PACKAGE_METADATA_DIR}/pslogConfig.cmake"
    INPUT "${PSLOG_PACKAGE_METADATA_DIR}/pslogConfig.cmake.in")
configure_file(cmake/pslogConfigVersion.cmake.in
    "${PSLOG_PACKAGE_METADATA_DIR}/pslogConfigVersion.cmake" @ONLY)
install(FILES "${PSLOG_PACKAGE_METADATA_DIR}/pslog.pc" DESTINATION lib/pkgconfig)
install(FILES "${PSLOG_PACKAGE_METADATA_DIR}/pslogConfig.cmake"
    "${PSLOG_PACKAGE_METADATA_DIR}/pslogConfigVersion.cmake" DESTINATION lib/cmake/pslog)
install(FILES LICENSE README.md DESTINATION share/doc/libpslog)

add_custom_target(package-archive
    COMMAND ${CMAKE_COMMAND}
        -DPSLOG_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
        -DPSLOG_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
        -DPSLOG_VERSION=${PSLOG_RESOLVED_VERSION}
        -DPSLOG_TARGET_ID=${PSLOG_TARGET_ID}
        -DPSLOG_STATIC_LIB_NAME=$<TARGET_FILE_NAME:pslog_static>
        -DPSLOG_SHARED_LIB_NAME=$<TARGET_FILE_NAME:pslog_shared>
        -DPSLOG_TARGET_TOOL_DISCOVERY=${CMAKE_CURRENT_SOURCE_DIR}/scripts/discover_target_tools.sh
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/package_archive.cmake
    DEPENDS pslog_shared pslog_static
    VERBATIM
)

add_custom_target(package-single-header
    COMMAND ${CMAKE_COMMAND}
        -DPSLOG_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
        -DPSLOG_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
        -DPSLOG_VERSION=${PSLOG_RESOLVED_VERSION}
        -DPSLOG_READELF=${CMAKE_READELF}
        -DPSLOG_OTOOL=${CPKT_OTOOL}
        -DPSLOG_PUBLIC_HEADER=${CMAKE_CURRENT_SOURCE_DIR}/include/pslog.h
        -DPSLOG_PUBLIC_VERSION_HEADER=${PSLOG_GENERATED_VERSION_HEADER}
        -DPSLOG_INTERNAL_HEADER=${CMAKE_CURRENT_SOURCE_DIR}/src/pslog_internal.h
        -DPSLOG_SINGLE_HEADER_BUILD=${PSLOG_SINGLE_HEADER_BUILD}
        -DPSLOG_SINGLE_HEADER_BUILD_ALIAS=${PSLOG_SINGLE_HEADER_BUILD_ALIAS}
        -DPSLOG_SINGLE_HEADER_DIST=${PSLOG_SINGLE_HEADER_DIST}
        -DPSLOG_SINGLE_HEADER_DIST_GZ=${PSLOG_SINGLE_HEADER_DIST_GZ}
        -DPSLOG_SINGLE_HEADER_SOURCES=${PSLOG_SINGLE_HEADER_SOURCE_PATHS_ARG}
        -DPSLOG_CLANG_FORMAT_BIN=${CLANG_FORMAT_BIN}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/package_single_header.cmake
    VERBATIM
)

add_custom_target(package-source
    COMMAND ${CMAKE_COMMAND}
        -DPSLOG_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
        -DPSLOG_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
        -DPSLOG_VERSION=${PSLOG_RESOLVED_VERSION}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/package_source_archive.cmake
    VERBATIM
)

add_custom_target(package-checksums
    COMMAND ${CMAKE_COMMAND}
        -DPSLOG_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
        -DPSLOG_VERSION=${PSLOG_RESOLVED_VERSION}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/package_checksums.cmake
    DEPENDS package-single-header
    VERBATIM
)

add_custom_target(package-privacy-gate
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify_release_privacy.sh
        --build-dir ${CMAKE_CURRENT_BINARY_DIR}
        --target-id ${PSLOG_TARGET_ID}
    DEPENDS package-checksums
    VERBATIM
)

add_custom_target(package-clean-dist
    COMMAND ${CMAKE_COMMAND}
        -DPSLOG_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/package_clean_dist.cmake
    VERBATIM
)
