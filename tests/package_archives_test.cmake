if(NOT DEFINED PSLOG_CROSSCOMPILING)
    set(PSLOG_CROSSCOMPILING FALSE)
endif()

set(archive "${PSLOG_ROOT}/dist/libpslog-${PSLOG_VERSION}-${PSLOG_TARGET_ID}.tar.gz")
set(single_header "${PSLOG_ROOT}/dist/pslog-${PSLOG_VERSION}.h")
set(single_header_gz "${single_header}.gz")
set(build_single_header "${PSLOG_BINARY_DIR}/generated/include/pslog_single_header.h")
set(checksums_file "${PSLOG_ROOT}/dist/libpslog-${PSLOG_VERSION}-CHECKSUMS")
set(installed_sdk "${PSLOG_BINARY_DIR}/installed-sdk-test")
file(REMOVE_RECURSE "${installed_sdk}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${PSLOG_BINARY_DIR}" --prefix "${installed_sdk}"
    RESULT_VARIABLE install_result)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR "failed to install the public SDK")
endif()
set(sdk_metadata lib/pkgconfig/pslog.pc lib/cmake/pslog/pslogConfig.cmake
    lib/cmake/pslog/pslogConfigVersion.cmake)
foreach(metadata IN LISTS sdk_metadata)
    if(NOT EXISTS "${installed_sdk}/${metadata}")
        message(FATAL_ERROR "installed SDK is missing consumer metadata: ${metadata}")
    endif()
endforeach()

set(redirected_prefix "${PSLOG_BINARY_DIR}/redirected-sdk-test")
file(REMOVE_RECURSE "${redirected_prefix}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "DESTDIR=${redirected_prefix}"
        "${CMAKE_COMMAND}" --build "${PSLOG_BINARY_DIR}" --target package-archive
    RESULT_VARIABLE redirected_result OUTPUT_VARIABLE redirected_output ERROR_VARIABLE redirected_error)
if(redirected_result EQUAL 0 OR EXISTS "${redirected_prefix}" OR
   NOT "${redirected_output}${redirected_error}" MATCHES "requires DESTDIR unset")
    message(FATAL_ERROR "archive staging accepted a redirected install: ${redirected_output}${redirected_error}")
endif()

file(REMOVE "${archive}" "${single_header}" "${single_header_gz}" "${checksums_file}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${PSLOG_BINARY_DIR}" --target package-archive package-single-header
    RESULT_VARIABLE build_result
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "failed to build package archives and single-header artifact")
endif()

# Exercise the real target strip tool, including archive-member metadata. The
# small archive fixture uses a stub and cannot detect nondeterministic strip.
file(SHA256 "${archive}" first_archive_digest)
execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1.1)
execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${PSLOG_BINARY_DIR}" --target package-archive
    RESULT_VARIABLE repeat_archive_result)
if(NOT repeat_archive_result EQUAL 0)
    message(FATAL_ERROR "failed to repeat SDK archive generation")
endif()
file(SHA256 "${archive}" repeated_archive_digest)
if(NOT first_archive_digest STREQUAL repeated_archive_digest)
    message(FATAL_ERROR "unchanged SDK inputs produced different archive bytes")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${PSLOG_BINARY_DIR}" --target package-checksums
    RESULT_VARIABLE checksum_build_result
)
if(NOT checksum_build_result EQUAL 0)
    message(FATAL_ERROR "failed to build package checksums")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${PSLOG_BINARY_DIR}" --target package-single-header
    RESULT_VARIABLE repeat_single_header_result
)
if(NOT repeat_single_header_result EQUAL 0)
    message(FATAL_ERROR "failed to rebuild the single-header artifact")
endif()
execute_process(
    COMMAND "${PSLOG_ROOT}/scripts/sha256_files.sh" --check "${checksums_file}"
    WORKING_DIRECTORY "${PSLOG_ROOT}/dist"
    RESULT_VARIABLE stable_checksum_result
    OUTPUT_VARIABLE stable_checksum_output
    ERROR_VARIABLE stable_checksum_error
)
if(NOT stable_checksum_result EQUAL 0)
    message(FATAL_ERROR
        "single-header rebuild changed the checksum-listed artifact:\n"
        "${stable_checksum_output}${stable_checksum_error}")
endif()

function(assert_consumer_runtime binary)
    if(NOT PSLOG_NATIVE_EXECUTION OR NOT PSLOG_BOOTLIN_INTERPRETER)
        return()
    endif()
    execute_process(COMMAND "${PSLOG_READELF}" -l -d "${binary}"
        OUTPUT_VARIABLE metadata COMMAND_ERROR_IS_FATAL ANY)
    string(REGEX MATCH "Requesting program interpreter: ([^]]+)" match "${metadata}")
    if(NOT CMAKE_MATCH_1 STREQUAL PSLOG_BOOTLIN_INTERPRETER OR NOT metadata MATCHES "\\(RPATH\\)")
        message(FATAL_ERROR "SDK verification consumer lacks private Bootlin runtime metadata: ${binary}")
    endif()
    execute_process(COMMAND "${PSLOG_BOOTLIN_INTERPRETER}" --list "${binary}"
        OUTPUT_VARIABLE objects COMMAND_ERROR_IS_FATAL ANY)
    string(REGEX MATCHALL "=> /[^ \n]+" paths "${objects}")
    foreach(path IN LISTS paths)
        string(REPLACE "=> " "" path "${path}")
        string(FIND "${path}" "${PSLOG_BOOTLIN_ROOT}/" runtime_match)
        string(FIND "${path}" "${extracted_package_root}/lib/" sdk_match)
        if(NOT runtime_match EQUAL 0 AND NOT sdk_match EQUAL 0)
            message(FATAL_ERROR "SDK verification consumer loaded an unexpected library: ${path}")
        endif()
    endforeach()
endfunction()

function(assert_archive_layout archive_path)
    if(NOT EXISTS "${archive_path}")
        message(FATAL_ERROR "missing archive: ${archive_path}")
    endif()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar tf "${archive_path}"
        RESULT_VARIABLE tar_result
        OUTPUT_VARIABLE archive_listing
    )
    if(NOT tar_result EQUAL 0)
        message(FATAL_ERROR "failed to list archive contents: ${archive_path}")
    endif()

    get_filename_component(archive_filename "${archive_path}" NAME)
    string(REGEX REPLACE "\\.tar\\.gz$" "" archive_root "${archive_filename}")
    string(REPLACE "." "\\." archive_root_regex "${archive_root}")

    if(archive_listing MATCHES "(^|\n)\\./")
        message(FATAL_ERROR "archive contains entries starting with ./: ${archive_path}")
    endif()
    if(NOT archive_listing MATCHES "(^|\n)${archive_root_regex}/")
        message(FATAL_ERROR "archive entries do not start with ${archive_root}: ${archive_path}")
    endif()
    if(archive_listing MATCHES "(^|\n)(include|lib|share)/")
        message(FATAL_ERROR "archive contains bare top-level entries instead of ${archive_root}/: ${archive_path}")
    endif()

    if(NOT archive_listing MATCHES "(^|\n)${archive_root_regex}/share/doc/libpslog/LICENSE(\n|$)")
        message(FATAL_ERROR "archive missing share/doc/libpslog/LICENSE: ${archive_path}")
    endif()
    if(NOT archive_listing MATCHES "(^|\n)${archive_root_regex}/share/doc/libpslog/README.md(\n|$)")
        message(FATAL_ERROR "archive missing share/doc/libpslog/README.md: ${archive_path}")
    endif()
    if(NOT archive_listing MATCHES "(^|\n)${archive_root_regex}/include/pslog.h(\n|$)")
        message(FATAL_ERROR "archive missing include/pslog.h: ${archive_path}")
    endif()
    if(archive_listing MATCHES "(^|\n)${archive_root_regex}/include/pslog_lua.h(\n|$)")
        message(FATAL_ERROR "C binary SDK unexpectedly contains Lua adapter header include/pslog_lua.h: ${archive_path}")
    endif()
    if(NOT archive_listing MATCHES "(^|\n)${archive_root_regex}/include/pslog_version.h(\n|$)")
        message(FATAL_ERROR "archive missing include/pslog_version.h: ${archive_path}")
    endif()
    if(NOT archive_listing MATCHES "(^|\n)${archive_root_regex}/lib/pkgconfig/pslog\\.pc(\n|$)")
        message(FATAL_ERROR "archive missing pkg-config metadata lib/pkgconfig/pslog.pc: ${archive_path}")
    endif()
    if(NOT archive_listing MATCHES "(^|\n)${archive_root_regex}/lib/cmake/pslog/pslogConfig\\.cmake(\n|$)")
        message(FATAL_ERROR "archive missing CMake package config lib/cmake/pslog/pslogConfig.cmake: ${archive_path}")
    endif()
    if(NOT archive_listing MATCHES "(^|\n)${archive_root_regex}/lib/cmake/pslog/pslogConfigVersion\\.cmake(\n|$)")
        message(FATAL_ERROR "archive missing CMake package version config lib/cmake/pslog/pslogConfigVersion.cmake: ${archive_path}")
    endif()
    string(REPLACE "." "\\." shared_lib_regex "${PSLOG_SHARED_LIB_NAME}")
    if(NOT archive_listing MATCHES "(^|\n)${archive_root_regex}/lib/${shared_lib_regex}(\n|$)")
        message(FATAL_ERROR "archive missing shared library lib/${PSLOG_SHARED_LIB_NAME}: ${archive_path}")
    endif()
    if(DEFINED PSLOG_SHARED_LINK_NAME
       AND NOT PSLOG_SHARED_LINK_NAME STREQUAL ""
       AND NOT PSLOG_SHARED_LINK_NAME STREQUAL PSLOG_SHARED_LIB_NAME)
        string(REPLACE "." "\\." shared_link_regex "${PSLOG_SHARED_LINK_NAME}")
        if(NOT archive_listing MATCHES "(^|\n)${archive_root_regex}/lib/${shared_link_regex}(\n|$)")
            message(FATAL_ERROR "archive missing shared-library linker entry lib/${PSLOG_SHARED_LINK_NAME}: ${archive_path}")
        endif()
    endif()
    string(REPLACE "." "\\." static_lib_regex "${PSLOG_STATIC_LIB_NAME}")
    if(NOT archive_listing MATCHES "(^|\n)${archive_root_regex}/lib/${static_lib_regex}(\n|$)")
        message(FATAL_ERROR "archive missing static library lib/${PSLOG_STATIC_LIB_NAME}: ${archive_path}")
    endif()
    if(archive_listing MATCHES "(^|\n)${archive_root_regex}/share/libpslog(/|\n|$)")
        message(FATAL_ERROR "archive still contains legacy share/libpslog path: ${archive_path}")
    endif()
    if(archive_listing MATCHES "(^|\n)${archive_root_regex}/share/doc/libpslog/demo.gif(\n|$)")
        message(FATAL_ERROR "archive unexpectedly contains share/doc/libpslog/demo.gif: ${archive_path}")
    endif()
    if(archive_listing MATCHES "(^|\n)${archive_root_regex}/share/doc/libpslog/elevatorpitch.gif(\n|$)")
        message(FATAL_ERROR "archive unexpectedly contains share/doc/libpslog/elevatorpitch.gif: ${archive_path}")
    endif()
    if(DEFINED PSLOG_SHARED_SONAME
       AND NOT PSLOG_SHARED_SONAME STREQUAL ""
       AND NOT PSLOG_SHARED_SONAME STREQUAL PSLOG_SHARED_LIB_NAME
        AND archive_path STREQUAL archive)
        string(REPLACE "." "\\." shared_soname_regex "${PSLOG_SHARED_SONAME}")
        if(NOT archive_listing MATCHES "(^|\n)${archive_root_regex}/lib/${shared_soname_regex}(\n|$)")
            message(FATAL_ERROR "archive missing shared-library SONAME entry lib/${PSLOG_SHARED_SONAME}: ${archive_path}")
        endif()
    endif()

    file(READ "${archive_path}" archive_xfl HEX OFFSET 8 LIMIT 1)
    string(TOLOWER "${archive_xfl}" archive_xfl)
    if(NOT archive_xfl STREQUAL "02")
        message(FATAL_ERROR "archive is not using gzip maximum compression header: ${archive_path}")
    endif()

    set(extract_root "${PSLOG_BINARY_DIR}/package-archive-metadata-test/${archive_root}")
    file(REMOVE_RECURSE "${extract_root}")
    file(MAKE_DIRECTORY "${extract_root}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar xzf "${archive_path}"
        WORKING_DIRECTORY "${extract_root}"
        RESULT_VARIABLE extract_result
    )
    if(NOT extract_result EQUAL 0)
        message(FATAL_ERROR "failed to extract archive for metadata checks: ${archive_path}")
    endif()

    set(extracted_package_root "${extract_root}/${archive_root}")
    foreach(metadata IN LISTS sdk_metadata)
        file(SHA256 "${installed_sdk}/${metadata}" installed_digest)
        file(SHA256 "${extracted_package_root}/${metadata}" packaged_digest)
        if(NOT installed_digest STREQUAL packaged_digest)
            message(FATAL_ERROR "installed and archived SDK metadata differ: ${metadata}")
        endif()
    endforeach()
    file(READ "${extracted_package_root}/lib/pkgconfig/pslog.pc" pkg_config_text)
    if(NOT pkg_config_text MATCHES "Name: pslog")
        message(FATAL_ERROR "pkg-config metadata has wrong package name: ${archive_path}")
    endif()
    if(NOT pkg_config_text MATCHES "Version: ${PSLOG_VERSION}")
        message(FATAL_ERROR "pkg-config metadata has wrong version: ${archive_path}")
    endif()
    if(NOT pkg_config_text MATCHES "Libs: -L\\$\\{libdir\\} -lpslog")
        message(FATAL_ERROR "pkg-config metadata does not link libpslog: ${archive_path}")
    endif()
    if(NOT pkg_config_text MATCHES "Cflags: -I\\$\\{includedir\\}")
        message(FATAL_ERROR "pkg-config metadata does not expose the include directory: ${archive_path}")
    endif()
    if(EXISTS "${extracted_package_root}/include/pslog_lua.h")
        message(FATAL_ERROR "C binary SDK extracted tree unexpectedly contains Lua adapter header: ${archive_path}")
    endif()

    file(READ "${extracted_package_root}/lib/cmake/pslog/pslogConfig.cmake" cmake_config_text)
    if(NOT cmake_config_text MATCHES "add_library\\(pslog::pslog_shared UNKNOWN IMPORTED\\)")
        message(FATAL_ERROR "CMake package config is missing pslog::pslog_shared imported target: ${archive_path}")
    endif()
    if(NOT cmake_config_text MATCHES "add_library\\(pslog::pslog_static STATIC IMPORTED\\)")
        message(FATAL_ERROR "CMake package config is missing pslog::pslog_static imported target: ${archive_path}")
    endif()
    if(NOT cmake_config_text MATCHES "add_library\\(pslog::pslog INTERFACE IMPORTED\\)")
        message(FATAL_ERROR "CMake package config is missing default pslog::pslog target: ${archive_path}")
    endif()
    string(REPLACE "." "\\." shared_lib_regex "${PSLOG_SHARED_LIB_NAME}")
    if(NOT cmake_config_text MATCHES "IMPORTED_LOCATION \"\\$\\{_PSLOG_PREFIX\\}/lib/${shared_lib_regex}\"")
        message(FATAL_ERROR "CMake package config points at the wrong shared library: ${archive_path}")
    endif()
    string(REPLACE "." "\\." static_lib_regex "${PSLOG_STATIC_LIB_NAME}")
    if(NOT cmake_config_text MATCHES "IMPORTED_LOCATION \"\\$\\{_PSLOG_PREFIX\\}/lib/${static_lib_regex}\"")
        message(FATAL_ERROR "CMake package config points at the wrong static library: ${archive_path}")
    endif()
    if(NOT cmake_config_text MATCHES "INTERFACE_INCLUDE_DIRECTORIES \"\\$\\{_PSLOG_PREFIX\\}/include\"")
        message(FATAL_ERROR "CMake package config does not expose the include directory: ${archive_path}")
    endif()

    file(READ "${extracted_package_root}/lib/cmake/pslog/pslogConfigVersion.cmake" cmake_version_text)
    if(NOT cmake_version_text MATCHES "set\\(PACKAGE_VERSION \"${PSLOG_VERSION}\"\\)")
        message(FATAL_ERROR "CMake package version config has wrong version: ${archive_path}")
    endif()

    set(cmake_consumer_root "${PSLOG_BINARY_DIR}/package-archive-cmake-consumer-test")
    set(cmake_consumer_build "${cmake_consumer_root}/build")
    file(REMOVE_RECURSE "${cmake_consumer_root}")
    file(MAKE_DIRECTORY "${cmake_consumer_root}")
    file(WRITE "${cmake_consumer_root}/CMakeLists.txt"
"cmake_minimum_required(VERSION 3.21)
project(pslog_package_consumer C)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
find_package(pslog ${PSLOG_VERSION} CONFIG REQUIRED)
add_executable(pslog_package_consumer main.c)
target_link_libraries(pslog_package_consumer PRIVATE pslog::pslog)
target_compile_options(pslog_package_consumer PRIVATE -std=c89 -Wall -Wextra -Wpedantic -Werror)
include(\"${PSLOG_ROOT}/cmake/pslog_local_runtime.cmake\")
pslog_configure_nonshipped_executable(pslog_package_consumer)
add_executable(pslog_package_static main.c)
target_link_libraries(pslog_package_static PRIVATE pslog::pslog_static)
target_compile_options(pslog_package_static PRIVATE -std=c89 -Wall -Wextra -Wpedantic -Werror)
pslog_configure_nonshipped_executable(pslog_package_static)
separate_arguments(local_link_flags NATIVE_COMMAND \"${PSLOG_EXE_LINKER_FLAGS}\")
target_link_options(pslog_package_consumer PRIVATE \${local_link_flags})
target_link_options(pslog_package_static PRIVATE \${local_link_flags})
")
    file(WRITE "${cmake_consumer_root}/main.c"
"#include <pslog.h>

int main(void) {
    pslog_config config;
    pslog_default_config(&config);
    return config.mode == PSLOG_MODE_CONSOLE ? 0 : 1;
}
")

    set(cmake_consumer_configure_args
        -DCMAKE_PREFIX_PATH=${extracted_package_root}
        -DCMAKE_C_COMPILER=${PSLOG_C_COMPILER}
        -DPSLOG_ALLOW_HOST_COMPILER=${PSLOG_ALLOW_HOST_COMPILER}
        -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH
    )
    if(DEFINED PSLOG_TOOLCHAIN_FILE AND NOT PSLOG_TOOLCHAIN_FILE STREQUAL "")
        list(APPEND cmake_consumer_configure_args
            -DCMAKE_TOOLCHAIN_FILE=${PSLOG_TOOLCHAIN_FILE})
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${cmake_consumer_root}" -B "${cmake_consumer_build}"
                ${cmake_consumer_configure_args}
        RESULT_VARIABLE cmake_consumer_configure_result
        OUTPUT_VARIABLE cmake_consumer_configure_stdout
        ERROR_VARIABLE cmake_consumer_configure_stderr
    )
    if(NOT cmake_consumer_configure_result EQUAL 0)
        message(FATAL_ERROR
            "failed to configure CMake consumer against archive metadata: ${archive_path}\n"
            "stdout:\n${cmake_consumer_configure_stdout}\n"
            "stderr:\n${cmake_consumer_configure_stderr}")
    endif()
    if(PSLOG_ALLOW_HOST_COMPILER)
        file(STRINGS "${cmake_consumer_build}/CMakeCache.txt" consumer_compiler_line
            REGEX "^CMAKE_C_COMPILER:[^=]*=")
        string(REGEX REPLACE "^[^=]+=" "" consumer_compiler "${consumer_compiler_line}")
        file(REAL_PATH "${PSLOG_C_COMPILER}" expected_compiler)
        file(REAL_PATH "${consumer_compiler}" configured_compiler)
        if(NOT configured_compiler STREQUAL expected_compiler)
            message(FATAL_ERROR
                "Package consumer did not retain the host compiler override: "
                "expected ${expected_compiler}, got ${configured_compiler}")
        endif()
    endif()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" --build "${cmake_consumer_build}"
        RESULT_VARIABLE cmake_consumer_build_result
        OUTPUT_VARIABLE cmake_consumer_build_stdout
        ERROR_VARIABLE cmake_consumer_build_stderr
    )
    if(NOT cmake_consumer_build_result EQUAL 0)
        message(FATAL_ERROR
            "failed to build CMake consumer against archive metadata: ${archive_path}\n"
            "stdout:\n${cmake_consumer_build_stdout}\n"
            "stderr:\n${cmake_consumer_build_stderr}")
    endif()

    string(REPLACE "|" ";" runner "${PSLOG_CROSSCOMPILING_EMULATOR}")
    if(NOT PSLOG_CROSSCOMPILING OR PSLOG_NATIVE_EXECUTION OR runner)
        foreach(binary pslog_package_consumer pslog_package_static)
            assert_consumer_runtime("${cmake_consumer_build}/${binary}")
            execute_process(COMMAND ${runner} "${cmake_consumer_build}/${binary}" RESULT_VARIABLE run_result)
            if(NOT run_result EQUAL 0)
                message(FATAL_ERROR "Installed SDK consumer failed: ${binary}: ${run_result}")
            endif()
        endforeach()
    endif()
    separate_arguments(local_flags NATIVE_COMMAND "${PSLOG_EXE_LINKER_FLAGS}")
    find_program(PKG_CONFIG_BIN NAMES pkg-config REQUIRED)
    if(PKG_CONFIG_BIN)
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env
                    "PKG_CONFIG_PATH=${extracted_package_root}/lib/pkgconfig"
                    "${PKG_CONFIG_BIN}" --cflags --libs pslog
            RESULT_VARIABLE pkg_config_result
            OUTPUT_VARIABLE pkg_config_flags
            ERROR_VARIABLE pkg_config_stderr
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(NOT pkg_config_result EQUAL 0)
            message(FATAL_ERROR
                "pkg-config could not resolve pslog metadata: ${archive_path}\n"
                "stderr:\n${pkg_config_stderr}")
        endif()

        separate_arguments(pkg_config_compile_flags NATIVE_COMMAND "${pkg_config_flags}")
        execute_process(
            COMMAND "${PSLOG_C_COMPILER}" -std=c89 -Wall -Wextra -Wpedantic -Werror "-o" "${cmake_consumer_root}/pkg-config-consumer"
                    "${cmake_consumer_root}/main.c" ${pkg_config_compile_flags}
                    ${local_flags} "-Wl,-rpath,${extracted_package_root}/lib"
            RESULT_VARIABLE pkg_config_compile_result
            OUTPUT_VARIABLE pkg_config_compile_stdout
            ERROR_VARIABLE pkg_config_compile_stderr
        )
        if(NOT pkg_config_compile_result EQUAL 0)
            message(FATAL_ERROR
                "failed to compile pkg-config consumer against archive metadata: ${archive_path}\n"
                "stdout:\n${pkg_config_compile_stdout}\n"
                "stderr:\n${pkg_config_compile_stderr}")
        endif()
        if(NOT PSLOG_CROSSCOMPILING OR PSLOG_NATIVE_EXECUTION OR runner)
            assert_consumer_runtime("${cmake_consumer_root}/pkg-config-consumer")
            execute_process(COMMAND ${runner} "${cmake_consumer_root}/pkg-config-consumer" RESULT_VARIABLE result)
            if(NOT result EQUAL 0)
                message(FATAL_ERROR "pkg-config SDK consumer failed: ${result}")
            endif()
        endif()
    endif()
endfunction()

assert_archive_layout("${archive}")

if(EXISTS "${single_header}")
    message(FATAL_ERROR "uncompressed single-header artifact must not remain under dist: ${single_header}")
endif()
if(NOT EXISTS "${single_header_gz}")
    message(FATAL_ERROR "missing gzipped single-header artifact: ${single_header_gz}")
endif()
if(NOT EXISTS "${build_single_header}")
    message(FATAL_ERROR "missing build-tree single-header artifact: ${build_single_header}")
endif()
if(NOT EXISTS "${checksums_file}")
    message(FATAL_ERROR "missing checksums file: ${checksums_file}")
endif()

file(READ "${build_single_header}" single_header_text)
if(NOT single_header_text MATCHES "PSLOG_IMPLEMENTATION")
    message(FATAL_ERROR "single-header artifact is missing PSLOG_IMPLEMENTATION section")
endif()
if(NOT single_header_text MATCHES "Artifact: pslog-${PSLOG_VERSION}\\.h")
    message(FATAL_ERROR "single-header artifact is missing versioned artifact metadata")
endif()
if(NOT single_header_text MATCHES "MIT License")
    message(FATAL_ERROR "single-header artifact is missing embedded license text")
endif()

file(READ "${checksums_file}" checksums_text)
if(checksums_text MATCHES "(^|\n)[0-9a-f]+  pslog-${PSLOG_VERSION}\\.h(\n|$)")
    message(FATAL_ERROR "checksums file unexpectedly includes pslog-${PSLOG_VERSION}.h")
endif()
if(NOT checksums_text MATCHES "(^|\n)[0-9a-f]+  pslog-${PSLOG_VERSION}\\.h\\.gz(\n|$)")
    message(FATAL_ERROR "checksums file is missing pslog-${PSLOG_VERSION}.h.gz")
endif()
