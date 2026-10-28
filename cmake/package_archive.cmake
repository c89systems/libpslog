set(archive_name "libpslog-${PSLOG_VERSION}-${PSLOG_TARGET_ID}")
string(REGEX MATCH "^[0-9]+" PSLOG_VERSION_MAJOR "${PSLOG_VERSION}")
set(package_stage_root "${PSLOG_BINARY_DIR}/package/archive")
set(package_root "${package_stage_root}/${archive_name}")
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
file(MAKE_DIRECTORY "${package_root}/include")
file(MAKE_DIRECTORY "${package_root}/lib")
file(MAKE_DIRECTORY "${package_root}/lib/pkgconfig")
file(MAKE_DIRECTORY "${package_root}/lib/cmake/pslog")
file(MAKE_DIRECTORY "${package_root}/share/doc/libpslog")

file(COPY "${PSLOG_PUBLIC_HEADER}" DESTINATION "${package_root}/include")
file(COPY "${PSLOG_PUBLIC_VERSION_HEADER}" DESTINATION "${package_root}/include")
file(COPY "${PSLOG_SHARED_LIB}" DESTINATION "${package_root}/lib")
file(COPY "${PSLOG_STATIC_LIB}" DESTINATION "${package_root}/lib")

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
            COMMAND "${PSLOG_DISCOVERED_STRIP}" "${strip_static_flag}" "${packaged_static_lib}"
            RESULT_VARIABLE strip_static_result
        )
        if(NOT strip_static_result EQUAL 0)
            message(FATAL_ERROR "failed to strip ${packaged_static_lib}")
        endif()
    endif()
endif()

if(DEFINED PSLOG_SHARED_LINK_NAME
   AND DEFINED PSLOG_SHARED_LIB_NAME
   AND NOT PSLOG_SHARED_LINK_NAME STREQUAL ""
   AND NOT PSLOG_SHARED_LINK_NAME STREQUAL PSLOG_SHARED_LIB_NAME)
    file(CREATE_LINK "${PSLOG_SHARED_LIB_NAME}"
         "${package_root}/lib/${PSLOG_SHARED_LINK_NAME}"
         SYMBOLIC)
endif()

if(DEFINED PSLOG_SHARED_SONAME
   AND DEFINED PSLOG_SHARED_LIB_NAME
   AND NOT PSLOG_SHARED_SONAME STREQUAL ""
   AND NOT PSLOG_SHARED_SONAME STREQUAL PSLOG_SHARED_LIB_NAME)
    file(CREATE_LINK "${PSLOG_SHARED_LIB_NAME}"
         "${package_root}/lib/${PSLOG_SHARED_SONAME}"
         SYMBOLIC)
endif()

file(WRITE "${package_root}/lib/pkgconfig/pslog.pc"
"prefix=\${pcfiledir}/../..
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: pslog
Description: High-performance structured logger for C
Version: ${PSLOG_VERSION}
Libs: -L\${libdir} -lpslog
Libs.private: -pthread
Cflags: -I\${includedir}
")

file(WRITE "${package_root}/lib/cmake/pslog/pslogConfig.cmake"
"include(CMakeFindDependencyMacro)
find_dependency(Threads)

get_filename_component(_PSLOG_PREFIX \"\${CMAKE_CURRENT_LIST_DIR}/../../..\" ABSOLUTE)

if(NOT TARGET pslog::pslog_shared)
    add_library(pslog::pslog_shared UNKNOWN IMPORTED)
    set_target_properties(pslog::pslog_shared PROPERTIES
        IMPORTED_LOCATION \"\${_PSLOG_PREFIX}/lib/${PSLOG_SHARED_LIB_NAME}\"
        INTERFACE_INCLUDE_DIRECTORIES \"\${_PSLOG_PREFIX}/include\"
    )
endif()

if(NOT TARGET pslog::pslog_static)
    add_library(pslog::pslog_static STATIC IMPORTED)
    set_target_properties(pslog::pslog_static PROPERTIES
        IMPORTED_LOCATION \"\${_PSLOG_PREFIX}/lib/${PSLOG_STATIC_LIB_NAME}\"
        INTERFACE_INCLUDE_DIRECTORIES \"\${_PSLOG_PREFIX}/include\"
        INTERFACE_LINK_LIBRARIES Threads::Threads
    )
endif()

if(NOT TARGET pslog::pslog)
    add_library(pslog::pslog INTERFACE IMPORTED)
    set_target_properties(pslog::pslog PROPERTIES
        INTERFACE_LINK_LIBRARIES pslog::pslog_shared
    )
endif()

unset(_PSLOG_PREFIX)
")

file(WRITE "${package_root}/lib/cmake/pslog/pslogConfigVersion.cmake"
"set(PACKAGE_VERSION \"${PSLOG_VERSION}\")

if(PACKAGE_FIND_VERSION STREQUAL PACKAGE_VERSION)
    set(PACKAGE_VERSION_EXACT TRUE)
endif()

if(PACKAGE_FIND_VERSION STREQUAL \"\")
    set(PACKAGE_VERSION_COMPATIBLE TRUE)
elseif(PACKAGE_VERSION VERSION_GREATER_EQUAL PACKAGE_FIND_VERSION
       AND PACKAGE_FIND_VERSION_MAJOR STREQUAL \"${PSLOG_VERSION_MAJOR}\")
    set(PACKAGE_VERSION_COMPATIBLE TRUE)
else()
    set(PACKAGE_VERSION_COMPATIBLE FALSE)
endif()
")

file(COPY "${PSLOG_ROOT}/LICENSE" DESTINATION "${package_root}/share/doc/libpslog")
file(COPY "${PSLOG_ROOT}/README.md" DESTINATION "${package_root}/share/doc/libpslog")

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
