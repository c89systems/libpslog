if(NOT DEFINED PSLOG_BINARY_DIR OR NOT DEFINED PSLOG_ROOT OR NOT DEFINED PSLOG_TARGET_ID)
    message(FATAL_ERROR "PSLOG_BINARY_DIR, PSLOG_ROOT, and PSLOG_TARGET_ID are required")
endif()
cmake_policy(SET CMP0007 NEW)

execute_process(
    COMMAND make release-lua-artifacts
    WORKING_DIRECTORY "${PSLOG_ROOT}"
    RESULT_VARIABLE artifact_result
    OUTPUT_VARIABLE artifact_output
    ERROR_VARIABLE artifact_error
)
if(NOT artifact_result EQUAL 0)
    message(FATAL_ERROR
        "failed to build Lua release artifacts\n${artifact_output}${artifact_error}")
endif()

execute_process(
    COMMAND "${PSLOG_ROOT}/lua/scripts/release_version.sh"
    WORKING_DIRECTORY "${PSLOG_ROOT}"
    RESULT_VARIABLE version_result
    OUTPUT_VARIABLE version
)
if(NOT version_result EQUAL 0)
    message(FATAL_ERROR "failed to resolve Lua release version")
endif()
string(STRIP "${version}" version)
set(source_archive "${PSLOG_ROOT}/dist/lua-pslog-${version}.tar.gz")
set(source_rock "${PSLOG_ROOT}/dist/lua-pslog-${version}-1.src.rock")
set(source_root "${PSLOG_BINARY_DIR}/lua-release-artifacts-test/lua-pslog-${version}")
set(extract_root "${PSLOG_BINARY_DIR}/lua-release-artifacts-test")
file(REMOVE_RECURSE "${extract_root}")
file(MAKE_DIRECTORY "${extract_root}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar xzf "${source_archive}"
    WORKING_DIRECTORY "${extract_root}"
    RESULT_VARIABLE extract_result
    ERROR_VARIABLE extract_error
)
if(NOT extract_result EQUAL 0)
    message(FATAL_ERROR "failed to extract Lua source package: ${extract_error}")
endif()

file(GLOB_RECURSE staged_files RELATIVE "${source_root}" LIST_DIRECTORIES false
    "${source_root}/*")
list(SORT staged_files)
set(expected_files
    LICENSE
    README.md
    RELEASE_MANIFEST
    VERSION
    include/pslog.h
    include/pslog_lua.h
    include/pslog_version.h
    lua/README.md
    lua/RELEASE_MANIFEST.in
    lua/lua-pslog.rockspec.in
    lua/pslog/init.lua
    lua/scripts/release_version.sh
    lua/scripts/render_release_rockspec.sh
    lua/scripts/stage_release_sources.sh
    lua/src/pslog_lua.c
)
list(SORT expected_files)
if(NOT staged_files STREQUAL expected_files)
    string(JOIN "\n" actual_listing ${staged_files})
    string(JOIN "\n" expected_listing ${expected_files})
    message(FATAL_ERROR
        "Lua source package does not match its explicit minimal manifest\n"
        "actual:\n${actual_listing}\nexpected:\n${expected_listing}")
endif()

file(READ "${source_root}/RELEASE_MANIFEST" release_manifest)
string(REPLACE "\n" ";" manifest_files "${release_manifest}")
list(REMOVE_ITEM manifest_files "")
list(SORT manifest_files)
if(NOT manifest_files STREQUAL expected_files)
    message(FATAL_ERROR "Lua source package RELEASE_MANIFEST does not match its payload")
endif()
file(READ "${source_root}/VERSION" staged_version)
string(STRIP "${staged_version}" staged_version)
if(NOT staged_version STREQUAL version)
    message(FATAL_ERROR "Lua source package VERSION does not match its artifact version")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar tf "${source_rock}"
    RESULT_VARIABLE rock_list_result
    OUTPUT_VARIABLE rock_listing
    ERROR_VARIABLE rock_list_error
)
if(NOT rock_list_result EQUAL 0)
    message(FATAL_ERROR "failed to inspect Lua source rock: ${rock_list_error}")
endif()
string(FIND "${rock_listing}" "lua-pslog-${version}.tar.gz" source_archive_entry)
string(FIND "${rock_listing}" "lua-pslog-${version}-1.rockspec" rockspec_entry)
if(source_archive_entry EQUAL -1 OR rockspec_entry EQUAL -1)
    message(FATAL_ERROR "Lua source rock is missing its rendered rockspec or staged source package")
endif()
set(rock_extract_root "${extract_root}/source-rock")
file(MAKE_DIRECTORY "${rock_extract_root}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar xf "${source_rock}"
    WORKING_DIRECTORY "${rock_extract_root}"
    RESULT_VARIABLE rock_extract_result
    ERROR_VARIABLE rock_extract_error
)
if(NOT rock_extract_result EQUAL 0)
    message(FATAL_ERROR "failed to extract Lua source rock: ${rock_extract_error}")
endif()
file(SHA256 "${source_archive}" source_archive_sha256)
file(SHA256 "${rock_extract_root}/lua-pslog-${version}.tar.gz" embedded_archive_sha256)
if(NOT source_archive_sha256 STREQUAL embedded_archive_sha256)
    message(FATAL_ERROR "Lua source rock does not contain the staged Lua source package")
endif()

# Use the extracted binary SDK for both static and shared interop consumers.
execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${PSLOG_ROOT}/build/host" --target package-archive
    RESULT_VARIABLE sdk_result
    OUTPUT_VARIABLE sdk_output
    ERROR_VARIABLE sdk_error
)
if(NOT sdk_result EQUAL 0)
    message(FATAL_ERROR "failed to package interop SDK\n${sdk_output}${sdk_error}")
endif()
set(sdk_name "libpslog-${version}-${PSLOG_TARGET_ID}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar xf "${PSLOG_ROOT}/dist/${sdk_name}.tar.gz"
    WORKING_DIRECTORY "${extract_root}"
    RESULT_VARIABLE sdk_extract_result
    ERROR_VARIABLE sdk_extract_error
)
if(NOT sdk_extract_result EQUAL 0)
    message(FATAL_ERROR "failed to extract interop SDK: ${sdk_extract_error}")
endif()

# Exercise packaged headers and facade source, rather than just archive names.
execute_process(
    COMMAND "${PSLOG_ROOT}/lua/scripts/run_interop_embedder_test.sh"
        "${PSLOG_BINARY_DIR}" "${version}" "${PSLOG_ROOT}/build/luarocks"
        "${extract_root}/${sdk_name}" "${source_root}"
    WORKING_DIRECTORY "${PSLOG_ROOT}"
    RESULT_VARIABLE consumer_result
    OUTPUT_VARIABLE consumer_output
    ERROR_VARIABLE consumer_error
)
if(NOT consumer_result EQUAL 0)
    message(FATAL_ERROR
        "packaged Lua interop consumers failed\n${consumer_output}${consumer_error}")
endif()

set(provenance_root "${extract_root}/provenance-fixture")
file(MAKE_DIRECTORY "${provenance_root}/lua")
file(WRITE "${provenance_root}/lua/RELEASE_MANIFEST.in"
    "lua/README.md\n"
    "tracked.txt\n")
file(WRITE "${provenance_root}/lua/README.md" "committed Lua documentation\n")
file(WRITE "${provenance_root}/tracked.txt" "committed source\n")
file(WRITE "${provenance_root}/other.txt" "committed but not released source\n")
file(WRITE "${provenance_root}/pslog_version.h" "#define PSLOG_VERSION_STRING \"9.9.9\"\n")
execute_process(
    COMMAND git init --quiet
    WORKING_DIRECTORY "${provenance_root}"
    RESULT_VARIABLE fixture_git_init_result
)
execute_process(COMMAND git config user.email lifecycle-test@example.invalid
    WORKING_DIRECTORY "${provenance_root}" RESULT_VARIABLE fixture_git_email_result)
execute_process(COMMAND git config user.name lifecycle-test
    WORKING_DIRECTORY "${provenance_root}" RESULT_VARIABLE fixture_git_name_result)
execute_process(COMMAND git add . WORKING_DIRECTORY "${provenance_root}"
    RESULT_VARIABLE fixture_git_add_result)
execute_process(COMMAND git commit --quiet -m fixture
    WORKING_DIRECTORY "${provenance_root}" RESULT_VARIABLE fixture_git_commit_result)
if(NOT fixture_git_init_result EQUAL 0 OR NOT fixture_git_email_result EQUAL 0
        OR NOT fixture_git_name_result EQUAL 0 OR NOT fixture_git_add_result EQUAL 0
        OR NOT fixture_git_commit_result EQUAL 0)
    message(FATAL_ERROR "failed to create the Lua release provenance fixture")
endif()
file(WRITE "${provenance_root}/tracked.txt" "uncommitted source\n")
file(WRITE "${provenance_root}/lua/RELEASE_MANIFEST.in"
    "lua/README.md\n"
    "other.txt\n")
execute_process(
    COMMAND bash "${PSLOG_ROOT}/lua/scripts/stage_release_sources.sh"
        "${provenance_root}" "${provenance_root}/stage" "9.9.9"
        "${provenance_root}/pslog_version.h"
    RESULT_VARIABLE provenance_result
    ERROR_VARIABLE provenance_error
)
if(NOT provenance_result EQUAL 0)
    message(FATAL_ERROR "Lua release provenance staging failed: ${provenance_error}")
endif()
file(READ "${provenance_root}/stage/tracked.txt" staged_tracked_source)
if(NOT staged_tracked_source STREQUAL "committed source\n")
    message(FATAL_ERROR "Lua release staging used uncommitted working-tree content")
endif()
if(EXISTS "${provenance_root}/stage/other.txt")
    message(FATAL_ERROR "Lua release staging used an uncommitted manifest")
endif()

set(source_package_fixture "${extract_root}/source-package-fixture")
file(MAKE_DIRECTORY "${source_package_fixture}/lua/scripts")
file(MAKE_DIRECTORY "${source_package_fixture}/lua/pslog")
file(MAKE_DIRECTORY "${source_package_fixture}/lua/src")
file(MAKE_DIRECTORY "${source_package_fixture}/include")
file(WRITE "${source_package_fixture}/VERSION" "8.7.6\n")
file(WRITE "${source_package_fixture}/LICENSE" "license\n")
file(WRITE "${source_package_fixture}/include/pslog.h" "pslog header\n")
file(WRITE "${source_package_fixture}/include/pslog_lua.h" "pslog lua header\n")
file(WRITE "${source_package_fixture}/lua/README.md" "lua docs\n")
file(WRITE "${source_package_fixture}/lua/RELEASE_MANIFEST.in"
    "LICENSE\n"
    "include/pslog.h\n"
    "include/pslog_lua.h\n"
    "lua/README.md\n"
    "lua/RELEASE_MANIFEST.in\n"
    "lua/lua-pslog.rockspec.in\n"
    "lua/pslog/init.lua\n"
    "lua/scripts/render_release_rockspec.sh\n"
    "lua/scripts/release_version.sh\n"
    "lua/scripts/stage_release_sources.sh\n"
    "lua/src/pslog_lua.c\n")
file(WRITE "${source_package_fixture}/lua/lua-pslog.rockspec.in" "rockspec\n")
file(WRITE "${source_package_fixture}/lua/pslog/init.lua" "return {}\n")
file(WRITE "${source_package_fixture}/lua/scripts/render_release_rockspec.sh" "#!/usr/bin/env sh\n")
file(COPY "${PSLOG_ROOT}/lua/scripts/release_version.sh"
    DESTINATION "${source_package_fixture}/lua/scripts")
file(WRITE "${source_package_fixture}/lua/scripts/stage_release_sources.sh" "#!/usr/bin/env bash\n")
file(WRITE "${source_package_fixture}/lua/src/pslog_lua.c" "int luaopen_pslog_core(void) { return 0; }\n")
file(WRITE "${source_package_fixture}/RELEASE_MANIFEST"
    "VERSION\n"
    "RELEASE_MANIFEST\n"
    "LICENSE\n"
    "include/pslog.h\n"
    "include/pslog_lua.h\n"
    "lua/README.md\n"
    "lua/RELEASE_MANIFEST.in\n"
    "lua/lua-pslog.rockspec.in\n"
    "lua/pslog/init.lua\n"
    "lua/scripts/render_release_rockspec.sh\n"
    "lua/scripts/release_version.sh\n"
    "lua/scripts/stage_release_sources.sh\n"
    "lua/src/pslog_lua.c\n")
execute_process(
    COMMAND bash "${PSLOG_ROOT}/lua/scripts/stage_release_sources.sh"
        "${source_package_fixture}" "${source_package_fixture}/stage" "8.7.6"
        "${source_package_fixture}/include/pslog.h"
    RESULT_VARIABLE source_package_stage_result
    ERROR_VARIABLE source_package_stage_error
)
if(NOT source_package_stage_result EQUAL 0)
    message(FATAL_ERROR "Lua release staging failed from non-Git source package: ${source_package_stage_error}")
endif()
if(NOT EXISTS "${source_package_fixture}/stage/lua/src/pslog_lua.c")
    message(FATAL_ERROR "Lua release staging from source package omitted Lua C source")
endif()
file(READ "${source_package_fixture}/stage/lua/src/pslog_lua.c" staged_fixture_lua_source)
if(NOT staged_fixture_lua_source STREQUAL "int luaopen_pslog_core(void) { return 0; }\n")
    message(FATAL_ERROR
        "Lua release staging from nested source package used enclosing Git checkout content")
endif()
execute_process(
    COMMAND bash lua/scripts/release_version.sh
    WORKING_DIRECTORY "${source_package_fixture}"
    RESULT_VARIABLE source_package_version_result
    OUTPUT_VARIABLE source_package_version
)
string(STRIP "${source_package_version}" source_package_version)
if(NOT source_package_version_result EQUAL 0 OR NOT source_package_version STREQUAL "8.7.6")
    message(FATAL_ERROR "Lua release version helper did not use source-package VERSION")
endif()
file(REMOVE_RECURSE "${extract_root}")
