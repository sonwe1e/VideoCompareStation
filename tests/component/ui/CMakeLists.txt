include(GoogleTest)

add_executable(
    dvs_ui_component_tests
    DiagnosticsProbeTests.cpp
    IssueLogControllerTests.cpp
    PairMetricsControllerTests.cpp
    ReviewControllerTests.cpp
    "${PROJECT_SOURCE_DIR}/src/app/RuntimeBridges.cpp"
    ReviewPreferencesControllerTests.cpp
    SourceIdentityTests.cpp
)
target_link_libraries(
    dvs_ui_component_tests
    PRIVATE
        dvs_ui_qml
        dvs_media_ffmpeg
        dvs_persistence_json
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
add_executable(
    dvs_image_review_controller_tests
    ImageReviewControllerTests.cpp
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

add_executable(dvs_main_qml_contract_tests MainQmlContractTests.cpp)
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
    TIMEOUT 20
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
add_test(
    NAME ui.ImageWorkspaceManual
    COMMAND
        "$<TARGET_FILE:Qt6::qmltestrunner>"
        -input "${CMAKE_CURRENT_SOURCE_DIR}/qml/tst_image_workspace_manual.qml"
)
set_tests_properties(
    ui.ImageWorkspaceManual
    PROPERTIES
        LABELS "component;ui_qml"
        TIMEOUT 20
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

foreach(dvs_review_shell_test IN ITEMS empty_review_view player_osc timeline_tracks timeline_thumbnail_cache)
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
            )
            set_tests_properties(
                "ui.${dvs_menu_qml_test}.${dvs_qml_style}.scale-${dvs_scale_name}"
                PROPERTIES
                    LABELS "component;ui_qml"
                    TIMEOUT 20
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
endif()
