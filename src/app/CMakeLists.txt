configure_file(CompareStation.rc.in "${CMAKE_CURRENT_BINARY_DIR}/CompareStation.rc" @ONLY)

add_executable(
    CompareStation
    WIN32
    Main.cpp
    PlaybackTraceEnvironment.cpp
    PlaybackTraceEnvironment.h
    ReviewRuntime.cpp
    ReviewRuntime.h
    RuntimeBridges.cpp
    RuntimeBridges.h
    StartupRequest.cpp
    StartupRequest.h
    StartupRequestBroker.cpp
    StartupRequestBroker.h
    StartupRequestDispatch.cpp
    StartupRequestDispatch.h
    StartupFailureReporter.cpp
    StartupFailureReporter.h
    "${CMAKE_CURRENT_BINARY_DIR}/CompareStation.rc"
)
add_executable(CompareStationCli CliMain.cpp)

set(
    dvs_app_dependencies
    dvs_platform_windows
    dvs_media_ffmpeg
    dvs_persistence_json
    # The per-user Explorer registration runs at startup, so the application links it directly.
    dvs_shell_registration
    dvs_ui_qml
)

foreach(target CompareStation CompareStationCli)
    target_link_libraries(${target} PRIVATE ${dvs_app_dependencies})
    dvs_apply_project_warnings(${target})
    dvs_register_architecture_target(${target})
    set_target_properties(${target} PROPERTIES FOLDER "Application")
endforeach()

target_link_libraries(CompareStation PRIVATE Qt6::Network user32)

# The registration points Explorer at the shell library of this product version, so the version has
# to reach the code that builds that path. The shell DLL name already carries the same number.
target_compile_definitions(CompareStation PRIVATE DVS_PROJECT_VERSION="${PROJECT_VERSION}")

add_custom_command(
    TARGET CompareStation
    POST_BUILD
    COMMAND
        "$<TARGET_FILE:Qt6::windeployqt>"
        --no-translations
        --qmldir "${PROJECT_SOURCE_DIR}/src/ui_qml/qml"
        "$<TARGET_FILE:CompareStation>"
    VERBATIM
    COMMENT "Deploying Qt runtime for CompareStation"
)
