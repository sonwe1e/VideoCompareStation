# CPack 4.4 supplies this staging path to CPACK_PRE_BUILD_SCRIPTS. The repository exercises
# both package generators before release.
if(NOT DEFINED CPACK_TEMPORARY_INSTALL_DIRECTORY OR
   CPACK_TEMPORARY_INSTALL_DIRECTORY STREQUAL "" OR
   NOT IS_ABSOLUTE "${CPACK_TEMPORARY_INSTALL_DIRECTORY}")
    message(FATAL_ERROR "CPack did not provide an absolute staging directory.")
endif()
cmake_path(SET stageRoot NORMALIZE "${CPACK_TEMPORARY_INSTALL_DIRECTORY}")
if(NOT IS_DIRECTORY "${stageRoot}")
    message(FATAL_ERROR "CPack staging directory does not exist: ${stageRoot}")
endif()

set(
    requiredFiles
    "CompareStation.exe"
    "CompareStationCli.exe"
    "Qt6Core.dll"
    "Qt6Gui.dll"
    "Qt6Qml.dll"
    "Qt6Quick.dll"
    "avcodec-62.dll"
    "avformat-62.dll"
    "avutil-60.dll"
    "swscale-9.dll"
    "platforms/qwindows.dll"
    "assets/branding/comparestation-icon.png"
    "assets/branding/comparestation.ico"
    "licenses/THIRD_PARTY_NOTICES.md"
    "licenses/vcpkg/ffmpeg.txt"
)
foreach(relativePath IN LISTS requiredFiles)
    set(installedPath "${stageRoot}/${relativePath}")
    if(NOT EXISTS "${installedPath}" OR IS_DIRECTORY "${installedPath}")
        message(
            FATAL_ERROR
            "Package staging is incomplete: '${relativePath}' is missing. "
            "Complete runtime deployment and notices before packaging."
        )
    endif()
    file(SIZE "${installedPath}" installedSize)
    if(installedSize EQUAL 0)
        message(FATAL_ERROR "Package staging contains an empty file: ${relativePath}")
    endif()
endforeach()

foreach(forbiddenTool IN ITEMS ffmpeg.exe ffprobe.exe)
    if(EXISTS "${stageRoot}/${forbiddenTool}")
        message(
            FATAL_ERROR
            "Package staging must not contain the unused external tool '${forbiddenTool}'."
        )
    endif()
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/VerifyPeSubsystem.cmake")
dvs_verify_pe_subsystem("${stageRoot}/CompareStation.exe" WINDOWS_GUI)
dvs_verify_pe_subsystem("${stageRoot}/CompareStationCli.exe" WINDOWS_CUI)

if(NOT IS_DIRECTORY "${stageRoot}/qml/QtQuick")
    message(FATAL_ERROR "Package staging is missing the QtQuick QML import tree.")
endif()
file(GLOB_RECURSE qtQuickFiles LIST_DIRECTORIES FALSE "${stageRoot}/qml/QtQuick/*")
if(NOT qtQuickFiles)
    message(FATAL_ERROR "The staged QtQuick QML import tree is empty.")
endif()

file(READ "${stageRoot}/licenses/THIRD_PARTY_NOTICES.md" notices)
if(NOT notices MATCHES "FFmpeg" OR NOT notices MATCHES "GPL")
    message(FATAL_ERROR "Third-party notices must identify FFmpeg and its GPL license profile.")
endif()

execute_process(
    COMMAND "${stageRoot}/CompareStationCli.exe" --probe
            "${CMAKE_CURRENT_LIST_DIR}/../tests/fixtures/media/h264_a_320x180_30fps_12.mp4"
    WORKING_DIRECTORY "${stageRoot}"
    RESULT_VARIABLE startupResult
    OUTPUT_VARIABLE startupOutput
    ERROR_VARIABLE startupError
    TIMEOUT 30
)
if(NOT startupResult EQUAL 0)
    message(
        FATAL_ERROR
        "Packaged startup check failed (${startupResult}). "
        "stdout: ${startupOutput} stderr: ${startupError}"
    )
endif()
