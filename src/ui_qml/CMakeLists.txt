add_library(
    dvs_ui_models
    STATIC
        include/dvs/ui/ClipExportController.h
        include/dvs/ui/FileAssociationController.h
        include/dvs/ui/ImageFolderPairModel.h
        include/dvs/ui/InterpolationProtocolController.h
        include/dvs/ui/IssueLogController.h
        include/dvs/ui/JogStateMachine.h
        include/dvs/ui/PairMetricsController.h
        include/dvs/ui/PreviewThumbnailController.h
        include/dvs/ui/ReviewController.h
        include/dvs/ui/ReviewPreferencesController.h
        include/dvs/ui/ReviewSessionFacade.h
        include/dvs/ui/ReviewShellController.h
        include/dvs/ui/SampleExportController.h
        include/dvs/ui/SourceIdentity.h
        include/dvs/ui/SourceListModel.h
        include/dvs/ui/VideoFolderModel.h
        src/ClipExportController.cpp
        src/FileAssociationController.cpp
        src/ImageFolderPairModel.cpp
        src/InterpolationProtocolController.cpp
        src/IssueLogController.cpp
        src/JogStateMachine.cpp
        src/PairMetricsController.cpp
        src/PreviewThumbnailController.cpp
        src/ReviewController.cpp
        src/RecentMediaFiles.h
        src/ReviewPreferencesController.cpp
        src/ReviewSessionFacade.cpp
        src/ReviewShellController.cpp
        src/SampleExportController.cpp
        src/SourceIdentity.cpp
        src/SourceListModel.cpp
        src/VideoFolderModel.cpp
)
add_library(dvs::ui_models ALIAS dvs_ui_models)

target_include_directories(
    dvs_ui_models
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(
    dvs_ui_models
    PUBLIC
        dvs_application
        dvs_presentation_contract
        dvs::qt_core_dependencies
)
set_target_properties(dvs_ui_models PROPERTIES AUTOMOC ON)
dvs_apply_project_warnings(dvs_ui_models)
dvs_register_architecture_target(dvs_ui_models)
set_target_properties(dvs_ui_models PROPERTIES FOLDER "Adapters/UI")

if(DVS_BUILD_ADAPTERS)
    add_library(
        dvs_ui_d3d11_bridge
        STATIC
            include/dvs/ui/ComparisonExportController.h
            include/dvs/ui/ComparisonSurface.h
            include/dvs/ui/ColorClipboardController.h
            include/dvs/ui/DesktopApplication.h
            include/dvs/ui/DiagnosticsProbe.h
            include/dvs/ui/DirectCompare.h
            include/dvs/ui/EditImageProvider.h
            include/dvs/ui/GraphicsBackend.h
            include/dvs/ui/HoldKeyHandler.h
            include/dvs/ui/ImageColorContract.h
            include/dvs/ui/ImageEditController.h
            include/dvs/ui/ImageEditOperations.h
            include/dvs/ui/ImagePairLoader.h
            include/dvs/ui/ImageReviewController.h
            include/dvs/ui/JogKeyHandler.h
            include/dvs/ui/MainWindowDependencies.h
            include/dvs/ui/Module.h
            include/dvs/ui/QmlTypeRegistration.h
            include/dvs/ui/RenderAckRelay.h
            include/dvs/ui/ReviewImageProvider.h
            include/dvs/ui/Sam2Segmenter.h
            include/dvs/ui/StartupMilestone.h
            include/dvs/ui/StartupSplash.h
            include/dvs/ui/TimingDiagnosticsController.h
            src/ComparisonExportController.cpp
            src/ColorClipboardController.cpp
            src/DesktopApplication.cpp
            src/DiagnosticsProbe.cpp
            src/DirectCompare.cpp
            src/ComparisonSurface.cpp
            src/EditImageProvider.cpp
            src/GraphicsBackend.cpp
            src/HoldKeyHandler.cpp
            src/ImageColorContract.cpp
            src/ImageEditController.cpp
            src/ImageEditOperations.cpp
            src/ImagePairLoader.cpp
            src/ImageReviewController.cpp
            src/JogKeyHandler.cpp
            src/MainWindowDependencies.cpp
            src/Module.cpp
            src/QmlTypeRegistration.cpp
            src/RenderAckRelay.cpp
            src/ReviewImageProvider.cpp
            src/Sam2Segmenter.cpp
            src/StartupMilestone.cpp
            src/StartupSplash.cpp
            src/TimingDiagnosticsController.cpp
    )
    add_library(dvs::ui_d3d11_bridge ALIAS dvs_ui_d3d11_bridge)

    target_compile_definitions(dvs_ui_d3d11_bridge PRIVATE DVS_PROJECT_VERSION="${PROJECT_VERSION}")

    target_include_directories(
        dvs_ui_d3d11_bridge
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(
        dvs_ui_d3d11_bridge
        PUBLIC
            dvs_ui_models
            dvs_graphics_d3d11
            dvs_presentation_contract
            dvs::qt_dependencies
        PRIVATE
            dwmapi
    )

    qt6_add_resources(dvs_ui_qml_resource_sources dvs_ui_qml_resources.qrc)
    target_sources(dvs_ui_d3d11_bridge PRIVATE ${dvs_ui_qml_resource_sources})

    set_target_properties(dvs_ui_d3d11_bridge PROPERTIES AUTOMOC ON)
    dvs_apply_project_warnings(dvs_ui_d3d11_bridge)
    dvs_register_architecture_target(dvs_ui_d3d11_bridge)
    set_target_properties(dvs_ui_d3d11_bridge PROPERTIES FOLDER "Adapters/UI")

    # Build-time description of the Dvs.Ui module for QML tooling (qmllint/qmlls). The
    # foreign wrappers live here, the wrapped classes stay in dvs_ui_d3d11_bridge; the
    # generated .qmltypes is consumed through an import path, never linked at runtime.
    # See docs/adr/0006-qml-type-registration.md.
    add_library(
        dvs_ui_qml_types
        STATIC
            qmltypes/DvsQmlTypes.cpp
            qmltypes/DvsQmlTypes.h
    )
    target_include_directories(dvs_ui_qml_types PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/qmltypes")
    target_link_libraries(dvs_ui_qml_types PRIVATE dvs_ui_d3d11_bridge Qt6::Qml)
    set_target_properties(dvs_ui_qml_types PROPERTIES AUTOMOC ON)
    dvs_apply_project_warnings(dvs_ui_qml_types)
    dvs_register_architecture_target(dvs_ui_qml_types)
    set_target_properties(dvs_ui_qml_types PROPERTIES FOLDER "Adapters/UI")

    # qmltyperegistrar reads the wrappers from this target's aggregated moc JSON and the
    # wrapped members from the bridge's; the tooling module is generated, never compiled
    # into a registration function (see the ADR for why the generated C++ is unused).
    qt6_extract_metatypes(dvs_ui_d3d11_bridge)
    qt6_extract_metatypes(dvs_ui_qml_types)

    set(
        DVS_QML_TYPES_IMPORT_DIR
        "${CMAKE_BINARY_DIR}/qml-imports"
        CACHE INTERNAL "Import directory that exposes the generated Dvs.Ui .qmltypes to QML tooling"
    )
    set(dvs_qml_types_module_dir "${DVS_QML_TYPES_IMPORT_DIR}/Dvs/Ui")
    configure_file(qmltypes/Dvs.Ui.qmldir.in "${dvs_qml_types_module_dir}/qmldir" @ONLY)
    add_custom_command(
        OUTPUT "${dvs_qml_types_module_dir}/dvs.qmltypes"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${dvs_qml_types_module_dir}"
        COMMAND
            "$<TARGET_FILE:Qt6::qmltyperegistrar>"
            "--generate-qmltypes" "${dvs_qml_types_module_dir}/dvs.qmltypes.raw"
            "--import-name" "Dvs.Ui"
            "--major-version" "1"
            "--minor-version" "0"
            "--foreign-types"
            "$<TARGET_PROPERTY:dvs_ui_d3d11_bridge,INTERFACE_QT_META_TYPES_BUILD_FILE>"
            "$<TARGET_PROPERTY:dvs_ui_qml_types,INTERFACE_QT_META_TYPES_BUILD_FILE>"
        # moc spells Q_PROPERTY(qreal ...) with the C++ name; QML tooling knows the type as
        # "double". Normalize the generated description and fail loudly if a spelling
        # survives that qmllint could not resolve (see cmake/NormalizeQmlTypes.cmake).
        COMMAND
            "${CMAKE_COMMAND}"
            "-DINPUT=${dvs_qml_types_module_dir}/dvs.qmltypes.raw"
            "-DOUTPUT=${dvs_qml_types_module_dir}/dvs.qmltypes"
            -P
            "${PROJECT_SOURCE_DIR}/cmake/NormalizeQmlTypes.cmake"
        DEPENDS
            dvs_ui_d3d11_bridge
            dvs_ui_qml_types
            "${PROJECT_SOURCE_DIR}/cmake/NormalizeQmlTypes.cmake"
            "$<TARGET_PROPERTY:dvs_ui_d3d11_bridge,INTERFACE_QT_META_TYPES_BUILD_FILE>"
            "$<TARGET_PROPERTY:dvs_ui_qml_types,INTERFACE_QT_META_TYPES_BUILD_FILE>"
        COMMENT "Generating the Dvs.Ui .qmltypes for QML tooling"
        VERBATIM
    )
    add_custom_target(
        dvs_ui_qml_types_generate
        ALL
        DEPENDS "${dvs_qml_types_module_dir}/dvs.qmltypes"
    )
    set_target_properties(dvs_ui_qml_types_generate PROPERTIES FOLDER "Adapters/UI")
endif()

# Stable aggregate consumed by the application and existing tests.
add_library(dvs_ui_qml INTERFACE)
add_library(dvs::ui_qml ALIAS dvs_ui_qml)
target_link_libraries(dvs_ui_qml INTERFACE dvs_ui_models)
if(DVS_BUILD_ADAPTERS)
    target_link_libraries(dvs_ui_qml INTERFACE dvs_ui_d3d11_bridge)
endif()
dvs_register_architecture_target(dvs_ui_qml)
set_target_properties(dvs_ui_qml PROPERTIES FOLDER "Adapters")
