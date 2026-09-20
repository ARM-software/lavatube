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
		"\"name\":\"clGetDeviceIDs\""
		"\"platform\":0"
		"\"device_type\":4294967295"
		"\"devices\":[0,1]"
		"\"num_devices\":2"
		"\"platform_unresolved\":true"
		"\"result\":0")
	string(FIND "${print_output}" "${expected}" found)
	if(found EQUAL -1)
		message(FATAL_ERROR
			"Missing ${expected} in lava-print output:\n${print_output}")
	endif()
endforeach()

string(REGEX MATCHALL "\"devices\":\\[0,1\\]" device_enumerations
	"${print_output}")
list(LENGTH device_enumerations device_enumeration_count)
if(NOT device_enumeration_count EQUAL 2)
	message(FATAL_ERROR
		"Expected two stable device enumerations:\n${print_output}")
endif()
