set(dist_dir "${PSLOG_ROOT}/dist")
set(checksums_name "libpslog-${PSLOG_VERSION}-CHECKSUMS")
set(checksums_path "${dist_dir}/${checksums_name}")

file(MAKE_DIRECTORY "${dist_dir}")
file(GLOB release_entries RELATIVE "${dist_dir}"
    "${dist_dir}/libpslog-${PSLOG_VERSION}.tar.gz"
    "${dist_dir}/libpslog-${PSLOG_VERSION}-*"
    "${dist_dir}/lua-pslog-${PSLOG_VERSION}.tar.gz"
    "${dist_dir}/lua-pslog-${PSLOG_VERSION}-*.rockspec"
    "${dist_dir}/lua-pslog-${PSLOG_VERSION}-*.src.rock"
    "${dist_dir}/pslog-${PSLOG_VERSION}.h.gz"
)

set(checksum_inputs "")
foreach(entry IN LISTS release_entries)
    if(NOT entry STREQUAL "${checksums_name}")
        list(APPEND checksum_inputs "${entry}")
    endif()
endforeach()

list(LENGTH checksum_inputs checksum_input_count)
if(checksum_input_count EQUAL 0)
    message(FATAL_ERROR "no release artifacts found in ${dist_dir} for version ${PSLOG_VERSION}")
endif()

list(SORT checksum_inputs)

execute_process(
    COMMAND "${PSLOG_ROOT}/scripts/sha256_files.sh" ${checksum_inputs}
    WORKING_DIRECTORY "${dist_dir}"
    OUTPUT_FILE "${checksums_path}"
    RESULT_VARIABLE checksum_result
)

if(NOT checksum_result EQUAL 0)
    message(FATAL_ERROR "failed to create ${checksums_name}")
endif()
