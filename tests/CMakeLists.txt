add_library(dvs_test_support INTERFACE)
add_library(dvs::test_support ALIAS dvs_test_support)
target_include_directories(
    dvs_test_support
    INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/support/include"
)

add_subdirectory(unit)
if(DVS_BUILD_ADAPTERS OR DVS_BUILD_UI)
    add_subdirectory(component)
endif()
if(DVS_BUILD_DESKTOP)
    add_subdirectory(hardware)
    add_subdirectory(smoke)
endif()
