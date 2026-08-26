cmake_minimum_required(VERSION 3.24)
project(gtfock_psi4 VERSION 0.1.0 LANGUAGES C CXX Fortran)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(GTF_GTFock_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/GTFock" CACHE PATH
    "GTFock source tree (the build script uses a patched staging copy)")
option(GTF_BUILD_PSCF "Build the native MPI SCF executable" ON)
option(GTF_COMBINED_JK "Accumulate Coulomb and exchange into one SCF matrix" ON)
set(GTF_MPIEXEC_PREFLAGS "--oversubscribe" CACHE STRING
    "mpiexec flags shared by every MPI test launch")

# Pinned submodules stay unpatched, so the default source directory is only a
# starting point: compiling it as-is reproduces the value-converting
# compare-and-swap in pfock/update_F.h that docs/numerical-root-cause.md
# identifies as the initiating numerical bug. Refuse any tree whose Fock update
# is not a bitwise atomic add, so every entry point - not just build_deps.sh -
# holds the invariant that the compiled tree is a corrected tree.
set(_gtf_source_help
  "Run ./build_deps.sh: it stages patches/gtfock-modern.patch into \
_build/gtfock-src and configures this project with -DGTF_GTFock_SOURCE_DIR set \
to that corrected copy. To build a different tree, pass \
-DGTF_GTFock_SOURCE_DIR=<path> explicitly.")
set(_gtf_update_f "${GTF_GTFock_SOURCE_DIR}/pfock/update_F.h")
if(NOT EXISTS "${_gtf_update_f}")
  message(FATAL_ERROR
    "No GTFock sources in ${GTF_GTFock_SOURCE_DIR}: pfock/update_F.h is missing.\n"
    "Initialize the pinned sources with "
    "'git submodule update --init --recursive'.\n"
    "${_gtf_source_help}")
endif()
file(READ "${_gtf_update_f}" _gtf_update_f_text)
# The pinned tree still names the classic intrinsic in comments, so match code.
string(REGEX REPLACE "/\\*[^*]*\\*+([^/*][^*]*\\*+)*/" " "
  _gtf_update_f_code "${_gtf_update_f_text}")
string(REGEX REPLACE "//[^\n]*" " " _gtf_update_f_code "${_gtf_update_f_code}")
set(_gtf_bitwise_atomic OFF)
if(_gtf_update_f_code MATCHES "#[ \t]*include[ \t]*\"gtfock_atomic\\.h\"")
  set(_gtf_bitwise_atomic ON)
elseif(_gtf_update_f_code MATCHES "_castf64_u64[ \t]*\\(")
  set(_gtf_bitwise_atomic ON)
endif()
if(_gtf_update_f_code MATCHES "\\([ \t]*uint64_t[ \t]*\\)[ \t]*\\(?[ \t]*old_value")
  set(_gtf_bitwise_atomic OFF)
endif()
if(NOT _gtf_bitwise_atomic)
  message(FATAL_ERROR
    "${_gtf_update_f} converts the Fock accumulator to an integer instead of "
    "reinterpreting its bits, so building this tree reproduces the numerical "
    "failure analyzed in docs/numerical-root-cause.md.\n"
    "${_gtf_source_help}")
endif()

find_package(MPI REQUIRED COMPONENTS C)
find_package(OpenMP REQUIRED COMPONENTS C)
find_package(simint 0.8 CONFIG REQUIRED)

# Search only conda's active build/host prefixes for numerical libraries. A
# normal developer environment uses CONDA_PREFIX; conda-build intentionally
# splits compilers into BUILD_PREFIX and link dependencies into PREFIX.
set(_gtf_prefix_hints ${CMAKE_PREFIX_PATH})
set(_gtf_allowed_prefixes "")
foreach(_prefix_var IN ITEMS CONDA_PREFIX PREFIX BUILD_PREFIX)
  if(DEFINED ENV{${_prefix_var}} AND NOT "$ENV{${_prefix_var}}" STREQUAL "")
    list(PREPEND _gtf_prefix_hints "$ENV{${_prefix_var}}")
    list(APPEND _gtf_allowed_prefixes "$ENV{${_prefix_var}}")
  endif()
endforeach()
list(REMOVE_DUPLICATES _gtf_prefix_hints)
list(REMOVE_DUPLICATES _gtf_allowed_prefixes)
find_path(MKL_INCLUDE_DIR mkl.h HINTS ${_gtf_prefix_hints} PATH_SUFFIXES include REQUIRED)
find_library(BLAS_LIBRARY NAMES blas HINTS ${_gtf_prefix_hints} PATH_SUFFIXES lib REQUIRED)
find_library(LAPACK_LIBRARY NAMES lapack HINTS ${_gtf_prefix_hints} PATH_SUFFIXES lib REQUIRED)
find_library(MKL_SCALAPACK_LIBRARY NAMES mkl_scalapack_lp64
  HINTS ${_gtf_prefix_hints} PATH_SUFFIXES lib REQUIRED)
find_library(MKL_BLACS_LIBRARY NAMES mkl_blacs_openmpi_lp64
  HINTS ${_gtf_prefix_hints} PATH_SUFFIXES lib REQUIRED)

if(NOT _gtf_allowed_prefixes)
  message(FATAL_ERROR
    "Activate the supported conda environment (or build through conda-build)")
endif()
function(gtf_require_conda_path label path)
  set(_in_conda OFF)
  foreach(_prefix IN LISTS _gtf_allowed_prefixes)
    cmake_path(IS_PREFIX _prefix "${path}" NORMALIZE _in_prefix)
    if(_in_prefix)
      set(_in_conda ON)
      break()
    endif()
  endforeach()
  if(NOT _in_conda)
    message(FATAL_ERROR
      "${label} resolved outside the supported conda prefixes: ${path}")
  endif()
endfunction()
foreach(_path IN ITEMS
    CMAKE_C_COMPILER CMAKE_CXX_COMPILER CMAKE_Fortran_COMPILER
    MKL_INCLUDE_DIR BLAS_LIBRARY LAPACK_LIBRARY
    MKL_SCALAPACK_LIBRARY MKL_BLACS_LIBRARY)
  gtf_require_conda_path("${_path}" "${${_path}}")
endforeach()
foreach(_mpi_library IN LISTS MPI_C_LIBRARIES)
  gtf_require_conda_path("MPI" "${_mpi_library}")
endforeach()
if(NOT CMAKE_C_COMPILER_ID STREQUAL "IntelLLVM" OR
   NOT CMAKE_CXX_COMPILER_ID STREQUAL "IntelLLVM")
  message(FATAL_ERROR
    "The validated C/C++ toolchain is IntelLLVM icx/icpx, but CMake found "
    "${CMAKE_C_COMPILER_ID}/${CMAKE_CXX_COMPILER_ID}")
endif()
if(NOT CMAKE_Fortran_COMPILER_ID STREQUAL "GNU")
  message(FATAL_ERROR
    "The validated ERD/OED Fortran toolchain is GNU Fortran, but CMake found "
    "${CMAKE_Fortran_COMPILER_ID}")
endif()

add_library(gtf_numeric INTERFACE)
target_include_directories(gtf_numeric INTERFACE "${MKL_INCLUDE_DIR}")
target_link_libraries(gtf_numeric INTERFACE
  "${MKL_SCALAPACK_LIBRARY}" "${MKL_BLACS_LIBRARY}"
  "${LAPACK_LIBRARY}" "${BLAS_LIBRARY}")

# GTF_COMBINED_JK must mean the same thing in the library and in pscf: both
# compile GTFock sources that branch on __SCF__.
set(_gtf_jk_definitions "")
if(GTF_COMBINED_JK)
  list(APPEND _gtf_jk_definitions __SCF__)
endif()

function(gtf_target_defaults target)
  target_compile_options(${target} PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-O3;-Wall;-Wextra;-Wno-unused-parameter;-Wno-unused-variable>
    $<$<COMPILE_LANGUAGE:Fortran>:-O3>)
endfunction()

# OptERD/OED retain Fortran kernels needed by libcint. Building them as CMake
# targets lets CMake carry the GNU Fortran runtime into IntelLLVM-linked targets.
file(GLOB ERD_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/OptErd_Makefile/external/erd/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/OptErd_Makefile/external/erd/*.f"
  "${CMAKE_CURRENT_SOURCE_DIR}/OptErd_Makefile/external/erd/*.F")
add_library(erd STATIC ${ERD_SOURCES})
target_include_directories(erd PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/OptErd_Makefile/external/erd>")
target_compile_definitions(erd PRIVATE __ALIGNLEN__=64)
target_link_libraries(erd PRIVATE OpenMP::OpenMP_C)
gtf_target_defaults(erd)

file(GLOB OED_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/OptErd_Makefile/external/oed/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/OptErd_Makefile/external/oed/*.f"
  "${CMAKE_CURRENT_SOURCE_DIR}/OptErd_Makefile/external/oed/*.F")
add_library(oed STATIC ${OED_SOURCES})
target_include_directories(oed PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/OptErd_Makefile/external/oed>")
target_link_libraries(oed PRIVATE OpenMP::OpenMP_C)
gtf_target_defaults(oed)

add_library(cint SHARED
  libcint/cint_basisset.c
  libcint/erd_integral.c
  libcint/oed_integral.c
  libcint/cint_simint.c)
target_include_directories(cint PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libcint>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_compile_definitions(cint PRIVATE __ALIGNLEN__=64)
target_link_libraries(cint PRIVATE simint::simint erd oed OpenMP::OpenMP_C m)
gtf_target_defaults(cint)
set_target_properties(cint PROPERTIES
  EXPORT_NAME CInt
  VERSION ${PROJECT_VERSION}
  SOVERSION 0
  INSTALL_RPATH "$ORIGIN")
add_library(GTFock::CInt ALIAS cint)

add_library(GTMatrix STATIC
  GTMatrix/GTMatrix_Typedef.c
  GTMatrix/GTMatrix_Get.c
  GTMatrix/GTMatrix_Update.c
  GTMatrix/GTMatrix_Other.c
  GTMatrix/GTM_Req_Vector.c
  GTMatrix/GTM_Task_Queue.c
  GTMatrix/utils.c)
target_include_directories(GTMatrix PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/GTMatrix>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(GTMatrix PUBLIC MPI::MPI_C)
gtf_target_defaults(GTMatrix)
set_target_properties(GTMatrix PROPERTIES EXPORT_NAME GTMatrix)
add_library(GTFock::GTMatrix ALIAS GTMatrix)

set(_gtfock_sources
  "${GTF_GTFock_SOURCE_DIR}/pfock/fock_buf.c"
  "${GTF_GTFock_SOURCE_DIR}/pfock/fock_task.c"
  "${GTF_GTFock_SOURCE_DIR}/pfock/one_electron.c"
  "${GTF_GTFock_SOURCE_DIR}/pfock/pfock.c"
  "${GTF_GTFock_SOURCE_DIR}/pfock/screening.c"
  "${GTF_GTFock_SOURCE_DIR}/pfock/taskq.c")
if(EXISTS "${GTF_GTFock_SOURCE_DIR}/aligned_malloc/aligned_malloc.c")
  list(APPEND _gtfock_sources
    "${GTF_GTFock_SOURCE_DIR}/aligned_malloc/aligned_malloc.c")
  set(_gtfock_needs_mm_malloc_header OFF)
else()
  set(_gtfock_needs_mm_malloc_header ON)
endif()
add_library(gtfock SHARED ${_gtfock_sources})
if(_gtfock_needs_mm_malloc_header)
  # Enables a read-only numerical comparison build of the proven Intel-classic
  # source tree without changing that checkout.
  target_compile_options(gtfock PRIVATE
    -include "${CMAKE_CURRENT_SOURCE_DIR}/src/legacy_icpc_compat.h")
endif()
target_include_directories(gtfock PUBLIC
  "$<BUILD_INTERFACE:${GTF_GTFock_SOURCE_DIR}/aligned_malloc>"
  "$<BUILD_INTERFACE:${GTF_GTFock_SOURCE_DIR}/pfock>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libcint>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/GTMatrix>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_include_directories(gtfock PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(gtfock PRIVATE __ALIGNLEN__=64 ${_gtf_jk_definitions})
# The public header exposes MPI, OpenMP, CInt, and GTMatrix types. Preserve
# those usage requirements in the installed target rather than relying on a
# consumer to reconstruct this source-tree link line.
target_link_libraries(gtfock PUBLIC
  MPI::MPI_C OpenMP::OpenMP_C cint GTMatrix)
target_link_libraries(gtfock PRIVATE simint::simint gtf_numeric m)
gtf_target_defaults(gtfock)
set_target_properties(gtfock PROPERTIES
  EXPORT_NAME GTFock
  VERSION ${PROJECT_VERSION}
  SOVERSION 0
  INSTALL_RPATH "$ORIGIN")
add_library(GTFock::GTFock ALIAS gtfock)

if(GTF_BUILD_PSCF)
  add_executable(pscf
    "${GTF_GTFock_SOURCE_DIR}/pscf/scf.c"
    "${GTF_GTFock_SOURCE_DIR}/pscf/purif.c"
    "${GTF_GTFock_SOURCE_DIR}/pscf/pdgemm.c")
  target_include_directories(pscf PRIVATE
    "${GTF_GTFock_SOURCE_DIR}/pscf")
  target_compile_definitions(pscf PRIVATE ${_gtf_jk_definitions})
  target_link_libraries(pscf PRIVATE
    gtfock cint GTMatrix simint::simint MPI::MPI_C OpenMP::OpenMP_C
    gtf_numeric m)
  gtf_target_defaults(pscf)
  set_target_properties(pscf PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()

if(BUILD_TESTING)
  find_package(Python 3.12 REQUIRED COMPONENTS Interpreter)

  add_executable(test_atomic_add tests/test_atomic_add.c)
  target_include_directories(test_atomic_add PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(test_atomic_add PRIVATE OpenMP::OpenMP_C)
  add_test(NAME atomic_add COMMAND test_atomic_add)

  add_executable(test_overlap tests/test_overlap.c)
  target_include_directories(test_overlap PRIVATE
    "${GTF_GTFock_SOURCE_DIR}/pfock"
    "${CMAKE_CURRENT_SOURCE_DIR}/libcint"
    "${CMAKE_CURRENT_SOURCE_DIR}/GTMatrix")
  target_link_libraries(test_overlap PRIVATE gtfock cint GTMatrix MPI::MPI_C)
  add_test(NAME legacy_mpi_overlap
    COMMAND "${CMAKE_COMMAND}" -E env
      "OMP_NUM_THREADS=1"
      "MKL_NUM_THREADS=1"
      "${MPIEXEC_EXECUTABLE}" ${GTF_MPIEXEC_PREFLAGS}
      "${MPIEXEC_NUMPROC_FLAG}" 2
      "$<TARGET_FILE:test_overlap>"
      "${CMAKE_CURRENT_SOURCE_DIR}/GTFock/data/sto-3g.gbs"
      "${CMAKE_CURRENT_SOURCE_DIR}/GTFock/data/water.xyz")
  set_tests_properties(legacy_mpi_overlap PROPERTIES TIMEOUT 120 PROCESSORS 2)

  add_test(NAME unpatched_source_rejected
    COMMAND "${CMAKE_COMMAND}"
      "-DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DWORK_DIR=${CMAKE_BINARY_DIR}/source-guard"
      "-DGENERATOR=${CMAKE_GENERATOR}"
      "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
      "-DC_COMPILER=${CMAKE_C_COMPILER}"
      "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
      "-DFortran_COMPILER=${CMAKE_Fortran_COMPILER}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_source_guard.cmake")
  set_tests_properties(unpatched_source_rejected PROPERTIES TIMEOUT 300)

  add_test(NAME installed_header_smoke
    COMMAND "${CMAKE_COMMAND}"
      "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
      "-DSTAGE_DIR=${CMAKE_BINARY_DIR}/install-smoke"
      "-DMPI_C_COMPILER=${MPI_C_COMPILER}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_installed_headers.cmake")

  add_test(NAME relocated_cmake_consumer
    COMMAND "${CMAKE_COMMAND}"
      "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
      "-DSTAGE_DIR=${CMAKE_BINARY_DIR}/consumer-stage"
      "-DRELOCATED_DIR=${CMAKE_CURRENT_SOURCE_DIR}/_relocated-test"
      "-DCONSUMER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/package-consumer"
      "-DGENERATOR=${CMAKE_GENERATOR}"
      "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_relocated_package.cmake")
  set_tests_properties(relocated_cmake_consumer PROPERTIES TIMEOUT 300)

  if(GTF_BUILD_PSCF)
    set(_gtf_preflag_args "")
    foreach(_flag IN LISTS GTF_MPIEXEC_PREFLAGS)
      list(APPEND _gtf_preflag_args "--mpiexec-preflag=${_flag}")
    endforeach()
    add_test(NAME pscf_numerical_regression
      COMMAND "${Python_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pscf_regression.py"
        --mpiexec "${MPIEXEC_EXECUTABLE}"
        ${_gtf_preflag_args}
        --pscf "$<TARGET_FILE:pscf>"
        --root "${CMAKE_CURRENT_SOURCE_DIR}"
        --timeout 30)
    set_tests_properties(pscf_numerical_regression PROPERTIES
      TIMEOUT 150 PROCESSORS 2)
  endif()
endif()

set(GTF_CMAKE_INSTALL_DIR
  "${CMAKE_INSTALL_LIBDIR}/cmake/GTFock" CACHE STRING
  "Install location for GTFock CMake package metadata")

configure_package_config_file(
  cmake/GTFockConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/GTFockConfig.cmake"
  INSTALL_DESTINATION "${GTF_CMAKE_INSTALL_DIR}")
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/GTFockConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion)

install(TARGETS cint GTMatrix gtfock
  EXPORT GTFockTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(GTF_BUILD_PSCF)
  install(TARGETS pscf RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(FILES
  libcint/CInt.h
  "${GTF_GTFock_SOURCE_DIR}/pfock/pfock.h"
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY GTMatrix/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.h")
install(FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/GTFock/data/sto-3g.gbs"
  "${CMAKE_CURRENT_SOURCE_DIR}/GTFock/data/water.xyz"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/gtfock/examples")
install(EXPORT GTFockTargets
  FILE GTFockTargets.cmake
  NAMESPACE GTFock::
  DESTINATION "${GTF_CMAKE_INSTALL_DIR}")
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/GTFockConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/GTFockConfigVersion.cmake"
  DESTINATION "${GTF_CMAKE_INSTALL_DIR}")
