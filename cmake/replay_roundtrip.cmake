# End-to-end check of the journal: record a simulated session with the TUI, replay the journal with the replay tool,
# and require the same state hash. CTest runs this script with -DTUI=..., -DREPLAY=..., and -DJOURNAL=....
execute_process(
    COMMAND "${TUI}" --seed 5 --frames 300 --speed 8 --no-sleep --headless --journal "${JOURNAL}"
    OUTPUT_VARIABLE tui_output
    ERROR_VARIABLE tui_errors
    RESULT_VARIABLE tui_result)
if(NOT tui_result EQUAL 0)
    message(FATAL_ERROR "tradematch_tui failed (${tui_result}):\n${tui_output}${tui_errors}")
endif()

string(REGEX MATCH "state hash (0x[0-9a-f]+)" hash_line "${tui_output}")
if(NOT hash_line)
    message(FATAL_ERROR "tradematch_tui did not report a state hash:\n${tui_output}")
endif()
set(expected_hash "${CMAKE_MATCH_1}")

execute_process(
    COMMAND "${REPLAY}" "${JOURNAL}" --expect-hash "${expected_hash}"
    OUTPUT_VARIABLE replay_output
    ERROR_VARIABLE replay_errors
    RESULT_VARIABLE replay_result)
message("${tui_output}${replay_output}")
file(REMOVE "${JOURNAL}")
if(NOT replay_result EQUAL 0)
    message(FATAL_ERROR "replaying the journal did not reproduce state ${expected_hash}:\n${replay_errors}")
endif()
