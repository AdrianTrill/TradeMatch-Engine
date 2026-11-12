# End-to-end check of the ITCH tool: write a simulated trading day as an ITCH 5.0 file, verify that replaying it
# rebuilds the engine's books exactly, then replay it as a user would. CTest runs this script with -DITCH=... and
# -DFEED=....
execute_process(
    COMMAND "${ITCH}" generate "${FEED}" --seed 3 --minutes 1 --verify
    OUTPUT_VARIABLE generate_output
    ERROR_VARIABLE generate_errors
    RESULT_VARIABLE generate_result)
if(NOT generate_result EQUAL 0 OR NOT generate_output MATCHES "verified")
    file(REMOVE "${FEED}")
    message(FATAL_ERROR "tradematch_itch generate failed (${generate_result}):\n${generate_output}${generate_errors}")
endif()

execute_process(
    COMMAND "${ITCH}" replay "${FEED}" --symbol ACME
    OUTPUT_VARIABLE replay_output
    ERROR_VARIABLE replay_errors
    RESULT_VARIABLE replay_result)
message("${generate_output}${replay_output}")
file(REMOVE "${FEED}")
if(NOT replay_result EQUAL 0 OR NOT replay_output MATCHES "anomalies 0")
    message(FATAL_ERROR "tradematch_itch replay failed (${replay_result}):\n${replay_output}${replay_errors}")
endif()
