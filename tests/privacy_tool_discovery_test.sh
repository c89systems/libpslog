#!/usr/bin/env bash
set -euo pipefail
repo_root=${1:?repository root required}
work=$(mktemp -d "$repo_root/build/privacy-tool-discovery-test.XXXXXXXX")
trap 'rm -rf -- "$work"' EXIT
trap 'exit 1' HUP INT TERM
fixture="$work/checkout with spaces"
mkdir -p "$fixture/scripts" "$fixture/lua/scripts" "$fixture/dist" "$work/linux" "$work/darwin" "$work/bin"
cp "$repo_root/scripts/verify_release_privacy.sh" "$fixture/scripts/"
cat > "$fixture/scripts/discover_target_tools.sh" <<'SH'
#!/usr/bin/env bash
printf 'READELF=%q\nOTOOL=%q\n' /selected/readelf /selected/otool
case "$*" in
  *"$FAIL_TARGET"*) echo 'fixture discovery failed' >&2; exit 29 ;;
esac
SH
cat > "$fixture/scripts/sha256_files.sh" <<'SH'
#!/bin/sh
touch "$PRODUCER_MARKER"
SH
cat > "$fixture/lua/scripts/release_version.sh" <<'SH'
#!/bin/sh
printf '1.2.3\n'
SH
cat > "$work/bin/cmake" <<'SH'
#!/bin/sh
printf '%s\n' "$@" > "$CMAKE_ARGUMENTS"
SH
chmod +x "$fixture/scripts/"*.sh "$fixture/lua/scripts/"*.sh "$work/bin/cmake"
touch "$fixture/dist/libpslog-1.2.3-CHECKSUMS"
export PATH="$work/bin:$PATH" PRODUCER_MARKER="$work/checksums-ran" CMAKE_ARGUMENTS="$work/cmake-arguments"
for target in x86_64-linux-gnu arm64-apple-darwin; do
  export FAIL_TARGET=$target
  if "$fixture/scripts/verify_release_privacy.sh" --build-dir "$work/linux" \
      --target-id x86_64-linux-gnu --darwin-build-dir "$work/darwin" > "$work/output" 2>&1; then
    echo "privacy gate concealed failed $target tool discovery" >&2; exit 1
  fi
  grep -Fq 'fixture discovery failed' "$work/output"
  [[ ! -e "$PRODUCER_MARKER" && ! -e "$CMAKE_ARGUMENTS" ]] || {
    echo 'privacy gate continued after failed tool discovery' >&2; exit 1;
  }
done
export FAIL_TARGET=none
"$fixture/scripts/verify_release_privacy.sh" --build-dir "$work/linux" \
  --target-id x86_64-linux-gnu --darwin-build-dir "$work/darwin"
[[ -e "$PRODUCER_MARKER" ]]
grep -Fxq -- '-DPSLOG_READELF=/selected/readelf' "$CMAKE_ARGUMENTS"
grep -Fxq -- '-DPSLOG_OTOOL=/selected/otool' "$CMAKE_ARGUMENTS"
printf 'privacy tool discovery failures stop verification\n'
