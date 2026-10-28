# libpslog Review Tracker

## Lifecycle alignment follow-up — 2026-10-28

The follow-up sweep found configure-time toolchain provisioning, a release
preflight that created/deleted an active-checkout tag, forced clangd database
selection, mutating formatting during release, and duplicate cross/coverage
checks. The repairs use the existing Make/CMake/CTest graph:

- Only the outer configure entrypoint prepares toolchains; CMake and its probes
  discover prepared collections, with helper selection propagated explicitly.
  Darwin selects its C++ compiler and symbol tool from the same osxcross prefix.
- Final-release preflight reads identity without mutating refs or generated
  state, requires the default branch, clean tree, verified signature and exact
  lightweight tag, and rejects candidate/developer overrides. Tag experiments
  and real signing tests use a disposable fixture with a fixture-local key.
- Required CTest presets stop on failure and reject empty selections. Direct
  CTest entrypoints do the same. The matrix owns cross checks; coverage-report
  owns its test run. Preset schema 3 matches the declared CMake 3.21 minimum.
  Cross/release presets select target runtime, package, symbol and example proof;
  host-only contract fixtures remain in native presets.
- Release checks formatting without rewriting signed source. The formatting
  inventory includes Lua C sources.
- Normal native builds prepare the editor database and generated dependencies;
  clangd validates public and implementation context through `.clangd`, without
  selecting a different database or compiler. Optional check-mode refactorings
  are limited to macro expansion; parser diagnostics remain enforced.
- Full clean removes hidden generated state and broken generated links while
  preserving unrelated files and shared caches.
- The release entrypoint validates identity before timing can write generated
  state. Privacy verification checks tool-discovery exit status before evaluating
  its output. Matrix readiness matches the complete status field and rejects a
  broken claimed-ready compiler before any producer; source smoke owns the
  matrix's source-archive generation.
- Binary/source/Lua archive recipes normalize ordering, timestamps, ownership
  and modes while preserving executable source files. Source rocks omit ZIP
  host metadata. Regression fixtures call the actual packaging entrypoints under
  different input mtimes, umasks and timezones and compare the complete bytes;
  Lua packaging remains an opt-in CTest dependency.

Product surfaces, supported shipment targets and the explicit developer-only
compiler override remain project choices. Final release requires pinned Linux
tools; this audit does not authorize a release, tag or push.

## Stashed Lua investigation — 2026-10-28

Scope: `stash/pslog/PR.md`, its ownership reproducer and exploratory patches,
compared with the binding at released 0.12.0. The checked-in binding matched
the reported source SHA-256 exactly. The archive's parity specifications and
format-emission requirement are explicitly withdrawn, not product requirements.

Confirmed defects and repairs:

| Defect | Evidence and repair |
| --- | --- |
| Lua allocation failure after native logger acquisition | The unchanged `with_level` probe retained about 4.95 MB after VM close. All constructor and child adoption now runs in a protected frame with native rollback. Callback setup installs cleanup before any Lua allocation. |
| Callback string setup could escape native locks | Persistent allocation failure through a public native view aborted the unchanged probe. Callback lookup, chunk creation and invocation now share one protected frame, with stack restoration and a native error status. |
| Callback reentry through shared children deadlocked | The unchanged parent-to-child callback probe timed out. A shared callback guard rejects Lua emission, derivation and close until callback unwind. |
| Self-capturing callbacks stayed permanently rooted | Weak-reference probes showed that collection could not reclaim the logger. Strong ownership now follows collectable userdata edges; the registry supplies only weak callback lookup by context identity. |
| Derived loggers did not retain borrowed Lua files | A weak-reference probe showed file collection while a child still existed. Each logger retains the shared output value, without retaining ancestor logger shells or their copied fields. Explicit file close still requires caller coordination. |
| Field conversion could invalidate live scratch fields | Valgrind reported invalid writes when `__tostring` recursively logged enough fields to reallocate scratch storage. Protected field collection rejects same-logger reentry and restores the guard on error. Stack capacity checks cover retained conversion temporaries. |
| Native constructor failure had inconsistent output ownership | Allocation-failure sweeps cover plain construction, environment replacement and tees. Failure now preserves the caller's seed output and releases only overlay resources; successful construction accepts close responsibility. This also prevents duplicate binding cleanup. |

`tests/lua_interop_embedder_test.c` covers all existing constructor/derivation
families over 24 Lua allocation budgets with native path, callback and borrowed
FILE outputs. It checks VM cleanup, parent recovery, callback OOM through public
C interop, collection, reentry, large conversion tables and exception identity.
Its test-hook configuration additionally sweeps native constructor failures.
Run the installed facade tests with `make lua-test`, and the test-hook suite
with `debug-lua` plus Valgrind. Keep the normal CTest and performance gates.
Validated string-key/scalar-value pairs retain a direct conversion path;
only conversions that can allocate Lua objects or execute Lua need the
protected collection frame.

Lua exposure recommendation: retain the current Lua-first API.

| Archived addition | Disposition |
| --- | --- |
| `withf` and formatted emission | Do not expose. `with(...)` and ordinary structured logging already cover Lua workflows. A Lua C-variadic grammar adds parsing, validation and maintenance. The archive's long-key/custom-palette counterexample prevents claiming exact formatter equivalence. |
| Typed field records, metadata and explicit views | Do not mirror C structs. Existing inference and semantic wrappers cover supported values; Lua strings already carry binary lengths. New capability objects would add ownership and conversion work without a demonstrated consumer. |
| Owned, borrowed, custom and observed output objects | Defer pending a concrete workflow. These create transfer, borrow, callback-result and statistics contracts beyond the existing file/function API. Ownership repairs do not require them. |
| Custom palettes, default records and full constant exports | Defer until requested by a consumer. Built-in palette names and current options suffice; no performance claim is made for the withdrawn implementation. |
| Trust predicates, level predicates and Lua interop tokens | Add only for a measured or concrete need. Explicit trusted wrappers and the existing public C check/ref/native-sink capability already provide the supported mechanisms. A Lua predicate alone does not establish native-thread safety. |

Callback return values remain ignored, and callback errors remain internal
native write failures after safe unwind. Propagating exceptions (including
`false`) or accepting `(code, written)` would be a deliberate behavior change,
so the archived callback-result expansion was not adopted. Native/Lua public
struct layouts and interop ABI 1 are unchanged.

The lifecycle helpers are updated from the current skill as a separate change:
`cpkt` toolchain/dependency caches, read-only resolver queries, explicit outer
provisioning, selected-runtime AFL++ wrappers and digest-based offline archive
reuse. Final local verification uses the `cpkt` collection; the old-cache
performance run is only a comparison baseline.

Use this file to coordinate hardening review passes across the logger
implementation. A checked item means the assigned reviewer completed that pass
and found no actionable findings for that category. If a reviewer finds an
issue, leave the item unchecked and track the finding separately until resolved.

Status convention:

- `- [ ] ...` means not started.
- `- [ ] ... (started)` marks the one review category currently in progress.
- `- [x] ...` means complete with no actionable findings from that review pass.

At most one checklist item may include `(started)` at any time. When a started
review pass completes cleanly, remove `(started)` and check the item. When a
new review pass begins, the reviewer may mark any unchecked item as
`(started)`.

## Core Logger Implementation

- [x] Public C API contract and ABI surface
  - Scope: `include/pslog.h`, `include/pslog_version.h.in`, exported symbols,
    documented ownership and lifetime rules, enum values, struct layout, method
    table shape, fatal/panic behavior, and backward-compatibility expectations.
- [x] Logger construction, configuration, and environment overlay
  - Scope: `pslog_default_config()`, `pslog_new()`, `pslog_new_from_env()`,
    level parsing, mode/color/palette parsing, timestamp options, min-level
    behavior, output path parsing, tee/default output handling, and invalid
    configuration fallback/error behavior.
- [x] Logger object lifecycle and shared-state ownership
  - Scope: root and child logger allocation, `with()`, `withf()`,
    `with_level()`, `with_level_field()`, refcounting, `destroy()`, `close()`,
    owned versus borrowed outputs, error preservation, and double-close safety.
- [x] Output abstraction, write retries, partial writes, and close semantics
  - Scope: `pslog_output`, observed output wrappers, file output initialization,
    callback contracts, short-write retry loops, chunked writes, output locking,
    close ownership, null userdata handling, and actionable failure reporting.
- [x] Buffering, chunking, allocation, and oversized-line behavior
  - Scope: `pslog_buffer`, stack versus heap storage, reserve/growth logic,
    flush behavior, line finalization, zero-allocation hot path claims, long
    line streaming, allocation failure handling, and buffer invariant safety.
- [x] Structured field constructors and scalar rendering
  - Scope: `pslog_null`, `pslog_str`, `pslog_trusted_str`, numeric fields,
    boolean fields, pointer fields, byte fields, time fields, duration fields,
    errno fields, cached key/value lengths, trusted flags, and borrowed-data
    lifetime assumptions.
- [x] Level filtering and dispatch semantics
  - Scope: `pslog_should_log()`, `log()`, level-specific methods, free-function
    wrappers, `PSLOG_LEVEL_NOLEVEL`, `PSLOG_LEVEL_DISABLED`, fatal exit path,
    panic abort path, derived logger thresholds, and optional emitted `loglevel`
    fields.

## Encoding and Formatting

- [x] Console emitter formatting
  - Scope: `src/pslog_emit_console.c`, timestamp/level/message layout, static
    field prefixes, dynamic fields, quoting rules, indentation preservation,
    printable Unicode handling, DEL/control/high-bit behavior, errno rendering,
    duration/time formatting, and Go pslog compatibility expectations.
- [x] JSON emitter formatting and validity
  - Scope: `src/pslog_emit_json.c`, JSON object shape, key ordering, string
    escaping, invalid UTF-8 handling, byte encoding, numeric rendering,
    non-finite float policy, null/bool rendering, trusted-string fast paths, and
    JSONL newline discipline.
- [x] Timestamp and time-zone handling
  - Scope: RFC3339/RFC3339Nano paths, custom `strftime` formats, UTC/local
    behavior, cached timestamp invalidation, nanosecond precision, explicit
    `pslog_time_value` rendering, and thread-safe time formatting.
- [x] Color and palette subsystem
  - Scope: `src/pslog_palette.c`, built-in palette registry, alias lookup,
    console ANSI decoration, JSON color metadata, auto-color `isatty` behavior,
    palette string lengths, long palette literal fallback, and plain/color
    output parity after stripping ANSI.
- [x] `kvfmt` parser, caches, and variadic field path
  - Scope: `infof`/`tracef`/etc., `withf()`, supported format verbs, `%m`
    errno snapshotting, max field limits, long key fallback, pointer/content
    caches, cached console/JSON prefixes, varargs type safety assumptions, and
    behavior for malformed or unsupported format strings.
- [x] Trust, escaping, and injection boundaries
  - Scope: `pslog_string_is_trusted()`, trusted key/value flags, console-simple
    detection, JSON escaping bypass conditions, Lua binding trust caches, ANSI
    injection exposure, newline/control-character handling, and documented
    caller responsibility for trusted fast paths.

## Concurrency, Safety, and Portability

- [x] Thread safety and lock ordering
  - Scope: state mutex, output mutex, shared logger trees, concurrent `withf`
    and `kvfmt` cache use, timestamp cache use, close versus emit races, output
    callback reentrancy expectations, and deadlock risk.
- [x] Memory safety and failure handling
  - Scope: all allocation paths, ownership transfer, cleanup on partial
    construction failure, integer overflow in size calculations, null handling,
    borrowed pointer lifetime assumptions, use-after-free risk in caches, and
    sanitizer/fuzz findings.
- [x] C portability and compiler/platform assumptions
  - Scope: C89 build policy, POSIX feature macros, pthread requirement, 32-bit
    and 64-bit integer behavior, musl/glibc differences, Darwin target behavior,
    endian/alignment assumptions, `strerror_r` variants, and cross-compile
    toolchains.
- [x] Performance-sensitive hot paths
  - Scope: normal log emission, prepared-field emission, `kvfmt` emission,
    prefix caches, double-render cache, timestamp cache, branch/lock costs,
    allocation counts, chunk size effects, and benchmark claims versus measured
    results.

## Bindings and Consumer Surfaces

- [x] Lua binding API and lifecycle
  - Scope: `lua/src/pslog_lua.c`, `lua/pslog/init.lua`, Lua constructors,
    output target ownership, callback output, file handles, wrapper values,
    field collection from pairs/tables, logger userdata GC/close behavior, and
    Lua error messages.
- [x] Lua packaging and examples
  - Scope: `lua/lua-pslog.rockspec.in`, `lua/scripts/*`, `lua/examples/*`,
    Lua README contract, module loading, shipped rock contents, version
    rendering, and consistency with the C logger behavior.
- [x] C examples and documentation contract
  - Scope: `README.md`, `examples/*`, API overview snippets, documented
    guarantees, terminology, build examples, and whether examples exercise the
    supported public surface without relying on internals.

## Build, Packaging, and Release

- [x] CMake build graph and target boundaries
  - Scope: `CMakeLists.txt`, presets, shared/static/test targets, compile
    options, include directories, generated version header, dependency fetches,
    optional benchmark dependencies, fuzz target wiring, install rules, and
    target properties.
- [x] Single-header generation and validation
  - Scope: `cmake/package_single_header.cmake`,
    `cmake/pslog_single_header_preamble.in`, generated alias header, embedded
    implementation, license/version content, formatting, and parity with normal
    library tests.
- [x] Binary/source package archive generation
  - Scope: `cmake/package_archive.cmake`, `package_checksums.cmake`,
    `package_clean_dist.cmake`, package metadata, pkg-config/CMake consumer
    files, library names/sonames, stripping behavior, archive layout, and
    checksum output.
- [x] Release privacy and distribution hygiene
  - Scope: `cmake/check_release_privacy.cmake`, release privacy tests, `dist/`
    contents, generated artifacts, absence of build-host paths/secrets/private
    metadata, and reproducibility-sensitive package contents.
- [x] Cross-target release matrix and scripts
  - Scope: `scripts/run_linux_release_matrix.sh`, `scripts/dist-tree.sh`,
    `scripts/clean.sh`, `cmake/toolchains/*`, qemu-emulated tests, Darwin
    packaging path, target IDs, and clean rebuild behavior.

## Verification Assets

- [x] C unit and integration tests
  - Scope: `tests/test_main.c`, `tests/test_main_single_header.c`,
    CMake-driven tests, fixture quality, observable behavior coverage,
    termination subprocess tests, thread tests, output failure tests, and gaps
    between documented contract and executable assertions.
- [x] Fuzzing harness
  - Scope: `fuzz/pslog_fuzz.c`, input modeling, console/JSON mode coverage,
    color parity assertion, field/value generation, sanitizer compatibility,
    crash triage workflow, and missing fuzz targets for high-risk parsers.
- [x] Benchmark harness and performance gates
  - Scope: `bench/*`, `gobencher/*`, benchmark datasets, generated wrappers,
    optional liblogger/Quill adapters, `run_perf_gate.sh`, `run_rebaseline.sh`,
    timing methodology, baseline artifacts, and benchmark-result
    actionability.
- [x] Performance log evidence and baseline management
  - Scope: `performance-logs/*`, baseline/current comparison files,
    provenance of committed measurements, median/run summaries, and whether
    future perf changes have a clear evidence trail.
- [x] CI-equivalent local verification workflow
  - Scope: documented commands, CMake presets, `ctest`, Lua tests, fuzz smoke,
    package tests, format target, coverage script, release matrix, and whether
    the review process can reproduce the expected quality gates locally.
