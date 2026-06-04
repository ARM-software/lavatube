if(NOT DEFINED REPLAY OR NOT DEFINED INPUT_TRACE OR NOT DEFINED OUTPUT_PREFIX)
	message(FATAL_ERROR "REPLAY, INPUT_TRACE, and OUTPUT_PREFIX must be set")
endif()

file(REMOVE "${OUTPUT_PREFIX}1.png")
execute_process(
	COMMAND "${REPLAY}" -C -w none --screenshots 1 --screenshot-prefix "${OUTPUT_PREFIX}" "${INPUT_TRACE}"
	RESULT_VARIABLE result
	OUTPUT_VARIABLE output
	ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
	message(FATAL_ERROR "lava-replay failed with ${result}\n${output}\n${error}")
endif()
if(EXISTS "${OUTPUT_PREFIX}1.png")
	message(FATAL_ERROR "Unexpected frame-boundary screenshot for frame 1: ${OUTPUT_PREFIX}1.png")
endif()
