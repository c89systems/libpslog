#!/usr/bin/env bash

set -eu

mode="all"
root_dir=""
repo_root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"

usage() {
    printf 'usage: %s [--dist-only] [--root DIR]\n' "$0" >&2
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --dist-only)
            mode="dist"
            shift
            ;;
        --root)
            if [ "$#" -lt 2 ]; then
                usage
                exit 1
            fi
            root_dir="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage
            exit 1
            ;;
    esac
done

if [ -z "$root_dir" ]; then
    root_dir="$repo_root"
else
    root_dir="$(CDPATH= cd -- "$root_dir" && pwd)"
fi

home_dir="${HOME:-}"
if [ "$root_dir" = "/" ] || { [ -n "$home_dir" ] && [ "$root_dir" = "$home_dir" ]; } || [ "$root_dir" = "$(dirname "$repo_root")" ]; then
    printf 'clean.sh: refusing unsafe cleanup root: %s\n' "$root_dir" >&2
    exit 1
fi
case "$root_dir" in
    "$repo_root"|"$repo_root"/build/*) ;;
    *)
        printf 'clean.sh: refusing cleanup root outside the repository generated-state root: %s\n' "$root_dir" >&2
        exit 1
        ;;
esac

remove_path() {
    target_path="$1"
    case "$target_path" in
        "$root_dir"/build|"$root_dir"/dist|"$root_dir"/.cache) ;;
        *)
            printf 'clean.sh: refusing unsafe generated-state deletion: %s\n' "$target_path" >&2
            exit 1
            ;;
    esac
    if [ -e "$target_path" ] || [ -L "$target_path" ]; then
        rm -rf -- "$target_path"
    fi
}

if [ "$mode" = "all" ]; then
    remove_path "$root_dir/build"
    remove_path "$root_dir/.cache"
fi

remove_path "$root_dir/dist"
