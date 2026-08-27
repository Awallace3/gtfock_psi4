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

# Every packaged submodule tree must be exactly the pinned gitlink, because
# conda/recipe/source-provenance.yaml records those commits as the artifact's
# component versions. Snapshot the gitlink commit itself (not the submodule's
# checked-out HEAD), and refuse to build when a checkout has drifted away from
# its pin or when a pin no longer matches the recorded provenance.
PROVENANCE="$ROOT/conda/recipe/source-provenance.yaml"
declare -A PINNED_COMPONENTS=()
while read -r component commit; do
    PINNED_COMPONENTS["$component"]=$commit
done < <(awk '
    /^component_versions:/ { in_block = 1; next }
    in_block && /^[^[:space:]#]/ { in_block = 0 }
    in_block && $1 ~ /:$/ { key = $1; sub(/:$/, "", key); print key, $2 }
' "$PROVENANCE")
unset 'PINNED_COMPONENTS[superproject]'

declare -A SNAPSHOT_COMPONENTS=()

snapshot_submodules() {
    local repo=$1 parent=$2
    local metadata path relative pinned checked_out recorded
    while IFS=$'\t' read -r metadata path; do
        [[ $metadata == 160000\ * ]] || continue
        pinned=$(cut -d' ' -f2 <<<"$metadata")
        relative=${parent:+$parent/}$path
        if ! git -C "$repo/$path" rev-parse --git-dir >/dev/null 2>&1; then
            echo "Submodule $relative is not initialized:" \
                 "run 'git submodule update --init --recursive'." >&2
            exit 2
        fi
        checked_out=$(git -C "$repo/$path" rev-parse HEAD)
        if [[ $checked_out != "$pinned" ]]; then
            echo "Submodule $relative is checked out at $checked_out but the" \
                 "superproject pins $pinned; run 'git submodule update" \
                 "--init --recursive' before packaging." >&2
            exit 2
        fi
        recorded=${PINNED_COMPONENTS[$path]:-}
        if [[ -n $recorded && $recorded != "$pinned" ]]; then
            echo "Submodule $relative is pinned at $pinned but" \
                 "source-provenance.yaml records $recorded." >&2
            exit 2
        fi
        SNAPSHOT_COMPONENTS["$path"]=$pinned
        mkdir -p -- "$SOURCE_SNAPSHOT/$relative"
        git -C "$repo/$path" archive "$pinned" \
            | tar -x -C "$SOURCE_SNAPSHOT/$relative"
        snapshot_submodules "$repo/$path" "$relative"
    done < <(git -C "$repo" ls-files --stage)
}

snapshot_submodules "$ROOT" ""

for component in "${!PINNED_COMPONENTS[@]}"; do
    if [[ -z ${SNAPSHOT_COMPONENTS[$component]:-} ]]; then
        echo "source-provenance.yaml records component $component, but no" \
             "such pinned submodule was snapshotted." >&2
        exit 2
    fi
done
for component in "${!SNAPSHOT_COMPONENTS[@]}"; do
    if [[ -z ${PINNED_COMPONENTS[$component]:-} ]]; then
        echo "Pinned submodule $component was snapshotted into the artifact," \
             "but source-provenance.yaml records no component version for" \
             "it; record its pinned commit before packaging." >&2
        exit 2
    fi
done

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
