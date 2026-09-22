include_guard(GLOBAL)

set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ".")
set(CMAKE_INSTALL_UCRT_LIBRARIES TRUE)
include(InstallRequiredSystemLibraries)

install(
    TARGETS CompareStation CompareStationCli CompareStationShell
    RUNTIME_DEPENDENCIES
        DIRECTORIES
            "$<TARGET_FILE_DIR:CompareStation>"
            "$<TARGET_FILE_DIR:CompareStationCli>"
        PRE_EXCLUDE_REGEXES
            "^api-ms-win-.*"
            "^ext-ms-.*"
        POST_EXCLUDE_REGEXES
            ".*[Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\][Ss][Yy][Ss][Tt][Ee][Mm]32[/\\\\].*"
    RUNTIME DESTINATION . COMPONENT Runtime
)

qt6_generate_deploy_script(
    TARGET CompareStation
    NAME dvs_qt_runtime
    OUTPUT_SCRIPT dvs_qt_deploy_script
    CONTENT "
set(QT_DEPLOY_BIN_DIR \".\")
set(QT_DEPLOY_PLUGINS_DIR \".\")
set(QT_DEPLOY_QML_DIR \"qml\")
qt6_deploy_runtime_dependencies(
    EXECUTABLE \"$<TARGET_FILE:CompareStation>\"
    GENERATE_QT_CONF
    NO_TRANSLATIONS
    DEPLOY_TOOL_OPTIONS
        --qmldir \"${PROJECT_SOURCE_DIR}/src/ui_qml/qml\"
)"
)
install(SCRIPT "${dvs_qt_deploy_script}" COMPONENT Runtime)

set(DVS_VCPKG_SHARE_DIR "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share")
foreach(
    packageName
    IN ITEMS
        double-conversion
        ffmpeg
        md4c
        pcre2
        qtbase
        qtdeclarative
        qtlanguageserver
        qtshadertools
        qtsvg
        zlib
)
    set(packageCopyright "${DVS_VCPKG_SHARE_DIR}/${packageName}/copyright")
    if(EXISTS "${packageCopyright}")
        install(
            FILES "${packageCopyright}"
            DESTINATION licenses/vcpkg
            RENAME "${packageName}.txt"
            COMPONENT Runtime
        )
    endif()
endforeach()

if(EXISTS "${PROJECT_SOURCE_DIR}/assets")
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/assets/" DESTINATION assets COMPONENT Runtime)
endif()

if(EXISTS "${PROJECT_SOURCE_DIR}/licenses")
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/licenses/" DESTINATION licenses COMPONENT Runtime)
endif()
