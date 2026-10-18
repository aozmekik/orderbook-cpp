# Generates a stream with obgen and replays it with obreplay through every
# engine: each replay must succeed, and all of them must print the same bytes.
#   cmake -DOBGEN=<path> -DOBREPLAY=<path> -DWORK=<dir> -P obgen_replay.cmake
file(MAKE_DIRECTORY "${WORK}")
set(stream "${WORK}/obgen.csv")
execute_process(
  COMMAND "${OBGEN}" --commands=20000 --symbols=2 --seed=7 --quiet
  OUTPUT_FILE "${stream}"
  RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "obgen failed with status ${status}")
endif()
foreach(engine IN ITEMS ladder map reference)
  execute_process(
    COMMAND "${OBREPLAY}" --events --engine=${engine} "${stream}"
    OUTPUT_VARIABLE out_${engine}
    ERROR_VARIABLE err
    RESULT_VARIABLE status)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "obreplay --engine=${engine} failed with status ${status}\n${err}")
  endif()
endforeach()
if(NOT out_ladder MATCHES "\nfill SYM1 " OR NOT out_ladder MATCHES "\nsnapshot SYM2 ")
  message(FATAL_ERROR "the replay printed no fills or no snapshot for SYM2")
endif()
if(NOT out_ladder STREQUAL out_map OR NOT out_ladder STREQUAL out_reference)
  message(FATAL_ERROR "engines printed different output for the same obgen stream")
endif()
