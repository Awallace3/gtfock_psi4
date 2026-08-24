cmake_minimum_required(VERSION 3.24)
project(gtfock_psi4 VERSION 0.1.0 LANGUAGES C CXX Fortran)

include(GNUInstallDirs)
include(CTest)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(GTF_GTFock_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/GTFock" CACHE PATH
    "GTFock source tree (the build script uses a patched staging copy)")
option(GTF_BUILD_PSCF "Build the native MPI SCF executable" ON)
option(GTF_COMBINED_JK "Accumulate Coulomb and exchange into one SCF matrix" ON)

find_package(MPI REQUIRED COMPONENTS C)
find_package(OpenMP REQUIRED COMPONENTS C)
find_package(simint 0.8 CONFIG REQUIRED)

# Search only the active environment/prefix for numerical libraries. This avoids
# silently mixing conda MPI with host BLAS, LAPACK, or ScaLAPACK installations.
set(_gtf_prefix_hints ${CMAKE_PREFIX_PATH})
if(DEFINED ENV{CONDA_PREFIX})
  list(PREPEND _gtf_prefix_hints "$ENV{CONDA_PREFIX}")
endif()
find_path(MKL_INCLUDE_DIR mkl.h HINTS ${_gtf_prefix_hints} PATH_SUFFIXES include REQUIRED)
find_library(BLAS_LIBRARY NAMES blas HINTS ${_gtf_prefix_hints} PATH_SUFFIXES lib REQUIRED)
find_library(LAPACK_LIBRARY NAMES lapack HINTS ${_gtf_prefix_hints} PATH_SUFFIXES lib REQUIRED)
find_library(MKL_SCALAPACK_LIBRARY NAMES mkl_scalapack_lp64
  HINTS ${_gtf_prefix_hints} PATH_SUFFIXES lib REQUIRED)
find_library(MKL_BLACS_LIBRARY NAMES mkl_blacs_openmpi_lp64
  HINTS ${_gtf_prefix_hints} PATH_SUFFIXES lib REQUIRED)

if(NOT DEFINED ENV{CONDA_PREFIX})
  message(FATAL_ERROR "Activate the supported gtf2 conda environment")
endif()
set(_gtf_conda_prefix "$ENV{CONDA_PREFIX}")
foreach(_path IN ITEMS
    CMAKE_C_COMPILER CMAKE_CXX_COMPILER CMAKE_Fortran_COMPILER
    MKL_INCLUDE_DIR BLAS_LIBRARY LAPACK_LIBRARY
    MKL_SCALAPACK_LIBRARY MKL_BLACS_LIBRARY)
  cmake_path(IS_PREFIX _gtf_conda_prefix "${${_path}}" NORMALIZE _in_conda)
  if(NOT _in_conda)
    message(FATAL_ERROR "${_path} resolved outside gtf2: ${${_path}}")
  endif()
endforeach()
foreach(_mpi_library IN LISTS MPI_C_LIBRARIES)
  cmake_path(IS_PREFIX _gtf_conda_prefix "${_mpi_library}" NORMALIZE _in_conda)
  if(NOT _in_conda)
    message(FATAL_ERROR "MPI resolved outside gtf2: ${_mpi_library}")
  endif()
endforeach()

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
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${MKL_INCLUDE_DIR}")
target_compile_definitions(gtfock PRIVATE __ALIGNLEN__=64)
if(GTF_COMBINED_JK)
  target_compile_definitions(gtfock PRIVATE __SCF__)
endif()
target_link_libraries(gtfock PUBLIC MPI::MPI_C)
target_link_libraries(gtfock PRIVATE
  cint GTMatrix simint::simint OpenMP::OpenMP_C
  "${MKL_SCALAPACK_LIBRARY}" "${MKL_BLACS_LIBRARY}"
  "${LAPACK_LIBRARY}" "${BLAS_LIBRARY}" m)
gtf_target_defaults(gtfock)
set_target_properties(gtfock PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION 0
  INSTALL_RPATH "$ORIGIN")

if(GTF_BUILD_PSCF)
  add_executable(pscf
    "${GTF_GTFock_SOURCE_DIR}/pscf/scf.c"
    "${GTF_GTFock_SOURCE_DIR}/pscf/purif.c"
    "${GTF_GTFock_SOURCE_DIR}/pscf/pdgemm.c")
  target_include_directories(pscf PRIVATE
    "${GTF_GTFock_SOURCE_DIR}/aligned_malloc"
    "${GTF_GTFock_SOURCE_DIR}/pfock"
    "${GTF_GTFock_SOURCE_DIR}/pscf"
    "${CMAKE_CURRENT_SOURCE_DIR}/libcint"
    "${CMAKE_CURRENT_SOURCE_DIR}/GTMatrix"
    "${MKL_INCLUDE_DIR}")
  target_link_libraries(pscf PRIVATE
    gtfock cint GTMatrix simint::simint MPI::MPI_C OpenMP::OpenMP_C
    "${MKL_SCALAPACK_LIBRARY}" "${MKL_BLACS_LIBRARY}"
    "${LAPACK_LIBRARY}" "${BLAS_LIBRARY}" m)
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
      "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" 2
      "$<TARGET_FILE:test_overlap>"
      "${CMAKE_CURRENT_SOURCE_DIR}/GTFock/data/sto-3g.gbs"
      "${CMAKE_CURRENT_SOURCE_DIR}/GTFock/data/water.xyz")
  set_tests_properties(legacy_mpi_overlap PROPERTIES TIMEOUT 120 PROCESSORS 2)

  if(GTF_BUILD_PSCF)
    add_test(NAME pscf_numerical_regression
      COMMAND "${Python_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pscf_regression.py"
        --mpiexec "${MPIEXEC_EXECUTABLE}"
        --pscf "$<TARGET_FILE:pscf>"
        --root "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(pscf_numerical_regression PROPERTIES
      TIMEOUT 150 PROCESSORS 2)
  endif()
endif()

install(TARGETS cint gtfock
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(TARGETS GTMatrix erd oed
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
if(GTF_BUILD_PSCF)
  install(TARGETS pscf RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(FILES
  libcint/CInt.h
  GTMatrix/GTMatrix.h
  "${GTF_GTFock_SOURCE_DIR}/pfock/pfock.h"
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
