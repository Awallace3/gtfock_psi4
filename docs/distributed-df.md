# Distributed density fitting

GTFock distributes exact four-center ERIs. This note specifies a second,
independent engine that distributes the *density-fitted* Coulomb and exchange
build, and records why the distribution axis is not the one the exact path uses.

## Why a DF path at all

Psi4's in-core `MemDFJK` keeps the full fitted tensor `B[Q][mn]` on one process.
For the 157-atom protein in `6-31+G**` (1863 primary functions, ~7000 auxiliary
functions) that tensor is

    naux * nbf^2 * 8 B = 7000 * 1863^2 * 8 B ~= 194 GB

which matches the 194145 MB peak RSS measured for the `MemDFJK` arm of Phoenix
job 12639820, and is why the 1863-function point in job 12638902 fell back to
`DiskDFJK`. Split over 8 ranks the same tensor is ~24 GB/rank: the disk fallback
disappears and the fit itself parallelizes. The setup cost this removes is not
incidental - it is 23-37% of SCF wall time across every system benchmarked, and
is invisible to the `JK: JK` timer (see Psi4's `doc/sphinxman/source/gtfock.rst`,
"What the J/K timer does not cover").

## The distribution axis

With `A[P][mn] = (P|mn)` and metric `J[PQ] = (P|Q)`,

    B[Q][mn] = sum_P [J^-1/2][PQ] A[P][mn]
    J[mn]    = sum_Q B[Q][mn] c[Q],   c[Q] = sum_rs B[Q][rs] D[rs]
    K[mn]    = sum_Q sum_i B[Q][mi] B[Q][ni],  B[Q][mi] = sum_n B[Q][mn] C[ni]

Coulomb is content with any distribution: `c` is a length-`naux` reduction and
`J` is a local scaling. Exchange is not. `K` contracts two `B` blocks that share
`Q`, so a rank that owns only part of `Q` for a given `mn` cannot form any part
of `K` without communicating the half-transformed `(Q, m, i)` tensor, which is
`naux * nbf * nocc` - the same order as `B` itself and far too large to move per
iteration. **`K` forces `Q`-locality**, so the layout is chosen for `K` and one
bulk redistribution is paid once, at setup.

## Phases

- **A. Integrals (no communication).** Rank `r` owns a contiguous block of
  primary shell pairs `mn` and computes `A[P][mn]` for *all* `P` over that
  block. Perfectly parallel; this is the expensive phase.
- **B. Fit (local GEMM).** The metric power `J^-1/2` is replicated
  (`naux^2 * 8 B ~= 392 MB` at `naux = 7000`, affordable), so `B[all Q][local mn]`
  is one local `dgemm`.
- **C. Redistribute (one `MPI_Alltoallv`).** Transpose the ownership from
  `all Q / local mn` to `local Q / all mn`, in `mn` chunks. Moves the whole
  tensor exactly once for the lifetime of the SCF.
- **Per iteration.** Local half-transform, local partial `J` and `K`, then a
  single `MPI_Allreduce` of `2 * nbf^2` doubles - 27 MB at `nbf = 1863`,
  negligible against the integral work it replaces.

The MVP forms `J^-1/2` with a replicated symmetric eigendecomposition; the
`gtf2` environment routes LAPACK to threaded OpenBLAS, so this is not serial.
MKL ScaLAPACK is already linked through the `gtf_numeric` interface target and
is the documented upgrade when `naux^2` stops fitting comfortably.

## Integral layer

libcint exposes only four-center quartets over a single `BasisSet_t`: there is
no auxiliary basis, no three-center and no two-center Coulomb entry point. Simint
underneath it does support them, through `simint_create_zero_shell()`, whose own
documentation names three- and two-center integrals as the use case. So
`src/gtfock_df.c` builds its own Simint layer over two `BasisSet_t` objects:

- primary and auxiliary shells built exactly as `libcint/cint_simint.c` builds
  them, including `simint_normalize_shells()`;
- **one zero shell, created after that call and never passed to it.** That
  function's own comment - "we assume there are no unit shells (shells with zero
  orbital exponent)" - is a correctness precondition, not a remark: normalizing a
  zero-exponent shell divides by zero;
- bra pairs `(P, zero)` for each auxiliary shell, ket pairs `(M, N)` for each
  ordered primary shell pair;
- `(P|MN)` is then `simint_compute_eri(pair(P,zero), pair(M,N), ...)` and
  `(P|Q)` is `simint_compute_eri(pair(P,zero), pair(Q,zero), ...)`, unchanged.

Cartesian only, matching the exact path: Simint fills Cartesian shell blocks and
Psi4's `GTFockJK` already refuses `has_puream()`.

### Angular momentum ceiling

`libcint/CInt.h` hardcodes `_SIMINT_OSTEI_MAXAM 4` while the Simint actually
generated here reports `SIMINT_OSTEI_MAXAM 5`
(`_install/include/simint/ostei/ostei_config.h`). GTFock's `fock_task.c` sizes
its angular-momentum pair table from the libcint value, so an `l = 5` shell
would index past it; what prevents that today is Psi4's `check_supported()`
guard rejecting `am > 4`, not anything in libcint. Auxiliary sets routinely
carry `l = 5`, so the DF engine sizes every buffer from
`simint_ostei_workmem()` and its own observed maximum angular momentum, and
does not consult `_SIMINT_OSTEI_MAXAM`.

## Where the code lives

The kernel is project-owned and lives in `src/`, because pinned submodules stay
immutable. It exports as `GTFock::GTFockDF`. The Psi4 `JK` subclass that
consumes it belongs in Psi4's own `libfock`, not here - this repository's
milestone is the native foundation and its CMake interface.

## Validation without committed reference data

`tests/test_df_ints.c` checks the integral layer with no stored reference:

1. **Analytic two-center.** For uncontracted `s` shells the normalized
   `(P|Q)` has a closed form,
   `N_a N_b * 2 pi^{5/2} / (a b sqrt(a+b)) * F_0(rho R^2)` with
   `rho = ab/(a+b)`, `N = (2a/pi)^{3/4}`, `F_0(x) = sqrt(pi/4x) erf(sqrt x)`.
   The normalization is exactly reproducible from
   `simint-generator/skel/simint/shell/shell.c` and libcint's Cartesian path,
   which skips its own `normalization()` entirely. This validates the zero shell
   and the normalization chain to ~1e-13.
2. **RI reconstruction.** `(mn|rs) ~= sum_PQ (mn|P) [J^-1][PQ] (Q|rs)` against
   libcint's exact four-center path. The fitting error makes this a loose check
   (1e-3), but the bugs it exists to catch - transposed AO ordering, a dropped
   normalization, a mis-sized shell block - are `O(1)` errors, not `1e-3` ones.
