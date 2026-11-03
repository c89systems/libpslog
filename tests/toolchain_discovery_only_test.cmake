set(root "${PSLOG_BINARY_DIR}/toolchain-discovery-only-test")
file(REMOVE_RECURSE "${root}")
file(MAKE_DIRECTORY "${root}/helpers")
foreach(name cpkt-toolchains.sh cpkt-aflpp.sh)
    file(WRITE "${root}/helpers/${name}"
        "#!/bin/sh\ncase \"$1\" in\ndiscover) printf 'status=missing\\nnote=status=ready\\n';;\n*) touch '${root}/provisioned'; exit 91;;\nesac\n")
    file(CHMOD "${root}/helpers/${name}"
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
endforeach()
foreach(toolchain host.cmake linux-aflpp.cmake arm64-apple-darwin.cmake)
    file(WRITE "${root}/probe.cmake"
        "set(CMAKE_HOST_SYSTEM_NAME Linux)\nset(CMAKE_HOST_SYSTEM_PROCESSOR x86_64)\n"
        "set(PSLOG_ALLOW_HOST_COMPILER OFF)\n"
        "set(CPKT_LIFECYCLE_SCRIPTS_DIR \"${root}/helpers\")\n"
        "include(\"${PSLOG_ROOT}/cmake/toolchains/${toolchain}\")\n")
    execute_process(COMMAND "${CMAKE_COMMAND}" -P "${root}/probe.cmake"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(result EQUAL 0 OR EXISTS "${root}/provisioned")
        message(FATAL_ERROR "${toolchain} accepted missing tools or attempted provisioning: ${output}${error}")
    endif()
    if(NOT "${output}${error}" MATCHES "ensure|resolve")
        message(FATAL_ERROR "${toolchain} did not explain preparation: ${output}${error}")
    endif()
endforeach()
