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
    "$PREFIX/lib/cmake/GTFock/GTFockConfig.cmake" \
    "$PREFIX/share/gtfock/examples/sto-3g.gbs" \
    "$PREFIX/share/gtfock/examples/water.xyz"; do
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
# numerical result, not merely successful MPI startup. The inputs come from the
# installed example data, so the packaged data is what is validated.
OMP_NUM_THREADS=1 MKL_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 \
python tests/test_pscf_regression.py \
    --mpiexec "$PREFIX/bin/mpirun" \
    --mpiexec-preflag=--oversubscribe \
    --pscf "$PREFIX/bin/pscf" \
    --data-dir "$PREFIX/share/gtfock/examples" \
    --timeout 60

package_metadata=$(find "$PREFIX/conda-meta" -maxdepth 1 -name 'gtfock-*.json' -print -quit)
test -n "$package_metadata"
! grep -i cuda "$package_metadata"

# conda-build re-points SRC_DIR and BUILD_PREFIX for this phase, so the exact
# build-time strings are asserted by build.sh, where they are authoritative.
# Assert here the property a leaked build or source directory would violate:
# every runtime search path is $ORIGIN-relative or inside the installed prefix.
for binary in "$PREFIX/bin/pscf" "$PREFIX/lib/libgtfock.so" "$PREFIX/lib/libcint.so"; do
    report=$(basename "$binary")
    ldd "$binary" | tee "$report.ldd"
    ! grep -q "not found" "$report.ldd"
    ! grep -i cuda "$report.ldd"
    readelf -d "$binary" | tee "$report.dynamic"
    search_paths=$(sed -n 's/.*(R[A-Z]*PATH).*\[\(.*\)\]/\1/p' "$report.dynamic" \
        | tr '\n' ':')
    IFS=':' read -r -a search_path_list <<<"$search_paths"
    for search_path in "${search_path_list[@]}"; do
        [[ -n $search_path ]] || continue
        case "$search_path" in
            '$ORIGIN'|'$ORIGIN'/*|"$PREFIX"|"$PREFIX"/*) ;;
            *)
                echo "$binary searches $search_path outside the installed prefix" >&2
                exit 1
                ;;
        esac
    done
done
