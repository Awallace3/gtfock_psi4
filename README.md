# gtfock_psi4: GTFock foundation

Reproducible conda-forge Linux build of GTFock with Intel oneAPI LLVM
(`icx`/`icpx`) and OpenMPI. This milestone covers the native GTFock foundation;
optional Psi4/mpi4py integration is intentionally out of scope.

The superproject pins every source submodule. The build applies the reviewed
modern-compiler patch to a disposable copy, leaving pinned submodules unchanged.

## Create the `gtf2` environment

For the audited conda-forge-only specification:

```bash
conda env create --file env.yml --solver libmamba
conda activate gtf2
```

For the exact solved Linux package URLs used for validation:

```bash
conda create --name gtf2 --file conda-linux-64.lock
conda activate gtf2
```

Verify activation selected one toolchain and MPI ABI:

```bash
command -v icx icpx mpicc mpirun python
icx --version
icpx --version
mpirun --version
```

`env.yml` pins oneAPI 2025.3.1, OpenMPI 5.0.8, MKL 2025.3.1, GNU
Fortran 14.3, and every configure/build/test tool. No system compiler, MPI,
BLAS, LAPACK, or ScaLAPACK library is used.

## Clean CMake build and tests

Clone recursively, activate `gtf2`, then run:

```bash
git submodule update --init --recursive
CMAKE_BUILD_PARALLEL_LEVEL=12 ./build_deps.sh --clean
```

The script cleanly builds the Simint generator and AVX2 generated library, then
configures the root CMake project with absolute conda `icx`, `icpx`, GNU
Fortran, OpenMPI, and MKL/ScaLAPACK paths. Artifacts are installed under
`_install/`. Set `SIMINT_VECTOR` only when the target CPU supports a different
Simint vector backend.

The build runs:

- generated Simint tests;
- an installed `pfock.h` public-header compile smoke test;
- the atomic Fock-update regression;
- a real two-rank GTFock/Simint overlap calculation against the read-only legacy
  GTFock result (`1e-13` tolerance; observed bitwise agreement);
- two-rank RHF/STO-3G water with a physical core-Hamiltonian fallback when SAD
  files are absent (`-74.9450213019 Eh`, `1e-9 Eh` tolerance).

Re-run the GTFock tests directly with:

```bash
ctest --test-dir _build/gtfock --output-on-failure
```

Run the native SCF path with:

```bash
OMP_NUM_THREADS=1 mpirun --oversubscribe -n 2 _install/bin/pscf \
  GTFock/data/sto-3g.gbs GTFock/data/water.xyz 2 1 1 2 15
```

## Numerical root cause

See [`docs/numerical-root-cause.md`](docs/numerical-root-cause.md) for the
legacy comparison, one-variable counterfactuals, initiating atomic bit-cast
bug, missing-SAD masking condition, visible failure, disconfirming checks, and
tolerance justification.
