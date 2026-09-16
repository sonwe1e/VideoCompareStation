add_library(
    dvs_windows_support
    STATIC
        src/Module.cpp
        src/AtomicFilePublisher.cpp
        src/ProcessTelemetry.cpp
        src/SourceIdentityService.cpp
        src/SteadyDeadlineScheduler.cpp
        src/TraceSink.cpp
        src/WindowsPaths.cpp
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
        src/FrameBudget.cpp
        src/CpuNv12FrameResource.cpp
        src/CpuP010FrameResource.cpp
        src/D3d11DecodedFrameResource.cpp
        src/D3d11GpuFrameBacking.cpp
        src/D3d11ComparisonRenderer.cpp
        src/D3d11RenderChannel.cpp
        src/FrameResourceFactory.cpp
        src/FrameMailbox.cpp
        src/GraphicsDeviceBroker.cpp
        src/GpuFrameSet.cpp
        src/GpuFrameResource.cpp
        src/GpuTransferActor.cpp
        src/PresentationAckMailbox.cpp
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
