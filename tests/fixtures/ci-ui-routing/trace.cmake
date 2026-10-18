cmake_minimum_required(VERSION 3.24)

# Evaluate the production source conditions without configuring a compiler or creating a window.
foreach(required SOURCE_ROOT TEST_CMAKE TRACE_OUTPUT ADAPTERS)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "Missing ${required}")
    endif()
endforeach()
set(PROJECT_SOURCE_DIR "${SOURCE_ROOT}")
set(DVS_BUILD_ADAPTERS "${ADAPTERS}")
function(add_executable target)
    if(target STREQUAL "dvs_ui_component_tests")
        set_property(GLOBAL PROPERTY DVS_ROUTING_SOURCES "${ARGN}")
    endif()
endfunction()
function(target_sources target visibility)
    if(target STREQUAL "dvs_ui_component_tests")
        set_property(GLOBAL APPEND PROPERTY DVS_ROUTING_SOURCES "${ARGN}")
    endif()
endfunction()
# Commands the production file uses that either cannot run in script mode (add_dependencies
# is not scriptable) or do not affect the source set. Stubbing them keeps the probe focused on
# source routing instead of failing on an unrelated target-level edge.
foreach(command target_link_libraries target_include_directories target_compile_definitions
                dvs_apply_project_warnings set_target_properties dvs_add_gtest add_test
                set_tests_properties add_dependencies)
    function(${command})
    endfunction()
endforeach()
include("${TEST_CMAKE}")
get_property(sources GLOBAL PROPERTY DVS_ROUTING_SOURCES)
if(NOT sources)
    message(FATAL_ERROR "No UI test sources were evaluated")
endif()
string(REPLACE ";" "\n" sources "${sources}")
file(WRITE "${TRACE_OUTPUT}" "${sources}\n")
