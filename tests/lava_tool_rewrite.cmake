if(NOT DEFINED LAVA_TOOL)
	message(FATAL_ERROR "LAVA_TOOL is required")
endif()

if(NOT DEFINED INPUT_TRACE)
	message(FATAL_ERROR "INPUT_TRACE is required")
endif()

if(NOT DEFINED OUTPUT_TRACE)
	message(FATAL_ERROR "OUTPUT_TRACE is required")
endif()

execute_process(
	COMMAND "${CMAKE_COMMAND}" -E rm -f "${OUTPUT_TRACE}"
	RESULT_VARIABLE cleanup_result
)
if(NOT cleanup_result EQUAL 0)
	message(FATAL_ERROR "Failed to remove stale output trace: ${OUTPUT_TRACE}")
endif()

execute_process(
	COMMAND "${LAVA_TOOL}" "${INPUT_TRACE}" "${OUTPUT_TRACE}"
	RESULT_VARIABLE rewrite_result
)
if(NOT rewrite_result EQUAL 0)
	message(FATAL_ERROR "lava-tool rewrite failed with exit code ${rewrite_result}")
endif()

execute_process(
	COMMAND "${CMAKE_COMMAND}" -E compare_files "${INPUT_TRACE}" "${OUTPUT_TRACE}"
	RESULT_VARIABLE compare_result
)
if(NOT compare_result EQUAL 0)
	message(FATAL_ERROR "Rewritten trace differs from input trace")
endif()

execute_process(
	COMMAND "${LAVA_TOOL}" -V "${OUTPUT_TRACE}"
	RESULT_VARIABLE validate_result
)
if(NOT validate_result EQUAL 0)
	message(FATAL_ERROR "Validation of rewritten trace failed with exit code ${validate_result}")
endif()
