#!/usr/bin/env bash
set -euo pipefail

repo_root=${1:?repo root is required}
scratch=$(mktemp -d "$repo_root/build/lua-interop-compiler-test.XXXXXX")
trap 'rm -rf "$scratch"' EXIT

# Exercise the entire runner with compiler stand-ins. Each linked consumer
# records execution, so success proves that both language/linkage loops finish.
mkdir -p "$scratch/lua/scripts" "$scratch/build/lua-host/include" \
    "$scratch/build/lua-host/lib" "$scratch/sdk/include" "$scratch/sdk/lib" \
    "$scratch/rocks/share/lua/5.5" "$scratch/rocks/lib/lua/5.5/pslog" \
    "$scratch/config" "$scratch/bin"
cp "$repo_root/lua/scripts/run_interop_embedder_test.sh" "$scratch/lua/scripts/runner.sh"
touch "$scratch/build/lua-host/include/lua.h" "$scratch/build/lua-host/lib/liblua.a" \
    "$scratch/sdk/include/pslog.h" "$scratch/sdk/lib/libpslog.so" \
    "$scratch/sdk/lib/libpslog.a" "$scratch/rocks/share/lua/5.5/pslog_lua.h" \
    "$scratch/rocks/lib/lua/5.5/pslog/core.so"

cat > "$scratch/bin/compiler" <<'COMPILER'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "${0##*/} $*" >> "$INTEROP_COMPILER_LOG"
while (($#)); do
    if [[ $1 == -o ]]; then
        output=$2
        break
    fi
    shift
done
cat > "${output:?missing compiler output}" <<'CONSUMER'
#!/usr/bin/env sh
printf 'ran\n' >> "$INTEROP_EXECUTION_LOG"
CONSUMER
chmod +x "$output"
COMPILER
chmod +x "$scratch/bin/compiler"
for compiler in cc c++ cached-cxx explicit-cxx; do
    cp "$scratch/bin/compiler" "$scratch/bin/$compiler"
done
cat > "$scratch/bin/uname" <<'UNAME'
#!/usr/bin/env sh
printf '%s\n' "$INTEROP_SYSTEM"
UNAME
chmod +x "$scratch/bin/uname"

run_case() {
    local name=$1 system=$2 override=$3 cached=$4 explicit=$5 expected=$6
    local log="$scratch/$name.compilers" executions="$scratch/$name.executions"
    : > "$log"
    : > "$executions"
    cat > "$scratch/config/CMakeCache.txt" <<CACHE
CMAKE_C_COMPILER:FILEPATH=$scratch/bin/cc
CMAKE_CXX_COMPILER:FILEPATH=$cached
PSLOG_ALLOW_HOST_COMPILER:BOOL=$override
PSLOG_NONSHIPPED_ELF_LINKER_FLAGS:STRING=-Wl,test-runtime
CACHE
    if env -u CC -u CFLAGS -u LDFLAGS CXX="$explicit" \
        PATH="$scratch/bin:$PATH" INTEROP_SYSTEM="$system" \
        INTEROP_COMPILER_LOG="$log" INTEROP_EXECUTION_LOG="$executions" \
        sh "$scratch/lua/scripts/runner.sh" "$scratch/config" 0.0.0 \
        "$scratch/rocks" "$scratch/sdk" > "$scratch/$name.output" 2>&1; then
        [[ -n "$expected" ]] || { printf '%s: unexpectedly succeeded\n' "$name" >&2; exit 1; }
        [[ $(grep -c '^ran$' "$executions") == 5 ]] || {
            printf '%s: not all consumers ran\n' "$name" >&2; exit 1;
        }
        [[ $(grep -c "^$expected " "$log") == 3 ]] || {
            printf '%s: wrong C++ compiler selected\n' "$name" >&2; exit 1;
        }
    else
        [[ -z "$expected" ]] || { cat "$scratch/$name.output" >&2; exit 1; }
        grep -Fq 'configured C++ compiler is unavailable' "$scratch/$name.output"
        if grep -Eq '^(c\+\+|cached-cxx|explicit-cxx) ' "$log"; then
            printf '%s: unexpectedly used a fallback compiler\n' "$name" >&2
            exit 1
        fi
    fi
}

run_case native-linux Linux ON '' '' 'c++'
run_case native-darwin Darwin OFF '' '' 'c++'
run_case cached-linux Linux OFF "$scratch/bin/cached-cxx" '' cached-cxx
run_case explicit-precedence Linux ON "$scratch/bin/cached-cxx" "$scratch/bin/explicit-cxx" explicit-cxx
run_case pinned-missing Linux OFF '' '' ''
run_case stale-cache Linux ON "$scratch/bin/nonexistent" '' ''
run_case invalid-explicit Linux ON "$scratch/bin/cached-cxx" "$scratch/bin/nonexistent" ''
for override in 1 TRUE YES; do
    run_case "native-linux-$override" Linux "$override" '' '' 'c++'
done
