if(NOT DEFINED REPLAY OR NOT DEFINED TRACE)
	message(FATAL_ERROR "REPLAY and TRACE are required")
endif()

execute_process(
	COMMAND "${REPLAY}" -V "${TRACE}"
	RESULT_VARIABLE replay_result
	OUTPUT_VARIABLE replay_stdout
	ERROR_VARIABLE replay_stderr
)

if(NOT replay_result EQUAL 0)
	message(FATAL_ERROR "Replay failed (${replay_result}):\n${replay_stdout}\n${replay_stderr}")
endif()

set(replay_output "${replay_stdout}\n${replay_stderr}")
if(replay_output MATCHES "simultaneously used")
	message(FATAL_ERROR "Replay reported an external-synchronization validation error:\n${replay_output}")
endif()
