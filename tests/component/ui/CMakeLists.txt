include(GoogleTest)

# Model-only sources: everything here builds in the ui-dev preset, which links Qt Core
# only (the ui-models vcpkg feature) and never links the Qt Quick bridge or the adapters.
add_executable(
    dvs_ui_component_tests
    ClipExportControllerTests.cpp
    EvidenceIdentityJsonTests.cpp
    JogStateMachineTests.cpp
    PairMetricsControllerTests.cpp
    PreviewThumbnailControllerTests.cpp
    ReviewPreferencesControllerTests.cpp
    SampleExportControllerTests.cpp
    SourceIdentityTests.cpp
    VideoFolderModelTests.cpp
)
target_link_libraries(
    dvs_ui_component_tests
    PRIVATE
        dvs_ui_qml
        dvs::test_dependencies
)
target_include_directories(
    dvs_ui_component_tests
    PRIVATE "${PROJECT_SOURCE_DIR}/src/ui_qml/src" "${PROJECT_SOURCE_DIR}/src/app"
)

dvs_apply_project_warnings(dvs_ui_component_tests)
set_target_properties(dvs_ui_component_tests PROPERTIES FOLDER "Tests/Component/UI")

dvs_add_gtest(
    TARGET dvs_ui_component_tests
    TEST_PREFIX "ui."
    LAYER component
    MODULE ui_qml
    TIMEOUT 10
)

if(DVS_BUILD_ADAPTERS)
# Adapter/GUI-dependent test sources, deliberately kept inside this guard: the ui-dev
# model-only preset links Qt Core only (no Qt Quick bridge, no media/persistence
# adapters), so anything including QKeyEvent, the bridge headers with bridge symbols, the
# Win32 splash or the media adapters can only compile and link here.
target_sources(dvs_ui_component_tests PRIVATE
    DiagnosticsProbeTests.cpp
    HoldKeyHandlerTests.cpp
    InterpolationProtocolControllerTests.cpp
    IssueLogControllerTests.cpp
    JogKeyHandlerTests.cpp
    PairMetricsThresholdReuseTests.cpp
    # ReviewControllerTests exercises the GUI-thread projection bridge from src/app,
    # whose header includes the platform adapter's sink interface.
    ReviewControllerTests.cpp
    "${PROJECT_SOURCE_DIR}/src/app/RuntimeBridges.cpp"
    StartupSplashTests.cpp
    TimingDiagnosticsControllerTests.cpp
)
target_link_libraries(
    dvs_ui_component_tests PRIVATE dvs_media_ffmpeg dvs_persistence_json
)
target_include_directories(
    dvs_ui_component_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/media_ffmpeg/src"
)
# TimingDiagnosticsControllerTests drives the presentation probe's deterministic failure seam
# (PresentationStatisticsProbeTestHooks.h) that lives next to the platform adapter's sources.
target_include_directories(
    dvs_ui_component_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/platform_windows/src"
)
target_compile_definitions(
    dvs_ui_component_tests PRIVATE DVS_MEDIA_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures/media"
)

# The Dvs.Ui registration contract: typed properties resolve for the composition-root
# controllers, missing required dependencies fail creation, the uncreatable/creatable
# split holds, and the generated tooling description matches the runtime registry. Lives
# behind the adapters guard because the registration entry links the Qt Quick bridge that
# defines the controllers.
add_executable(
    dvs_qml_type_registration_tests
    QmlTypeRegistrationTests.cpp
)
target_link_libraries(
    dvs_qml_type_registration_tests
    PRIVATE
        dvs_ui_qml
        dvs::qt_dependencies
        dvs::test_dependencies
)
# The consistency test reads the artifact qmllint consumes and compares it with the
# runtime registry, so the build must generate it first.
target_compile_definitions(
    dvs_qml_type_registration_tests
    PRIVATE DVS_QMLTYPES_PATH="${DVS_QML_TYPES_IMPORT_DIR}/Dvs/Ui/dvs.qmltypes"
)
add_dependencies(dvs_qml_type_registration_tests dvs_ui_qml_types_generate)

dvs_apply_project_warnings(dvs_qml_type_registration_tests)
set_target_properties(
    dvs_qml_type_registration_tests
    PROPERTIES FOLDER "Tests/Component/UI"
)

dvs_add_gtest(
    TARGET dvs_qml_type_registration_tests
    TEST_PREFIX "ui."
    LAYER component
    MODULE ui_qml
    TIMEOUT 10
)

add_executable(
    dvs_image_review_controller_tests
    ImageReviewControllerTests.cpp
    ImageColorContractTests.cpp
)
target_link_libraries(
    dvs_image_review_controller_tests
    PRIVATE
        dvs_ui_qml
        dvs::qt_dependencies
        dvs::test_dependencies
)

dvs_apply_project_warnings(dvs_image_review_controller_tests)
set_target_properties(
    dvs_image_review_controller_tests
    PROPERTIES FOLDER "Tests/Component/UI"
)

dvs_add_gtest(
    TARGET dvs_image_review_controller_tests
    TEST_PREFIX "ui."
    LAYER component
    MODULE ui_qml
    TIMEOUT 10
)

add_executable(
    dvs_image_color_sample_tests
    ImageColorSampleTests.cpp
    ImageColorConversionTests.cpp
)
target_link_libraries(
    dvs_image_color_sample_tests
    PRIVATE
        dvs_ui_qml
        dvs_media_ffmpeg
        dvs::ffmpeg_dependencies
        dvs::json_dependencies
        dvs::qt_dependencies
        dvs::test_dependencies
)
target_compile_definitions(
    dvs_image_color_sample_tests
    PRIVATE DVS_IMAGE_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures/images"
)

dvs_apply_project_warnings(dvs_image_color_sample_tests)
set_target_properties(
    dvs_image_color_sample_tests
    PROPERTIES FOLDER "Tests/Component/UI"
)

dvs_add_gtest(
    TARGET dvs_image_color_sample_tests
    TEST_PREFIX "ui."
    LAYER component
    MODULE ui_qml
    TIMEOUT 30
)

add_executable(
    dvs_color_clipboard_controller_tests
    ColorClipboardControllerTests.cpp
)
target_link_libraries(
    dvs_color_clipboard_controller_tests
    PRIVATE
        dvs_ui_qml
        dvs::qt_dependencies
        dvs::test_dependencies
)

dvs_apply_project_warnings(dvs_color_clipboard_controller_tests)
set_target_properties(
    dvs_color_clipboard_controller_tests
    PROPERTIES FOLDER "Tests/Component/UI"
)

dvs_add_gtest(
    TARGET dvs_color_clipboard_controller_tests
    TEST_PREFIX "ui."
    LAYER component
    MODULE ui_qml
    TIMEOUT 10
)

add_executable(
    dvs_image_edit_controller_tests
    ImageEditControllerTests.cpp
    ImageEditOperationsTests.cpp
)
target_link_libraries(
    dvs_image_edit_controller_tests
    PRIVATE
        dvs_ui_qml
        dvs::qt_dependencies
        dvs::test_dependencies
)

dvs_apply_project_warnings(dvs_image_edit_controller_tests)
set_target_properties(
    dvs_image_edit_controller_tests
    PROPERTIES FOLDER "Tests/Component/UI"
)

dvs_add_gtest(
    TARGET dvs_image_edit_controller_tests
    TEST_PREFIX "ui."
    LAYER component
    MODULE ui_qml
    TIMEOUT 10
)

add_executable(
    dvs_image_folder_pair_model_tests
    ImageFolderPairModelTests.cpp
)
target_link_libraries(
    dvs_image_folder_pair_model_tests
    PRIVATE
        dvs_ui_qml
        dvs::qt_dependencies
        dvs::test_dependencies
)

dvs_apply_project_warnings(dvs_image_folder_pair_model_tests)
set_target_properties(
    dvs_image_folder_pair_model_tests
    PROPERTIES FOLDER "Tests/Component/UI"
)

dvs_add_gtest(
    TARGET dvs_image_folder_pair_model_tests
    TEST_PREFIX "ui."
    LAYER component
    MODULE ui_qml
    TIMEOUT 10
)

add_executable(
    dvs_image_folder_cursor_tests
    ImageFolderCursorTests.cpp
)
target_link_libraries(
    dvs_image_folder_cursor_tests
    PRIVATE
        dvs_ui_models
        dvs::qt_dependencies
        dvs::test_dependencies
)

dvs_apply_project_warnings(dvs_image_folder_cursor_tests)
set_target_properties(
    dvs_image_folder_cursor_tests
    PROPERTIES FOLDER "Tests/Component/UI"
)

dvs_add_gtest(
    TARGET dvs_image_folder_cursor_tests
    TEST_PREFIX "ui."
    LAYER component
    MODULE ui_qml
    TIMEOUT 10
)

add_executable(
    dvs_main_qml_contract_tests
    MainQmlContractTests.cpp
    ViewportPixelScaleTests.cpp
)
target_link_libraries(
    dvs_main_qml_contract_tests
    PRIVATE
        dvs_ui_qml
        dvs::qt_dependencies
        GTest::gtest
)

dvs_apply_project_warnings(dvs_main_qml_contract_tests)
set_target_properties(
    dvs_main_qml_contract_tests
    PROPERTIES FOLDER "Tests/Component/UI"
)

dvs_add_gtest(
    TARGET dvs_main_qml_contract_tests
    TEST_PREFIX "ui."
    LAYER component
    MODULE ui_qml
    # Real-window popup and mouse cases take 2-15 s locally but 21-27 s on the shared
    # self-hosted runner (see run 37713616081); 20 s cut them off there while every run
    # stayed green locally. 120 s keeps a >4x margin without masking a real hang.
    TIMEOUT 120
    RUN_SERIAL
    ENVIRONMENT QSG_RENDER_LOOP=basic QT_QUICK_CONTROLS_STYLE=Basic
)

# DPI scale factors covered by the per-test QT_SCALE_FACTOR loops below (100/125/150/200%). Kept
# in one variable so the four loops cannot drift apart when a factor is added or removed.
set(dvs_dpi_scale_factors 1 1.25 1.5 2)

set(dvs_toolbar_combo_qml_test "${CMAKE_CURRENT_SOURCE_DIR}/qml/tst_toolbar_combo.qml")
foreach(dvs_qml_style IN ITEMS Basic Windows)
    add_test(
        NAME "ui.ToolbarCombo.${dvs_qml_style}"
        COMMAND
            "$<TARGET_FILE:Qt6::qmltestrunner>"
            -input "${dvs_toolbar_combo_qml_test}"
    )
    set_tests_properties(
        "ui.ToolbarCombo.${dvs_qml_style}"
        PROPERTIES
            LABELS "component;ui_qml"
            TIMEOUT 15
            RUN_SERIAL TRUE
            ENVIRONMENT "QT_QUICK_CONTROLS_STYLE=${dvs_qml_style}"
    )
endforeach()

set(
    dvs_drop_confirmation_qml_test
    "${CMAKE_CURRENT_SOURCE_DIR}/qml/tst_drop_confirmation_dialog.qml"
)
foreach(dvs_scale_factor ${dvs_dpi_scale_factors})
    string(REPLACE "." "_" dvs_scale_name "${dvs_scale_factor}")
    add_test(
        NAME "ui.DropConfirmationDialog.scale-${dvs_scale_name}"
        COMMAND
            "$<TARGET_FILE:Qt6::qmltestrunner>"
            -input "${dvs_drop_confirmation_qml_test}"
    )
    set_tests_properties(
        "ui.DropConfirmationDialog.scale-${dvs_scale_name}"
        PROPERTIES
            LABELS "component;ui_qml"
            TIMEOUT 20
            RUN_SERIAL TRUE
            ENVIRONMENT
                "QSG_RHI_BACKEND=d3d11;QSG_RENDER_LOOP=basic;QT_QUICK_CONTROLS_STYLE=Basic;QT_SCALE_FACTOR=${dvs_scale_factor}"
    )
endforeach()

set(dvs_wipe_handle_qml_test "${CMAKE_CURRENT_SOURCE_DIR}/qml/tst_wipe_handle.qml")
# Repo-built Qt Quick Test runner for the image workspace: the stock qmltestrunner cannot
# register the Dvs.Ui types, and the typed workspace rejects QtObject stand-in controllers
# at component compile time. The runner mirrors the production composition root (types,
# real controller and clipboard, review image provider); see the file header.
add_executable(
    dvs_image_workspace_qml_tests
    ImageWorkspaceQmlTestMain.cpp
)
target_link_libraries(
    dvs_image_workspace_qml_tests
    PRIVATE
        dvs_ui_qml
        dvs::qt_dependencies
        Qt6::QuickTest
)
dvs_apply_project_warnings(dvs_image_workspace_qml_tests)
set_target_properties(
    dvs_image_workspace_qml_tests
    PROPERTIES
        FOLDER "Tests/Component/UI"
        # Q_OBJECT classes live in the runner's .cpp; AUTOMOC generates the .moc file the
        # source includes.
        AUTOMOC ON
)

add_test(
    NAME ui.ImageWorkspaceManual
    COMMAND
        "$<TARGET_FILE:dvs_image_workspace_qml_tests>"
        -input "${CMAKE_CURRENT_SOURCE_DIR}/qml/tst_image_workspace_manual.qml"
)
set_tests_properties(
    ui.ImageWorkspaceManual
    PROPERTIES
        LABELS "component;ui_qml"
        # The suite measures ~13 s serial but is RUN_SERIAL only per-suite: under the full
        # -TestJobs 8 gate the machine runs other suites between its cases, and 20 s cut it
        # off mid-run (standalone it passed). 60 s matches the menu suites' headroom.
        TIMEOUT 60
        RUN_SERIAL TRUE
        ENVIRONMENT "QSG_RHI_BACKEND=d3d11;QSG_RENDER_LOOP=basic;QT_QUICK_CONTROLS_STYLE=Basic"
)
foreach(dvs_scale_factor ${dvs_dpi_scale_factors})
    string(REPLACE "." "_" dvs_scale_name "${dvs_scale_factor}")
    add_test(
        NAME "ui.WipeHandle.scale-${dvs_scale_name}"
        COMMAND
            "$<TARGET_FILE:Qt6::qmltestrunner>"
            -input "${dvs_wipe_handle_qml_test}"
    )
    set_tests_properties(
        "ui.WipeHandle.scale-${dvs_scale_name}"
        PROPERTIES
            LABELS "component;ui_qml"
            TIMEOUT 20
            RUN_SERIAL TRUE
            ENVIRONMENT
                "QSG_RHI_BACKEND=d3d11;QSG_RENDER_LOOP=basic;QT_SCALE_FACTOR=${dvs_scale_factor}"
    )
endforeach()

add_test(
    NAME ui.ReviewActions
    COMMAND
        "$<TARGET_FILE:Qt6::qmltestrunner>"
        -input "${CMAKE_CURRENT_SOURCE_DIR}/qml/tst_review_actions.qml"
)
set_tests_properties(
    ui.ReviewActions
    PROPERTIES
        LABELS "component;ui_qml"
        TIMEOUT 15
        RUN_SERIAL TRUE
)

# V01 "panorama + three same-ROI locals": the strip is plain QtQuick (no runtime-registered
# types), so qmltestrunner can instantiate it directly with a stand-in surface item.
add_test(
    NAME ui.RoiTripletStrip
    COMMAND
        "$<TARGET_FILE:Qt6::qmltestrunner>"
        -input "${CMAKE_CURRENT_SOURCE_DIR}/qml/tst_roi_triplet_strip.qml"
)
set_tests_properties(
    ui.RoiTripletStrip
    PROPERTIES
        LABELS "component;ui_qml"
        TIMEOUT 20
        RUN_SERIAL TRUE
        ENVIRONMENT "QT_QUICK_CONTROLS_STYLE=Basic;QSG_RENDER_LOOP=basic"
)

foreach(dvs_review_shell_test IN ITEMS empty_review_view player_osc timeline_tracks timeline_thumbnail_cache review_action_button review_offset_spin_box vcs_theme layout_metrics stage_states clip_export_dialog)
    add_test(
        NAME "ui.${dvs_review_shell_test}"
        COMMAND
            "$<TARGET_FILE:Qt6::qmltestrunner>"
            -input
            "${CMAKE_CURRENT_SOURCE_DIR}/qml/tst_${dvs_review_shell_test}.qml"
    )
    set_tests_properties(
        "ui.${dvs_review_shell_test}"
        PROPERTIES
            LABELS "component;ui_qml"
            TIMEOUT 20
            RUN_SERIAL TRUE
            ENVIRONMENT "QT_QUICK_CONTROLS_STYLE=Basic;QSG_RENDER_LOOP=basic"
    )
endforeach()

foreach(dvs_menu_qml_test IN ITEMS vcs_menu application_menu_bar active_source_strip)
    foreach(dvs_qml_style IN ITEMS Basic Windows)
        foreach(dvs_scale_factor ${dvs_dpi_scale_factors})
            string(REPLACE "." "_" dvs_scale_name "${dvs_scale_factor}")
            add_test(
                NAME "ui.${dvs_menu_qml_test}.${dvs_qml_style}.scale-${dvs_scale_name}"
                COMMAND
                    "$<TARGET_FILE:Qt6::qmltestrunner>"
                    -input "${CMAKE_CURRENT_SOURCE_DIR}/qml/tst_${dvs_menu_qml_test}.qml"
                    -o "${CMAKE_BINARY_DIR}/Testing/qml-${dvs_menu_qml_test}-${dvs_qml_style}-${dvs_scale_name}.txt,txt"
                    -o "-,txt"
            )
            # The expanded source-strip drag suite needs ~21 s locally but measured 77.8 s
            # on the shared self-hosted runner (run 37713625606's main-push sibling), so it
            # gets a wider bound than the two small menu suites. 60 s stays enough for those.
            if(dvs_menu_qml_test STREQUAL "active_source_strip")
                set(dvs_menu_qml_timeout 180)
            else()
                set(dvs_menu_qml_timeout 60)
            endif()
            set_tests_properties(
                "ui.${dvs_menu_qml_test}.${dvs_qml_style}.scale-${dvs_scale_name}"
                PROPERTIES
                    LABELS "component;ui_qml"
                    TIMEOUT ${dvs_menu_qml_timeout}
                    RUN_SERIAL TRUE
                    ENVIRONMENT
                        "QT_QUICK_CONTROLS_STYLE=${dvs_qml_style};QSG_RENDER_LOOP=basic;QT_SCALE_FACTOR=${dvs_scale_factor}"
            )
        endforeach()
    endforeach()
endforeach()

foreach(dvs_qml_style IN ITEMS Basic Windows)
    foreach(dvs_scale_factor ${dvs_dpi_scale_factors})
        string(REPLACE "." "_" dvs_scale_name "${dvs_scale_factor}")
        add_test(
            NAME "ui.vcs_tool_tip.${dvs_qml_style}.scale-${dvs_scale_name}"
            COMMAND
                "$<TARGET_FILE:Qt6::qmltestrunner>"
                -input "${CMAKE_CURRENT_SOURCE_DIR}/qml/tst_vcs_tool_tip.qml"
        )
        set_tests_properties(
            "ui.vcs_tool_tip.${dvs_qml_style}.scale-${dvs_scale_name}"
            PROPERTIES
                LABELS "component;ui_qml"
                TIMEOUT 20
                RUN_SERIAL TRUE
                ENVIRONMENT
                    "QT_QUICK_CONTROLS_STYLE=${dvs_qml_style};QSG_RENDER_LOOP=basic;QT_SCALE_FACTOR=${dvs_scale_factor}"
        )
    endforeach()
endforeach()

foreach(dvs_sidebar_style IN ITEMS Basic Windows)
    add_test(
        NAME "ui.video_file_sidebar.${dvs_sidebar_style}"
        COMMAND
            "$<TARGET_FILE:Qt6::qmltestrunner>"
            -input "${CMAKE_CURRENT_SOURCE_DIR}/qml/tst_video_file_sidebar.qml"
    )
    set_tests_properties(
        "ui.video_file_sidebar.${dvs_sidebar_style}"
        PROPERTIES
            LABELS "component;ui_qml"
            TIMEOUT 60
            ENVIRONMENT "QT_QUICK_CONTROLS_STYLE=${dvs_sidebar_style}"
    )
endforeach()
endif()
