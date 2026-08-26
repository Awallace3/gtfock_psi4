#!/usr/bin/env bash
set -euo pipefail
set -x

: "${BUILD_PREFIX:?conda-build BUILD_PREFIX is required}"
: "${PREFIX:?conda-build PREFIX is required}"
: "${SRC_DIR:?conda-build SRC_DIR is required}"
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

# The verifications below must never be able to pass by inspecting nothing.
for required in \
    "$PREFIX/bin/pscf" \
    "$PREFIX/lib/libgtfock.so" \
    "$PREFIX/lib/libcint.so" \
    "$PREFIX/lib/cmake/GTFock/GTFockTargets.cmake"; do
    if [[ ! -e $required ]]; then
        echo "build_deps.sh did not install $required" >&2
        exit 1
    fi
done

# Simint is generated and linked into libcint as a static implementation
# detail. Its standalone archive and development metadata are not part of the
# GTFock consumer ABI and conda-forge discourages shipping bundled static libs.
# Decision CF-SIMINT-006 requires the removal to be verified rather than
# assumed, and Simint's own install location for its CMake package metadata is
# not part of this project's contract, so remove by search instead of by a
# hard-coded list.
simint_artifacts=()
while IFS= read -r -d '' path; do
    simint_artifacts+=("$path")
done < <(find "$PREFIX" -iname '*simint*' -print0)
if ((${#simint_artifacts[@]} == 0)); then
    echo "No installed Simint artifacts found: the build no longer installs" \
         "the generated Simint that CF-SIMINT-006 removes" >&2
    exit 1
fi
if [[ ! -f "$PREFIX/lib/libsimint.a" ]]; then
    echo "Generated Simint did not install lib/libsimint.a; refusing to" \
         "package an unverified Simint layout" >&2
    exit 1
fi
rm -rf -- "${simint_artifacts[@]}"
simint_survivors=$(find "$PREFIX" -iname '*simint*' -print)
if [[ -n $simint_survivors ]]; then
    echo "Simint artifacts survived removal:" >&2
    printf '%s\n' "$simint_survivors" >&2
    exit 1
fi
if grep -R -i -e simint "$PREFIX/lib/cmake/GTFock"; then
    echo "GTFock CMake metadata references the removed Simint package" >&2
    exit 1
fi

# Fail the build if any installed GTFock file leaks the source or build prefix.
# SRC_DIR and BUILD_PREFIX are authoritative only here: conda-build re-points
# them for the test phase, so the shipped tree must be proven clean now.
shopt -s nullglob
installed_artifacts=(
    "$PREFIX"/bin/pscf
    "$PREFIX"/lib/libgtfock.so*
    "$PREFIX"/lib/libcint.so*
    "$PREFIX"/lib/libGTMatrix.a
    "$PREFIX"/lib/cmake/GTFock
    "$PREFIX"/share/gtfock
)
shopt -u nullglob
if ((${#installed_artifacts[@]} == 0)); then
    echo "No installed GTFock artifacts to check for build-path leaks" >&2
    exit 1
fi
if grep -R -F -e "$SRC_DIR" -e "$BUILD_PREFIX" "${installed_artifacts[@]}"; then
    echo "An installed GTFock artifact contains a build-time path" >&2
    exit 1
fi
