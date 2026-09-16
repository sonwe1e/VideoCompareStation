include(GoogleTest)

add_executable(
    dvs_app_unit_tests
    PlaybackTraceEnvironmentTests.cpp
    StartupRequestBrokerTests.cpp
    StartupRequestParserTests.cpp
    "${PROJECT_SOURCE_DIR}/src/app/PlaybackTraceEnvironment.cpp"
    "${PROJECT_SOURCE_DIR}/src/app/StartupRequest.cpp"
    "${PROJECT_SOURCE_DIR}/src/app/StartupRequestBroker.cpp"
)
target_include_directories(dvs_app_unit_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/app")
target_link_libraries(dvs_app_unit_tests PRIVATE Qt6::Core Qt6::Network dvs::test_dependencies)

dvs_apply_project_warnings(dvs_app_unit_tests)
set_target_properties(dvs_app_unit_tests PROPERTIES FOLDER "Tests/Unit/App")

dvs_add_gtest(
    TARGET dvs_app_unit_tests
    TEST_PREFIX "app."
    LAYER unit
    MODULE app
    TIMEOUT 10
)

find_program(DVS_TEST_POWERSHELL_EXECUTABLE NAMES pwsh powershell REQUIRED)
dvs_add_test(
    NAME app.playback-trace-gate-schema
    LAYER unit
    MODULE app
    TIMEOUT 20
    COMMAND
        "${DVS_TEST_POWERSHELL_EXECUTABLE}"
        -NoLogo
        -NoProfile
        -File "${CMAKE_CURRENT_SOURCE_DIR}/PlaybackTraceGateTests.ps1"
        -ModulePath "${PROJECT_SOURCE_DIR}/tools/testing/PlaybackTraceGate.psm1"
        -TestRoot "${PROJECT_BINARY_DIR}/test-output"
)
