cmake_minimum_required(VERSION 4.4)

foreach(requiredVariable DVS_BINARY_DIR DVS_SOURCE_DIR DVS_NINJA_EXECUTABLE)
    if(NOT DEFINED ${requiredVariable} OR "${${requiredVariable}}" STREQUAL "")
        message(FATAL_ERROR "${requiredVariable} is required.")
    endif()
endforeach()

function(dependency_failure reason)
    message(FATAL_ERROR
        "MSVC header dependency tracking is broken for ${dependencyObject}: ${reason}. "
        "Header edits may leave stale objects. Reconfigure and rebuild using "
        "tools/build/build.ps1 -Preset <preset> -Fresh. Ninja reported:\n${depsOutput}")
endfunction()

function(check_object_dependencies dependencyObject required)
    if(NOT EXISTS "${DVS_BINARY_DIR}/${dependencyObject}")
        if(required)
            message(FATAL_ERROR
                "MSVC dependency check requires ${dependencyObject}. "
                "Build dvs_domain before running this check.")
        endif()
        return()
    endif()
    execute_process(
        COMMAND "${DVS_NINJA_EXECUTABLE}" -C "${DVS_BINARY_DIR}" -t deps "${dependencyObject}"
        RESULT_VARIABLE depsResult
        OUTPUT_VARIABLE depsOutput
        ERROR_VARIABLE depsError
        OUTPUT_STRIP_TRAILING_WHITESPACE
        TIMEOUT 10
    )
    if(NOT depsResult EQUAL 0)
        dependency_failure("could not inspect Ninja's header dependencies: ${depsError}")
    endif()

    if(NOT depsOutput MATCHES "#deps [1-9][0-9]*, [^\r\n]*\\(VALID\\)")
        dependency_failure("the production object has no valid header dependency record")
    endif()

    string(REPLACE "\r\n" "\n" depsOutput "${depsOutput}")
    string(REPLACE "\n" ";" depsLines "${depsOutput}")
    list(REMOVE_AT depsLines 0)
    set(recordedHeaders)
    foreach(dependency IN LISTS depsLines)
        string(STRIP "${dependency}" dependency)
        if(dependency STREQUAL "")
            continue()
        endif()
        file(TO_CMAKE_PATH "${dependency}" dependency)
        cmake_path(ABSOLUTE_PATH dependency BASE_DIRECTORY "${DVS_BINARY_DIR}" NORMALIZE)
        string(TOLOWER "${dependency}" dependency)
        list(APPEND recordedHeaders "${dependency}")
    endforeach()

    foreach(header IN LISTS ARGN)
        set(expectedHeader "${DVS_SOURCE_DIR}/${header}")
        file(TO_CMAKE_PATH "${expectedHeader}" expectedHeader)
        cmake_path(NORMAL_PATH expectedHeader)
        string(TOLOWER "${expectedHeader}" expectedHeader)
        if(NOT expectedHeader IN_LIST recordedHeaders)
            dependency_failure("the production object's dependencies do not include ${header}")
        endif()
    endforeach()
endfunction()

# Verify actual records, including UI consumers: a healthy domain object does not rule out
# partially rebuilt UI objects with stale class layouts. Optional targets may not be built yet.
check_object_dependencies(
    "src/domain/CMakeFiles/dvs_domain.dir/src/ComparisonSelection.cpp.obj" TRUE
    "src/domain/include/dvs/domain/ComparisonSelection.h"
    "src/domain/include/dvs/domain/Identifiers.h"
)
check_object_dependencies(
    "src/application/CMakeFiles/dvs_application.dir/src/PlaybackCoordinator.cpp.obj" FALSE
    "src/application/include/dvs/application/SessionSnapshot.h"
)
check_object_dependencies(
    "src/ui_qml/CMakeFiles/dvs_ui_models.dir/src/ReviewController.cpp.obj" FALSE
    "src/ui_qml/include/dvs/ui/ReviewController.h"
    "src/application/include/dvs/application/SessionSnapshot.h"
)
check_object_dependencies(
    "src/ui_qml/CMakeFiles/dvs_ui_d3d11_bridge.dir/src/ImageReviewController.cpp.obj" FALSE
    "src/ui_qml/include/dvs/ui/ImageReviewController.h"
)
check_object_dependencies(
    "tests/component/ui/CMakeFiles/dvs_main_qml_contract_tests.dir/MainQmlContractTests.cpp.obj" FALSE
    "src/ui_qml/include/dvs/ui/ReviewController.h"
    "src/ui_qml/include/dvs/ui/ImageReviewController.h"
)

message(STATUS "MSVC header dependencies verified against Ninja's production and UI records.")
