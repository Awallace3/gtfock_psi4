# Regression test for the MPI header/library ABI guard.
#
# The failure it protects against is silent: an MPI header from one
# implementation compiles and links against another implementation's library
# without a diagnostic, then segmentation faults inside the first MPI call that
# takes a communicator. docs/hpc-site-mpi.md has the incident this came from.
#
# Two shadowing routes are exercised. -I is unconditional and proves the guard
# itself fires; CPATH is the route that actually bites on an HPC login node, and
# is checked only after confirming that this compiler really does search CPATH
# ahead of the -isystem directories CMake emits for MPI::MPI_C.

if(NOT C_COMPILER OR NOT GUARD_HEADER OR NOT WORK_DIR)
  message(FATAL_ERROR "C_COMPILER, GUARD_HEADER, and WORK_DIR are required")
endif()
if(NOT DEFINED EXPECT_OPEN_MPI)
  message(FATAL_ERROR "EXPECT_OPEN_MPI is required")
endif()

# add_test cannot carry a semicolon-separated list through one -D argument.
string(REPLACE "|" ";" _mpi_include_dirs "${MPI_INCLUDE_DIRS}")
set(_isystem_flags "")
foreach(_dir IN LISTS _mpi_include_dirs)
  list(APPEND _isystem_flags -isystem "${_dir}")
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

set(_guard_source "${WORK_DIR}/guard_tu.c")
file(WRITE "${_guard_source}"
  "#include \"${GUARD_HEADER}\"\n"
  "int gtf_guard_translation_unit;\n")

# Compiles only when the fake mpi.h below is the header the preprocessor opened.
set(_marker_source "${WORK_DIR}/marker_tu.c")
file(WRITE "${_marker_source}"
  "#include <mpi.h>\n"
  "int gtf_marker = GTF_FAKE_MPI_MARKER;\n")

# The decoy claims to be whichever implementation this build is not using.
set(_fake_include "${WORK_DIR}/fake-include")
file(MAKE_DIRECTORY "${_fake_include}")
if(EXPECT_OPEN_MPI)
  set(_fake_body
    "#define MPICH_NAME 1\n"
    "#define MPICH_NUMVERSION 30300000\n"
    "typedef int MPI_Comm;\n"
    "#define MPI_COMM_WORLD ((MPI_Comm)0x44000000)\n")
  set(_decoy "an MPICH-family")
else()
  set(_fake_body
    "#define OPEN_MPI 1\n"
    "struct ompi_communicator_t;\n"
    "typedef struct ompi_communicator_t *MPI_Comm;\n")
  set(_decoy "an Open MPI")
endif()
file(WRITE "${_fake_include}/mpi.h"
  "#ifndef GTF_FAKE_MPI_H\n"
  "#define GTF_FAKE_MPI_H\n"
  "#define GTF_FAKE_MPI_MARKER 1\n"
  ${_fake_body}
  "#endif\n")

# cpath empty means "compile with the include-path environment cleared", which
# is how the build is supposed to run.
function(gtf_syntax_check result_var output_var source cpath)
  set(_compile "${C_COMPILER}" -std=gnu11 -fsyntax-only
      ${ARGN} ${_isystem_flags} "${source}")
  if(cpath)
    set(_command "${CMAKE_COMMAND}" -E env "CPATH=${cpath}" ${_compile})
  else()
    set(_command "${CMAKE_COMMAND}" -E env
        --unset=CPATH --unset=C_INCLUDE_PATH ${_compile})
  endif()
  execute_process(COMMAND ${_command}
    RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  set(${result_var} "${_result}" PARENT_SCOPE)
  set(${output_var} "${_out}${_err}" PARENT_SCOPE)
endfunction()

function(gtf_require_guard_message case output)
  foreach(_needle IN ITEMS "MPI header/library mismatch" "CPATH" "hpc-site-mpi")
    if(NOT "${output}" MATCHES "${_needle}")
      message(FATAL_ERROR
        "the guard rejected the ${case} build but without actionable guidance "
        "('${_needle}'):\n${output}")
    endif()
  endforeach()
endfunction()

gtf_syntax_check(_ok_result _ok_output "${_guard_source}" "")
if(NOT _ok_result EQUAL 0)
  message(FATAL_ERROR
    "the ABI guard rejects the MPI header this build was configured against, "
    "so it would fail every build rather than only mismatched ones "
    "(${_ok_result}):\n${_ok_output}")
endif()

gtf_syntax_check(_inc_result _inc_output "${_guard_source}" ""
  "-I${_fake_include}")
if(_inc_result EQUAL 0)
  message(FATAL_ERROR
    "the ABI guard accepted ${_decoy} <mpi.h> shadowing the configured one "
    "through -I, so an implementation mismatch would again reach run time")
endif()
gtf_require_guard_message("-I shadowed" "${_inc_output}")

gtf_syntax_check(_marker_result _marker_output "${_marker_source}"
  "${_fake_include}")
if(NOT _marker_result EQUAL 0)
  # Nothing to protect against on this compiler: -isystem wins over CPATH here.
  message(STATUS
    "skipping the CPATH case: ${C_COMPILER} does not search CPATH ahead of "
    "-isystem")
  return()
endif()

gtf_syntax_check(_cpath_result _cpath_output "${_guard_source}"
  "${_fake_include}")
if(_cpath_result EQUAL 0)
  message(FATAL_ERROR
    "CPATH replaced the configured <mpi.h> with ${_decoy} header and the ABI "
    "guard still accepted the build, which is exactly the silent mismatch it "
    "exists to catch")
endif()
gtf_require_guard_message("CPATH shadowed" "${_cpath_output}")
