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

# Run the single numerical oracle against the installed example data, so the
# fresh-install gate cannot drift from tests/test_pscf_regression.py. The
# override is removed for execution: only the solve above needed it.
env -u CONDA_OVERRIDE_CUDA "$CONDA" run --prefix "$FRESH_PREFIX" \
    "$(dirname "$CONDA")/python" "$ROOT/tests/test_pscf_regression.py" \
    --mpiexec "$FRESH_PREFIX/bin/mpirun" \
    --mpiexec-preflag=--oversubscribe \
    --pscf "$FRESH_PREFIX/bin/pscf" \
    --data-dir "$FRESH_PREFIX/share/gtfock/examples" \
    --timeout 60
