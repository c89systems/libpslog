#!/usr/bin/env bash
set -euo pipefail

repo_root=$1
real_make=$(command -v make)
fixture_root=$(mktemp -d "$repo_root/build/release-matrix-test.XXXXXXXX")
cleanup() { rm -rf "$fixture_root"; }
trap cleanup EXIT HUP INT TERM

fake_repo="$fixture_root/repo"
fake_bin="$fixture_root/bin"
fake_home="$fixture_root/home"
mkdir -p "$fake_repo/scripts" "$fake_repo/lua/scripts" "$fake_bin" "$fake_home"
for command in bash grep sed tail; do
  ln -s "$(command -v "$command")" "$fake_bin/$command"
done
cp "$repo_root/scripts/run_linux_release_matrix.sh" "$fake_repo/scripts/"
cp "$repo_root/Makefile" "$fake_repo/"

printf '%s\n' '#!/bin/sh' 'label=$1; shift' 'exec "$@"' >"$fake_repo/scripts/run_timed.sh"
printf '%s\n' '#!/bin/sh' 'printf "0.0.0\\n"' >"$fake_repo/lua/scripts/release_version.sh"
printf '%s\n' '#!/bin/sh' 'exit 0' >"$fake_repo/scripts/verify_release_privacy.sh"
printf '%s\n' '#!/bin/sh' 'exit 0' >"$fake_repo/scripts/configure_cmake.sh"
printf '%s\n' '#!/bin/sh' \
  '[ "$2" = arm64-apple-darwin ] || exit 1' \
  'if [ "$1" = ensure ]; then : > "$MIG_PREPARED"; [ "${FAIL_MIG_ENSURE:-0}" = 0 ] || exit 93; printf "%s\n" status=ready "cc=$DARWIN_CC"; exit 0; fi' \
  'if [ "$1" = discover ]; then' \
  '  if [ "$DARWIN_STATUS" = needs-mig ]; then printf "%s\n" status=missing "root=$OSXCROSS_ROOT" prefix=arm64-apple-darwin25 mig_revision=fixture; else printf "%s\n" target=arm64-apple-darwin "status=$DARWIN_STATUS" "cc=$DARWIN_CC"; fi; exit 0' \
  'fi' \
  'exit 1' >"$fake_repo/scripts/cpkt-toolchains.sh"
printf '%s\n' '#!/bin/sh' \
  'printf "%s\n" "$*" >> "$MATRIX_TRACE"' \
  'for arg in "$@"; do case "$arg" in *arm64-apple-darwin*) [ "$EXPECT_DARWIN" = 1 ] || exit 92 ;; esac; done' \
  'exit 0' >"$fake_bin/cmake"
chmod +x "$fake_bin/cmake"
for command in ctest qemu-aarch64 qemu-arm; do
  printf '%s\n' '#!/bin/sh' 'exit 0' >"$fake_bin/$command"
  chmod +x "$fake_bin/$command"
done
printf '%s\n' '#!/bin/sh' \
  'while [ "$#" -gt 0 ]; do case "$1" in *=*) export "$1"; shift ;; *) break ;; esac; done' \
  'exec "$@"' >"$fake_bin/env"
printf '%s\n' '#!/bin/sh' \
  '[ "$1" = "--" ] && shift' \
  'printf "%s\\n" "${1%/*}"' >"$fake_bin/dirname"
printf '%s\n' '#!/bin/sh' \
  '[ "$LUA_ROCKS" = "$EXPECTED_LUA_ROCKS" ] || exit 91' \
  'exit 0' >"$fake_bin/make"
printf '%s\n' '#!/bin/sh' 'exit 0' >"$fake_bin/custom-luarocks"
chmod +x "$fake_repo/scripts/run_timed.sh" "$fake_repo/lua/scripts/release_version.sh" \
  "$fake_repo/scripts/verify_release_privacy.sh" "$fake_repo/scripts/cpkt-toolchains.sh" \
  "$fake_repo/scripts/configure_cmake.sh" \
  "$fake_bin/env" "$fake_bin/dirname" "$fake_bin/make" "$fake_bin/custom-luarocks"

mkdir -p "$fake_home/.local/cross/osxcross/bin"
printf '%s\n' '#!/bin/sh' 'exit 0' >"$fake_home/.local/cross/osxcross/bin/arm64-apple-darwin25-clang"
chmod +x "$fake_home/.local/cross/osxcross/bin/arm64-apple-darwin25-clang"

# Deliberately omit a command named "luarocks": the matrix must accept the
# explicit lifecycle override and pass it into the Lua artifact target.
export MATRIX_TRACE="$fixture_root/matrix-trace" EXPECTED_LUA_ROCKS="$fake_bin/custom-luarocks"
export DARWIN_CC="$fake_home/.local/cross/osxcross/bin/arm64-apple-darwin25-clang"
export OSXCROSS_ROOT="$fake_home/.local/cross/osxcross" MIG_PREPARED="$fixture_root/mig-prepared"
for status in missing ready-but-unavailable ready needs-mig; do
  export DARWIN_STATUS=$status EXPECT_DARWIN=0
  [[ "$status" != ready && "$status" != needs-mig ]] || EXPECT_DARWIN=1
  rm -f "$MATRIX_TRACE" "$MIG_PREPARED"
  PATH="$fake_bin" HOME="$fake_home" LUA_ROCKS="$fake_bin/custom-luarocks" \
    /bin/bash "$fake_repo/scripts/run_linux_release_matrix.sh" > "$fixture_root/output" 2>&1 || {
      cat "$fixture_root/output" >&2; exit 1;
    }
  if [[ "$EXPECT_DARWIN" == 1 ]]; then
    grep -Fq 'arm64-apple-darwin-release' "$MATRIX_TRACE" || {
      echo "matrix skipped selected Darwin collection: $status" >&2; exit 1;
    }
  elif grep -Fq 'arm64-apple-darwin-release' "$MATRIX_TRACE"; then
    echo "matrix accepted non-ready Darwin status: $status" >&2; exit 1
  fi
  if [[ "$status" == needs-mig ]]; then
    [[ -f "$MIG_PREPARED" ]] || { echo 'matrix skipped host MIG preparation' >&2; exit 1; }
  else
    [[ ! -e "$MIG_PREPARED" ]] || { echo 'matrix prepared an unselected or ready target' >&2; exit 1; }
  fi
  if grep -Fq -- '--target package-source' "$MATRIX_TRACE"; then
    echo 'matrix generated source before the source smoke producer' >&2; exit 1
  fi
  [[ $(grep -c -- '-P tests/source_archive_smoke_test.cmake' "$MATRIX_TRACE") == 1 ]]
  if grep -Eq -- '^--build build/.*linux.* --target package-archive$|^--build build/host --target package-single-header$' "$MATRIX_TRACE"; then
    echo 'matrix repeated a package or header producer owned by its test/checksum gate' >&2; exit 1
  fi
  # The public build-release entrypoint must select installed osxcross before
  # its configure wrapper prepares the missing MIG prerequisite, too.
  if [[ "$status" == needs-mig || "$status" == missing ]]; then
    rm -f "$MATRIX_TRACE"
    PATH="$fake_bin" HOME="$fake_home" "$real_make" -C "$fake_repo" --no-print-directory build-release \
      > "$fixture_root/output" 2>&1 || { cat "$fixture_root/output" >&2; exit 1; }
    if [[ "$status" == needs-mig ]]; then
      grep -Fq 'arm64-apple-darwin-release' "$MATRIX_TRACE" || {
        echo 'build-release skipped installed osxcross awaiting MIG' >&2; exit 1;
      }
    elif grep -Fq 'arm64-apple-darwin-release' "$MATRIX_TRACE"; then
      echo 'build-release selected unavailable osxcross' >&2; exit 1
    fi
  fi
done

export DARWIN_STATUS=needs-mig FAIL_MIG_ENSURE=1
rm -f "$MATRIX_TRACE" "$MIG_PREPARED"
if PATH="$fake_bin" HOME="$fake_home" LUA_ROCKS="$fake_bin/custom-luarocks" \
    /bin/bash "$fake_repo/scripts/run_linux_release_matrix.sh" > "$fixture_root/output" 2>&1; then
  echo 'matrix ignored failed host MIG preparation' >&2; exit 1
fi
[[ -f "$MIG_PREPARED" && ! -e "$MATRIX_TRACE" ]] || { echo 'MIG failure did not stop before producers' >&2; exit 1; }
unset FAIL_MIG_ENSURE

# A claimed ready collection with a missing compiler is broken, not optional.
export DARWIN_STATUS=ready DARWIN_CC="$fixture_root/missing-clang"
rm -f "$MATRIX_TRACE"
if PATH="$fake_bin" HOME="$fake_home" LUA_ROCKS="$fake_bin/custom-luarocks" \
    /bin/bash "$fake_repo/scripts/run_linux_release_matrix.sh" > "$fixture_root/output" 2>&1; then
  echo 'matrix skipped a broken ready Darwin collection' >&2; exit 1
fi
[[ ! -e "$MATRIX_TRACE" ]] || { echo 'broken tools started producers' >&2; exit 1; }
printf 'release matrix readiness, source producer and LuaRocks override passed\n'
