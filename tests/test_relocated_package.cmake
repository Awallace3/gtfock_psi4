foreach(_required IN ITEMS BUILD_DIR STAGE_DIR RELOCATED_DIR
                           CONSUMER_SOURCE_DIR GENERATOR MAKE_PROGRAM)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "${_required} is required")
  endif()
endforeach()

file(REMOVE_RECURSE "${STAGE_DIR}" "${RELOCATED_DIR}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${STAGE_DIR}"
  RESULT_VARIABLE _install_result
  OUTPUT_VARIABLE _install_output
  ERROR_VARIABLE _install_error)
if(NOT _install_result EQUAL 0)
  message(FATAL_ERROR
    "staged install failed (${_install_result})\n${_install_output}${_install_error}")
endif()

file(RENAME "${STAGE_DIR}" "${RELOCATED_DIR}")
file(GLOB_RECURSE _metadata "${RELOCATED_DIR}/*GTFock*.cmake")
if(NOT _metadata)
  message(FATAL_ERROR "relocated install has no GTFock CMake metadata")
endif()
foreach(_file IN LISTS _metadata)
  file(READ "${_file}" _contents)
  foreach(_forbidden IN ITEMS "${BUILD_DIR}" "${STAGE_DIR}" "${CONSUMER_SOURCE_DIR}")
    string(FIND "${_contents}" "${_forbidden}" _position)
    if(NOT _position EQUAL -1)
      message(FATAL_ERROR "${_file} embeds build-tree path ${_forbidden}")
    endif()
  endforeach()
endforeach()

set(_consumer_build "${BUILD_DIR}/package-consumer-build")
file(REMOVE_RECURSE "${_consumer_build}")
execute_process(
  COMMAND "${CMAKE_COMMAND}"
    -S "${CONSUMER_SOURCE_DIR}"
    -B "${_consumer_build}"
    -G "${GENERATOR}"
    "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}"
    "-DCMAKE_PREFIX_PATH=${RELOCATED_DIR}"
    -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF
  RESULT_VARIABLE _configure_result
  OUTPUT_VARIABLE _configure_output
  ERROR_VARIABLE _configure_error)
if(NOT _configure_result EQUAL 0)
  message(FATAL_ERROR
    "relocated consumer configure failed (${_configure_result})\n"
    "${_configure_output}${_configure_error}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${_consumer_build}"
  RESULT_VARIABLE _build_result
  OUTPUT_VARIABLE _build_output
  ERROR_VARIABLE _build_error)
if(NOT _build_result EQUAL 0)
  message(FATAL_ERROR
    "relocated consumer build failed (${_build_result})\n"
    "${_build_output}${_build_error}")
endif()
execute_process(
  COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${_consumer_build}"
          --output-on-failure
  RESULT_VARIABLE _test_result
  OUTPUT_VARIABLE _test_output
  ERROR_VARIABLE _test_error)
if(NOT _test_result EQUAL 0)
  message(FATAL_ERROR
    "relocated consumer runtime failed (${_test_result})\n"
    "${_test_output}${_test_error}")
endif()

find_program(_ldd ldd REQUIRED)
execute_process(
  COMMAND "${_ldd}" "${_consumer_build}/gtfock_consumer"
  RESULT_VARIABLE _ldd_result
  OUTPUT_VARIABLE _ldd_output
  ERROR_VARIABLE _ldd_error)
if(NOT _ldd_result EQUAL 0 OR "${_ldd_output}${_ldd_error}" MATCHES "not found")
  message(FATAL_ERROR "consumer has unresolved libraries:\n${_ldd_output}${_ldd_error}")
endif()
foreach(_forbidden IN ITEMS "${BUILD_DIR}" "${STAGE_DIR}")
  string(FIND "${_ldd_output}" "${_forbidden}" _position)
  if(NOT _position EQUAL -1)
    message(FATAL_ERROR "consumer resolves a build-tree library: ${_forbidden}\n${_ldd_output}")
  endif()
endforeach()
