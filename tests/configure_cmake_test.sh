#!/usr/bin/env bash
set -euo pipefail

repo_root=${1:?repo root is required}
configured_host_override=${2:-${PSLOG_ALLOW_HOST_COMPILER:-}}
if [[ -n "$configured_host_override" ]]; then
    export PSLOG_ALLOW_HOST_COMPILER="$configured_host_override"
fi
work_root="$repo_root/build/configure-cmake-test"
host_build="$work_root/host"
lua_build="$work_root/lua"
host_platform=$(uname -s)

host_compiler_override_requested() {
    case "${PSLOG_ALLOW_HOST_COMPILER:-}" in
        1|ON|on|TRUE|true|YES|yes) return 0 ;;
    esac
    return 1
}

linux_host_target() {
    case "$(uname -m)" in
        x86_64|amd64) printf '%s\n' x86_64-linux-gnu ;;
        aarch64|arm64) printf '%s\n' aarch64-linux-gnu ;;
        armv7l) printf '%s\n' armhf-linux-gnu ;;
        *) printf 'unsupported Linux host architecture: %s\n' "$(uname -m)" >&2; exit 1 ;;
    esac
}

assert_configured_compiler() {
    local name=$1 compiler=$2 expected
    case "$host_platform" in
        Linux)
            if host_compiler_override_requested; then
                [[ -n "$compiler" ]] || {
                    printf '%s compiler did not select the requested host compiler: %s\n' "$name" "$compiler" >&2
                    exit 1
                }
                return
            fi
            expected=$("$repo_root/scripts/cpkt-toolchains.sh" discover "$(linux_host_target)" |
                sed -n 's/^cc=//p' | tail -n 1)
            [[ -n "$expected" && "$compiler" == "$expected" ]] || {
                printf '%s compiler does not match the resolved Bootlin compiler: %s\n' "$name" "$compiler" >&2
                exit 1
            }
            ;;
        Darwin) [[ -n "$compiler" ]] ;;
        *) printf 'unsupported host platform: %s\n' "$host_platform" >&2; exit 1 ;;
    esac
}

assert_cache_policy() {
    local name=$1 build_dir=$2 output=$3
    case "$host_platform" in
        Linux)
            printf '%s\n' "$output" | grep -F 'discarding stale compiler state' >/dev/null
            [[ ! -e "$build_dir/CMakeFiles/stale-marker" ]]
            ;;
        Darwin)
            if printf '%s\n' "$output" | grep -F 'discarding stale compiler state' >/dev/null; then
                printf 'configure_cmake discarded native macOS cache state for %s\n' "$name" >&2
                exit 1
            fi
            [[ -e "$build_dir/CMakeFiles/stale-marker" ]] || {
                printf 'configure_cmake did not preserve native macOS cache state for %s\n' "$name" >&2
                exit 1
            }
            ;;
    esac
}

rm -rf "$work_root"
trap 'rm -rf "$work_root"' EXIT
mkdir -p "$host_build/CMakeFiles"
printf 'CMAKE_C_COMPILER:FILEPATH=/usr/bin/cc\n' > "$host_build/CMakeCache.txt"
printf 'stale\n' > "$host_build/CMakeFiles/stale-marker"

host_output=$("$repo_root/scripts/configure_cmake.sh" \
    --source . --build "${host_build#$repo_root/}" --target host 2>&1)
assert_cache_policy host "$host_build" "$host_output"
host_cc=$(sed -n 's/^CMAKE_C_COMPILER:[^=]*=//p' "$host_build/CMakeCache.txt" | tail -n 1)
assert_configured_compiler host "$host_cc"

# Cache-policy transitions are exercised with a minimal CMake stand-in.  The
# real host configure above proves the resolver contract without repeatedly
# configuring the full project in this focused script test.
policy_repo="$work_root/policy-repo"
policy_bin="$policy_repo/bin"
policy_build="$policy_repo/build/host"
policy_cache="$policy_repo/cache"
mkdir -p "$policy_repo/scripts" "$policy_bin" "$policy_build/CMakeFiles"
cp "$repo_root/scripts/configure_cmake.sh" "$policy_repo/scripts/"
printf '%s\n' '#!/usr/bin/env bash' \
    'case "$1" in' \
    'ensure) mkdir -p "$CPKT_TOOLCHAIN_CACHE"; touch "$CPKT_TOOLCHAIN_CACHE/prepared" ;;' \
    'discover) [[ -f "$CPKT_TOOLCHAIN_CACHE/prepared" ]] || exit 41; printf "cc=%s/x86_64-gcc\\n" "$CPKT_TOOLCHAIN_CACHE" ;;' \
    'esac' \
    > "$policy_repo/scripts/cpkt-toolchains.sh"
printf '%s\n' '#!/usr/bin/env bash' \
    'if [[ "$1" == -E ]]; then shift; [[ "$1" == rm ]] || exit 1; shift; command rm "$@"; exit; fi' \
    "build_dir='$policy_build'" \
    'if [[ "${PSLOG_ALLOW_HOST_COMPILER:-}" =~ ^(1|ON|on|TRUE|true|YES|yes)$ ]]; then' \
    '    printf "CMAKE_C_COMPILER:FILEPATH=/usr/bin/cc\\nPSLOG_ALLOW_HOST_COMPILER:BOOL=ON\\n" > "$build_dir/CMakeCache.txt"' \
    'else' \
    '    [[ -f "$CPKT_TOOLCHAIN_CACHE/prepared" ]] || { echo "Bootlin was not prepared" >&2; exit 42; }' \
    '    printf "CMAKE_C_COMPILER:FILEPATH=%s/x86_64-gcc\\nPSLOG_BOOTLIN_TOOLCHAIN:BOOL=TRUE\\n" "$CPKT_TOOLCHAIN_CACHE" > "$build_dir/CMakeCache.txt"' \
    'fi' \
    > "$policy_bin/cmake"
printf '%s\n' '#!/usr/bin/env bash' \
    'case "${1:-}" in -s) printf "Linux\\n" ;; -m) printf "x86_64\\n" ;; *) exit 1 ;; esac' \
    > "$policy_bin/uname"
chmod +x "$policy_repo/scripts/"*.sh "$policy_bin/cmake" "$policy_bin/uname"
printf 'CMAKE_C_COMPILER:FILEPATH=/toolchains/x86_64-gcc\nPSLOG_BOOTLIN_TOOLCHAIN:BOOL=TRUE\n' > "$policy_build/CMakeCache.txt"
printf 'stale\n' > "$policy_build/CMakeFiles/stale-marker"
host_override_output=$(PSLOG_ALLOW_HOST_COMPILER=1 CPKT_TOOLCHAIN_CACHE="$policy_cache" PATH="$policy_bin:$PATH" \
    "$policy_repo/scripts/configure_cmake.sh" --preset host 2>&1)
printf '%s\n' "$host_override_output" | grep -F 'discarding stale compiler state' >/dev/null
grep -Eq '^PSLOG_ALLOW_HOST_COMPILER:BOOL=ON$' "$policy_build/CMakeCache.txt"
! grep -Eq '^PSLOG_BOOTLIN_TOOLCHAIN:BOOL=TRUE$' "$policy_build/CMakeCache.txt"
[[ ! -e "$policy_build/CMakeFiles/stale-marker" ]]
[[ ! -e "$policy_cache" ]] || { echo 'host override prepared Bootlin' >&2; exit 1; }
mkdir -p "$policy_build/CMakeFiles"
printf 'stale\n' > "$policy_build/CMakeFiles/stale-marker"
host_restore_output=$(PSLOG_ALLOW_HOST_COMPILER=0 CPKT_TOOLCHAIN_CACHE="$policy_cache" PATH="$policy_bin:$PATH" \
    "$policy_repo/scripts/configure_cmake.sh" --preset host 2>&1) || {
      printf '%s\n' "$host_restore_output" >&2; exit 1;
    }
printf '%s\n' "$host_restore_output" | grep -F 'discarding stale compiler state' >/dev/null
grep -Eq '^PSLOG_BOOTLIN_TOOLCHAIN:BOOL=TRUE$' "$policy_build/CMakeCache.txt"
[[ ! -e "$policy_build/CMakeFiles/stale-marker" ]]
[[ -f "$policy_cache/prepared" ]]

mkdir -p "$policy_build/CMakeFiles"
printf 'CMAKE_C_COMPILER:FILEPATH=/cache-one/x86_64-gcc\nPSLOG_BOOTLIN_TOOLCHAIN:BOOL=TRUE\n' > "$policy_build/CMakeCache.txt"
printf 'stale\n' > "$policy_build/CMakeFiles/stale-marker"
cache_root_output=$(PSLOG_ALLOW_HOST_COMPILER=0 CPKT_TOOLCHAIN_CACHE="$policy_repo/cache-two" PATH="$policy_bin:$PATH" \
    "$policy_repo/scripts/configure_cmake.sh" --preset host 2>&1)
printf '%s\n' "$cache_root_output" | grep -F 'discarding stale compiler state' >/dev/null
grep -Fxq "CMAKE_C_COMPILER:FILEPATH=$policy_repo/cache-two/x86_64-gcc" "$policy_build/CMakeCache.txt"
[[ ! -e "$policy_build/CMakeFiles/stale-marker" ]]

mkdir -p "$lua_build/CMakeFiles"
printf 'CMAKE_C_COMPILER:FILEPATH=/usr/bin/cc\n' > "$lua_build/CMakeCache.txt"
printf 'stale\n' > "$lua_build/CMakeFiles/stale-marker"
lua_output=$("$repo_root/scripts/configure_cmake.sh" \
    --source cmake/lua --build "${lua_build#$repo_root/}" --target host -- \
    -DCMAKE_BUILD_TYPE=Release 2>&1)
assert_cache_policy lua "$lua_build" "$lua_output"
lua_cc=$(sed -n 's/^CMAKE_C_COMPILER:[^=]*=//p' "$lua_build/CMakeCache.txt" | tail -n 1)
assert_configured_compiler lua "$lua_cc"

fuzz_repo="$work_root/fuzz-repo"
fuzz_build="$fuzz_repo/build/fuzz"
mkdir -p "$fuzz_repo/scripts" "$fuzz_build/CMakeFiles" "$fuzz_repo/fuzz-bin"
cp "$repo_root/scripts/configure_cmake.sh" "$fuzz_repo/scripts/"
printf '%s\n' '#!/usr/bin/env bash' \
    "printf 'cc=%s\\n' '$fuzz_repo/fuzz-bin/cpkt-afl-gcc'" \
    > "$fuzz_repo/scripts/cpkt-aflpp.sh"
printf '%s\n' '#!/usr/bin/env bash' 'exit 1' > "$fuzz_repo/scripts/cpkt-toolchains.sh"
printf '%s\n' '#!/usr/bin/env bash' 'exit 0' > "$fuzz_repo/cmake"
chmod +x "$fuzz_repo/scripts/"*.sh "$fuzz_repo/cmake"
printf 'CMAKE_C_COMPILER:FILEPATH=%s\n' "$fuzz_repo/fuzz-bin/cpkt-afl-gcc" > "$fuzz_build/CMakeCache.txt"
printf 'matching\n' > "$fuzz_build/CMakeFiles/cache-marker"
fuzz_output=$(PATH="$fuzz_repo:$PATH" "$fuzz_repo/scripts/configure_cmake.sh" --preset fuzz 2>&1)
if printf '%s\n' "$fuzz_output" | grep -F 'discarding stale compiler state' >/dev/null ||
   [[ ! -e "$fuzz_build/CMakeFiles/cache-marker" ]]; then
    printf 'configure_cmake discarded a matching AFL++ compiler cache\n' >&2
    exit 1
fi

# The macOS system Bash is 3.2.  It treats an empty array as unset under
# `set -u`, so a normal preset invocation must not expand forwarded arguments
# when there are none.
darwin_repo="$work_root/darwin-repo"
darwin_bin="$darwin_repo/bin"
darwin_args="$darwin_repo/cmake-arguments"
mkdir -p "$darwin_repo/scripts" "$darwin_bin"
cp "$repo_root/scripts/configure_cmake.sh" "$darwin_repo/scripts/"
printf '%s\n' '#!/usr/bin/env bash' \
    'case "$1" in -s) printf "Darwin\\n" ;; -m) printf "arm64\\n" ;; *) exit 1 ;; esac' \
    > "$darwin_bin/uname"
printf '%s\n' '#!/usr/bin/env bash' \
    "printf '%s\\n' \"\$@\" > '$darwin_args'" \
    > "$darwin_bin/cmake"
chmod +x "$darwin_bin/uname" "$darwin_bin/cmake"
PATH="$darwin_bin:$PATH" "$darwin_repo/scripts/configure_cmake.sh" --preset host
cmp -s <(printf '%s\n' --preset host) "$darwin_args" || {
    printf 'configure_cmake did not preserve an empty forwarded argument list on Darwin\n' >&2
    exit 1
}

direct_repo="$work_root/direct-repo"
direct_args="$direct_repo/cmake-arguments"
direct_bin="$direct_repo/bin"
mkdir -p "$direct_repo/scripts" "$direct_repo/cmake/toolchains" "$direct_bin"
cp "$repo_root/scripts/configure_cmake.sh" "$direct_repo/scripts/"
printf '%s\n' '#!/usr/bin/env bash' \
    'case "$1" in ensure) exit 0 ;; discover) printf "cc=/toolchains/aarch64-gcc\\n" ;; esac' \
    > "$direct_repo/scripts/cpkt-toolchains.sh"
printf '%s\n' '#!/usr/bin/env bash' \
    "printf '%s\\n' \"\$@\" > '$direct_args'" \
    > "$direct_bin/cmake"
chmod +x "$direct_repo/scripts/"*.sh "$direct_bin/cmake"
PATH="$direct_bin:$PATH" "$direct_repo/scripts/configure_cmake.sh" \
    --source . --build build/direct-aarch64 --target aarch64-linux-gnu -- \
    -DCMAKE_TOOLCHAIN_FILE=/wrong-toolchain
expected_toolchain="-DCMAKE_TOOLCHAIN_FILE=$direct_repo/cmake/toolchains/linux-aarch64-gnu.cmake"
[[ "$(tail -n 1 "$direct_args")" == "$expected_toolchain" ]] || {
    printf 'direct configuration did not select the requested aarch64 toolchain\n' >&2
    exit 1
}
absolute_build="$direct_repo/build/absolute-aarch64"
PATH="$direct_bin:$PATH" "$direct_repo/scripts/configure_cmake.sh" \
    --source "$direct_repo" --build "$absolute_build" --target aarch64-linux-gnu
[[ "$(sed -n '2p' "$direct_args")" == "$direct_repo" &&
   "$(sed -n '4p' "$direct_args")" == "$absolute_build" ]] || {
    printf 'direct configuration did not preserve absolute source and build paths\n' >&2
    exit 1
}

arm_host_repo="$work_root/arm-host-repo"
arm_host_bin="$arm_host_repo/bin"
mkdir -p "$arm_host_repo/scripts" "$arm_host_bin"
cp "$repo_root/scripts/configure_cmake.sh" "$arm_host_repo/scripts/"
printf '%s\n' '#!/usr/bin/env bash' \
    'case "${1:-}" in -s) printf "Linux\\n" ;; -m) printf "aarch64\\n" ;; *) exit 1 ;; esac' \
    > "$arm_host_bin/uname"
printf '%s\n' '#!/usr/bin/env bash' 'exit 90' > "$arm_host_repo/scripts/cpkt-toolchains.sh"
printf '%s\n' '#!/usr/bin/env bash' 'exit 0' > "$arm_host_bin/cmake"
chmod +x "$arm_host_repo/scripts/"*.sh "$arm_host_bin/"*
if arm_output=$(PSLOG_ALLOW_HOST_COMPILER=0 PATH="$arm_host_bin:$PATH" "$arm_host_repo/scripts/configure_cmake.sh" \
    --source . --build build/host --target host 2>&1); then
    printf 'configure_cmake accepted an unsupported native ARM host\n' >&2
    exit 1
elif ! printf '%s\n' "$arm_output" | grep -F 'PSLOG_ALLOW_HOST_COMPILER=1' >/dev/null; then
    printf 'configure_cmake did not explain the native ARM host override\n' >&2
    exit 1
fi
PSLOG_ALLOW_HOST_COMPILER=1 PATH="$arm_host_bin:$PATH" \
    "$arm_host_repo/scripts/configure_cmake.sh" --source . --build build/host --target host

printf 'configure_cmake stale-cache recovery tests passed.\n'
