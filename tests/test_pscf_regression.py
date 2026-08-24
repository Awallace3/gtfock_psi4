"""Two-rank numerical regression for the real GTFock SCF driver."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess


REFERENCE_ENERGY = -74.9450213019  # frozen converged legacy-algorithm result
ENERGY_ATOL = 1.0e-9  # hartree; fixed-rank/thread path normally agrees < 1e-10


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mpiexec", required=True)
    parser.add_argument("--mpiexec-preflag", action="append")
    parser.add_argument("--pscf", required=True)
    parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()

    preflags = args.mpiexec_preflag
    if preflags is None:
        preflags = ["--oversubscribe"]
    command = [
        args.mpiexec,
        *preflags,
        "-n",
        "2",
        args.pscf,
        str(args.root / "GTFock/data/sto-3g.gbs"),
        str(args.root / "GTFock/data/water.xyz"),
        "2",
        "1",
        "1",
        "2",
        "15",
    ]
    env = os.environ.copy()
    env.update(OMP_NUM_THREADS="1", MKL_NUM_THREADS="1", OPENBLAS_NUM_THREADS="1")
    completed = subprocess.run(
        command,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=120,
        check=False,
    )
    if completed.returncode != 0:
        raise AssertionError(
            f"pscf exited {completed.returncode}\n{completed.stdout[-6000:]}"
        )
    if "SAD guess unavailable; using core-Hamiltonian guess" not in completed.stdout:
        raise AssertionError("missing-SAD counterfactual did not exercise the fallback")

    energies = [
        float(value)
        for value in re.findall(
            r"^\s*energy\s+([-+0-9.eE]+)\s+\(", completed.stdout, re.MULTILINE
        )
    ]
    if not energies:
        raise AssertionError(f"pscf emitted no SCF energies\n{completed.stdout[-6000:]}")
    error = abs(energies[-1] - REFERENCE_ENERGY)
    if error > ENERGY_ATOL:
        raise AssertionError(
            f"final energy {energies[-1]:.12f} differs from reference by "
            f"{error:.3e} Eh (tolerance {ENERGY_ATOL:.1e})"
        )
    if len(energies) > 10:
        raise AssertionError(f"SCF needed {len(energies)} iterations")

    print(
        f"pscf energy={energies[-1]:.12f} error={error:.3e} Eh "
        f"iterations={len(energies)}"
    )


if __name__ == "__main__":
    main()
