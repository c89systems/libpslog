#!/usr/bin/env bash
set -euo pipefail
repo_root=${1:?repository root required}
scope=${2:-binary-source}
case "$scope" in binary-source|lua) ;; *) echo "unknown archive scope: $scope" >&2; exit 2 ;; esac
work=$(mktemp -d "$repo_root/build/archive-reproducibility-test.XXXXXXXX")
trap 'rm -rf -- "$work"' EXIT
trap 'exit 1' HUP INT TERM
fixture="$work/checkout"
mkdir -p "$fixture" "$work/no-hooks" "$work/payload"
# Small real source fixture; exercise the shipping entrypoints without building
# another compiler configuration. SDK/runtime correctness has its own tests.
while IFS= read -r entry; do
  [[ -z "$entry" || "$entry" == \#* ]] && continue
  mkdir -p "$fixture/$(dirname -- "$entry")"
  cp "$repo_root/$entry" "$fixture/$entry"
done < "$repo_root/lua/RELEASE_MANIFEST.in"
for entry in Makefile README.md lua/scripts/build_local_rock.sh lua/scripts/check_binding_boundary.sh; do
  cp "$repo_root/$entry" "$fixture/$entry"
done
mkdir -p "$fixture/scripts" "$fixture/build/lua-sdk/include"
printf '#!/bin/sh\nprintf "status=missing\\n"\n' > "$fixture/scripts/cpkt-toolchains.sh"
chmod +x "$fixture/scripts/cpkt-toolchains.sh"
printf '/build/\n/dist/\n' > "$fixture/.gitignore"
cat > "$fixture/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.21)
project(archive_fixture NONE)
install(FILES include/pslog.h build/lua-sdk/include/pslog_version.h DESTINATION include)
install(FILES ../payload/libpslog.so ../payload/libpslog.a DESTINATION lib)
install(FILES LICENSE README.md DESTINATION share/doc/libpslog)
CMAKE
git -C "$fixture" -c init.defaultBranch=trunk init --quiet
git -C "$fixture" config user.email lifecycle-test@example.invalid
git -C "$fixture" config user.name lifecycle-test
git -C "$fixture" config core.hooksPath "$work/no-hooks"
git -C "$fixture" config commit.gpgSign false
git -C "$fixture" add .
git -C "$fixture" commit --quiet -m 'test(package): create archive fixture'
printf '#define PSLOG_VERSION_STRING "0.0.0"\n' > "$fixture/build/lua-sdk/include/pslog_version.h"
touch "$fixture/build/lua-sdk/.installed.stamp"
printf 'fixture shared library\n' > "$work/payload/libpslog.so"
printf 'fixture static library\n' > "$work/payload/libpslog.a"
printf '#!/bin/sh\nexit 0\n' > "$work/no-strip"
chmod +x "$work/no-strip"
cmake -S "$fixture" -B "$fixture/build" > "$work/output" 2>&1 || { cat "$work/output" >&2; exit 1; }

package() (
  umask "$1"
  export TZ=$2
  rm -rf "$fixture/dist" "$fixture/build/package" "$fixture/build/lua-release-pack"
  if [[ "$scope" == binary-source ]]; then
    cmake -DPSLOG_ROOT="$fixture" -DPSLOG_BINARY_DIR="$fixture/build" \
    -DPSLOG_VERSION=0.0.0 -DPSLOG_TARGET_ID=x86_64-linux-gnu \
    -DPSLOG_TARGET_TOOL_DISCOVERY="$work/no-strip" \
    -DPSLOG_STATIC_LIB_NAME=libpslog.a -DPSLOG_SHARED_LIB_NAME=libpslog.so \
    -P "$repo_root/cmake/package_archive.cmake"
    cmake -DPSLOG_ROOT="$fixture" -DPSLOG_BINARY_DIR="$fixture/build" \
      -DPSLOG_VERSION=0.0.0 -P "$repo_root/cmake/package_source_archive.cmake"
  else
    make -C "$fixture" --no-print-directory \
      -o "$fixture/build/lua-sdk/.installed.stamp" release-lua-artifacts
  fi
)
package 022 UTC > "$work/output" 2>&1 || { cat "$work/output" >&2; exit 1; }
cp -R "$fixture/dist" "$work/first"
# Change input mtimes independently of the clock and use a restrictive umask.
# Intentional executable source modes remain part of the payload contract.
touch -t 200101010101 "$fixture/include/pslog.h" "$fixture/README.md" "$work/payload/"*
package 077 Pacific/Honolulu > "$work/output" 2>&1 || { cat "$work/output" >&2; exit 1; }
for first in "$work/first/"*; do
  name=${first##*/}
  cmp "$first" "$fixture/dist/$name" || {
    printf 'unchanged package inputs produced different bytes: %s\n' "$name" >&2; exit 1;
  }
done
mkdir -p "$work/extracted"
if [[ "$scope" == lua ]]; then
  source_archive="$fixture/dist/lua-pslog-0.0.0.tar.gz"
  source_name=lua-pslog-0.0.0
else
  source_archive="$fixture/dist/libpslog-0.0.0.tar.gz"
  source_name=libpslog-0.0.0
fi
tar -xzf "$source_archive" -C "$work/extracted"
[[ -x "$work/extracted/$source_name/lua/scripts/release_version.sh" ]]
[[ ! -x "$work/extracted/$source_name/include/pslog.h" ]]
for archive in "$fixture/dist/"*.tar.gz; do
  listing=$(tar --numeric-owner -tvzf "$archive")
  if grep -Evq '^.{10} 0/0 ' <<< "$listing"; then
    echo 'tarball retained workstation ownership' >&2; exit 1
  fi
done
printf '%s archives are byte-stable across mtimes, umask and timezone; source modes and normalized ownership verified\n' "$scope"
