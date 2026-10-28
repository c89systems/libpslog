if(NOT DEFINED PSLOG_ROOT)
    message(FATAL_ERROR "PSLOG_ROOT is required")
endif()

# This is deliberately structural: make -n still executes recursive $(MAKE)
# recipes, which makes a clean-tree release-contract test materialize outputs.
file(READ "${PSLOG_ROOT}/Makefile" makefile)

if(NOT makefile MATCHES "prerelease: release-pipeline")
    message(FATAL_ERROR "prerelease does not invoke the shared release-pipeline")
endif()
if(NOT makefile MATCHES "release:\n[ \t]*\\$\\(MAKE\\) lifecycle-version-contract\n[ \t]*PKT_TIMING_FILE=.*release-clean \\$\\(MAKE\\) clean\n[ \t]*PKT_TIMING_FILE=.*release-pipeline \\$\\(MAKE\\) release-pipeline")
    message(FATAL_ERROR "release must verify the version contract before cleaning and invoking the shared release-pipeline")
endif()

string(FIND "${makefile}" "release-pipeline:" pipeline_offset)
string(FIND "${makefile}" "$(MAKE) format-check" format_offset)
string(FIND "${makefile}" "$(MAKE) test\n" ordinary_offset REVERSE)
string(FIND "${makefile}" "$(MAKE) release-matrix" matrix_offset)
if(pipeline_offset LESS 0 OR format_offset LESS pipeline_offset OR
   ordinary_offset LESS format_offset OR matrix_offset LESS ordinary_offset)
    message(FATAL_ERROR "release-pipeline does not run ordinary checks before release-matrix")
endif()
string(SUBSTRING "${makefile}" ${pipeline_offset} -1 release_tail)
foreach(phase test valgrind fuzz-smoke gobencher-tests lua-test perf-gate release-matrix)
    if(NOT release_tail MATCHES "\\$\\(TIMED\\) ${phase} \\$\\(MAKE\\) ${phase}")
        message(FATAL_ERROR "release pipeline is missing ${phase}")
    endif()
endforeach()
if(release_tail MATCHES "\\$\\(MAKE\\) (test-all|cross-test)(\n|$)")
    message(FATAL_ERROR "release pipeline must leave cross checks to the release matrix")
endif()

if(NOT makefile MATCHES "prerelease-hardening: prerelease fuzz-long")
    message(FATAL_ERROR "prerelease-hardening must include the explicit long AFL++ tier")
endif()

if(NOT makefile MATCHES "RELEASE_TIMING_FILE := \\$\\(CURDIR\\)/build/release-timings\\.tsv" OR
   NOT makefile MATCHES "TIMED := ./scripts/run_timed\\.sh")
    message(FATAL_ERROR "release must expose the lifecycle timing output through scripts/run_timed.sh")
endif()
if(NOT makefile MATCHES "test-all:\n[ \t]*\\$\\(TIMED\\) test \\$\\(MAKE\\) test\n[ \t]*\\$\\(TIMED\\) valgrind \\$\\(MAKE\\) valgrind" OR
   NOT makefile MATCHES "\\$\\(TIMED\\) fuzz-smoke \\$\\(MAKE\\) fuzz-smoke" OR
   NOT makefile MATCHES "\\$\\(TIMED\\) cross-test \\$\\(MAKE\\) cross-test" OR
   NOT makefile MATCHES "\\$\\(TIMED\\) gobencher-tests \\$\\(MAKE\\) gobencher-tests" OR
   NOT makefile MATCHES "\\$\\(TIMED\\) perf-gate \\$\\(MAKE\\) perf-gate")
    message(FATAL_ERROR "test-all must expose its serial release timing phases")
endif()
if(makefile MATCHES "run_host_binary\\.sh" OR
   makefile MATCHES "go test -exec" OR
   makefile MATCHES "PSLOG_HOST_EXECUTOR")
    message(FATAL_ERROR "Go host tools must execute normally; only project ELF executables select Bootlin at link time")
endif()
if(makefile MATCHES "export (LD_LIBRARY_PATH|DYLD_LIBRARY_PATH)" OR
   NOT makefile MATCHES "build/lua-runtime/pslog_lua")
    message(FATAL_ERROR "Lua module tests require the local interpreter without runtime environment injection")
endif()
file(READ "${PSLOG_ROOT}/scripts/verify_release_privacy.sh" privacy_gate)
if(NOT privacy_gate MATCHES "scripts/sha256_files\\.sh\" --check" OR
   privacy_gate MATCHES "(^|\n)[^\n]*sha256sum -c")
    message(FATAL_ERROR "release privacy checksum verification must use the portable checksum helper")
endif()
file(READ "${PSLOG_ROOT}/tests/package_archives_test.cmake" package_archives_test)
if(NOT package_archives_test MATCHES "scripts/sha256_files\\.sh\" --check" OR
   package_archives_test MATCHES "(^|\n)[^\n]*sha256sum -c")
    message(FATAL_ERROR "package archive checksum verification must use the portable checksum helper")
endif()
if(NOT makefile MATCHES "valgrind --leak-check=full --track-origins=yes --error-exitcode=1" OR
   makefile MATCHES "run_sysroot_binary\\.sh" OR
   makefile MATCHES "--library-path")
    message(FATAL_ERROR "Valgrind must execute the ELF-pinned Bootlin facade process directly")
endif()
