if(NOT DEFINED PSLOG_BINARY_DIR OR NOT DEFINED PSLOG_ROOT)
    message(FATAL_ERROR "PSLOG_BINARY_DIR and PSLOG_ROOT are required")
endif()

set(test_root "${PSLOG_BINARY_DIR}/dependency-cache-test")
file(REMOVE_RECURSE "${test_root}")
file(MAKE_DIRECTORY "${test_root}")
unset(CPKT_DEPENDENCY_CACHE)
unset(CPKT_DEPENDENCY_CACHE CACHE)
set(ENV{CPKT_DEPENDENCY_CACHE} "")
set(ENV{XDG_CACHE_HOME} "")
set(ENV{HOME} "")
include("${PSLOG_ROOT}/cmake/pslog_dependencies.cmake")
if(DEFINED CPKT_DEPENDENCY_CACHE AND NOT CPKT_DEPENDENCY_CACHE STREQUAL "")
    message(FATAL_ERROR "dependency cache was resolved before an archive was requested")
endif()
set(CPKT_DEPENDENCY_CACHE "${test_root}/shared-cache")
set(PSLOG_DEPENDENCY_TEST_ALLOW_FILE_URL TRUE)
set(source_archive "${test_root}/fixture.tar.gz")
file(WRITE "${source_archive}" "verified fixture archive\n")
file(SHA256 "${source_archive}" fixture_sha256)
file(READ "${PSLOG_ROOT}/cmake/pslog_dependencies.cmake" dependency_helper)
if(NOT dependency_helper MATCHES "file\\(LOCK[ \\t]+\"\\$\\{lock_path\\}\"")
    message(FATAL_ERROR "dependency cache helper does not serialize archive writers per digest")
endif()
if(NOT dependency_helper MATCHES "file\\(RENAME[ \\t]+\"\\$\\{temp_path\\}\"[ \\t]+\"\\$\\{archive_path\\}\"")
    message(FATAL_ERROR "dependency cache helper does not atomically publish verified archives")
endif()
if(dependency_helper MATCHES "EXPECTED_HASH" OR
   NOT dependency_helper MATCHES "foreach\\(download_attempt[ \\t]+RANGE[ \\t]+1[ \\t]+3\\)" OR
   NOT dependency_helper MATCHES "file\\(SHA256[ \\t]+\"\\$\\{temp_path\\}\"[ \\t]+downloaded_digest\\)")
    message(FATAL_ERROR "dependency downloads must retry and explicitly verify temporary archives before publication")
endif()
if(dependency_helper MATCHES "\\$\\{CMAKE_SOURCE_DIR\\}/\\.cache/deps")
    message(FATAL_ERROR "dependency staging must not write extracted state or locks under the source tree")
endif()
if(NOT dependency_helper MATCHES "set\\(stage_root[ \\t]+\"\\$\\{CMAKE_BINARY_DIR\\}/dependency-sources/" OR
   NOT dependency_helper MATCHES "set\\(stage_lock_path[ \\t]+\"\\$\\{stage_lock_dir\\}/" OR
   NOT dependency_helper MATCHES "file\\(LOCK[ \\t]+\"\\$\\{stage_lock_path\\}\"[ \\t]+GUARD[ \\t]+FUNCTION" OR
   NOT dependency_helper MATCHES "file\\(SHA256[ \\t]+\"\\$\\{ARG_ARCHIVE\\}\"[ \\t]+staged_archive_digest")
    message(FATAL_ERROR "dependency staging does not use build-tree staging with a per-stage lock through extraction")
endif()
pslog_acquire_verified_archive(
    COMPONENT fixture URL "file://${source_archive}" SHA256 "${fixture_sha256}" OUTPUT first_archive)
if(NOT EXISTS "${first_archive}")
    message(FATAL_ERROR "cache miss did not publish an archive")
endif()
file(SHA256 "${first_archive}" first_digest)
if(NOT first_digest STREQUAL fixture_sha256)
    message(FATAL_ERROR "published archive is not checksum verified")
endif()

file(REMOVE "${source_archive}")
pslog_acquire_verified_archive(
    COMPONENT fixture URL "file://${source_archive}" SHA256 "${fixture_sha256}" OUTPUT offline_archive)
if(NOT offline_archive STREQUAL first_archive)
    message(FATAL_ERROR "offline cache hit did not reuse the verified archive")
endif()
pslog_acquire_verified_archive(
    COMPONENT fixture URL "file://${test_root}/renamed.tar.gz"
    SHA256 "${fixture_sha256}" OUTPUT renamed_archive)
if(NOT renamed_archive STREQUAL first_archive)
    message(FATAL_ERROR "renamed asset did not reuse the digest-verified offline archive")
endif()

file(WRITE "${first_archive}" "corrupt cache entry\n")
file(WRITE "${source_archive}" "verified fixture archive\n")
pslog_acquire_verified_archive(
    COMPONENT fixture URL "file://${source_archive}" SHA256 "${fixture_sha256}" OUTPUT repaired_archive)
file(SHA256 "${repaired_archive}" repaired_digest)
if(NOT repaired_digest STREQUAL fixture_sha256)
    message(FATAL_ERROR "corrupt cache entry was not rejected and repaired")
endif()

set(stage_source_dir "${test_root}/stage-fixture-src")
file(MAKE_DIRECTORY "${stage_source_dir}")
file(WRITE "${stage_source_dir}/CMakeLists.txt" "cmake_minimum_required(VERSION 3.21)\nproject(stage_fixture C)\n")
file(WRITE "${stage_source_dir}/fixture.c" "int fixture(void) { return 0; }\n")
set(stage_source_archive "${test_root}/stage-fixture.tar")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar cf "${stage_source_archive}" .
    WORKING_DIRECTORY "${stage_source_dir}"
    RESULT_VARIABLE stage_archive_result
    OUTPUT_VARIABLE stage_archive_output
    ERROR_VARIABLE stage_archive_error
)
if(NOT stage_archive_result EQUAL 0)
    message(FATAL_ERROR
        "failed to create staging fixture archive\n"
        "stdout:\n${stage_archive_output}\n"
        "stderr:\n${stage_archive_error}")
endif()
file(SHA256 "${stage_source_archive}" stage_source_sha256)
set(CMAKE_SOURCE_DIR "${test_root}/readonly-source-contract")
set(CMAKE_BINARY_DIR "${test_root}/build-tree-contract")
pslog_stage_dependency_archive(
    COMPONENT stage-fixture ARCHIVE "${stage_source_archive}" SHA256 "${stage_source_sha256}" OUTPUT staged_source_dir)
if(NOT staged_source_dir MATCHES "^${CMAKE_BINARY_DIR}/dependency-sources/")
    message(FATAL_ERROR "dependency staging did not return a build-tree source directory: ${staged_source_dir}")
endif()
if(NOT EXISTS "${staged_source_dir}/CMakeLists.txt")
    message(FATAL_ERROR "dependency staging did not extract the fixture source tree")
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/.cache/deps")
    message(FATAL_ERROR "dependency staging created source-tree disposable state: ${CMAKE_SOURCE_DIR}/.cache/deps")
endif()

set(stage_failure_script "${test_root}/stage-failure.cmake")
file(WRITE "${stage_failure_script}"
"set(CMAKE_SOURCE_DIR \"${test_root}/stage-project\")\n"
"set(CMAKE_BINARY_DIR \"${test_root}/stage-build\")\n"
"include(\"${PSLOG_ROOT}/cmake/pslog_dependencies.cmake\")\n"
"pslog_stage_dependency_archive(COMPONENT fixture ARCHIVE \"${repaired_archive}\" SHA256 \"0000000000000000000000000000000000000000000000000000000000000000\" OUTPUT staged)\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -P "${stage_failure_script}"
    RESULT_VARIABLE stage_failure_result
    OUTPUT_VARIABLE stage_failure_output
    ERROR_VARIABLE stage_failure_error
)
if(stage_failure_result EQUAL 0 OR
   NOT stage_failure_error MATCHES "dependency archive checksum mismatch before staging")
    message(FATAL_ERROR
        "dependency staging accepted a mismatched archive\n"
        "stdout:\n${stage_failure_output}\n"
        "stderr:\n${stage_failure_error}")
endif()
file(REMOVE_RECURSE "${test_root}")
