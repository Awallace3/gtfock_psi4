#!/usr/bin/env python3
import json
import sys

CUDA_COMPONENTS = (
    "cudnn",
    "cudss",
    "custatevec",
    "cutensor",
    "cupti",
    "nccl",
    "npp",
    "nvjitlink",
    "nvrtc",
    "nvtx",
    "nvcomp",
    "cublas",
    "cufft",
    "cufile",
    "curand",
    "cusolver",
    "cusparse",
)


def is_cuda_package(name):
    name = name.lower()
    if "cuda" in name or "nvidia" in name:
        return True
    stem = name[3:] if name.startswith("lib") else name
    return any(
        stem == component or stem.startswith(component + "-")
        for component in CUDA_COMPONENTS
    )


def main():
    names = {record["name"] for record in json.load(sys.stdin)}
    forbidden = sorted(name for name in names if is_cuda_package(name))
    if forbidden:
        raise SystemExit(
            "CUDA packages entered CPU-only prefix: " + ", ".join(forbidden)
        )
    print(f"fresh prefix contains {len(names)} packages and no CUDA package")


if __name__ == "__main__":
    main()
