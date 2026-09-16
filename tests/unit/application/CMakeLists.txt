include(GoogleTest)

add_executable(
    dvs_application_unit_tests
    AlignmentTests.cpp
    AlignmentWorkEstimatorTests.cpp
    ContractCompileTests.cpp
    ComparisonExactnessTests.cpp
    PlaybackCoordinatorTests.cpp
    PlaybackTraceTests.cpp
    PrefetchSchedulerTests.cpp
)
target_link_libraries(
    dvs_application_unit_tests
    PRIVATE
        dvs_application
        dvs::test_dependencies
)

dvs_apply_project_warnings(dvs_application_unit_tests)
set_target_properties(dvs_application_unit_tests PROPERTIES FOLDER "Tests/Unit/Application")

dvs_add_gtest(
    TARGET dvs_application_unit_tests
    TEST_PREFIX "application."
    LAYER unit
    MODULE application
    TIMEOUT 10
)
