#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
# shellcheck source=conda/recipe/grep-assert.sh
source "$ROOT/conda/recipe/grep-assert.sh"
CONDA=${CONDA_EXE:-"$HOME/miniconda3/bin/conda"}
OUTPUT_DIR=${GTF_CONDA_OUTPUT_DIR:-"$ROOT/.conda-pkgs/output"}
FRESH_PREFIX=${GTF_CONDA_TEST_PREFIX:-"$ROOT/.conda-envs/package-test"}

mapfile -d '' -t package_artifacts < <(
    find "$OUTPUT_DIR/linux-64" -maxdepth 1 -type f \
        \( -name 'gtfock-*.conda' -o -name 'gtfock-*.tar.bz2' \) -print0
)
if ((${#package_artifacts[@]} != 1)); then
    echo "Expected exactly one local GTFock artifact, found ${#package_artifacts[@]}." >&2
    exit 2
fi
package_artifact=${package_artifacts[0]}
package_filename=${package_artifact##*/}
case $package_filename in
    *.conda) package_record=${package_filename%.conda} ;;
    *.tar.bz2) package_record=${package_filename%.tar.bz2} ;;
    *) echo "Unsupported package artifact: $package_artifact" >&2; exit 2 ;;
esac
package_metadata="$FRESH_PREFIX/conda-meta/$package_record.json"

rm -rf -- "$FRESH_PREFIX"
# Work around only OpenMPI's upstream virtual-package metadata while solving.
# No CUDA package is requested or permitted below.
CONDA_OVERRIDE_CUDA=12.0 "$CONDA" create --yes --prefix "$FRESH_PREFIX" \
    --override-channels --channel conda-forge \
    "$package_artifact"

# CONDA_EXE is commonly <base>/condabin/conda, which has no sibling python, so
# resolve the interpreter through the conda base prefix instead.
CONDA_BASE=$("$CONDA" info --base)
PYTHON="$CONDA_BASE/bin/python"
if [[ ! -x $PYTHON ]]; then
    echo "No python interpreter at $PYTHON (conda base $CONDA_BASE)." >&2
    exit 2
fi

list_json=$("$CONDA" list --prefix "$FRESH_PREFIX" --json)
"$PYTHON" -c '
import json, sys
names = {record["name"] for record in json.load(sys.stdin)}
# Any package carrying a CUDA runtime, stub, or math library disproves the
# CPU-only claim, so match the whole namespace rather than an ad hoc list:
# "cuda"/"nvidia" anywhere in the name, plus the CUDA component libraries
# whose names contain neither (optionally "lib"-prefixed and "-dev"-suffixed).
CUDA_COMPONENTS = ("cudnn", "cutensor", "cupti", "nccl", "npp", "nvjitlink",
                   "nvrtc", "nvtx", "nvcomp", "cublas", "cufft", "cufile",
                   "curand", "cusolver", "cusparse")
def is_cuda(name):
    if "cuda" in name or "nvidia" in name:
        return True
    stem = name[3:] if name.startswith("lib") else name
    return any(stem == component or stem.startswith(component + "-")
               for component in CUDA_COMPONENTS)
forbidden = sorted(name for name in names if is_cuda(name))
if forbidden:
    raise SystemExit("CUDA packages entered CPU-only prefix: " + ", ".join(forbidden))
print(f"fresh prefix contains {len(names)} packages and no CUDA package")
' <<<"$list_json"

if [[ ! -f $package_metadata ]]; then
    echo "Exact local artifact was not installed: $package_record" >&2
    exit 1
fi
gtf_grep_absent "gtfock package metadata unexpectedly mentions CUDA" \
    -i -e cuda -- "$package_metadata"

# Match CUDA-family shared-library basenames rather than the substring "cuda",
# which also occurs inside unrelated ICU's libicudata.so.
for binary in "$FRESH_PREFIX/bin/pscf" \
              "$FRESH_PREFIX/lib/libgtfock.so" \
              "$FRESH_PREFIX/lib/libcint.so"; do
    links=$(ldd "$binary")
    printf '%s\n' "$links"
    gtf_grep_absent "$binary has unresolved libraries" \
        -F -e "not found" <<<"$links"
    gtf_grep_absent "$binary has CUDA dynamic linkage" \
        -E -i -e "$GTF_CUDA_LINK_PATTERN" <<<"$links"
done

# Run the single numerical oracle against the installed example data, so the
# fresh-install gate cannot drift from tests/test_pscf_regression.py. The
# override is removed for execution: only the solve above needed it.
env -u CONDA_OVERRIDE_CUDA "$CONDA" run --prefix "$FRESH_PREFIX" \
    "$PYTHON" "$ROOT/tests/test_pscf_regression.py" \
    --mpiexec "$FRESH_PREFIX/bin/mpirun" \
    --mpiexec-preflag=--oversubscribe \
    --pscf "$FRESH_PREFIX/bin/pscf" \
    --data-dir "$FRESH_PREFIX/share/gtfock/examples" \
    --timeout 60
