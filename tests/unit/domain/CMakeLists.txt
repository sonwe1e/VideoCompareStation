include(GoogleTest)

add_executable(
    dvs_domain_unit_tests
    ComparisonValidatorTests.cpp
    FrameTimelineTests.cpp
    FrameTimelineCanonicalContractTests.cpp
    VfrMediaDescriptorConsistencyTests.cpp
    MediaDescriptorAndErrorTests.cpp
    ModuleNameTests.cpp
    RationalRateTests.cpp
)
target_link_libraries(
    dvs_domain_unit_tests
    PRIVATE
        dvs_domain
        dvs::test_dependencies
)

dvs_apply_project_warnings(dvs_domain_unit_tests)
set_target_properties(dvs_domain_unit_tests PROPERTIES FOLDER "Tests/Unit/Domain")

dvs_add_gtest(
    TARGET dvs_domain_unit_tests
    TEST_PREFIX "domain."
    LAYER unit
    MODULE domain
    TIMEOUT 10
)
