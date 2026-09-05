# Project agent memory

This file is the project's committed home for project-intrinsic agent knowledge: build, test, release, architecture, and sharp-edge notes that should travel with the code.

- Use `env.yml` (audited direct constraints) or `conda-linux-64.lock` (exact Linux solve); the supported C/C++ toolchain is conda-forge IntelLLVM `icx`/`icpx` with OpenMPI.
- The canonical clean validation is `CMAKE_BUILD_PARALLEL_LEVEL=12 ./build_deps.sh --clean`; it builds through CMake and runs Simint plus native MPI/numerical regressions.
- Pinned submodules stay immutable. `build_deps.sh` applies `patches/gtfock-modern.patch` to `_build/gtfock-src`; numerical causality and counterfactual evidence are in `docs/numerical-root-cause.md`.
- The local package gate is `./conda/build-local.sh`; it builds the artifact, runs installed CMake/MPI tests, and validates a fresh CPU-only install. Submission blockers and the OpenMPI solver-only CUDA metadata workaround are keyed in `docs/conda-packaging.md`.
- On an HPC login node the site modules silently hijack the conda toolchain: `CPATH` is searched ahead of the `-isystem` that carries conda's `mpi.h`, and `GCC_ROOT` redirects conda's gfortran to a site GCC. `build_deps.sh` and `set_vars.sh` scrub both; the incident, the operator procedure, and the `nm -Du <lib> | grep -c ompi_mpi_` ABI audit are in `docs/hpc-site-mpi.md`.
- This repository milestone is the native GTFock foundation and installable CMake interface. Optional Psi4/mpi4py integration implementation is handled separately; see `README.md`.

## Maintaining this file

Keep this file for knowledge useful to almost every future agent session in this project.
Do not repeat what the codebase already shows; point to the authoritative file or command instead.
Prefer rewriting or pruning existing entries over appending new ones.
When updating this file, preserve this bar for all agents and keep entries concise.
