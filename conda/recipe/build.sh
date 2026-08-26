#!/usr/bin/env bash
set -euo pipefail
set -x

: "${BUILD_PREFIX:?conda-build BUILD_PREFIX is required}"
: "${PREFIX:?conda-build PREFIX is required}"
: "${CPU_COUNT:=2}"

export CC="$(command -v icx)"
export CXX="$(command -v icpx)"
export FC="$(command -v x86_64-conda-linux-gnu-gfortran)"
export OMPI_CC="$CC"
export OMPI_CXX="$CXX"
export OMPI_FC="$FC"

# Prove that OpenMPI wrappers dispatch to the validated compilers rather than
# silently falling back to the GCC compiler used to build OpenMPI itself.
mpicc --showme:command | tee /tmp/gtfock-mpicc-command.txt
grep -F "$CC" /tmp/gtfock-mpicc-command.txt
mpicxx --showme:command | grep -F "$CXX"
mpifort --showme:command | grep -F "$FC"
"$CC" --version | head -n 1
mpirun --version | head -n 1

export CMAKE_BUILD_PARALLEL_LEVEL="$CPU_COUNT"
export GTF_BUILD_ROOT="$SRC_DIR/_conda-build"
export GTF_INSTALL_PREFIX="$PREFIX"
export GTF_PRESERVE_INSTALL_PREFIX=1
export GTF_PINNED_SOURCE_ARCHIVES=1
export SIMINT_VECTOR=avx2

"$SRC_DIR/build_deps.sh" --clean

# Simint is generated and linked into libcint as a static implementation
# detail. Its standalone archive and development metadata are not part of the
# GTFock consumer ABI and conda-forge discourages shipping bundled static libs.
rm -f "$PREFIX/lib/libsimint.a"
rm -rf "$PREFIX/include/simint" "$PREFIX/share/cmake/simint"

# Fail the build if installed metadata leaks the source/build prefixes.
if grep -R -F -e "$SRC_DIR" -e "$BUILD_PREFIX" \
    "$PREFIX/lib/cmake/GTFock"; then
    echo "GTFock CMake metadata contains a build-time path" >&2
    exit 1
fi
