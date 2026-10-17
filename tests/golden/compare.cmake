# Runs a command and compares its standard output with a file.
#   cmake -DCOMMAND=<tool;args...> -DEXPECTED=<file> -P compare.cmake
# COMMAND is a list; pass it with '|' separators (CTest mangles ';').
string(REPLACE "|" ";" command "${COMMAND}")
execute_process(
  COMMAND ${command}
  OUTPUT_VARIABLE actual
  ERROR_VARIABLE errors
  RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "command failed with status ${status}: ${command}\n${errors}")
endif()
file(READ "${EXPECTED}" expected)
if(NOT actual STREQUAL expected)
  get_filename_component(name "${EXPECTED}" NAME)
  set(actual_file "${CMAKE_CURRENT_BINARY_DIR}/${name}.actual")
  file(WRITE "${actual_file}" "${actual}")
  message(FATAL_ERROR "output differs from ${EXPECTED}; actual output written to ${actual_file}")
endif()
