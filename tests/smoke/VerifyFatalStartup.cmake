if(NOT DEFINED DVS_GUI_EXECUTABLE OR NOT DEFINED DVS_TEST_ROOT)
    message(FATAL_ERROR "DVS_GUI_EXECUTABLE and DVS_TEST_ROOT are required.")
endif()

file(REMOVE_RECURSE "${DVS_TEST_ROOT}")
file(MAKE_DIRECTORY "${DVS_TEST_ROOT}")

execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        -E
        env
        "LOCALAPPDATA=${DVS_TEST_ROOT}"
        "${DVS_GUI_EXECUTABLE}"
        --ui-fatal-startup-smoke
    RESULT_VARIABLE fatalResult
    OUTPUT_VARIABLE fatalOutput
    ERROR_VARIABLE fatalError
    TIMEOUT 5
)
if(fatalResult EQUAL 0)
    message(FATAL_ERROR "The fatal-startup smoke path unexpectedly succeeded.")
endif()

set(logPath "${DVS_TEST_ROOT}/CompareStation/logs/CompareStation.log")
if(NOT EXISTS "${logPath}" OR IS_DIRECTORY "${logPath}")
    message(
        FATAL_ERROR
        "The fatal-startup smoke path did not create '${logPath}'. "
        "stdout: ${fatalOutput} stderr: ${fatalError}"
    )
endif()

file(READ "${logPath}" logContent)
if(NOT logContent MATCHES "DVS_UI_FATAL_STARTUP_SMOKE")
    message(FATAL_ERROR "The startup log does not contain the fatal diagnostic marker.")
endif()
