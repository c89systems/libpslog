set(root "${PSLOG_BINARY_DIR}/required-ctest-test")
file(REMOVE_RECURSE "${root}")
file(MAKE_DIRECTORY "${root}/build/host")
file(COPY "${PSLOG_ROOT}/CMakePresets.json" DESTINATION "${root}")
file(WRITE "${root}/CMakeLists.txt" "cmake_minimum_required(VERSION 3.21)\nproject(required_tests NONE)\n")
file(WRITE "${root}/build/host/CTestTestfile.cmake"
    "add_test(first \"${CMAKE_COMMAND}\" -E false)\n"
    "add_test(second \"${CMAKE_COMMAND}\" -E touch \"${root}/second-ran\")\n")
execute_process(COMMAND "${CMAKE_CTEST_COMMAND}" --preset host -R "^missing$"
    WORKING_DIRECTORY "${root}" RESULT_VARIABLE empty_result
    OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(empty_result EQUAL 0)
    message(FATAL_ERROR "required preset accepted an empty selection: ${output}${error}")
endif()
execute_process(COMMAND "${CMAKE_CTEST_COMMAND}" --preset host
    WORKING_DIRECTORY "${root}" RESULT_VARIABLE failed_result
    OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(failed_result EQUAL 0 OR EXISTS "${root}/second-ran")
    message(FATAL_ERROR "required preset continued after failure: ${output}${error}")
endif()
file(MAKE_DIRECTORY "${root}/build/aarch64-linux-gnu-release")
file(WRITE "${root}/build/aarch64-linux-gnu-release/CTestTestfile.cmake"
    "add_test(target \"${CMAKE_COMMAND}\" -E true)\n"
    "set_tests_properties(target PROPERTIES LABELS target)\n"
    "add_test(host_fixture \"${CMAKE_COMMAND}\" -E false)\n")
execute_process(COMMAND "${CMAKE_CTEST_COMMAND}" --preset aarch64-linux-gnu-release
    WORKING_DIRECTORY "${root}" RESULT_VARIABLE target_result
    OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT target_result EQUAL 0 OR NOT output MATCHES "1/1")
    message(FATAL_ERROR "cross preset did not retain target proof and exclude host fixtures: ${output}${error}")
endif()
