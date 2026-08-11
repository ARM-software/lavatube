if(NOT DEFINED IMPORTER OR NOT DEFINED INPUT_TRACE OR NOT DEFINED OUTPUT_TRACE)
	message(FATAL_ERROR "IMPORTER, INPUT_TRACE and OUTPUT_TRACE are required")
endif()

file(REMOVE "${OUTPUT_TRACE}")
execute_process(
	COMMAND "${IMPORTER}" "${INPUT_TRACE}" "${OUTPUT_TRACE}"
	RESULT_VARIABLE result
	OUTPUT_VARIABLE stdout
	ERROR_VARIABLE stderr)

if(result EQUAL 0)
	message(FATAL_ERROR "Import unexpectedly succeeded")
endif()

string(CONCAT output "${stdout}" "${stderr}")
if(NOT output MATCHES "VK_KHR_push_descriptor")
	message(FATAL_ERROR "Import failed without the expected push descriptor diagnostic:\n${output}")
endif()

file(REMOVE "${OUTPUT_TRACE}")
