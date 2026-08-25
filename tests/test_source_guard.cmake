if(NOT SOURCE_ROOT OR NOT WORK_DIR)
  message(FATAL_ERROR "SOURCE_ROOT and WORK_DIR are required")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")

# Reproduces the Fock update of the pinned, unpatched GTFock submodule,
# including the commented-out mention of the classic bit-cast intrinsic.
file(WRITE "${WORK_DIR}/unpatched/pfock/update_F.h"
"#include <immintrin.h>
#pragma once

static inline void atomic_add_f64(volatile double* global_value, double addend)
{
    uint64_t expected_value, new_value;
    do {
        double old_value = *global_value;
        // expected_value = _castf64_u64(old_value);
        // new_value = _castf64_u64(old_value + addend);
        expected_value = (uint64_t)old_value;
        new_value = (uint64_t)(old_value + addend);
    } while (!__sync_bool_compare_and_swap((volatile uint64_t*)global_value, expected_value, new_value));
}
")

function(gtf_configure_expecting_failure case source_dir expected_guidance)
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
      -S "${SOURCE_ROOT}"
      -B "${WORK_DIR}/build-${case}"
      "-DCMAKE_C_COMPILER=${C_COMPILER}"
      "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}"
      "-DCMAKE_Fortran_COMPILER=${Fortran_COMPILER}"
      "-DGTF_GTFock_SOURCE_DIR=${source_dir}"
      -DBUILD_TESTING=OFF
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
  if(result EQUAL 0)
    message(FATAL_ERROR
      "configure accepted GTFock sources without a bitwise atomic add (${case})")
  endif()
  foreach(_needle IN LISTS expected_guidance)
    if(NOT "${output}${error}" MATCHES "${_needle}")
      message(FATAL_ERROR
        "configure failure for ${case} lacks actionable guidance "
        "('${_needle}'):\n${output}${error}")
    endif()
  endforeach()
endfunction()

set(_patched_guidance "build_deps.sh" "numerical-root-cause")
gtf_configure_expecting_failure(unpatched "${WORK_DIR}/unpatched"
  "${_patched_guidance}")

set(_missing_guidance "build_deps.sh" "submodule update --init")
gtf_configure_expecting_failure(missing "${WORK_DIR}/absent"
  "${_missing_guidance}")
