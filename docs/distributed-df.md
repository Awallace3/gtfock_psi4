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

    B[Q][mn] = sum_P X[QP] A[P][mn],  where X^T X = J^-1
    J[mn]    = sum_Q B[Q][mn] c[Q],   c[Q] = sum_rs B[Q][rs] D[rs]
    K[mn]    = sum_Q sum_i B[Q][mi] B[Q][ni],  B[Q][mi] = sum_n B[Q][mn] C[ni]

Both matrices see the fitted tensor only through `B^T B = A^T J^-1 A`, so `X` is
pinned only up to a left orthogonal factor: any square root, Cholesky factor or
pivoted Cholesky factor of `J^-1` gives the same `J` and `K`. "Factoring the
metric" below spends that freedom.

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
- **B. Fit (local triangular solve).** The metric factor is replicated
  (`naux^2 * 8 B ~= 392 MB` at `naux = 7000`, affordable), so
  `B[all Q][local mn]` is one local `dtrsm` that overwrites `A` in place.
- **C. Redistribute (phased pairwise `MPI_Sendrecv`).** Transpose the ownership
  from `all Q / local mn` to `local Q / all mn`. Moves the whole tensor exactly
  once for the lifetime of the SCF.
- **Per iteration.** Local half-transform, local partial `J` and `K`, then a
  single `MPI_Allreduce` of `2 * nbf^2` doubles - 27 MB at `nbf = 1863`,
  negligible against the integral work it replaces.

`MPI_Alltoallw` would express phase C in one call, and was rejected: its
displacements are byte counts in `int`, which overflow at the ~24 GB/rank this
exists to make possible, and chunking the `mn` axis shrinks the message but not
the send displacement. The phased `MPI_Sendrecv` loop sends the same number of
messages, needs no packing buffer - the receive side uses an `MPI_Type_vector`
to land each peer's columns directly in place - and has no such ceiling.

## Banding the exchange half-transform

`K` is where the per-iteration time goes, and for a while it went somewhere
avoidable. The half-transform needs `B_Q` as a dense `nbf x nbf` matrix, but
only unique shell pairs are stored, so the first implementation scattered one
auxiliary function's packed pair row into an `nbf^2` buffer and then multiplied
that buffer by the occupied coefficients. The scatter writes a `dM x dN` patch
per shell pair, strided by `nbf`, which touches a cache line per few useful
doubles: measured at 7.2 GB/s against a machine that streams several times
that, and 71% of the local J/K clock on a 574-function nanotube.

It is now banded. A thread takes one auxiliary function and one run of
consecutive primary shells, gathers just that band of rows - straight for
`N <= M`, transposed for `N > M`, always writing along a band row - and hands
the band to one GEMM against the occupied coefficients. The band is a few
hundred kilobytes, so the GEMM reads it back out of cache and the densified
matrix never reaches memory at all. Several auxiliary functions then share one
`dsyrk`, which is the same `K += H H^T` but passes over the `nbf x nbf`
accumulator once per batch instead of once per `Q`.

The band width is the one tuning parameter and both ends of it cost. One shell
per band leaves a GEMM two or three columns wide, too narrow to pay for its own
setup; a band that no longer fits in L2 alongside the coefficients puts the
densified rows back out to memory and gives up the point of banding. On a
24-core socket the cost is flat from about 16 to 48 rows and rises on either
side, so `PDF_PANEL_ROWS` sits at 32. Unlike the `nbf^2` buffer it replaced,
the scratch does not grow with the system.

Measured single-rank on one host, 24 threads, `6-31+G**`, against the scatter it
replaced:

| system | `nbf` | J/K before | after | speedup |
|---|---|---|---|---|
| peptide | 260 | 3.14 s | 1.47 s | 2.14x |
| nanotube | 574 | 27.50 s | 16.00 s | 1.72x |

What is left of the local clock is close to irreducible. Splitting one nanotube
build by matrix gives `J` 0.41 s and `K` 0.95 s. `J` is two `dgemv` over the
whole 4 GB local tensor and is flat from 6 to 24 threads, so it is already at
this machine's memory ceiling and no amount of restructuring will move it; the
two passes are inherent, since every `Q` of `c_Q` is needed before any element
of `J`. Of `K`, about 0.28 s is the gather - down from 1.18 s - and the rest is
the two BLAS calls running at roughly a quarter of peak.

One MVP shortcut remains, local and replaceable without touching the interface:
the shell-pair partition is a static balance on AO-element count rather than on
measured integral cost.

## Factoring the metric

The factorization is the one part of setup that gets no faster as ranks are
added: every rank runs it bit-for-bit on the same `naux x naux` data, and since
each rank is bound to `ncores / nranks` cores, at a fixed core count it gets
*slower*. Reducing it therefore means reducing its flops, not distributing them,
until `naux^2` stops fitting comfortably - at which point MKL ScaLAPACK is
already linked through the `gtf_numeric` interface target and is the documented
upgrade.

The first version formed `J^-1/2` with a symmetric eigendecomposition: `dsyev`
at `~9 naux^3` plus a `dsyrk` at `naux^3` to reassemble, holding the
eigenvectors and the assembled `J^-1/2` at the same time. Nothing downstream
needs the inverse square root, per the identity above. `PDF_create` now takes a
pivoted Cholesky of the metric instead - `dpstrf`, `naux^3 / 3` flops, in place
- and phase B falls out of it as a triangular solve at half the GEMM's flops,
also in place.

Truncation changes meaning with the factorization, and that is a real
behavioural difference rather than a reformulation. `dpstrf` pivots the largest
remaining Schur-complement diagonal to the front and stops once it falls below
`fitting_cond` times the first, so the retained leading block is factored
exactly and the result is an exact fit in a pivot-selected subset of the
auxiliary basis. The eigen path, like Psi4's `DFHelper` under
`DF_FITTING_CONDITION`, instead dropped every eigenvector below `fitting_cond`
times the largest eigenvalue: a pseudo-inverse over the whole basis. The two
criteria select the same functions on a metric that is clearly well conditioned
or clearly rank-deficient, and disagree when its spectrum straddles the cutoff,
where the pivot criterion keeps more.

Water in `cc-pVDZ`/`cc-pVDZ-JKFIT` at Psi4's 1e-10 default is exactly that case.
The smallest eigenvalue is `3.83e-08` against an eigenvalue floor of `4.17e-08`
and a pivot cutoff of `4.42e-09`, so `DFHelper` discards one function and this
engine keeps all 131. Measured against the untruncated fit `A^T J^-1 A`, the
eigen path is `8.0e-07` off and the Cholesky `1.6e-14`, and `J` differs between
the two engines by `3.4e-07`. Psi4's `tests/pytests/test_gtfock.py` pins the
condition to 1e-12 for its tight `MemDFJK` comparisons for that reason and
covers the difference itself in a separate test. `PDF_nFitFuncs` and
`PDF_nMetricNullVectors` report what survived.

## Setup memory

Steady state is one tensor slice per rank, `naux * npair_ao / nranks` doubles.
Setup peaks above it because phase C cannot release the phase-A buffer until the
last `MPI_Sendrecv` has completed, so a multi-rank run holds two slices at once
- roughly `2x` steady state, plus the replicated `naux^2` metric.

Two of the three excess copies are gone. The in-place `dtrsm` removed the
separate destination buffer phase B used to need, along with the assembled
`J^-1/2` that sat beside the eigenvectors. At one rank there is no
redistribution at all, so the integral buffer is adopted as the tensor and
shrunk with `realloc`, and setup peaks at one slice plus the metric. Bringing
the multi-rank peak to the same place requires phases A/B/C blocked over column
strips of `mn`, which is a larger change and has not been made.

`PDF_phaseSeconds` reports this rank's wall seconds in each of the five phases
(`metric`, `factor`, `int3c`, `fit`, `redist`), so where setup goes is measured
rather than modelled. Each rank times its own elapsed seconds including any wait
inside a collective, so the spread across ranks is the load imbalance.

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
immutable. Both files ship in `GTFock::GTFockDF`:

- `src/gtfock_df.c` / `gtfock_df.h` - the serial integral layer above
  (`GTFDF_compute3c`, `GTFDF_compute2c`). No MPI.
- `src/gtfock_pdf.c` / `gtfock_pdf.h` - phases A/B/C and the per-iteration
  contraction (`PDF_create`, `PDF_computeJK`, `PDF_destroy`). `PDF_create` is
  collective over a duplicated communicator and does all the expensive work;
  `PDF_computeJK` takes a replicated density and occupied coefficients and
  returns replicated `J` and `K`.

Because `gtfock_pdf.h` names `MPI_Comm`, `MPI::MPI_C` is a public dependency of
the exported target; OpenMP and BLAS/LAPACK stay private. BLAS and LAPACK are
called through the LP64 Fortran symbols (`dgemm_`, `dgemv_`, `dsyrk_`,
`dtrsm_`, `dpstrf_`) declared in the translation unit rather than through
`<mkl.h>`, so
the file builds unchanged against either MKL or the netlib libraries that
`gtf_numeric` also carries.

The Psi4 `JK` subclass that consumes this belongs in Psi4's own `libfock`, not
here - this repository's milestone is the native foundation and its CMake
interface.

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
   libcint's exact four-center path. The auxiliary basis is built to span the
   primary products *exactly* - one `s` primitive on A and one `p` primitive on
   B, fitted by `s(2a)` at A, `s(a+b)` and `p(a+b)` at the Gaussian product
   center, and Cartesian `d(2b)` at B - so Coulomb fitting has zero error and
   the comparison runs at machine precision (tol 1e-9, observed ~3e-14) rather
   than at fitting accuracy.

`tests/test_pdf_jk.c` checks the distributed build the same way, and is
registered at 1, 2, and 3 ranks. A third primary shell on a third center
extends the exact-span construction to six shell pairs and 17 auxiliary
functions, so DF `J` and `K` must equal the exact four-center matrices to
machine precision (observed ~1e-15). It also asserts that both partitions cover
their axis exactly once, that `J` and `K` come back identical on every rank, and
that asking for one matrix does not change the other. Three ranks is the
interesting count: the 9-element `(p,p)` block cannot be split, so one rank
receives no shell pairs at all and the empty-block paths in phase A and in the
redistribution run. The test asserts that too, rather than leaving it to
coincidence.
