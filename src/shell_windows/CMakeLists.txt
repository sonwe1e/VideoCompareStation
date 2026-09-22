configure_file(
    CompareStationShell.rc.in
    "${CMAKE_CURRENT_BINARY_DIR}/CompareStationShell.rc"
    @ONLY
)

add_library(
    CompareStationShell
    SHARED
    ClassFactory.cpp
    ExplorerCommand.cpp
    ExplorerCommand.h
    ExplorerCommandSupport.cpp
    ExplorerCommandSupport.h
    CompareStationShell.def
    "${CMAKE_CURRENT_BINARY_DIR}/CompareStationShell.rc"
)
target_link_libraries(CompareStationShell PRIVATE ole32 shell32 shlwapi)

dvs_apply_project_warnings(CompareStationShell)
dvs_register_architecture_target(CompareStationShell)
set_target_properties(
    CompareStationShell
    PROPERTIES
        FOLDER "Application"
        PREFIX ""
        OUTPUT_NAME "${DVS_SHELL_BINARY_BASENAME}"
)
