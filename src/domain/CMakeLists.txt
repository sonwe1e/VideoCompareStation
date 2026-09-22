add_library(
    dvs_domain
    STATIC
        src/ComparisonSelection.cpp
        src/ComparisonValidator.cpp
        src/PixelDifference.cpp
        src/FrameTimeline.cpp
        src/MediaDescriptor.cpp
        src/MediaError.cpp
        src/Module.cpp
        src/RationalRate.cpp
)
add_library(dvs::domain ALIAS dvs_domain)

target_include_directories(
    dvs_domain
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

dvs_apply_project_warnings(dvs_domain)
dvs_register_architecture_target(dvs_domain)
set_target_properties(dvs_domain PROPERTIES FOLDER "Modules")
