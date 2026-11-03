set(clean_test_root "${PSLOG_BINARY_DIR}/clean-script-test-root")
set(unrelated_file "${clean_test_root}/keep.txt")
set(shared_cache_file "${clean_test_root}/shared-dependency-cache/archive.tar.gz")

file(REMOVE_RECURSE "${clean_test_root}")
file(MAKE_DIRECTORY "${clean_test_root}/build/nested")
file(MAKE_DIRECTORY "${clean_test_root}/dist")
file(MAKE_DIRECTORY "${clean_test_root}/.cache/nested")
file(WRITE "${clean_test_root}/build/.hidden" "hidden\n")
file(WRITE "${clean_test_root}/dist/.hidden" "hidden\n")
file(WRITE "${clean_test_root}/.cache/nested/.hidden" "hidden\n")
file(WRITE "${clean_test_root}/build/nested/build.txt" "build\n")
file(WRITE "${clean_test_root}/dist/artifact.txt" "dist\n")
file(WRITE "${unrelated_file}" "keep\n")
file(MAKE_DIRECTORY "${clean_test_root}/shared-dependency-cache")
file(WRITE "${shared_cache_file}" "verified shared archive\n")

execute_process(
    COMMAND "${PSLOG_ROOT}/scripts/clean.sh" --root "${clean_test_root}"
    RESULT_VARIABLE clean_result
)

if(NOT clean_result EQUAL 0)
    message(FATAL_ERROR "full clean failed for ${clean_test_root}")
endif()

if(EXISTS "${clean_test_root}/build")
    message(FATAL_ERROR "full clean did not remove build/ under ${clean_test_root}")
endif()

if(EXISTS "${clean_test_root}/dist")
    message(FATAL_ERROR "full clean did not remove dist/ under ${clean_test_root}")
endif()
if(EXISTS "${clean_test_root}/.cache")
    message(FATAL_ERROR "full clean did not remove local .cache/ including hidden entries")
endif()

if(NOT EXISTS "${unrelated_file}")
    message(FATAL_ERROR "full clean removed unrelated files under ${clean_test_root}")
endif()
if(NOT EXISTS "${shared_cache_file}")
    message(FATAL_ERROR "full clean removed state outside repository-local generated roots")
endif()

file(MAKE_DIRECTORY "${clean_test_root}/build")
file(MAKE_DIRECTORY "${clean_test_root}/dist")
execute_process(
    COMMAND env -u HOME "${PSLOG_ROOT}/scripts/clean.sh" --root "${clean_test_root}"
    RESULT_VARIABLE home_unset_result
    ERROR_VARIABLE home_unset_error
)
if(NOT home_unset_result EQUAL 0)
    message(FATAL_ERROR
        "full clean failed with HOME unset: ${home_unset_error}")
endif()
if(EXISTS "${clean_test_root}/build" OR EXISTS "${clean_test_root}/dist")
    message(FATAL_ERROR "full clean with HOME unset did not remove generated state")
endif()

file(MAKE_DIRECTORY "${clean_test_root}/build")
file(MAKE_DIRECTORY "${clean_test_root}/dist")
file(WRITE "${clean_test_root}/build/recreated.txt" "build\n")
file(WRITE "${clean_test_root}/dist/recreated.txt" "dist\n")

execute_process(
    COMMAND "${PSLOG_ROOT}/scripts/clean.sh" --dist-only --root "${clean_test_root}"
    RESULT_VARIABLE clean_dist_result
)

if(NOT clean_dist_result EQUAL 0)
    message(FATAL_ERROR "dist-only clean failed for ${clean_test_root}")
endif()

if(NOT EXISTS "${clean_test_root}/build")
    message(FATAL_ERROR "dist-only clean unexpectedly removed build/ under ${clean_test_root}")
endif()

if(EXISTS "${clean_test_root}/dist")
    message(FATAL_ERROR "dist-only clean did not remove dist/ under ${clean_test_root}")
endif()

if(NOT EXISTS "${unrelated_file}")
    message(FATAL_ERROR "dist-only clean removed unrelated files under ${clean_test_root}")
endif()

file(MAKE_DIRECTORY "${clean_test_root}/build")
file(MAKE_DIRECTORY "${clean_test_root}/dist")
file(WRITE "${clean_test_root}/build/from-cmake.txt" "build\n")
file(WRITE "${clean_test_root}/dist/from-cmake.txt" "dist\n")

execute_process(
    COMMAND ${CMAKE_COMMAND}
        -DCLEAN_ROOT=${clean_test_root}
        -P ${PSLOG_ROOT}/cmake/clean.cmake
    RESULT_VARIABLE clean_cmake_result
)

if(NOT clean_cmake_result EQUAL 0)
    message(FATAL_ERROR "cmake clean wrapper failed for ${clean_test_root}")
endif()

if(EXISTS "${clean_test_root}/build")
    message(FATAL_ERROR "cmake clean wrapper did not remove build/ under ${clean_test_root}")
endif()

if(EXISTS "${clean_test_root}/dist")
    message(FATAL_ERROR "cmake clean wrapper did not remove dist/ under ${clean_test_root}")
endif()

if(NOT EXISTS "${unrelated_file}")
    message(FATAL_ERROR "cmake clean wrapper removed unrelated files under ${clean_test_root}")
endif()

execute_process(
    COMMAND "${PSLOG_ROOT}/scripts/clean.sh" --root /tmp
    RESULT_VARIABLE unsafe_root_result
    ERROR_VARIABLE unsafe_root_error
)
if(unsafe_root_result EQUAL 0)
    message(FATAL_ERROR "clean script accepted an unsafe root outside generated state")
endif()
if(NOT unsafe_root_error MATCHES "refusing cleanup root outside")
    message(FATAL_ERROR "clean script did not explain unsafe-root refusal: ${unsafe_root_error}")
endif()

file(CREATE_LINK "${clean_test_root}/absent" "${clean_test_root}/.cache" SYMBOLIC)
execute_process(COMMAND "${PSLOG_ROOT}/scripts/clean.sh" --root "${clean_test_root}"
    RESULT_VARIABLE broken_link_result)
if(NOT broken_link_result EQUAL 0 OR IS_SYMLINK "${clean_test_root}/.cache")
    message(FATAL_ERROR "full clean left a broken link in generated state")
endif()

# A generated path can be a link outside the checkout. Guard rm so this
# negative test cannot delete external state even if root validation regresses.
file(MAKE_DIRECTORY "${clean_test_root}/build" "${clean_test_root}/guard-bin")
file(WRITE "${clean_test_root}/guard-bin/rm"
    "#!/bin/sh\n: > '${clean_test_root}/unsafe-rm'\nexit 91\n")
file(CHMOD "${clean_test_root}/guard-bin/rm"
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
file(CREATE_LINK "/tmp" "${clean_test_root}/build/redirect" SYMBOLIC)
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "PATH=${clean_test_root}/guard-bin:$ENV{PATH}"
        "${PSLOG_ROOT}/scripts/clean.sh" --root "${clean_test_root}/build/redirect"
    RESULT_VARIABLE redirected_result ERROR_VARIABLE redirected_error)
if(redirected_result EQUAL 0 OR EXISTS "${clean_test_root}/unsafe-rm" OR
   NOT redirected_error MATCHES "refusing cleanup root outside")
    message(FATAL_ERROR "clean accepted a root redirected outside the repository: ${redirected_error}")
endif()
file(REMOVE_RECURSE "${clean_test_root}")
