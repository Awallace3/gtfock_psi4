#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=conda/recipe/grep-assert.sh
source "$ROOT/conda/recipe/grep-assert.sh"

sandbox=$(mktemp -d)
trap 'rm -rf "$sandbox"' EXIT
report="$sandbox/ldd.txt"

# ICU contains the letters "cuda" but is unrelated to NVIDIA CUDA.
printf '\tlibicudata.so.78 => /prefix/lib/libicudata.so.78\n' >"$report"
gtf_grep_absent "ICU misclassified as CUDA" \
    -E -i -e "$GTF_CUDA_LINK_PATTERN" -- "$report"

for library in libcudart.so.12 libcudnn.so.9 libcupti.so.12 libnppc.so.12 libnvrtc.so.12; do
    printf '\t%s => /prefix/lib/%s\n' "$library" "$library" >"$report"
    if gtf_grep_absent "CUDA library accepted" \
         -E -i -e "$GTF_CUDA_LINK_PATTERN" -- "$report" >/dev/null 2>&1; then
        echo "CUDA classifier accepted $library" >&2
        exit 1
    fi
done

# An unreadable/missing operand is an assertion error, not proof of absence.
if gtf_grep_absent "missing report accepted" \
     -E -i -e "$GTF_CUDA_LINK_PATTERN" -- "$sandbox/missing" >/dev/null 2>&1; then
    echo "absence helper accepted a missing report" >&2
    exit 1
fi

echo "Packaging guard regression passed"
