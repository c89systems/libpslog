if(NOT DEFINED PSLOG_ROOT)
    message(FATAL_ERROR "PSLOG_ROOT is required")
endif()
if(NOT DEFINED PSLOG_BINARY_DIR)
    message(FATAL_ERROR "PSLOG_BINARY_DIR is required")
endif()
if(NOT DEFINED PSLOG_VERSION)
    message(FATAL_ERROR "PSLOG_VERSION is required")
endif()

set(dist_dir "${PSLOG_ROOT}/dist")
set(source_name "libpslog-${PSLOG_VERSION}")
set(stage_root "${PSLOG_BINARY_DIR}/package/source")
set(source_root "${stage_root}/${source_name}")
set(stage_archive "${stage_root}/${source_name}.stage.tar")
set(source_tar "${dist_dir}/${source_name}.tar")
set(source_archive "${source_tar}.gz")

find_program(PSLOG_GIT_BIN NAMES git)
find_program(PSLOG_TAR_BIN NAMES tar)
find_program(PSLOG_GZIP_BIN NAMES gzip)
if(NOT PSLOG_GIT_BIN)
    message(FATAL_ERROR "failed to find git for source archive generation")
endif()
if(NOT PSLOG_TAR_BIN)
    message(FATAL_ERROR "failed to find tar for source archive generation")
endif()
if(NOT PSLOG_GZIP_BIN)
    message(FATAL_ERROR "failed to find gzip for source archive generation")
endif()

execute_process(
    COMMAND "${PSLOG_GIT_BIN}" status --porcelain --untracked-files=normal
    WORKING_DIRECTORY "${PSLOG_ROOT}"
    RESULT_VARIABLE status_result
    OUTPUT_VARIABLE status_output
    ERROR_VARIABLE status_error
)
if(NOT status_result EQUAL 0)
    message(FATAL_ERROR "failed to inspect source archive worktree state\n${status_error}")
endif()
if(NOT status_output STREQUAL "")
    message(FATAL_ERROR
        "source archive generation requires a clean committed worktree; commit or remove local changes first")
endif()

file(REMOVE_RECURSE "${stage_root}")
file(MAKE_DIRECTORY "${source_root}")

execute_process(
    COMMAND "${PSLOG_GIT_BIN}" archive --format=tar --worktree-attributes -o "${stage_archive}" HEAD
    WORKING_DIRECTORY "${PSLOG_ROOT}"
    RESULT_VARIABLE archive_result
    ERROR_VARIABLE archive_error
)
if(NOT archive_result EQUAL 0)
    message(FATAL_ERROR "failed to create source archive payload from HEAD\n${archive_error}")
endif()

execute_process(
    COMMAND "${PSLOG_TAR_BIN}" -tf "${stage_archive}"
    RESULT_VARIABLE manifest_result
    OUTPUT_VARIABLE manifest_output
    ERROR_VARIABLE manifest_error
)
if(NOT manifest_result EQUAL 0)
    message(FATAL_ERROR "failed to collect source archive manifest\n${manifest_error}")
endif()

execute_process(
    COMMAND "${PSLOG_TAR_BIN}" -xf "${stage_archive}" -C "${source_root}"
    RESULT_VARIABLE extract_result
    ERROR_VARIABLE extract_error
)
if(NOT extract_result EQUAL 0)
    message(FATAL_ERROR "failed to stage source archive from HEAD\n${extract_error}")
endif()

file(REMOVE "${source_root}/REVIEW.md")

string(STRIP "${manifest_output}" manifest_output)
string(REPLACE "\n" ";" manifest_entries "${manifest_output}")
list(SORT manifest_entries)
list(LENGTH manifest_entries manifest_count)
if(manifest_count EQUAL 0)
    message(FATAL_ERROR "source archive manifest is empty")
endif()

set(release_manifest "")
foreach(entry IN LISTS manifest_entries)
    if(entry MATCHES "/$")
        continue()
    endif()
    if(entry STREQUAL "REVIEW.md")
        continue()
    endif()
    if(entry MATCHES "^/" OR entry MATCHES "(^|/)\\.git(/|$)" OR entry MATCHES "\\.\\.")
        message(FATAL_ERROR "source archive manifest contains unsafe path: ${entry}")
    endif()
    if(entry MATCHES "^(build|dist|\\.cache)/")
        message(FATAL_ERROR "source archive manifest contains generated path: ${entry}")
    endif()
    string(APPEND release_manifest "${entry}\n")
endforeach()

string(APPEND release_manifest "VERSION\n")
string(APPEND release_manifest "RELEASE_MANIFEST\n")
file(WRITE "${source_root}/VERSION" "${PSLOG_VERSION}\n")
file(WRITE "${source_root}/RELEASE_MANIFEST" "${release_manifest}")

file(MAKE_DIRECTORY "${dist_dir}")
file(REMOVE "${source_tar}" "${source_archive}")
execute_process(
    COMMAND "${PSLOG_TAR_BIN}" -cf "${source_tar}" --format=gnu
        --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner
        --mode=u=rwX,go=rX "${source_name}"
    WORKING_DIRECTORY "${stage_root}"
    RESULT_VARIABLE tar_result
)
if(NOT tar_result EQUAL 0)
    message(FATAL_ERROR "failed to create source archive")
endif()

execute_process(
    COMMAND "${PSLOG_GZIP_BIN}" -n -9 -f "${source_tar}"
    RESULT_VARIABLE gzip_result
)
if(NOT gzip_result EQUAL 0)
    message(FATAL_ERROR "failed to gzip source archive")
endif()
