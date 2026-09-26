#!/usr/bin/env sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "${script_dir}/../.." && pwd)
build_dir="${1:-${repo_root}/build/host}"
version="${2:-0.0.0}"
rock_tree="${3:-${repo_root}/build/luarocks}"
sdk_prefix="${4:-${repo_root}/build/lua-sdk}"
facade_root="${5:-${repo_root}}"
out_dir="${repo_root}/build/lua-interop"
test_bin="${out_dir}/pslog_lua_interop_tests"
installed_consumer_src="${out_dir}/installed_rock_consumer.c"
installed_consumer_bin="${out_dir}/installed_rock_consumer"
if [ "$(uname -s)" = Darwin ] && [ -n "${CC:-}" ]; then
    cc_bin="${CC}"
else
    cache_file="${build_dir}/CMakeCache.txt"
    if [ ! -f "${cache_file}" ]; then
        printf 'lua interop embedder test: missing configured compiler cache: %s\n' "${cache_file}" >&2
        exit 1
    fi
    cc_bin=$(sed -n 's/^CMAKE_C_COMPILER:[^=]*=//p' "${cache_file}" | tail -n 1)
fi
if [ -z "${cc_bin}" ] || [ ! -x "${cc_bin}" ]; then
    printf 'lua interop embedder test: configured C compiler is unavailable: %s\n' "${cc_bin:-<empty>}" >&2
    exit 1
fi
cflags="${CFLAGS:-}"
ldflags="${LDFLAGS:-}"
lua_stage_dir="${repo_root}/build/lua-host"
lua_include_dir="${lua_stage_dir}/include"
lua_lib_dir="${lua_stage_dir}/lib"
sdk_include_dir="${sdk_prefix}/include"
sdk_lib_dir="${sdk_prefix}/lib"
cache_file="${build_dir}/CMakeCache.txt"
sdk_shared_lib=
for candidate in "${sdk_lib_dir}/libpslog.so" "${sdk_lib_dir}/libpslog.dylib"; do
    if [ -e "${candidate}" ]; then
        sdk_shared_lib="${candidate}"
        break
    fi
done

mkdir -p "${out_dir}"

if [ ! -f "${lua_include_dir}/lua.h" ] || [ ! -f "${lua_lib_dir}/liblua.a" ]; then
    printf 'lua interop embedder test: missing staged Lua 5.5 C development files; run make lua-rock first\n' >&2
    exit 1
fi
if [ ! -f "${sdk_include_dir}/pslog.h" ] || [ -z "${sdk_shared_lib}" ]; then
    printf 'lua interop embedder test: missing installed local libpslog SDK; run make lua-rock first\n' >&2
    exit 1
fi
case "$(uname -s)" in
    Darwin)
        lua_dynamic_loader_libs=""
        private_rpath="-Wl,-rpath,"
        ;;
    *)
        lua_dynamic_loader_libs="-ldl"
        private_rpath="-Wl,--disable-new-dtags,-rpath,"
        ;;
esac
lua_cflags="-I${lua_include_dir}"
lua_libs="-L${lua_lib_dir} -llua -lm ${lua_dynamic_loader_libs}"
elf_linker_flags=
if [ "$(uname -s)" != Darwin ]; then
    elf_linker_flags=$(sed -n 's/^PSLOG_NONSHIPPED_ELF_LINKER_FLAGS:STRING=//p' "${cache_file}" | tail -n 1)
    host_compiler_override=$(sed -n 's/^PSLOG_ALLOW_HOST_COMPILER:[^=]*=//p' "${cache_file}" | tail -n 1)
    case "${host_compiler_override}" in 1|ON|TRUE|YES) host_compiler_override=ON ;; *) host_compiler_override=OFF ;; esac
    if [ -z "${elf_linker_flags}" ] && [ "${host_compiler_override}" != ON ]; then
        printf 'lua interop embedder test: missing Bootlin ELF runtime flags in %s\n' "${cache_file}" >&2
        exit 1
    fi
fi

"${cc_bin}" -std=c99 -Wall -Wextra -Werror ${cflags} \
    -D_POSIX_C_SOURCE=200809L \
    -DPSLOG_VERSION_STRING=\"${version}\" \
    -I"${sdk_include_dir}" \
    -I"${facade_root}/include" \
    ${lua_cflags} \
    "${repo_root}/tests/lua_interop_embedder_test.c" \
    "${facade_root}/lua/src/pslog_lua.c" \
    -L"${sdk_lib_dir}" -lpslog ${lua_libs} -pthread ${ldflags} ${elf_linker_flags} \
    "${private_rpath}${sdk_lib_dir}" \
    -o "${test_bin}"

"${test_bin}"

installed_header=$(find "${rock_tree}/share/lua" -name pslog_lua.h -type f | head -n 1)
if [ -z "${installed_header}" ]; then
    printf 'lua interop embedder test: installed rock is missing pslog_lua.h\n' >&2
    exit 1
fi
installed_header_dir=$(dirname "${installed_header}")
if [ "$#" -ge 5 ]; then
    installed_header_dir="${facade_root}/include"
fi
installed_core=$(find "${rock_tree}/lib/lua" -path '*/pslog/core.so' -type f | head -n 1)
if [ -z "${installed_core}" ]; then
    printf 'lua interop embedder test: installed rock is missing pslog/core.so\n' >&2
    exit 1
fi

installed_consumer_src="${repo_root}/tests/lua_interop_installed_consumer.c"

# Build the facade once for an embedding consumer of the installed static SDK.
"${cc_bin}" -std=c99 -Wall -Wextra -Werror ${cflags} \
    -D_POSIX_C_SOURCE=200809L -DPSLOG_VERSION_STRING=\"${version}\" \
    -I"${installed_header_dir}" -I"${sdk_include_dir}" ${lua_cflags} \
    -c "${facade_root}/lua/src/pslog_lua.c" -o "${out_dir}/facade.o"

cxx_bin="${CXX:-}"
if [ -z "${cxx_bin}" ]; then
    cxx_bin=$(sed -n 's/^CMAKE_CXX_COMPILER:[^=]*=//p' "${cache_file}" | tail -n 1)
fi
if [ -z "${cxx_bin}" ]; then
    case "$(uname -s):${host_compiler_override:-OFF}" in
        Darwin:*|*:ON) cxx_bin=$(command -v c++ 2>/dev/null || true) ;;
    esac
fi
if [ -z "${cxx_bin}" ] || [ ! -x "${cxx_bin}" ]; then
    printf 'lua interop embedder test: configured C++ compiler is unavailable\n' >&2
    exit 1
fi
for language in c89 c++; do
    compiler="${cc_bin}"
    # Lua's C89 numeric mode uses long; this native LP64 consumer checks
    # that it matches the staged runtime's 64-bit integer representation.
    language_flags="-std=c89 -Wpedantic -DLUA_USE_C89"
    if [ "${language}" = c++ ]; then
        compiler="${cxx_bin}"
        language_flags="-x c++ -std=c++11 -Wpedantic"
    fi
    "${compiler}" ${language_flags} -Wall -Wextra -Werror ${cflags} \
        -I"${installed_header_dir}" -I"${sdk_include_dir}" ${lua_cflags} \
        -c "${installed_consumer_src}" -o "${out_dir}/consumer.o"
    for linkage in shared static; do
        if [ "${linkage}" = shared ]; then
            facade="${installed_core}"
            core="${sdk_shared_lib}"
        else
            facade="${out_dir}/facade.o"
            core="${sdk_lib_dir}/libpslog.a"
        fi
        "${compiler}" "${out_dir}/consumer.o" "${facade}" "${core}" \
            ${lua_libs} -pthread ${ldflags} ${elf_linker_flags} \
            "${private_rpath}${sdk_lib_dir}" \
            "${private_rpath}$(dirname "${installed_core}")" \
            -o "${installed_consumer_bin}"
        "${installed_consumer_bin}"
    done
done
