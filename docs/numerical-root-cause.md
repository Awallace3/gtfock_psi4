# Numerical divergence: evidence and fix

The reference checkout `/home/awallace43/gits/GTFock` was treated as read-only.
Its `master` is `bae28ef`; this repository pins the modern CMake line at
`383dfba`. Both descend from `b2b6b1`.

## Earliest causal divergence

Commit `c5b9270` replaced Intel classic's `_castf64_u64` in
`pfock/update_F.h` with a C cast to `uint64_t`:

```c
expected_value = (uint64_t)old_value;
new_value = (uint64_t)(old_value + addend);
```

`_castf64_u64` is a **bit cast**. A C integer cast is a **value conversion**.
The converted integers were passed to an integer compare-and-swap operating on
the double's representation. For example, adding `1.25` to `0.0` stored the
integer bit pattern `0x1`, which is about `4.94e-324` as a double. A subsequent
nonzero update normally spins because the converted expected value no longer
matches the stored double bits. `tests/test_atomic_add.c` executes this primitive
and its concurrent OpenMP use directly.

The initiating trigger was therefore the incorrect replacement bit conversion,
not `icx` optimization. The compiler condition was that Intel classic supplied
the nonstandard intrinsic while IntelLLVM does not. The portable fix uses the
compiler's generic atomic load/compare-exchange on `double`, preserving the
legacy bitwise CAS semantics.

## Mask and visible symptom

The pinned `run.sh` switched its active example to `data/sto-3g.gbs`, but that
file has no colocated `H.dat`/`O.dat` SAD guesses. libcint intentionally replaces
missing SAD files with zero matrices. That zero density masked the broken atomic
add during iteration 0 because all Fock increments were zero. The observed
modern path therefore reported the nuclear-only energy `9.7793999065 Eh`; after
purification made the density nonzero, it hung in the next Fock build.

Focused counterfactuals separated the two effects:

1. Pinned source + `icx` + STO-3G (no SAD files): all 49 density entries were
   zero, iteration 0 had zero electronic energy, and iteration 1 stalled.
2. Same binary/compiler/MPI/ranks + `cc-pvdz/cc-pvdz.gbs` (SAD files present):
   the initial density was nonzero and the first Fock build stalled. This
   disconfirms compiler choice and the initial-guess copy as the hang trigger.
3. Correct atomic primitive + the original missing-SAD input: the SCF driver
   uses a standard core-Hamiltonian fallback and converges normally.

The fallback is not a constant-density mask: it transforms the already computed
core Hamiltonian into the orthonormal basis with the same `X` the SCF loop
applies every pass, then purifies it, yielding a physically meaningful AO
density.

## Confounders fixed but not blamed

- `one_electron.c` queried `pdsyev` (no integer workspace) and then read
  uninitialized `iwork[0]`. The unused integer workspace was removed and the
  ScaLAPACK local leading dimension is now at least one.
- Commit `383dfba` added diagnostics with `<= nbf` bounds, reading beyond
  `nbf*nbf`. Those diagnostics postdate the `b96024d` zero-density observation,
  so they cannot be its initiating cause; they were removed from the staged
  source.
- The legacy tree itself has `memset(pfock, 0, sizeof(PFock_t))`, which clears
  only a pointer-sized prefix and happened to be masked in its historical
  allocator/toolchain. For the read-only numerical comparison only, a disposable
  copy corrected that one initialization and emulated the classic bit-cast
  intrinsic; the reference checkout was not changed.

## Numerical evidence and tolerances

With two OpenMPI ranks and one OpenMP thread:

- modern and read-only-legacy GTFock/Simint overlap matrices are bitwise
  identical (`max_abs=0`); `tests/test_overlap.c` retains the legacy matrix and
  enforces `1e-13` absolute agreement and symmetry;
- fixed GTFock RHF/STO-3G water converges in eight reported iterations to
  `-74.9450213019 Eh`;
- `tests/test_pscf_regression.py` enforces `1e-9 Eh` agreement with that frozen
  converged result and fails if the missing-SAD path returns the old
  nuclear-only energy or the Fock update hangs. The two-rank launch has a
  30-second wall limit and terminates the entire MPI process group on timeout,
  preventing a failed counterfactual from leaving orphaned ranks.

The overlap tolerance is near roundoff for normalized STO-3G integrals. The SCF
energy tolerance is much tighter than chemical accuracy and allows only benign
fixed-order floating-point variation; optimization, MPI, and the real integral
path remain enabled.
