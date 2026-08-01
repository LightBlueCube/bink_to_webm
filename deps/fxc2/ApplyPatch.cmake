file(MAKE_DIRECTORY "${OUTPUT_DIR}")
file(COPY_FILE "${SOURCE_FILE}" "${OUTPUT_FILE}" ONLY_IF_DIFFERENT)

foreach(patch_file IN LISTS PATCH_FILES)
	message(STATUS "Applying patch: ${patch_file}")
	execute_process(
		COMMAND "${PATCH_EXECUTABLE}" --batch -p1 -i "${patch_file}"
		WORKING_DIRECTORY "${OUTPUT_DIR}"
		RESULT_VARIABLE patch_result
		OUTPUT_VARIABLE patch_output
		ERROR_VARIABLE patch_error
	)

	if(NOT patch_result EQUAL 0)
		message(FATAL_ERROR "Failed to apply patch ${patch_file} (code:${patch_result}):\n${patch_output}\n${patch_error}")
	endif()
endforeach()
