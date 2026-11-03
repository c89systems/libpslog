#!/usr/bin/env bash
set -eu

is_semver_tag() {
  [[ "$1" =~ ^v[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?(\+[0-9A-Za-z.-]+)?$ ]]
}

if [ -e .git ]; then
  version_tags=()
  while IFS= read -r tag; do
    if ! is_semver_tag "$tag"; then
      continue
    fi
    tag_type=$(git cat-file -t "refs/tags/$tag" 2>/dev/null || true)
    if git symbolic-ref --quiet "refs/tags/$tag" >/dev/null 2>&1 || [ "$tag_type" != commit ]; then
      printf 'release version: exact tag %s must be lightweight\n' "$tag" >&2
      exit 1
    fi
    version_tags+=("$tag")
  done < <(git -C . tag --points-at HEAD)

  if [ "${#version_tags[@]}" -gt 1 ]; then
    printf 'release version: multiple exact lightweight tags point at HEAD: %s\n' "${version_tags[*]}" >&2
    exit 1
  fi
  if [ "${#version_tags[@]}" -eq 1 ]; then
    printf '%s\n' "${version_tags[0]#v}"
    exit 0
  fi
  printf '0.0.0\n'
  exit 0
fi

if [ -f VERSION ]; then
  version=$(sed -n '1p' VERSION)
  case "$version" in
    [0-9]*.[0-9]*.[0-9]*) printf '%s\n' "$version" ;;
    *) printf '0.0.0\n' ;;
  esac
else
  printf '0.0.0\n'
fi
