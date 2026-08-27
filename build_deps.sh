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

# conda-build splits compilers into BUILD_PREFIX and link dependencies into
# PREFIX. Honor those only when conda-build is actually driving this run: a
# stray PREFIX export in a developer shell must neither abort the build nor
# widen the prefixes that tools and libraries are allowed to come from.
if [[ -n ${CONDA_BUILD:-}${CONDA_BUILD_STATE:-} \
      && -n ${PREFIX:-} && -n ${BUILD_PREFIX:-} ]]; then
    HOST_PREFIX_CANDIDATES=("$PREFIX")
    TOOL_PREFIX_CANDIDATES=("$BUILD_PREFIX")
else
    HOST_PREFIX_CANDIDATES=("${CONDA_PREFIX:-}")
    TOOL_PREFIX_CANDIDATES=("${CONDA_PREFIX:-}")
fi

resolve_prefixes() {
    local prefix
    for prefix in "$@"; do
        [[ -n $prefix && -d $prefix ]] || continue
        (cd -- "$prefix" && pwd -P)
    done
}

HOST_PREFIXES=()
TOOL_PREFIXES=()
while IFS= read -r prefix; do
    HOST_PREFIXES+=("$prefix")
done < <(resolve_prefixes "${HOST_PREFIX_CANDIDATES[@]}")
while IFS= read -r prefix; do
    TOOL_PREFIXES+=("$prefix")
done < <(resolve_prefixes "${TOOL_PREFIX_CANDIDATES[@]}")

CONDA_ROOTS=("${HOST_PREFIXES[@]}" "${TOOL_PREFIXES[@]}")
if ((${#HOST_PREFIXES[@]} == 0)); then
    echo "Activate the supported conda environment or run through conda-build." >&2
    exit 2
fi

path_is_in_conda() {
    local path=$1 prefix
    for prefix in "${CONDA_ROOTS[@]}"; do
        [[ $path == "$prefix"/* ]] && return 0
    done
    return 1
}

for tool in cmake ninja python git icx icpx mpicc mpirun; do
    path=$(command -v "$tool" || true)
    if [[ -z $path ]] || ! path_is_in_conda "$(readlink -f "$path")"; then
        echo "$tool must come from a supported conda prefix (found: ${path:-missing})." >&2
        exit 2
    fi
done

CC=$(readlink -f "$(command -v icx)")
CXX=$(readlink -f "$(command -v icpx)")
if [[ -z ${FC:-} ]]; then
    for prefix in "${TOOL_PREFIXES[@]}"; do
        candidate="$prefix/bin/x86_64-conda-linux-gnu-gfortran"
        if [[ -x $candidate ]]; then
            FC=$candidate
            break
        fi
    done
fi
FC=${FC:-}
if [[ ! -x $FC ]] || ! path_is_in_conda "$(readlink -f "$FC")"; then
    echo "GNU Fortran must come from a supported conda prefix: ${FC:-missing}" >&2
    exit 2
fi

# Only $ROOT's own repository can report on the pinned sources being compiled.
# --is-inside-work-tree is true for any enclosing repository as well, so a
# conda-build $SRC_DIR snapshot that happens to sit inside an unrelated
# checkout would be judged by that checkout's submodules instead of the
# pinned archives it actually contains.
gtf_repo_root=$(git -C "$ROOT" rev-parse --show-toplevel 2>/dev/null || true)
if [[ -n $gtf_repo_root ]] &&
   [[ $(cd -- "$gtf_repo_root" && pwd -P) == "$ROOT" ]]; then
    if git -C "$ROOT" submodule status | grep -q '^-'; then
        echo "Initialize pinned sources first: git submodule update --init --recursive" >&2
        exit 2
    fi
elif [[ ${GTF_PINNED_SOURCE_ARCHIVES:-0} != 1 ]]; then
    echo "Source archives require GTF_PINNED_SOURCE_ARCHIVES=1 from the pinned recipe." >&2
    exit 2
fi

if [[ $CLEAN == ON ]]; then
    rm -rf -- "$BUILD_ROOT"
    if [[ ${GTF_PRESERVE_INSTALL_PREFIX:-0} != 1 ]]; then
        rm -rf -- "$INSTALL_PREFIX"
    fi
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
# Only the host prefix may supply numerical libraries and MPI; BUILD_PREFIX
# stays off CMAKE_PREFIX_PATH so a compiler-only prefix cannot satisfy them.
PREFIX_PATHS=("$INSTALL_PREFIX" "${HOST_PREFIXES[@]}")
CMAKE_PREFIXES=$(IFS=';'; echo "${PREFIX_PATHS[*]}")

cmake -S "$ROOT" -B "$BUILD_ROOT/gtfock" \
    "${configure_common[@]}" \
    -DCMAKE_Fortran_COMPILER="$FC" \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
    -DCMAKE_PREFIX_PATH="$CMAKE_PREFIXES" \
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
