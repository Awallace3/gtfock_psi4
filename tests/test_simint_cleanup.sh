#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=conda/recipe/simint-cleanup.sh
source "$ROOT/conda/recipe/simint-cleanup.sh"

sandbox=$(mktemp -d)
trap 'rm -rf "$sandbox"' EXIT
PREFIX="$sandbox/prefix"
GTF_BUILD_ROOT="$sandbox/build"
manifest="$GTF_BUILD_ROOT/simint/install_manifest.txt"
mkdir -p "$PREFIX/lib" "$PREFIX/include/simint" "$(dirname "$manifest")"
touch "$PREFIX/lib/libsimint.a" "$PREFIX/include/simint/cpp_restrict.hpp"

# Reproduce the CMake manifest shape that exposed the bug: a redundant path
# separator and no newline after the final installed header.
printf '%s\n%s' \
    "$PREFIX/lib/libsimint.a" \
    "$PREFIX/include//simint/cpp_restrict.hpp" >"$manifest"
gtf_cleanup_simint
[[ ! -e "$PREFIX/lib/libsimint.a" ]]
[[ ! -e "$PREFIX/include/simint" ]]

# An unrecorded Simint file must fail closed and must not be deleted.
rm -rf "$PREFIX" "$GTF_BUILD_ROOT"
mkdir -p "$PREFIX/lib" "$PREFIX/include/simint" "$(dirname "$manifest")"
touch "$PREFIX/lib/libsimint.a" "$PREFIX/include/simint/unrecorded.h"
printf '%s' "$PREFIX/lib/libsimint.a" >"$manifest"
if (gtf_cleanup_simint >/dev/null 2>&1); then
    echo "cleanup accepted an unrecorded Simint artifact" >&2
    exit 1
fi
[[ -e "$PREFIX/include/simint/unrecorded.h" ]]

echo "Simint cleanup regression passed"
