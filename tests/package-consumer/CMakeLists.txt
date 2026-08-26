cmake_minimum_required(VERSION 3.24)
project(gtfock_package_consumer LANGUAGES C)

find_package(GTFock 0.1 CONFIG REQUIRED)

# An installed package must resolve entirely inside the prefix it was found in.
# A library location or include directory outside it means the metadata still
# points at the tree that produced the package, which breaks relocation.
foreach(_gtfock_target IN ITEMS GTFock::GTFock GTFock::CInt GTFock::GTMatrix)
  # Both halves must be present: guarding the union would let an unresolvable
  # imported library location go unchecked while the include directories alone
  # carried the assertion.
  get_target_property(_gtfock_location ${_gtfock_target} LOCATION)
  if(NOT _gtfock_location)
    message(FATAL_ERROR
      "${_gtfock_target} exposes no imported library location to check")
  endif()
  get_target_property(_gtfock_includes ${_gtfock_target}
    INTERFACE_INCLUDE_DIRECTORIES)
  if(NOT _gtfock_includes)
    message(FATAL_ERROR
      "${_gtfock_target} exposes no include directories to check")
  endif()
  set(_gtfock_paths "${_gtfock_location}" ${_gtfock_includes})
  foreach(_gtfock_path IN LISTS _gtfock_paths)
    set(_gtfock_inside FALSE)
    foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
      cmake_path(IS_PREFIX _prefix "${_gtfock_path}" NORMALIZE _in_prefix)
      if(_in_prefix)
        set(_gtfock_inside TRUE)
        break()
      endif()
    endforeach()
    if(NOT _gtfock_inside)
      message(FATAL_ERROR
        "${_gtfock_target} resolves ${_gtfock_path} outside the package "
        "prefix (${CMAKE_PREFIX_PATH})")
    endif()
  endforeach()
endforeach()

add_executable(gtfock_consumer main.c)
target_link_libraries(gtfock_consumer PRIVATE GTFock::GTFock)

enable_testing()
add_test(NAME consumer_runtime COMMAND gtfock_consumer)
