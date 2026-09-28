configure_file(
    CompareStationShell.rc.in
    "${CMAKE_CURRENT_BINARY_DIR}/CompareStationShell.rc"
    @ONLY
)

# Explorer loads CompareStationShell as its command server, but the per-user keys also have to be
# registered by the application at startup - an unpacked ZIP has no installer to do it. That code
# therefore lives in a static library the application links, and it reads the extension lists from
# ExplorerCommandSupport instead of keeping a copy, so the entry can never be offered for a file the
# handler then refuses.
add_library(
    dvs_shell_registration
    STATIC
    ExplorerCommandRegistration.cpp
    ExplorerCommandSupport.cpp
    ExplorerCommandSupport.h
)
target_include_directories(
    dvs_shell_registration
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(dvs_shell_registration PUBLIC advapi32 ole32)

dvs_apply_project_warnings(dvs_shell_registration)
dvs_register_architecture_target(dvs_shell_registration)
set_target_properties(dvs_shell_registration PROPERTIES FOLDER "Application")

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
