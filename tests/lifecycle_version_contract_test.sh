#!/usr/bin/env bash
set -euo pipefail
repo_root=${1:?repository root required}
work=$(mktemp -d "$repo_root/build/lifecycle-version-contract-test.XXXXXXXX")
trap 'rm -rf -- "$work"' EXIT
trap 'exit 1' HUP INT TERM
fixture="$work/checkout with spaces"
mkdir -p "$fixture/scripts" "$fixture/lua/scripts" "$fixture/cmake" "$work/no-hooks"
cp "$repo_root/scripts/lifecycle_version_contract.sh" "$fixture/scripts/"
cp "$repo_root/lua/scripts/release_version.sh" "$fixture/lua/scripts/"
cp "$repo_root/cmake/pslog_version.cmake" "$fixture/cmake/"
cp "$repo_root/Makefile" "$fixture/"
cp "$repo_root/scripts/run_timed.sh" "$fixture/scripts/"
printf '#!/bin/sh\nprintf "status=missing\\n"\n' > "$fixture/scripts/cpkt-toolchains.sh"
chmod +x "$fixture/scripts/cpkt-toolchains.sh"
printf '/build/\n/dist/\n/.cache/\n' > "$fixture/.gitignore"
printf 'fixture\n' > "$fixture/README.md"

git -C "$fixture" -c init.defaultBranch=trunk init --quiet
git -C "$fixture" config user.email test@example.com
git -C "$fixture" config user.name 'libpslog lifecycle test'
git -C "$fixture" config core.hooksPath "$work/no-hooks"
git -C "$fixture" config commit.gpgSign false
git -C "$fixture" config tag.gpgSign false
git -C "$fixture" add .
git -C "$fixture" commit --quiet -m 'test(release): create version fixture'
git -C "$fixture" tag v99.99.99
git -C "$fixture" commit --quiet --allow-empty -m 'test(release): create unsigned candidate'
for directory in build dist .cache; do
  mkdir -p "$fixture/$directory"
  printf 'preserve\n' > "$fixture/$directory/sentinel"
done
printf 'existing timings\n' > "$fixture/build/release-timings.tsv"
refs() { git -C "$fixture" for-each-ref --format='%(refname) %(objectname) %(symref)'; }
assert_preserved() {
  [[ "$(refs)" == "$before_refs" ]] || { echo 'release preflight mutated refs' >&2; exit 1; }
  for directory in build dist .cache; do
    [[ "$(cat "$fixture/$directory/sentinel")" == preserve ]] || exit 1
  done
  [[ "$(cat "$fixture/build/release-timings.tsv")" == 'existing timings' ]] || {
    echo 'rejected release changed timing output' >&2; exit 1;
  }
}
expect_failure() {
  local reason=$1
  shift
  before_refs=$(refs)
  if "$@" > "$work/output" 2>&1; then
    printf 'release preflight accepted invalid state: %s\n' "$reason" >&2
    exit 1
  fi
  grep -Fq "$reason" "$work/output" || { cat "$work/output" >&2; exit 1; }
  assert_preserved
}
contract="$fixture/scripts/lifecycle_version_contract.sh"
expect_failure 'exactly one lightweight' "$contract"
git -C "$fixture" tag v1.2.3
expect_failure 'verified signature' "$contract"
expect_failure 'verified signature' make -C "$fixture" --no-print-directory release
git -C "$fixture" switch --quiet -c feature/probe
expect_failure 'default branch trunk' "$contract"
git -C "$fixture" switch --quiet trunk
git -C "$fixture" switch --quiet --detach
expect_failure 'attached' "$contract"
git -C "$fixture" switch --quiet trunk
printf 'dirty\n' >> "$fixture/README.md"
expect_failure 'clean committed worktree' "$contract"
git -C "$fixture" restore README.md
expect_failure 'candidate version override' env PSLOG_VERSION_OVERRIDE=9.9.9 "$contract"
expect_failure 'host compiler override' env PSLOG_ALLOW_HOST_COMPILER=1 "$contract"

# Real signature verification with a fixture-local key and trust file. No
# account keys, tags, config or workstation signing policy are changed.
ssh-keygen -q -t ed25519 -N '' -f "$work/signing-key"
printf 'test@example.com %s\n' "$(cat "$work/signing-key.pub")" > "$work/allowed-signers"
git -C "$fixture" config gpg.format ssh
git -C "$fixture" config user.signingKey "$work/signing-key"
git -C "$fixture" config gpg.ssh.allowedSignersFile "$work/allowed-signers"
git -C "$fixture" commit --quiet -S --allow-empty -m 'test(release): create signed candidate'
git -C "$fixture" tag -d v1.2.3 >/dev/null
git -C "$fixture" tag v1.2.3
before_refs=$(refs)
output=$("$contract")
[[ "$output" == *'version 1.2.3 verified on trunk'* ]] || { printf '%s\n' "$output" >&2; exit 1; }
assert_preserved

git -C "$fixture" tag v1.2.4
expect_failure 'exactly one lightweight' "$contract"
git -C "$fixture" tag -d v1.2.4 >/dev/null
git -C "$fixture" symbolic-ref refs/tags/v1.2.4 refs/tags/v1.2.3
expect_failure 'must be lightweight' "$contract"
expect_failure 'must be lightweight' bash -c 'cd "$1"; ./lua/scripts/release_version.sh' _ "$fixture"
expect_failure 'must be a lightweight' cmake -DPSLOG_VERSION_SOURCE_DIR="$fixture" -P "$fixture/cmake/pslog_version.cmake"
git -C "$fixture" symbolic-ref --delete refs/tags/v1.2.4
git -C "$fixture" tag -d v1.2.3 >/dev/null
git -C "$fixture" tag -a v1.2.3 -m 'annotated fixture'
expect_failure 'must be lightweight' "$contract"
printf 'read-only signed release preflight passed\n'
