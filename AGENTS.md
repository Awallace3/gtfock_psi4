# Project agent memory

This file is the project's committed home for project-intrinsic agent knowledge: build, test, release, architecture, and sharp-edge notes that should travel with the code.

- Use `env.yml` (audited direct constraints) or `conda-linux-64.lock` (exact Linux solve); the supported C/C++ toolchain is conda-forge IntelLLVM `icx`/`icpx` with OpenMPI.
- The canonical clean validation is `CMAKE_BUILD_PARALLEL_LEVEL=12 ./build_deps.sh --clean`; it builds through CMake and runs Simint plus native MPI/numerical regressions.
- Pinned submodules stay immutable. `build_deps.sh` applies `patches/gtfock-modern.patch` to `_build/gtfock-src`; numerical causality and counterfactual evidence are in `docs/numerical-root-cause.md`.
- This repository milestone is the native GTFock foundation. Optional Psi4/mpi4py integration is intentionally handled separately; see `README.md`.

## Maintaining this file

Keep this file for knowledge useful to almost every future agent session in this project.
Do not repeat what the codebase already shows; point to the authoritative file or command instead.
Prefer rewriting or pruning existing entries over appending new ones.
When updating this file, preserve this bar for all agents and keep entries concise.
