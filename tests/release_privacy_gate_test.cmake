if(NOT DEFINED PSLOG_ROOT)
    message(FATAL_ERROR "PSLOG_ROOT is required")
endif()
if(NOT DEFINED PSLOG_BINARY_DIR)
    message(FATAL_ERROR "PSLOG_BINARY_DIR is required")
endif()

set(test_root "${PSLOG_BINARY_DIR}/release-privacy-gate-test")
set(clean_artifact "${test_root}/clean.rockspec")
set(leaky_artifact "${test_root}/leaky.rockspec")
set(generic_home_artifact "${test_root}/generic-home.rockspec")
set(fake_dylib "${test_root}/libpslog.dylib")
set(fake_otool "${test_root}/fake-otool")
set(manifest_dist "${test_root}/dist")
set(manifest_artifact "${manifest_dist}/manifest-clean.rockspec")
set(manifest_file "${manifest_dist}/libpslog-9.9.9-CHECKSUMS")
set(manifest_stale_artifact "${manifest_dist}/libpslog-1.2.3-x86_64-linux-gnu.tar.gz")
set(manifest_uncompressed_header "${manifest_dist}/pslog-9.9.9.h")
set(manifest_darwin_smoke_zip
    "${manifest_dist}/libpslog-9.9.9-arm64-apple-darwin-smoke-test.zip")
set(missing_manifest_root "${test_root}/missing-manifest-root")
set(missing_manifest_archive "${test_root}/missing-manifest.tar.gz")

file(REMOVE_RECURSE "${test_root}")
file(MAKE_DIRECTORY "${test_root}")
file(MAKE_DIRECTORY "${manifest_dist}")

file(WRITE "${clean_artifact}"
    "package = \"lua-pslog\"\n"
    "source = { url = \"file://lua-pslog-9.9.9.tar.gz\" }\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -DPSLOG_ROOT=${PSLOG_ROOT}
        -DPSLOG_BINARY_DIR=${PSLOG_BINARY_DIR}
        -DPSLOG_VERSION=9.9.9
        -DPSLOG_PRIVACY_PATHS=${clean_artifact}
        -P ${PSLOG_ROOT}/cmake/check_release_privacy.cmake
    RESULT_VARIABLE clean_result
    OUTPUT_VARIABLE clean_output
    ERROR_VARIABLE clean_error
)
if(NOT clean_result EQUAL 0)
    message(FATAL_ERROR
        "release privacy gate rejected clean relative source URL\n"
        "stdout:\n${clean_output}\n"
        "stderr:\n${clean_error}")
endif()

# This fixture ships in the source archive. Its simulated cache paths must
# satisfy the same production privacy scan as the complete source payload.
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -DPSLOG_ROOT=${PSLOG_ROOT}
        -DPSLOG_BINARY_DIR=${PSLOG_BINARY_DIR}
        -DPSLOG_VERSION=9.9.9
        -DPSLOG_PRIVACY_PATHS=${PSLOG_ROOT}/tests/cpkt_toolchains_cache_recovery_test.sh
        -P ${PSLOG_ROOT}/cmake/check_release_privacy.cmake
    RESULT_VARIABLE fixture_result
    OUTPUT_VARIABLE fixture_output
    ERROR_VARIABLE fixture_error
)
if(NOT fixture_result EQUAL 0)
    message(FATAL_ERROR
        "shipped cache fixture failed source privacy verification\n${fixture_output}${fixture_error}")
endif()

file(WRITE "${leaky_artifact}"
    "package = \"lua-pslog\"\n"
    "source = { url = \"git+file://${PSLOG_ROOT}\" }\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -DPSLOG_ROOT=${PSLOG_ROOT}
        -DPSLOG_BINARY_DIR=${PSLOG_BINARY_DIR}
        -DPSLOG_VERSION=9.9.9
        -DPSLOG_PRIVACY_PATHS=${leaky_artifact}
        -P ${PSLOG_ROOT}/cmake/check_release_privacy.cmake
    RESULT_VARIABLE leaky_result
    OUTPUT_VARIABLE leaky_output
    ERROR_VARIABLE leaky_error
)
if(leaky_result EQUAL 0)
    message(FATAL_ERROR "release privacy gate accepted an artifact containing the repo path")
endif()
if(NOT "${leaky_output}${leaky_error}" MATCHES "leaks private path")
    message(FATAL_ERROR
        "release privacy gate failure did not explain the private path leak\n"
        "stdout:\n${leaky_output}\n"
        "stderr:\n${leaky_error}")
endif()

string(CONCAT generic_home_url "file:///" "home/builder/src")
file(WRITE "${generic_home_artifact}"
    "package = \"lua-pslog\"\n"
    "source = { url = \"${generic_home_url}\" }\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -DPSLOG_ROOT=${PSLOG_ROOT}
        -DPSLOG_BINARY_DIR=${PSLOG_BINARY_DIR}
        -DPSLOG_VERSION=9.9.9
        -DPSLOG_PRIVACY_PATHS=${generic_home_artifact}
        -P ${PSLOG_ROOT}/cmake/check_release_privacy.cmake
    RESULT_VARIABLE generic_home_result
    OUTPUT_VARIABLE generic_home_output
    ERROR_VARIABLE generic_home_error
)
if(generic_home_result EQUAL 0)
    message(FATAL_ERROR "release privacy gate accepted a formatted /home path leak")
endif()
if(NOT "${generic_home_output}${generic_home_error}" MATCHES "hard-coded home directory")
    message(FATAL_ERROR
        "release privacy gate failure did not explain the generic home path leak\n"
        "stdout:\n${generic_home_output}\n"
        "stderr:\n${generic_home_error}")
endif()

file(WRITE "${manifest_artifact}"
    "package = \"lua-pslog\"\n"
    "source = { url = \"file://lua-pslog-9.9.9.tar.gz\" }\n")
file(WRITE "${manifest_file}"
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef  manifest-clean.rockspec\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -DPSLOG_ROOT=${PSLOG_ROOT}
        -DPSLOG_BINARY_DIR=${PSLOG_BINARY_DIR}
        -DPSLOG_VERSION=9.9.9
        -DPSLOG_DIST_DIR=${manifest_dist}
        -P ${PSLOG_ROOT}/cmake/check_release_privacy.cmake
    RESULT_VARIABLE manifest_result
    OUTPUT_VARIABLE manifest_output
    ERROR_VARIABLE manifest_error
)
if(NOT manifest_result EQUAL 0)
    message(FATAL_ERROR
        "release privacy gate rejected checksum-manifest-selected artifacts\n"
        "stdout:\n${manifest_output}\n"
        "stderr:\n${manifest_error}")
endif()

file(MAKE_DIRECTORY "${missing_manifest_root}/release-with-bad-manifest")
file(WRITE "${missing_manifest_root}/release-with-bad-manifest/VERSION" "9.9.9\n")
file(WRITE "${missing_manifest_root}/release-with-bad-manifest/RELEASE_MANIFEST"
    "VERSION\n"
    "RELEASE_MANIFEST\n"
    "missing-export-ignored-file.txt\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar cfz "${missing_manifest_archive}" release-with-bad-manifest
    WORKING_DIRECTORY "${missing_manifest_root}"
    RESULT_VARIABLE missing_manifest_archive_result
)
if(NOT missing_manifest_archive_result EQUAL 0)
    message(FATAL_ERROR "failed to create missing-manifest fixture archive")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -DPSLOG_ROOT=${PSLOG_ROOT}
        -DPSLOG_BINARY_DIR=${PSLOG_BINARY_DIR}
        -DPSLOG_VERSION=9.9.9
        -DPSLOG_PRIVACY_PATHS=${missing_manifest_archive}
        -P ${PSLOG_ROOT}/cmake/check_release_privacy.cmake
    RESULT_VARIABLE missing_manifest_result
    OUTPUT_VARIABLE missing_manifest_output
    ERROR_VARIABLE missing_manifest_error
)
if(missing_manifest_result EQUAL 0)
    message(FATAL_ERROR "release privacy gate accepted a RELEASE_MANIFEST with a missing file")
endif()
if(NOT "${missing_manifest_output}${missing_manifest_error}" MATCHES "release manifest lists missing file")
    message(FATAL_ERROR
        "release privacy gate failure did not explain missing manifest entry\n"
        "stdout:\n${missing_manifest_output}\n"
        "stderr:\n${missing_manifest_error}")
endif()

file(WRITE "${manifest_stale_artifact}" "stale release artifact\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -DPSLOG_ROOT=${PSLOG_ROOT}
        -DPSLOG_BINARY_DIR=${PSLOG_BINARY_DIR}
        -DPSLOG_VERSION=9.9.9
        -DPSLOG_DIST_DIR=${manifest_dist}
        -P ${PSLOG_ROOT}/cmake/check_release_privacy.cmake
    RESULT_VARIABLE stale_manifest_result
    OUTPUT_VARIABLE stale_manifest_output
    ERROR_VARIABLE stale_manifest_error
)
if(stale_manifest_result EQUAL 0)
    message(FATAL_ERROR "release privacy gate accepted a stale unlisted release artifact")
endif()
if(NOT "${stale_manifest_output}${stale_manifest_error}" MATCHES "not listed in checksum manifest")
    message(FATAL_ERROR
        "release privacy gate failure did not explain stale artifact rejection\n"
        "stdout:\n${stale_manifest_output}\n"
        "stderr:\n${stale_manifest_error}")
endif()
file(REMOVE "${manifest_stale_artifact}")

file(WRITE "${manifest_uncompressed_header}" "uncompressed single-header artifact\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -DPSLOG_ROOT=${PSLOG_ROOT}
        -DPSLOG_BINARY_DIR=${PSLOG_BINARY_DIR}
        -DPSLOG_VERSION=9.9.9
        -DPSLOG_DIST_DIR=${manifest_dist}
        -P ${PSLOG_ROOT}/cmake/check_release_privacy.cmake
    RESULT_VARIABLE uncompressed_header_result
    OUTPUT_VARIABLE uncompressed_header_output
    ERROR_VARIABLE uncompressed_header_error
)
if(uncompressed_header_result EQUAL 0)
    message(FATAL_ERROR "release privacy gate accepted an unlisted uncompressed single-header artifact")
endif()
if(NOT "${uncompressed_header_output}${uncompressed_header_error}" MATCHES "not listed in checksum manifest")
    message(FATAL_ERROR
        "release privacy gate failure did not explain uncompressed header rejection\n"
        "stdout:\n${uncompressed_header_output}\n"
        "stderr:\n${uncompressed_header_error}")
endif()
file(REMOVE "${manifest_uncompressed_header}")

file(WRITE "${manifest_darwin_smoke_zip}" "darwin smoke fixture\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -DPSLOG_ROOT=${PSLOG_ROOT}
        -DPSLOG_BINARY_DIR=${PSLOG_BINARY_DIR}
        -DPSLOG_VERSION=9.9.9
        -DPSLOG_DIST_DIR=${manifest_dist}
        -P ${PSLOG_ROOT}/cmake/check_release_privacy.cmake
    RESULT_VARIABLE darwin_smoke_result
    OUTPUT_VARIABLE darwin_smoke_output
    ERROR_VARIABLE darwin_smoke_error
)
if(darwin_smoke_result EQUAL 0)
    message(FATAL_ERROR "release privacy gate accepted an unlisted Darwin smoke zip")
endif()
if(NOT "${darwin_smoke_output}${darwin_smoke_error}" MATCHES "not listed in checksum manifest")
    message(FATAL_ERROR
        "release privacy gate failure did not explain Darwin smoke zip rejection\n"
        "stdout:\n${darwin_smoke_output}\n"
        "stderr:\n${darwin_smoke_error}")
endif()
file(REMOVE "${manifest_darwin_smoke_zip}")

file(WRITE "${fake_dylib}" "fake Mach-O dylib fixture\n")
file(WRITE "${fake_otool}" [=[
#!/bin/sh
mode="$1"
if [ -n "${PSLOG_FAKE_OTOOL_FAIL:-}" ]; then
  printf 'fake otool failure\n' >&2
  exit 1
fi
case "$mode" in
  -D)
    printf '%s\n' "$2"
    printf '%s\n' "${PSLOG_FAKE_INSTALL_NAME:-@rpath/libpslog.0.dylib}"
    ;;
  -L)
    printf '%s:\n' "$2"
    printf '\t%s (compatibility version 1.0.0, current version 1.0.0)\n' "${PSLOG_FAKE_DEPENDENCY:-/usr/lib/libSystem.B.dylib}"
    ;;
  -l)
    if [ -n "${PSLOG_FAKE_RPATH:-}" ]; then
      printf 'Load command 0\n'
      printf '          cmd LC_RPATH\n'
      printf '      cmdsize 32\n'
      printf '         path %s (offset 12)\n' "$PSLOG_FAKE_RPATH"
    fi
    ;;
esac
]=])
file(CHMOD "${fake_otool}"
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "PSLOG_FAKE_INSTALL_NAME=@rpath/libpslog.0.dylib"
        "PSLOG_FAKE_DEPENDENCY=/usr/lib/libSystem.B.dylib"
        "PSLOG_FAKE_RPATH=@loader_path"
        "${CMAKE_COMMAND}"
            -DPSLOG_ROOT=${PSLOG_ROOT}
            -DPSLOG_BINARY_DIR=${PSLOG_BINARY_DIR}
            -DPSLOG_VERSION=9.9.9
            -DPSLOG_PRIVACY_PATHS=${fake_dylib}
            -DPSLOG_OTOOL=${fake_otool}
            -P ${PSLOG_ROOT}/cmake/check_release_privacy.cmake
    RESULT_VARIABLE clean_darwin_result
    OUTPUT_VARIABLE clean_darwin_output
    ERROR_VARIABLE clean_darwin_error
)
if(NOT clean_darwin_result EQUAL 0)
    message(FATAL_ERROR
        "release privacy gate rejected clean Darwin loader metadata\n"
        "stdout:\n${clean_darwin_output}\n"
        "stderr:\n${clean_darwin_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "PSLOG_FAKE_INSTALL_NAME=libpslog.dylib"
        "PSLOG_FAKE_DEPENDENCY=/usr/lib/libSystem.B.dylib"
        "${CMAKE_COMMAND}"
            -DPSLOG_ROOT=${PSLOG_ROOT}
            -DPSLOG_BINARY_DIR=${PSLOG_BINARY_DIR}
            -DPSLOG_VERSION=9.9.9
            -DPSLOG_PRIVACY_PATHS=${fake_dylib}
            -DPSLOG_OTOOL=${fake_otool}
            -P ${PSLOG_ROOT}/cmake/check_release_privacy.cmake
    RESULT_VARIABLE bare_install_name_result
    OUTPUT_VARIABLE bare_install_name_output
    ERROR_VARIABLE bare_install_name_error
)
if(bare_install_name_result EQUAL 0)
    message(FATAL_ERROR "release privacy gate accepted a non-@rpath Darwin install name")
endif()
if(NOT "${bare_install_name_output}${bare_install_name_error}" MATCHES "install name is not @rpath-relative")
    message(FATAL_ERROR
        "release privacy gate failure did not explain Darwin install-name failure\n"
        "stdout:\n${bare_install_name_output}\n"
        "stderr:\n${bare_install_name_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "PSLOG_FAKE_INSTALL_NAME=@rpath/libpslog.0.dylib"
        "PSLOG_FAKE_DEPENDENCY=/usr/local/lib/libbad.dylib"
        "${CMAKE_COMMAND}"
            -DPSLOG_ROOT=${PSLOG_ROOT}
            -DPSLOG_BINARY_DIR=${PSLOG_BINARY_DIR}
            -DPSLOG_VERSION=9.9.9
            -DPSLOG_PRIVACY_PATHS=${fake_dylib}
            -DPSLOG_OTOOL=${fake_otool}
            -P ${PSLOG_ROOT}/cmake/check_release_privacy.cmake
    RESULT_VARIABLE local_dependency_result
    OUTPUT_VARIABLE local_dependency_output
    ERROR_VARIABLE local_dependency_error
)
if(local_dependency_result EQUAL 0)
    message(FATAL_ERROR "release privacy gate accepted a non-system Darwin dependency")
endif()
if(NOT "${local_dependency_output}${local_dependency_error}" MATCHES "non-system absolute dependency")
    message(FATAL_ERROR
        "release privacy gate failure did not explain Darwin dependency failure\n"
        "stdout:\n${local_dependency_output}\n"
        "stderr:\n${local_dependency_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "PSLOG_FAKE_INSTALL_NAME=@rpath/libpslog.0.dylib"
        "PSLOG_FAKE_DEPENDENCY=/usr/lib/libSystem.B.dylib"
        "PSLOG_FAKE_RPATH=/tmp/build/lib"
        "${CMAKE_COMMAND}"
            -DPSLOG_ROOT=${PSLOG_ROOT}
            -DPSLOG_BINARY_DIR=${PSLOG_BINARY_DIR}
            -DPSLOG_VERSION=9.9.9
            -DPSLOG_PRIVACY_PATHS=${fake_dylib}
            -DPSLOG_OTOOL=${fake_otool}
            -P ${PSLOG_ROOT}/cmake/check_release_privacy.cmake
    RESULT_VARIABLE absolute_rpath_result
    OUTPUT_VARIABLE absolute_rpath_output
    ERROR_VARIABLE absolute_rpath_error
)
if(absolute_rpath_result EQUAL 0)
    message(FATAL_ERROR "release privacy gate accepted a non-relocatable Darwin rpath")
endif()
if(NOT "${absolute_rpath_output}${absolute_rpath_error}" MATCHES "non-relocatable LC_RPATH")
    message(FATAL_ERROR
        "release privacy gate failure did not explain Darwin rpath failure\n"
        "stdout:\n${absolute_rpath_output}\n"
        "stderr:\n${absolute_rpath_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "PSLOG_FAKE_OTOOL_FAIL=1"
        "${CMAKE_COMMAND}"
            -DPSLOG_ROOT=${PSLOG_ROOT}
            -DPSLOG_BINARY_DIR=${PSLOG_BINARY_DIR}
            -DPSLOG_VERSION=9.9.9
            -DPSLOG_PRIVACY_PATHS=${fake_dylib}
            -DPSLOG_OTOOL=${fake_otool}
            -P ${PSLOG_ROOT}/cmake/check_release_privacy.cmake
    RESULT_VARIABLE otool_failure_result
    OUTPUT_VARIABLE otool_failure_output
    ERROR_VARIABLE otool_failure_error
)
if(otool_failure_result EQUAL 0)
    message(FATAL_ERROR "release privacy gate accepted a Darwin artifact when otool failed")
endif()
if(NOT "${otool_failure_output}${otool_failure_error}" MATCHES "failed to inspect Darwin load commands")
    message(FATAL_ERROR
        "release privacy gate failure did not explain Darwin otool failure\n"
        "stdout:\n${otool_failure_output}\n"
        "stderr:\n${otool_failure_error}")
endif()
