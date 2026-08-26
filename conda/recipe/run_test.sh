#!/usr/bin/env bash
set -euo pipefail
set -x

for path in \
    "$PREFIX/bin/pscf" \
    "$PREFIX/lib/libgtfock.so" \
    "$PREFIX/lib/libcint.so" \
    "$PREFIX/lib/libGTMatrix.a" \
    "$PREFIX/include/pfock.h" \
    "$PREFIX/include/CInt.h" \
    "$PREFIX/lib/cmake/GTFock/GTFockConfig.cmake"; do
    test -e "$path"
done

# Discover, compile, link, and run as an external CMake consumer. The test
# compiler is intentionally conda-forge's normal C compiler, demonstrating the
# C ABI expected by optional Psi4 consumption.
cmake -S tests/package-consumer -B consumer-build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$PREFIX" \
    -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF
cmake --build consumer-build --parallel 2
ctest --test-dir consumer-build --output-on-failure

# Exercise the installed pscf executable on two ranks and verify the converged
# numerical result, not merely successful MPI startup.
OMP_NUM_THREADS=1 MKL_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 \
python tests/test_pscf_regression.py \
    --mpiexec "$PREFIX/bin/mpirun" \
    --mpiexec-preflag=--oversubscribe \
    --pscf "$PREFIX/bin/pscf" \
    --root "$PWD" \
    --timeout 60

package_metadata=$(find "$PREFIX/conda-meta" -maxdepth 1 -name 'gtfock-*.json' -print -quit)
test -n "$package_metadata"
! grep -i cuda "$package_metadata"

for binary in "$PREFIX/bin/pscf" "$PREFIX/lib/libgtfock.so" "$PREFIX/lib/libcint.so"; do
    ldd "$binary" | tee "$(basename "$binary").ldd"
    ! grep -q "not found" "$(basename "$binary").ldd"
    ! grep -i cuda "$(basename "$binary").ldd"
    readelf -d "$binary" | tee "$(basename "$binary").dynamic"
    ! grep -F -e "${SRC_DIR:-__unset_src_dir__}" \
              -e "${BUILD_PREFIX:-__unset_build_prefix__}" \
              "$(basename "$binary").dynamic"
done

! grep -R -F -e "${SRC_DIR:-__unset_src_dir__}" \
              -e "${BUILD_PREFIX:-__unset_build_prefix__}" \
              "$PREFIX/lib/cmake/GTFock"
