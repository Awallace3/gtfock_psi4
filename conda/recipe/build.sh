#!/usr/bin/env bash
set -euo pipefail
set -x

: "${BUILD_PREFIX:?conda-build BUILD_PREFIX is required}"
: "${PREFIX:?conda-build PREFIX is required}"
: "${SRC_DIR:?conda-build SRC_DIR is required}"
: "${CPU_COUNT:=2}"

CC=$(command -v icx || true)
CXX=$(command -v icpx || true)
FC=$(command -v x86_64-conda-linux-gnu-gfortran || true)
# export CC=$(...) reports export's status, not the substitution's, so an
# unresolved compiler would leave the variable empty and every grep pattern
# below would match unconditionally.
for _tool in CC CXX FC; do
    if [[ -z ${!_tool} ]]; then
        echo "$_tool did not resolve to a validated compiler on PATH, so the" \
             "OpenMPI wrapper dispatch proof cannot be made" >&2
        exit 1
    fi
done
export CC CXX FC

# libcint diagnostics embed __FILE__ in both C and Fortran objects. Preserve
# useful, stable source locations without leaking conda-build's temporary work
# directory into the installed shared library.
source_map_flags="-ffile-prefix-map=$SRC_DIR=/usr/src/gtfock -fdebug-prefix-map=$SRC_DIR=/usr/src/gtfock"
export CFLAGS="${CFLAGS:+$CFLAGS }$source_map_flags"
export CXXFLAGS="${CXXFLAGS:+$CXXFLAGS }$source_map_flags"
# CMake's Ninja Fortran scanner preprocesses absolute source operands into
# intermediate files. Suppress their #line markers so gfortran cannot retain
# the original temporary filename after applying the prefix maps.
export FFLAGS="${FFLAGS:+$FFLAGS }$source_map_flags -P"

export OMPI_CC="$CC"
export OMPI_CXX="$CXX"
export OMPI_FC="$FC"

# Prove that OpenMPI wrappers dispatch to the validated compilers rather than
# silently falling back to the GCC compiler used to build OpenMPI itself.
mpicc --showme:command | grep -F "$CC"
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

# The verifications below must never be able to pass by inspecting nothing, so
# every path they inspect is required to exist first.
for required in \
    "$PREFIX/bin/pscf" \
    "$PREFIX/lib/libgtfock.so" \
    "$PREFIX/lib/libcint.so" \
    "$PREFIX/lib/libGTMatrix.a" \
    "$PREFIX/include/pfock.h" \
    "$PREFIX/include/CInt.h" \
    "$PREFIX/lib/cmake/GTFock/GTFockTargets.cmake" \
    "$PREFIX/share/gtfock/examples/sto-3g.gbs" \
    "$PREFIX/share/gtfock/examples/water.xyz"; do
    if [[ ! -e $required ]]; then
        echo "build_deps.sh did not install $required" >&2
        exit 1
    fi
done

# Simint is generated and linked into libcint as a static implementation
# detail. Its standalone archive and development metadata are not part of the
# GTFock consumer ABI and conda-forge discourages shipping bundled static libs.
# Decision CF-SIMINT-006 requires the removal to be verified rather than
# assumed. $PREFIX is the shared conda-build host prefix holding every host
# dependency, so remove exactly the files this build's Simint install recorded
# rather than everything under $PREFIX whose name contains "simint"; anything
# else matching afterwards belongs to a dependency and is reported, not deleted.
simint_manifest="$GTF_BUILD_ROOT/simint/install_manifest.txt"
if [[ ! -s $simint_manifest ]]; then
    echo "No Simint install manifest at $simint_manifest: cannot prove which" \
         "installed files CF-SIMINT-006 must remove" >&2
    exit 1
fi
simint_artifacts=()
while IFS= read -r path || [[ -n $path ]]; do
    [[ -n $path ]] || continue
    # CMake install manifests may contain redundant separators (for example
    # include//simint). Normalize them so parent directories are deduplicated
    # and pruned in a deterministic deepest-first order below.
    path=$(realpath -m -- "$path")
    if [[ $path != "$PREFIX"/* ]]; then
        echo "Simint install manifest lists $path outside \$PREFIX" >&2
        exit 1
    fi
    simint_artifacts+=("$path")
done <"$simint_manifest"
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
if ! printf '%s\n' "${simint_artifacts[@]}" | grep -qxF "$PREFIX/lib/libsimint.a"; then
    echo "lib/libsimint.a is not owned by the Simint install manifest;" \
         "refusing to delete a file this build does not own" >&2
    exit 1
fi
rm -f -- "${simint_artifacts[@]}"
# Drop the directories the removed files leave behind, deepest first, so an
# emptied include/simint tree does not ship as a stray empty directory. Only
# directories that become empty are removed, and never $PREFIX itself.
simint_directories=()
for path in "${simint_artifacts[@]}"; do
    directory=$(dirname -- "$path")
    while [[ $directory == "$PREFIX"/* ]]; do
        simint_directories+=("$directory")
        directory=$(dirname -- "$directory")
    done
done
while IFS= read -r directory; do
    rmdir --ignore-fail-on-non-empty -- "$directory" 2>/dev/null || true
done < <(
    printf '%s\n' "${simint_directories[@]}" |
        awk '{ print length($0), $0 }' |
        sort -k1,1nr -k2,2r |
        cut -d' ' -f2- |
        awk '!seen[$0]++'
)
for path in "${simint_artifacts[@]}"; do
    if [[ -e $path ]]; then
        echo "Simint artifact survived removal: $path" >&2
        exit 1
    fi
done
simint_survivors=$(find "$PREFIX" -iname '*simint*' -not -path "$PREFIX/conda-meta/*" -print)
if [[ -n $simint_survivors ]]; then
    echo "Simint-named files remain under \$PREFIX but are not owned by this" \
         "build's Simint install manifest; resolve before packaging:" >&2
    printf '%s\n' "$simint_survivors" >&2
    exit 1
fi
# grep exits 2 on an unreadable or missing operand, so "no match" alone would
# let this assertion pass without reading the shipped metadata. Distinguish
# "scanned and clean" (1) from every other outcome.
set +e
simint_metadata=$(grep -R -i -e simint -- "$PREFIX/lib/cmake/GTFock" 2>&1)
simint_metadata_status=$?
set -e
if ((simint_metadata_status == 0)); then
    echo "GTFock CMake metadata references the removed Simint package:" >&2
    printf '%s\n' "$simint_metadata" >&2
    exit 1
fi
if ((simint_metadata_status != 1)); then
    echo "Simint metadata scan failed with grep status" \
         "$simint_metadata_status:" >&2
    printf '%s\n' "$simint_metadata" >&2
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
    "$PREFIX"/include/pfock.h
    "$PREFIX"/include/CInt.h
    "$PREFIX"/lib/cmake/GTFock
    "$PREFIX"/share/gtfock
)
shopt -u nullglob
# A non-existent literal survives nullglob, and grep exits 2 on an unreadable
# operand even when another operand matched, so "no match" alone would let this
# assertion pass without inspecting the shipped tree. Require every operand to
# exist and treat any grep failure as a leak.
if ((${#installed_artifacts[@]} == 0)); then
    echo "No installed GTFock artifacts to check for build-path leaks" >&2
    exit 1
fi
for artifact in "${installed_artifacts[@]}"; do
    if [[ ! -e $artifact ]]; then
        echo "Cannot check build-path leaks: $artifact is not installed" >&2
        exit 1
    fi
done
set +e
leaking_files=$(grep -R -F -l -e "$SRC_DIR" -e "$BUILD_PREFIX" \
    -- "${installed_artifacts[@]}" 2>&1)
leak_status=$?
set -e
if ((leak_status == 0)); then
    echo "An installed GTFock artifact contains a build-time path:" >&2
    printf '%s\n' "$leaking_files" >&2
    exit 1
fi
if ((leak_status != 1)); then
    echo "Build-path leak scan failed with grep status $leak_status:" >&2
    printf '%s\n' "$leaking_files" >&2
    exit 1
fi
