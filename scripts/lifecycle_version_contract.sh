#!/usr/bin/env bash
# Read-only final-release identity check. Rehearsals use make prerelease.
set -euo pipefail

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
die() { printf 'lifecycle-version-contract: %s\n' "$*" >&2; exit 1; }
for required in git make cmake; do
  command -v "$required" >/dev/null 2>&1 || die "$required is required"
done

git_root=$(git -C "$repo_root" rev-parse --show-toplevel 2>/dev/null) || die 'an owning Git worktree is required'
git_root=$(CDPATH= cd -- "$git_root" && pwd -P)
[[ "$git_root" == "$repo_root" ]] || die 'source root must own its Git worktree'
branch=$(git -C "$repo_root" symbolic-ref --quiet --short HEAD) || die 'HEAD must be attached to the default branch'
default_branch=$(git -C "$repo_root" symbolic-ref --quiet --short refs/remotes/origin/HEAD 2>/dev/null || true)
default_branch=${default_branch#origin/}
if [[ -z "$default_branch" ]]; then
  for candidate in trunk main master; do
    if git -C "$repo_root" show-ref --verify --quiet "refs/heads/$candidate"; then
      default_branch=$candidate
      break
    fi
  done
fi
[[ -n "$default_branch" ]] || die 'cannot resolve the local default branch'
[[ "$branch" == "$default_branch" ]] || die "release must run on default branch $default_branch; HEAD is on $branch"
[[ -z "$(git -C "$repo_root" status --porcelain=v1 -uall)" ]] || die 'release requires a clean committed worktree'
[[ -z "${PSLOG_VERSION_OVERRIDE:-}" ]] || die 'release forbids a candidate version override'
case "${PSLOG_ALLOW_HOST_COMPILER:-}" in
  1|ON|on|TRUE|true|YES|yes) die 'release forbids the developer host compiler override' ;;
esac

exact_tags=()
while IFS= read -r tag; do
  [[ "$tag" =~ ^v[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?(\+[0-9A-Za-z.-]+)?$ ]] || continue
  if git -C "$repo_root" symbolic-ref --quiet "refs/tags/$tag" >/dev/null 2>&1 ||
     [[ "$(git -C "$repo_root" cat-file -t "refs/tags/$tag")" != commit ]]; then
    die "exact release tag $tag must be lightweight, not annotated or symbolic"
  fi
  exact_tags+=("$tag")
done < <(git -C "$repo_root" tag --points-at HEAD)
[[ ${#exact_tags[@]} -eq 1 ]] || die 'release requires exactly one lightweight vX.Y.Z tag on HEAD'
git -C "$repo_root" verify-commit HEAD >/dev/null 2>&1 || die 'HEAD must have a verified signature; run git verify-commit HEAD'

expected_version=${exact_tags[0]#v}
make_version=$(make -C "$repo_root" --no-print-directory print-release-version)
[[ "$make_version" == "$expected_version" ]] || die "Make resolved $make_version, expected $expected_version"
cmake_output=$(cmake -DPSLOG_VERSION_SOURCE_DIR="$repo_root" -P "$repo_root/cmake/pslog_version.cmake")
cmake_version=$(sed -n 's/^-- libpslog release version: //p' <<<"$cmake_output")
[[ "$cmake_version" == "$expected_version" ]] || die "CMake resolved $cmake_version, expected $expected_version"
printf 'lifecycle-version-contract: version %s verified on %s at %s\n' \
  "$expected_version" "$branch" "$(git -C "$repo_root" rev-parse HEAD)"
