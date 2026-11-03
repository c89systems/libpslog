if(NOT DEFINED PSLOG_ROOT)
    message(FATAL_ERROR "PSLOG_ROOT is required")
endif()
if(NOT DEFINED PSLOG_BINARY_DIR)
    message(FATAL_ERROR "PSLOG_BINARY_DIR is required")
endif()
if(NOT DEFINED PSLOG_VERSION)
    message(FATAL_ERROR "PSLOG_VERSION is required")
endif()

set(source_archive "${PSLOG_ROOT}/dist/libpslog-${PSLOG_VERSION}.tar.gz")
set(test_root "${PSLOG_BINARY_DIR}/source-archive-smoke-test")
set(extract_root "${test_root}/extract")
set(source_root "${extract_root}/libpslog-${PSLOG_VERSION}")
set(source_build "${test_root}/build")
set(source_package_archive_glob "${source_root}/dist/libpslog-${PSLOG_VERSION}-*.tar.gz")

file(REMOVE "${source_archive}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${PSLOG_BINARY_DIR}" --target package-source
    RESULT_VARIABLE package_source_result
)
if(NOT package_source_result EQUAL 0)
    message(FATAL_ERROR "failed to build source archive")
endif()
if(NOT EXISTS "${source_archive}")
    message(FATAL_ERROR "missing source archive: ${source_archive}")
endif()

file(REMOVE_RECURSE "${test_root}")
file(MAKE_DIRECTORY "${extract_root}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar xzf "${source_archive}"
    WORKING_DIRECTORY "${extract_root}"
    RESULT_VARIABLE extract_result
)
if(NOT extract_result EQUAL 0)
    message(FATAL_ERROR "failed to extract source archive")
endif()

if(NOT EXISTS "${source_root}/VERSION")
    message(FATAL_ERROR "source archive missing VERSION")
endif()
if(NOT EXISTS "${source_root}/RELEASE_MANIFEST")
    message(FATAL_ERROR "source archive missing RELEASE_MANIFEST")
endif()
file(READ "${source_root}/VERSION" archive_version)
string(STRIP "${archive_version}" archive_version)
if(NOT archive_version STREQUAL "${PSLOG_VERSION}")
    message(FATAL_ERROR
        "source archive VERSION does not match package version: ${archive_version} != ${PSLOG_VERSION}")
endif()

set(configure_command
    "${CMAKE_COMMAND}" -S "${source_root}" -B "${source_build}"
    -DCMAKE_BUILD_TYPE=Release
    -DPSLOG_BUILD_BENCHMARKS=OFF
    -DPSLOG_BUILD_FUZZ=OFF
)
if(DEFINED PSLOG_TOOLCHAIN_RELATIVE AND NOT PSLOG_TOOLCHAIN_RELATIVE STREQUAL "")
    set(source_toolchain_file "${source_root}/${PSLOG_TOOLCHAIN_RELATIVE}")
    if(NOT EXISTS "${source_toolchain_file}")
        message(FATAL_ERROR "source archive smoke toolchain does not exist: ${source_toolchain_file}")
    endif()
    list(APPEND configure_command "-DCMAKE_TOOLCHAIN_FILE=${source_toolchain_file}")
else()
    message(FATAL_ERROR "source archive smoke requires PSLOG_TOOLCHAIN_RELATIVE")
endif()
if(DEFINED PSLOG_C_COMPILER AND NOT PSLOG_C_COMPILER STREQUAL "")
    list(APPEND configure_command "-DCMAKE_C_COMPILER=${PSLOG_C_COMPILER}")
endif()
execute_process(
    COMMAND ${configure_command}
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_stdout
    ERROR_VARIABLE configure_stderr
)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR
        "failed to configure extracted source archive\n"
        "stdout:\n${configure_stdout}\n"
        "stderr:\n${configure_stderr}")
endif()

set(generated_version_header "${source_build}/generated/include/pslog_version.h")
if(NOT EXISTS "${generated_version_header}")
    message(FATAL_ERROR "extracted source configure did not generate pslog_version.h")
endif()
file(READ "${generated_version_header}" generated_version_header_text)
if(NOT generated_version_header_text MATCHES "#define PSLOG_VERSION_STRING \"${PSLOG_VERSION}\"")
    message(FATAL_ERROR
        "generated version header does not match source archive VERSION ${PSLOG_VERSION}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${source_build}" --target pslog_tests pslog_single_header_tests
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_stdout
    ERROR_VARIABLE build_stderr
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR
        "failed to build extracted source archive smoke targets\n"
        "stdout:\n${build_stdout}\n"
        "stderr:\n${build_stderr}")
endif()

execute_process(
    COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${source_build}" -R "^(pslog_tests|pslog_single_header_tests)$" --output-on-failure --stop-on-failure --no-tests=error
    RESULT_VARIABLE test_result
    OUTPUT_VARIABLE test_stdout
    ERROR_VARIABLE test_stderr
)
if(NOT test_result EQUAL 0)
    message(FATAL_ERROR
        "extracted source archive smoke tests failed\n"
        "stdout:\n${test_stdout}\n"
        "stderr:\n${test_stderr}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${source_build}" --target package-archive
    RESULT_VARIABLE package_archive_result
    OUTPUT_VARIABLE package_archive_stdout
    ERROR_VARIABLE package_archive_stderr
)
if(NOT package_archive_result EQUAL 0)
    message(FATAL_ERROR
        "failed to build binary package archive from extracted source archive\n"
        "stdout:\n${package_archive_stdout}\n"
        "stderr:\n${package_archive_stderr}")
endif()

file(GLOB source_package_archives "${source_package_archive_glob}")
list(LENGTH source_package_archives source_package_archive_count)
if(NOT source_package_archive_count EQUAL 1)
    message(FATAL_ERROR
        "expected one binary package archive from extracted source, got ${source_package_archive_count}")
endif()
list(GET source_package_archives 0 source_package_archive)

set(package_extract_root "${test_root}/package-extract")
file(MAKE_DIRECTORY "${package_extract_root}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar xzf "${source_package_archive}"
    WORKING_DIRECTORY "${package_extract_root}"
    RESULT_VARIABLE package_extract_result
)
if(NOT package_extract_result EQUAL 0)
    message(FATAL_ERROR "failed to extract binary package archive from source smoke")
endif()

get_filename_component(source_package_archive_name "${source_package_archive}" NAME)
string(REGEX REPLACE "\\.tar\\.gz$" "" source_package_root_name "${source_package_archive_name}")
set(source_package_root "${package_extract_root}/${source_package_root_name}")
set(source_pkg_config "${source_package_root}/lib/pkgconfig/pslog.pc")
set(source_cmake_config "${source_package_root}/lib/cmake/pslog/pslogConfig.cmake")
set(source_cmake_version "${source_package_root}/lib/cmake/pslog/pslogConfigVersion.cmake")

foreach(metadata_file IN ITEMS
        "${source_pkg_config}"
        "${source_cmake_config}"
        "${source_cmake_version}")
    if(NOT EXISTS "${metadata_file}")
        message(FATAL_ERROR
            "binary package built from source archive is missing metadata: ${metadata_file}")
    endif()
endforeach()

file(READ "${source_pkg_config}" source_pkg_config_text)
if(NOT source_pkg_config_text MATCHES "Version: ${PSLOG_VERSION}")
    message(FATAL_ERROR "pkg-config metadata version does not match source archive VERSION")
endif()
if(NOT source_pkg_config_text MATCHES "prefix=\\$\\{pcfiledir\\}/\\.\\./\\.\\.")
    message(FATAL_ERROR "pkg-config metadata is not relocatable from lib/pkgconfig")
endif()

file(READ "${source_cmake_config}" source_cmake_config_text)
if(NOT source_cmake_config_text MATCHES "add_library\\(pslog::pslog_shared UNKNOWN IMPORTED\\)")
    message(FATAL_ERROR "CMake metadata is missing pslog::pslog_shared")
endif()
if(NOT source_cmake_config_text MATCHES "get_filename_component\\(_PSLOG_PREFIX")
    message(FATAL_ERROR "CMake metadata is not relocatable from its package directory")
endif()

file(READ "${source_cmake_version}" source_cmake_version_text)
if(NOT source_cmake_version_text MATCHES "set\\(PACKAGE_VERSION \"${PSLOG_VERSION}\"\\)")
    message(FATAL_ERROR "CMake package version does not match source archive VERSION")
endif()

file(REMOVE_RECURSE "${test_root}")
