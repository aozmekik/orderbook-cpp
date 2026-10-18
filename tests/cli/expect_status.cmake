# Runs a command and checks its exact exit status, and optionally that its
# standard output or standard error matches a regular expression.
#   cmake -DCOMMAND=<tool|args...> -DSTATUS=<n> [-DMATCH=<regex>] -P expect_status.cmake
# COMMAND is a list; pass it with '|' separators (CTest mangles ';').
string(REPLACE "|" ";" command "${COMMAND}")
execute_process(
  COMMAND ${command}
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
  RESULT_VARIABLE status)
if(NOT "${status}" STREQUAL "${STATUS}")
  message(FATAL_ERROR "expected exit status ${STATUS}, got '${status}': ${command}\n${err}")
endif()
if(DEFINED MATCH AND NOT "${out}${err}" MATCHES "${MATCH}")
  message(FATAL_ERROR "output does not match '${MATCH}': ${command}\n${out}${err}")
endif()
