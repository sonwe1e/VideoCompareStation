include(GoogleTest)

add_executable(
    dvs_shell_support_unit_tests
    ExplorerCommandRegistrationTests.cpp
    ExplorerCommandSupportTests.cpp
    "${PROJECT_SOURCE_DIR}/src/shell_windows/ExplorerCommandRegistration.cpp"
    "${PROJECT_SOURCE_DIR}/src/shell_windows/ExplorerCommandSupport.cpp"
)
target_include_directories(
    dvs_shell_support_unit_tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src/shell_windows"
        "${PROJECT_SOURCE_DIR}/src/shell_windows/include"
)
# advapi32 and ole32 are what the registration itself calls: RegCreateKeyExW and StringFromGUID2.
target_link_libraries(
    dvs_shell_support_unit_tests
    PRIVATE dvs::test_dependencies shell32 advapi32 ole32
)

dvs_apply_project_warnings(dvs_shell_support_unit_tests)
set_target_properties(
    dvs_shell_support_unit_tests
    PROPERTIES FOLDER "Tests/Unit/ShellWindows"
)

dvs_add_gtest(
    TARGET dvs_shell_support_unit_tests
    TEST_PREFIX "shell_windows."
    LAYER unit
    MODULE shell_windows
    TIMEOUT 10
)
