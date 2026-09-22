add_library(dvs_test_support INTERFACE)
add_library(dvs::test_support ALIAS dvs_test_support)
target_include_directories(
    dvs_test_support
    INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/support/include"
)

if(DVS_BUILD_DESKTOP)
    # All repository scripts require PowerShell 7; Windows PowerShell 5.1 cannot run them.
    if(DEFINED DVS_POWERSHELL_EXECUTABLE AND NOT EXISTS "${DVS_POWERSHELL_EXECUTABLE}")
        unset(DVS_POWERSHELL_EXECUTABLE CACHE)
    endif()
    find_program(
        DVS_POWERSHELL_EXECUTABLE
        NAMES pwsh
        HINTS "$ENV{ProgramFiles}/PowerShell/7"
        DOC "PowerShell 7 or later used by repository script tests"
        REQUIRED
    )
    execute_process(
        COMMAND "${DVS_POWERSHELL_EXECUTABLE}" -NoLogo -NoProfile
                -Command "$PSVersionTable.PSVersion.ToString()"
        RESULT_VARIABLE dvs_powershell_result
        OUTPUT_VARIABLE dvs_powershell_version
        ERROR_VARIABLE dvs_powershell_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
        TIMEOUT 10
    )
    if(NOT dvs_powershell_result EQUAL 0 OR
       NOT dvs_powershell_version MATCHES "^[0-9]+\\.[0-9]+" OR
       dvs_powershell_version VERSION_LESS "7.0")
        message(FATAL_ERROR
            "Repository script tests require PowerShell 7 or later. "
            "Set DVS_POWERSHELL_EXECUTABLE to pwsh; '${DVS_POWERSHELL_EXECUTABLE}' "
            "reported '${dvs_powershell_version}': ${dvs_powershell_error}")
    endif()

    dvs_add_test(
        NAME quality.build-scripts
        LAYER unit
        MODULE app
        TIMEOUT 60
        COMMAND
            "${DVS_POWERSHELL_EXECUTABLE}"
            -NoLogo
            -NoProfile
            -File "${PROJECT_SOURCE_DIR}/tools/quality/check-build-scripts.ps1"
    )
endif()

add_subdirectory(unit)
if(DVS_BUILD_ADAPTERS OR DVS_BUILD_UI)
    add_subdirectory(component)
endif()
if(DVS_BUILD_DESKTOP)
    add_subdirectory(hardware)
    add_subdirectory(smoke)
endif()
