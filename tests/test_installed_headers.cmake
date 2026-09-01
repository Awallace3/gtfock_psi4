if(NOT BUILD_DIR OR NOT STAGE_DIR OR NOT MPI_C_COMPILER)
  message(FATAL_ERROR "BUILD_DIR, STAGE_DIR, and MPI_C_COMPILER are required")
endif()

file(REMOVE_RECURSE "${STAGE_DIR}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${STAGE_DIR}"
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_output
  ERROR_VARIABLE install_error)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR
    "staged install failed (${install_result})\n${install_output}${install_error}")
endif()

set(smoke_source "${STAGE_DIR}/installed_header_smoke.c")
file(WRITE "${smoke_source}"
  "#include <pfock.h>\n"
  "int main(void) { PFock_t pfock = 0; return pfock != 0; }\n")
execute_process(
  COMMAND "${MPI_C_COMPILER}" -std=c11 -fsyntax-only
          "-I${STAGE_DIR}/include" "${smoke_source}"
  RESULT_VARIABLE compile_result
  OUTPUT_VARIABLE compile_output
  ERROR_VARIABLE compile_error)
if(NOT compile_result EQUAL 0)
  message(FATAL_ERROR
    "installed pfock.h is not self-contained (${compile_result})\n"
    "${compile_output}${compile_error}")
endif()

set(df_source "${STAGE_DIR}/installed_df_header_smoke.c")
file(WRITE "${df_source}"
  "#include <gtfock_df.h>\n"
  "int main(void) { GTFDF_t df = 0; return df != 0; }\n")
execute_process(
  COMMAND "${MPI_C_COMPILER}" -std=c11 -fsyntax-only
          "-I${STAGE_DIR}/include" "${df_source}"
  RESULT_VARIABLE df_result
  OUTPUT_VARIABLE df_output
  ERROR_VARIABLE df_error)
if(NOT df_result EQUAL 0)
  message(FATAL_ERROR
    "installed gtfock_df.h is not self-contained (${df_result})\n"
    "${df_output}${df_error}")
endif()
