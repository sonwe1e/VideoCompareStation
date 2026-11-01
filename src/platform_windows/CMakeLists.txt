add_library(
    dvs_windows_support
    STATIC
        src/support/Module.cpp
        src/support/AtomicFilePublisher.cpp
        src/support/ProcessTelemetry.cpp
        src/support/SourceIdentityService.cpp
        src/support/SteadyDeadlineScheduler.cpp
        src/support/TraceSink.cpp
        src/support/WindowsPaths.cpp
)
add_library(dvs::windows_support ALIAS dvs_windows_support)

target_include_directories(
    dvs_windows_support
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(dvs_windows_support PUBLIC dvs_application)
target_link_libraries(dvs_windows_support PRIVATE ole32 psapi shell32 uuid)

dvs_apply_project_warnings(dvs_windows_support)
dvs_register_architecture_target(dvs_windows_support)
set_target_properties(dvs_windows_support PROPERTIES FOLDER "Adapters/Windows")

add_library(
    dvs_graphics_d3d11
    STATIC
        src/graphics/FrameBudget.cpp
        src/graphics/CpuNv12FrameResource.cpp
        src/graphics/CpuP010FrameResource.cpp
        src/graphics/D3d11DecodedFrameResource.cpp
        src/graphics/D3d11GpuFrameBacking.cpp
        src/graphics/D3d11SourcePixelProbe.cpp
        src/graphics/D3d11ComparisonRenderer.cpp
        src/graphics/D3d11RenderChannel.cpp
        src/graphics/FrameResourceFactory.cpp
        src/graphics/FrameMailbox.cpp
        src/graphics/GraphicsDeviceBroker.cpp
        src/graphics/GpuFrameSet.cpp
        src/graphics/GpuFrameResource.cpp
        src/graphics/GpuTransferActor.cpp
        src/graphics/PresentationAckMailbox.cpp
        src/graphics/PresentationStatisticsProbe.cpp
)
add_library(dvs::graphics_d3d11 ALIAS dvs_graphics_d3d11)

include(DvsHlsl)
dvs_compile_nv12_shaders(
    OUTPUT_HEADER_VARIABLE dvs_nv12_shader_header
    OUTPUT_INCLUDE_DIRECTORY_VARIABLE dvs_nv12_shader_include_directory
)
target_sources(dvs_graphics_d3d11 PRIVATE "${dvs_nv12_shader_header}")

target_include_directories(
    dvs_graphics_d3d11
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE "${dvs_nv12_shader_include_directory}"
)
target_link_libraries(
    dvs_graphics_d3d11
    PUBLIC
        dvs_application
        dvs_presentation_contract
    PRIVATE
        d3d11
        dxgi
        dwmapi
)

dvs_apply_project_warnings(dvs_graphics_d3d11)
dvs_register_architecture_target(dvs_graphics_d3d11)
set_target_properties(dvs_graphics_d3d11 PROPERTIES FOLDER "Adapters/Graphics")

# Compatibility aggregate retained for existing consumers and the stable startup module contract.
add_library(dvs_platform_windows INTERFACE)
add_library(dvs::platform_windows ALIAS dvs_platform_windows)
target_link_libraries(
    dvs_platform_windows
    INTERFACE
        dvs_windows_support
        dvs_graphics_d3d11
)
dvs_register_architecture_target(dvs_platform_windows)
set_target_properties(dvs_platform_windows PROPERTIES FOLDER "Adapters")
