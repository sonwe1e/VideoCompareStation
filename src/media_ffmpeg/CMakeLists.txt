add_library(
    dvs_media_ffmpeg
    STATIC
        src/AlignmentAnalysisService.cpp
        src/Module.cpp
        src/MultiSourceFrameProvider.cpp
        src/PresentationIndexCache.cpp
        src/FrameSetAssembler.cpp
        src/FrameTimelineIndex.cpp
        src/MediaProbe.cpp
        src/PairMetricsDecodeSession.cpp
        src/PairMetricsService.cpp
        src/PreviewThumbnailService.cpp
        src/SourceDecodeActor.cpp
        src/SourceFrameCache.cpp
        src/SoftwareDecoder.cpp
        src/SignatureCache.cpp
        src/SignatureDecodeSession.cpp
        src/StillImageDecoder.cpp
)
add_library(dvs::media_ffmpeg ALIAS dvs_media_ffmpeg)

target_include_directories(
    dvs_media_ffmpeg
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(dvs_media_ffmpeg PUBLIC dvs_application)
target_link_libraries(
    dvs_media_ffmpeg
    PRIVATE
        dvs_windows_support
        dvs_graphics_d3d11
        dvs::ffmpeg_dependencies
)

dvs_apply_project_warnings(dvs_media_ffmpeg)
dvs_register_architecture_target(dvs_media_ffmpeg)
set_target_properties(dvs_media_ffmpeg PROPERTIES FOLDER "Adapters")
