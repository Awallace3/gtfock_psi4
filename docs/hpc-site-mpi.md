# Building and running on an HPC system with a site MPI

This stack is built against conda-forge OpenMPI. Every HPC login node this
project has been used on ships its own MPI, loaded by default, and the two do
not share an ABI. Nothing in a normal build reports the collision: the compiler
accepts it, the linker accepts it, and the program faults inside MPI on its
first call that takes a communicator. This note records the incident that cost
a benchmark sweep, the reason the ordinary provenance checks did not catch it,
and the procedure that does.

## The failure

Phoenix (Georgia Tech PACE) job 12819579 aborted at a pre-flight `ctest` gate
70 s in. All three ranks counts died the same way:

```
df_distributed_jk_np1  Subprocess aborted
    signal 11 (Segmentation fault), address 0x54110540
    PMPI_Comm_dup+0x3f
    PDF_create+0x92          <- src/gtfock_pdf.c, MPI_Comm_dup(comm, &p->comm)
```

`PDF_create` is the first line of this project's own code that touches MPI, and
it crashed before executing anything of its own. Memory was not a factor
(692 MB used of 1000 GB), and np1 fails as readily as np3, so it is not a rank
count or a fabric problem.

## The cause

Open MPI's `MPI_Comm` is `ompi_communicator_t *`. In the MPICH family - MPICH,
MVAPICH2, Intel MPI - `MPI_Comm` is an `int`. The function names are identical,
so an MVAPICH2 `mpi.h` compiled against conda's `libmpi.so.40` produces no
diagnostic at any stage.

The library really was compiled that way. In the object that crashed:

```
PDF_create:  mov %edi,%ebp             # MPI_Comm taken as a 32-bit int
             movl $0x4000000,(%r12)    # MPICH's MPI_COMM_NULL, as an immediate
nm -u libgtfockdf.so.0.1.0 | grep -c ompi_mpi_   ->  0
```

while the caller, built in a different shell, had the Open MPI ABI:

```
main:        mov 0x44666(%rip),%rdi    # ompi_mpi_comm_world, a full pointer
nm -u test_pdf_jk | grep -c ompi_mpi_            ->  4
```

So `MPI_COMM_WORLD` was passed as `0x0000155554110460` and received as
`0x54110460`. `PMPI_Comm_dup+0x3f` is `testb $0x30,0xe0(%rbx)`, the
`ompi_comm_invalid` read of `c_flags`, so the faulting address is
`0x54110460 + 0xe0 = 0x54110540` - exactly the address reported. The crash
address is a truncated pointer, not a wild one.

The header came from `CPATH`. A Phoenix login node loads `mvapich2/2.3.7-1` by
default and puts its include directory first:

```
$ echo $CPATH | tr : '\n' | head -1
/usr/local/pace-apps/spack/packages/linux-rhel9-x86_64_v3/gcc-12.3.0/mvapich2-2.3.7-1-.../include
```

**`icx` and `gcc` both search `CPATH` ahead of `-isystem`**, and `MPI::MPI_C`
contributes its include directory as `-isystem`. Adding `-I$CONDA_PREFIX/include`
does not help either: `CPATH` still wins under `icx`. Verified with `icx -H`:

| environment | `#include <mpi.h>` resolves to |
| --- | --- |
| login node as-is | MVAPICH2's `mpi.h` |
| plus `-I$CONDA_PREFIX/include` | MVAPICH2's `mpi.h` |
| `env -u CPATH` | conda's `mpi.h` |

`LIBRARY_PATH` has the mirror-image hazard: the same module puts its `lib`
directory first there, so a bare `-lmpi` can resolve to the site library.

UCX (`libucs`) appears at the top of the backtrace because it installs Open
MPI's signal handler. It reports the fault; it does not cause it.

## Two more hijacks from the same modules

Chasing the first one turned up a second, unrelated to MPI. PACE's `gcc/12.3.0`
module exports `GCC_ROOT` pointing at its own Spack prefix, and conda-forge's
GCC driver reads `GCC_ROOT` as *its* installation prefix. The driver then looks
for its own backends under the site tree, where they do not exist:

```
$ x86_64-conda-linux-gnu-gfortran -c t.f
x86_64-conda-linux-gnu-gfortran: fatal error: cannot execute 'f951': posix_spawnp: No such file or directory
```

Same binary, same arguments, only the environment differs:

| environment | first entry of `-print-search-dirs` `programs:` |
| --- | --- |
| `GCC_ROOT` from the site module | `<spack gcc-12.3.0>/libexec/gcc/x86_64-conda-linux-gnu/14.4.0/` |
| `GCC_ROOT=$CONDA_PREFIX` | `$CONDA_PREFIX/libexec/gcc/x86_64-conda-linux-gnu/14.4.0/` |
| `env -i` | `$CONDA_PREFIX/bin/../libexec/gcc/x86_64-conda-linux-gnu/14.4.0/` |

Note the version directory: `14.4.0` is conda's GCC, so the driver is correctly
identifying itself and only the prefix is wrong. `GCC_EXEC_PREFIX` and
`COMPILER_PATH` (the latter set here by the `xalt` module) redirect the same
search and are scrubbed alongside it.

This one is not silent - it stops the build at `project(... Fortran)` - but it
has the same shape and the same fix, so the scrub covers all three.

With that cleared, a third appeared. The `mvapich2` module exports `MPI_ROOT`,
and `FindMPI` searches `ENV MPI_ROOT` as a *hint*, which `find_program`
consults **before** `PATH`. So conda's `mpicc` being first on `PATH` does not
help:

```
-- Found MPI_C: <spack mvapich2-2.3.7-1>/lib/libmpi.so (found version "3.1")
CMake Error at CMakeLists.txt:118 (message):
  MPI resolved outside the supported conda prefixes
```

That error is this project's own prefix gate refusing the result, which is the
outcome we want from a check - but the build still stops, so `MPI_ROOT`,
`MPI_HOME` and `I_MPI_ROOT` are scrubbed too, and `set_vars.sh` repoints
`MPIF90`/`MPIF77` at conda alongside the `MPICC`/`MPICXX` it already set.

The pattern is worth stating once: **the site environment does not just add
directories to a search path, it hands build tools an explicit root to prefer.**
Naming the compiler you want is not sufficient; the environment has to be
cleared as well.

## Why the existing checks missed it

`build_deps.sh` already required `cmake`, `ninja`, `python`, `git`, `icx`,
`icpx`, `mpicc` and `mpirun` to resolve inside a conda prefix, and
`CMakeLists.txt` already held `MPI_C_LIBRARIES` and the numerical libraries to
the same rule. All of those passed. The gate covered the MPI *library* and
never the MPI *header the compiler actually opened*, and `CPATH` changes only
the second.

The timing made it worse. The four benchmark variant libraries were configured
in a clean shell and then built later with `cmake --build` in a shell that had
the default modules loaded. A configure-time check could not have fired: by the
time the poisoned compile ran, configure was long over. That is why the guard
this project now carries is a **per-translation-unit** one.

## What the repository now does

1. `build_deps.sh` and `set_vars.sh` drop every entry outside the active conda
   prefixes from `CPATH`, `C_INCLUDE_PATH`, `CPLUS_INCLUDE_PATH`,
   `OBJC_INCLUDE_PATH`, `OBJCPLUS_INCLUDE_PATH`, `LIBRARY_PATH`, `GCC_ROOT`,
   `GCC_EXEC_PREFIX`, `COMPILER_PATH`, `MPI_ROOT`, `MPI_HOME` and `I_MPI_ROOT`,
   printing what they removed.
   `set_vars.sh` covers the interactive `cmake --build` in an already-configured
   tree, which is the path that actually broke.
2. `CMakeLists.txt` resolves the `mpi.h` that ships with the MPI library
   `FindMPI` accepted, holds it to the same conda-prefix rule as
   `MPI_C_LIBRARIES`, and records which implementation it is.
3. That record is substituted into `src/gtfock_mpi_abi.h.in` and generated as
   `gtfock_mpi_abi.h`. The header compares the implementation the build was
   configured against with the macros defined by the `<mpi.h>` the preprocessor
   actually opened, and `#error`s on a mismatch with a message that names
   `CPATH`. It is force-included (`-include`) into every target that compiles
   MPI code - `gtfockdf`, `gtfock`, `GTMatrix`, `pscf`, `test_pdf_jk`,
   `test_overlap` - so it is checked on every compile, not once per configure.
   `gtfock_pdf.h` includes it too, so consumers of the installed package get the
   same check against their own `mpi.h`.
4. `tests/test_mpi_abi_guard.cmake` (ctest `mpi_abi_guard`) shadows `<mpi.h>`
   with a decoy from the other implementation, through `-I` and through `CPATH`,
   and fails if the guard accepts the build or rejects it without naming
   `CPATH`.

A run-time check cannot substitute for this. By the time `PDF_create` receives
the communicator the pointer has already been truncated, and there is no valid
value to compare it against.

## Before and after, on a compute node

Verified on `atl1-1-02-005-24-1.pace.gatech.edu` (SLURM job 12840982, 24 cores,
2026-09-04). One `srun` ran both halves back to back, so the *only* difference
between them is the environment scrub and the rebuilt library.

Before, against the library the benchmark shipped:

```
$ nm -u .../fm-gtfock/diag38/tree/_install/lib64/libgtfockdf.so.0.1.0 | grep -c ompi_mpi_
0
$ mpirun -n 1 .../test_pdf_jk ...
Caught signal 11 (Segmentation fault: address not mapped to object at address 0x540df540)
  4 .../libmpi.so.40(PMPI_Comm_dup+0x3f)
  5 .../libgtfockdf.so.0(PDF_create+0x92)
exit 139
```

After `source set_vars.sh` (which reported dropping the mvapich2, pmix, slurm
and gcc entries from `CPATH` and `LIBRARY_PATH`, and unsetting `GCC_ROOT`,
`COMPILER_PATH` and `MPI_ROOT`), a clean configure and build:

```
-- Found MPI_C: .../envs/p4gtf/lib/libmpi.so
$ nm -u .../diag38/build/gtfock/libgtfockdf.so.0.1.0 | grep -c ompi_mpi_
3
$ ctest --test-dir .../diag38/build/gtfock --output-on-failure -R "pdf|df"
    Start 5: df_integrals ................ Passed 0.10 sec
    Start 6: df_distributed_jk_np1 ....... Passed 1.87 sec
    Start 7: df_distributed_jk_np2 ....... Passed 1.76 sec
    Start 8: df_distributed_jk_np3 ....... Passed 1.79 sec
100% tests passed, 0 tests failed out of 4
```

The undefined-symbol count is the whole story in one number: 0 means the object
files were compiled against a header that made `MPI_Comm` an `int` and the Open
MPI symbols were never referenced; 3 means they were compiled against the header
belonging to the library that gets linked.

The full suite (13 tests) passes on the same node, and the guard was exercised
both ways: `ctest -R mpi_abi_guard` passes, and touching `src/gtfock_pdf.c` and
rebuilding with the mvapich2 include directory put back into `CPATH` now stops
at the first object file with the `#error` above instead of producing a library
that segfaults.

## Operator procedure

Both an interactive allocation and a batch job are shown. The build is done
inside the allocation, not on the login node, because the login node is where
the site modules are loaded and because it keeps the build and the test in one
environment.

```bash
R=/path/to/scratch/gtfock                       # your work root
salloc --no-shell -J gtfock-build -A <account> -q <qos> \
       -p cpu-small -N 1 -n 1 -c 24 --mem=32G -t 01:00:00
# note the job id, then run each step with:  srun --jobid=<id> ...
```

Inside the allocation:

```bash
# 1. Leave the site toolchain behind. Either is sufficient; do both if you can.
module purge                                    # drops all of the below too
unset CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH LIBRARY_PATH
unset GCC_ROOT GCC_EXEC_PREFIX COMPILER_PATH
unset MPI_ROOT MPI_HOME I_MPI_ROOT MPICC MPICXX MPIF90 MPIF77

# 2. Activate the validated toolchain.
source <miniconda>/etc/profile.d/conda.sh
conda activate "$R/envs/<env>"                  # built from env.yml / the lock

# 3. Build. build_deps.sh re-checks and re-scrubs the environment itself.
cd "$R/gtfock_psi4"
CMAKE_BUILD_PARALLEL_LEVEL=24 ./build_deps.sh --clean

# 4. The distributed DF tests, on the compute node.
ctest --test-dir _build/gtfock --output-on-failure -R "pdf|df"
```

The batch equivalent, for the record:

```bash
#!/bin/bash
#SBATCH -J gtfock-build -N 1 -n 1 -c 24 --mem=32G -t 01:00:00
#SBATCH -A <account> -q <qos> -p cpu-small
module purge
source <miniconda>/etc/profile.d/conda.sh && conda activate "$R/envs/<env>"
cd "$R/gtfock_psi4" && CMAKE_BUILD_PARALLEL_LEVEL=24 ./build_deps.sh --clean
```

If a site module is genuinely required for something else, load it *after* the
build and before the run only if the run needs it. It must not be loaded while
anything is compiled.

## Verifying an artifact after the fact

The ABI a shared object was compiled with is visible without running it. Against
conda OpenMPI, every object that uses a predefined communicator must reference
Open MPI's globals:

```bash
nm -u libgtfockdf.so.0.1.0 | grep -c ompi_mpi_    # expect > 0; 0 means MPICH ABI
```

Zero undefined `ompi_mpi_*` symbols in a library that calls `MPI_Comm_dup` on
`MPI_COMM_WORLD` means the MPICH ABI was compiled in, and the object must be
rebuilt. This is the cheapest way to audit a directory of previously built
variants; it is how the four benchmark libraries in this incident were found to
be affected while the executables that loaded them were not.
