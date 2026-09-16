configure_file(VCStation.rc.in "${CMAKE_CURRENT_BINARY_DIR}/VCStation.rc" @ONLY)

add_executable(
    VCStation
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
    StartupFailureReporter.cpp
    StartupFailureReporter.h
    "${CMAKE_CURRENT_BINARY_DIR}/VCStation.rc"
)
add_executable(VCStationCli CliMain.cpp)

set(
    dvs_app_dependencies
    dvs_platform_windows
    dvs_media_ffmpeg
    dvs_persistence_json
    dvs_ui_qml
)

foreach(target VCStation VCStationCli)
    target_link_libraries(${target} PRIVATE ${dvs_app_dependencies})
    dvs_apply_project_warnings(${target})
    dvs_register_architecture_target(${target})
    set_target_properties(${target} PROPERTIES FOLDER "Application")
endforeach()

target_link_libraries(VCStation PRIVATE Qt6::Network user32)

add_custom_command(
    TARGET VCStation
    POST_BUILD
    COMMAND
        "$<TARGET_FILE:Qt6::windeployqt>"
        --no-translations
        --qmldir "${PROJECT_SOURCE_DIR}/src/ui_qml/qml"
        "$<TARGET_FILE:VCStation>"
    VERBATIM
    COMMENT "Deploying Qt runtime for VCStation"
)
