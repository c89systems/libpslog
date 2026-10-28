if(NOT DEFINED PSLOG_BINARY_DIR)
    message(FATAL_ERROR "PSLOG_BINARY_DIR is required")
endif()
if(NOT DEFINED PSLOG_ROOT)
    message(FATAL_ERROR "PSLOG_ROOT is required")
endif()

set(test_root "${PSLOG_BINARY_DIR}/darwin-linker-route-test")
set(fake_osxcross_root "${test_root}/osxcross")
set(fake_bin "${fake_osxcross_root}/bin")
set(fake_host "arm64-apple-darwin25.4")
set(fake_ld "${fake_bin}/${fake_host}-ld")

file(REMOVE_RECURSE "${test_root}")
file(MAKE_DIRECTORY "${fake_bin}")
file(MAKE_DIRECTORY "${fake_osxcross_root}/SDK/MacOSX15.sdk/usr/include")

foreach(tool clang clang++ ar ranlib ld install_name_tool strip nm otool)
    set(tool_path "${fake_bin}/${fake_host}-${tool}")
    if(tool STREQUAL "clang")
        file(WRITE "${tool_path}"
"#!/bin/sh
expected='--ld-path=${fake_ld}'
output=''
found=0
previous=''
for arg in \"$@\"; do
  if [ \"$previous\" = '-o' ]; then
    output=\"$arg\"
  fi
  if [ \"$arg\" = \"$expected\" ]; then
    found=1
  fi
  previous=\"$arg\"
done
if [ \"$found\" -ne 1 ]; then
  printf 'selected host linker: /usr/bin/ld\n' >&2
  exit 42
fi
if [ -n \"$output\" ]; then
  : >\"$output\"
fi
exit 0
")
    else()
        file(WRITE "${tool_path}" "#!/bin/sh\nexit 0\n")
    endif()
    file(CHMOD "${tool_path}"
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
endforeach()
file(WRITE "${fake_bin}/${fake_host}-cmake-clang" "#!/bin/sh\nexit 0\n")
file(CHMOD "${fake_bin}/${fake_host}-cmake-clang"
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
# Newer, otherwise complete installation: only install_name_tool is missing.
# It must not displace the usable 25.4 collection.
set(newer_host "arm64-apple-darwin25.5")
foreach(tool clang clang++ ar ranlib ld strip nm otool)
    file(WRITE "${fake_bin}/${newer_host}-${tool}" "#!/bin/sh\nexit 0\n")
    file(CHMOD "${fake_bin}/${newer_host}-${tool}"
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
endforeach()

set(ENV{OSXCROSS_ROOT} "${fake_osxcross_root}")
set(ENV{CPKT_OSXCROSS_HOST} "")
# Own every resolver input: a populated workstation MIG cache must not decide
# whether this synthetic osxcross collection can exercise the linker route.
set(ENV{CPKT_TOOLCHAIN_CACHE} "${test_root}/toolchains")
execute_process(
    COMMAND "${PSLOG_ROOT}/scripts/cpkt-toolchains.sh" discover arm64-apple-darwin
    RESULT_VARIABLE discovery_result
    OUTPUT_VARIABLE discovery_output
    ERROR_VARIABLE discovery_error)
if(NOT discovery_result EQUAL 0 OR NOT discovery_output MATCHES "status=missing" OR
   NOT discovery_output MATCHES "mig_revision=([0-9a-f]+)")
    message(FATAL_ERROR "expected a complete osxcross collection awaiting MIG: ${discovery_output}${discovery_error}")
endif()
set(fake_mig_root "$ENV{CPKT_TOOLCHAIN_CACHE}/roots/host-mig-puredarwin-${CMAKE_MATCH_1}-x86_64-linux-gnu")
file(MAKE_DIRECTORY "${fake_mig_root}/bin" "${fake_mig_root}/libexec")
foreach(tool bin/mig bin/mig-upstream libexec/migcom)
    file(WRITE "${fake_mig_root}/${tool}" "#!/bin/sh\nexit 0\n")
    file(CHMOD "${fake_mig_root}/${tool}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
endforeach()
file(WRITE "${fake_mig_root}/TOOLCHAIN" "fixture host MIG\n")
set(PSLOG_OSXCROSS_HOST "${fake_host}-cmake" CACHE STRING "" FORCE)
set(CMAKE_AR "${fake_bin}/${fake_host}-cmake-ar" CACHE FILEPATH "" FORCE)
include("${PSLOG_ROOT}/cmake/toolchains/arm64-apple-darwin.cmake")

if(NOT PSLOG_OSXCROSS_HOST STREQUAL "${fake_host}" OR
   NOT CMAKE_AR STREQUAL "${fake_bin}/${fake_host}-ar")
    message(FATAL_ERROR
        "Darwin autodetection selected a helper alias, incomplete prefix, or stale cache: "
        "${PSLOG_OSXCROSS_HOST}, ${CMAKE_AR}")
endif()

if(NOT CMAKE_LINKER STREQUAL "${fake_ld}")
    message(FATAL_ERROR
        "Darwin toolchain did not select target ld\n"
        "expected: ${fake_ld}\n"
        "actual: ${CMAKE_LINKER}")
endif()
if(NOT CMAKE_CXX_COMPILER STREQUAL "${fake_bin}/${fake_host}-clang++" OR
   NOT CMAKE_NM STREQUAL "${fake_bin}/${fake_host}-nm")
    message(FATAL_ERROR "Darwin C++ and symbol tools must come from the selected osxcross collection")
endif()
if(NOT CPKT_OTOOL STREQUAL "${fake_bin}/${fake_host}-otool")
    message(FATAL_ERROR
        "Darwin toolchain did not select target otool\n"
        "expected: ${fake_bin}/${fake_host}-otool\n"
        "actual: ${CPKT_OTOOL}")
endif()

foreach(linker_flags
        CMAKE_EXE_LINKER_FLAGS
        CMAKE_SHARED_LINKER_FLAGS
        CMAKE_MODULE_LINKER_FLAGS)
    if(NOT "${${linker_flags}}" MATCHES "--ld-path=${fake_ld}")
        message(FATAL_ERROR
            "Darwin toolchain did not force ${linker_flags} through target ld: ${${linker_flags}}")
    endif()
    if("${${linker_flags}}" MATCHES "-fuse-ld=")
        message(FATAL_ERROR
            "Darwin toolchain still uses deprecated absolute -fuse-ld: ${${linker_flags}}")
    endif()
endforeach()

string(FIND "$ENV{PATH}" "${fake_bin}" fake_bin_offset)
if(NOT fake_bin_offset EQUAL 0)
    message(FATAL_ERROR
        "Darwin toolchain did not prepend osxcross bin to PATH\n"
        "PATH=$ENV{PATH}")
endif()

set(probe_source "${test_root}/probe.c")
set(probe_binary "${test_root}/probe")
file(WRITE "${probe_source}" "int main(void) { return 0; }\n")

execute_process(
    COMMAND "${CMAKE_C_COMPILER}" "${probe_source}" -o "${probe_binary}"
    RESULT_VARIABLE unfixed_result
    ERROR_VARIABLE unfixed_error
)
if(unfixed_result EQUAL 0)
    message(FATAL_ERROR "Darwin linker-route regression did not prove the unfixed route fails")
endif()
if(NOT unfixed_error MATCHES "selected host linker")
    message(FATAL_ERROR
        "Darwin linker-route regression did not explain the unfixed host-linker route\n"
        "${unfixed_error}")
endif()

separate_arguments(pslog_darwin_exe_linker_flags NATIVE_COMMAND "${CMAKE_EXE_LINKER_FLAGS}")
execute_process(
    COMMAND "${CMAKE_C_COMPILER}" ${pslog_darwin_exe_linker_flags} "${probe_source}" -o "${probe_binary}"
    RESULT_VARIABLE fixed_result
    ERROR_VARIABLE fixed_error
)
if(NOT fixed_result EQUAL 0)
    message(FATAL_ERROR
        "Darwin linker-route regression did not route through target ld\n"
        "${fixed_error}")
endif()

function(assert_darwin_discovery expected_prefix)
    execute_process(
        COMMAND "${PSLOG_ROOT}/scripts/cpkt-toolchains.sh" discover arm64-apple-darwin
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Darwin discovery failed: ${output}${error}")
    endif()
    if(expected_prefix STREQUAL "")
        if(NOT output MATCHES "(^|\n)status=missing(\n|$)" OR output MATCHES "(^|\n)prefix=")
            message(FATAL_ERROR "discovery accepted an incomplete collection: ${output}")
        endif()
        execute_process(
            COMMAND "${PSLOG_ROOT}/scripts/cpkt-toolchains.sh" env arm64-apple-darwin
            RESULT_VARIABLE env_result OUTPUT_VARIABLE env_output ERROR_VARIABLE env_error)
        if(env_result EQUAL 0 OR NOT env_output STREQUAL "")
            message(FATAL_ERROR "environment query exported an incomplete collection: ${env_output}${env_error}")
        endif()
    else()
        string(REGEX MATCH "(^|\n)prefix=([^\n]+)" match "${output}")
        if(NOT CMAKE_MATCH_2 STREQUAL expected_prefix OR
           NOT output MATCHES "(^|\n)status=ready(\n|$)")
            message(FATAL_ERROR "discovery did not select ${expected_prefix}: ${output}")
        endif()
    endif()
endfunction()

# Once complete, the newer prefix wins. Removing any required executable must
# restore the older candidate; explicitly pinning the broken prefix must fail.
file(WRITE "${fake_bin}/${newer_host}-install_name_tool" "#!/bin/sh\nexit 0\n")
file(CHMOD "${fake_bin}/${newer_host}-install_name_tool"
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
assert_darwin_discovery("${newer_host}")
foreach(tool clang clang++ ar ranlib ld install_name_tool strip nm otool)
    file(RENAME "${fake_bin}/${newer_host}-${tool}" "${fake_bin}/${newer_host}-${tool}.saved")
    assert_darwin_discovery("${fake_host}")
    set(ENV{CPKT_OSXCROSS_HOST} "${newer_host}")
    assert_darwin_discovery("")
    set(ENV{CPKT_OSXCROSS_HOST} "")
    file(RENAME "${fake_bin}/${newer_host}-${tool}.saved" "${fake_bin}/${newer_host}-${tool}")
endforeach()
file(CHMOD "${fake_bin}/${newer_host}-install_name_tool" PERMISSIONS OWNER_READ OWNER_WRITE)
assert_darwin_discovery("${fake_host}")
file(CHMOD "${fake_bin}/${newer_host}-install_name_tool"
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)

file(RENAME "${fake_osxcross_root}/SDK" "${fake_osxcross_root}/SDK.saved")
assert_darwin_discovery("")
file(RENAME "${fake_osxcross_root}/SDK.saved" "${fake_osxcross_root}/SDK")
file(RENAME "${fake_osxcross_root}/SDK/MacOSX15.sdk/usr/include"
    "${fake_osxcross_root}/SDK/MacOSX15.sdk/usr/include.saved")
assert_darwin_discovery("")
file(RENAME "${fake_osxcross_root}/SDK/MacOSX15.sdk/usr/include.saved"
    "${fake_osxcross_root}/SDK/MacOSX15.sdk/usr/include")
# Match CMake even when a newer SDK directory is present but incomplete.
file(MAKE_DIRECTORY "${fake_osxcross_root}/SDK/MacOSX16.sdk")
assert_darwin_discovery("")
file(REMOVE_RECURSE "${fake_osxcross_root}/SDK/MacOSX16.sdk")
assert_darwin_discovery("${newer_host}")
