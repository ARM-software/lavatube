execute_process(
	COMMAND "${OPENCL_LAYER_TEST}" "${OPENCL_LAYER}" "${TRACE}"
	RESULT_VARIABLE capture_result
	OUTPUT_VARIABLE capture_output
	ERROR_VARIABLE capture_error
)
if(NOT capture_result EQUAL 0)
	message(FATAL_ERROR
		"OpenCL capture failed: ${capture_output}${capture_error}")
endif()

execute_process(
	COMMAND "${LAVA_PRINT}" -s 1 "${TRACE}"
	RESULT_VARIABLE print_result
	OUTPUT_VARIABLE print_output
	ERROR_VARIABLE print_error
)
if(NOT print_result EQUAL 0)
	message(FATAL_ERROR
		"OpenCL print failed: ${print_output}${print_error}")
endif()

foreach(expected
		"\"name\":\"clGetPlatformIDs\""
		"\"num_entries\":2"
		"\"platforms\":[0,1]"
		"\"num_platforms\":2"
		"\"result\":0")
	string(FIND "${print_output}" "${expected}" found)
	if(found EQUAL -1)
		message(FATAL_ERROR
			"Missing ${expected} in lava-print output:\n${print_output}")
	endif()
endforeach()
