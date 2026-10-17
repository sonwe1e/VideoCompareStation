include_guard(GLOBAL)

include(CTest)

set(
    DVS_TEST_LAYERS
    unit
    component
    integration
    ui
    e2e
    hardware
    performance
    packaged
    soak
)

set(
    DVS_TEST_MODULES
    domain
    application
    platform_windows
    media_ffmpeg
    persistence_json
    presentation_contract
    shell_windows
    ui_qml
    app
)

function(dvs_add_test)
    cmake_parse_arguments(PARSE_ARGV 0 argument "" "NAME;LAYER;MODULE;TIMEOUT" "COMMAND")

    if(argument_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "Unknown dvs_add_test arguments: ${argument_UNPARSED_ARGUMENTS}")
    endif()

    foreach(requiredArgument NAME LAYER MODULE COMMAND)
        if(NOT argument_${requiredArgument})
            message(FATAL_ERROR "dvs_add_test requires ${requiredArgument}.")
        endif()
    endforeach()

    if(NOT argument_LAYER IN_LIST DVS_TEST_LAYERS)
        message(FATAL_ERROR "Unknown test layer '${argument_LAYER}' for ${argument_NAME}.")
    endif()

    if(NOT argument_MODULE IN_LIST DVS_TEST_MODULES)
        message(FATAL_ERROR "Unknown test module '${argument_MODULE}' for ${argument_NAME}.")
    endif()

    add_test(NAME "${argument_NAME}" COMMAND ${argument_COMMAND})
    set_tests_properties(
        "${argument_NAME}"
        PROPERTIES LABELS "${argument_LAYER};${argument_MODULE}"
    )

    if(argument_TIMEOUT)
        set_tests_properties("${argument_NAME}" PROPERTIES TIMEOUT "${argument_TIMEOUT}")
    endif()

    set_property(GLOBAL APPEND PROPERTY DVS_REGISTERED_TESTS "${argument_NAME}")
endfunction()

function(dvs_add_gtest)
    cmake_parse_arguments(
        PARSE_ARGV 0
        argument
        "RUN_SERIAL"
        "TARGET;TEST_PREFIX;LAYER;MODULE;TIMEOUT;TEST_FILTER"
        "ENVIRONMENT;EXTRA_LABELS;EXTRA_PROPERTIES"
    )
    foreach(requiredArgument TARGET TEST_PREFIX LAYER MODULE)
        if(NOT argument_${requiredArgument})
            message(FATAL_ERROR "dvs_add_gtest requires ${requiredArgument}.")
        endif()
    endforeach()
    if(NOT argument_LAYER IN_LIST DVS_TEST_LAYERS)
        message(FATAL_ERROR "Unknown test layer '${argument_LAYER}'.")
    endif()
    if(NOT argument_MODULE IN_LIST DVS_TEST_MODULES)
        message(FATAL_ERROR "Unknown test module '${argument_MODULE}'.")
    endif()

    set(labels "${argument_LAYER}" "${argument_MODULE}" ${argument_EXTRA_LABELS})
    string(JOIN ";" labelsValue ${labels})
    string(REPLACE ";" "\;" labelsValue "${labelsValue}")
    set(properties LABELS "${labelsValue}")
    if(argument_TIMEOUT)
        list(APPEND properties TIMEOUT "${argument_TIMEOUT}")
    endif()
    if(argument_RUN_SERIAL)
        list(APPEND properties RUN_SERIAL TRUE)
    endif()
    if(argument_ENVIRONMENT)
        string(JOIN ";" environmentValue ${argument_ENVIRONMENT})
        string(REPLACE ";" "\;" environmentValue "${environmentValue}")
        list(APPEND properties ENVIRONMENT "${environmentValue}")
    endif()
    list(APPEND properties ${argument_EXTRA_PROPERTIES})
    set(discoveryArguments)
    if(argument_TEST_FILTER)
        list(APPEND discoveryArguments TEST_FILTER "${argument_TEST_FILTER}")
    endif()

    gtest_discover_tests(
        "${argument_TARGET}"
        TEST_PREFIX "${argument_TEST_PREFIX}"
        DISCOVERY_MODE PRE_TEST
        ${discoveryArguments}
        PROPERTIES ${properties}
    )
endfunction()
