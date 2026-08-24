#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
BUILD_ROOT=${GTF_BUILD_ROOT:-"$ROOT/_build"}
INSTALL_PREFIX=${GTF_INSTALL_PREFIX:-"$ROOT/_install"}
SIMINT_VECTOR=${SIMINT_VECTOR:-avx2}
JOBS=${CMAKE_BUILD_PARALLEL_LEVEL:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}
RUN_TESTS=ON
CLEAN=OFF

usage() {
    cat <<'EOF'
Usage: ./build_deps.sh [--clean] [--no-tests]

Builds generated Simint and the complete native GTFock stack into _install.
The active conda environment must be the gtf2 environment from env.yml.
SIMINT_VECTOR defaults to avx2; set it explicitly only for a supported CPU.
EOF
}

while (($#)); do
    case "$1" in
        --clean) CLEAN=ON ;;
        --no-tests) RUN_TESTS=OFF ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

if [[ -z ${CONDA_PREFIX:-} ]]; then
    echo "Activate the gtf2 conda environment before building." >&2
    exit 2
fi

for tool in cmake ninja python git icx icpx mpicc mpirun; do
    path=$(command -v "$tool" || true)
    if [[ -z $path || $path != "$CONDA_PREFIX"/* ]]; then
        echo "$tool must come from the active conda environment (found: ${path:-missing})." >&2
        exit 2
    fi
done

CC=$(command -v icx)
CXX=$(command -v icpx)
FC=${FC:-"$CONDA_PREFIX/bin/x86_64-conda-linux-gnu-gfortran"}
if [[ ! -x $FC || $FC != "$CONDA_PREFIX"/* ]]; then
    echo "Fortran compiler must come from the active conda environment: $FC" >&2
    exit 2
fi

if git -C "$ROOT" submodule status | grep -q '^-'; then
    echo "Initialize pinned sources first: git submodule update --init --recursive" >&2
    exit 2
fi

if [[ $CLEAN == ON ]]; then
    rm -rf -- "$BUILD_ROOT" "$INSTALL_PREFIX"
fi
mkdir -p -- "$BUILD_ROOT" "$INSTALL_PREFIX"

configure_common=(
    -G Ninja
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_C_COMPILER="$CC"
    -DCMAKE_CXX_COMPILER="$CXX"
)

echo "==> Building Simint generator with $($CXX --version | head -n1)"
cmake -S "$ROOT/simint-generator" -B "$BUILD_ROOT/simint-generator" \
    "${configure_common[@]}"
cmake --build "$BUILD_ROOT/simint-generator" --parallel "$JOBS"

SIMINT_SOURCE="$BUILD_ROOT/simint-src"
rm -rf -- "$SIMINT_SOURCE"
(
    cd "$BUILD_ROOT"
    python "$ROOT/simint-generator/create.py" \
        -g "$BUILD_ROOT/simint-generator/generator/ostei" \
        -l 5 -p 4 -d 0 -ve 4 -vg 5 -he 4 -hg 5 \
        "$SIMINT_SOURCE"
)

echo "==> Building generated Simint ($SIMINT_VECTOR)"
cmake -S "$SIMINT_SOURCE" -B "$BUILD_ROOT/simint" \
    "${configure_common[@]}" \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
    -DSIMINT_VECTOR="$SIMINT_VECTOR" \
    -DENABLE_TESTS="$RUN_TESTS"
cmake --build "$BUILD_ROOT/simint" --parallel "$JOBS"
if [[ $RUN_TESTS == ON ]]; then
    ctest --test-dir "$BUILD_ROOT/simint" --output-on-failure
fi
cmake --install "$BUILD_ROOT/simint"

# Keep submodules immutable: apply reviewed modern-compiler/numerical fixes to a
# disposable copy. The patch is part of this superproject and its source commit
# is pinned by the GTFock gitlink.
GTF_SOURCE="$BUILD_ROOT/gtfock-src"
rm -rf -- "$GTF_SOURCE"
mkdir -p -- "$GTF_SOURCE"
cp -a "$ROOT/GTFock/." "$GTF_SOURCE/"
rm -f -- "$GTF_SOURCE/.git"
(
    cd "$GTF_SOURCE"
    git apply "$ROOT/patches/gtfock-modern.patch"
)

echo "==> Configuring GTFock with icx/icpx and conda OpenMPI"
cmake -S "$ROOT" -B "$BUILD_ROOT/gtfock" \
    "${configure_common[@]}" \
    -DCMAKE_Fortran_COMPILER="$FC" \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
    -DCMAKE_PREFIX_PATH="$INSTALL_PREFIX;$CONDA_PREFIX" \
    -DGTF_GTFock_SOURCE_DIR="$GTF_SOURCE" \
    -DBUILD_TESTING="$RUN_TESTS"
cmake --build "$BUILD_ROOT/gtfock" --parallel "$JOBS"
if [[ $RUN_TESTS == ON ]]; then
    ctest --test-dir "$BUILD_ROOT/gtfock" --output-on-failure
fi
cmake --install "$BUILD_ROOT/gtfock"

cat <<EOF
==> Build complete
    C compiler:   $CC
    C++ compiler: $CXX
    MPI:          $(command -v mpirun)
    Prefix:       $INSTALL_PREFIX
EOF
