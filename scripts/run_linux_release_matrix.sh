#!/usr/bin/env bash

set -eu

repo_root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"

require_command() {
    if ! command -v "$1" >/dev/null 2>&1; then
        printf 'missing required command: %s\n' "$1" >&2
        exit 1
    fi
}

require_file() {
    if [ ! -e "$1" ]; then
        printf 'missing required file: %s\n' "$1" >&2
        exit 1
    fi
}

run_target() {
    preset="$1"
    archive_target="$2"

    printf '\n== %s ==\n' "$preset"
    "$repo_root/scripts/run_timed.sh" "${preset}:configure" "$repo_root/scripts/configure_cmake.sh" --preset "$preset"
    "$repo_root/scripts/run_timed.sh" "${preset}:build" cmake --build --preset "$preset"
    "$repo_root/scripts/run_timed.sh" "${preset}:test" ctest --preset "$preset"
    "$repo_root/scripts/run_timed.sh" "${preset}:package" cmake --build "build/$preset" --target "$archive_target"
}

run_build_only_target() {
    preset="$1"
    archive_target="$2"

    printf '\n== %s ==\n' "$preset"
    "$repo_root/scripts/run_timed.sh" "${preset}:configure" "$repo_root/scripts/configure_cmake.sh" --preset "$preset"
    "$repo_root/scripts/run_timed.sh" "${preset}:build" cmake --build --preset "$preset"
    "$repo_root/scripts/run_timed.sh" "${preset}:package" cmake --build "build/$preset" --target "$archive_target"
}

require_command cmake
require_command ctest
require_command make
lua_rocks="${LUA_ROCKS:-luarocks}"
require_command "$lua_rocks"
require_command qemu-aarch64
require_command qemu-arm

darwin_toolchain=""
darwin_status="$("$repo_root/scripts/cpkt-toolchains.sh" discover arm64-apple-darwin)"
case $'\n'"$darwin_status"$'\n' in
  *$'\nstatus=ready\n'*)
    darwin_toolchain="$(printf '%s\n' "$darwin_status" | sed -n 's/^cc=//p' | tail -n 1)"
    if [ ! -x "$darwin_toolchain" ]; then
        printf 'Darwin resolver reported ready but its compiler is unavailable: %s\n' "$darwin_toolchain" >&2
        exit 1
    fi
    ;;
esac

cd "$repo_root"

"$repo_root/scripts/run_timed.sh" release-matrix:host-configure "$repo_root/scripts/configure_cmake.sh" --preset host
"$repo_root/scripts/run_timed.sh" release-matrix:clean-dist cmake --build build/host --target package-clean-dist

run_target x86_64-linux-gnu-release package-archive
run_target x86_64-linux-musl-release package-archive
run_target aarch64-linux-gnu-release package-archive
run_target aarch64-linux-musl-release package-archive
run_target armhf-linux-gnu-release package-archive
run_target armhf-linux-musl-release package-archive
if [ -x "$darwin_toolchain" ]; then
    run_build_only_target arm64-apple-darwin-release package-archive
else
    printf '\n== arm64-apple-darwin-release ==\n'
    printf 'Skipping Darwin release target: complete osxcross toolchain is not available.\n'
    printf '%s\n' "$darwin_status"
fi

"$repo_root/scripts/run_timed.sh" release-matrix:single-header cmake --build build/host --target package-single-header
"$repo_root/scripts/run_timed.sh" release-matrix:source-smoke cmake -DPSLOG_ROOT="$repo_root" -DPSLOG_BINARY_DIR="$repo_root/build/host" -DPSLOG_VERSION="$(./lua/scripts/release_version.sh)" -DPSLOG_TOOLCHAIN_RELATIVE=cmake/toolchains/linux-x86_64-gnu.cmake -P tests/source_archive_smoke_test.cmake
"$repo_root/scripts/run_timed.sh" release-matrix:lua-artifacts env LUA_ROCKS="$lua_rocks" make release-lua-artifacts
"$repo_root/scripts/run_timed.sh" release-matrix:checksums cmake --build build/host --target package-checksums
if [ -n "$darwin_toolchain" ]; then
    "$repo_root/scripts/run_timed.sh" release-matrix:privacy ./scripts/verify_release_privacy.sh --build-dir build/host --target-id x86_64-linux-gnu --darwin-build-dir build/arm64-apple-darwin-release
else
    "$repo_root/scripts/run_timed.sh" release-matrix:privacy ./scripts/verify_release_privacy.sh --build-dir build/host --target-id x86_64-linux-gnu
fi

printf '\nRelease matrix completed successfully.\n'
