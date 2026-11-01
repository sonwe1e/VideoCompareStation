cmake_minimum_required(VERSION 4.4)

foreach(requiredVariable DVS_TEST_ROOT DVS_NINJA_EXECUTABLE)
    if(NOT DEFINED ${requiredVariable} OR "${${requiredVariable}}" STREQUAL "")
        message(FATAL_ERROR "${requiredVariable} is required.")
    endif()
endforeach()

set(domainObject "src/domain/CMakeFiles/dvs_domain.dir/src/ComparisonSelection.cpp.obj")
set(uiObject "tests/component/ui/CMakeFiles/dvs_main_qml_contract_tests.dir/MainQmlContractTests.cpp.obj")
set(modelObject "src/ui_qml/CMakeFiles/dvs_ui_models.dir/src/playback/ReviewController.cpp.obj")
set(bridgeObject "src/ui_qml/CMakeFiles/dvs_ui_d3d11_bridge.dir/src/image/ImageReviewController.cpp.obj")
set(domainHeaders
    "src/domain/include/dvs/domain/ComparisonSelection.h"
    "src/domain/include/dvs/domain/Identifiers.h")
set(uiHeaders
    "src/ui_qml/include/dvs/ui/ReviewController.h"
    "src/ui_qml/include/dvs/ui/ImageReviewController.h")
set(modelHeaders
    "src/ui_qml/include/dvs/ui/ReviewController.h"
    "src/application/include/dvs/application/SessionSnapshot.h")
set(bridgeHeaders "src/ui_qml/include/dvs/ui/ImageReviewController.h")
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef runId)
set(fixtureRoot "${DVS_TEST_ROOT}/${runId}/repository with spaces")

# Populate real Ninja dependency records without invoking a compiler. The checker reads
# Ninja's database, whose format is independent of the depfile/MSVC ingestion mechanism.
function(add_fixture_object object)
    cmake_path(GET object PARENT_PATH objectDirectory)
    file(MAKE_DIRECTORY "${binaryDir}/${objectDirectory}")
    set(depfile "${object}:")
    foreach(header IN LISTS ARGN)
        file(WRITE "${sourceDir}/${header}" "// fixture header\n")
        string(REPLACE " " "\\ " escapedHeader "${sourceDir}/${header}")
        string(APPEND depfile " ${escapedHeader}")
    endforeach()
    file(WRITE "${binaryDir}/${object}.input" "${depfile}\n")
    file(APPEND "${binaryDir}/build.ninja" "build ${object}: record\n")
endfunction()

set(checkCount 0)
foreach(case core-only healthy-ui empty-ui missing-header unrecorded-ui
             healthy-production-ui missing-model-header empty-bridge)
    math(EXPR checkCount "${checkCount} + 1")
    set(expectedFailureObject "MainQmlContractTests.cpp.obj")
    set(sourceDir "${fixtureRoot}/${case}/source")
    set(binaryDir "${fixtureRoot}/${case}/build")
    file(MAKE_DIRECTORY "${binaryDir}")
    file(WRITE "${binaryDir}/seed" "fixture object\n")
    file(WRITE "${binaryDir}/record.cmake"
        "configure_file(seed \"\${OBJECT}\" COPYONLY)\n"
        "configure_file(\"\${OBJECT}.input\" \"\${OBJECT}.d\" COPYONLY)\n")
    file(WRITE "${binaryDir}/build.ninja"
        "rule record\n"
        "  command = \"${CMAKE_COMMAND}\" -DOBJECT=$out -P record.cmake\n"
        "  depfile = $out.d\n"
        "  deps = gcc\n")
    add_fixture_object("${domainObject}" ${domainHeaders})
    if(case STREQUAL "healthy-ui")
        add_fixture_object("${uiObject}" ${uiHeaders})
    elseif(case STREQUAL "empty-ui")
        add_fixture_object("${uiObject}")
    elseif(case STREQUAL "missing-header")
        list(GET uiHeaders 0 firstHeader)
        add_fixture_object("${uiObject}" "${firstHeader}")
    elseif(case STREQUAL "unrecorded-ui")
        file(WRITE "${binaryDir}/${uiObject}" "untracked object\n")
        file(APPEND "${binaryDir}/build.ninja" "build ${uiObject}: phony\n")
    elseif(case STREQUAL "healthy-production-ui")
        add_fixture_object("${modelObject}" ${modelHeaders})
        add_fixture_object("${bridgeObject}" ${bridgeHeaders})
    elseif(case STREQUAL "missing-model-header")
        list(GET modelHeaders 0 firstHeader)
        add_fixture_object("${modelObject}" "${firstHeader}")
        set(expectedFailureObject "ReviewController.cpp.obj")
    elseif(case STREQUAL "empty-bridge")
        add_fixture_object("${bridgeObject}")
        set(expectedFailureObject "ImageReviewController.cpp.obj")
    endif()
    execute_process(
        COMMAND "${DVS_NINJA_EXECUTABLE}" -C "${binaryDir}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 10)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${case}: could not create Ninja records: ${output}\n${errors}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DDVS_BINARY_DIR=${binaryDir}" "-DDVS_SOURCE_DIR=${sourceDir}"
            "-DDVS_NINJA_EXECUTABLE=${DVS_NINJA_EXECUTABLE}"
            -P "${CMAKE_CURRENT_LIST_DIR}/CheckMsvcDependencies.cmake"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 10)
    if(case STREQUAL "core-only" OR case STREQUAL "healthy-ui" OR
       case STREQUAL "healthy-production-ui")
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "${case}: valid dependencies rejected: ${output}\n${errors}")
        endif()
    else()
        if(result EQUAL 0 OR NOT errors MATCHES "${expectedFailureObject}" OR
           NOT errors MATCHES "-Fresh")
            message(FATAL_ERROR "${case}: corrupt UI dependencies not diagnosed: ${output}\n${errors}")
        endif()
    endif()
    message(STATUS "PASS: ${case}")
endforeach()
if(NOT checkCount EQUAL 8)
    message(FATAL_ERROR "Expected eight MSVC dependency contract cases; ran ${checkCount}.")
endif()
