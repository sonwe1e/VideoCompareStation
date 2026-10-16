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
            include/dvs/ui/ImageEditController.h
            include/dvs/ui/ImageEditOperations.h
            include/dvs/ui/ImagePairLoader.h
            include/dvs/ui/ImageReviewController.h
            include/dvs/ui/JogKeyHandler.h
            include/dvs/ui/Module.h
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
            src/ImageEditController.cpp
            src/ImageEditOperations.cpp
            src/ImagePairLoader.cpp
            src/ImageReviewController.cpp
            src/JogKeyHandler.cpp
            src/Module.cpp
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
