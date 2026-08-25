if(NOT SOURCE_ROOT OR NOT WORK_DIR OR NOT GENERATOR OR NOT MAKE_PROGRAM)
  message(FATAL_ERROR
    "SOURCE_ROOT, WORK_DIR, GENERATOR, and MAKE_PROGRAM are required")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")

function(gtf_configure_expecting_failure case expected_guidance)
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
      -S "${SOURCE_ROOT}"
      -B "${WORK_DIR}/build-${case}"
      -G "${GENERATOR}"
      "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}"
      "-DCMAKE_C_COMPILER=${C_COMPILER}"
      "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}"
      "-DCMAKE_Fortran_COMPILER=${Fortran_COMPILER}"
      -DBUILD_TESTING=OFF
      ${ARGN}
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

# No -DGTF_GTFock_SOURCE_DIR: the cache default resolves to the pinned GTFock
# submodule, which is either absent or still carries the uncorrected Fock
# update. Both refusals name build_deps.sh as the way forward.
gtf_configure_expecting_failure(default "build_deps.sh")

gtf_configure_expecting_failure(missing
  "build_deps.sh;submodule update --init"
  "-DGTF_GTFock_SOURCE_DIR=${WORK_DIR}/absent")
