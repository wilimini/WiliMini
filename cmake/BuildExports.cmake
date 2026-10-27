if(NOT PSP_BUILD_EXPORTS OR NOT EXPORTS_EXP OR NOT OUTPUT_FILE)
    message(FATAL_ERROR "BuildExports.cmake requires PSP_BUILD_EXPORTS, EXPORTS_EXP, and OUTPUT_FILE")
endif()

execute_process(
    COMMAND "${PSP_BUILD_EXPORTS}" -b "${EXPORTS_EXP}"
    OUTPUT_FILE "${OUTPUT_FILE}"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "psp-build-exports failed with ${result}")
endif()
