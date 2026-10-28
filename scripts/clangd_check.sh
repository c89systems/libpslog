#!/usr/bin/env bash
# Validate the host-editor configuration against the native Debug database only.
set -euo pipefail

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
database_dir="$repo_root/build/debug"
# CMake owns the language, includes and defines. Exercise normal .clangd
# discovery for a public consumer, a public header and an implementation.
source_file="$repo_root/examples/example.c"

command -v clangd >/dev/null 2>&1 || {
  printf 'clangd_check: host clangd is required; install it through the host package manager\n' >&2
  exit 1
}
[[ -f "$database_dir/compile_commands.json" ]] || {
  printf 'clangd_check: native debug compile database is missing: %s\n' "$database_dir/compile_commands.json" >&2
  exit 1
}
[[ -f "$source_file" ]] || {
  printf 'clangd_check: source is missing: %s\n' "$source_file" >&2
  exit 1
}
if ! grep -Fq "\"file\": \"$source_file\"" "$database_dir/compile_commands.json"; then
  printf 'clangd_check: native public-consumer command is missing from %s: %s\n' \
    "$database_dir/compile_commands.json" "$source_file" >&2
  exit 1
fi

cd "$repo_root"
log_dir="$database_dir/clangd-check"
mkdir -p "$log_dir"
for check_file in "$source_file" "$repo_root/include/pslog.h" "$repo_root/src/pslog_palette.c"; do
  output_file="$log_dir/$(basename -- "$check_file").log"
  # Check mode also tries refactorings. DefineOutline needs a live editor index
  # to find header implementations; macro expansion keeps this a parse check
  # without changing the compilation database, driver or project configuration.
  if ! clangd --check="$check_file" --tweaks=ExpandMacro >"$output_file" 2>&1; then
    cat "$output_file" >&2
    exit 1
  fi
  if ! grep -Fq "Loaded compilation database from $database_dir/compile_commands.json" "$output_file"; then
    printf 'clangd_check: .clangd did not discover %s for %s; see %s\n' \
      "$database_dir/compile_commands.json" "$check_file" "$output_file" >&2
    exit 1
  fi
done
