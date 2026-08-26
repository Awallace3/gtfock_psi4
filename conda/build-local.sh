#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
CONDA=${CONDA_EXE:-"$HOME/miniconda3/bin/conda"}
TOOLS_PREFIX=${GTF_CONDA_TOOLS_PREFIX:-"$ROOT/.conda-envs/package-tools"}
SOURCE_SNAPSHOT="$ROOT/.conda-source"
OUTPUT_DIR="$ROOT/.conda-pkgs/output"
PKG_CACHE="$ROOT/.conda-pkgs/cache"

if [[ ! -x $CONDA ]]; then
    echo "Set CONDA_EXE to a conda executable." >&2
    exit 2
fi

# Create a clean source snapshot without build products or tool environments.
# Root files come from the current working tree so the recipe can be validated
# before publication; submodule content comes from each immutable gitlink.
rm -rf -- "$SOURCE_SNAPSHOT"
mkdir -p -- "$SOURCE_SNAPSHOT"
while IFS= read -r -d '' path; do
    if [[ -f "$ROOT/$path" || -L "$ROOT/$path" ]]; then
        mkdir -p -- "$SOURCE_SNAPSHOT/$(dirname -- "$path")"
        cp -a -- "$ROOT/$path" "$SOURCE_SNAPSHOT/$path"
    fi
done < <(git -C "$ROOT" ls-files --cached --others --exclude-standard -z)

while read -r _commit path _rest; do
    mkdir -p -- "$SOURCE_SNAPSHOT/$path"
    git -C "$ROOT/$path" archive HEAD | tar -x -C "$SOURCE_SNAPSHOT/$path"
done < <(git -C "$ROOT" submodule status --recursive | sed 's/^[ +-U]//')

mkdir -p -- "$PKG_CACHE" "$OUTPUT_DIR"
rm -f -- "$OUTPUT_DIR/linux-64"/gtfock-*.conda \
          "$OUTPUT_DIR/linux-64"/gtfock-*.tar.bz2
export CONDA_PKGS_DIRS="$PKG_CACHE"
if [[ ! -x "$TOOLS_PREFIX/bin/conda-build" ]]; then
    "$CONDA" create --yes --prefix "$TOOLS_PREFIX" \
        --override-channels --channel conda-forge \
        "conda-build>=25" conda-index
fi

export GTF_CONDA_SOURCE_PATH="$SOURCE_SNAPSHOT"
# The pinned OpenMPI package has a __cuda>=12 solver constraint despite having
# no CUDA dependency or linkage. Override only virtual-package detection so a
# CPU host with an older/no NVIDIA driver can solve the audited build.
export CONDA_OVERRIDE_CUDA=12.0
"$CONDA" run --prefix "$TOOLS_PREFIX" conda build \
    "$ROOT/conda/recipe" \
    --override-channels --channel conda-forge \
    --output-folder "$OUTPUT_DIR" \
    --no-anaconda-upload

"$CONDA" run --prefix "$TOOLS_PREFIX" python -m conda_index "$OUTPUT_DIR"
find "$OUTPUT_DIR/linux-64" -maxdepth 1 -type f \
    \( -name 'gtfock-*.conda' -o -name 'gtfock-*.tar.bz2' \) -print
"$ROOT/conda/validate-installed.sh"
