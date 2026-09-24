#!/usr/bin/env bash
set -euo pipefail

repo_root=$1
fixture_root=$(mktemp -d)
cleanup() { rm -rf "$fixture_root"; }
trap cleanup EXIT HUP INT TERM

source "$repo_root/scripts/cpkt-toolchains.sh"

archive_source="$fixture_root/fixture.tar.xz"
stage_root="$fixture_root/stage/fixture"
mkdir -p "$stage_root/bin"
touch "$stage_root/.ready"
tar -C "$fixture_root/stage" -cJf "$archive_source" fixture
expected_sha=$(sha256_file "$archive_source")
export CPKT_TOOLCHAIN_CACHE="$fixture_root/cache"
unset CPKT_TOOLCHAIN_LOCK_TIMEOUT

bootlin_values() {
  printf 'test|fixture|%s|fake|sysroot|%s\n' "$expected_sha" "$CPKT_TOOLCHAIN_CACHE/roots/fixture"
}
lock_held=0
ready_checks=0
reject_lock=0
allow_unlocked_ready=0
flock() {
  if [[ "${1:-}" == '-w' ]]; then
    [[ "${2:-}" == 600 ]] || { printf 'toolchain cache lock did not use the lifecycle timeout\n' >&2; return 1; }
    shift 2
  fi
  [[ "$reject_lock" -eq 0 ]] || { printf 'ready toolchain unexpectedly acquired a cache lock\n' >&2; return 1; }
  if [[ "${1:-}" == '-u' ]]; then
    lock_held=0
  else
    lock_held=1
  fi
}
bootlin_ready() {
  ready_checks=$((ready_checks + 1))
  if [[ "$ready_checks" -gt 1 && "$lock_held" -ne 1 && "$allow_unlocked_ready" -ne 1 ]]; then
    printf 'toolchain readiness check ran outside its cache lock\n' >&2
    return 1
  fi
  [[ -f "$1/.ready" ]]
}
download_calls=0
download_file() {
  [[ "$lock_held" -eq 1 ]] || { printf 'toolchain download ran outside its cache lock\n' >&2; return 1; }
  download_calls=$((download_calls + 1))
  cp "$archive_source" "$2"
}

mkdir -p "$CPKT_TOOLCHAIN_CACHE/archives"
printf 'corrupt cached archive\n' >"$CPKT_TOOLCHAIN_CACHE/archives/fixture.tar.xz"
install_bootlin fixture

[[ "$download_calls" -eq 1 ]] || { printf 'corrupt cache entry was not reacquired\n' >&2; exit 1; }
[[ "$(sha256_file "$CPKT_TOOLCHAIN_CACHE/archives/fixture.tar.xz")" == "$expected_sha" ]] || {
  printf 'reacquired cache entry was not verified\n' >&2
  exit 1
}
[[ -f "$CPKT_TOOLCHAIN_CACHE/roots/fixture/.ready" ]] || {
  printf 'reacquired toolchain archive was not extracted\n' >&2
  exit 1
}
[[ -f "$CPKT_TOOLCHAIN_CACHE/locks/bootlin-fixture.lock" ]] || {
  printf 'toolchain cache lock was not created\n' >&2
  exit 1
}
[[ "$lock_held" -eq 0 ]] || {
  printf 'toolchain cache lock was not released after installation\n' >&2
  exit 1
}
reject_lock=1
allow_unlocked_ready=1
install_bootlin fixture

darwin_root="$fixture_root/osxcross"
darwin_bin="$darwin_root/bin"
darwin_host=arm64-apple-darwin25.4
mkdir -p "$darwin_bin"
for tool in clang clang++ ld ar ranlib strip nm otool; do
  : >"$darwin_bin/$darwin_host-$tool"
  chmod +x "$darwin_bin/$darwin_host-$tool"
done
unset CPKT_OSXCROSS_HOST
if OSXCROSS_ROOT="$darwin_root" osxcross_candidate >/dev/null; then
  printf 'Darwin discovery accepted a toolchain without CMake-required SDK prerequisites\n' >&2
  exit 1
fi
: >"$darwin_bin/$darwin_host-install_name_tool"
chmod +x "$darwin_bin/$darwin_host-install_name_tool"
mkdir -p "$darwin_root/SDK/MacOSX15.sdk/usr/include"
darwin_candidate=$(OSXCROSS_ROOT="$darwin_root" osxcross_candidate)
[[ "$darwin_candidate" == "osxcross|$darwin_root|$darwin_host" ]] || {
  printf 'Darwin discovery did not accept the installed minor-version target prefix: %s\n' "$darwin_candidate" >&2
  exit 1
}
newer_host=arm64-apple-darwin25.10
for tool in clang clang++ ld ar ranlib strip nm otool install_name_tool; do
  cp "$darwin_bin/$darwin_host-$tool" "$darwin_bin/$newer_host-$tool"
done
cp "$darwin_bin/$newer_host-clang" "$darwin_bin/$newer_host-cmake-clang"
cp "$darwin_bin/$newer_host-clang" "$darwin_bin/arm64-apple-darwin25.11-clang"
darwin_candidate=$(OSXCROSS_ROOT="$darwin_root" osxcross_candidate)
[[ "$darwin_candidate" == "osxcross|$darwin_root|$newer_host" ]] || {
  printf 'Darwin discovery did not select the newest complete 25.x prefix: %s\n' "$darwin_candidate" >&2
  exit 1
}
darwin_candidate=$(OSXCROSS_ROOT="$darwin_root" CPKT_OSXCROSS_HOST="$darwin_host" osxcross_candidate)
[[ "$darwin_candidate" == "osxcross|$darwin_root|$darwin_host" ]] || {
  printf 'Darwin discovery did not honor the exact prefix pin: %s\n' "$darwin_candidate" >&2
  exit 1
}
if OSXCROSS_ROOT="$darwin_root" CPKT_OSXCROSS_HOST=arm64-apple-darwin25.11 osxcross_candidate >/dev/null; then
  printf 'Darwin discovery accepted an incomplete pinned prefix\n' >&2
  exit 1
fi

# macOS system Bash has no mapfile, and its sort has no GNU -V option.
mapfile() { return 127; }
sort() { return 2; }
darwin_candidate=$(OSXCROSS_ROOT="$darwin_root" osxcross_candidate)
[[ "$darwin_candidate" == "osxcross|$darwin_root|$newer_host" ]] || {
  printf 'Darwin discovery required mapfile or GNU sort -V: %s\n' "$darwin_candidate" >&2
  exit 1
}
