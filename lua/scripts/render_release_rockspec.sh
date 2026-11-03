#!/usr/bin/env bash
set -eu

if [ "$#" -lt 2 ] || [ "$#" -gt 4 ]; then
  printf '%s\n' "usage: $0 VERSION OUTPUT_PATH [SOURCE_URL] [SOURCE_TAG]" >&2
  exit 1
fi

version="$1"
output_path="$2"
source_url="git+https://github.com/c89systems/libpslog.git"
source_tag=""
tag_line=""

case "$version" in
  ''|*[!0-9A-Za-z.+-]*)
    printf 'invalid version: %s\n' "$version" >&2
    exit 1
    ;;
esac

version_core="${version%%[-+]*}"
major="${version_core%%.*}"
rest="${version_core#*.}"
minor="${rest%%.*}"
patch="${rest#*.}"

if [ "$version_core" = "$rest" ] || [ "$rest" = "$patch" ]; then
  printf 'version must be semantic: %s\n' "$version" >&2
  exit 1
fi

if [ "$#" -ge 3 ]; then
  source_url="$3"
fi

if [ "$#" -ge 4 ]; then
  source_tag="$4"
elif [ "$version" != "0.0.0" ]; then
  source_tag="v$version"
fi

if [ -n "$source_tag" ]; then
  tag_line='  tag = "'"$source_tag"'",'
fi

sed \
  -e "s|@VERSION@|$version|g" \
  -e "s|@SOURCE_URL@|$source_url|g" \
  -e "s|@SOURCE_TAG_LINE@|$tag_line|g" \
  -e "s|@PSLOG_VERSION_MAJOR@|$major|g" \
  -e "s|@PSLOG_VERSION_MINOR@|$minor|g" \
  -e "s|@PSLOG_VERSION_PATCH@|$patch|g" \
  lua/lua-pslog.rockspec.in >"$output_path"
