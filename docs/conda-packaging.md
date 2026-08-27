# Conda package preparation

This repository can build a local, conda-forge-compatible Linux artifact for
the complete validated native stack. It does **not** publish the artifact and
is not yet eligible for a public staged-recipes submission; the keyed decisions
below deliberately preserve the validated behavior instead of substituting an
untested compiler, MPI, or numerical library.

## Build and validate locally

Initialize the pinned submodules, then run:

```bash
git submodule update --init --recursive
./conda/build-local.sh
```

The script keeps all tools, caches, source snapshots, artifacts, and fresh test
environments below `.conda-envs/`, `.conda-pkgs/`, and `.conda-source/`. It:

1. snapshots the working tree and each immutable gitlink;
2. builds generated AVX2 Simint and GTFock with IntelLLVM 2025.3.1, GNU
   Fortran 14.3.0, OpenMPI 5.0.8, and MKL 2025.3.1;
3. runs the Simint and native CTest suites, including two-rank numerical tests;
4. builds a `.conda` artifact without uploading it;
5. tests CMake discovery, external compilation/linking, installed MPI SCF,
   dynamic-library resolution, and build-path absence in conda-build's fresh
   test prefix; and
6. installs the artifact into a second fresh CPU-only prefix and repeats the
   two-rank water calculation against `-74.9450213019 Eh`.

The final artifact is under `.conda-pkgs/output/linux-64/`. Re-run only the
fresh-install check with:

```bash
./conda/validate-installed.sh
```

Fresh validation requires exactly one local artifact, selects its exact
version/build from the strict-priority local channel, and verifies that exact
package record after installation.

### Native continuous integration

Pull requests and pushes to `main` run the
[`GTFock CI`](../.github/workflows/gtfock-ci.yml) GitHub Actions workflow. It
checks out the pinned submodules, creates the audited `env.yml` toolchain, and
runs the canonical `./build_deps.sh --clean` build. That command executes the
existing native CTest suite, including atomic operations, two-rank MPI overlap,
source-patch enforcement, installed-header and relocated-CMake consumers, the
two-rank `pscf` numerical regression, generated-Simint cleanup, and CUDA
classifier behavior.

The CI build validates GTFock itself. The heavier conda artifact construction,
fresh-prefix installation, relocatability, dependency, and dynamic-linkage
gate remains `./conda/build-local.sh`.

### CPU-only OpenMPI metadata workaround

The audited conda-forge OpenMPI 5.0.8 build has a `__cuda >=12` *virtual-package
constraint* even though CUDA is optional and neither OpenMPI nor GTFock depends
on a CUDA runtime package. On a CPU host with no sufficiently new NVIDIA
driver, the solver therefore rejects that exact OpenMPI build before it can
observe that CUDA support is unused.

The CI workflow, `build-local.sh`, and `validate-installed.sh` set
`CONDA_OVERRIDE_CUDA=12.0` for conda's virtual-package detection while solving.
The override does not enable CUDA at build or runtime. Validation asserts all
of the following, and the second-prefix numerical run explicitly removes the
override:

- no package whose name contains `cuda` or `nvidia` (`cuda-version`, `cuda-*`,
  `cudatoolkit`, `libcuda*`, `nvidia-*`, `libnvidia-*`, ...) and no CUDA
  component library whose name contains neither (`cudnn`, `cudss`,
  `custatevec`, `cutensor`, `cupti`, `nccl`, `npp`, `nvjitlink`, `nvrtc`,
  `nvtx`, `nvcomp`, `cublas`, `cufft`, `cufile`, `curand`, `cusolver`,
  `cusparse`, each optionally `lib`-prefixed or `-dev`-suffixed) is installed;
- GTFock package metadata contains no CUDA requirement;
- `pscf`, `libgtfock`, and `libcint` have no CUDA dynamic linkage; and
- the installed two-rank numerical execution succeeds on the CPU.

For a reusable local install, let the validator select the exact local
version/build, then activate the prefix it created:

```bash
./conda/validate-installed.sh
conda activate "$PWD/.conda-envs/package-test"
```

The override is scoped to the validator's create transaction and is not
exported into the activated shell.

## Installed interface and optional Psi4 consumption

The package installs:

- `lib/libgtfock.so.0`, `lib/libcint.so.0`, and `lib/libGTMatrix.a`;
- the complete public `pfock.h`, `CInt.h`, and GTMatrix header set;
- `lib/cmake/GTFock/GTFockConfig.cmake` and relocatable imported targets
  `GTFock::GTFock`, `GTFock::CInt`, and `GTFock::GTMatrix`;
- `bin/pscf`; and
- the small water/STO-3G example used for installed-artifact validation.

An optional Psi4 component or any external CMake consumer should use package
discovery rather than reconstructing source-tree paths:

```cmake
find_package(GTFock 0.1 CONFIG REQUIRED)
target_link_libraries(my_optional_component PRIVATE GTFock::GTFock)
```

`GTFock::GTFock` carries its public MPI, OpenMP, CInt, GTMatrix, include, and
link requirements. The package config calls `find_dependency(MPI COMPONENTS C)`
and `find_dependency(OpenMP COMPONENTS C)`. Installed RPATHs are relative to
the artifact, and the regression test physically relocates a staged install
before configuring and running an external consumer.

## Source and patch provenance

[`conda/recipe/source-provenance.yaml`](../conda/recipe/source-provenance.yaml)
records every gitlink commit, public archive URL, SHA256, destination folder,
and `patches/gtfock-modern.patch`. Local packaging applies that patch to a
disposable GTFock copy exactly as the normal `build_deps.sh` path does; pinned
submodules remain immutable.

The local recipe uses a generated clean `source.path` snapshot because this
repository task may not create a release. A staged-recipes recipe must replace
that block with the checksummed archive list in the provenance file.

## Keyed submission decisions

Public packaging remains gated by the following explicit decisions.

### CF-LICENSE-001 — GTFock redistribution license (unresolved, blocking)

The pinned GTFock fork has no license file or source notices. The recipe bundles
all license material that actually exists (GPL-2.0-or-later OED, LGPL-2.1-or-
later GTMatrix/libcint, and BSD-3-Clause Simint) and adds a `LicenseRef` notice;
that notice is not a license grant.

Options:

1. **Preferred:** obtain an explicit redistributable license from the GTFock
   copyright holders and add its exact file to the source archive and recipe.
2. Keep the artifact on a private/internal channel where distribution rights
   have been established separately.
3. Replace GTFock with a separately licensed implementation, which requires a
   complete repeat of the numerical validation and is not this package.

Do not submit or publish while this key is unresolved.

### CF-SOURCE-002 — public superproject release (unresolved, blocking)

Conda-forge expects stable tarballs and SHA256 hashes, not credentialed recursive
git clones. Component archives are already checksummed, but task scope forbids
creating a release.

Options:

1. **Preferred:** after CF-LICENSE-001, publish a `v0.1.0` source archive, fill
   its SHA256 in `source-provenance.yaml`, and use the documented multi-source
   layout.
2. Create one complete, immutable release archive containing the exact gitlink
   sources and patch, with every bundled license.

### CF-COMPILER-003 — IntelLLVM conda-forge exception (unresolved, blocking)

`dpcpp_linux-64` is a conda-forge package that supplies `icx`/`icpx`, but
IntelLLVM is not a normal `compiler('c')`/`compiler('cxx')` variant in
conda-forge's global compiler policy. The recipe therefore names and verifies
the exact validated compiler instead of silently building with GCC.

Options:

1. **Preferred for current evidence:** request staged-recipes/core approval for
   the Linux-only IntelLLVM build and retain the wrapper-dispatch assertions.
2. Validate GCC/Clang and generic ScaLAPACK as a separate numerical project;
   only then may a standard compiler recipe replace this one.
3. Use an internal feedstock if conda-forge declines the exception.

### CF-MPI-CUDA-004 — OpenMPI virtual CUDA constraint (decided)

Use `CONDA_OVERRIDE_CUDA=12.0` only for dependency solving, never as a runtime
feature. Require automated proof that no CUDA package, package requirement, or
dynamic link enters GTFock. This is a workaround for upstream OpenMPI metadata,
not a GTFock CUDA dependency.

### CF-GPL-005 — OED/ERD and downstream license boundary (unresolved)

The complete validated libcint build statically incorporates OED/ERD sources;
OED files state GPL-2.0-or-later. A public artifact must therefore preserve the
GPL license, and optional Psi4 distribution needs license review.

Options:

1. Distribute the complete artifact under the applicable GPL terms and keep
   Psi4 consumption optional/dynamic with project legal review.
2. Remove the unused legacy OED/ERD backend only after proving the Simint-only
   build preserves every native and Psi4 numerical path, then revise licensing.
3. Split the legacy backend into a separate package after an ABI/design review.

### CF-SIMINT-006 — generated bundled Simint (unresolved for feedstock review)

The build generates Simint from pinned source and statically incorporates it
into `libcint`; the standalone static archive and development metadata are
removed from the final package. Options are to justify this licensed build-time
vendoring, or package generated Simint separately and depend on it. Either path
must preserve the exact generator inputs and AVX2 validation.

## Later staged-recipes/feedstock checklist

After all blocking keys are resolved:

1. publish the immutable superproject source and verify every SHA256;
2. replace local `source.path` with `source-provenance.yaml`'s archive list;
3. copy the recipe and license files into `conda-forge/staged-recipes` and
   convert to the then-current v1 `recipe.yaml` format if required;
4. request the IntelLLVM policy exception explicitly—do not replace the
   compiler to make lint pass;
5. let conda-forge global pins/run exports constrain runtime ABIs where they can
   reproduce the audited solve, documenting any exact compatibility pin that
   remains necessary;
6. retain installed CMake-consumer, CPU-only/no-CUDA, RPATH, and two-rank
   numerical tests; and
7. submit through the separate approved publication task. This repository task
   does not open that external PR.

Policy references: [adding packages](https://conda-forge.org/docs/maintainer/adding_pkgs/),
[compiler infrastructure](https://conda-forge.org/docs/maintainer/infrastructure/#compilers-and-runtimes),
[MPI guidance](https://conda-forge.org/docs/maintainer/knowledge_base/#message-passing-interface-mpi),
[recipe review](https://conda-forge.org/docs/maintainer/guidelines/#reviewing-recipes),
and [binary relocation](https://docs.conda.io/projects/conda-build/en/stable/resources/define-metadata.html#binary-relocation).
