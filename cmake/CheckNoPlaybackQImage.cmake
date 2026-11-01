if(NOT DEFINED PROJECT_SOURCE_DIR)
    message(FATAL_ERROR "PROJECT_SOURCE_DIR is required.")
endif()

file(
    GLOB_RECURSE playbackRenderFiles
    LIST_DIRECTORIES FALSE
    "${PROJECT_SOURCE_DIR}/src/application/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/application/*.h"
    "${PROJECT_SOURCE_DIR}/src/media_ffmpeg/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/media_ffmpeg/*.h"
    "${PROJECT_SOURCE_DIR}/src/platform_windows/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/platform_windows/*.h"
)
set(requiredUiFiles
    "${PROJECT_SOURCE_DIR}/src/ui_qml/include/dvs/ui/RenderAckRelay.h"
    "${PROJECT_SOURCE_DIR}/src/ui_qml/src/comparison/RenderAckRelay.cpp"
)
list(LENGTH requiredUiFiles requiredUiFileCount)
if(NOT requiredUiFileCount EQUAL 2)
    message(FATAL_ERROR "The playback QImage check must include both RenderAckRelay files.")
endif()
foreach(requiredUiFile IN LISTS requiredUiFiles)
    if(NOT EXISTS "${requiredUiFile}")
        message(FATAL_ERROR "Required playback QImage check anchor is missing: ${requiredUiFile}")
    endif()
endforeach()
list(APPEND playbackRenderFiles ${requiredUiFiles})
list(REMOVE_DUPLICATES playbackRenderFiles)
list(SORT playbackRenderFiles)

set(violations)
foreach(sourceFile IN LISTS playbackRenderFiles)
    if(NOT EXISTS "${sourceFile}")
        continue()
    endif()
    file(READ "${sourceFile}" sourceContents)
    string(FIND "${sourceContents}" "QImage" qimageOffset)
    if(NOT qimageOffset EQUAL -1)
        cmake_path(RELATIVE_PATH sourceFile BASE_DIRECTORY "${PROJECT_SOURCE_DIR}")
        list(APPEND violations "${sourceFile}")
    endif()
endforeach()

if(violations)
    list(JOIN violations "\n  " formattedViolations)
    message(FATAL_ERROR
        "QImage is forbidden in playback/render production code. Offending files:\n  "
        "${formattedViolations}")
endif()
