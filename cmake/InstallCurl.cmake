if(NOT SOURCE_DIR OR NOT BUILD_DIR OR NOT INSTALL_PREFIX OR NOT MAKE_PROGRAM)
    message(FATAL_ERROR "InstallCurl.cmake requires SOURCE_DIR, BUILD_DIR, INSTALL_PREFIX, and MAKE_PROGRAM")
endif()

execute_process(
    COMMAND "${MAKE_PROGRAM}" -C "${BUILD_DIR}/lib" install
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "libcurl install failed with ${result}")
endif()

execute_process(
    COMMAND "${MAKE_PROGRAM}" -C "${BUILD_DIR}/include" install
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "libcurl headers install failed with ${result}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${SOURCE_DIR}/COPYING" "${INSTALL_PREFIX}/COPYING"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "libcurl license install failed with ${result}")
endif()
