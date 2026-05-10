if(NOT EXISTS "${INPUT_TRACE}")
	message(FATAL_ERROR "Input trace missing: ${INPUT_TRACE}")
endif()

if(NOT DEFINED COMPARE_TRACE)
	set(COMPARE_TRACE "${INPUT_TRACE}")
endif()

set(TOOL_ARGS "${INPUT_TRACE}" "${OUTPUT_TRACE}")
if(SIMULATE)
	set(TOOL_ARGS -S "${INPUT_TRACE}" "${OUTPUT_TRACE}")
endif()

execute_process(
	COMMAND "${LAVA_TOOL}" ${TOOL_ARGS}
	RESULT_VARIABLE write_out_result
)
if(NOT write_out_result EQUAL 0)
	message(FATAL_ERROR "lava-tool write-out failed with exit code ${write_out_result}")
endif()

execute_process(
	COMMAND "${PACKTOOL}" diff --assert-markings "${COMPARE_TRACE}" "${OUTPUT_TRACE}"
	RESULT_VARIABLE diff_result
)
if(NOT diff_result EQUAL 0)
	message(FATAL_ERROR "packtool diff --assert-markings failed with exit code ${diff_result}")
endif()
