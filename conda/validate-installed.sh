#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
CONDA=${CONDA_EXE:-"$HOME/miniconda3/bin/conda"}
OUTPUT_DIR=${GTF_CONDA_OUTPUT_DIR:-"$ROOT/.conda-pkgs/output"}
FRESH_PREFIX=${GTF_CONDA_TEST_PREFIX:-"$ROOT/.conda-envs/package-test"}

rm -rf -- "$FRESH_PREFIX"
# Work around only OpenMPI's upstream virtual-package metadata while solving.
# No CUDA package is requested or permitted below.
CONDA_OVERRIDE_CUDA=12.0 "$CONDA" create --yes --prefix "$FRESH_PREFIX" \
    --override-channels --channel "file://$OUTPUT_DIR" --channel conda-forge \
    gtfock=0.1.0

list_json=$("$CONDA" list --prefix "$FRESH_PREFIX" --json)
"$(dirname "$CONDA")/python" -c '
import json, sys
names = {record["name"] for record in json.load(sys.stdin)}
forbidden = sorted(name for name in names if name == "cuda-version" or
                   name.startswith(("cuda-", "libcuda", "libcudart")))
if forbidden:
    raise SystemExit("CUDA packages entered CPU-only prefix: " + ", ".join(forbidden))
print(f"fresh prefix contains {len(names)} packages and no CUDA package")
' <<<"$list_json"

package_metadata=$(find "$FRESH_PREFIX/conda-meta" -maxdepth 1 -name 'gtfock-*.json' -print -quit)
test -n "$package_metadata"
if grep -i cuda "$package_metadata"; then
    echo "gtfock package metadata unexpectedly mentions CUDA" >&2
    exit 1
fi

for binary in "$FRESH_PREFIX/bin/pscf" \
              "$FRESH_PREFIX/lib/libgtfock.so" \
              "$FRESH_PREFIX/lib/libcint.so"; do
    links=$(ldd "$binary")
    printf '%s\n' "$links"
    ! grep -q "not found" <<<"$links"
    ! grep -i cuda <<<"$links"
done

output=$(env -u CONDA_OVERRIDE_CUDA "$CONDA" run --prefix "$FRESH_PREFIX" \
    env OMP_NUM_THREADS=1 MKL_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 \
    mpirun --oversubscribe -n 2 "$FRESH_PREFIX/bin/pscf" \
    "$FRESH_PREFIX/share/gtfock/examples/sto-3g.gbs" \
    "$FRESH_PREFIX/share/gtfock/examples/water.xyz" \
    2 1 1 2 15)
printf '%s\n' "$output"
grep -q "SAD guess unavailable; using core-Hamiltonian guess" <<<"$output"
energy=$(awk '/^[[:space:]]*energy[[:space:]]/ { value=$2 } END { print value }' <<<"$output")
test -n "$energy"
awk -v actual="$energy" -v reference=-74.9450213019 'BEGIN {
    error = actual - reference; if (error < 0) error = -error;
    printf("installed pscf energy=%0.12f error=%0.3e Eh\n", actual, error);
    exit(error > 1.0e-9);
}'
