set(PSLOG_VERSION_OVERRIDE "" CACHE STRING
    "Override the resolved libpslog release version (for example 0.1.0 or 0.1.0-rc.1)")

set(_pslog_default_version "0.0.0")

if(DEFINED PSLOG_VERSION_SOURCE_DIR AND NOT PSLOG_VERSION_SOURCE_DIR STREQUAL "")
    set(_pslog_version_source_dir "${PSLOG_VERSION_SOURCE_DIR}")
else()
    get_filename_component(_pslog_version_source_dir "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

set(_pslog_resolved_version "${_pslog_default_version}")

if(NOT PSLOG_VERSION_OVERRIDE STREQUAL "")
    set(_pslog_resolved_version "${PSLOG_VERSION_OVERRIDE}")
else()
    find_program(PSLOG_GIT_EXECUTABLE NAMES git)
    set(_pslog_git_describe_result 1)
    if(EXISTS "${_pslog_version_source_dir}/.git")
        set(_pslog_source_has_git_metadata TRUE)
    else()
        set(_pslog_source_has_git_metadata FALSE)
    endif()
    if(PSLOG_GIT_EXECUTABLE AND _pslog_source_has_git_metadata)
        execute_process(
            COMMAND "${PSLOG_GIT_EXECUTABLE}" -C "${_pslog_version_source_dir}" tag --points-at HEAD
            RESULT_VARIABLE _pslog_git_describe_result
            OUTPUT_VARIABLE _pslog_git_describe_output
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(_pslog_git_describe_result EQUAL 0)
            string(REPLACE "\n" ";" _pslog_git_tags "${_pslog_git_describe_output}")
            set(_pslog_exact_version_tags "")
            foreach(_pslog_git_tag IN LISTS _pslog_git_tags)
                if(_pslog_git_tag MATCHES "^v([0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z.-]+)?(\\+[0-9A-Za-z.-]+)?)$")
                    execute_process(
                        COMMAND "${PSLOG_GIT_EXECUTABLE}" -C "${_pslog_version_source_dir}" cat-file -t "refs/tags/${_pslog_git_tag}"
                        RESULT_VARIABLE _pslog_tag_type_result
                        OUTPUT_VARIABLE _pslog_tag_type
                        ERROR_QUIET
                        OUTPUT_STRIP_TRAILING_WHITESPACE
                    )
                    execute_process(
                        COMMAND "${PSLOG_GIT_EXECUTABLE}" -C "${_pslog_version_source_dir}"
                            symbolic-ref --quiet "refs/tags/${_pslog_git_tag}"
                        RESULT_VARIABLE _pslog_symbolic_tag_result
                        OUTPUT_QUIET ERROR_QUIET)
                    if(_pslog_symbolic_tag_result EQUAL 0 OR
                       NOT _pslog_tag_type_result EQUAL 0 OR NOT _pslog_tag_type STREQUAL "commit")
                        message(FATAL_ERROR
                            "release tag '${_pslog_git_tag}' must be a lightweight tag that resolves directly to a commit")
                    endif()
                    list(APPEND _pslog_exact_version_tags "${_pslog_git_tag}")
                endif()
            endforeach()
            list(LENGTH _pslog_exact_version_tags _pslog_exact_version_tag_count)
            if(_pslog_exact_version_tag_count GREATER 1)
                message(FATAL_ERROR
                    "multiple exact lightweight release tags point at HEAD: ${_pslog_exact_version_tags}")
            elseif(_pslog_exact_version_tag_count EQUAL 1)
                list(GET _pslog_exact_version_tags 0 _pslog_exact_version_tag)
                string(REGEX REPLACE "^v" "" _pslog_resolved_version "${_pslog_exact_version_tag}")
            endif()
        endif()
    endif()
    if(NOT _pslog_git_describe_result EQUAL 0
       AND NOT _pslog_source_has_git_metadata
       AND EXISTS "${_pslog_version_source_dir}/VERSION")
        file(READ "${_pslog_version_source_dir}/VERSION" _pslog_version_file)
        string(STRIP "${_pslog_version_file}" _pslog_version_file)
        if(_pslog_version_file MATCHES "^([0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z.-]+)?(\\+[0-9A-Za-z.-]+)?)$")
            set(_pslog_resolved_version "${_pslog_version_file}")
        endif()
    endif()
endif()

if(NOT _pslog_resolved_version MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)(-[0-9A-Za-z.-]+)?(\\+[0-9A-Za-z.-]+)?$")
    message(FATAL_ERROR
        "invalid libpslog version '${_pslog_resolved_version}'; expected X.Y.Z with optional prerelease metadata")
endif()

set(PSLOG_RESOLVED_VERSION "${_pslog_resolved_version}")
set(PSLOG_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(PSLOG_VERSION_MINOR "${CMAKE_MATCH_2}")
set(PSLOG_VERSION_PATCH "${CMAKE_MATCH_3}")
set(PSLOG_PROJECT_VERSION_CORE
    "${PSLOG_VERSION_MAJOR}.${PSLOG_VERSION_MINOR}.${PSLOG_VERSION_PATCH}")

message(STATUS "libpslog release version: ${PSLOG_RESOLVED_VERSION}")
