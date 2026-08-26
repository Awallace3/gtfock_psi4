"""Two-rank numerical regression for the real GTFock SCF driver."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import signal
import subprocess


REFERENCE_ENERGY = -74.9450213019  # frozen converged legacy-algorithm result
ENERGY_ATOL = 1.0e-9  # hartree; fixed-rank/thread path normally agrees < 1e-10


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mpiexec", required=True)
    parser.add_argument("--mpiexec-preflag", action="append")
    parser.add_argument("--pscf", required=True)
    parser.add_argument("--root", type=Path, help="source tree holding GTFock/data")
    parser.add_argument("--data-dir", type=Path, help="installed example data directory")
    parser.add_argument("--timeout", type=float, default=30.0)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    if (args.root is None) == (args.data_dir is None):
        parser.error("pass exactly one of --root or --data-dir")
    data_dir = args.data_dir if args.data_dir is not None else args.root / "GTFock/data"
    basis = data_dir / "sto-3g.gbs"
    geometry = data_dir / "water.xyz"
    for path in (basis, geometry):
        if not path.is_file():
            parser.error(f"missing pscf input {path}")

    preflags = args.mpiexec_preflag
    if preflags is None:
        preflags = ["--oversubscribe"]
    command = [
        args.mpiexec,
        *preflags,
        "-n",
        "2",
        args.pscf,
        str(basis),
        str(geometry),
        "2",
        "1",
        "1",
        "2",
        "15",
    ]
    env = os.environ.copy()
    env.update(OMP_NUM_THREADS="1", MKL_NUM_THREADS="1", OPENBLAS_NUM_THREADS="1")
    process = subprocess.Popen(
        command,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    try:
        output, _ = process.communicate(timeout=args.timeout)
    except subprocess.TimeoutExpired:
        # mpirun and every local rank inherit this process group. Terminate the
        # whole group so a failed regression cannot leave MPI ranks orphaned.
        os.killpg(process.pid, signal.SIGTERM)
        try:
            output, _ = process.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            output, _ = process.communicate()
        raise AssertionError(
            f"pscf exceeded {args.timeout:g} second limit\n{output[-6000:]}"
        )

    if process.returncode != 0:
        raise AssertionError(f"pscf exited {process.returncode}\n{output[-6000:]}")
    if "SAD guess unavailable; using core-Hamiltonian guess" not in output:
        raise AssertionError("missing-SAD counterfactual did not exercise the fallback")

    energies = [
        float(value)
        for value in re.findall(
            r"^\s*energy\s+([-+0-9.eE]+)\s+\(", output, re.MULTILINE
        )
    ]
    if not energies:
        raise AssertionError(f"pscf emitted no SCF energies\n{output[-6000:]}")
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
