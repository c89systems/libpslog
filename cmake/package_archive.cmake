set(archive_name "libpslog-${PSLOG_VERSION}-${PSLOG_TARGET_ID}")
set(package_stage_root "${PSLOG_BINARY_DIR}/package/archive")
set(package_root "${package_stage_root}/${archive_name}")
if(DEFINED ENV{DESTDIR} AND NOT "$ENV{DESTDIR}" STREQUAL "")
    message(FATAL_ERROR "package generation requires DESTDIR unset; the archive owns its installation prefix")
endif()
if(NOT DEFINED PSLOG_TARGET_TOOL_DISCOVERY OR PSLOG_TARGET_TOOL_DISCOVERY STREQUAL "")
    message(FATAL_ERROR "PSLOG_TARGET_TOOL_DISCOVERY is required for package generation")
endif()
execute_process(
    COMMAND "${PSLOG_TARGET_TOOL_DISCOVERY}"
        --build-dir "${PSLOG_BINARY_DIR}"
        --target-id "${PSLOG_TARGET_ID}"
        --format cmake
    RESULT_VARIABLE tool_discovery_result
    OUTPUT_VARIABLE tool_discovery_output
    ERROR_VARIABLE tool_discovery_error
)
if(NOT tool_discovery_result EQUAL 0)
    message(FATAL_ERROR
        "target tool discovery failed for ${PSLOG_TARGET_ID}:\n${tool_discovery_error}")
endif()
cmake_language(EVAL CODE "${tool_discovery_output}")
file(REMOVE_RECURSE "${package_stage_root}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${PSLOG_BINARY_DIR}" --prefix "${package_root}"
    RESULT_VARIABLE install_result)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR "failed to stage the public SDK through CMake install")
endif()

set(packaged_shared_lib "${package_root}/lib/${PSLOG_SHARED_LIB_NAME}")
set(packaged_static_lib "${package_root}/lib/${PSLOG_STATIC_LIB_NAME}")
if(DEFINED PSLOG_DISCOVERED_STRIP AND NOT PSLOG_DISCOVERED_STRIP STREQUAL "")
    if(NOT PSLOG_TARGET_ID MATCHES "darwin")
        set(strip_shared_flag "--strip-unneeded")
        set(strip_static_flag "--strip-debug")

        execute_process(
            COMMAND "${PSLOG_DISCOVERED_STRIP}" "${strip_shared_flag}" "${packaged_shared_lib}"
            RESULT_VARIABLE strip_shared_result
        )
        if(NOT strip_shared_result EQUAL 0)
            message(FATAL_ERROR "failed to strip ${packaged_shared_lib}")
        endif()
        execute_process(
            COMMAND "${PSLOG_DISCOVERED_STRIP}" --enable-deterministic-archives
                "${strip_static_flag}" "${packaged_static_lib}"
            RESULT_VARIABLE strip_static_result
        )
        if(NOT strip_static_result EQUAL 0)
            message(FATAL_ERROR "failed to strip ${packaged_static_lib}")
        endif()
    endif()
endif()

file(MAKE_DIRECTORY "${PSLOG_ROOT}/dist")
set(archive_base "${PSLOG_ROOT}/dist/${archive_name}.tar")
set(archive "${archive_base}.gz")

find_program(PSLOG_TAR_BIN NAMES tar)
find_program(PSLOG_GZIP_BIN NAMES gzip)
if(NOT PSLOG_TAR_BIN)
    message(FATAL_ERROR "failed to find tar for archive creation")
endif()
if(NOT PSLOG_GZIP_BIN)
    message(FATAL_ERROR "failed to find gzip for archive creation")
endif()

file(REMOVE "${archive_base}" "${archive}")
execute_process(
    COMMAND "${PSLOG_TAR_BIN}" -cf "${archive_base}" --format=gnu
        --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner
        --mode=u=rwX,go=rX "${archive_name}"
    WORKING_DIRECTORY "${package_stage_root}"
    RESULT_VARIABLE tar_result
)
if(NOT tar_result EQUAL 0)
    message(FATAL_ERROR "failed to create package tar archive")
endif()

file(REMOVE "${archive}")
execute_process(
    COMMAND "${PSLOG_GZIP_BIN}" -n -9 -f "${archive_base}"
    RESULT_VARIABLE gzip_result
)
if(NOT gzip_result EQUAL 0)
    message(FATAL_ERROR "failed to gzip package archive")
endif()
