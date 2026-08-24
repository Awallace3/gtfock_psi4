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
