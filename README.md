# libpslog

`libpslog` is a high-performance, zero-dependency structured logger for C.

![libpslog_demo](demo.gif)

`libpslog` is the C port of the Go [pkt.systems/pslog](https://github.com/sa6mwa/pslog) logger and aims to preserve the same overall product shape: console and JSON output, strong structured logging support, careful control over allocations, aggressive benchmarking, and output semantics that are close to the Go implementation. The Go variant is already one of the fastest loggers in that ecosystem; `libpslog` brings the same performance-first design to C while exposing a C-shaped API.

![libpslog elevator pitch](elevatorpitch.gif)

## What It Provides

- Console and JSON loggers from the same API surface.
- Colorized and non-colorized output selected from config, with adaptive tty-aware color by default.
- Structured fields through typed `pslog_field[]` arrays.
- Structured `kvfmt` logging through `tracef/debugf/infof/warnf/errorf`, where `infof` means `message + kvfmt`, not `printf`-formatting the message itself.
- Derived loggers through `with()`, `withf()`, `with_level()`, and `with_level_field()`.
- Environment-driven construction through `pslog_new_from_env()`.
- Explicit trusted-string fast paths for JSON emission.
- Thread-safe shared logging through the same logger tree.
- Zero-allocation hot-path emission for normal log lines, with chunked output for oversized lines instead of truncation.
- Unit tests, fuzzing, pure C benchmarks, and Go-vs-C comparison benchmarks.
- Optional Lua binding release artifacts shipped as `lua-pslog`.

## Release Targets

First-release binary support is:

- `x86_64-linux-gnu`
- `x86_64-linux-musl`
- `aarch64-linux-gnu`
- `aarch64-linux-musl`
- `armhf-linux-gnu`
- `armhf-linux-musl`
- `arm64-apple-darwin`

All targets are wired for:

- configure/build via CMake presets
- tests where the target can run locally or under qemu
- runtime package generation
- dev package generation

For Linux ARM targets, the test presets run under qemu. These are mandatory
release routes: `make release-matrix` and `make release` fail if `qemu-aarch64`
or `qemu-arm` is unavailable. The Darwin target is a build-and-package target
when the local osxcross toolchain is available.

## API Overview

Public symbols use the `pslog_` prefix. Preferred usage is through instance methods on `pslog_logger`.

Core structured path:

```c
#include "pslog.h"

pslog_config config;
pslog_logger *log;
pslog_field fields[2];

pslog_default_config(&config);
config.mode = PSLOG_MODE_JSON;
config.color = PSLOG_COLOR_NEVER;
config.output = pslog_output_from_fp(stdout, 0);

log = pslog_new(&config);

fields[0] = pslog_str("service", "api");
fields[1] = pslog_i64("attempt", 3L);
log->info(log, "request handled", fields, 2u);

log->destroy(log);
```

For strings with a known byte length, use views for the message, field keys,
and string values. The input does not need a trailing NUL byte. There are view
constructors for every field type. For logging, each level has a receiver
method (`log->info_view(...)`) and a free function (`pslog_info_view(...)`);
generic calls use `log->log_view(...)` or `pslog_fields_view(...)`. The
`*f_view` forms accept a message view plus C-string `kvfmt` arguments. Views
are borrowed until the logging call returns; `with()` copies attached fields
into the child logger.

```c
const char message[] = {'r', 'e', 'a', 'd', 'y'};
const char key[] = {'i', 'd'};
const char value[] = {'a', '\0', 'b'};
pslog_field field = pslog_str_view(
    (pslog_string_view){key, sizeof(key)},
    (pslog_string_view){value, sizeof(value)});

log->info_view(log, (pslog_string_view){message, sizeof(message)}, &field, 1u);
/* Equivalent free-function call: */
pslog_info_view(log, (pslog_string_view){message, sizeof(message)}, &field, 1u);
```

Embedded NUL bytes are escaped as `\\u0000` in JSON and `\\x00` in console
output. Existing C-string calls remain available. `kvfmt` and its `%s`
arguments continue to use NUL-terminated C strings.

Derived logger path:

```c
pslog_config config;
pslog_field base[1];
pslog_logger *log;
pslog_logger *child;
pslog_logger *next;

pslog_default_config(&config);
config.output = pslog_output_from_fp(stdout, 0);
log = pslog_new(&config);

base[0] = pslog_str("subsystem", "worker");
child = log->with(log, base, 1u);
log->destroy(log);
next = child->with_level_field(child);
child->destroy(child);
child = next;
child->warn(child, "retrying", NULL, 0u);
child->destroy(child);
```

Derived logger from `kvfmt`:

```c
child = log->withf(log, "service=%s subsystem=%s", "api", "worker");
child->info(child, "request handled", NULL, 0u);
child->destroy(child);
```

`kvfmt` path:

```c
log->infof(log, "request handled", "user=%s code=%d ok=%b ms=%f",
           "alice", 200, 1, 12.34);
```

Direct built-in palette selection:

```c
pslog_default_config(&config);
config.mode = PSLOG_MODE_CONSOLE;
config.color = PSLOG_COLOR_ALWAYS;
config.palette = &pslog_builtin_palette_nord;
```

Additional typed fields include:

- `pslog_bool`
- `pslog_bytes_field`
- `pslog_duration_field`
- `pslog_f64`
- `pslog_i64`
- `pslog_null`
- `pslog_ptr`
- `pslog_time_field`
- `pslog_trusted_str`
- `pslog_u64`

Relevant behavioral notes:

- `PSLOG_LEVEL_NOLEVEL` emits `---` in console mode and `"nolevel"` in JSON mode.
- `log->fatal(...)` and `log->fatalf(...)` log and then exit with failure status.
- `log->panic(...)` and `log->panicf(...)` log and then abort.
- The free-function `pslog_fatal(...)` / `pslog_fatalf(...)` wrappers also terminate.
- The free-function `pslog_panic(...)` / `pslog_panicf(...)` wrappers also abort.
- `with()` returns a derived logger and does not mutate the receiver.
- `close()` closes only owned outputs.
- `pslog_new_from_env()` overlays `LOG_*` settings on top of a seed config.

The public header in [include/pslog.h](include/pslog.h) contains the full API contract and doc comments.

## Examples

The main example is [examples/example.c](examples/example.c). It demonstrates:

- console and JSON loggers
- `log()` and level-specific methods
- `with()`, `with_level()`, and `with_level_field()`
- `withf()` for static `kvfmt`-derived fields
- typed fields
- `infof`/`kvfmt`
- `pslog_new_from_env()`
- palette iteration
- adaptive color behavior

Build it in normal library mode:

```sh
./scripts/configure_cmake.sh --preset host
cmake --build --preset host
cd examples
"$(sed -n 's/^CMAKE_C_COMPILER:[^=]*=//p' ../build/host/CMakeCache.txt)" -I../build/host/generated/include -I../include \
  -o example example.c ../build/host/libpslog.a -pthread \
  $(sed -n 's/^PSLOG_NONSHIPPED_ELF_LINKER_FLAGS:STRING=//p' ../build/host/CMakeCache.txt)
./example
```

For an extracted binary SDK, point pkg-config at that SDK and use the compiler
selected for the matching target rather than an ambient host compiler:

```sh
sdk=/path/to/libpslog-<version>-<target>
export PKG_CONFIG_PATH="$sdk/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
"$CC" $(pkg-config --cflags pslog) -o example example.c $(pkg-config --libs pslog)
```

```cmake
set(CMAKE_PREFIX_PATH "/path/to/libpslog-<version>-<target>")
find_package(pslog CONFIG REQUIRED)
target_link_libraries(example PRIVATE pslog::pslog)
```

The default CMake target links the shared library. Use `pslog::pslog_static`
when a static link is preferred.

After a normal build, `cmake --install build/host --prefix /path/to/sdk`
installs the public libraries, headers, CMake/pkg-config metadata and notices.
Target archives stage through those same install rules.

Build the same example in single-header mode:

```sh
./scripts/configure_cmake.sh --preset host
cmake --build ../build/host --target package-single-header
cd examples
"$(sed -n 's/^CMAKE_C_COMPILER:[^=]*=//p' ../build/host/CMakeCache.txt)" -DPSLOG_EXAMPLE_SINGLE_HEADER=1 \
  -I../build/host/generated/include \
  -o example example.c -pthread \
  $(sed -n 's/^PSLOG_NONSHIPPED_ELF_LINKER_FLAGS:STRING=//p' ../build/host/CMakeCache.txt)
./example
```

The release single-header artifact is written to `dist/pslog-<version>.h.gz`.
The uncompressed generated header remains in the build tree at
`build/host/generated/include/pslog_single_header.h` for local compile smoke
tests. The generated header contains the public API, the `PSLOG_IMPLEMENTATION`
section, the embedded license text, and the single-header usage notes at the top
of the file.

## Lua Binding

`libpslog` ships an optional Lua binding as the `lua-pslog` rock package. The
Lua module itself is required as `pslog`:

```lua
local pslog = require("pslog")

local log = pslog.new_json("/tmp/app.log", {
  no_color = true,
  disable_timestamp = true,
}):with("service", "checkout")

log:info("ready", "port", 8080, "ok", true)
```

The Lua binding keeps the same logger product shape as C and Go, but the API is
written for Lua instead of mirroring the C surface literally:

- ordinary Lua scalars are inferred automatically
- structured logging through key/value pairs is the primary call shape
- a single-table field form exists as convenience
- explicit wrappers exist for semantic types Lua does not natively represent
  such as bytes, time, duration, errno, trusted strings, and `u64`
- constructors support stdout/stderr, file handles, file paths, and callback
  sinks

Examples:

```lua
log:info("typed fields",
  "payload", pslog.bytes(string.char(0, 1, 2, 255)),
  "started_at", pslog.time { sec = 1711737600, nsec = 0, offset = 0 },
  "elapsed", pslog.duration_ms(125),
  "errno", pslog.errno(2)
)
```

Local Lua development helpers:

```sh
make lua-rock
make lua-test
```

`make lua-rock` first installs the selected-compiler public C SDK into the
repo-local `build/lua-sdk/` prefix. The Lua module, C interop checks, and Go
Lua benchmark bridge all consume that installed SDK rather than source-tree
headers or libraries. Lua 5.5.1 is built from a checksum-pinned source archive
with the selected compiler; its headers and static library are staged in
`build/lua-host/`, and `build/lua-runtime/pslog_lua` loads local modules with
the selected runtime. LuaRocks remains a host tool.

Run the Lua examples from the repository root:

```sh
make lua-rock
eval "$(make lua-env)"
build/lua-runtime/pslog_lua lua/examples/example.lua
build/lua-runtime/pslog_lua lua/examples/basic.lua
build/lua-runtime/pslog_lua lua/examples/from_env.lua
build/lua-runtime/pslog_lua lua/examples/callback.lua
```

Example entry points live under [`lua/examples/`](lua/examples/):

- [`lua/examples/example.lua`](lua/examples/example.lua)
- [`lua/examples/basic.lua`](lua/examples/basic.lua)
- [`lua/examples/callback.lua`](lua/examples/callback.lua)
- [`lua/examples/from_env.lua`](lua/examples/from_env.lua)

The full Lua API reference lives in [`lua/README.md`](lua/README.md).

`make release` also emits:

- `dist/lua-pslog-<version>.tar.gz`
- `dist/lua-pslog-<version>-1.rockspec`
- `dist/lua-pslog-<version>-1.src.rock`

Release packaging uses GNU tar and gzip; Lua source-rock packaging also requires
the host `zip` and LuaRocks tools. Tar entries have sorted names, epoch timestamps,
numeric root ownership and normalized permissions that preserve executable bits.
Gzip omits input names and timestamps. Source rocks use fixed ZIP timestamps and
permissions without host ownership or extra timestamp fields. Unchanged payloads
produce identical bytes across input mtimes, umask and timezone changes.

## Build And Test

The core and single-header suites use default worker-thread stacks, including
musl's smaller worker stack. Large capture fixtures use test-owned heap storage.

If you want a simple frontend instead of typing the CMake commands directly, use the repository `Makefile`:

```sh
make build
make test
make test-all
make fuzz
make benchmarks-c
make benchmarks-gobencher
make benchmarks-all
make lua-test
make prerelease
make prerelease-hardening
make release
```

Run `make help` for the full target list.

Standard debug build:

```sh
./scripts/configure_cmake.sh --preset debug
cmake --build --preset debug
ctest --preset debug
```

Native Valgrind check:

```sh
make valgrind
```

Fuzzing:

```sh
make fuzz
```

## Release Matrix

One-command incremental rehearsal for the full shipped matrix:

```sh
make release-matrix
```

`make prerelease` runs the shared deterministic proof graph without cleaning
generated state first. `make release` cleans first and then runs that exact
same proof graph; it is the final local release gate. Before cleaning, it
requires a clean committed worktree on the resolved default branch, a verified
commit signature, exactly one lightweight `vX.Y.Z` tag on `HEAD`, and no
candidate-version or developer-compiler override. It then verifies Make/CMake
version agreement without creating tags or generated state. Untagged candidate
rehearsals use `make prerelease` and version `0.0.0`.
The release-preflight regression fixture uses `ssh-keygen` for a disposable
signing key and trust file under `build/`; it does not use account signing keys.
The release pipeline checks formatting without modifying signed source. Cross
tests belong to the release matrix, and coverage-report owns its CTest run;
neither is repeated by its enclosing recipe.
Cross/release test presets select target runtime, package, symbol and example
proof. Host-only contract fixtures run in native presets rather than once per
foreign target.
`make prerelease-hardening` adds the explicit long AFL++ fuzz tier to that
proof graph. `make release` writes a tab-separated phase timing report to
`build/release-timings.tsv`; its matrix entries distinguish configure, build,
QEMU-backed test, packaging, source smoke, Lua artifacts, checksums, and
privacy verification. It also separates ordinary C tests, Valgrind, AFL++
smoke, cross tests, Go tests, and the performance gate.

Inspect the latest completed report with:

```sh
column -t -s $'\t' build/release-timings.tsv
```

That script runs, for every shipped Linux target:

- `scripts/configure_cmake.sh --preset ...`
- `cmake --build --preset ...`
- `ctest --preset ...`
- runtime package generation
- dev package generation
- `lua-pslog-<version>-1.rockspec` generation
- `lua-pslog-<version>-1.src.rock` generation
- final `libpslog-<version>-CHECKSUMS` generation over every shipped file in `dist/`

When the osxcross arm64 Darwin compiler is available, the script also builds
and packages `arm64-apple-darwin`.

Toolchain expectations:

- The outer `scripts/configure_cmake.sh` entrypoint prepares the checksum-pinned Bootlin stable-2026.08-1 collection through `scripts/cpkt-toolchains.sh`. CMake toolchain files and compiler probes only discover prepared tools; raw CMake invocation requires prior preparation. `CPKT_LIFECYCLE_SCRIPTS_DIR` selects the helper collection and propagates into compiler probes; its default is the vendored `scripts/` directory. The published Bootlin compilers require an x86-64 Linux host. `PSLOG_ALLOW_HOST_COMPILER=1` remains an explicit developer-only escape hatch for host-target presets; it leaves compiler selection to CMake and disables the private Bootlin runtime. Final `make release` forbids this override. Explicit cross/release target presets retain their selected toolchains. On macOS, local development uses Apple tools while the Darwin release preset uses its configured osxcross collection.
- The override applies to its complete host workflow, including CTest, package-consumer verification, Go and Lua consumers, and coverage. Invoke it for each command, for example `PSLOG_ALLOW_HOST_COMPILER=1 make test` or `PSLOG_ALLOW_HOST_COMPILER=1 make coverage`. Coverage resolves `gcov` from the selected host compiler.
- Unless `PSLOG_ALLOW_HOST_COMPILER=1` is active, every non-shipped native Linux executable pins the selected Bootlin ELF interpreter and a private DT_RPATH at link time, so CTest, Valgrind, examples, benchmarks, fuzzing, and generated local consumers run directly with the selected runtime. Native memory checking still uses host Valgrind against a focused Bootlin-built facade test; native x86_64 fuzzing uses the cached AFL++ GCC-plugin wrapper from `scripts/cpkt-aflpp.sh`, which delegates to the same Bootlin collection. Cross-target tests continue to use QEMU with their matching sysroot.
- `clang-format` and `clangd` are host development tools only. Run `make build` to prepare native dependencies, generated headers and `build/debug/compile_commands.json`. `.clangd` discovers that database and retains each owning target's language, defines and includes. `make clangd` checks a public consumer, public header and implementation through ordinary discovery, without database/driver overrides. Its check mode limits optional refactoring actions to macro expansion so missing editor-index definitions do not mask parser results. Public declarations use Doxygen comments for hover documentation. clangd is not a compiler, ABI verifier or release dependency. `make format-check` validates all owned C/C++ sources, including the Lua binding, without editing them.
- Release privacy verification inspects every ELF payload by magic, including extensionless files and nested archives. It rejects private interpreters, bundled libc loaders, absolute dependency paths, and every non-relative runtime search-path entry; missing inspection tools fail the gate. Local runtime flags never enter installed SDK metadata.
- The shared toolchain cache is `${CPKT_TOOLCHAIN_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/cpkt/toolchains}`. Its Bootlin and AFL++ provisioners serialize each collection with a bounded `CPKT_TOOLCHAIN_LOCK_TIMEOUT` (600 seconds by default). External dependency archives use `${CPKT_DEPENDENCY_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/cpkt/deps}`. Both survive `make clean`; disposable extracted dependency source state is kept under the active CMake build tree. The lifecycle helpers in `scripts/` are vendored from `pkt-systems-cmake-lifecycle`; keep its five companion helpers together when updating. Discovery and environment queries never provision tools; the outer configure entrypoint prepares them explicitly. Host Bash 4.4 or newer is required. Existing build caches are reconfigured when the selected compiler changes.
- Cross test execution requires `qemu-aarch64` and `qemu-arm`; each uses the matching Bootlin sysroot.
- `arm64-apple-darwin` expects osxcross under `OSXCROSS_ROOT` or `$HOME/.local/cross/osxcross`. It selects the newest complete Darwin 25.x prefix (including minor versions such as `25.4`); set `CPKT_OSXCROSS_HOST` to pin an exact installed prefix.
- Darwin readiness also includes the pinned Linux-host MIG helper in the cpkt toolchain cache. `scripts/cpkt-toolchains.sh ensure arm64-apple-darwin` prepares it with Bootlin; cold preparation requires host `bison` and `flex`. The resolver reports its `mig`, `migcom` and `mig_root` paths. `make build-release` and the release matrix select an installed osxcross collection even before MIG is cached, then prepare that prerequisite through the outer workflow. Compiler probes remain read-only.

Single-target examples:

```sh
./scripts/configure_cmake.sh --preset aarch64-linux-gnu-release
cmake --build --preset aarch64-linux-gnu-release
ctest --preset aarch64-linux-gnu-release

cmake --build build/aarch64-linux-gnu-release --target package-archive
```

Each target archive in `dist/` now contains the public headers, the shared
library, and the static library for that target variant.

After the full matrix runs, `dist/` also contains `libpslog-<version>-CHECKSUMS`
with `sha256sum` output for every shipped release file except the checksum file
itself. The listed filenames are bare archive names, not `dist/...` paths.

## Benchmarks

There are two benchmark layers:

- pure C benchmarks in [bench](bench)
- Go-vs-C comparison harness in [gobencher](gobencher)

Useful commands:

```sh
make benchmarks-c
make perf-gate
make bench-freeze-baseline
```

`make perf-gate` compares the fastest result from five pinned C samples for
each metric with this host's baseline. Set `PSLOG_PERF_C_SAMPLES` to change the
sample count when measuring on a controlled machine.

The benchmark suite covers:

- fixed synthetic workloads
- production-shaped workloads
- prebuilt field-array paths
- per-call field rebuild paths
- `with()`-based production shapes
- `kvfmt` convenience paths
- optional benchmark-only `liblogger` comparison on JSON
- optional benchmark-only `Quill` comparison on JSON

The external comparisons are intentionally scoped:

- `liblogger` is kept only as a simple JSON baseline. It emits JSON, but it is still not a full peer for modern container-style structured JSONL workloads: no `with()`-style persistent fields, no native boolean field type, and a narrower JSON surface than `libpslog`. It is also far slower on the production-shaped benchmark, so this is not just a feature gap.
- `Quill` is kept only as an opt-in negative comparison. Its `JsonSink` is not first-class structured JSON for modern container-style JSONL logging: it does not preserve typed structured fields, it stringifies named arguments internally, and it lacks persistent structured-field attachment. It is also far slower on the production-shaped benchmark once forced into this workload class. If you are evaluating JSON loggers for Kubernetes, serverless, or other container-first workloads, do not treat Quill's `JsonSink` as the same kind of JSON logging product as `libpslog`.

See [bench/README.md](bench/README.md) and [gobencher/README.md](gobencher/README.md) for naming and interpretation details.

## Environment Configuration

`pslog_new_from_env()` reads settings like:

- `LOG_MODE`
- `LOG_LEVEL`
- `LOG_DISABLE_TIMESTAMP`
- `LOG_VERBOSE_FIELDS`
- `LOG_NO_COLOR`
- `LOG_FORCE_COLOR`
- `LOG_PALETTE`
- `LOG_OUTPUT`
- `LOG_OUTPUT_FILE_MODE`
- `LOG_TIME_FORMAT`
- `LOG_UTC`

Example:

```c
pslog_config seed;
pslog_logger *log;

pslog_default_config(&seed);
seed.output = pslog_output_from_fp(stdout, 0);

log = pslog_new_from_env("LOG_", &seed);
log->infof(log, "hello", "key=%s", "value");
log->destroy(log);
```

## Operational Notes

- Shared logging through the same logger tree is thread-safe.
- `infof/debugf/...` do structured `kvfmt`; they are not `printf` message formatters.
- Normal emission stays allocation-free on the hot path for ordinary line sizes.
- Long lines are streamed in chunks instead of truncated.
- Trusted strings are explicit. Untrusted strings are escaped normally.
- The runtime `.so` and static `.a` packages both ship the public header.
