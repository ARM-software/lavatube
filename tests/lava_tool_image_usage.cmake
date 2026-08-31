if(NOT DEFINED LAVA_TOOL OR NOT DEFINED INPUT_TRACE OR NOT DEFINED IMAGE_USAGE_OPTION)
	message(FATAL_ERROR "LAVA_TOOL, INPUT_TRACE, and IMAGE_USAGE_OPTION are required")
endif()

set(image_usage_command "${LAVA_TOOL}" "${IMAGE_USAGE_OPTION}")
if(OUTPUT_TSV)
	list(APPEND image_usage_command "--tsv")
endif()
list(APPEND image_usage_command "${INPUT_TRACE}")

execute_process(
	COMMAND ${image_usage_command}
	RESULT_VARIABLE image_usage_result
	OUTPUT_VARIABLE image_usage_output
	ERROR_VARIABLE image_usage_error)

if(NOT image_usage_result EQUAL 0)
	message(FATAL_ERROR "lava-tool image usage failed with exit code ${image_usage_result}:\n${image_usage_error}")
endif()

if(OUTPUT_TSV)
	set(expected_header "Format\tBytes\tSize\t% of Image Sources\tDescription")
else()
	set(expected_header "| Format")
endif()

foreach(expected_text IN ITEMS
		"${expected_header}"
		"VK_FORMAT_R8G8B8A8_UNORM"
		"VK_FORMAT_R8G8B8A8_SRGB"
		"Total image sources in buffers")
	string(FIND "${image_usage_output}" "${expected_text}" expected_position)
	if(expected_position EQUAL -1)
		message(FATAL_ERROR "Image usage output is missing '${expected_text}':\n${image_usage_output}")
	endif()
endforeach()

if(OUTPUT_TSV)
	foreach(expected_row IN ITEMS
			"VK_FORMAT_R8G8B8A8_SRGB\t1048625"
			"VK_FORMAT_R8G8B8A8_UNORM\t524337"
			"Total image sources in buffers\t1572962")
		string(FIND "${image_usage_output}" "${expected_row}" expected_position)
		if(expected_position EQUAL -1)
			message(FATAL_ERROR "Image usage output has unexpected byte attribution; missing '${expected_row}':\n${image_usage_output}")
		endif()
	endforeach()
endif()

string(FIND "${image_usage_output}" "Unknown" unknown_position)
if(NOT unknown_position EQUAL -1)
	message(FATAL_ERROR "Known sample image formats were reported as unknown:\n${image_usage_output}")
endif()
