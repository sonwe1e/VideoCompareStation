include_guard(GLOBAL)

macro(dvs_resolve_dependencies)
    if(DVS_BUILD_UI)
        find_package(Qt6 6.11 REQUIRED COMPONENTS Core)

        add_library(dvs_qt_core_dependencies INTERFACE)
        add_library(dvs::qt_core_dependencies ALIAS dvs_qt_core_dependencies)
        target_link_libraries(dvs_qt_core_dependencies INTERFACE Qt6::Core)

        if(DVS_BUILD_ADAPTERS)
            find_package(Qt6 6.11 REQUIRED COMPONENTS Gui Network Qml Quick QuickTest ShaderTools)

            add_library(dvs_qt_dependencies INTERFACE)
            add_library(dvs::qt_dependencies ALIAS dvs_qt_dependencies)
            target_link_libraries(
                dvs_qt_dependencies
                INTERFACE
                    dvs_qt_core_dependencies
                    Qt6::Gui
                    Qt6::Qml
                    Qt6::Quick
                    Qt6::ShaderTools
            )
        endif()
    endif()

    if(DVS_BUILD_ADAPTERS)
        find_package(nlohmann_json CONFIG REQUIRED)
        find_package(FFMPEG REQUIRED)

        add_library(dvs_ffmpeg_dependencies INTERFACE)
        add_library(dvs::ffmpeg_dependencies ALIAS dvs_ffmpeg_dependencies)
        target_include_directories(
            dvs_ffmpeg_dependencies
            SYSTEM INTERFACE ${FFMPEG_INCLUDE_DIRS}
        )
        target_link_libraries(dvs_ffmpeg_dependencies INTERFACE ${FFMPEG_LIBRARIES})

        add_library(dvs_json_dependencies INTERFACE)
        add_library(dvs::json_dependencies ALIAS dvs_json_dependencies)
        target_link_libraries(dvs_json_dependencies INTERFACE nlohmann_json::nlohmann_json)
    endif()

    if(BUILD_TESTING)
        find_package(GTest CONFIG REQUIRED)
        add_library(dvs_test_dependencies INTERFACE)
        add_library(dvs::test_dependencies ALIAS dvs_test_dependencies)
        target_link_libraries(dvs_test_dependencies INTERFACE GTest::gtest GTest::gtest_main)
    endif()
endmacro()
