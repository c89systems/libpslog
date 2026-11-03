set(root "${PSLOG_BINARY_DIR}/package-checksums-test")
file(REMOVE_RECURSE "${root}")
file(MAKE_DIRECTORY "${root}/scripts" "${root}/dist" "${root}/bin")
file(COPY "${PSLOG_ROOT}/scripts/sha256_files.sh" DESTINATION "${root}/scripts")
find_program(host_sh sh REQUIRED)
file(CREATE_LINK "${host_sh}" "${root}/bin/sh" SYMBOLIC)
# Emulate the stock macOS hash interface using CMake's portable hash command.
# No GNU sha256sum is present on this fixture's PATH.
file(WRITE "${root}/bin/shasum"
    "#!/bin/sh\n[ \"$1\" = -a ] && [ \"$2\" = 256 ] || exit 91\nshift 2\n"
    ": > '${root}/fallback-used'\nexec '${CMAKE_COMMAND}' -E sha256sum \"$@\"\n")
file(CHMOD "${root}/bin/shasum" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
set(name "libpslog-1.2.3-x86_64-linux-gnu.tar.gz")
file(WRITE "${root}/dist/${name}" "checksum payload\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "PATH=${root}/bin"
        "${CMAKE_COMMAND}" -DPSLOG_ROOT=${root} -DPSLOG_VERSION=1.2.3
        -P "${PSLOG_ROOT}/cmake/package_checksums.cmake"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0 OR NOT EXISTS "${root}/fallback-used")
    message(FATAL_ERROR "checksum generation failed without GNU tools: ${output}${error}")
endif()
file(SHA256 "${root}/dist/${name}" digest)
file(READ "${root}/dist/libpslog-1.2.3-CHECKSUMS" manifest)
if(NOT manifest STREQUAL "${digest}  ${name}\n")
    message(FATAL_ERROR "checksum manifest has the wrong payload identity or inventory: ${manifest}")
endif()
file(REMOVE "${root}/bin/shasum")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "PATH=${root}/bin"
        "${CMAKE_COMMAND}" -DPSLOG_ROOT=${root} -DPSLOG_VERSION=1.2.3
        -P "${PSLOG_ROOT}/cmake/package_checksums.cmake"
    RESULT_VARIABLE missing_result OUTPUT_VARIABLE missing_output ERROR_VARIABLE missing_error)
if(missing_result EQUAL 0 OR NOT missing_error MATCHES "need sha256sum or shasum")
    message(FATAL_ERROR "missing hash tools did not fail clearly: ${missing_output}${missing_error}")
endif()
file(REMOVE_RECURSE "${root}")
