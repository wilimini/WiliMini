if(NOT SOURCE_DIR OR NOT PATCH_FILES)
    message(FATAL_ERROR "ApplyPatch.cmake requires SOURCE_DIR and PATCH_FILES")
endif()

foreach(PATCH_FILE IN LISTS PATCH_FILES)
    execute_process(
        COMMAND patch -d "${SOURCE_DIR}" -p1 -R --dry-run
        INPUT_FILE "${PATCH_FILE}"
        RESULT_VARIABLE reverse_result
        OUTPUT_QUIET ERROR_QUIET)
    if(reverse_result EQUAL 0)
        continue()
    endif()

    execute_process(
        COMMAND patch -d "${SOURCE_DIR}" -p1 --forward
        INPUT_FILE "${PATCH_FILE}"
        RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Failed to apply ${PATCH_FILE}")
    endif()
endforeach()
