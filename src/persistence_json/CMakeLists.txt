add_library(
    dvs_persistence_json
    STATIC
        src/IssueRecordRepository.cpp
        src/Module.cpp
        src/SettingsRepository.cpp
)
add_library(dvs::persistence_json ALIAS dvs_persistence_json)

target_include_directories(
    dvs_persistence_json
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(
    dvs_persistence_json
    PUBLIC dvs_application
    PRIVATE dvs_windows_support dvs::json_dependencies
)

dvs_apply_project_warnings(dvs_persistence_json)
dvs_register_architecture_target(dvs_persistence_json)
set_target_properties(dvs_persistence_json PROPERTIES FOLDER "Adapters")
