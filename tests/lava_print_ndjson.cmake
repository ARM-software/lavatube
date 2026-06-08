if(NOT DEFINED LAVA_PRINT)
	message(FATAL_ERROR "LAVA_PRINT is required")
endif()

if(NOT DEFINED PYTHON_EXECUTABLE)
	message(FATAL_ERROR "PYTHON_EXECUTABLE is required")
endif()

if(NOT DEFINED INPUT_TRACE)
	message(FATAL_ERROR "INPUT_TRACE is required")
endif()

if(NOT DEFINED OUTPUT_FILE)
	message(FATAL_ERROR "OUTPUT_FILE is required")
endif()

set(TOOL_ARGS)
if(DEFINED FRAME_START OR DEFINED FRAME_END)
	if(NOT DEFINED FRAME_START OR NOT DEFINED FRAME_END)
		message(FATAL_ERROR "FRAME_START and FRAME_END must be provided together")
	endif()
	list(APPEND TOOL_ARGS -f "${FRAME_START}" "${FRAME_END}")
endif()

execute_process(
	COMMAND "${LAVA_PRINT}" ${TOOL_ARGS} "${INPUT_TRACE}"
	OUTPUT_FILE "${OUTPUT_FILE}"
	RESULT_VARIABLE print_result
)
if(NOT print_result EQUAL 0)
	message(FATAL_ERROR "lava-print failed with exit code ${print_result}")
endif()

if(DEFINED EXPECT_FRAME)
	execute_process(
		COMMAND "${PYTHON_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/check_ndjson_packets.py" "${OUTPUT_FILE}" "${EXPECT_FRAME}"
		RESULT_VARIABLE check_result
	)
else()
	execute_process(
		COMMAND "${PYTHON_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/check_ndjson_packets.py" "${OUTPUT_FILE}"
		RESULT_VARIABLE check_result
	)
endif()
if(NOT check_result EQUAL 0)
	message(FATAL_ERROR "lava-print NDJSON validation failed with exit code ${check_result}")
endif()
