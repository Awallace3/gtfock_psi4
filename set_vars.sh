#!/usr/bin/env bash

if [[ -z ${CONDA_PREFIX:-} ]]; then
    echo "Activate the gtf2 conda environment first." >&2
    return 2 2>/dev/null || exit 2
fi

export CC="$CONDA_PREFIX/bin/icx"
export CXX="$CONDA_PREFIX/bin/icpx"
export FC="$CONDA_PREFIX/bin/x86_64-conda-linux-gnu-gfortran"
export MPICC="$CONDA_PREFIX/bin/mpicc"
export MPICXX="$CONDA_PREFIX/bin/mpicxx"
# A site MPI module exports MPIF90/MPIF77 alongside MPICC; leaving them behind
# would point a Fortran build at a different MPI than the C one.
export MPIFORT="$CONDA_PREFIX/bin/mpifort"
export MPIF90="$MPIFORT"
export MPIF77="$MPIFORT"

# icx and gcc search CPATH ahead of the -isystem directories CMake emits for
# MPI::MPI_C, and LIBRARY_PATH ahead of the link directories, so a loaded HPC
# site MPI module substitutes its own <mpi.h> and libmpi into a build that
# names only conda tools. build_deps.sh does the same scrub for the builds it
# drives; this covers the interactive `cmake --build` in an already-configured
# tree, which is how the mismatch documented in docs/hpc-site-mpi.md was
# introduced after a clean configure.
_gtf_conda_root=$(readlink -f "$CONDA_PREFIX" 2>/dev/null || true)
_gtf_conda_root=${_gtf_conda_root:-$CONDA_PREFIX}
# GCC_ROOT, GCC_EXEC_PREFIX and COMPILER_PATH are scrubbed by the same rule:
# conda-forge's gcc reads them to locate its own installation and subprograms, so
# a site gcc module exporting them sends conda's gfortran looking for f951 under
# the site GCC tree, and the Fortran language check fails outright.
for _gtf_search_path in CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH \
                        OBJC_INCLUDE_PATH OBJCPLUS_INCLUDE_PATH LIBRARY_PATH \
                        GCC_ROOT GCC_EXEC_PREFIX COMPILER_PATH \
                        MPI_ROOT MPI_HOME I_MPI_ROOT; do
    _gtf_value=${!_gtf_search_path:-}
    [[ -n $_gtf_value ]] || continue
    _gtf_kept=()
    _gtf_dropped=()
    IFS=: read -r -a _gtf_entries <<<"$_gtf_value"
    for _gtf_entry in "${_gtf_entries[@]}"; do
        [[ -n $_gtf_entry ]] || continue
        if [[ $(readlink -f "$_gtf_entry" 2>/dev/null || echo "$_gtf_entry") == "$_gtf_conda_root"/* ]]; then
            _gtf_kept+=("$_gtf_entry")
        else
            _gtf_dropped+=("$_gtf_entry")
        fi
    done
    ((${#_gtf_dropped[@]})) || continue
    echo "Dropping non-conda $_gtf_search_path entries: ${_gtf_dropped[*]}" >&2
    if ((${#_gtf_kept[@]})); then
        export "$_gtf_search_path=$(IFS=:; echo "${_gtf_kept[*]}")"
    else
        unset "$_gtf_search_path"
    fi
done
unset _gtf_conda_root _gtf_search_path _gtf_value _gtf_kept _gtf_dropped
unset _gtf_entries _gtf_entry
